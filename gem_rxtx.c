#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_prefetch.h>
#include <rte_io.h>
#include <rte_byteorder.h>
#include <rte_random.h>

#include "gem_ethdev.h"
#include "gem_rxtx.h"

/* Keep a minimal MMIO helper local to datapath for TX kick (self-clearing bit). */
#define GEM_NCR_OFFSET       0x0000u
#define GEM_NCR_TXSTART      (1u << 9)

static inline void
gem_mmio_set_bits(struct gem_priv *priv, uint32_t off, uint32_t bits)
{
	volatile uint32_t *reg =
		(volatile uint32_t *)((char *)priv->base_addr + off);
	uint32_t v = rte_le_to_cpu_32(*reg);
	*reg = rte_cpu_to_le_32(v | bits);
	rte_wmb();
}

uint16_t
gem_rx_pkt_burst(void *queue,
	struct rte_mbuf **bufs,
	uint16_t nb_bufs)
{
	struct gem_queue *q = queue;
	uint16_t nb_rx = 0;
	struct gem_priv *priv;

	if (q == NULL || q->priv == NULL)
		return 0;

	priv = q->priv;

	/* ===== FAKE RX MODE (QEMU) =====
	 * Enable via: --vdev="net_gem0,sim_rx=1"
	 */
	if (priv->base_addr == NULL) {
		uint16_t i;

		if (!priv->sim_rx || q->mb_pool == NULL)
			return 0;

		for (i = 0; i < nb_bufs; i++) {
			struct rte_mbuf *m;
			char *data;
			uint16_t len = (uint16_t)(64 + (rte_rand() & 0x3F)); /* 64..127 */

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

			/* Fake Ethernet header: dst/src + EtherType IPv4 */
			if (len >= 14) {
				/* dst MAC: 02:00:00:00:00:01 */
				data[0] = 0x02; data[1] = 0x00; data[2] = 0x00;
				data[3] = 0x00; data[4] = 0x00; data[5] = 0x01;
				/* src MAC: 02:00:00:00:00:02 */
				data[6] = 0x02; data[7] = 0x00; data[8] = 0x00;
				data[9] = 0x00; data[10] = 0x00; data[11] = 0x02;
				/* EtherType: IPv4 */
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

		/* HW sets OWN=1 when frame written */
		if ((addr & GEM_RX_OWN) == 0)
			break;

		rte_prefetch0(bd);
		stat = bd->stat;
		len = (uint16_t)(stat & GEM_RX_LEN_MASK);

		m = q->rx_sw_ring[q->rx_head];
		if (m == NULL) {
			/* Should not happen; recycle descriptor */
			bd->addr = addr & ~GEM_RX_OWN; /* clear OWN -> give back to HW */
			q->rx_head = (q->rx_head + 1) % GEM_DESC_NUM;
			continue;
		}

		/* Minimal bring-up: only accept single-buffer complete frames */
		if ((stat & (GEM_RX_SOF | GEM_RX_EOF)) != (GEM_RX_SOF | GEM_RX_EOF) ||
		    len == 0) {
			/* Drop and recycle */
			bd->stat = 0;
			bd->addr = addr & ~GEM_RX_OWN; /* clear OWN -> HW can refill */
			q->rx_head = (q->rx_head + 1) % GEM_DESC_NUM;
			continue;
		}

		m->pkt_len = len;
		m->data_len = len;
		bufs[nb_rx++] = m;
		priv->rx_pkts++;
		priv->rx_bytes += len;

		/* Refill: allocate a new mbuf and give it to HW (OWN=0) */
		new_m = rte_pktmbuf_alloc(q->mb_pool);
		if (new_m == NULL) {
			/* If we cannot refill, keep ownership with SW (OWN=1) so we don't lose it */
			q->rx_sw_ring[q->rx_head] = NULL;
			priv->rx_nombuf++;
			q->rx_head = (q->rx_head + 1) % GEM_DESC_NUM;
			continue;
		}

		q->rx_sw_ring[q->rx_head] = new_m;

		new_addr_flags = (uint32_t)(rte_pktmbuf_iova(new_m) & GEM_RX_ADDR_MASK);
		if (addr & GEM_RX_WRAP)
			new_addr_flags |= GEM_RX_WRAP;

		/* Clear status and clear OWN -> hand back to HW */
		bd->stat = 0;
		rte_wmb();
		bd->addr = new_addr_flags; /* OWN=0 */

		q->rx_head = (q->rx_head + 1) % GEM_DESC_NUM;
	}

	return nb_rx;
}

static inline void
gem_tx_cleanup(struct gem_queue *q)
{
	while (q->tx_tail != q->tx_head) {
		struct gem_tx_bd *bd = &q->tx_ring[q->tx_tail];

		/* HW sets USED=1 when it is done with this descriptor */
		if ((bd->ctrl & GEM_TX_USED) == 0)
			break;

		if (q->tx_sw_ring[q->tx_tail] != NULL) {
			rte_pktmbuf_free(q->tx_sw_ring[q->tx_tail]);
			q->tx_sw_ring[q->tx_tail] = NULL;
		}

		q->tx_tail = (q->tx_tail + 1) % GEM_DESC_NUM;
	}
}

uint16_t
gem_tx_pkt_burst(void *queue,
	struct rte_mbuf **bufs,
	uint16_t nb_bufs)
{
	struct gem_queue *q = queue;
	uint16_t nb_tx = 0;
	struct gem_priv *priv;

	if (q == NULL || q->priv == NULL || q->priv->base_addr == NULL) {
		uint16_t i;
	
		if (q == NULL || q->priv == NULL) {
			for (i = 0; i < nb_bufs; i++)
				rte_pktmbuf_free(bufs[i]);
			return nb_bufs;
		}
	
		struct gem_priv *priv = q->priv;
	
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

	/* Free completed TX mbufs */
	gem_tx_cleanup(q);

	while (nb_tx < nb_bufs) {
		struct gem_tx_bd *bd = &q->tx_ring[q->tx_head];
		struct rte_mbuf *m = bufs[nb_tx];
		uint32_t ctrl;

		/* Descriptor must be free (USED=1) */
		if ((bd->ctrl & GEM_TX_USED) == 0)
			break;

		/* Single-buffer TX only for bring-up */
		if (m->pkt_len > GEM_TX_LEN_MASK)
			break;

		q->tx_sw_ring[q->tx_head] = m;

		bd->addr = (uint32_t)rte_pktmbuf_iova(m);

		ctrl = (uint32_t)(m->pkt_len & GEM_TX_LEN_MASK) | GEM_TX_LAST;
		if (bd->ctrl & GEM_TX_WRAP)
			ctrl |= GEM_TX_WRAP;

		/* Hand descriptor to HW by clearing USED bit (USED=0) */
		rte_wmb();
		bd->ctrl = ctrl; /* USED=0 */

		q->tx_head = (q->tx_head + 1) % GEM_DESC_NUM;
		nb_tx++;
		priv->tx_pkts++;
		priv->tx_bytes += m->pkt_len;
	}

	/* Some GEM configurations require a "kick" (NCR tx_start_pclk, self-clearing). */
	if (nb_tx > 0 && q->priv != NULL && q->priv->base_addr != NULL)
		gem_mmio_set_bits(q->priv, GEM_NCR_OFFSET, GEM_NCR_TXSTART);

	return nb_tx;
}