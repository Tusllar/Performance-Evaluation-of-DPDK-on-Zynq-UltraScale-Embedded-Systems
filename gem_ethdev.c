/* SPDX-License-Identifier: BSD-3-Clause */
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>

#include <rte_ethdev_driver.h>
#include <rte_ethdev_vdev.h>
#include <rte_bus_vdev.h>
#include <rte_kvargs.h>
#include <rte_memzone.h>
#include <rte_memory.h>
#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_io.h>

#include "gem_ethdev.h"
#include "gem_rxtx.h"

#define GEM_REG_SIZE 0x1000

/* User-provided minimal GEM register offsets (base: GEM0 0x00FF0B0000) */
#define GEM_NCR             0x0000 /* network_control */
#define GEM_DMA_CFG         0x0010 /* dma_config */
#define GEM_TX_STATUS       0x0014 /* transmit_status (write-1-to-clear) */
#define GEM_RX_Q_PTR        0x0018 /* receive_q_ptr */
#define GEM_TX_Q_PTR        0x001C /* transmit_q_ptr */
#define GEM_RX_STATUS       0x0020 /* receive_status (write-1-to-clear) */

/* Register used for quick access probe */
#define GEM_ID              0x00FC

/* Network control bits (common for Cadence GEM / Zynq GEM).
 * Confirmed by user-provided 'network_control' bitfield summary:
 *   enable_receive  bit[2]
 *   enable_transmit bit[3]
 *   tx_start_pclk   bit[9] (self-clearing)
 */
#define GEM_NCR_RXEN         (1u << 2)
#define GEM_NCR_TXEN         (1u << 3)
#define GEM_NCR_TXSTART      (1u << 9)

RTE_LOG_REGISTER(gem_logtype, pmd.net.gem, NOTICE);

#define PMD_LOG(level, fmt, args...) \
	rte_log(RTE_LOG_ ## level, gem_logtype, \
		"%s(): " fmt "\n", __func__, ##args)

static inline uint32_t
gem_read32(struct gem_priv *priv, uint32_t off)
{
	volatile uint32_t *reg =
		(volatile uint32_t *)((char *)priv->base_addr + off);
	return rte_le_to_cpu_32(*reg);
}

static inline void
gem_write32(struct gem_priv *priv, uint32_t off, uint32_t val)
{
	volatile uint32_t *reg =
		(volatile uint32_t *)((char *)priv->base_addr + off);
	*reg = rte_cpu_to_le_32(val);
	rte_wmb();
}

static int
gem_kvarg_base_handler(const char *key, const char *value, void *opaque)
{
	uint64_t *out = (uint64_t *)opaque;

	if (key == NULL || value == NULL || out == NULL)
		return 0;

	if (strcmp(key, "base") == 0)
		*out = strtoull(value, NULL, 0);

	return 0;
}

static int
gem_kvarg_sim_rx_handler(const char *key, const char *value, void *opaque)
{
	uint8_t *out = (uint8_t *)opaque;

	if (key == NULL || value == NULL || out == NULL)
		return 0;

	if (strcmp(key, "sim_rx") == 0)
		*out = (uint8_t)strtoul(value, NULL, 0);

	return 0;
}

static int
gem_runtime_is_hw(void)
{
	if (access("/dev/mem", F_OK) == 0)
		return 1;

	return 0;
}

static struct rte_eth_link gem_link = {
	.link_speed = ETH_SPEED_NUM_1G,
	.link_duplex = ETH_LINK_FULL_DUPLEX,
	// .link_status = ETH_LINK_DOWN,
	.link_status = ETH_LINK_UP,
	.link_autoneg = ETH_LINK_FIXED,
};

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

	/* If running without mapped HW, keep simulation behavior. */
	if (priv->base_addr == NULL || rxq == NULL || txq == NULL) {
		dev->data->dev_link.link_status = ETH_LINK_UP;
		return 0;
	}

	/* Program descriptor ring base addresses (32-bit addressing) */
	gem_write32(priv, GEM_RX_Q_PTR, (uint32_t)rxq->rx_ring_iova);
	gem_write32(priv, GEM_TX_Q_PTR, (uint32_t)txq->tx_ring_iova);

	/* Clear status latches */
	gem_write32(priv, GEM_TX_STATUS, 0xFFFFFFFFu);
	gem_write32(priv, GEM_RX_STATUS, 0xFFFFFFFFu);

	/* Enable RX/TX */
	gem_write32(priv, GEM_NCR, GEM_NCR_RXEN | GEM_NCR_TXEN);

	dev->data->dev_link.link_status = ETH_LINK_UP;
	return 0;
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

static int
gem_dev_close(struct rte_eth_dev *dev)
{
	PMD_LOG(INFO, "Closing GEM device");

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	dev->data->mac_addrs = NULL;

	return 0;
}

static int
gem_dev_infos_get(struct rte_eth_dev *dev,
	struct rte_eth_dev_info *dev_info)
{
	struct gem_priv *priv = dev->data->dev_private;

	dev_info->max_rx_queues =
		RTE_DIM(priv->rxq);

	dev_info->max_tx_queues =
		RTE_DIM(priv->txq);

	dev_info->max_mac_addrs = 1;

	return 0;
}

static int
gem_rx_queue_setup(struct rte_eth_dev *dev,
	uint16_t qid,
	uint16_t nb_desc,
	unsigned int socket_id,
	const struct rte_eth_rxconf *conf,
	struct rte_mempool *mb_pool)
{
	struct gem_priv *priv = dev->data->dev_private;
	struct gem_queue *q;
	const struct rte_memzone *mz;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	uint32_t i;

	RTE_SET_USED(nb_desc);
	RTE_SET_USED(socket_id);
	RTE_SET_USED(conf);

	q = &priv->rxq[qid];
	memset(q, 0, sizeof(*q));
	q->priv = priv;
	q->mb_pool = mb_pool;

	/* Allocate RX descriptor ring in DMA-safe memory */
	snprintf(mz_name, sizeof(mz_name), "gem_rx_bd_%u_%u",
		 priv->port_id, qid);
	mz = rte_memzone_reserve_aligned(mz_name,
					 sizeof(struct gem_rx_bd) * GEM_DESC_NUM,
					 socket_id,
					 RTE_MEMZONE_IOVA_CONTIG,
					 RTE_CACHE_LINE_SIZE);
	if (mz == NULL)
		return -ENOMEM;

	q->rx_ring = mz->addr;
	q->rx_ring_iova = mz->iova;

	q->rx_sw_ring = rte_zmalloc(NULL,
				    sizeof(struct rte_mbuf *) * GEM_DESC_NUM,
				    RTE_CACHE_LINE_SIZE);
	if (q->rx_sw_ring == NULL)
		return -ENOMEM;

	/* Initialize RX ring: WRAP on last, prefill buffers, OWN=0 (HW can write) */
	for (i = 0; i < GEM_DESC_NUM; i++) {
		struct rte_mbuf *m = rte_pktmbuf_alloc(mb_pool);
		uint32_t addr_flags;
		if (m == NULL)
			return -ENOMEM;
		q->rx_sw_ring[i] = m;

		addr_flags = (uint32_t)(rte_pktmbuf_iova(m) & GEM_RX_ADDR_MASK);
		if (i == (GEM_DESC_NUM - 1))
			addr_flags |= GEM_RX_WRAP;
		/* OWN must be 0 for HW to write */
		q->rx_ring[i].addr = addr_flags;
		q->rx_ring[i].stat = 0;
	}
	q->rx_head = 0;

	dev->data->rx_queues[qid] = q;

	return 0;
}

static int
gem_tx_queue_setup(struct rte_eth_dev *dev,
	uint16_t qid,
	uint16_t nb_desc,
	unsigned int socket_id,
	const struct rte_eth_txconf *conf)
{
	struct gem_priv *priv = dev->data->dev_private;
	struct gem_queue *q;
	const struct rte_memzone *mz;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	uint32_t i;

	RTE_SET_USED(nb_desc);
	RTE_SET_USED(socket_id);
	RTE_SET_USED(conf);

	q = &priv->txq[qid];
	memset(q, 0, sizeof(*q));
	q->priv = priv;

	snprintf(mz_name, sizeof(mz_name), "gem_tx_bd_%u_%u",
		 priv->port_id, qid);
	mz = rte_memzone_reserve_aligned(mz_name,
					 sizeof(struct gem_tx_bd) * GEM_DESC_NUM,
					 socket_id,
					 RTE_MEMZONE_IOVA_CONTIG,
					 RTE_CACHE_LINE_SIZE);
	if (mz == NULL)
		return -ENOMEM;

	q->tx_ring = mz->addr;
	q->tx_ring_iova = mz->iova;

	q->tx_sw_ring = rte_zmalloc(NULL,
				    sizeof(struct rte_mbuf *) * GEM_DESC_NUM,
				    RTE_CACHE_LINE_SIZE);
	if (q->tx_sw_ring == NULL)
		return -ENOMEM;

	/* Init TX ring: mark all descriptors as free (USED=1), WRAP on last. */
	for (i = 0; i < GEM_DESC_NUM; i++) {
		q->tx_ring[i].addr = 0;
		q->tx_ring[i].ctrl = GEM_TX_USED | (i == (GEM_DESC_NUM - 1) ? GEM_TX_WRAP : 0);
		q->tx_sw_ring[i] = NULL;
	}
	q->tx_head = 0;
	q->tx_tail = 0;

	dev->data->tx_queues[qid] = q;

	return 0;
}

static void
gem_queue_release(void *q)
{
	RTE_SET_USED(q);
}

static int
gem_stats_get(struct rte_eth_dev *dev,
	struct rte_eth_stats *stats)
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
gem_link_update(struct rte_eth_dev *dev,
	int wait)
{
	RTE_SET_USED(wait);

	dev->data->dev_link = gem_link;

	return 0;
}

static int
gem_mac_addr_set(struct rte_eth_dev *dev,
	struct rte_ether_addr *addr)
{
	struct gem_priv *priv = dev->data->dev_private;

	rte_ether_addr_copy(addr, &priv->mac_addr);

	return 0;
}

static const struct eth_dev_ops gem_ops = {
	.dev_configure = gem_dev_configure,
	.dev_start = gem_dev_start,
	.dev_stop = gem_dev_stop,
	.dev_close = gem_dev_close,

	.dev_infos_get = gem_dev_infos_get,

	.rx_queue_setup = gem_rx_queue_setup,
	.tx_queue_setup = gem_tx_queue_setup,

	.rx_queue_release = gem_queue_release,
	.tx_queue_release = gem_queue_release,

	.link_update = gem_link_update,

	.stats_get = gem_stats_get,
	.stats_reset = gem_stats_reset,

	.mac_addr_set = gem_mac_addr_set,
};

static uint64_t
gem_get_base_from_sysfs(void)
{
	FILE *f;
	const char *paths[] = {
		/* Common ZynqMP GEM instances */
		"/proc/device-tree/amba/ethernet@ff0e0000/reg",
		"/proc/device-tree/amba/ethernet@ff0b0000/reg",
	};

	uint64_t reg[2];
	size_t i;

	for (i = 0; i < RTE_DIM(paths); i++) {
		f = fopen(paths[i], "rb");
		if (!f)
			continue;

		if (fread(reg, sizeof(reg), 1, f) != 1) {
			fclose(f);
			continue;
		}
		fclose(f);

		return reg[1];
	}

	return 0;
}

static int
gem_hw_map(struct gem_priv *priv, uint64_t base)
{
	int fd;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		return -1;

	priv->base_addr = mmap(NULL,
			       GEM_REG_SIZE,
			       PROT_READ | PROT_WRITE,
			       MAP_SHARED,
			       fd,
			       base);

	close(fd);

	if (priv->base_addr == MAP_FAILED)
		return -1;

	return 0;
}

static void
gem_hw_reset(struct gem_priv *priv)
{
	volatile uint32_t *ncr;

	ncr = (uint32_t *)((char *)priv->base_addr + GEM_NCR);

	*ncr = 0x0;

	rte_delay_ms(10);

	PMD_LOG(INFO, "GEM MAC reset done");
}

static void
gem_hw_probe(struct gem_priv *priv)
{
	volatile uint32_t *id;

	id = (uint32_t *)((char *)priv->base_addr + GEM_ID);

	PMD_LOG(INFO,
		"GEM register access OK, ID = 0x%x",
		*id);
}

int
gem_dev_create(struct rte_vdev_device *dev)
{
	struct rte_eth_dev *eth_dev;
	struct gem_priv *priv;
	const char *args;
	const char *const valid_keys[] = { "base", "sim_rx", NULL };

	eth_dev = rte_eth_vdev_allocate(dev, sizeof(*priv));
	if (!eth_dev)
		return -ENOMEM;

	priv = eth_dev->data->dev_private;

	uint64_t base;

	/* Optional vdev args: --vdev='net_gem0,base=0x00FF0B0000' */
	args = rte_vdev_device_args(dev);
	base = 0;
	priv->sim_rx = 0;
	if (args != NULL) {
		struct rte_kvargs *kv = rte_kvargs_parse(args, valid_keys);
		if (kv != NULL) {
			/* DPDK 20.11: use rte_kvargs_process callback (no rte_kvargs_get). */
			rte_kvargs_process(kv, "base",
					   gem_kvarg_base_handler, &base);
			rte_kvargs_process(kv, "sim_rx",
					   gem_kvarg_sim_rx_handler, &priv->sim_rx);
			rte_kvargs_free(kv);
		}
	}

	if (!base)
		base = gem_get_base_from_sysfs();

if (!base) {

    PMD_LOG(INFO,
        "GEM hardware not found, running in simulation mode");

} else {

    PMD_LOG(INFO,
        "Detected GEM base address: 0x%lx", base);

    if (gem_hw_map(priv, base)) {
        PMD_LOG(ERR, "GEM mmap failed");
        return -1;
    }

    PMD_LOG(INFO,
        "GEM registers mapped at %p",
        priv->base_addr);

    gem_hw_reset(priv);
    gem_hw_probe(priv);
}

	priv->port_id = eth_dev->data->port_id;

	rte_eth_random_addr(priv->mac_addr.addr_bytes);

	eth_dev->dev_ops = &gem_ops;

	eth_dev->rx_pkt_burst = gem_rx_pkt_burst;
	eth_dev->tx_pkt_burst = gem_tx_pkt_burst;

	eth_dev->data->nb_rx_queues = 1;
	eth_dev->data->nb_tx_queues = 1;

	eth_dev->data->mac_addrs = &priv->mac_addr;
	eth_dev->data->dev_link = gem_link;

	rte_eth_dev_probing_finish(eth_dev);

	return 0;
}

static int
rte_pmd_gem_probe(struct rte_vdev_device *dev)
{
	PMD_LOG(INFO, "Initializing GEM PMD");

	return gem_dev_create(dev);
}

static int
rte_pmd_gem_remove(struct rte_vdev_device *dev)
{
	struct rte_eth_dev *eth_dev;

	eth_dev = rte_eth_dev_allocated(
		rte_vdev_device_name(dev));

	if (eth_dev == NULL)
		return 0;

	gem_dev_close(eth_dev);

	rte_eth_dev_release_port(eth_dev);

	return 0;
}

static struct rte_vdev_driver pmd_gem_drv = {
	.probe = rte_pmd_gem_probe,
	.remove = rte_pmd_gem_remove,
};

RTE_PMD_REGISTER_VDEV(net_gem, pmd_gem_drv);
RTE_PMD_REGISTER_ALIAS(net_gem, eth_gem);