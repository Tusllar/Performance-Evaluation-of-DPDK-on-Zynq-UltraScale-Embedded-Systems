/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <rte_bus_vdev.h>
#include <rte_ethdev_vdev.h>
#include <rte_kvargs.h>

#include "gem_internal.h"
#include "gem_rxtx.h"

RTE_LOG_REGISTER(gem_logtype, pmd.net.gem, NOTICE);

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
gem_kvarg_phy_addr_handler(const char *key, const char *value, void *opaque)
{
	uint8_t *out = (uint8_t *)opaque;

	if (key == NULL || value == NULL || out == NULL)
		return 0;

	if (strcmp(key, "phy_addr") == 0)
		*out = (uint8_t)(strtoul(value, NULL, 0) & 0x1Fu);

	return 0;
}

static int
gem_kvarg_autoneg_handler(const char *key, const char *value, void *opaque)
{
	uint8_t *out = (uint8_t *)opaque;

	if (key == NULL || value == NULL || out == NULL)
		return 0;

	if (strcmp(key, "autoneg") == 0)
		*out = (uint8_t)((strtoul(value, NULL, 0) != 0) ? 1 : 0);

	return 0;
}

static int
gem_kvarg_mdc_div_handler(const char *key, const char *value, void *opaque)
{
	uint8_t *out = (uint8_t *)opaque;

	if (key == NULL || value == NULL || out == NULL)
		return 0;

	if (strcmp(key, "mdc_div") == 0)
		*out = (uint8_t)(strtoul(value, NULL, 0) & 0x7u);

	return 0;
}

int
gem_dev_create(struct rte_vdev_device *dev)
{
	struct rte_eth_dev *eth_dev;
	struct gem_priv *priv;
	const char *args;
	const char *const valid_keys[] = {
		"base", "sim_rx", "phy_addr", "autoneg", "mdc_div", NULL
	};
	uint64_t base = 0;

	eth_dev = rte_eth_vdev_allocate(dev, sizeof(*priv));
	if (!eth_dev)
		return -ENOMEM;

	priv = eth_dev->data->dev_private;

	args = rte_vdev_device_args(dev);
	priv->sim_rx = 0;
	priv->phy_addr = 0x0Cu;
	priv->autoneg = 1;
	priv->mdc_div_bits = 5;

	if (args != NULL) {
		struct rte_kvargs *kv = rte_kvargs_parse(args, valid_keys);
		if (kv != NULL) {
			rte_kvargs_process(kv, "base", gem_kvarg_base_handler, &base);
			rte_kvargs_process(kv, "sim_rx", gem_kvarg_sim_rx_handler,
					   &priv->sim_rx);
			rte_kvargs_process(kv, "phy_addr", gem_kvarg_phy_addr_handler,
					   &priv->phy_addr);
			rte_kvargs_process(kv, "autoneg", gem_kvarg_autoneg_handler,
					   &priv->autoneg);
			rte_kvargs_process(kv, "mdc_div", gem_kvarg_mdc_div_handler,
					   &priv->mdc_div_bits);
			rte_kvargs_free(kv);
		}
	}

	if (!base)
		base = gem_get_base_from_sysfs();

	if (!base) {
		PMD_LOG(INFO, "GEM hardware not found, running in simulation mode");
	} else {
		uint32_t net_cfg;

		PMD_LOG(INFO, "Detected GEM base address: 0x%lx", base);

		if (gem_hw_map(priv, base)) {
			PMD_LOG(ERR, "GEM mmap failed");
			return -1;
		}

		PMD_LOG(INFO, "GEM registers mapped at %p", priv->base_addr);
		gem_hw_init(priv);
		gem_hw_probe(priv);
		/*
		 * Program NET_CFG (at least MDC divider bits) before MDIO access.
		 * PHY probing/init happens in gem_phy_init(), so MDIO clock must
		 * be valid at this point.
		 */
		net_cfg = GEM_NET_CFG_VAL;
		net_cfg &= ~(0x7u << 18);
		net_cfg |= ((uint32_t)(priv->mdc_div_bits & 0x7u) << 18);
		gem_write32(priv, GEM_NET_CFG, net_cfg);
		if (gem_phy_init(priv))
			PMD_LOG(WARNING, "PHY init failed for addr 0x%02x",
				priv->phy_addr);
		else
			PMD_LOG(INFO, "PHY configured: addr=0x%02x autoneg=%u",
				priv->phy_addr, priv->autoneg);
	}

	priv->port_id = eth_dev->data->port_id;
	rte_eth_random_addr(priv->mac_addr.addr_bytes);

	eth_dev->dev_ops = gem_get_eth_dev_ops();
	eth_dev->rx_pkt_burst = gem_rx_pkt_burst;
	eth_dev->tx_pkt_burst = gem_tx_pkt_burst;
	eth_dev->data->nb_rx_queues = 1;
	eth_dev->data->nb_tx_queues = 1;
	eth_dev->data->mac_addrs = &priv->mac_addr;
	eth_dev->data->dev_link = gem_default_link();

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

	eth_dev = rte_eth_dev_allocated(rte_vdev_device_name(dev));
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
