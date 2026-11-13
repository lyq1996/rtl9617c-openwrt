// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/rcupdate.h>
#include <linux/unaligned.h>
#include "rtl9617c-qm.h"
#include "rtl9617c-ni-regs.h"
#include "rtl9617c-fe-regs.h"
#include "rtl9617c-rx-checksum.h"

#define EQ_ID 4
#define EQ_CFG (RTL_NI_QM_EQ_CFG(EQ_ID))
#define EQ_PROFILE (RTL_NI_QM_EQ_PROFILE(EQ_ID))

static int rtl_qm_indirect(struct rtl_qm_rx *rx, unsigned int reg, u32 value)
{
	return rtl9617c_ni_indirect(rx->regs, reg, value, RTL_NI_INDIRECT_BUSY);
}

static int rtl_qm_attribute(struct rtl_qm_rx *rx, unsigned int entry, u32 value)
{
	rtl_ni_write(rx->regs, RTL_NI_QM_ATTRIBUTE_DATA, value);
	return rtl_qm_indirect(rx, RTL_NI_QM_ATTRIBUTE_CMD, BIT(31) | BIT(30) | entry);
}

static int rtl_qm_pon_destination_map(struct rtl_qm_rx *rx)
{
	unsigned int port, flags;
	int ret;

	for (flags = 0; flags < 256; flags += 64)
		for (port = 32; port < 64; port++) {
			rtl_ni_write(rx->regs, RTL_NI_QM_PORT_MAP_DATA, 8);
			ret = rtl_qm_indirect(rx, RTL_NI_QM_PORT_MAP_CMD, BIT(31) | BIT(30) | flags | port);
			if (ret)
				return ret;
		}
	return 0;
}

static int rtl_qm_push(struct rtl_qm_rx *rx, u32 address, unsigned int eqid)
{
	u32 status;
	int ret;

	ret = regmap_read_poll_timeout_atomic(rx->regs, RTL_NI_QM_EQ_FREE(eqid), status,
				       status & GENMASK(13, 0), 1, 10000);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout_atomic(rx->regs, RTL_NI_QM_CPU_READY, status, status & BIT(31), 1, 10000);
	if (ret)
		return ret;
	rtl_ni_write(rx->regs, RTL_NI_QM_CPU_PUSH, address | eqid);
	return 0;
}

static int rtl_qm_try_push(void *context, unsigned int address, unsigned int eqid)
{
	struct rtl_qm_rx *rx = context;

	if (!(rtl_ni_read(rx->regs, RTL_NI_QM_EQ_FREE(eqid)) & GENMASK(13, 0)) ||
	    !(rtl_ni_read(rx->regs, RTL_NI_QM_CPU_READY) & BIT(31)))
		return -EAGAIN;
	rtl_ni_write(rx->regs, RTL_NI_QM_CPU_PUSH, address | eqid);
	return 0;
}

static int rtl_qm_ingress(struct rtl_qm_rx *rx, unsigned int port)
{

	static const u32 ilpb[] = {0xc1000b80, 0xc1800deb, 0x1800a363, 0x00108003, 1};
	static const u32 key[] = {0x00007ef0, 0, 0, 0, 0, 0x02800000, 0x0001fbc0,
				 0, 0, 0, 0, 0x0a000000, 0x00000a40};
	static const u32 fib[] = {0, 0, 0x0003c000, 0x42, 0};
	unsigned int i;
	int ret;

	if (port >= 5)
		port = 6;

	for (i = 0; i < 256; i += 64) {
		rtl_ni_write(rx->regs, RTL_NI_QM_PORT_MAP_DATA, i & 64 ? 8 : port);
		ret = rtl_qm_indirect(rx, RTL_NI_QM_PORT_MAP_CMD, BIT(31) | BIT(30) | i | port);
		if (ret)
			return ret;
	}
	rtl_ni_write(rx->regs, RTL_NI_QM_ILPB_DATA, port);
	ret = rtl_qm_indirect(rx, RTL_NI_QM_ILPB_CMD, BIT(31) | BIT(30) | port);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(ilpb); i++)
		rtl_ni_write(rx->regs, RTL_NI_QM_ILPB_WORDS - 4 * i, ilpb[i]);
	ret = rtl_qm_indirect(rx, RTL_NI_QM_ILPB_APPLY_CMD, BIT(31) | BIT(30) | port);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(key); i++)
		rtl_ni_write(rx->regs, RTL_NI_QM_KEY_WORDS - 4 * i, key[i]);
	ret = rtl_qm_indirect(rx, RTL_NI_QM_KEY_CMD, BIT(31) | BIT(30));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(fib); i++)
		rtl_ni_write(rx->regs, RTL_NI_QM_FIB_WORDS - 4 * i, fib[i]);
	ret = rtl_qm_indirect(rx, RTL_NI_QM_FIB_CMD, BIT(31) | BIT(30));
	if (ret)
		return ret;

	rtl_ni_write(rx->regs, RTL_NI_QM_PORT_MAP_DATA, 9);
	ret = rtl_qm_indirect(rx, RTL_NI_QM_PORT_MAP_CMD, BIT(31) | BIT(30) | 16);
	if (ret)
		return ret;
	ret = rtl_qm_indirect(rx, RTL_NI_QM_PORT_MAP_CMD, BIT(31) | BIT(30) | 0x90);
	if (ret)
		return ret;
	ret = rtl_qm_indirect(rx, RTL_NI_QM_PORT_MAP_CMD, BIT(31) | BIT(30) | 0x50);
	if (ret)
		return ret;
	ret = rtl_qm_indirect(rx, RTL_NI_QM_PORT_MAP_CMD, BIT(31) | BIT(30) | 0xd0);
	if (ret)
		return ret;

	rtl_ni_write(rx->regs, RTL_NI_QM_PORT_ATTR_A_DATA3, ~(port << 6));
	rtl_ni_write(rx->regs, RTL_NI_QM_PORT_ATTR_A_DATA2, ~0U);
	rtl_ni_write(rx->regs, RTL_NI_QM_PORT_ATTR_A_DATA, ~0U);
	ret = rtl_qm_indirect(rx, RTL_NI_QM_PORT_ATTR_A_CMD, BIT(31) | BIT(30) | port);
	if (ret)
		return ret;
	rtl_ni_write(rx->regs, RTL_NI_QM_PORT_ATTR_A_DATA3, ~0xfc0U | (port << 6));
	rtl_ni_write(rx->regs, RTL_NI_QM_PORT_ATTR_A_DATA2, ~0U);
	rtl_ni_write(rx->regs, RTL_NI_QM_PORT_ATTR_A_DATA, ~0U);
	ret = rtl_qm_indirect(rx, RTL_NI_QM_PORT_ATTR_A_CMD, BIT(31) | BIT(30) | (512 + port));
	if (ret)
		return ret;
	rtl_ni_write(rx->regs, RTL_NI_QM_PORT_ATTR_B_DATA3, 0);
	rtl_ni_write(rx->regs, RTL_NI_QM_PORT_ATTR_B_DATA2, (3 << 5) | (16 << 8));
	rtl_ni_write(rx->regs, RTL_NI_QM_PORT_ATTR_B_DATA, BIT(6));
	ret = rtl_qm_indirect(rx, RTL_NI_QM_PORT_ATTR_B_CMD, BIT(31) | BIT(30) | port);
	if (ret)
		return ret;
	rtl_ni_write(rx->regs, RTL_NI_QM_CPU_PORT_EN, BIT(1));
	rtl_ni_write(rx->regs, RTL_NI_QM_CPU_PORT_CFG, 0);
	ret = rtl_qm_indirect(rx, RTL_NI_QM_CPU_PORT_CMD, BIT(31) | BIT(30) | 16);
	if (ret)
		return ret;

	rtl_ni_write(rx->regs, RTL_NI_NITX_THROTTLE, rtl_ni_read(rx->regs, RTL_NI_NITX_THROTTLE) & ~0xffffU);
	rtl_ni_write(rx->regs, RTL_NI_NITX_THROTTLE_2, rtl_ni_read(rx->regs, RTL_NI_NITX_THROTTLE_2) & ~0xffffU);

	rtl_ni_write(rx->regs, RTL_NI_QM_RX_ENABLE, rtl_ni_read(rx->regs, RTL_NI_QM_RX_ENABLE) | BIT(0) | BIT(1));
	return 0;
}

int rtl_qm_rx_add_port(struct rtl_qm_rx *rx, unsigned int port)
{
	if (port >= ARRAY_SIZE(rx->lan))
		return -EINVAL;
	if (!rx->deepq_configured)
		return -EAGAIN;
	return rtl_qm_ingress(rx, port);
}

int rtl_qm_rx_alloc(struct device *dev, struct rtl_qm_rx *rx)
{
	rx->device = dev;
	rx->memory = dma_alloc_noncoherent(dev, RTL_QM_MEMORY_BYTES, &rx->memory_dma,
		DMA_FROM_DEVICE, GFP_KERNEL);
	if (!rx->memory)
		return -ENOMEM;
	rx->ring_memory = dma_alloc_coherent(dev, RTL_QM_RING_BYTES * RTL_QM_VOQS,
		&rx->ring_dma, GFP_KERNEL);
	if (!rx->ring_memory)
		goto free_payload;
	rx->deepq_memory = dma_alloc_coherent(dev, RTL_DQ_MEMORY_BYTES, &rx->deepq_dma, GFP_KERNEL);
	if (!rx->deepq_memory) {
		dma_free_coherent(dev, RTL_QM_RING_BYTES * RTL_QM_VOQS, rx->ring_memory, rx->ring_dma);
		rx->ring_memory = NULL;
		goto free_payload;
	}
	return 0;
free_payload:
	dma_free_noncoherent(dev, RTL_QM_MEMORY_BYTES, rx->memory, rx->memory_dma, DMA_FROM_DEVICE);
	rx->memory = NULL;
	return -ENOMEM;
}

static unsigned int rtl_dq_read(void *context, unsigned int reg)
{
	struct rtl_qm_rx *rx = context;

	return rtl_ni_read(rx->regs, reg);
}

static void rtl_dq_write(void *context, unsigned int reg, unsigned int value)
{
	struct rtl_qm_rx *rx = context;

	rtl_ni_write(rx->regs, reg, value);
}

static int rtl_dq_indirect(void *context, unsigned int reg, unsigned int value)
{
	return rtl_qm_indirect(context, reg, value);
}

static void rtl_qm_axi_reorder(struct rtl_qm_rx *rx)
{
	void __iomem *base = rx->axi_reorder;

	writel(15, base);
	writel(BIT(31) | 12, base + RTL_NI_QM_AXI_USER);
	writel(BIT(28), base + RTL_NI_QM_AXI_ATTR);
	writel(BIT(28), base + RTL_NI_QM_AXI_ATTR2);
	writel(BIT(31) | 13, base + RTL_NI_QM_AXI_ID);
	writel(15, base + RTL_NI_QM_AXI_CH1);
	writel(BIT(31) | 12, base + RTL_NI_QM_AXI_CH1 + RTL_NI_QM_AXI_USER);
	writel(BIT(28), base + RTL_NI_QM_AXI_CH1 + RTL_NI_QM_AXI_ATTR);
	writel(BIT(28), base + RTL_NI_QM_AXI_CH1 + RTL_NI_QM_AXI_ATTR2);
	writel(BIT(31) | 13, base + RTL_NI_QM_AXI_CH1 + RTL_NI_QM_AXI_ID);
	writel(2, base + RTL_NI_QM_AXI_CH2);
	writel(BIT(31) | 8, base + RTL_NI_QM_AXI_CH2 + RTL_NI_QM_AXI_USER);
	writel(BIT(28), base + RTL_NI_QM_AXI_CH2 + RTL_NI_QM_AXI_ATTR);
	writel(BIT(28), base + RTL_NI_QM_AXI_CH2 + RTL_NI_QM_AXI_ATTR2);
	writel(BIT(31) | 9, base + RTL_NI_QM_AXI_CH2 + RTL_NI_QM_AXI_ID);
}

static int rtl_qm_cpu_credits(struct rtl_qm_rx *rx)
{
	unsigned int profile, cpu, voq;
	u32 select = rtl_ni_read(rx->regs, RTL_NI_QM_CPU_SELECT);
	int ret;

	for (profile = 0; profile < 2; profile++) {
		unsigned int count = profile ? RTL_QM_PE_BUFFERS - 4 : RTL_QM_BUFFERS_PER_EQ;

		rtl_ni_write(rx->regs, RTL_NI_QM_CPU_BUDGET(profile), (count / 8) << 16 | (count / 64));
		rtl_ni_write(rx->regs, RTL_NI_QM_CPU_BURST(profile), 0x3fff3fff);
	}
	for (cpu = 0; cpu < 4; cpu++) {
		unsigned int count0, count1, eq_profile;

		if (cpu == 1)
			continue;
		profile = cpu ? 1 : 0;
		count0 = cpu ? RTL_QM_PE_BUFFERS - 4 : RTL_QM_BUFFERS_PER_EQ;
		count1 = cpu ? 0 : RTL_QM_BUFFERS_PER_EQ;
		eq_profile = cpu ? cpu + 4 : EQ_ID;

		if (cpu) {
			unsigned int eq = RTL_QM_PE_EQ + (3 - cpu);

			rtl_ni_write(rx->regs, RTL_NI_QM_EQ_PROFILE(eq_profile), eq | (15 << 4));
			rtl_ni_write(rx->regs, RTL_NI_QM_CPU_EQ(cpu), eq_profile);
		}
		rtl_ni_write(rx->regs, RTL_NI_QM_CPU_CFG, count0 | BIT(14) | (count1 << 16) | BIT(30));
		ret = rtl_qm_indirect(rx, RTL_NI_QM_CPU_CFG_CMD, BIT(31) | BIT(30) | cpu);
		if (ret)
			return ret;
		select = (select & ~(3U << (cpu * 2))) | (profile << (cpu * 2));

		rtl_ni_write(rx->regs, RTL_NI_QM_EQ_WEIGHT(eq_profile), 12 | (12 << 16));
		for (voq = cpu * 8; voq < cpu * 8 + 8; voq++) {
			if (cpu)
				rtl_ni_write(rx->regs, RTL_NI_QM_VOQ_EQ(voq), eq_profile);

			rtl_ni_write(rx->regs, RTL_NI_QM_VOQ_PROFILE, cpu == 3 ? 4 : profile);
			ret = rtl_qm_indirect(rx, RTL_NI_QM_VOQ_PROFILE_CMD, BIT(31) | BIT(30) | voq);
			if (ret)
				return ret;
		}
	}
	for (profile = 0; profile < 3; profile++) {
		unsigned int count = profile ? RTL_QM_PE_BUFFERS - 4 : RTL_QM_BUFFERS_PER_EQ;

		rtl_ni_write(rx->regs, RTL_NI_QM_VOQ_CFG, 0);
		rtl_ni_write(rx->regs, RTL_NI_QM_VOQ_WEIGHT, (profile == 1 ? 12 : 3) << 24);
		rtl_ni_write(rx->regs, RTL_NI_QM_VOQ_THRESHOLD, ((count * 7 / 8) << 14) | (count * 6 / 8));
		ret = rtl_qm_indirect(rx, RTL_NI_QM_VOQ_CMD, BIT(31) | BIT(30) | (profile == 2 ? 4 : profile));
		if (ret)
			return ret;
	}
	rtl_ni_write(rx->regs, RTL_NI_QM_CPU_SELECT, select);
	return 0;
}

int rtl_qm_rx_start(struct rtl_qm_rx *rx, unsigned int port)
{
	struct regmap *ni = rx->regs;
	unsigned int i;
	u32 value;
	int ret;
	const struct rtl_dq_io dq = {rx, rtl_dq_read, rtl_dq_write, rtl_dq_indirect};

	rx->deepq_configured = false;
	rtl_ni_write(ni, RTL_NI_QM_CB_CTRL, rtl_ni_read(ni, RTL_NI_QM_CB_CTRL) & ~BIT(31));
	rtl_ni_write(ni, RTL_NI_QM_RMU_CTRL, rtl_ni_read(ni, RTL_NI_QM_RMU_CTRL) & ~BIT(31));
	rtl_ni_write(ni, RTL_NI_QM_ES_CTRL, rtl_ni_read(ni, RTL_NI_QM_ES_CTRL) & ~BIT(31));

	writel(readl(rx->block_reset) | BIT(5), rx->block_reset);
	udelay(1000);
	writel(readl(rx->block_reset) & ~BIT(5), rx->block_reset);
	ret = regmap_read_poll_timeout(ni, RTL_NI_QM_RESET_STATUS, value, value & BIT(30), 1, 100000);
	if (ret)
		return ret;

	memset(&rx->refill, 0, sizeof(rx->refill));
	rtl_ni_write(ni, RTL_NI_QM_RMU_CTRL, rtl_ni_read(ni, RTL_NI_QM_RMU_CTRL) & ~BIT(31));
	rtl_ni_write(ni, RTL_NI_QM_ES_CTRL, rtl_ni_read(ni, RTL_NI_QM_ES_CTRL) & ~BIT(31));
	rtl_ni_write(ni, RTL_NI_QM_EQ_IRQ_ENABLE_LO, 0);
	rtl_ni_write(ni, RTL_NI_QM_EQ_IRQ_ENABLE_HI, 0);

	for (i = 0; i < RTL_QM_VOQS; i++) {
		rx->read[i] = rtl_ni_read(ni, RTL_NI_QM_EPP_WRITE + i * 4) & (RTL_QM_RING_BYTES - 1);
		rtl_ni_write(ni, RTL_NI_QM_EPP_READ + i * 4, rx->read[i]);
	}
	rtl_ni_read(ni, RTL_NI_QM_EPP_READ);
	rtl_qm_axi_reorder(rx);
	memset(rx->ring_memory, 0, RTL_QM_RING_BYTES * RTL_QM_VOQS);
	dma_sync_single_for_device(rx->device, rx->memory_dma, RTL_QM_MEMORY_BYTES, DMA_FROM_DEVICE);

	for (i = 0; i < 16; i++) {
		rtl_ni_write(ni, RTL_NI_QM_EQ_CFG(i), 0);
		rtl_ni_write(ni, RTL_NI_QM_EQ_CFG(i) + 4, 0);
	}

	for (i = 0; i < 2; i++) {
		unsigned int cfg = EQ_CFG + i * 20;

		rtl_ni_write(ni, cfg, 1);
		rtl_ni_write(ni, cfg + 4, (RTL_QM_BUFFERS_PER_EQ << 16) | (i * RTL_QM_BUFFERS_PER_EQ));
		rtl_ni_write(ni, cfg + 8, 5 | BIT(3) | (24 << 8));
		rtl_ni_write(ni, cfg + 12, 0x61008060);
		rtl_ni_write(ni, cfg + 16, 2);
	}
	ret = rtl_qm_deepq_prepare(&dq, lower_32_bits(rx->deepq_dma));
	if (ret)
		return ret;
	rtl_ni_write(ni, EQ_PROFILE, EQ_ID | ((EQ_ID + 1) << 4));
	rtl_ni_write(ni, RTL_NI_QM_CPU_EQ(0), EQ_ID);
	rtl_ni_write(ni, RTL_NI_QM_PACKET_LAYOUT, RTL_QM_PACKET_LAYOUT);
	rtl_ni_write(ni, RTL_NI_QM_INGRESS_CFG, 0x05102013);
	rtl_ni_write(ni, RTL_NI_QM_FLUSH, 0xffff);
	rtl_ni_write(ni, RTL_NI_QM_FLUSH, 0);
	rtl_ni_write(ni, RTL_NI_QM_INGRESS_CFG, 0);

	for (i = 0; i < 16; i++) {
		ret = rtl_qm_attribute(rx, i, 0);
		if (ret)
			return ret;
	}
	ret = rtl_qm_attribute(rx, 48, 0x0a008060);
	if (ret)
		return ret;
	value = rtl_ni_read(ni, RTL_NI_QM_CONTROL);
	rtl_ni_write(ni, RTL_NI_QM_CONTROL, (value & ~(BIT(11) | BIT(13))) | 0xff);
	rtl_ni_write(ni, RTL_NI_QM_STATUS, rtl_ni_read(ni, RTL_NI_QM_STATUS) | BIT(2));
	rtl_ni_write(ni, RTL_NI_QM_DROP_CFG, 0);

	rtl_ni_write(ni, RTL_NI_QM_EQ_QUEUE(EQ_ID),
		     (rtl_ni_read(ni, RTL_NI_QM_EQ_QUEUE(EQ_ID)) & GENMASK(31, 29)) |
		     ((RTL_QM_RING_BYTES / 16) << 8) | 0xf2);
	for (i = 0; i < RTL_QM_VOQS; i++) {
		rtl_ni_write(ni, RTL_NI_QM_VOQ_EQ(i), EQ_ID);
		rtl_ni_write(ni, RTL_NI_QM_EPP_BASE + i * 4, rx->ring_dma + i * RTL_QM_RING_BYTES);
	}
	for (i = 0; i < RTL_QM_BUFFERS; i++) {
		ret = rtl_qm_push(rx, rx->memory_dma + RTL_QM_RING_BYTES * RTL_QM_VOQS +
				  i * RTL_QM_BUFFER_BYTES, EQ_ID + i / RTL_QM_BUFFERS_PER_EQ);
		if (ret)
			return ret;
	}
	ret = rtl_qm_cpu_credits(rx);
	if (ret)
		return ret;
	rtl_ni_write(ni, RTL_NI_QM_QUEUE_ENABLE, 0xff);
	rtl_ni_write(ni, RTL_NI_NITX_BACKPRESSURE, rtl_ni_read(ni, RTL_NI_NITX_BACKPRESSURE) & ~0xffU);

	for (i = 0; i < ARRAY_SIZE(rx->lan); i++)
		if (rcu_access_pointer(rx->lan[i])) {
			ret = rtl_qm_ingress(rx, i);
			if (ret)
				return ret;
		}
	ret = rtl_qm_pon_destination_map(rx);
	if (ret)
		return ret;

	for (i = 0; i < 16; i++) {
		rtl_ni_write(ni, RTL_NI_QM_TCAM_DATA, 0x8e308000);
		ret = rtl_qm_indirect(rx, RTL_NI_QM_TCAM_CMD, BIT(31) | BIT(30) | i);
		if (ret)
			return ret;
	}

	rtl_ni_write(ni, RTL_NI_L2TM_ES_CTRL, rtl_ni_read(ni, RTL_NI_L2TM_ES_CTRL) | BIT(8) | BIT(9));
	rtl_ni_write(ni, RTL_NI_PORT_TABLE_ENTRY(8), 0xff | (6 << 16));
	rtl_ni_write(ni, RTL_NI_PORT_TABLE_ENTRY(9), 0xff | (6 << 16));

	rtl_ni_write(ni, RTL_NI_QM_CB_CTRL, rtl_ni_read(ni, RTL_NI_QM_CB_CTRL) | BIT(31));

	rtl_ni_write(ni, RTL_NI_QM_CLOCK_CTRL, rtl_ni_read(ni, RTL_NI_QM_CLOCK_CTRL) | BIT(2));

	rtl_ni_write(ni, RTL_NI_QM_ES_CTRL, 0x8462ff01);
	rtl_ni_write(ni, 0x610c, rtl_ni_read(ni, 0x610c) | 0xffffU);
	rtl_ni_write(ni, RTL_NI_QM_RMU_CTRL, rtl_ni_read(ni, RTL_NI_QM_RMU_CTRL) | BIT(31));
	rx->deepq_configured = true;
	return 0;
}

void rtl_qm_rx_stop(struct rtl_qm_rx *rx)
{
	rx->deepq_configured = false;
	rtl_ni_write(rx->regs, RTL_NI_QM_CB_CTRL, rtl_ni_read(rx->regs, RTL_NI_QM_CB_CTRL) & ~BIT(31));
	rtl_ni_write(rx->regs, 0x610c, rtl_ni_read(rx->regs, 0x610c) & ~0xffffU);
	rtl_ni_write(rx->regs, RTL_NI_QM_RMU_CTRL, rtl_ni_read(rx->regs, RTL_NI_QM_RMU_CTRL) & ~BIT(31));
	rtl_ni_write(rx->regs, RTL_NI_QM_ES_CTRL, rtl_ni_read(rx->regs, RTL_NI_QM_ES_CTRL) & ~BIT(31));
}

void rtl_qm_rx_free(struct device *dev, struct rtl_qm_rx *rx)
{
	dma_free_coherent(dev, RTL_DQ_MEMORY_BYTES, rx->deepq_memory, rx->deepq_dma);
	dma_free_coherent(dev, RTL_QM_RING_BYTES * RTL_QM_VOQS, rx->ring_memory, rx->ring_dma);
	dma_free_noncoherent(dev, RTL_QM_MEMORY_BYTES, rx->memory, rx->memory_dma, DMA_FROM_DEVICE);
}

static void rtl_qm_rx_retire(struct rtl_qm_rx *rx, unsigned int cos,
	u32 command, bool refill)
{
	rx->read[cos] = (rx->read[cos] + RTL_QM_COMMAND_BYTES) & (RTL_QM_RING_BYTES - 1);
	mb();
	rtl_ni_write(rx->regs, RTL_NI_QM_EPP_READ + cos * 4, rx->read[cos]);
	if (refill) {
		u32 offset = (command & 0xffffff80) - lower_32_bits(rx->memory_dma);
		dma_sync_single_for_device(rx->device, rx->memory_dma + offset,
			RTL_QM_BUFFER_BYTES, DMA_FROM_DEVICE);
		rtl_qm_refill_add(&rx->refill, command);
	}
}

int rtl_qm_rx_poll(struct rtl_qm_rx *rx, struct napi_struct *napi, int budget)
{
	struct net_device *dev = napi->dev;
	int cos;
	int count = 0;
	bool checksum_enabled = rtl_ni_read(rx->regs, RTL_FE_CAM_DATA_AUX) & BIT(16);

	rtl_qm_refill_drain(&rx->refill, rx, rtl_qm_try_push, NAPI_POLL_WEIGHT);

	for (cos = RTL_QM_VOQS - 1; cos >= 0 && count < budget; cos--) {

		bool nonempty = rtl_ni_read(rx->regs, RTL_NI_QM_VOQ_STATUS) & BIT(cos);
		unsigned int wr = rtl_ni_read(rx->regs, RTL_NI_QM_EPP_WRITE + cos * 4) & 0xffffff;
		bool full = rx->read[cos] == wr && nonempty;

		while ((rx->read[cos] != wr || full) && count < budget) {
			unsigned int read = rx->read[cos];
			void *entry = rx->ring_memory + cos * RTL_QM_RING_BYTES + read;
			u32 command, length_word, address, offset, h0, h1;
			unsigned int length, header;
			struct sk_buff *skb;
			struct net_device *input;
			unsigned int source;
			bool refill_buffer = false;
			full = false;

			dma_rmb();
			command = get_unaligned_le32(entry);
			length_word = get_unaligned_le32(entry + 4);
			address = command & 0xffffff80;
			if (((command & 15) == EQ_ID || (command & 15) == EQ_ID + 1) &&
			    rx->refill.count[(command & 15) - EQ_ID] == RTL_QM_REFILL_SIZE)
				break;
			offset = address - lower_32_bits(rx->memory_dma);
			count++;
			if (offset < RTL_QM_RING_BYTES * RTL_QM_VOQS ||
			    offset >= RTL_QM_MEMORY_BYTES ||
			    ((command & 0xf) != EQ_ID && (command & 0xf) != EQ_ID + 1)) {
				dev->stats.rx_errors++;
				goto consume;
			}
			refill_buffer = true;

			dma_sync_single_for_cpu(rx->device, rx->memory_dma + offset,
				RTL_QM_BUFFER_BYTES, DMA_FROM_DEVICE);
			h0 = get_unaligned_be32(rx->memory + offset + RTL_QM_HEADROOM);
			h1 = get_unaligned_be32(rx->memory + offset + RTL_QM_HEADROOM + 4);
			header = 8 + ((h0 & BIT(31)) ? 8 : 0);
			length = (h1 >> 15) & 0x3fff;
			if (h0 & BIT(31))
				length -= 8;
			if ((command & (BIT(4) | BIT(6))) != (BIT(4) | BIT(6)) ||
			    length < ETH_HLEN || length > RTL_QM_DATA_BYTES - header) {
				dev->stats.rx_errors++;
				goto consume;
			}
			skb = napi_alloc_skb(napi, length);
			if (!skb) {
				dev->stats.rx_dropped++;
				goto consume;
			}
			memcpy(skb_put(skb, length), rx->memory + offset + RTL_QM_HEADROOM + header, length);
			if (checksum_enabled && rtl_rx_checksum_valid(command, skb->data, length))
				skb->ip_summed = CHECKSUM_UNNECESSARY;
			source = FIELD_GET(GENMASK(14, 9), h1);

			rtl_qm_rx_retire(rx, cos, command, refill_buffer);
			if (source >= ARRAY_SIZE(rx->lan)) {
				struct rtl9617c_ni_endpoint *endpoint;

				rcu_read_lock();
				endpoint = rcu_dereference(rx->pon);
				if (endpoint && (source == 7 || source >= 32))
					endpoint->receive(endpoint, napi, skb, source |
						(FIELD_GET(GENMASK(26, 18), h0) << 8));
				else {
					dev->stats.rx_dropped++;
					dev_kfree_skb_any(skb);
				}
				rcu_read_unlock();
				continue;
			}
			rcu_read_lock();
			input = rcu_dereference(rx->lan[source]);
			if (!input || !netif_running(input)) {
				dev->stats.rx_dropped++;
				dev_kfree_skb_any(skb);
				rcu_read_unlock();
				continue;
			}
			skb->protocol = eth_type_trans(skb, input);
			input->stats.rx_packets++;
			input->stats.rx_bytes += length;
			napi_gro_receive(napi, skb);
			rcu_read_unlock();
			continue;
consume:
			rtl_qm_rx_retire(rx, cos, command, refill_buffer);
		}
	}

	rtl_qm_refill_drain(&rx->refill, rx, rtl_qm_try_push, NAPI_POLL_WEIGHT);
	return count;
}
