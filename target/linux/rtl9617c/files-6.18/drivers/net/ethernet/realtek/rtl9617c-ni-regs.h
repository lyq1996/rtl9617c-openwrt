/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL9617C_NI_REGS_H
#define RTL9617C_NI_REGS_H

#include <linux/bits.h>
#include <linux/regmap.h>
#include <linux/types.h>

#define RTL_NI_INDIRECT_BUSY		BIT(31)
#define RTL_NI_INDIRECT_BUSY_LOW	BIT(0)

#define RTL_NI_REPORT			0x96d4
#define RTL_NI_PUC_DEQUEUE		0x610c

#define RTL_NI_QM_ATTRIBUTE_CMD		0x66a4
#define RTL_NI_QM_ATTRIBUTE_DATA	0x66ac
#define RTL_NI_QM_PORT_MAP_CMD		0x1654
#define RTL_NI_QM_PORT_MAP_DATA		0x1658
#define RTL_NI_QM_ILPB_CMD		0x11dc
#define RTL_NI_QM_ILPB_DATA		0x11e0
#define RTL_NI_QM_ILPB_WORDS		0x11f8
#define RTL_NI_QM_ILPB_APPLY_CMD	0x11e4
#define RTL_NI_QM_KEY_CMD		0x1b0c
#define RTL_NI_QM_KEY_WORDS		0x1b40
#define RTL_NI_QM_FIB_CMD		0x1b44
#define RTL_NI_QM_FIB_WORDS		0x1b58
#define RTL_NI_QM_EPP_READ		0x7100
#define RTL_NI_QM_EPP_WRITE		0x7000
#define RTL_NI_QM_EPP_BASE		0x7200
#define RTL_NI_QM_CPU_PUSH		0x636c
#define RTL_NI_QM_CPU_READY		0x6368
#define RTL_NI_QM_EQ_FREE(eq)		(0x6328 + (eq) * 4)
#define RTL_NI_QM_CB_CTRL		0x950c
#define RTL_NI_QM_RMU_CTRL		0x6104
#define RTL_NI_QM_ES_CTRL		0x6108

#define RTL_NI_FDB_CONTROL		0x1c00
#define RTL_NI_FDB_RESULT		0x1c2c
#define RTL_NI_FDB_DATA			0x1cb0
#define RTL_NI_FDB_CMD			0x1ca0

#define RTL_NI_GLB_STATIC		0xa01c
#define RTL_NI_PORT_TO_CPU		GENMASK(3, 0)
#define RTL_NI_MAC_ADDR0		0xa024
#define RTL_NI_MAC_ADDR1		0xa028
#define RTL_NI_PORT_STATIC		0xa5c0
#define RTL_NI_PORT_GLB			0xa5c4
#define RTL_NI_PORT_RX			0xa5c8
#define RTL_NI_PORT_TX			0xa5d4
#define RTL_NI_PORT_STRIDE		0x90
#define RTL_NI_LAN_RX_CTRL(port)	(0xa5f8 + RTL_NI_PORT_STRIDE * (port))
#define RTL_NI_PORT_TABLE		0x2304
#define RTL_NI_PORT_TABLE_ENTRY(port)	(RTL_NI_PORT_TABLE + 12 * (port))
#define RTL_NI_RX_ADDR			0xaa20
#define RTL_NI_TX_ADDR			0xaa24
#define RTL_NI_XRAM_CFG			0xaa28
#define RTL_NI_RX_READ			0xaa2c
#define RTL_NI_RX_WRITE			0xaa30
#define RTL_NI_TX_WRITE			0xaa44
#define RTL_NI_TX_READ			0xaa48
#define RTL_NI_XRAM_POINTER_MASK	GENMASK(9, 0)
#define RTL_NI_XRAM_CPU_DISABLE		(BIT(0) | BIT(9))
#define RTL_NI_XRAM_RX_PROMISC		GENMASK(15, 14)
#define RTL_NI_XRAM_OWN			BIT(31)
#define RTL_NI_XRAM_PKT_SIZE		GENMASK(13, 0)
#define RTL_NI_XRAM_PKT_VALID		BIT(19)
#define RTL_NI_XRAM_PKT_ERRORS		(GENMASK(23, 20) | BIT(25) | BIT(26))

#define RTL_NI_QM_VOQ_STATUS		0x659c
#define RTL_NI_QM_CLOCK_CTRL		0x9004
#define RTL_NI_QM_AXI_CH0		0x000
#define RTL_NI_QM_AXI_CH1		0x400
#define RTL_NI_QM_AXI_CH2		0x480
#define RTL_NI_QM_AXI_CTRL		0x00
#define RTL_NI_QM_AXI_USER		0x04
#define RTL_NI_QM_AXI_ATTR		0x08
#define RTL_NI_QM_AXI_ATTR2		0x0c
#define RTL_NI_QM_AXI_ID		0x10
#define RTL_NI_QM_EQ_CFG(eq)		(0x61e8 + (eq) * 20)
#define RTL_NI_QM_CONTROL		0x6000
#define RTL_NI_QM_PACKET_LAYOUT		0x61c8
#define RTL_NI_QM_INGRESS_CFG		0x66d4
#define RTL_NI_QM_FLUSH			0x63a8
#define RTL_NI_QM_QUEUE_ENABLE		0x63c4
#define RTL_NI_QM_STATUS		0x68f4
#define RTL_NI_QM_DROP_CFG		0x6558
#define RTL_NI_QM_EQ_QUEUE(eq)		(0x657c + (eq) * 4)
#define RTL_NI_QM_TCAM_CMD		0x2db4
#define RTL_NI_QM_TCAM_DATA		0x2db8
#define RTL_NI_NITX_BACKPRESSURE	0xa1bc
#define RTL_NI_QM_EQ_IRQ_ENABLE_LO	0x6110
#define RTL_NI_QM_EQ_IRQ_ENABLE_HI	0x6114
#define RTL_NI_QM_RESET_STATUS		0x6850
#define RTL_NI_QM_RX_ENABLE		0x1404
#define RTL_NI_QM_PORT_ATTR_A_CMD	0x1e04
#define RTL_NI_QM_PORT_ATTR_A_DATA	0x1e08
#define RTL_NI_QM_PORT_ATTR_A_DATA2	0x1e0c
#define RTL_NI_QM_PORT_ATTR_A_DATA3	0x1e10
#define RTL_NI_QM_PORT_ATTR_B_CMD	0x1e14
#define RTL_NI_QM_PORT_ATTR_B_DATA	0x1e18
#define RTL_NI_QM_PORT_ATTR_B_DATA2	0x1e1c
#define RTL_NI_QM_PORT_ATTR_B_DATA3	0x1e20
#define RTL_NI_QM_CPU_PORT_CMD		0x1420
#define RTL_NI_QM_CPU_PORT_CFG		0x1424
#define RTL_NI_QM_CPU_PORT_EN		0x1428
#define RTL_NI_NITX_THROTTLE		0xa1dc
#define RTL_NI_NITX_THROTTLE_2		0xa1ec
#define RTL_NI_QM_CPU_BUDGET(profile)	(0x9580 + (profile) * 4)
#define RTL_NI_QM_CPU_BURST(profile)	(0x9590 + (profile) * 4)
#define RTL_NI_QM_CPU_SELECT		0x95a0
#define RTL_NI_QM_CPU_CFG_CMD		0x95a8
#define RTL_NI_QM_CPU_CFG		0x95ac
#define RTL_NI_QM_VOQ_CMD		0x95b0
#define RTL_NI_QM_VOQ_CFG		0x95b4
#define RTL_NI_QM_VOQ_WEIGHT		0x95b8
#define RTL_NI_QM_VOQ_THRESHOLD		0x95bc
#define RTL_NI_QM_VOQ_PROFILE_CMD	0x95c0
#define RTL_NI_QM_VOQ_PROFILE		0x95c4
#define RTL_NI_QM_EQ_WEIGHT(profile)	(0x9560 + (profile) * 4)
#define RTL_NI_QM_EQ_PROFILE(profile)	(0x6128 + (profile) * 4)
#define RTL_NI_QM_CPU_EQ(cpu)		(0x6148 + (cpu) * 4)
#define RTL_NI_QM_VOQ_EQ(voq)		(0x65a4 + (voq) * 4)
#define RTL_NI_XGE_MAC_CFG_1		0xb204
#define RTL_NI_P6_SDS_ADDR		GENMASK(10, 0)

#define RTL_NI_NITX_PORT_CFG		0x2208
#define RTL_NI_NITX_CPU_EN		0x2210
#define RTL_NI_NITX_CPU_PORT		0x2218

#define RTL_NI_GLB_INTF_RST_CONFIG	0xa008
#define RTL_NI_XGE_BYPASS_CFG		0xa040
#define RTL_NI_XGE_BYPASS_FIELDS	GENMASK(12, 0)
#define RTL_NI_XGE_BYPASS_CPU0		(BIT(12) | (16U << 4) | BIT(3))
#define RTL_NI_L2TM_ES_CTRL		0x2300
#define RTL_NI_NITX_MISC_CFG		0xa1f4
#define RTL_NI_XGE_MAC_CFG_COM		0xb200
#define RTL_NI_XGE_MAC_CFG_TX		0xb208
#define RTL_NI_XGE_MAC_CFG_TX_2		0xb20c
#define RTL_NI_XGE_MISC_CONFIG		0xb2a8
#define RTL_NI_P6_MODE_WORD		GENMASK(16, 0)
#define RTL_NI_P6_LANE_CFG(lane)	(0x8 + (lane) * 4)

int rtl9617c_ni_indirect(struct regmap *regs, unsigned int reg, u32 value, u32 busy);

static inline unsigned int rtl_ni_read(struct regmap *regs, unsigned int reg)
{
	unsigned int value = 0;

	regmap_read(regs, reg, &value);
	return value;
}

static inline void rtl_ni_write(struct regmap *regs, unsigned int reg, unsigned int value)
{
	regmap_write(regs, reg, value);
}

#endif
