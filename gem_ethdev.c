/* SPDX-License-Identifier: BSD-3-Clause */
#include <string.h>
#include <sys/mman.h>

#include <rte_malloc.h>
#include <rte_memzone.h>

#include "gem_internal.h"

static int
gem_dev_configure(struct rte_eth_dev *dev)
{
	RTE_SET_USED(dev);
	return 0;
}

static int
gem_dev_start(struct rte_eth_dev *dev)
{
	struct gem_priv *priv = dev->data->dev_private;
	struct gem_queue *rxq = dev->data->rx_queues[0];
	struct gem_queue *txq = dev->data->tx_queues[0];
	uint32_t net_cfg;

	if (priv->base_addr == NULL || rxq == NULL || txq == NULL) {
		dev->data->dev_link.link_status = ETH_LINK_DOWN;
		return 0;
	}

	gem_write32(priv, GEM_RX_Q_PTR, (uint32_t)rxq->rx_ring_iova);
	gem_write32(priv, GEM_TX_Q_PTR, (uint32_t)txq->tx_ring_iova);
	PMD_LOG(INFO, "Rings: RX_Q_PTR=0x%08x TX_Q_PTR=0x%08x (rx_iova=0x%llx tx_iova=0x%llx)",
		(uint32_t)rxq->rx_ring_iova, (uint32_t)txq->tx_ring_iova,
		(unsigned long long)rxq->rx_ring_iova, (unsigned long long)txq->tx_ring_iova);

	gem_write32(priv, GEM_DMA_CFG, GEM_DMA_CFG_INIT);
	PMD_LOG(INFO, "DMA_CFG=0x%08x", GEM_DMA_CFG_INIT);

	net_cfg = GEM_NET_CFG_VAL;
	net_cfg &= ~(0x7u << 18);
	net_cfg |= ((uint32_t)(priv->mdc_div_bits & 0x7u) << 18);
	gem_write32(priv, GEM_NET_CFG, net_cfg);
	PMD_LOG(INFO, "NET_CFG=0x%08x (mdc_div_bits=%u)", net_cfg, priv->mdc_div_bits & 0x7u);

	{
		const uint8_t *a = priv->mac_addr.addr_bytes;
		uint32_t bot = (uint32_t)a[0]        |
			       ((uint32_t)a[1] << 8)  |
			       ((uint32_t)a[2] << 16) |
			       ((uint32_t)a[3] << 24);
		uint32_t top = (uint32_t)a[4] | ((uint32_t)a[5] << 8);
		gem_write32(priv, GEM_SPEC_ADD1_BOT, bot);
		gem_write32(priv, GEM_SPEC_ADD1_TOP, top);
		PMD_LOG(INFO, "MAC %02x:%02x:%02x:%02x:%02x:%02x programmed to HW",
			a[0], a[1], a[2], a[3], a[4], a[5]);
	}

	gem_write32(priv, GEM_TX_STATUS, 0xFFu);
	gem_write32(priv, GEM_RX_STATUS, 0x0Fu);
	PMD_LOG(DEBUG, "TX_STATUS after clear = 0x%08x",
		gem_read32(priv, GEM_TX_STATUS));

	gem_write32(priv, GEM_NCR,
		    GEM_NCR_MAN_PORT_EN | GEM_NCR_RXEN | GEM_NCR_TXEN);
	PMD_LOG(INFO, "NCR set: 0x%08x", gem_read32(priv, GEM_NCR));

	return gem_link_update(dev, 1);
}

static int
gem_dev_stop(struct rte_eth_dev *dev)
{
	struct gem_priv *priv = dev->data->dev_private;

	if (priv->base_addr != NULL)
		gem_write32(priv, GEM_NCR, 0);

	dev->data->dev_link.link_status = ETH_LINK_DOWN;
	return 0;
}

int
gem_dev_close(struct rte_eth_dev *dev)
{
	PMD_LOG(INFO, "Closing GEM device");

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	if (dev->data->dev_private != NULL) {
		struct gem_priv *priv = dev->data->dev_private;
		if (priv->base_addr != NULL) {
			munmap(priv->base_addr, GEM_REG_SIZE);
			priv->base_addr = NULL;
		}
	}

	dev->data->mac_addrs = NULL;
	return 0;
}

static int
gem_dev_infos_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
	struct gem_priv *priv = dev->data->dev_private;

	dev_info->max_rx_queues = RTE_DIM(priv->rxq);
	dev_info->max_tx_queues = RTE_DIM(priv->txq);
	dev_info->max_mac_addrs = 1;
	dev_info->rx_desc_lim.nb_min = 64;
	dev_info->rx_desc_lim.nb_max = GEM_DESC_NUM;
	dev_info->tx_desc_lim.nb_min = 64;
	dev_info->tx_desc_lim.nb_max = GEM_DESC_NUM;

	return 0;
}

static int
gem_rxq_info_get(struct rte_eth_dev *dev, uint16_t qid,
		 struct rte_eth_rxq_info *qinfo)
{
	struct gem_queue *q;

	if (qid >= dev->data->nb_rx_queues || qinfo == NULL)
		return -EINVAL;

	q = dev->data->rx_queues[qid];
	if (q == NULL)
		return -EINVAL;

	memset(qinfo, 0, sizeof(*qinfo));
	qinfo->nb_desc = GEM_DESC_NUM;
	qinfo->mp = q->mb_pool;
	return 0;
}

static int
gem_txq_info_get(struct rte_eth_dev *dev, uint16_t qid,
		 struct rte_eth_txq_info *qinfo)
{
	if (qid >= dev->data->nb_tx_queues || qinfo == NULL)
		return -EINVAL;

	if (dev->data->tx_queues[qid] == NULL)
		return -EINVAL;

	memset(qinfo, 0, sizeof(*qinfo));
	qinfo->nb_desc = GEM_DESC_NUM;
	return 0;
}

static int
gem_rx_queue_setup(struct rte_eth_dev *dev, uint16_t qid, uint16_t nb_desc,
		   unsigned int socket_id, const struct rte_eth_rxconf *conf,
		   struct rte_mempool *mb_pool)
{
	struct gem_priv *priv = dev->data->dev_private;
	struct gem_queue *q;
	const struct rte_memzone *mz = NULL;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	uint32_t i;
	uint32_t attempt;

	RTE_SET_USED(conf);

	if (nb_desc != 0 && nb_desc != GEM_DESC_NUM)
		return -EINVAL;

	q = &priv->rxq[qid];
	memset(q, 0, sizeof(*q));
	q->priv = priv;
	q->mb_pool = mb_pool;

	/*
	 * Zynq GEM DMA uses 32-bit addressing. If hugepages/memsegs are backed
	 * by physical addresses above 4GB, we must retry reserve until we find
	 * an IOVA <= 0xFFFFFFFF and 4B-aligned.
	 */
	for (attempt = 0; attempt < 8; attempt++) {
		snprintf(mz_name, sizeof(mz_name), "gem_rx_bd_%u_%u_try%u",
			 priv->port_id, qid, attempt);
		mz = rte_memzone_reserve_aligned(mz_name,
						 sizeof(struct gem_rx_bd) *
						 GEM_DESC_NUM,
						 socket_id,
						 RTE_MEMZONE_IOVA_CONTIG,
						 RTE_CACHE_LINE_SIZE);
		if (mz == NULL)
			continue;

		q->rx_ring = mz->addr;
		q->rx_ring_iova = mz->iova;
		if (q->rx_ring_iova <= 0xFFFFFFFFULL &&
		    (q->rx_ring_iova & 0x3ULL) == 0)
			break;

		PMD_LOG(WARNING, "RX ring iova not 32-bit/4B aligned (try=%u): 0x%llx",
			attempt, (unsigned long long)q->rx_ring_iova);
		(void)rte_memzone_free(mz);
		mz = NULL;
		q->rx_ring = NULL;
		q->rx_ring_iova = 0;
	}
	if (mz == NULL)
		return -ENOTSUP;

	q->rx_sw_ring = rte_zmalloc(NULL,
				    sizeof(struct rte_mbuf *) * GEM_DESC_NUM,
				    RTE_CACHE_LINE_SIZE);
	if (q->rx_sw_ring == NULL)
		return -ENOMEM;

	for (i = 0; i < GEM_DESC_NUM; i++) {
		struct rte_mbuf *m = rte_pktmbuf_alloc(mb_pool);
		uint32_t addr_flags;
		if (m == NULL)
			return -ENOMEM;
		q->rx_sw_ring[i] = m;
		addr_flags = (uint32_t)(rte_pktmbuf_iova(m) & GEM_RX_ADDR_MASK);
		if (i == (GEM_DESC_NUM - 1))
			addr_flags |= GEM_RX_WRAP;
		q->rx_ring[i].addr = addr_flags;
		q->rx_ring[i].stat = 0;
	}
	q->rx_head = 0;
	dev->data->rx_queues[qid] = q;
	return 0;
}

static int
gem_tx_queue_setup(struct rte_eth_dev *dev, uint16_t qid, uint16_t nb_desc,
		   unsigned int socket_id, const struct rte_eth_txconf *conf)
{
	struct gem_priv *priv = dev->data->dev_private;
	struct gem_queue *q;
	const struct rte_memzone *mz = NULL;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	uint32_t i;
	uint32_t attempt;

	RTE_SET_USED(conf);

	if (nb_desc != 0 && nb_desc != GEM_DESC_NUM)
		return -EINVAL;

	q = &priv->txq[qid];
	memset(q, 0, sizeof(*q));
	q->priv = priv;

	/*
	 * Zynq GEM DMA uses 32-bit addressing. Retry until tx ring IOVA fits.
	 */
	for (attempt = 0; attempt < 8; attempt++) {
		snprintf(mz_name, sizeof(mz_name), "gem_tx_bd_%u_%u_try%u",
			 priv->port_id, qid, attempt);
		mz = rte_memzone_reserve_aligned(mz_name,
						 sizeof(struct gem_tx_bd) *
						 GEM_DESC_NUM,
						 socket_id,
						 RTE_MEMZONE_IOVA_CONTIG,
						 RTE_CACHE_LINE_SIZE);
		if (mz == NULL)
			continue;

		q->tx_ring = mz->addr;
		q->tx_ring_iova = mz->iova;
		if (q->tx_ring_iova <= 0xFFFFFFFFULL &&
		    (q->tx_ring_iova & 0x3ULL) == 0)
			break;

		PMD_LOG(WARNING, "TX ring iova not 32-bit/4B aligned (try=%u): 0x%llx",
			attempt, (unsigned long long)q->tx_ring_iova);
		(void)rte_memzone_free(mz);
		mz = NULL;
		q->tx_ring = NULL;
		q->tx_ring_iova = 0;
	}
	if (mz == NULL)
		return -ENOTSUP;

	q->tx_sw_ring = rte_zmalloc(NULL,
				    sizeof(struct rte_mbuf *) * GEM_DESC_NUM,
				    RTE_CACHE_LINE_SIZE);
	if (q->tx_sw_ring == NULL)
		return -ENOMEM;

	for (i = 0; i < GEM_DESC_NUM; i++) {
		q->tx_ring[i].addr = 0;
		q->tx_ring[i].ctrl = GEM_TX_USED |
			(i == (GEM_DESC_NUM - 1) ? GEM_TX_WRAP : 0);
		q->tx_sw_ring[i] = NULL;
	}
	q->tx_head = 0;
	q->tx_tail = 0;
	q->tx_pending = 0;

	dev->data->tx_queues[qid] = q;
	return 0;
}

static void
gem_queue_release(void *q)
{
	RTE_SET_USED(q);
}

static int
gem_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct gem_priv *priv = dev->data->dev_private;

	memset(stats, 0, sizeof(*stats));
	stats->ipackets = priv->rx_pkts;
	stats->ibytes = priv->rx_bytes;
	stats->ierrors = priv->rx_errs;
	stats->rx_nombuf = priv->rx_nombuf;
	stats->opackets = priv->tx_pkts;
	stats->obytes = priv->tx_bytes;
	stats->oerrors = priv->tx_errs;
	return 0;
}

static int
gem_stats_reset(struct rte_eth_dev *dev)
{
	struct gem_priv *priv = dev->data->dev_private;

	priv->rx_pkts = 0;
	priv->rx_bytes = 0;
	priv->rx_errs = 0;
	priv->rx_nombuf = 0;
	priv->tx_pkts = 0;
	priv->tx_bytes = 0;
	priv->tx_errs = 0;
	return 0;
}

static int
gem_mac_addr_set(struct rte_eth_dev *dev, struct rte_ether_addr *addr)
{
	struct gem_priv *priv = dev->data->dev_private;
	uint32_t bot, top;

	rte_ether_addr_copy(addr, &priv->mac_addr);
	if (priv->base_addr == NULL)
		return 0;

	bot = (uint32_t)addr->addr_bytes[0]        |
	      ((uint32_t)addr->addr_bytes[1] << 8)  |
	      ((uint32_t)addr->addr_bytes[2] << 16) |
	      ((uint32_t)addr->addr_bytes[3] << 24);
	top = (uint32_t)addr->addr_bytes[4] |
	      ((uint32_t)addr->addr_bytes[5] << 8);

	gem_write32(priv, GEM_SPEC_ADD1_BOT, bot);
	gem_write32(priv, GEM_SPEC_ADD1_TOP, top);
	return 0;
}

static const struct eth_dev_ops gem_ops = {
	.dev_configure = gem_dev_configure,
	.dev_start = gem_dev_start,
	.dev_stop = gem_dev_stop,
	.dev_close = gem_dev_close,
	.dev_infos_get = gem_dev_infos_get,
	.rxq_info_get = gem_rxq_info_get,
	.txq_info_get = gem_txq_info_get,
	.rx_queue_setup = gem_rx_queue_setup,
	.tx_queue_setup = gem_tx_queue_setup,
	.rx_queue_release = gem_queue_release,
	.tx_queue_release = gem_queue_release,
	.link_update = gem_link_update,
	.stats_get = gem_stats_get,
	.stats_reset = gem_stats_reset,
	.mac_addr_set = gem_mac_addr_set,
};

const struct eth_dev_ops *
gem_get_eth_dev_ops(void)
{
	return &gem_ops;
}
