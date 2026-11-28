/* SPDX-License-Identifier: GPL-2.0-or-later */
/* EPON stack internal interface: register offsets, NI header encoding and the
 * per-port state shared by the control and the data netdev. */
#ifndef RTL9617C_PON_EPON_EPON_H
#define RTL9617C_PON_EPON_EPON_H
#include <linux/types.h>
#include "pon/pon.h"

struct net_device;

struct rtl_oam_port {
	struct rtl9617c_pon *pon;
	unsigned int llid;
	bool control;
	bool mac_configured;
};

#define EPON_STATIC 0x0014
#define EPON_FEC_XG 0x0024
#define EPON_MISC 0x0180
#define MIB_STATUS 0x0600
#define MIB_CTRL 0x0604
#define FEC_1G 0x0810
#define TX_1G 0x0834
#define MAX_PACKET_1G 0x0838
#define RX_1G 0x088c
#define RTT_RX 0x0f08
#define RTT_TX 0x0f0c
#define TX_XG 0x1004
#define RX_XG 0x10b4
#define RX_LOCALTIME 0x10bc
#define RX_DROP 0x1250
#define RX_MPCP_MIB 0x1288
#define TX_MPCP_MIB 0x12a0
#define PCS_TX 0x130c
#define PCS_RX 0x137c
#define LINK_DETECT 0x13cc
#define PUC_BTC 0x8050
#define PUC_REPORT 0x8054
#define EPON_FRAME_LIMIT 1700
#define GLB_IRQ 0x0000
#define GLB_IE 0x0004
#define MAC_HIGH 0x0018
#define MAC_LOW 0x001c
#define LLID_TABLE 0x0064
#define LLID_DATA 0x0164
#define LLID_ACCESS 0x0168
#define LLID_RESULT 0x016c
#define LLID_MAPPING 0x0170
#define MPCP_IRQ 0x2000
#define MPCP_IE 0x2004
#define MPCP_INIT 0x2008
#define MPCP_START RTL_MPCP_START
#define REGISTER_FRAME 0x2010
#define REGISTER_STATUS 0x2014
#define MPCP_CFG0 0x2020
#define MPCP_CFG 0x2024
#define MPCP_1G 0x2028
#define MPCP_10G 0x202c
#define LASER_TIME 0x203c
#define TIMEOUT_THRESHOLD 0x204c
#define REGISTER_TIMEOUT 0x2050
#define SYNC_TIME 0x20d0
#define OFFLINE 0x20e0
#define MPCP_EVENT_MASK (BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(6) | BIT(14))
#define OPT_EVENT_MASK (BIT(0) | BIT(1) | BIT(2) | BIT(8) | BIT(9))
#define PON_TOP_EVENTS (BIT(0) | BIT(3))
#define EPON_GLOBAL_EVENTS (BIT(0) | BIT(1) | BIT(3))


#define RTL_MPCP_START 0x200c
#define RTL_MPCP_PACKET_FLAG 0x20d8
#define RTL_OAM_PREFIX_BYTES 16

/* Largest PON payload accepted for 1536 Baby Jumbo + ETH + QinQ (<= 1700). */
#define RTL_EPON_MAX_LLIDS		32
#define RTL_EPON_MAX_FRAME_BYTES	1558
/* Above 16 LLIDs the queue field only has 4 usable slots. */
#define RTL_EPON_QUEUE_SPLIT_LLIDS	16
#define RTL_EPON_QUEUES_HIGH_LLID	4
#define RTL_EPON_QUEUES_LOW_LLID	8

/* Reference-unit live MPCP_10G value (0x10000c06). Bit 28 selects the 2.5G
 * grant configuration; bits 2:1 are part of the same reference programming.
 * The mask keeps every other field of 0x202c untouched. */
#define RTL_MPCP_10G_REFERENCE_VALUE	0x10000c06U
#define RTL_MPCP_10G_REFERENCE_MASK	(GENMASK(31, 28) | GENMASK(15, 8) | GENMASK(2, 1))

/* NI Header A layout, from the RTL8277C EPON DAL OAM transmit path
 * (GP3000 GPL dal_rt_rtl8277c_epon_oam_tx and send_static_oam_pkt_tx).
 * OAM control frames pass cos=7 (RTL_NI_HDR_OAM_COS) with ldpid=0xf, while
 * business frames pass their per-packet PUC VoQ (0..7) with ldpid=0x20+llid,
 * lspid=0x11 and fe_bypass. Field positions come from the ca-ne.ko Header A encoder:
 * [2:0] cos, [8:3] ldpid, [14:9] lspid, [28:15] length, [31:29] flags; the
 * DMA layer fills [28:15]. no_drop lives in the high word's pkt_info field,
 * whose bit position is not recoverable from the GPL sources, so the high word
 * stays zero exactly like the OEM leaves it. */
/* Header A field geometry: [2:0] cos, [8:3] ldpid, [14:9] lspid, [31:29]
 * flags. Every shift and mask is named so a call site only passes semantics. */
#define RTL_NI_HDR_COS_SHIFT	0
#define RTL_NI_HDR_LDPID_SHIFT	3
#define RTL_NI_HDR_LSPID_SHIFT	9
#define RTL_NI_HDR_COS_MASK	0x7
#define RTL_NI_HDR_LDPID_MASK	0x3f
#define RTL_NI_HDR_LSPID_MASK	0x3f
#define RTL_NI_HDR_FE_BYPASS	BIT(29)
#define RTL_NI_HDR_OAM_COS	7
#define RTL_NI_HDR_OAM_LSPID	0x11
/* Control container: LDPID 15 is what stock maps for the OAM netdev. */
#define RTL_NI_HDR_OAM_LDPID	0xf

/* pkt_info bit position is not recoverable from the GPL sources; OEM leaves
 * the high word zero, so the whole word stays a named constant. */
#define RTL_NI_HEADER_HIGH_OAM	0u

static inline u32 rtl9617c_ni_header_low(unsigned int ldpid, unsigned int queue)
{
	return (queue & RTL_NI_HDR_COS_MASK) << RTL_NI_HDR_COS_SHIFT |
	       (ldpid & RTL_NI_HDR_LDPID_MASK) << RTL_NI_HDR_LDPID_SHIFT |
	       (RTL_NI_HDR_OAM_LSPID & RTL_NI_HDR_LSPID_MASK) << RTL_NI_HDR_LSPID_SHIFT |
	       RTL_NI_HDR_FE_BYPASS;
}

/* MPCP register access shared by the protocol state machine and the holdover
 * timer; both run under pon->lock. */
static inline unsigned int rtl_mpcp_mmio_read(const struct rtl_mpcp_io *io,
					      unsigned int reg)
{
	return io->read(io->context, reg);
}

static inline void rtl_mpcp_mmio_write(const struct rtl_mpcp_io *io, unsigned int reg,
				       unsigned int value)
{
	io->write(io->context, reg, value);
}

void rtl_oam_prefix(u8 *header, unsigned int llid);
netdev_tx_t rtl_oam_transmit(struct sk_buff *skb, struct net_device *dev);
void rtl_port_address(struct net_device *dev, const u8 *base, unsigned int index);
unsigned int rtl_crypto_read(void *context, unsigned int reg);
void rtl_crypto_write(void *context, unsigned int reg, unsigned int value);
int rtl_crypto_indirect(void *context, unsigned int reg, unsigned int value);
void rtl_epon_crypto_profile(const struct rtl_dq_io *io, enum rtl_pon_mode mode);
u32 rtl_mpcp_holdover(struct rtl9617c_pon *pon, u32 registered);
void rtl_mpcp_holdover_work(struct work_struct *work);
void rtl_mpcp_refresh(struct rtl9617c_pon *pon);
void rtl_oam_netlink_receive(struct sk_buff *skb);
void rtl_oam_receive(struct rtl9617c_ni_endpoint *endpoint, struct napi_struct *napi, struct sk_buff *skb, unsigned int source);
void rtl_oam_wake(struct rtl9617c_ni_endpoint *endpoint);
void rtl_oam_update_features(struct rtl9617c_ni_endpoint *endpoint);
int rtl_oam_open(struct net_device *dev);
int rtl_oam_stop(struct net_device *dev);
extern const struct net_device_ops rtl_oam_ops;
extern const struct net_device_ops rtl_data_ops;
extern const struct attribute_group rtl_oam_group;
extern struct rtl9617c_pon __rcu *rtl_oam_netlink_pon;
int rtl_oam_open(struct net_device *dev);
int rtl_oam_stop(struct net_device *dev);

#endif
