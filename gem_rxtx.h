#ifndef _GEM_RXTX_H_
#define _GEM_RXTX_H_

#include <rte_mbuf.h>

uint16_t
gem_rx_pkt_burst(void *queue,
		struct rte_mbuf **bufs,
		uint16_t nb_bufs);

uint16_t
gem_tx_pkt_burst(void *queue,
		struct rte_mbuf **bufs,
		uint16_t nb_bufs);

#endif