/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef _GEM_REGS_H_
#define _GEM_REGS_H_

/* GEM register offsets — Zynq UltraScale+ TRM UG1085 Table 34-21 */
#define GEM_NCR             0x0000
#define GEM_NET_CFG         0x0004
#define GEM_NSR             0x0008
#define GEM_DMA_CFG         0x0010
#define GEM_TX_STATUS       0x0014
#define GEM_RX_Q_PTR        0x0018
#define GEM_TX_Q_PTR        0x001C
#define GEM_RX_STATUS       0x0020
#define GEM_INT_DISABLE     0x002C
#define GEM_PHYMNTNC        0x0034
#define GEM_SPEC_ADD1_BOT   0x0088
#define GEM_SPEC_ADD1_TOP   0x008C
#define GEM_ID              0x00FC

/* network_control bits */
#define GEM_NCR_RXEN         (1u << 2)
#define GEM_NCR_TXEN         (1u << 3)
#define GEM_NCR_CLEAR_STATS  (1u << 5)
#define GEM_NCR_MAN_PORT_EN  (1u << 4)
#define GEM_NSR_MDIOIDLE     (1u << 2)

/* PHY maintenance command (Clause 22) */
#define GEM_MAN_SOF          (1u << 30)
#define GEM_MAN_RW_WRITE     (1u << 28)
#define GEM_MAN_RW_READ      (2u << 28)
#define GEM_MAN_PHYA_SHIFT   23
#define GEM_MAN_REGA_SHIFT   18
#define GEM_MAN_CODE         (2u << 16)
#define GEM_MAN_DATA_MASK    0xFFFFu

#endif
