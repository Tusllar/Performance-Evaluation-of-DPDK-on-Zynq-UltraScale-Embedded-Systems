#include <string.h>

#include <rte_byteorder.h>
#include <rte_io.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_prefetch.h>
#include <rte_random.h>
#include <stddef.h>

#include "gem_ethdev.h"
#include "gem_rxtx.h"

/* Cache maintenance for non-coherent DMA (common on embedded SoCs). */
static inline void
gem_dma_invalidate(const void *addr, size_t len)
{
#if defined(__aarch64__)
	uintptr_t p = (uintptr_t)addr;
	uintptr_t end = p + len;
	const uintptr_t line = 64; /* typical; over-invalidation is OK */

	p &= ~(line - 1);
	for (; p < end; p += line)
		__asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");

	__asm__ volatile("dsb ish" ::: "memory");
#else
	RTE_SET_USED(addr);
	RTE_SET_USED(len);
#endif
}

uint16_t
gem_rx_pkt_burst(void *queue, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	struct gem_queue *q = queue;
	uint16_t nb_rx = 0;
	struct gem_priv *priv;
	static uint32_t rx_log_throttle;

	if (q == NULL || q->priv == NULL)
		return 0;

	priv = q->priv;

	if (priv->base_addr == NULL) {
		uint16_t i;

		if (!priv->sim_rx || q->mb_pool == NULL)
			return 0;

		for (i = 0; i < nb_bufs; i++) {
			struct rte_mbuf *m;
			char *data;
			uint16_t len = (uint16_t)(64 + (rte_rand() & 0x3F));

			m = rte_pktmbuf_alloc(q->mb_pool);
			if (m == NULL) {
				priv->rx_nombuf++;
				break;
			}

			data = rte_pktmbuf_append(m, len);
			if (data == NULL) {
				rte_pktmbuf_free(m);
				priv->rx_errs++;
				break;
			}

			memset(data, 0, len);
			if (len >= 14) {
				data[0] = 0x02; data[1] = 0x00; data[2] = 0x00;
				data[3] = 0x00; data[4] = 0x00; data[5] = 0x01;
				data[6] = 0x02; data[7] = 0x00; data[8] = 0x00;
				data[9] = 0x00; data[10] = 0x00; data[11] = 0x02;
				data[12] = 0x08; data[13] = 0x00;
			}

			bufs[i] = m;
			priv->rx_pkts++;
			priv->rx_bytes += len;
		}

		return i;
	}

	if (q->rx_ring == NULL || q->rx_sw_ring == NULL)
		return 0;

	while (nb_rx < nb_bufs) {
		struct gem_rx_bd *bd = &q->rx_ring[q->rx_head];
		uint32_t addr = bd->addr;
		uint32_t stat;
		uint16_t len;
		struct rte_mbuf *m;
		struct rte_mbuf *new_m;
		uint32_t new_addr_flags;

		if ((addr & GEM_RX_OWN) == 0)
			break;

		rte_prefetch0(bd);
		stat = bd->stat;
		len = (uint16_t)(stat & GEM_RX_LEN_MASK);

		m = q->rx_sw_ring[q->rx_head];
		if (m == NULL) {
			new_m = rte_pktmbuf_alloc(q->mb_pool);
			if (new_m == NULL) {
				priv->rx_nombuf++;
				break;
			}

			q->rx_sw_ring[q->rx_head] = new_m;
			new_addr_flags = (uint32_t)(rte_pktmbuf_iova(new_m) &
						    GEM_RX_ADDR_MASK);
			if (addr & GEM_RX_WRAP)
				new_addr_flags |= GEM_RX_WRAP;

			bd->stat = 0;
			rte_wmb();
			bd->addr = new_addr_flags;

			q->rx_head = (q->rx_head + 1) % GEM_DESC_NUM;
			continue;
		}

		if ((stat & (GEM_RX_SOF | GEM_RX_EOF)) != (GEM_RX_SOF | GEM_RX_EOF) ||
		    len == 0) {
			bd->stat = 0;
			rte_wmb();
			bd->addr = (addr & GEM_RX_WRAP) | (addr & ~GEM_RX_OWN);
			priv->rx_errs++;
			if ((++rx_log_throttle & 0xFFFFu) == 0)
				GEM_LOG("RX drop: bad frame stat=0x%08x len=%u", stat, len);
			q->rx_head = (q->rx_head + 1) % GEM_DESC_NUM;
			continue;
		}

		new_m = rte_pktmbuf_alloc(q->mb_pool);
		if (unlikely(new_m == NULL)) {
			priv->rx_nombuf++;
			break;
		}

		{
			uint64_t iova = rte_pktmbuf_iova(new_m);
			if (unlikely(iova > 0xFFFFFFFFULL || (iova & 0x3ULL))) {
				rte_pktmbuf_free(new_m);
				priv->rx_errs++;
				break;
			}
			q->rx_sw_ring[q->rx_head] = new_m;
			new_addr_flags = (uint32_t)(iova & GEM_RX_ADDR_MASK);
		}
		new_addr_flags |= (addr & GEM_RX_WRAP);

		bd->stat = 0;
		rte_wmb();
		bd->addr = new_addr_flags;

		m->pkt_len = len;
		m->data_len = len;
		m->nb_segs = 1;
		m->next = NULL;
		m->port = q->priv->port_id;
		m->ol_flags = 0;

		/* Non-coherent DMA: invalidate payload so CPU reads fresh bytes. */
		gem_dma_invalidate(rte_pktmbuf_mtod(m, const void *), len);

		bufs[nb_rx++] = m;
		priv->rx_pkts++;
		priv->rx_bytes += len;
		q->rx_head = (q->rx_head + 1) % GEM_DESC_NUM;
	}

	return nb_rx;
}
