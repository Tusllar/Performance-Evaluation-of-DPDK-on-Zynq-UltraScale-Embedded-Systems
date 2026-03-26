#include <rte_byteorder.h>
#include <rte_io.h>
#include <rte_mbuf.h>
#include <rte_branch_prediction.h>
#include <stddef.h>

#include "gem_ethdev.h"
#include "gem_rxtx.h"

/* Cache maintenance for non-coherent DMA (common on embedded SoCs). */
static inline void
gem_dma_clean(const void *addr, size_t len)
{
#if defined(__aarch64__)
	uintptr_t p = (uintptr_t)addr;
	uintptr_t end = p + len;
	const uintptr_t line = 64; /* typical; over-cleaning is OK */

	p &= ~(line - 1);
	for (; p < end; p += line)
		__asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");

	__asm__ volatile("dsb ish" ::: "memory");
#else
	RTE_SET_USED(addr);
	RTE_SET_USED(len);
#endif
}

/* Keep minimal MMIO helpers local to datapath for TX kick/status readback. */
#define GEM_NCR_OFFSET       0x0000u
#define GEM_NCR_TXSTART      (1u << 9)
#define GEM_NCR_MAN_PORT_EN  (1u << 4)
#define GEM_NCR_RXEN         (1u << 2)
#define GEM_NCR_TXEN         (1u << 3)
#define GEM_TX_STATUS_OFFSET 0x0014u

static inline void
gem_mmio_write32(struct gem_priv *priv, uint32_t off, uint32_t val)
{
	volatile uint32_t *reg =
		(volatile uint32_t *)((char *)priv->base_addr + off);
	*reg = rte_cpu_to_le_32(val);
	rte_wmb();
}

static inline uint32_t
gem_mmio_read32(struct gem_priv *priv, uint32_t off)
{
	volatile uint32_t *reg =
		(volatile uint32_t *)((char *)priv->base_addr + off);
	uint32_t v = rte_le_to_cpu_32(*reg);
	rte_rmb();
	return v;
}

static inline void
gem_tx_kick(struct gem_priv *priv)
{
	uint32_t ncr;

	/*
	 * Keep enable bits while asserting TXSTART pulse.
	 * Writing only TXSTART can inadvertently clear RX/TX/MDIO enables.
	 */
	ncr = gem_mmio_read32(priv, GEM_NCR_OFFSET);
	ncr |= (GEM_NCR_MAN_PORT_EN | GEM_NCR_RXEN | GEM_NCR_TXEN |
		GEM_NCR_TXSTART);
	gem_mmio_write32(priv, GEM_NCR_OFFSET, ncr);
	GEM_LOG("TX kick: NCR=0x%08x TX_STATUS=0x%08x",
		gem_mmio_read32(priv, GEM_NCR_OFFSET),
		gem_mmio_read32(priv, GEM_TX_STATUS_OFFSET));
	rte_wmb();
}

static inline void
gem_tx_cleanup(struct gem_queue *q)
{
	struct gem_priv *priv;

	if (q == NULL)
		return;

	priv = q->priv;

	while (q->tx_pending != 0) {
		struct gem_tx_bd *bd = &q->tx_ring[q->tx_tail];
		struct rte_mbuf *m;

		if ((bd->ctrl & GEM_TX_USED) == 0)
			break;

		m = q->tx_sw_ring[q->tx_tail];
		if (m != NULL) {
			if (priv != NULL) {
				priv->tx_pkts++;
				priv->tx_bytes += m->pkt_len;
			}
			rte_pktmbuf_free(m);
			q->tx_sw_ring[q->tx_tail] = NULL;
		}

		q->tx_tail = (q->tx_tail + 1) % GEM_DESC_NUM;
		q->tx_pending--;
	}
}

uint16_t
gem_tx_pkt_burst(void *queue, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	struct gem_queue *q = queue;
	uint16_t nb_tx = 0;
	struct gem_priv *priv;
	static uint32_t log_throttle;

	if (q == NULL || q->priv == NULL || q->priv->base_addr == NULL) {
		uint16_t i;

		if (q == NULL || q->priv == NULL) {
			for (i = 0; i < nb_bufs; i++)
				rte_pktmbuf_free(bufs[i]);
			return nb_bufs;
		}

		priv = q->priv;
		for (i = 0; i < nb_bufs; i++) {
			struct rte_mbuf *m = bufs[i];
			priv->tx_bytes += m->pkt_len;
			rte_pktmbuf_free(m);
		}
		priv->tx_pkts += nb_bufs;
		return nb_bufs;
	}

	priv = q->priv;

	if (q->tx_ring == NULL || q->tx_sw_ring == NULL)
		return 0;

	gem_tx_cleanup(q);

	while (nb_tx < nb_bufs) {
		struct gem_tx_bd *bd = &q->tx_ring[q->tx_head];
		struct rte_mbuf *m = bufs[nb_tx];
		uint32_t ctrl;
		uint16_t desc_idx = q->tx_head;
		uint64_t iova;

		if ((bd->ctrl & GEM_TX_USED) == 0)
			break;
		if (q->tx_pending >= GEM_DESC_NUM)
			break;

		if (unlikely(m->nb_segs != 1)) {
			rte_pktmbuf_free(m);
			priv->tx_errs++;
			if ((++log_throttle & 0xFFFFu) == 0)
				GEM_LOG("TX drop: multi-seg packet not supported (nb_segs=%u)", m->nb_segs);
			nb_tx++;
			continue;
		}

		if (unlikely(m->pkt_len == 0 || m->pkt_len > GEM_TX_LEN_MASK)) {
			rte_pktmbuf_free(m);
			priv->tx_errs++;
			if ((++log_throttle & 0xFFFFu) == 0)
				GEM_LOG("TX drop: bad len=%u (max=%u)", m->pkt_len, (unsigned)GEM_TX_LEN_MASK);
			nb_tx++;
			continue;
		}

		q->tx_sw_ring[q->tx_head] = m;
		{
			iova = rte_pktmbuf_iova(m);
			if (unlikely(iova > 0xFFFFFFFFULL || (iova & 0x3ULL))) {
				rte_pktmbuf_free(m);
				priv->tx_errs++;
				if ((++log_throttle & 0xFFFFu) == 0)
					GEM_LOG("TX drop: bad IOVA=0x%llx (align=%llu)",
						(unsigned long long)iova,
						(unsigned long long)(iova & 0x3ULL));
				nb_tx++;
				continue;
			}
			bd->addr = (uint32_t)iova;
		}

		ctrl = (uint32_t)(m->pkt_len & GEM_TX_LEN_MASK) | GEM_TX_LAST;
		if (desc_idx == (GEM_DESC_NUM - 1))
			ctrl |= GEM_TX_WRAP;

		/* Non-coherent DMA: clean packet payload before handoff. */
		gem_dma_clean(rte_pktmbuf_mtod(m, const void *), m->pkt_len);

		rte_wmb();
		bd->ctrl = ctrl;

		/*
		 * Non-coherent DMA: clean the whole descriptor once it's fully populated
		 * (addr + ctrl).
		 */
		gem_dma_clean(bd, sizeof(*bd));

		/* Debug: confirm descriptor/payload locations. */
		GEM_LOG("TX[%u]: bd@%p addr_iova=0x%08x payload_va=%p payload_iova=0x%llx len=%u ctrl=0x%08x",
			desc_idx, (void *)bd, bd->addr,
			rte_pktmbuf_mtod(m, void *),
			(unsigned long long)iova, m->pkt_len, ctrl);

		GEM_LOG("TX[%u]: ctrl=0x%08x addr=0x%08x len=%u",
			desc_idx, ctrl, bd->addr, m->pkt_len);

		q->tx_head = (q->tx_head + 1) % GEM_DESC_NUM;
		q->tx_pending++;
		nb_tx++;
	}

	if (nb_tx > 0 && q->priv != NULL && q->priv->base_addr != NULL)
		gem_tx_kick(q->priv);

	return nb_tx;
}
