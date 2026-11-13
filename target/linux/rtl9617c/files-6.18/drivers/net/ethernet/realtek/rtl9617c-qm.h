/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL9617C_QM_H
#define RTL9617C_QM_H
#include <linux/dma-mapping.h>
#include <linux/regmap.h>
#include <linux/netdevice.h>
#include "rtl9617c-qm-layout.h"
#include "rtl9617c-ni.h"
#include "rtl9617c-qm-refill.h"
#include "rtl9617c-qm-deepq.h"

#define RTL_QM_RING_BYTES 1024
#define RTL_QM_COMMAND_BYTES 8
#define RTL_QM_VOQS 8
#define RTL_QM_BUFFERS_PER_EQ 256
#define RTL_QM_BUFFERS (2 * RTL_QM_BUFFERS_PER_EQ)
#define RTL_QM_MEMORY_BYTES (RTL_QM_RING_BYTES * RTL_QM_VOQS + RTL_QM_BUFFERS * RTL_QM_BUFFER_BYTES)
#define RTL_QM_PE_EQ 6
#define RTL_QM_PE_BUFFERS 2496

struct rtl_qm_rx {
	struct regmap *regs;
	void __iomem *ni;
	void __iomem *axi_reorder;
	void __iomem *block_reset;
	void *memory;
	dma_addr_t memory_dma;
	struct device *device;
	void *ring_memory;
	dma_addr_t ring_dma;
	void *deepq_memory;
	dma_addr_t deepq_dma;
	bool deepq_configured;
	unsigned int read[RTL_QM_VOQS];
	struct net_device __rcu *lan[7];
	struct rtl_qm_refill refill;
	struct rtl9617c_ni_endpoint __rcu *pon;
};

int rtl_qm_rx_alloc(struct device *dev, struct rtl_qm_rx *rx);
int rtl_qm_rx_start(struct rtl_qm_rx *rx, unsigned int port);

int rtl_qm_rx_add_port(struct rtl_qm_rx *rx, unsigned int port);
void rtl_qm_rx_stop(struct rtl_qm_rx *rx);
void rtl_qm_rx_free(struct device *dev, struct rtl_qm_rx *rx);
int rtl_qm_rx_poll(struct rtl_qm_rx *rx, struct napi_struct *napi, int budget);
#endif
