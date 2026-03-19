#ifndef _GEM_ETHDEV_H_
#define _GEM_ETHDEV_H_

#include <rte_ethdev_driver.h>
#include <rte_ether.h>

/* Zynq UltraScale+ GEM (32-bit addressing mode) buffer descriptors
 *
 * RX BD (Table 34-5):
 *   word0: [31:2] addr, bit1 WRAP, bit0 OWN (0=HW can write, 1=HW done)
 *   word1: [12:0] length, bit14 SOF, bit15 EOF, other status bits
 *
 * TX BD (Table 34-8):
 *   word0: [31:0] addr
 *   word1: bit31 USED (0=HW can read/send, 1=HW done), bit30 WRAP,
 *          bit15 LAST, [13:0] length, other status bits
 */

#define GEM_DESC_NUM 512u

struct gem_rx_bd {
	uint32_t addr; /* word0 */
	uint32_t stat; /* word1 */
};

struct gem_tx_bd {
	uint32_t addr; /* word0 */
	uint32_t ctrl; /* word1 */
};

/* RX word0 */
#define GEM_RX_OWN       (1u << 0)
#define GEM_RX_WRAP      (1u << 1)
#define GEM_RX_ADDR_MASK 0xFFFFFFFCu

/* RX word1 */
#define GEM_RX_LEN_MASK  0x00001FFFu /* [12:0] */
#define GEM_RX_SOF       (1u << 14)
#define GEM_RX_EOF       (1u << 15)

/* TX word1 */
#define GEM_TX_USED      (1u << 31)
#define GEM_TX_WRAP      (1u << 30)
#define GEM_TX_LAST      (1u << 15)
#define GEM_TX_LEN_MASK  0x00003FFFu /* [13:0] */

struct gem_queue {
	struct gem_priv *priv;
	struct rte_mempool *mb_pool;

	/* RX side */
	struct gem_rx_bd *rx_ring;
	struct rte_mbuf **rx_sw_ring;
	rte_iova_t rx_ring_iova;
	uint16_t rx_head;

	/* TX side */
	struct gem_tx_bd *tx_ring;
	struct rte_mbuf **tx_sw_ring;
	rte_iova_t tx_ring_iova;
	uint16_t tx_head;
	uint16_t tx_tail;
};

struct gem_priv {
	uint16_t port_id;

	struct gem_queue rxq[RTE_MAX_QUEUES_PER_PORT];
	struct gem_queue txq[RTE_MAX_QUEUES_PER_PORT];

	struct rte_ether_addr mac_addr;

	void *base_addr;

	/* Simulation knobs */
	uint8_t sim_rx;

	/* Software statistics (used for both sim and bring-up HW mode) */
	uint64_t rx_pkts;
	uint64_t rx_bytes;
	uint64_t rx_errs;
	uint64_t rx_nombuf;
	uint64_t tx_pkts;
	uint64_t tx_bytes;
	uint64_t tx_errs;
};

int gem_dev_create(struct rte_vdev_device *dev);

#endif