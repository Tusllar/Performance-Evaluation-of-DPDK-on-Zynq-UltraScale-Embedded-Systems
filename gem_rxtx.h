#ifndef _GEM_RXTX_H_
#define _GEM_RXTX_H_

#include <rte_mbuf.h>
#include <rte_log.h>

/*
 * GEM_LOG — datapath debug macro, dùng gem_logtype được khai báo trong gem_core.c.
 * Mức DEBUG để không ảnh hưởng hiệu năng khi level mặc định là NOTICE.
 * Tắt log bằng: --log-level=pmd.net.gem:NOTICE khi chạy testpmd.
 */
extern int gem_logtype;

#define GEM_LOG(fmt, ...) \
	rte_log(RTE_LOG_DEBUG, gem_logtype, \
		"%s(): " fmt "\n", __func__, ##__VA_ARGS__)

uint16_t
gem_rx_pkt_burst(void *queue,
		struct rte_mbuf **bufs,
		uint16_t nb_bufs);

uint16_t
gem_tx_pkt_burst(void *queue,
		struct rte_mbuf **bufs,
		uint16_t nb_bufs);

#endif