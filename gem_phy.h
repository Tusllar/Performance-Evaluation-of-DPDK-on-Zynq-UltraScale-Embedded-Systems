/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef _GEM_PHY_H_
#define _GEM_PHY_H_

/* MII registers/bits */
#define MII_BMCR             0x00u
#define MII_BMSR             0x01u
#define MII_PHYIDR1          0x02u
#define MII_PHYIDR2          0x03u
#define MII_MMD_CTRL         0x0Du
#define MII_MMD_DATA         0x0Eu
#define DP83867_PHYSTS       0x11u
#define MII_BMSR_LINK_STATUS (1u << 2)
#define MII_BMCR_AUTONEG_EN  (1u << 12)
#define MII_BMCR_RESTART_AN  (1u << 9)
#define MII_BMCR_RESET       (1u << 15)

/* DP83867 vendor-specific extended registers (MMD devaddr 0x1f) */
#define DP83867_MMD_DEVADDR          0x1Fu
#define DP83867_RGMIICTL             0x0032u
#define DP83867_RGMIICTL_RGMII_EN    (1u << 7)
#define DP83867_RGMIICTL_RX_DELAY_EN (1u << 0)
#define DP83867_RGMIICTL_TX_DELAY_EN (1u << 1)
#define DP83867_RGMIIDCTL            0x0086u
#define DP83867_RGMIIDCTL_2P00NS     0x0088u

#endif
