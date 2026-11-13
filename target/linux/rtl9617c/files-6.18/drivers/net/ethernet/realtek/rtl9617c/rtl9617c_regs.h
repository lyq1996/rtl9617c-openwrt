/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL9617C_REGS_H
#define RTL9617C_REGS_H

#include <linux/bits.h>
#include <linux/regmap.h>
#include <linux/types.h>

#define RTL_NI_INDIRECT_BUSY		BIT(31)
#define RTL_NI_INDIRECT_BUSY_LOW	BIT(0)
#define RTL_NI_INDIRECT_ACCESS		BIT(31)
#define RTL_NI_INDIRECT_WRITE		BIT(30)
#define RTL_NI_INDIRECT_TIMEOUT_US	100000

#define RTL_NI_QM_PORT_MAP_CMD		0x1654
#define RTL_NI_QM_PORT_MAP_DATA		0x1658
#define RTL_NI_QM_RX_ENABLE		0x1404
#define RTL_NI_QM_CPU_PORT_CMD		0x1420
#define RTL_NI_QM_CPU_PORT_CFG		0x1424
#define RTL_NI_QM_CPU_PORT_EN		0x1428
#define RTL_NI_QM_ILPB_CMD		0x11dc
#define RTL_NI_QM_ILPB_DATA		0x11e0
#define RTL_NI_QM_ILPB_APPLY_CMD	0x11e4
#define RTL_NI_QM_ILPB_WORDS		0x11f8
#define RTL_NI_QM_KEY_CMD		0x1b0c
#define RTL_NI_QM_KEY_WORDS		0x1b40
#define RTL_NI_QM_FIB_CMD		0x1b44
#define RTL_NI_QM_FIB_WORDS		0x1b58
#define RTL_NI_QM_PORT_ATTR_A_CMD	0x1e04
#define RTL_NI_QM_PORT_ATTR_A_DATA	0x1e08
#define RTL_NI_QM_PORT_ATTR_A_DATA2	0x1e0c
#define RTL_NI_QM_PORT_ATTR_A_DATA3	0x1e10
#define RTL_NI_QM_PORT_ATTR_B_CMD	0x1e14
#define RTL_NI_QM_PORT_ATTR_B_DATA	0x1e18
#define RTL_NI_QM_PORT_ATTR_B_DATA2	0x1e1c
#define RTL_NI_QM_PORT_ATTR_B_DATA3	0x1e20
#define RTL_NI_FDB_CONTROL		0x1c00
#define RTL_NI_FDB_RESULT		0x1c2c
#define RTL_NI_FDB_CMD			0x1ca0
#define RTL_NI_FDB_DATA			0x1cb0
#define RTL_NI_NITX_PORT_CFG		0x2208
#define RTL_NI_NITX_CPU_EN		0x2210
#define RTL_NI_NITX_CPU_PORT		0x2218
#define RTL_NI_L2TM_ES_CTRL		0x2300
#define RTL_NI_PORT_TABLE_ENTRY(n)	(0x2304 + 12 * (n))
#define RTL_NI_L2TM_TABLE_0		0x23a0
#define RTL_NI_L2TM_TABLE_1		0x23a4
#define RTL_NI_QM_TCAM_CMD		0x2db4
#define RTL_NI_QM_TCAM_DATA		0x2db8

#define RTL_NI_QM_CONTROL		0x6000
#define RTL_NI_QM_RMU_CTRL		0x6104
#define RTL_NI_QM_ES_CTRL		0x6108
#define RTL_NI_PUC_DEQUEUE		0x610c
#define RTL_NI_QM_EQ_IRQ_ENABLE_LO	0x6110
#define RTL_NI_QM_EQ_IRQ_ENABLE_HI	0x6114
#define RTL_NI_QM_EQ_PROFILE(n)		(0x6128 + (n) * 4)
#define RTL_NI_QM_CPU_EQ(n)		(0x6148 + (n) * 4)
#define RTL_NI_QM_PACKET_LAYOUT		0x61c8
#define RTL_NI_QM_EQ_CFG(n)		(0x61e8 + (n) * 20)
#define RTL_NI_QM_EQ_FREE(n)		(0x6328 + (n) * 4)
#define RTL_NI_QM_CPU_READY		0x6368
#define RTL_NI_QM_CPU_PUSH		0x636c
#define RTL_NI_QM_FLUSH			0x63a8
#define RTL_NI_QM_QUEUE_ENABLE(n)	(0x63c4 + (n) * 4)
#define RTL_NI_QM_PORT_WEIGHT(n)	(0x6444 + (n) * 8)
#define RTL_NI_QM_DROP_CFG		0x6558
#define RTL_NI_QM_EQ_QUEUE(n)		(0x657c + (n) * 4)
#define RTL_NI_QM_VOQ_STATUS		0x659c
#define RTL_NI_QM_VOQ_EQ(n)		(0x65a4 + (n) * 4)
#define RTL_NI_QM_ATTRIBUTE_CMD		0x66a4
#define RTL_NI_QM_ATTRIBUTE_DATA	0x66ac
#define RTL_NI_QM_INGRESS_CFG		0x66d4
#define RTL_NI_QM_RESET_STATUS		0x6850
#define RTL_NI_QM_BUF_CFG		0x68b0
#define RTL_NI_QM_STATUS		0x68f4
#define RTL_NI_QM_DQ_CFG		0x68f8
#define RTL_NI_QM_EPP_WRITE(n)		(0x7000 + (n) * 4)
#define RTL_NI_QM_EPP_READ(n)		(0x7100 + (n) * 4)
#define RTL_NI_QM_EPP_BASE(n)		(0x7200 + (n) * 4)
#define RTL_NI_QM_CLOCK_CTRL		0x9004
#define RTL_NI_QM_CB_CTRL		0x950c
#define RTL_NI_QM_EQ_WEIGHT(n)		(0x9560 + (n) * 4)
#define RTL_NI_QM_CPU_BUDGET(n)		(0x9580 + (n) * 4)
#define RTL_NI_QM_DQ_THRESHOLD		0x958c
#define RTL_NI_QM_CPU_BURST(n)		(0x9590 + (n) * 4)
#define RTL_NI_QM_GROUP_THRESHOLD	0x959c
#define RTL_NI_QM_CPU_SELECT		0x95a0
#define RTL_NI_QM_PORT_PROFILE		0x95a4
#define RTL_NI_QM_CPU_CFG_CMD		0x95a8
#define RTL_NI_QM_CPU_CFG		0x95ac
#define RTL_NI_QM_VOQ_CMD		0x95b0
#define RTL_NI_QM_VOQ_CFG		0x95b4
#define RTL_NI_QM_VOQ_WEIGHT		0x95b8
#define RTL_NI_QM_VOQ_THRESHOLD		0x95bc
#define RTL_NI_QM_VOQ_PROFILE_CMD	0x95c0
#define RTL_NI_QM_VOQ_PROFILE		0x95c4
#define RTL_NI_REPORT			0x96d4

#define RTL_NI_GLB_INTF_RST_CONFIG	0xa008
#define RTL_NI_GLB_STATIC		0xa01c
#define RTL_NI_GLB_PORT_TO_CPU		GENMASK(3, 0)
#define RTL_NI_MAC_ADDR0		0xa024
#define RTL_NI_MAC_ADDR1		0xa028
#define RTL_NI_XGE_BYPASS_CFG		0xa040
#define RTL_NI_XGE_BYPASS_FIELDS	GENMASK(12, 0)
#define RTL_NI_XGE_BYPASS_CPU0		(BIT(12) | (16U << 4) | BIT(3))
#define RTL_NI_RXMIB_ACCESS		0xa1a4
#define RTL_NI_TXMIB_ACCESS		0xa1b0
#define RTL_NI_MIB_DATA_HI		0x4
#define RTL_NI_MIB_DATA_LO		0x8
#define RTL_NI_NITX_BACKPRESSURE	0xa1bc
#define RTL_NI_NITX_THROTTLE		0xa1dc
#define RTL_NI_NITX_CONTROL		0xa1e0
#define RTL_NI_NITX_THROTTLE_2		0xa1ec
#define RTL_NI_NITX_MISC_CFG		0xa1f4
#define RTL_NI_PORT_STRIDE		0x90
#define RTL_NI_PORT_STATIC(n)		(0xa5c0 + RTL_NI_PORT_STRIDE * (n))
#define RTL_NI_PORT_GLB(n)		(0xa5c4 + RTL_NI_PORT_STRIDE * (n))
#define RTL_NI_PORT_RX(n)		(0xa5c8 + RTL_NI_PORT_STRIDE * (n))
#define RTL_NI_PORT_TX(n)		(0xa5d4 + RTL_NI_PORT_STRIDE * (n))
#define RTL_NI_LAN_RX_CTRL(n)		(0xa5f8 + RTL_NI_PORT_STRIDE * (n))
#define RTL_NI_RX_ADDR			0xaa20
#define RTL_NI_TX_ADDR			0xaa24
#define RTL_NI_XRAM_CFG			0xaa28
#define RTL_NI_XRAM_CPU_DISABLE		(BIT(0) | BIT(9))
#define RTL_NI_XRAM_RX_PROMISC		GENMASK(15, 14)
#define RTL_NI_XRAM_RX_END		0x300
#define RTL_NI_XRAM_TX_START		0x300
#define RTL_NI_XRAM_TX_END		0x400
#define RTL_NI_XGE_MAC_CFG_COM		0xb200
#define RTL_NI_XGE_MAC_CFG_1		0xb204
#define RTL_NI_XGE_MAC_CFG_TX		0xb208
#define RTL_NI_XGE_MAC_CFG_TX_2		0xb20c
#define RTL_NI_XGE_MISC_CONFIG		0xb2a8

#define RTL_FE_INTERFACE_CMD		0x3000
#define RTL_FE_INTERFACE_DATA		0x3004
#define RTL_FE_TABLE_CFG0		0x30a4
#define RTL_FE_TABLE_CFG1		0x30cc
#define RTL_FE_CAM_CMD			0x3200
#define RTL_FE_CAM_DATA			0x3214
#define RTL_FE_CAM_DATA_AUX		0x3218
#define RTL_FE_CAM_AUX_L4_CSUM		BIT(16)
#define RTL_FE_TPID0			0x3278
#define RTL_FE_TPID1			0x327c
#define RTL_FE_TPID2			0x3280
#define RTL_FE_CLASSIFIER_CMD		0x33b0
#define RTL_FE_CLASSIFIER_DATA		0x33cc
#define RTL_FE_LPB_CFG			0x3400
#define RTL_FE_LPB_PROFILE		0x3404
#define RTL_FE_LPB_TABLE(n, side)	(0x3414 + (n) * 12 + (side) * 4)
#define RTL_FE_PPP_PROTOCOL0		0x3500
#define RTL_FE_PPP_PROTOCOL1		0x3504
#define RTL_FE_PPP_CONFIG		0x3508
#define RTL_FE_CONTROL			0x351c
#define RTL_FE_MTU(n)			(0x3520 + ((n) / 2) * 4)
#define RTL_FE_COUNTER_CMD		0x35c8
#define RTL_FE_COUNTER_DATA		0x35e4
#define RTL_FE_HASH_MODE		0x3700
#define RTL_FE_SWO_TUPLE		0x3704
#define RTL_FE_KEY_PROFILE(n)		(0x3704 + (n) * 4)
#define RTL_FE_ACTION_GROUP_CFG		0x3724
#define RTL_FE_BUCKET_CFG		0x3834
#define RTL_FE_HASH_BASE_HIGH		0x3838
#define RTL_FE_HASH_BASE_LOW		0x383c
#define RTL_FE_ACTION_BASE_HIGH		0x3840
#define RTL_FE_ACTION_BASE_LOW		0x3844
#define RTL_FE_ACTION_SLOT_CFG		0x3848
#define RTL_FE_OVERFLOW_BASE_HIGH	0x384c
#define RTL_FE_OVERFLOW_BASE_LOW	0x3850
#define RTL_FE_DEFAULT_SLOT_CFG		0x3854
#define RTL_FE_DEFAULT_BASE_HIGH	0x3858
#define RTL_FE_DEFAULT_BASE_LOW		0x385c
#define RTL_FE_PARSER_CFG		0x3860
#define RTL_FE_HASH_CFG			0x38a0
#define RTL_FE_CACHE_DATA		0x38ac
#define RTL_FE_CACHE_CMD		0x38b0
#define RTL_FE_KEY_MASK_0		0x38b8
#define RTL_FE_KEY_MASK_1		0x38bc
#define RTL_FE_HASH_ENABLE		0x38c4
#define RTL_FE_SWO_CLEAR		0x38d8
#define RTL_FE_SWO_DATA			0x38dc
#define RTL_FE_SWO_CMD			0x38e0
#define RTL_FE_CACHE_READ_CMD		0x38e4
#define RTL_FE_CACHE_READ_DATA		0x3900
#define RTL_FE_MASK_CMD			0x3910
#define RTL_FE_MASK_DATA		0x3920
#define RTL_FE_CACHE_CFG		0x3924
#define RTL_FE_AGE_CMD			0x3928
#define RTL_FE_AGE_DATA			0x3938
#define RTL_FE_SYNC_CMD			0x393c
#define RTL_FE_MISS_TEMPLATE		0x3940
#define RTL_FE_AXI_PATCH		0x3944
#define RTL_FE_AXI_ID1_LIMIT		0x3948
#define RTL_FE_ACTION_GROUP(n)		(0x394c + (n) * 4)
#define RTL_FE_AXI_OUTSTANDING		0x3c80

#define RTL_NI_MAX_REGISTER		0xffff

static inline u32 rtl_ne_read(struct regmap *regs, unsigned int reg)
{
	unsigned int val = 0;

	regmap_read(regs, reg, &val);
	return val;
}

static inline void rtl_ne_write(struct regmap *regs, unsigned int reg, u32 val)
{
	regmap_write(regs, reg, val);
}

static inline void rtl_ne_rmw(struct regmap *regs, unsigned int reg, u32 clear,
			      u32 set)
{
	rtl_ne_write(regs, reg, (rtl_ne_read(regs, reg) & ~clear) | set);
}

static inline int rtl_ne_indirect(struct regmap *regs, unsigned int reg, u32 cmd,
				  u32 busy)
{
	unsigned int status;

	regmap_write(regs, reg, cmd);
	return regmap_read_poll_timeout(regs, reg, status, !(status & busy), 1,
					RTL_NI_INDIRECT_TIMEOUT_US);
}

static inline int rtl_ne_table_write(struct regmap *regs, unsigned int reg,
				     u32 index)
{
	return rtl_ne_indirect(regs, reg, RTL_NI_INDIRECT_ACCESS |
			       RTL_NI_INDIRECT_WRITE | index,
			       RTL_NI_INDIRECT_BUSY);
}

static inline int rtl_ne_table_read(struct regmap *regs, unsigned int reg,
				    u32 index)
{
	return rtl_ne_indirect(regs, reg, RTL_NI_INDIRECT_ACCESS | index,
			       RTL_NI_INDIRECT_BUSY);
}

#endif
