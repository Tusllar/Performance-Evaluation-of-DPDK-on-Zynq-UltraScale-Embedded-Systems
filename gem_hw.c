/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include <rte_cycles.h>

#include "gem_internal.h"

static int
gem_mdio_wait_idle(struct gem_priv *priv, uint32_t timeout_us)
{
	while (timeout_us--) {
		if (gem_read32(priv, GEM_NSR) & GEM_NSR_MDIOIDLE)
			return 0;
		rte_delay_us_block(1);
	}

	return -ETIMEDOUT;
}

static void
gem_mdio_enable(struct gem_priv *priv)
{
	uint32_t ncr = gem_read32(priv, GEM_NCR);

	if ((ncr & GEM_NCR_MAN_PORT_EN) == 0)
		gem_write32(priv, GEM_NCR, ncr | GEM_NCR_MAN_PORT_EN);
}

static int
gem_mdio_read(struct gem_priv *priv, uint8_t phy_addr, uint8_t reg, uint16_t *val)
{
	uint32_t cmd;
	uint32_t man;

	if (priv->base_addr == NULL || val == NULL)
		return -EINVAL;

	gem_mdio_enable(priv);
	if (gem_mdio_wait_idle(priv, 2000)) {
		PMD_LOG(WARNING, "MDIO busy before read (NSR=0x%08x)", gem_read32(priv, GEM_NSR));
		return -ETIMEDOUT;
	}

	cmd = GEM_MAN_SOF | GEM_MAN_RW_READ | GEM_MAN_CODE |
	      ((uint32_t)(phy_addr & 0x1Fu) << GEM_MAN_PHYA_SHIFT) |
	      ((uint32_t)(reg & 0x1Fu) << GEM_MAN_REGA_SHIFT);
	gem_write32(priv, GEM_PHYMNTNC, cmd);

	if (gem_mdio_wait_idle(priv, 2000)) {
		PMD_LOG(WARNING,
			"MDIO read timeout (phy=0x%02x reg=0x%02x NSR=0x%08x PHYMNTNC=0x%08x)",
			phy_addr, reg, gem_read32(priv, GEM_NSR),
			gem_read32(priv, GEM_PHYMNTNC));
		return -ETIMEDOUT;
	}

	man = gem_read32(priv, GEM_PHYMNTNC);
	*val = (uint16_t)(man & GEM_MAN_DATA_MASK);
	return 0;
}

static int
gem_mdio_write(struct gem_priv *priv, uint8_t phy_addr, uint8_t reg, uint16_t val)
{
	uint32_t cmd;

	if (priv->base_addr == NULL)
		return -EINVAL;

	gem_mdio_enable(priv);
	if (gem_mdio_wait_idle(priv, 2000)) {
		PMD_LOG(WARNING, "MDIO busy before write (NSR=0x%08x)", gem_read32(priv, GEM_NSR));
		return -ETIMEDOUT;
	}

	cmd = GEM_MAN_SOF | GEM_MAN_RW_WRITE | GEM_MAN_CODE |
	      ((uint32_t)(phy_addr & 0x1Fu) << GEM_MAN_PHYA_SHIFT) |
	      ((uint32_t)(reg & 0x1Fu) << GEM_MAN_REGA_SHIFT) |
	      (uint32_t)val;
	gem_write32(priv, GEM_PHYMNTNC, cmd);

	if (gem_mdio_wait_idle(priv, 2000)) {
		PMD_LOG(WARNING,
			"MDIO write timeout (phy=0x%02x reg=0x%02x val=0x%04x NSR=0x%08x PHYMNTNC=0x%08x)",
			phy_addr, reg, val, gem_read32(priv, GEM_NSR),
			gem_read32(priv, GEM_PHYMNTNC));
		return -ETIMEDOUT;
	}

	return 0;
}

static int
gem_phy_ext_read(struct gem_priv *priv, uint16_t reg, uint16_t *val)
{
	if (gem_mdio_write(priv, priv->phy_addr, MII_MMD_CTRL, DP83867_MMD_DEVADDR))
		return -EIO;
	if (gem_mdio_write(priv, priv->phy_addr, MII_MMD_DATA, reg))
		return -EIO;
	if (gem_mdio_write(priv, priv->phy_addr, MII_MMD_CTRL,
			   (uint16_t)(0x4000u | DP83867_MMD_DEVADDR)))
		return -EIO;
	if (gem_mdio_read(priv, priv->phy_addr, MII_MMD_DATA, val))
		return -EIO;

	return 0;
}

static int
gem_phy_ext_write(struct gem_priv *priv, uint16_t reg, uint16_t val)
{
	if (gem_mdio_write(priv, priv->phy_addr, MII_MMD_CTRL, DP83867_MMD_DEVADDR))
		return -EIO;
	if (gem_mdio_write(priv, priv->phy_addr, MII_MMD_DATA, reg))
		return -EIO;
	if (gem_mdio_write(priv, priv->phy_addr, MII_MMD_CTRL,
			   (uint16_t)(0x4000u | DP83867_MMD_DEVADDR)))
		return -EIO;
	if (gem_mdio_write(priv, priv->phy_addr, MII_MMD_DATA, val))
		return -EIO;

	return 0;
}

static int
gem_phy_scan_addr(struct gem_priv *priv, uint8_t phy_addr,
		  uint16_t *id1, uint16_t *id2)
{
	uint16_t v1 = 0;
	uint16_t v2 = 0;

	if (gem_mdio_read(priv, phy_addr, MII_PHYIDR1, &v1) ||
	    gem_mdio_read(priv, phy_addr, MII_PHYIDR2, &v2))
		return -EIO;

	if (v1 == 0x0000u || v1 == 0xFFFFu)
		return -ENODEV;

	if (id1 != NULL)
		*id1 = v1;
	if (id2 != NULL)
		*id2 = v2;

	return 0;
}

int
gem_phy_init(struct gem_priv *priv)
{
	uint16_t bmcr = 0;
	uint16_t bmsr = 0;
	uint16_t bmsr_latched = 0;
	uint16_t rgmii_ctl = 0;
	uint16_t phyid1 = 0, phyid2 = 0;
	uint8_t first_found_phy = 0xFFu;
	uint8_t configured_phy_ok = 0;
	uint8_t addr;
	uint32_t i;

	if (priv->base_addr == NULL)
		return 0;

	PMD_LOG(INFO, "Scanning MDIO bus for PHYs (0..31)...");
	for (addr = 0; addr < 32u; addr++) {
		if (gem_phy_scan_addr(priv, addr, &phyid1, &phyid2) == 0) {
			PMD_LOG(INFO, "FOUND PHY at addr %u: ID1=0x%04x ID2=0x%04x",
				addr, phyid1, phyid2);
			if (first_found_phy == 0xFFu)
				first_found_phy = addr;
			if (addr == priv->phy_addr)
				configured_phy_ok = 1;
		}
	}
	if (first_found_phy == 0xFFu)
		PMD_LOG(WARNING, "No PHY detected on MDIO bus (all addresses silent)");

	if (!configured_phy_ok && first_found_phy != 0xFFu) {
		PMD_LOG(WARNING, "Configured phy_addr=0x%02x not responding; fallback to detected addr=0x%02x",
			priv->phy_addr, first_found_phy);
		priv->phy_addr = first_found_phy;
	}

	if (gem_phy_scan_addr(priv, priv->phy_addr, &phyid1, &phyid2) == 0) {
		if (phyid1 != 0x2000u || ((phyid2 & 0xFFF0u) != 0xA230u)) {
			PMD_LOG(WARNING, "Unexpected PHY ID: ID1=0x%04x ID2=0x%04x (addr=0x%02x)",
				phyid1, phyid2, priv->phy_addr);
		} else {
			PMD_LOG(INFO, "Detected TI DP83867: ID1=0x%04x ID2=0x%04x", phyid1, phyid2);
		}
	} else {
		PMD_LOG(WARNING, "Failed to read PHY ID at addr 0x%02x", priv->phy_addr);
	}

	if (gem_mdio_read(priv, priv->phy_addr, MII_BMCR, &bmcr) == 0) {
		if (gem_mdio_write(priv, priv->phy_addr, MII_BMCR,
				   (uint16_t)(bmcr | MII_BMCR_RESET)) == 0) {
			PMD_LOG(INFO, "PHY soft reset asserted (BMCR=0x%04x)", (uint16_t)(bmcr | MII_BMCR_RESET));
			for (i = 0; i < 200; i++) {
				if (gem_mdio_read(priv, priv->phy_addr, MII_BMCR, &bmcr))
					break;
				if ((bmcr & MII_BMCR_RESET) == 0)
					break;
				rte_delay_ms(1);
			}
			PMD_LOG(INFO, "PHY soft reset done (BMCR=0x%04x, waited=%u ms)", bmcr, i);
		}
	}

	/*
	 * BMSR link status is latch-low on many PHYs; read twice to get
	 * current status after clearing the latch.
	 */
	if (gem_mdio_read(priv, priv->phy_addr, MII_BMSR, &bmsr_latched) ||
	    gem_mdio_read(priv, priv->phy_addr, MII_BMSR, &bmsr))
		return -EIO;
	PMD_LOG(INFO, "PHY[0x%02x] BMSR(latched)=0x%04x BMSR(cur)=0x%04x",
		priv->phy_addr, bmsr_latched, bmsr);

	if (gem_phy_ext_read(priv, DP83867_RGMIICTL, &rgmii_ctl) == 0) {
		rgmii_ctl |= DP83867_RGMIICTL_RGMII_EN |
			     DP83867_RGMIICTL_RX_DELAY_EN |
			     DP83867_RGMIICTL_TX_DELAY_EN;
		(void)gem_phy_ext_write(priv, DP83867_RGMIICTL, rgmii_ctl);
		(void)gem_phy_ext_write(priv, DP83867_RGMIIDCTL,
					DP83867_RGMIIDCTL_2P00NS);
		PMD_LOG(INFO, "DP83867 RGMII delay configured: RGMIICTL=0x%04x RGMIIDCTL=0x%04x",
			rgmii_ctl, DP83867_RGMIIDCTL_2P00NS);
	} else {
		PMD_LOG(WARNING, "DP83867 extended register access failed");
	}

	if (priv->autoneg) {
		if (gem_mdio_read(priv, priv->phy_addr, MII_BMCR, &bmcr))
			return -EIO;
		bmcr |= (MII_BMCR_AUTONEG_EN | MII_BMCR_RESTART_AN);
		if (gem_mdio_write(priv, priv->phy_addr, MII_BMCR, bmcr))
			return -EIO;

		for (i = 0; i < 300; i++) {
			if (gem_mdio_read(priv, priv->phy_addr, MII_BMSR, &bmsr_latched) ||
			    gem_mdio_read(priv, priv->phy_addr, MII_BMSR, &bmsr))
				break;
			if (bmsr & MII_BMSR_LINK_STATUS)
				break;
			rte_delay_ms(10);
		}
	}

	return 0;
}

struct rte_eth_link
gem_default_link(void)
{
	return (struct rte_eth_link){
		.link_speed = ETH_SPEED_NUM_NONE,
		.link_duplex = ETH_LINK_HALF_DUPLEX,
		.link_status = ETH_LINK_DOWN,
		.link_autoneg = ETH_LINK_AUTONEG,
	};
}

int
gem_link_update(struct rte_eth_dev *dev, int wait)
{
	struct gem_priv *priv = dev->data->dev_private;
	struct rte_eth_link link = gem_default_link();
	uint16_t bmsr = 0;
	uint16_t bmsr_latched = 0;
	uint16_t physts = 0;
	uint32_t retry = wait ? 100 : 1;
	uint32_t i;

	if (priv->base_addr == NULL) {
		dev->data->dev_link = link;
		return 0;
	}

	for (i = 0; i < retry; i++) {
		if (gem_mdio_read(priv, priv->phy_addr, MII_BMSR, &bmsr_latched) == 0 &&
		    gem_mdio_read(priv, priv->phy_addr, MII_BMSR, &bmsr) == 0 &&
		    (bmsr & MII_BMSR_LINK_STATUS))
			break;
		if (!wait)
			break;
		rte_delay_ms(10);
	}

	if (bmsr & MII_BMSR_LINK_STATUS) {
		link.link_status = ETH_LINK_UP;
		if (gem_mdio_read(priv, priv->phy_addr, DP83867_PHYSTS, &physts) == 0) {
			uint16_t speed_bits = (uint16_t)((physts >> 14) & 0x3u);
			link.link_duplex = (physts & (1u << 13)) ?
				ETH_LINK_FULL_DUPLEX : ETH_LINK_HALF_DUPLEX;
			switch (speed_bits) {
			case 0x2: link.link_speed = ETH_SPEED_NUM_1G; break;
			case 0x1: link.link_speed = ETH_SPEED_NUM_100M; break;
			default:  link.link_speed = ETH_SPEED_NUM_10M; break;
			}
		}
	}

	dev->data->dev_link = link;
	return 0;
}

uint64_t
gem_get_base_from_sysfs(void)
{
	FILE *f;
	const char *paths[] = {
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

int
gem_hw_map(struct gem_priv *priv, uint64_t base)
{
	int fd;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		return -1;

	priv->base_addr = mmap(NULL, GEM_REG_SIZE, PROT_READ | PROT_WRITE,
			       MAP_SHARED, fd, base);
	close(fd);

	if (priv->base_addr == MAP_FAILED)
		return -1;

	return 0;
}

void
gem_hw_init(struct gem_priv *priv)
{
	gem_write32(priv, GEM_NCR, 0x0);
	gem_write32(priv, GEM_NCR, GEM_NCR_CLEAR_STATS);
	gem_write32(priv, GEM_NCR, 0x0);

	gem_write32(priv, GEM_RX_STATUS, 0x0Fu);
	gem_write32(priv, GEM_TX_STATUS, 0xFFu);
	PMD_LOG(DEBUG, "TX_STATUS after init clear = 0x%08x",
		gem_read32(priv, GEM_TX_STATUS));

	gem_write32(priv, GEM_INT_DISABLE, 0x07FFFFFFu);
	gem_write32(priv, GEM_RX_Q_PTR, 0x0);
	gem_write32(priv, GEM_TX_Q_PTR, 0x0);

	rte_delay_ms(10);
	PMD_LOG(INFO, "GEM controller initialized (5-step TRM sequence done)");
}

void
gem_hw_probe(struct gem_priv *priv)
{
	uint32_t id = gem_read32(priv, GEM_ID);

	PMD_LOG(INFO, "GEM module_id (0x02FC) = 0x%08x", id);
	if (id == 0x00000000u || id == 0xFFFFFFFFu)
		PMD_LOG(WARNING, "module_id suspicious (0x%08x) — check base address!",
			id);
}
