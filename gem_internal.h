/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef _GEM_INTERNAL_H_
#define _GEM_INTERNAL_H_

#include <stdint.h>

#include <rte_byteorder.h>
#include <rte_ethdev_driver.h>
#include <rte_io.h>
#include <rte_log.h>

#include "gem_ethdev.h"
#include "gem_hw_cfg.h"
#include "gem_phy.h"
#include "gem_regs.h"

#define GEM_REG_SIZE 0x1000

extern int gem_logtype;

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

const struct eth_dev_ops *gem_get_eth_dev_ops(void);
int gem_dev_close(struct rte_eth_dev *dev);

struct rte_eth_link gem_default_link(void);
int gem_link_update(struct rte_eth_dev *dev, int wait);

uint64_t gem_get_base_from_sysfs(void);
int gem_hw_map(struct gem_priv *priv, uint64_t base);
void gem_hw_init(struct gem_priv *priv);
void gem_hw_probe(struct gem_priv *priv);
int gem_phy_init(struct gem_priv *priv);

#endif
