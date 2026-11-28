// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/io.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/kstrtox.h>
#include <linux/ppp_defs.h>
#include <linux/rcupdate.h>
#include <linux/rhashtable.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/uaccess.h>
#include <net/arp.h>
#include <net/flow_offload.h>
#include <net/ip_tunnels.h>
#include <net/ndisc.h>
#include <net/pkt_cls.h>

#include "../ni/ni_te_flow.h"
#include "fe_priv.h"
#include "fe_mcast.h"
#include "fe_napt6.h"
#include "fe_tunnel.h"
#include "rtl9617c_itu.h"

#define FE_CONTROL_ENABLE		BIT(10)
#define FE_CONTROL_FIELDS		(GENMASK(9, 4) | GENMASK(21, 16) | GENMASK(26, 24))
#define FE_CONTROL_INIT			(BIT(31) | BIT(12) | (32U << 4) | (16U << 16))
#define FE_CLASSIFIER_BASE		1024
#define FE_MAP_BASE			24
#define FE_MAP_SIDES			4
#define FE_LPB_SIDES			3
#define FE_MTU_MASK			GENMASK(13, 0)
#define FE_INTERFACE_INIT		(BIT(23) | BIT(18))
#define FE_INTERFACE_SMAC_SHIFT		19
#define FE_SMAC_VALID			BIT(16)
#define FE_FDB_FOUND			5
#define FE_FDB_LOOKUP			4
#define FE_FDB_ADD			0x45
#define FE_FDB_DELETE			0x85
#define FE_FDB_STATIC			(BIT(9) | BIT(19) | BIT(20) | BIT(21))
#define FE_FDB_INDEX			GENMASK(16, 4)
#define FE_REORDER_CTRL			0x80
#define FE_REORDER_USER			0x84
#define FE_REORDER_ATTR			0x88
#define FE_REORDER_ATTR2		0x8c
#define FE_REORDER_ID			0x90
#define FE_PPPOE_HLEN			8

static const unsigned int rtl_fe_saved_regs[] = {
	RTL_FE_BUCKET_CFG, RTL_FE_HASH_BASE_HIGH, RTL_FE_HASH_BASE_LOW,
	RTL_FE_ACTION_BASE_HIGH, RTL_FE_ACTION_BASE_LOW, RTL_FE_ACTION_SLOT_CFG,
	RTL_FE_OVERFLOW_BASE_HIGH, RTL_FE_OVERFLOW_BASE_LOW,
	RTL_FE_DEFAULT_SLOT_CFG, RTL_FE_DEFAULT_BASE_HIGH, RTL_FE_DEFAULT_BASE_LOW,
	RTL_FE_HASH_CFG, RTL_FE_KEY_MASK_0, RTL_FE_KEY_MASK_1, RTL_FE_HASH_ENABLE,
	RTL_FE_CACHE_CFG, RTL_FE_ACTION_GROUP_CFG,
	RTL_FE_LPB_CFG, RTL_FE_LPB_PROFILE,
	RTL_FE_LPB_TABLE(0, 0), RTL_FE_LPB_TABLE(0, 1), RTL_FE_LPB_TABLE(0, 2),
	RTL_FE_LPB_TABLE(1, 0), RTL_FE_LPB_TABLE(1, 1), RTL_FE_LPB_TABLE(1, 2),
	RTL_FE_HASH_MODE, RTL_FE_SWO_TUPLE,
	RTL_FE_KEY_PROFILE(1), RTL_FE_KEY_PROFILE(2), RTL_FE_KEY_PROFILE(3),
	RTL_FE_KEY_PROFILE(4), RTL_FE_KEY_PROFILE(5), RTL_FE_KEY_PROFILE(6),
	RTL_FE_KEY_PROFILE(7), RTL_FE_PARSER_CFG,
	RTL_FE_ACTION_GROUP(0), RTL_FE_ACTION_GROUP(1), RTL_FE_ACTION_GROUP(2),
	RTL_FE_ACTION_GROUP(3), RTL_FE_ACTION_GROUP(4),
	RTL_L2TM_ES_CTRL, RTL_L2TM_ES_SCH_CFG0(13), RTL_L2TM_ES_VOQ3_0_W(13),
	RTL_NI_GLB_NIRX_L3FE_DEMUX_CFG0,
	RTL_FE_TPID0, RTL_FE_TPID1, RTL_FE_TPID2,
	RTL_FE_PPP_PROTOCOL0, RTL_FE_PPP_PROTOCOL1, RTL_FE_CONTROL,
	RTL_FE_MISS_TEMPLATE, RTL_FE_AXI_PATCH, RTL_FE_AXI_ID1_LIMIT,
	RTL_FE_AXI_OUTSTANDING,
	RTL_FE_PPP_CONFIG, RTL_FE_TABLE_CFG1, RTL_FE_TABLE_CFG0, RTL_FE_CAM_DATA_AUX,
};

static const unsigned int rtl_fe_bridge_masks[] = {
	RTL_FE_MASK_BRIDGE, RTL_FE_MASK_LEARN, RTL_FE_MASK_LEARN_PPPOE,
};

static const struct rhashtable_params rtl_fe_flow_params = {
	.head_offset = offsetof(struct rtl_fe_flow, node),
	.key_offset = offsetof(struct rtl_fe_flow, cookie),
	.key_len = sizeof(unsigned long),
	.automatic_shrinking = true,
};

struct rtl_fe_port *rtl_fe_port_find(struct rtl_fe *fe, struct net_device *netdev)
{
	struct rtl_fe_port *port;

	list_for_each_entry(port, &fe->ports, list)
		if (!port->retired && port->netdev == netdev)
			return port;
	return NULL;
}

static struct rtl_fe_flow *rtl_fe_flow_find(struct rtl_fe *fe, unsigned long cookie)
{
	return rhashtable_lookup_fast(&fe->cookies, &cookie, rtl_fe_flow_params);
}

static void rtl_fe_collect_ports(struct rtl_fe *fe)
{
	struct rtl_fe_port *port, *next;
	struct rtl_fe_flow *flow;

	list_for_each_entry_safe(port, next, &fe->ports, list) {
		bool owned = false;

		if (!port->retired || port->bindings)
			continue;
		list_for_each_entry(flow, &fe->flows, list) {
			if (flow->input == port || flow->output == port) {
				owned = true;
				break;
			}
		}
		if (!owned) {
			list_del(&port->list);
			kfree(port);
		}
	}
}

static int rtl_fe_map_read(struct rtl_fe *fe, unsigned int index, u32 *val)
{
	int ret;

	ret = rtl_fe_indirect(fe, RTL_L2FE_ARB_PDPID_MAP_TBL_ACCESS,
			      RTL_NI_INDIRECT_ACCESS | index);
	if (!ret)
		*val = rtl_fe_read(fe, RTL_L2FE_ARB_PDPID_MAP_TBL_DATA);
	return ret;
}

static int rtl_fe_map_write(struct rtl_fe *fe, unsigned int index, u32 val)
{
	rtl_fe_write(fe, RTL_L2FE_ARB_PDPID_MAP_TBL_DATA, val);
	return rtl_fe_indirect(fe, RTL_L2FE_ARB_PDPID_MAP_TBL_ACCESS,
			       RTL_NI_INDIRECT_ACCESS |
			       RTL_NI_INDIRECT_WRITE | index);
}

static void rtl_fe_save_dual_hdr(struct rtl_fe *fe)
{
	unsigned int side;

	fe->dual_dos_saved = rtl_fe_read(fe, RTL_FE_PP_DUAL_DOS_EN);
	fe->l2tp_sport_saved = rtl_fe_read(fe, RTL_FE_L3PP_L2TP_SPORT);
	for (side = 0; side < FE_LPB_SIDES; side++)
		fe->lpb3_saved[side] =
			rtl_fe_read(fe, RTL_FE_LPB_TABLE(RTL_FE_LPB_CLS_INDEX,
							 side));
}

static int rtl_fe_save(struct rtl_fe *fe)
{
	unsigned int i, side;
	int ret;

	for (i = 0; i < ARRAY_SIZE(rtl_fe_saved_regs); i++)
		fe->saved[i] = rtl_fe_read(fe, rtl_fe_saved_regs[i]);
	for (i = 0; i < FE_LAN_PORTS; i++)
		fe->lan_saved[i] = rtl_fe_read(fe, RTL_NI_PORT_RX_CTRL(i));
	fe->xge_saved = rtl_fe_read(fe, RTL_NI_XGE_BYPASS_CFG);

	ret = rtl_fe_mask_read(fe, RTL_FE_MASK_L3, fe->mask_saved);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(rtl_fe_bridge_masks); i++) {
		ret = rtl_fe_mask_read(fe, rtl_fe_bridge_masks[i],
				       fe->bridge_mask_saved[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < FE_LPB_ENTRIES; i++) {
		ret = rtl_fe_classifier_read(fe, FE_CLASSIFIER_BASE + 4 * i,
					     fe->classifier_saved[i]);
		if (ret)
			return ret;
		for (side = 0; side < FE_MAP_SIDES; side++) {
			ret = rtl_fe_map_read(fe, (FE_MAP_BASE + i) | (side << 6),
					      &fe->map_saved[i][side]);
			if (ret)
				return ret;
		}
	}
	if (fe->dual_hdr_active)
		rtl_fe_save_dual_hdr(fe);
	fe->prepared = true;
	return 0;
}

static int rtl_fe_setup_masks(struct rtl_fe *fe)
{
	u32 mask[RTL_FE_MASK_WORDS];
	int ret;

	rtl_fe_mask_l3(mask);
	mask[6] |= BIT(23) | BIT(26);
	mask[0] &= ~(BIT(1) | BIT(2) | BIT(3));
	mask[3] &= ~BIT(4);
	mask[2] &= ~(BIT(30) | BIT(31));
	mask[3] &= ~(BIT(2) | BIT(3));
	mask[2] &= ~(BIT(25) | BIT(27));
	ret = rtl_fe_mask_write(fe, RTL_FE_MASK_L3, mask);
	if (ret)
		return ret;

	rtl_fe_mask_bridge(mask);
	ret = rtl_fe_mask_write(fe, RTL_FE_MASK_BRIDGE, mask);
	if (ret)
		return ret;

	rtl_fe_mask_bridge(mask);
	rtl_fe_bits_set(mask, 92, 2, 0);
	rtl_fe_bits_set(mask, 96, 2, 0);
	ret = rtl_fe_mask_write(fe, RTL_FE_MASK_LEARN, mask);
	if (ret)
		return ret;

	rtl_fe_bits_set(mask, 135, 8, 255);
	rtl_fe_bits_set(mask, 88, 1, 0);
	return rtl_fe_mask_write(fe, RTL_FE_MASK_LEARN_PPPOE, mask);
}

static void rtl_fe_write_l2tp_sport(struct rtl_fe *fe)
{
	rtl_fe_write(fe, RTL_FE_L3PP_L2TP_SPORT,
		     FIELD_PREP(RTL_FE_L3PP_L2TP_SPORT0_MASK,
				RTL_FE_L3PP_L2TP_SPORT_DFT) |
		     FIELD_PREP(RTL_FE_L3PP_L2TP_SPORT1_MASK,
				fe->l2tp_alt_sport ?:
				RTL_FE_L3PP_L2TP_SPORT_DFT));
}

static void rtl_fe_program_dual_hdr(struct rtl_fe *fe)
{
	u32 lpb[RTL_FE_LPB_WORDS];
	unsigned int side;

	rtl_fe_write(fe, RTL_FE_PP_DUAL_DOS_EN, RTL_FE_PP_DUAL_DOS_EN_ALL);
	/* Direct fallback LPB_TBL_3 to CLS profile 1 so Ethernet WAN traffic
	 * hitting the default LPB slot preserves lspid and evaluates WAN CLS.
	 */
	rtl_fe_ingress_lpb(lpb, 0, RTL_FE_CLS_MCGID_L3FE);
	rtl_fe_bits_set(lpb, RTL_FE_LPB_CLS_PROFILE_BIT,
			RTL_FE_LPB_CLS_PROFILE_WIDTH, 1);
	for (side = 0; side < FE_LPB_SIDES; side++)
		rtl_fe_write(fe, RTL_FE_LPB_TABLE(RTL_FE_LPB_CLS_INDEX, side),
			     lpb[side]);
	rtl_fe_write_l2tp_sport(fe);
}

static int rtl_fe_setup_lpb(struct rtl_fe *fe)
{
	u32 cls[RTL_FE_CLASSIFIER_WORDS], lpb[RTL_FE_LPB_WORDS];
	unsigned int i, side, source;
	int ret;

	for (i = 0; i < FE_LPB_ENTRIES; i++) {
		source = i ? FE_SOURCE_PON : 0;

		rtl_fe_classify_hash(cls, 0, source);
		if (!i)
			rtl_fe_bits_set(cls, 208, 1, 0);
		ret = rtl_fe_classifier_write(fe, FE_CLASSIFIER_BASE + 4 * !i, cls);
		if (ret)
			return ret;

		rtl_fe_ingress_lpb(lpb, source, RTL_FE_CLS_MCGID_L3FE);
		rtl_fe_bits_set(lpb, RTL_FE_LPB_CLS_PROFILE_BIT,
				RTL_FE_LPB_CLS_PROFILE_WIDTH, !i);
		for (side = 0; side < FE_LPB_SIDES; side++)
			rtl_fe_write(fe, RTL_FE_LPB_TABLE(i, side), lpb[side]);

		for (side = 0; side < FE_MAP_SIDES; side++) {
			ret = rtl_fe_map_write(fe, (i ? 24 : 25) | (side << 6),
					       i ? 12 : 13);
			if (ret)
				return ret;
		}
	}

	if (fe->dual_hdr_active)
		rtl_fe_program_dual_hdr(fe);

	rtl_fe_rmw(fe, RTL_FE_LPB_CFG, BIT(0) | BIT(10), BIT(3) | BIT(2) | BIT(19));
	rtl_fe_write(fe, RTL_FE_LPB_PROFILE, 7 | (25 << 8) | (1 << 14) | (24 << 16) |
		     (2 << 22) | (3 << 24));
	return 0;
}

static int rtl_fe_prepare(struct rtl_fe *fe)
{
	static const u16 protocols[FE_CAM_PROTOCOLS] = {
		ETH_P_PPP_SES, PPP_IP, PPP_IPV6,
	};
	unsigned int i;
	int ret;

	fe->napt6_active = rtl_fe_napt6_mode_enabled();
	fe->dual_hdr_active = rtl_fe_tunnel_flow_enabled();
	ret = rtl_fe_save(fe);
	if (ret) {
		fe->napt6_active = false;
		fe->dual_hdr_active = false;
		return ret;
	}

	rtl_fe_rmw(fe, RTL_FE_CAM_DATA_AUX, 0, RTL_FE_CAM_AUX_L4_CSUM);
	rtl_fe_rmw(fe, RTL_FE_CONTROL, FE_CONTROL_ENABLE, 0);
	rtl_fe_dma_tables(fe, lower_32_bits(fe->dma),
			  (upper_32_bits(fe->dma) & 0xff) | 2);
	ret = rtl_fe_indirect(fe, RTL_FE_SYNC_CMD, 1);
	if (ret)
		return ret;

	ret = rtl_fe_setup_masks(fe);
	if (ret)
		return ret;

	rtl_fe_write(fe, RTL_FE_PPP_PROTOCOL0, 0x11);
	rtl_fe_write(fe, RTL_FE_PPP_PROTOCOL1, PPP_IP | PPP_IPV6 << 16);
	rtl_fe_rmw(fe, RTL_FE_TABLE_CFG0, GENMASK(17, 12),
		   BIT(31) | (RTL_FE_DUAL_HASH_EIF_BASE << 12));
	rtl_fe_rmw(fe, RTL_FE_CONTROL, FE_CONTROL_FIELDS, FE_CONTROL_INIT);
	rtl_fe_rmw(fe, RTL_FE_PPP_CONFIG, 0, 0x1f00);
	rtl_fe_rmw(fe, RTL_FE_TABLE_CFG1, GENMASK(11, 0), 0xf21);

	ret = rtl_fe_setup_lpb(fe);
	if (ret)
		return ret;

	rtl_fe_rmw(fe, RTL_L2TM_ES_CTRL, 0, BIT(12) | BIT(13));
	rtl_fe_write(fe, RTL_L2TM_ES_SCH_CFG0(12),
		     FIELD_PREP(RTL_L2TM_ES_SCH_VOQ_EN, 0xff) |
		     FIELD_PREP(RTL_L2TM_ES_SCH_SPEED, 6));
	rtl_fe_write(fe, RTL_L2TM_ES_SCH_CFG0(13),
		     FIELD_PREP(RTL_L2TM_ES_SCH_VOQ_EN, 0xff) |
		     FIELD_PREP(RTL_L2TM_ES_SCH_SPEED, 6));
	for (i = 0; i < 5; i++)
		rtl_fe_write(fe, RTL_FE_ACTION_GROUP(i), 0);
	rtl_fe_write(fe, RTL_FE_ACTION_GROUP_CFG,
		     fe->napt6_active ?
		     (RTL_FE_ACTION_GROUPS_NAPT6 |
		      (RTL_FE_ACTION_WIDTH_384 << RTL_FE_ACTION_WIDTH_SHIFT)) :
		     (RTL_FE_ACTION_GROUPS |
		      (RTL_FE_ACTION_WIDTH_256 << RTL_FE_ACTION_WIDTH_SHIFT)));
	rtl_fe_write(fe, RTL_FE_SWO_TUPLE, RTL_FE_MASK_L3);
	rtl_fe_write(fe, RTL_FE_KEY_PROFILE(1), RTL_FE_MASK_BRIDGE | (1 << 8));
	rtl_fe_write(fe, RTL_FE_KEY_PROFILE(2), RTL_FE_MASK_LEARN | (2 << 8));
	rtl_fe_write(fe, RTL_FE_KEY_PROFILE(3), RTL_FE_MASK_LEARN_PPPOE | (3 << 8));
	rtl_fe_write(fe, RTL_FE_HASH_MODE, 4);
	rtl_fe_write(fe, RTL_FE_PARSER_CFG, 0x020d0000);
	rtl_fe_write(fe, RTL_FE_TPID0, 0x88a88100);
	rtl_fe_write(fe, RTL_FE_TPID1, 0x92009100);
	rtl_fe_write(fe, RTL_FE_TPID2, 0x77);

	for (i = 0; i < FE_CAM_PROTOCOLS; i++) {
		ret = rtl_fe_cam_protocol_get(fe, &fe->protocol_cam[i], i ? 8 : 2,
					      protocols[i]);
		if (ret < 0)
			return ret;
		if (i)
			fe->ppp_protocol_ids[i - 1] = ret;
	}

	rtl_fe_rmw(fe, RTL_NI_GLB_NIRX_L3FE_DEMUX_CFG0, 0, GENMASK(13, 0));
	ret = rtl_fe_mcast_prepare(fe);
	if (ret)
		return ret;
	rtl_fe_cache_start(fe);
	rtl_fe_rmw(fe, RTL_FE_CONTROL, 0, FE_CONTROL_ENABLE);
	return 0;
}

static int rtl_fe_restore(struct rtl_fe *fe)
{
	unsigned int i, side;
	int ret;

	if (!fe->prepared)
		return 0;

	rtl_fe_rmw(fe, RTL_FE_CONTROL, FE_CONTROL_ENABLE, 0);
	rtl_fe_write(fe, RTL_FE_HASH_ENABLE, 0);
	ret = rtl_fe_indirect(fe, RTL_FE_SYNC_CMD, 1);
	if (ret)
		return ret;

	for (i = 0; i < FE_CAM_PROTOCOLS; i++) {
		ret = rtl_fe_cam_protocol_put(fe, &fe->protocol_cam[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < FE_LPB_ENTRIES; i++) {
		for (side = 0; side < FE_MAP_SIDES; side++) {
			ret = rtl_fe_map_write(fe, (FE_MAP_BASE + i) | (side << 6),
					       fe->map_saved[i][side]);
			if (ret)
				return ret;
		}
		ret = rtl_fe_classifier_write(fe, FE_CLASSIFIER_BASE + 4 * i,
					      fe->classifier_saved[i]);
		if (ret)
			return ret;
	}

	ret = rtl_fe_mask_write(fe, RTL_FE_MASK_L3, fe->mask_saved);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(rtl_fe_bridge_masks); i++) {
		ret = rtl_fe_mask_write(fe, rtl_fe_bridge_masks[i],
					fe->bridge_mask_saved[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < ARRAY_SIZE(rtl_fe_saved_regs); i++)
		rtl_fe_write(fe, rtl_fe_saved_regs[i], fe->saved[i]);
	if (fe->dual_hdr_active) {
		for (side = 0; side < FE_LPB_SIDES; side++)
			rtl_fe_write(fe,
				     RTL_FE_LPB_TABLE(RTL_FE_LPB_CLS_INDEX,
						      side),
				     fe->lpb3_saved[side]);
		rtl_fe_write(fe, RTL_FE_PP_DUAL_DOS_EN, fe->dual_dos_saved);
		rtl_fe_write(fe, RTL_FE_L3PP_L2TP_SPORT, fe->l2tp_sport_saved);
		fe->l2tp_alt_sport = 0;
		fe->l2tp_alt_refs = 0;
		fe->dual_hdr_active = false;
	}
	for (i = 0; i < FE_LAN_PORTS; i++)
		rtl_fe_write(fe, RTL_NI_PORT_RX_CTRL(i), fe->lan_saved[i]);
	rtl_fe_write(fe, RTL_NI_XGE_BYPASS_CFG, fe->xge_saved);
	rtl_fe_read(fe, RTL_FE_HASH_BASE_LOW);
	fe->prepared = false;
	fe->napt6_active = false;
	return 0;
}

static int rtl_fe_smac_get(struct rtl_fe *fe, const u8 *address,
			   unsigned int *index)
{
	u32 words[RTL_FE_SMAC_WORDS] = { };
	unsigned int i, byte;
	int ret;

	for (i = 0; i < FE_SOURCE_MACS; i++) {
		struct rtl_fe_mac *mac = &fe->source_macs[i];

		if (mac->users && ether_addr_equal(address, mac->address)) {
			mac->users++;
			*index = i;
			return 0;
		}
	}

	for (i = 0; i < FE_SOURCE_MACS; i++)
		if (!fe->source_macs[i].users)
			break;
	if (i == FE_SOURCE_MACS)
		return -ENOSPC;

	ret = rtl_fe_smac_read(fe, i, fe->source_macs[i].saved);
	if (ret)
		return ret;
	for (byte = 0; byte < ETH_ALEN; byte++)
		rtl_fe_bits_set(words, (ETH_ALEN - 1 - byte) * 8, 8, address[byte]);
	words[1] |= FE_SMAC_VALID;
	ret = rtl_fe_smac_write(fe, i, words);
	if (ret)
		return ret;

	ether_addr_copy(fe->source_macs[i].address, address);
	fe->source_macs[i].users = 1;
	*index = i;
	return 0;
}

static int rtl_fe_smac_put(struct rtl_fe *fe, unsigned int index)
{
	struct rtl_fe_mac *mac = &fe->source_macs[index];
	int ret;

	if (mac->users > 1) {
		mac->users--;
		return 0;
	}
	ret = rtl_fe_smac_write(fe, index, mac->saved);
	if (!ret)
		mac->users = 0;
	return ret;
}

static u32 rtl_fe_interface_value(const struct rtl_fe_flow *flow)
{
	return FE_INTERFACE_INIT |
	       ((flow->source_mac + 1) << FE_INTERFACE_SMAC_SHIFT) |
	       flow->pppoe;
}

static int rtl_fe_interface_bind_slot(struct rtl_fe *fe,
				      struct rtl_fe_flow *flow,
				      unsigned int index)
{
	struct rtl_fe_interface *intf;
	int ret;

	if (index >= FE_INTERFACES)
		return -EINVAL;

	intf = &fe->interfaces[index];
	if (intf->users && (intf->source_mac != flow->source_mac ||
			    intf->pppoe != flow->pppoe))
		return -EBUSY;

	if (!intf->users) {
		ret = rtl_fe_indirect(fe, RTL_FE_INTERFACE_CMD,
				      RTL_NI_INDIRECT_ACCESS | index);
		if (ret)
			return ret;
		intf->saved = rtl_fe_read(fe, RTL_FE_INTERFACE_DATA);
		intf->source_mac = flow->source_mac;
		intf->pppoe = flow->pppoe;
		__set_bit(index, fe->counters);
	}

	flow->counter = index;
	flow->counted = index < FE_COUNTERS && !intf->counted;
	intf->counted |= flow->counted;
	intf->users++;
	flow->interface_owned = true;
	if (intf->users != 1)
		return 0;

	rtl_fe_write(fe, RTL_FE_INTERFACE_DATA, rtl_fe_interface_value(flow));
	return rtl_fe_indirect(fe, RTL_FE_INTERFACE_CMD, RTL_NI_INDIRECT_ACCESS |
			       RTL_NI_INDIRECT_WRITE | index);
}

static int rtl_fe_interface_get(struct rtl_fe *fe, struct rtl_fe_flow *flow)
{
	unsigned int limit = (rtl_fe_vpn_offload_enabled() ||
			      rtl_fe_tunnel_flow_enabled()) ?
			     FE_INTERFACES - FE_TUNNEL_EIF_RESERVE :
			     FE_INTERFACES;
	unsigned int index = FE_INTERFACES;
	struct rtl_fe_interface *intf;
	unsigned int i;

	/*
	 * Prefer exclusive rows 0..15, skip rows held by tunnel slots, and
	 * reserve the top FE_TUNNEL_EIF_RESERVE aliasable rows when tunnel
	 * offload is active so ordinary counted flows cannot starve dual-header
	 * sessions out of physical EIF rows 16..31.
	 */
	for (i = 0; i < limit; i++) {
		if (!test_bit(i, fe->counters) &&
		    !rtl_fe_tunnel_phys_busy(fe, i)) {
			index = i;
			break;
		}
	}

	if (index == FE_INTERFACES) {
		for (i = 0; i < limit; i++) {
			intf = &fe->interfaces[i];
			if (intf->source_mac == flow->source_mac &&
			    intf->pppoe == flow->pppoe &&
			    !rtl_fe_tunnel_phys_busy(fe, i) &&
			    (!intf->counted || index == FE_INTERFACES)) {
				index = i;
				if (!intf->counted)
					break;
			}
		}
		if (index == FE_INTERFACES)
			return -ENOSPC;
	}

	return rtl_fe_interface_bind_slot(fe, flow, index);
}

static int rtl_fe_interface_put(struct rtl_fe *fe, struct rtl_fe_flow *flow)
{
	struct rtl_fe_interface *intf = &fe->interfaces[flow->counter];
	int ret;

	if (intf->users == 1) {
		rtl_fe_write(fe, RTL_FE_INTERFACE_DATA, intf->saved);
		ret = rtl_fe_indirect(fe, RTL_FE_INTERFACE_CMD,
				      RTL_NI_INDIRECT_ACCESS |
				      RTL_NI_INDIRECT_WRITE | flow->counter);
		if (ret)
			return ret;
		__clear_bit(flow->counter, fe->counters);
	}
	intf->users--;
	if (flow->counted)
		intf->counted = false;
	return 0;
}

static unsigned int rtl_fe_mtu_shift(unsigned int index)
{
	return (index % 2) * 16;
}

static int rtl_fe_mtu_get(struct rtl_fe *fe, struct rtl_fe_flow *flow,
			  unsigned int size)
{
	unsigned int i, index = FE_MTUS, shift;
	struct rtl_fe_mtu *mtu;
	u32 val;

	for (i = 0; i < FE_MTUS; i++) {
		if (fe->mtus[i].users && fe->mtus[i].size == size) {
			index = i;
			break;
		}
		if (!fe->mtus[i].users && index == FE_MTUS)
			index = i;
	}
	if (index == FE_MTUS)
		return -ENOSPC;

	mtu = &fe->mtus[index];
	if (!mtu->users) {
		shift = rtl_fe_mtu_shift(index);
		val = rtl_fe_read(fe, RTL_FE_MTU(index));
		mtu->saved = (val >> shift) & FE_MTU_MASK;
		mtu->size = size;
		rtl_fe_write(fe, RTL_FE_MTU(index),
			     (val & ~(FE_MTU_MASK << shift)) | size << shift);
	}
	mtu->users++;
	flow->mtu_index = index;
	flow->mtu_owned = true;
	return 0;
}

static void rtl_fe_mtu_put(struct rtl_fe *fe, struct rtl_fe_flow *flow)
{
	unsigned int index = flow->mtu_index, shift = rtl_fe_mtu_shift(index);
	struct rtl_fe_mtu *mtu = &fe->mtus[index];

	if (!--mtu->users)
		rtl_fe_rmw(fe, RTL_FE_MTU(index), FE_MTU_MASK << shift,
			   mtu->saved << shift);
	flow->mtu_owned = false;
}

static int rtl_fe_destination_get(struct rtl_fe *fe, struct rtl_fe_flow *flow,
				  const u8 *address)
{
	u32 words[RTL_FE_FDB_WORDS], result;
	struct rtl_fe_dest_mac *dm;
	int ret;

	ether_addr_copy(flow->destination, address);
	list_for_each_entry(dm, &fe->dest_macs, list) {
		if (!ether_addr_equal(dm->address, address))
			continue;
		if (!dm->ready)
			return -EAGAIN;
		dm->users++;
		flow->dest_mac = dm;
		flow->destination_index = dm->index;
		return 0;
	}

	dm = kzalloc(sizeof(*dm), GFP_KERNEL);
	if (!dm)
		return -ENOMEM;
	ether_addr_copy(dm->address, address);

	rtl_fe_fdb_key(words, address);
	ret = rtl_fe_fdb_command(fe, FE_FDB_LOOKUP, words, &result);
	if (ret)
		goto err_free;
	dm->created = (result & 15) != FE_FDB_FOUND;
	if (!dm->created)
		memcpy(dm->saved, words, sizeof(words));

	if (dm->created)
		rtl_fe_fdb_key(words, address);
	else
		words[0] |= FE_FDB_STATIC;
	ret = rtl_fe_fdb_command(fe, FE_FDB_ADD, words, &result);
	if (ret)
		goto err_free;
	if ((result & 15) != FE_FDB_FOUND) {
		ret = -ENOSPC;
		goto err_free;
	}

	dm->users = 1;
	list_add_tail(&dm->list, &fe->dest_macs);
	flow->dest_mac = dm;

	rtl_fe_fdb_key(words, address);
	ret = rtl_fe_fdb_command(fe, FE_FDB_LOOKUP, words, &result);
	if (ret)
		return ret;
	if ((result & 15) != FE_FDB_FOUND)
		return -EIO;
	dm->index = FIELD_GET(FE_FDB_INDEX, result);
	dm->ready = true;
	flow->destination_index = dm->index;
	return 0;

err_free:
	kfree(dm);
	return ret;
}

static int rtl_fe_destination_put(struct rtl_fe *fe, struct rtl_fe_flow *flow)
{
	struct rtl_fe_dest_mac *dm = flow->dest_mac;
	u32 words[RTL_FE_FDB_WORDS], result;
	int ret;

	if (!dm)
		return 0;
	if (dm->users > 1) {
		dm->users--;
		flow->dest_mac = NULL;
		return 0;
	}

	if (dm->created) {
		rtl_fe_fdb_key(words, dm->address);
		ret = rtl_fe_fdb_command(fe, FE_FDB_DELETE, words, &result);
	} else {
		memcpy(words, dm->saved, sizeof(words));
		ret = rtl_fe_fdb_command(fe, FE_FDB_ADD, words, &result);
	}
	if (ret)
		return ret;

	list_del(&dm->list);
	kfree(dm);
	flow->dest_mac = NULL;
	return 0;
}

int rtl_fe_vlan_tpid(__be16 protocol)
{
	switch (ntohs(protocol)) {
	case ETH_P_8021Q:
		return 1;
	case ETH_P_8021AD:
		return 2;
	default:
		return -EOPNOTSUPP;
	}
}

static int rtl_fe_vlan_match(const struct flow_match_vlan *match,
			     struct rtl_fe_vlan *vlan)
{
	int tpid;

	if (match->mask->vlan_id != VLAN_VID_MASK ||
	    match->mask->vlan_tpid != htons(0xffff) ||
	    match->mask->vlan_priority || match->mask->vlan_dei)
		return -EOPNOTSUPP;
	tpid = rtl_fe_vlan_tpid(match->key->vlan_tpid);
	if (tpid < 0)
		return tpid;
	*vlan = (struct rtl_fe_vlan){ .vid = match->key->vlan_id, .tpid = tpid };
	return 0;
}

static int rtl_fe_parse_vlans(struct flow_rule *rule, struct rtl_fe_rule *data)
{
	struct flow_match_vlan vlan;
	int ret;

	data->vlan_count = 0;
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		flow_rule_match_vlan(rule, &vlan);
		ret = rtl_fe_vlan_match(&vlan, &data->vlan[data->vlan_count++]);
		if (ret)
			return ret;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		if (!data->vlan_count)
			return -EOPNOTSUPP;
		flow_rule_match_cvlan(rule, &vlan);
		ret = rtl_fe_vlan_match(&vlan, &data->vlan[data->vlan_count++]);
		if (ret)
			return ret;
	}
	return 0;
}

struct rtl_fe_vlan_edit {
	struct rtl_fe_vlan vlan[2];
	bool known[2];
	unsigned int count;
	bool changed;
};

static int rtl_fe_vlan_edit(struct rtl_fe_vlan_edit *edit,
			    const struct flow_action_entry *act)
{
	int tpid;

	if (act->id == FLOW_ACTION_VLAN_POP) {
		if (!edit->count)
			return -EOPNOTSUPP;
		edit->vlan[0] = edit->vlan[1];
		edit->known[0] = edit->known[1];
		edit->count--;
		edit->changed = true;
		return 0;
	}

	if (edit->count == 2 || act->vlan.vid > VLAN_VID_MASK || act->vlan.prio > 7)
		return -EOPNOTSUPP;
	tpid = rtl_fe_vlan_tpid(act->vlan.proto);
	if (tpid < 0)
		return tpid;
	edit->vlan[1] = edit->vlan[0];
	edit->known[1] = edit->known[0];
	edit->vlan[0] = (struct rtl_fe_vlan){
		.vid = act->vlan.vid,
		.tpid = tpid,
		.priority = act->vlan.prio,
	};
	edit->known[0] = true;
	edit->count++;
	edit->changed = true;
	return 0;
}

static int rtl_fe_vlan_edit_done(const struct rtl_fe_vlan_edit *edit)
{
	unsigned int i;

	if (!edit->changed)
		return 0;
	for (i = 0; i < edit->count; i++)
		if (!edit->known[i])
			return -EOPNOTSUPP;
	return 0;
}

/* Decode a pedit mangle entry that rewrites only the IP DSCP field.
 *
 * The action field is six bits wide (rtl_fe_action_fields[RTL_FE_IP_DSCP]),
 * which is exactly the DSCP field of the IPv4 TOS byte and of the IPv6
 * Traffic Class byte.  A mangle that also touches the two ECN bits therefore
 * cannot be expressed and is refused instead of being approximated: the
 * hardware writes only those six bits, so ECN is preserved by construction.
 * On IPv4 the header checksum needs no software fixup because the very same
 * L3FE rewrite stage already decrements the TTL for every L3 rule
 * (rtl_fe_action_build() sets IP_TTL_DEC unconditionally for l3 entries), and
 * a stage that rewrites the header that way necessarily regenerates its
 * checksum; the driver has no separate IPv4 checksum control to enable.
 *
 * Returns 0 and the new 6-bit DSCP, or -EOPNOTSUPP for any other shape.
 */
#define RTL_FE_DSCP_MASK		0x3fU
#define RTL_FE_DSCP_WIDTH		6
#define RTL_FE_IPV6_DSCP_SHIFT		22
#define RTL_FE_IPV6_DSCP_MASK		GENMASK(27, 22)
#define RTL_FE_IPV4_TOS_OFF0_DSCP_SHIFT	18
#define RTL_FE_IPV4_TOS_OFF1_DSCP_SHIFT	26

static int rtl_fe_dscp_mangle(const struct flow_action_entry *act, bool ipv6,
			      u8 *dscp)
{
	u32 mask = act->mangle.mask;
	u32 val = act->mangle.val;
	unsigned int shift;

	if (!mask || (val & ~mask))
		return -EOPNOTSUPP;

	if (ipv6) {
		/* version[31:28] | traffic class[27:20] | flow label[19:0] */
		if (act->mangle.offset || (mask & ~(u32)RTL_FE_IPV6_DSCP_MASK))
			return -EOPNOTSUPP;
		shift = RTL_FE_IPV6_DSCP_SHIFT;
	} else {
		/*
		 * IPv4 TOS byte.  A pedit word holding it starts either at
		 * offset 0 (version/ihl/TOS/tot_len[15:8]) or at offset 1
		 * (TOS/tot_len/id).
		 */
		if (act->mangle.offset == 1)
			shift = RTL_FE_IPV4_TOS_OFF1_DSCP_SHIFT;
		else if (!act->mangle.offset)
			shift = RTL_FE_IPV4_TOS_OFF0_DSCP_SHIFT;
		else
			return -EOPNOTSUPP;
		if (mask & ~(u32)GENMASK(shift + RTL_FE_DSCP_WIDTH - 1, shift))
			return -EOPNOTSUPP;
	}

	*dscp = (val >> shift) & RTL_FE_DSCP_MASK;
	return 0;
}

static int rtl_fe_ipv6_mangle(__be32 target[4], u32 offset, u32 base,
			      u32 mask, u32 val)
{
	unsigned int word;
	u32 cur;

	if (offset < base || offset > base + 12 || (offset - base) % 4)
		return -EOPNOTSUPP;
	word = (offset - base) / 4;
	memcpy(&cur, &target[word], sizeof(cur));
	cur = (cur & mask) | val;
	memcpy(&target[word], &cur, sizeof(cur));
	return 0;
}

/*
 * Translate one FLOW_ACTION_POLICE entry into L3TE policer parameters.
 *
 * Only the single rate, drop-on-exceed form is accepted, because that is the
 * only shape this driver has evidence for:
 *
 *  - exceed = DROP and not-exceed = ACCEPT is one rate plus one burst.  The
 *    L3TE profile has independent CIR and PIR fields and both are programmed
 *    with that single rate, which makes the entry a single-rate policer that
 *    drops what it exceeds.  tc's optional peakrate is a real dual-rate
 *    shaper, so it is refused instead of being mapped onto PIR: the CIR/PIR
 *    colouring semantics are still U4 and the two models are not the same.
 *  - the hardware stores the rate as kbps = 500 * hi + lo (spec 2.3), so
 *    bytes/s is rounded up to kbps and anything above
 *    L3TE_POL_RATE_MAX_KBPS cannot be encoded at all.
 *  - burst is copied verbatim.  The unit of the 12 bit burst field has no
 *    direct evidence (phase0-re2 2.3 only records that the OEM caller passes
 *    it through and passes bytes), so it is not rescaled here.
 *    TODO(U-burst): confirm the unit on hardware before widening this.
 *
 * Returns 0 with *kbps and *burst_bytes set, or -EOPNOTSUPP.
 */
static int rtl_fe_police_encode(u64 rate_bytes_ps, u32 burst, u8 exceed,
				u8 notexceed, u32 *kbps, u32 *burst_bytes)
{
	u64 value;

	if (exceed != FLOW_ACTION_DROP || notexceed != FLOW_ACTION_ACCEPT)
		return -EOPNOTSUPP;
	if (!rate_bytes_ps || rate_bytes_ps > (u64)L3TE_POL_RATE_MAX_KBPS * 125u)
		return -EOPNOTSUPP;
	if (!burst || burst > L3TE_POL_BURST_MAX)
		return -EOPNOTSUPP;

	value = (rate_bytes_ps * 8u + 999u) / 1000u;
	if (!value || value > L3TE_POL_RATE_MAX_KBPS)
		return -EOPNOTSUPP;

	*kbps = (u32)value;
	*burst_bytes = burst;
	return 0;
}

/* Take one L3TE policer slot for a flow that asked for FLOW_ACTION_POLICE. */
static int rtl_fe_police_alloc(struct rtl_te_flow *te, u32 kbps, u32 burst,
			       u8 *slot, bool *owned)
{
	u8 index;
	int ret;

	if (*owned)
		return -EEXIST;
	if (!te)
		return -EOPNOTSUPP;

	/* CIR = PIR = rate: a single-rate policer.  The burst is used for both
	 * buckets for the same reason (see rtl_fe_police_encode()).
	 */
	ret = rtl_te_policer_alloc(te, (u64)kbps * 1000u, (u64)kbps * 1000u,
				   burst, burst, &index);
	if (ret)
		return ret;

	*slot = index;
	*owned = true;
	return 0;
}

/*
 * Return the slot, exactly once.  *owned is the authoritative state, so a
 * second call is a no-op and never reaches the policer layer; if that layer
 * reports the slot as already free (-EINVAL) only the bookkeeping is cleared,
 * because refusing here would wedge the flow teardown forever.
 */
static int rtl_fe_police_free(struct rtl_te_flow *te, u8 slot, bool *owned)
{
	int ret;

	if (!*owned)
		return 0;
	if (!te) {
		*owned = false;
		return 0;
	}

	ret = rtl_te_policer_free(te, slot);
	if (ret == -EINVAL) {
		*owned = false;
		return 0;
	}
	if (ret)
		return ret;

	*owned = false;
	return 0;
}

static int rtl_fe_tunnel_copy_ipv6(struct flow_rule *rule, u8 src[16],
				   u8 dst[16])
{
	struct flow_match_ipv6_addrs match;
	unsigned int i;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS))
		return -EOPNOTSUPP;
	flow_rule_match_enc_ipv6_addrs(rule, &match);
	for (i = 0; i < 4; i++)
		if (match.mask->src.s6_addr32[i] != htonl(~0U) ||
		    match.mask->dst.s6_addr32[i] != htonl(~0U))
			return -EOPNOTSUPP;
	memcpy(src, &match.key->src, 16);
	memcpy(dst, &match.key->dst, 16);
	return 0;
}

static int rtl_fe_tunnel_copy_ipv4(struct flow_rule *rule, u8 src[16],
				   u8 dst[16])
{
	struct flow_match_ipv4_addrs match;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS))
		return -EOPNOTSUPP;
	flow_rule_match_enc_ipv4_addrs(rule, &match);
	if (match.mask->src != htonl(~0U) || match.mask->dst != htonl(~0U))
		return -EOPNOTSUPP;
	memcpy(src, &match.key->src, 4);
	memcpy(dst, &match.key->dst, 4);
	return 0;
}

/* Recognise the dual-header and PPP tunnel profiles: DS-Lite (IPv4 inner,
 * outer IPv6 addresses), 6RD (IPv6 inner, 20-byte outer IPv4 encap Content),
 * and L2TPv2/PPTP cleartext PPP (inner IPv4/IPv6 over outer IPv4).
 */
static int rtl_fe_parse_ppp_opts(struct flow_rule *rule,
				 struct rtl_fe_rule *data, bool decap,
				 const struct ip_tunnel_info *info,
				 const struct net_device_ppp_tunnel_opts *opts)
{
	u16 tid, sid, peer_tid, peer_sid, sport, dport;

	tid = ntohs(opts->tid);
	sid = ntohs(opts->sid);
	peer_tid = ntohs(opts->peer_tid);
	peer_sid = ntohs(opts->peer_sid);
	if (opts->reserved)
		return -EOPNOTSUPP;

	if (opts->proto == IPPROTO_UDP) {
		if (opts->ppp_len != 4 || opts->flags)
			return -EOPNOTSUPP;
		if (!tid || !sid || !peer_tid || !peer_sid)
			return -EOPNOTSUPP;
		if (decap) {
			struct flow_match_ports ports;

			if (!flow_rule_match_key(rule,
						 FLOW_DISSECTOR_KEY_ENC_PORTS))
				return -EOPNOTSUPP;
			flow_rule_match_enc_ports(rule, &ports);
			if (ports.mask->src != htons(0xffff) ||
			    ports.mask->dst != htons(0xffff))
				return -EOPNOTSUPP;
			sport = ntohs(ports.key->src);
			dport = ntohs(ports.key->dst);
		} else {
			sport = ntohs(info->key.tp_src);
			dport = ntohs(info->key.tp_dst);
		}
		if (!sport || !dport ||
		    (sport != RTL_FE_L3PP_L2TP_SPORT_DFT &&
		     dport != RTL_FE_L3PP_L2TP_SPORT_DFT))
			return -EOPNOTSUPP;
		data->tunnel_type = RTL_FE_DUAL_TYPE_L2TP;
		data->tunnel_sport = sport;
		data->tunnel_dport = dport;
		data->tunnel_sid = sid;
		data->tunnel_peer_sid = peer_sid;
	} else if (opts->proto == IPPROTO_GRE) {
		if (decap &&
		    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_PORTS))
			return -EOPNOTSUPP;
		if (!decap && (info->key.tp_src || info->key.tp_dst))
			return -EOPNOTSUPP;
		if (!opts->ppp_len || opts->ppp_len > 4 ||
		    (opts->flags & ~(NETDEV_PPP_TUN_F_SEQ |
				     NETDEV_PPP_TUN_F_ACK)))
			return -EOPNOTSUPP;
		if (!tid || !peer_tid || sid || peer_sid)
			return -EOPNOTSUPP;
		data->tunnel_type = RTL_FE_DUAL_TYPE_PPTP;
		data->tunnel_gre_seq = ntohl(opts->gre_seq);
		data->tunnel_gre_ack = ntohl(opts->gre_ack);
	} else {
		return -EOPNOTSUPP;
	}

	data->tunnel_af = AF_INET;
	data->tunnel_ppp_len = opts->ppp_len;
	data->tunnel_flags = opts->flags;
	data->tunnel_tid = tid;
	data->tunnel_peer_tid = peer_tid;
	data->tunnel_decap = decap;
	return 0;
}

static int rtl_fe_parse_ppp_tunnel(struct flow_rule *rule,
				   struct rtl_fe_rule *data, bool decap,
				   const struct ip_tunnel_info *info,
				   unsigned int addr_type)
{
	struct net_device_ppp_tunnel_opts opts;

	if (!rtl_fe_vpn_offload_enabled())
		return -EOPNOTSUPP;

	if (decap) {
		struct flow_match_enc_opts enc_opts;
		int ret;

		if (addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS)
			return -EOPNOTSUPP;
		flow_rule_match_enc_opts(rule, &enc_opts);
		if (enc_opts.mask->len != 0xff ||
		    enc_opts.mask->dst_opt_type != ~0U ||
		    enc_opts.key->dst_opt_type != NETDEV_PPP_TUN_OPT_TYPE ||
		    enc_opts.key->len != sizeof(opts) ||
		    memchr_inv(enc_opts.mask->data, 0xff, sizeof(opts)))
			return -EOPNOTSUPP;
		memcpy(&opts, enc_opts.key->data, sizeof(opts));
		ret = rtl_fe_tunnel_copy_ipv4(rule, data->tunnel_src,
					      data->tunnel_dst);
		if (ret)
			return ret;
	} else {
		if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_PORTS) ||
		    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_OPTS))
			return -EOPNOTSUPP;
		if ((info->mode & IP_TUNNEL_INFO_IPV6) ||
		    info->options_len != sizeof(opts) ||
		    info->encap.type != NETDEV_PPP_TUN_OPT_TYPE ||
		    !test_bit(IP_TUNNEL_CRIT_OPT_BIT, info->key.tun_flags))
			return -EOPNOTSUPP;
		if (!info->key.u.ipv4.src || !info->key.u.ipv4.dst)
			return -EOPNOTSUPP;
		memcpy(&opts, ip_tunnel_info_opts(info), sizeof(opts));
		memcpy(data->tunnel_src, &info->key.u.ipv4.src, 4);
		memcpy(data->tunnel_dst, &info->key.u.ipv4.dst, 4);
	}

	return rtl_fe_parse_ppp_opts(rule, data, decap, info, &opts);
}

static int rtl_fe_parse_tunnel(struct flow_rule *rule,
			       struct rtl_fe_rule *data)
{
	struct flow_action_entry *act;
	struct flow_match_control enc_control;
	const struct ip_tunnel_info *info = NULL;
	bool have_control, have_tunnel = false, decap = false;
	unsigned int addr_type = 0;
	int i, ret;

	flow_action_for_each(i, act, &rule->action) {
		if (act->id != FLOW_ACTION_TUNNEL_ENCAP &&
		    act->id != FLOW_ACTION_TUNNEL_DECAP)
			continue;
		if (have_tunnel)
			return -EOPNOTSUPP;
		have_tunnel = true;
		decap = act->id == FLOW_ACTION_TUNNEL_DECAP;
		if (!decap)
			info = act->tunnel;
	}

	/* Preserve the pre-W8b behaviour when no tunnel action is present:
	 * only an ENC_CONTROL key was refused outright.
	 */
	if (!have_tunnel)
		return flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_CONTROL) ?
		       -EOPNOTSUPP : 0;
	if (!decap && !info)
		return -EOPNOTSUPP;
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_IP) ||
	    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_KEYID))
		return -EOPNOTSUPP;

	have_control = flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_CONTROL);
	if (have_control) {
		flow_rule_match_enc_control(rule, &enc_control);
		addr_type = enc_control.key->addr_type;
	}

	if (decap ? flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_OPTS) :
		    (info->options_len || info->encap.type ||
		     test_bit(IP_TUNNEL_CRIT_OPT_BIT, info->key.tun_flags)))
		return rtl_fe_parse_ppp_tunnel(rule, data, decap, info,
					       addr_type);

	if (!rtl_fe_tunnel_flow_enabled())
		return -EOPNOTSUPP;
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_PORTS) ||
	    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_OPTS))
		return -EOPNOTSUPP;

	if (decap) {
		if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS)
			data->tunnel_type = RTL_FE_DUAL_TYPE_IP4INIP6;
		else if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS)
			data->tunnel_type = RTL_FE_DUAL_TYPE_6RD;
		else
			return -EOPNOTSUPP;
	} else {
		/* The OEM DS-Lite/6RD templates carry no outer L4 header. */
		if (info->key.tp_src || info->key.tp_dst)
			return -EOPNOTSUPP;
		if (info->mode & IP_TUNNEL_INFO_IPV6)
			data->tunnel_type = RTL_FE_DUAL_TYPE_IP4INIP6;
		else
			data->tunnel_type = RTL_FE_DUAL_TYPE_6RD;
		if (have_control &&
		    addr_type != (data->tunnel_type == RTL_FE_DUAL_TYPE_IP4INIP6 ?
				  FLOW_DISSECTOR_KEY_IPV6_ADDRS :
				  FLOW_DISSECTOR_KEY_IPV4_ADDRS))
			return -EOPNOTSUPP;
	}

	if (data->tunnel_type == RTL_FE_DUAL_TYPE_IP4INIP6) {
		if (data->kind != RTL_FE_RULE_IPV4)
			return -EOPNOTSUPP;
		data->tunnel_af = AF_INET6;
		if (decap) {
			ret = rtl_fe_tunnel_copy_ipv6(rule, data->tunnel_src,
						      data->tunnel_dst);
			if (ret)
				return ret;
		} else {
			memcpy(data->tunnel_src, &info->key.u.ipv6.src, 16);
			memcpy(data->tunnel_dst, &info->key.u.ipv6.dst, 16);
		}
	} else {
		if (data->kind != RTL_FE_RULE_IPV6)
			return -EOPNOTSUPP;
		data->tunnel_af = AF_INET;
		if (decap) {
			ret = rtl_fe_tunnel_copy_ipv4(rule, data->tunnel_src,
						      data->tunnel_dst);
			if (ret)
				return ret;
		} else {
			memcpy(data->tunnel_src, &info->key.u.ipv4.src, 4);
			memcpy(data->tunnel_dst, &info->key.u.ipv4.dst, 4);
		}
	}
	data->tunnel_decap = decap;
	return 0;
}

static int rtl_fe_parse_l3(struct rtl_fe_port *port, struct flow_rule *rule,
			   struct rtl_fe_rule *data)
{
	struct rtl_fe_vlan_edit edit = { };
	struct flow_match_ipv4_addrs addrs = { };
	struct flow_match_ipv6_addrs addrs6 = { };
	struct flow_match_ports ports = { };
	struct flow_match_basic basic;
	struct flow_match_meta meta;
	struct flow_action_entry *act;
	struct net_device *output = NULL;
	u8 eth[ETH_HLEN] = { };
	u8 cos = 0, dscp = 0;
	u16 sport = 0, dport = 0;
	__be32 new_source = 0, new_destination = 0;
	__be32 new_source6[4] = { }, new_destination6[4] = { };
	__be16 new_sport, new_dport, protocol;
	bool source_changed, destination_changed, cos_set = false, dscp_set = false;
	bool gre = false, has_v4, has_v6;
	int i, ret;

	has_v4 = flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS);
	has_v6 = flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV6_ADDRS);
	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC) ||
	    has_v4 == has_v6)
		return -EOPNOTSUPP;

	flow_rule_match_meta(rule, &meta);
	if (meta.key->ingress_ifindex != port->netdev->ifindex)
		return -EOPNOTSUPP;

	flow_rule_match_basic(rule, &basic);
	protocol = basic.key->n_proto;
	if (protocol == htons(ETH_P_PPP_SES)) {
		struct flow_match_pppoe pppoe;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PPPOE))
			return -EOPNOTSUPP;
		flow_rule_match_pppoe(rule, &pppoe);
		if (pppoe.mask->session_id != htons(0xffff) ||
		    pppoe.mask->ppp_proto != htons(0xffff) ||
		    pppoe.mask->type != htons(0xffff) ||
		    pppoe.key->type != htons(ETH_P_PPP_SES))
			return -EOPNOTSUPP;
		if (pppoe.key->ppp_proto == htons(PPP_IP))
			protocol = htons(ETH_P_IP);
		else if (pppoe.key->ppp_proto == htons(PPP_IPV6))
			protocol = htons(ETH_P_IPV6);
		else
			return -EOPNOTSUPP;
		data->pppoe_input = true;
		data->pppoe_session = ntohs(pppoe.key->session_id);
	}

	data->kind = has_v6 ? RTL_FE_RULE_IPV6 : RTL_FE_RULE_IPV4;
	if ((protocol != htons(ETH_P_IP) && protocol != htons(ETH_P_IPV6)) ||
	    basic.mask->n_proto != htons(0xffff) || basic.mask->ip_proto != 0xff)
		return -EOPNOTSUPP;

	/*
	 * TCP/UDP still require an exact L4 port key.  GRE is matched on the
	 * outer IP header only: it has no L4 ports, so its port key is 0/0 and
	 * no port or address translation may be requested for it.
	 */
	switch (basic.key->ip_proto) {
	case IPPROTO_TCP:
	case IPPROTO_UDP:
		break;
	case IPPROTO_GRE:
		gre = true;
		break;
	default:
		return -EOPNOTSUPP;
	}
	/*
	 * A tunnel/encap key would describe the inner packet, which this table
	 * cannot match on.
	 */
	if (gre && (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS) ||
		    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS) ||
		    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS) ||
		    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_PORTS)))
		return -EOPNOTSUPP;
	ret = rtl_fe_parse_tunnel(rule, data);
	if (ret)
		return ret;
	if (!data->tunnel_type) {
		if ((data->kind == RTL_FE_RULE_IPV6) !=
		    (protocol == htons(ETH_P_IPV6)))
			return -EOPNOTSUPP;
	} else if (data->tunnel_af && data->pppoe_input) {
		if ((data->tunnel_af == AF_INET6) !=
		    (protocol == htons(ETH_P_IPV6)))
			return -EOPNOTSUPP;
	}
	if (gre && data->tunnel_type)
		return -EOPNOTSUPP;

	ret = rtl_fe_parse_vlans(rule, data);
	if (ret)
		return ret;
	edit.count = data->vlan_count;
	memcpy(edit.vlan, data->vlan, sizeof(edit.vlan));

	if (!gre) {
		flow_rule_match_ports(rule, &ports);
		if (ports.mask->src != htons(0xffff) ||
		    ports.mask->dst != htons(0xffff))
			return -EOPNOTSUPP;
		sport = ntohs(ports.key->src);
		dport = ntohs(ports.key->dst);
	}
	data->protocol = basic.key->ip_proto;

	data->input = port;
	data->key = (struct rtl_fe_key){
		.source_port = sport,
		.destination_port = dport,
		.protocol = basic.key->ip_proto,
		.ingress = port->source,
		.llid = port->llid,
		.mask = RTL_FE_MASK_L3,
	};

	if (data->kind == RTL_FE_RULE_IPV6) {
		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV6_ADDRS))
			return -EOPNOTSUPP;
		flow_rule_match_ipv6_addrs(rule, &addrs6);
		for (i = 0; i < 4; i++) {
			if (addrs6.mask->src.s6_addr32[i] != htonl(~0U) ||
			    addrs6.mask->dst.s6_addr32[i] != htonl(~0U))
				return -EOPNOTSUPP;
			data->key.source[i] = ntohl(addrs6.key->src.s6_addr32[i]);
			data->key.destination[i] = ntohl(addrs6.key->dst.s6_addr32[i]);
		}
		memcpy(new_source6, &addrs6.key->src, sizeof(new_source6));
		memcpy(new_destination6, &addrs6.key->dst, sizeof(new_destination6));
	} else {
		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS))
			return -EOPNOTSUPP;
		flow_rule_match_ipv4_addrs(rule, &addrs);
		if (addrs.mask->src != htonl(~0U) || addrs.mask->dst != htonl(~0U))
			return -EOPNOTSUPP;
		data->key.source[3] = ntohl(addrs.key->src);
		data->key.destination[3] = ntohl(addrs.key->dst);
		new_source = addrs.key->src;
		new_destination = addrs.key->dst;
	}

	new_sport = htons(sport);
	new_dport = htons(dport);
	flow_action_for_each(i, act, &rule->action) {
		unsigned int offset = act->mangle.offset;
		u32 val;

		switch (act->id) {
		case FLOW_ACTION_PPPOE_PUSH:
			if (!act->pppoe.sid || !act->pppoe.mtu ||
			    act->pppoe.mtu > FE_MTU_MASK)
				return -EOPNOTSUPP;
			data->egress_pppoe = 3U | ((unsigned int)act->pppoe.sid << 2);
			data->mtu = act->pppoe.mtu;
			break;
		case FLOW_ACTION_VLAN_POP:
		case FLOW_ACTION_VLAN_PUSH:
			ret = rtl_fe_vlan_edit(&edit, act);
			if (ret)
				return ret;
			break;
		case FLOW_ACTION_REDIRECT:
			output = act->dev;
			break;
		case FLOW_ACTION_PRIORITY:
			if (act->priority > 7)
				return -EOPNOTSUPP;
			cos = act->priority;
			cos_set = true;
			break;
		case FLOW_ACTION_CSUM:
			break;
		case FLOW_ACTION_POLICE: {
			u32 police_kbps, police_burst;

			if (data->police)
				return -EOPNOTSUPP;
			/* peakrate / packet-rate / burst_pkt / overhead / avrate
			 * have no recovered hardware counterpart (U4), so a rule
			 * that uses them is refused rather than partly honoured.
			 * police.mtu is deliberately not checked: tc derives a
			 * non-zero value from the rate table even when the user
			 * never asked for one (255 << cell_log), and the profile has
			 * no frame-size drop field.  TODO(U-mtu).
			 */
			if (act->police.peakrate_bytes_ps ||
			    act->police.rate_pkt_ps || act->police.burst_pkt ||
			    act->police.overhead || act->police.avrate)
				return -EOPNOTSUPP;
			ret = rtl_fe_police_encode(act->police.rate_bytes_ps,
						   act->police.burst,
						   act->police.exceed.act_id,
						   act->police.notexceed.act_id,
						   &police_kbps, &police_burst);
			if (ret)
				return ret;
			data->police = true;
			data->police_kbps = police_kbps;
			data->police_burst = police_burst;
			break;
		}
		case FLOW_ACTION_TUNNEL_ENCAP:
		case FLOW_ACTION_TUNNEL_DECAP:
			/* Consumed by rtl_fe_parse_tunnel(). */
			break;
		case FLOW_ACTION_MANGLE:
			switch (act->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				if (offset > ETH_HLEN - sizeof(val))
					return -EOPNOTSUPP;
				val = get_unaligned((u32 *)(eth + offset));
				put_unaligned((val & act->mangle.mask) | act->mangle.val,
					      (u32 *)(eth + offset));
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				if (data->kind != RTL_FE_RULE_IPV4)
					return -EOPNOTSUPP;
				if (act->mangle.mask) {
					ret = rtl_fe_dscp_mangle(act, false, &dscp);
					if (ret)
						return ret;
					dscp_set = true;
					break;
				}
				/* GRE is forwarded untouched: no address NAT. */
				if (gre)
					return -EOPNOTSUPP;
				if (offset == offsetof(struct iphdr, saddr))
					memcpy(&new_source, &act->mangle.val, 4);
				else if (offset == offsetof(struct iphdr, daddr))
					memcpy(&new_destination, &act->mangle.val, 4);
				else
					return -EOPNOTSUPP;
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP6:
				if (data->kind != RTL_FE_RULE_IPV6)
					return -EOPNOTSUPP;
				if (offset < offsetof(struct ipv6hdr, saddr)) {
					ret = rtl_fe_dscp_mangle(act, true, &dscp);
					if (ret)
						return ret;
					dscp_set = true;
					break;
				}
				if (gre)
					return -EOPNOTSUPP;
				if (offset < offsetof(struct ipv6hdr, daddr))
					ret = rtl_fe_ipv6_mangle(new_source6, offset,
								 offsetof(struct ipv6hdr, saddr),
								 act->mangle.mask,
								 act->mangle.val);
				else
					ret = rtl_fe_ipv6_mangle(new_destination6, offset,
								 offsetof(struct ipv6hdr, daddr),
								 act->mangle.mask,
								 act->mangle.val);
				if (ret)
					return ret;
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
			case FLOW_ACT_MANGLE_HDR_TYPE_UDP: {
				__be16 pair[2] = { new_sport, new_dport };

				if (gre || offset)
					return -EOPNOTSUPP;
				memcpy(&val, pair, 4);
				val = (val & act->mangle.mask) | act->mangle.val;
				memcpy(pair, &val, 4);
				new_sport = pair[0];
				new_dport = pair[1];
				break;
			}
			default:
				return -EOPNOTSUPP;
			}
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	data->output = rtl_fe_port_find(port->fe, output);
	if (!data->output || data->input == data->output)
		return -EOPNOTSUPP;
	ether_addr_copy(data->destination, eth);
	ether_addr_copy(data->source, eth + ETH_ALEN);
	if (!is_valid_ether_addr(data->source) ||
	    !is_valid_ether_addr(data->destination))
		return -EINVAL;

	if (data->kind == RTL_FE_RULE_IPV6) {
		source_changed = memcmp(new_source6, &addrs6.key->src,
					sizeof(new_source6)) != 0 ||
				 new_sport != htons(sport);
		destination_changed = memcmp(new_destination6, &addrs6.key->dst,
					     sizeof(new_destination6)) != 0 ||
				      new_dport != htons(dport);
	} else {
		source_changed = new_source != addrs.key->src ||
				 new_sport != htons(sport);
		destination_changed = new_destination != addrs.key->dst ||
				      new_dport != htons(dport);
	}
	if (gre && (source_changed || destination_changed))
		return -EOPNOTSUPP;
	if (source_changed && destination_changed)
		return -EOPNOTSUPP;
	if (data->kind == RTL_FE_RULE_IPV6 &&
	    (source_changed || destination_changed) && !port->fe->napt6_active)
		return -EOPNOTSUPP;
	ret = rtl_fe_vlan_edit_done(&edit);
	if (ret)
		return ret;

	/*
	 * GRE reuses the shared L3 check-mask: the driver builds exactly one
	 * L3 mask template (rtl_fe_mask_l3()) and the OEM driver exposes no
	 * portless variant, so the absent L4 port key is carried as 0/0 rather
	 * than inventing a new mask profile without field-level evidence.
	 */
	data->action = (struct rtl_fe_action){
		.destination = data->output->destination,
		.check_mask = RTL_FE_MASK_L3,
		.deep_queue = rtl_fe_use_deep_queue(port->fe, data->input,
						    data->output),
		.cos = cos,
		.dscp = dscp,
		.dscp_set = dscp_set,
		.translate = source_changed || destination_changed,
		.translate_destination = destination_changed,
		.translate_ipv6 = data->kind == RTL_FE_RULE_IPV6 &&
				  (source_changed || destination_changed),
		.address = data->kind == RTL_FE_RULE_IPV6 ? 0 :
			   ntohl(destination_changed ? new_destination :
				 new_source),
		.port = ntohs(destination_changed ? new_dport : new_sport),
		.vlan_set = edit.changed,
		.pppoe_pop = data->pppoe_input && !data->egress_pppoe,
		.vlan_count = edit.count,
	};
	if (data->action.translate_ipv6) {
		const __be32 *addr6 = destination_changed ? new_destination6 :
							    new_source6;

		for (i = 0; i < 4; i++)
			data->action.address6[i] = ntohl(addr6[i]);
	}
	data->cos_set = cos_set;
	memcpy(data->action.vlan, edit.vlan, sizeof(edit.vlan));
	data->ethernet_bytes = ETH_HLEN + VLAN_HLEN * edit.count +
			       (data->egress_pppoe ? FE_PPPOE_HLEN : 0);
	return 0;
}

static int rtl_fe_parse(struct rtl_fe_port *port, struct flow_cls_offload *cls,
			struct rtl_fe_rule *data)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls);
	int ret;

	ret = rtl_fe_parse_l3(port, rule, data);
	if (ret)
		return ret;
	return rtl_fe_itu_rule(data);
}

static void rtl_fe_l2tp_sport_put(struct rtl_fe *fe, struct rtl_fe_flow *flow)
{
	if (!flow->l2tp_alt_sport)
		return;
	flow->l2tp_alt_sport = 0;
	if (!fe->l2tp_alt_refs || --fe->l2tp_alt_refs)
		return;
	fe->l2tp_alt_sport = 0;
	if (fe->dual_hdr_active)
		rtl_fe_write_l2tp_sport(fe);
}

static int rtl_fe_tunnel_teardown(struct rtl_fe *fe, struct rtl_fe_flow *flow)
{
	bool is_last = false, cls_active = false;
	unsigned int cls_index = 0;
	int ret;

	if (!flow->tunnel_owned) {
		rtl_fe_l2tp_sport_put(fe, flow);
		return 0;
	}

	ret = rtl_fe_tunnel_flow_cls_info(fe, flow->dual_index, &is_last,
					  &cls_active, &cls_index);
	if (ret == -ENOENT) {
		flow->tunnel_owned = false;
		rtl_fe_l2tp_sport_put(fe, flow);
		return 0;
	}
	if (ret)
		return ret;

	/* Disarm stage-0 CLS 0x14 before tearing down the dual-header slot. */
	if (is_last && cls_active) {
		ret = rtl_fe_cls_clear_pair(fe, cls_index);
		if (ret)
			return ret;
		rtl_fe_tunnel_flow_cls_cleared(fe, flow->dual_index);
	}

	ret = rtl_fe_tunnel_flow_put(fe, flow->dual_index);
	if (ret == -ENOENT) {
		flow->tunnel_owned = false;
		rtl_fe_l2tp_sport_put(fe, flow);
		return 0;
	}
	if (ret)
		return ret;

	flow->tunnel_owned = false;
	rtl_fe_l2tp_sport_put(fe, flow);
	return 0;
}

int rtl_fe_delete(struct rtl_fe *fe, struct rtl_fe_flow *flow)
{
	int ret;

	flow->deleting = true;
	if (flow->published) {
		ret = rtl_fe_age_write(fe, flow->hash_index, 0);
		if (ret)
			goto retry;
		ret = rtl_fe_cache_command(fe, flow->hash_index, flow->crc16, 1);
		if (ret)
			goto retry;
		flow->published = false;
	}
	((__le32 *)fe->memory)[flow->hash_index] = 0;
	dma_wmb();

	if (flow->tunnel_owned || flow->l2tp_alt_sport) {
		ret = rtl_fe_tunnel_teardown(fe, flow);
		if (ret)
			goto retry;
	}
	if (flow->dest_mac) {
		ret = rtl_fe_destination_put(fe, flow);
		if (ret)
			goto retry;
	}
	if (flow->source_owned) {
		ret = rtl_fe_smac_put(fe, flow->source_mac);
		if (ret)
			goto retry;
		flow->source_owned = false;
	}
	if (flow->interface_owned) {
		ret = rtl_fe_interface_put(fe, flow);
		if (ret)
			goto retry;
		flow->interface_owned = false;
	}
	if (flow->mtu_owned)
		rtl_fe_mtu_put(fe, flow);

	/* Last fallible step: the hardware entry is already gone, so the slot can
	 * be returned.  A failure here is retried like the resource puts above.
	 * A borrowed Host Policing slot is never released here: pol_owned is false
	 * for those flows, so rtl_fe_police_free() returns without touching it.
	 */
	ret = rtl_fe_police_free(fe->te, flow->pol_slot, &flow->pol_owned);
	if (ret)
		goto retry;

	__clear_bit(flow->hash_index, fe->hashes);
	if (flow->learned)
		rtl_fe_bridge_flow_removed(fe);
	if (flow->hashed)
		rhashtable_remove_fast(&fe->cookies, &flow->node, rtl_fe_flow_params);
	list_del_init(&flow->retry);
	list_del_init(&flow->bridge_list);
	list_del(&flow->list);
	if (flow->installed)
		fe->deletes++;
	kfree_rcu(flow, rcu);
	return 0;

retry:
	if (list_empty(&flow->retry))
		list_add_tail(&flow->retry, &fe->retries);
	mod_delayed_work(system_wq, &fe->cleanup_work, HZ);
	return ret;
}

static void rtl_fe_cleanup_work(struct work_struct *work)
{
	struct rtl_fe *fe = container_of(to_delayed_work(work), struct rtl_fe,
					 cleanup_work);
	struct rtl_fe_flow *flow, *next;

	mutex_lock(&fe->lock);
	list_for_each_entry_safe(flow, next, &fe->retries, retry)
		rtl_fe_delete(fe, flow);
	rtl_fe_collect_ports(fe);
	mutex_unlock(&fe->lock);
}

static void rtl_fe_rule_key(const struct rtl_fe_rule *data,
			    u32 words[RTL_FE_KEY_WORDS])
{
	unsigned int i;

	if (data->kind == RTL_FE_RULE_BRIDGE)
		rtl_fe_key_bridge(words, data->source, data->destination,
				  data->ethernet_protocol, &data->key);
	else
		rtl_fe_key_ip(words, &data->key, data->kind == RTL_FE_RULE_IPV6);
	rtl_fe_key_vlan(words, data->vlan, data->vlan_count);

	if (data->vlan_exact) {
		for (i = 0; i < data->vlan_count; i++) {
			rtl_fe_key_set(words, i ? RTL_FE_KEY_INNER_DEI : RTL_FE_KEY_TOP_DEI,
				       !!(data->ingress_tci[i] & VLAN_CFI_MASK));
			rtl_fe_key_set(words, i ? RTL_FE_KEY_INNER_PCP : RTL_FE_KEY_TOP_PCP,
				       data->ingress_tci[i] >> VLAN_PRIO_SHIFT);
		}
	}
	rtl_fe_key_set(words, RTL_FE_KEY_PPPOE_SESSION, data->pppoe_session);
	rtl_fe_key_set(words, RTL_FE_KEY_PPPOE_TYPE, data->pppoe_input ? 2 : 0);
	if (data->vlan_exact && data->pppoe_input)
		rtl_fe_key_set(words, RTL_FE_KEY_PPP_PROTOCOL, data->ppp_protocol_slot);
	rtl_fe_key_set(words, RTL_FE_KEY_ORIG_LSPID, 0);
	rtl_fe_key_set(words, RTL_FE_KEY_CTRL_SET_ID, 0);
	rtl_fe_key_set(words, RTL_FE_KEY_TABLE_ID, 0);
}

static bool rtl_fe_port_ready(const struct rtl_fe_port *port)
{
	return !port->retired && port->bindings && tc_can_offload(port->netdev) &&
	       (port->source != FE_SOURCE_PON || port->fe->pon_attached);
}

/* Slot number shown in the flows dump: -1 when the flow has no policer. */
static int rtl_fe_flow_pol_id(const struct rtl_fe_flow *flow)
{
	return flow->pol_bound ? (int)flow->pol_slot : -1;
}

static int rtl_fe_hostpol_lookup(struct rtl_fe *fe, const u8 mac[ETH_ALEN],
				 u8 dir, u8 *slot, bool *rate_limited)
{
	int ret = rtl_te_hostpol_lookup_ex(fe->te, mac, dir, slot,
					   rate_limited);

	/* "no such MAC entry" and "entry has no slot in this direction" are both
	 * plain misses for a flow binding.
	 */
	return ret == -ENOENT || ret == -EOPNOTSUPP ? -ENOENT : ret;
}

/*
 * Decide the single Host Policing POL1 slot for an L3 flow from its source
 * (SA / ingress) and destination (DA / egress) MACs.
 *
 * Priority follows fc_8277c _rtk_fc_g3FlowIdDecision:
 *   1. SA ingress rate limit (0xA0 + vm)
 *   2. DA egress rate limit  (0xA0 + vm)
 *   3. SA ingress MIB slot   (0xC0 + idx, counting only)
 *   4. DA egress MIB slot    (0xE0 + idx, counting only)
 */
static int rtl_fe_hostpol_decide(struct rtl_fe *fe, const u8 src[ETH_ALEN],
				 const u8 dst[ETH_ALEN], u8 *slot)
{
	bool in_limited = false, eg_limited = false;
	u8 in_slot = 0, eg_slot = 0;
	int in_ret, eg_ret;

	in_ret = rtl_fe_hostpol_lookup(fe, src, RTL_TE_HOSTPOL_DIR_INGRESS,
				       &in_slot, &in_limited);
	if (in_ret && in_ret != -ENOENT)
		return in_ret;
	if (!in_ret && in_limited) {
		*slot = in_slot;
		return 0;
	}

	eg_ret = rtl_fe_hostpol_lookup(fe, dst, RTL_TE_HOSTPOL_DIR_EGRESS,
				       &eg_slot, &eg_limited);
	if (eg_ret && eg_ret != -ENOENT)
		return eg_ret;
	if (!eg_ret && eg_limited) {
		*slot = eg_slot;
		return 0;
	}

	if (!in_ret) {
		*slot = in_slot;
		return 0;
	}
	if (!eg_ret) {
		*slot = eg_slot;
		return 0;
	}
	return -ENOENT;
}

/*
 * Bind an L3 flow to a Host Policing entry.  The source MAC is the ingress
 * (SA) key and the destination MAC the egress (DA) key
 * (phase2-re4-hostpolicing.md section 5.5: one MAC entry serves both
 * directions).  A hit does not take ownership of the POL1 slot: the slot
 * belongs to the MAC table entry and outlives every flow bound to it.
 */
static int rtl_fe_hostpol_bind(struct rtl_fe *fe, struct rtl_fe_rule *data,
			       struct rtl_fe_flow *flow)
{
	u8 slot;
	int ret;

	ret = rtl_fe_hostpol_decide(fe, data->source, data->destination, &slot);
	if (ret == -ENOENT)
		return 0;
	if (ret)
		return ret;

	flow->pol_slot = slot;
	flow->pol_host = true;
	flow->pol_bound = true;
	return 0;
}

static bool rtl_fe_neigh_mac(struct net_device *dev, enum rtl_fe_rule_kind kind,
			     const u32 src[4], u8 mac[ETH_ALEN])
{
	struct neighbour *n = NULL;
	bool found = false;

	if (!dev)
		return false;

	if (kind == RTL_FE_RULE_IPV4) {
		__be32 saddr = htonl(src[3]);

		n = neigh_lookup(&arp_tbl, &saddr, dev);
	}
#if IS_ENABLED(CONFIG_IPV6)
	else if (kind == RTL_FE_RULE_IPV6) {
		struct in6_addr saddr6;
		unsigned int i;

		for (i = 0; i < 4; i++)
			saddr6.s6_addr32[i] = htonl(src[i]);
		n = neigh_lookup(&nd_tbl, &saddr6, dev);
	}
#endif
	if (!n)
		return false;

	read_lock_bh(&n->lock);
	if ((n->nud_state & NUD_VALID) && is_valid_ether_addr(n->ha)) {
		ether_addr_copy(mac, n->ha);
		found = true;
	}
	read_unlock_bh(&n->lock);
	neigh_release(n);
	return found;
}

/*
 * On routed/SNAT flows, FLOW_ACTION_MANGLE rewrites the Ethernet source MAC to
 * the egress router interface MAC.  After rtl_fe_smac_get() has consumed that
 * egress MAC, resolve the pre-NAT ingress host MAC from the neighbour table on
 * the input port (or its bridge master) so Host Policing SA rules match.
 */
static void rtl_fe_resolve_hostpol_source(struct rtl_fe_rule *data)
{
	struct net_device *in_dev, *master;
	u8 mac[ETH_ALEN];

	if (!data || !data->input || !data->input->netdev ||
	    data->kind == RTL_FE_RULE_BRIDGE)
		return;

	in_dev = data->input->netdev;
	if (rtl_fe_neigh_mac(in_dev, data->kind, data->key.source, mac)) {
		ether_addr_copy(data->source, mac);
		return;
	}

	rcu_read_lock();
	master = netdev_master_upper_dev_get_rcu(in_dev);
	dev_hold(master);
	rcu_read_unlock();
	if (master) {
		if (rtl_fe_neigh_mac(master, data->kind, data->key.source, mac))
			ether_addr_copy(data->source, mac);
		dev_put(master);
	}
}

/*
 * Pick the flow's policer.  An explicit FLOW_ACTION_POLICE owns its slot and
 * always wins; otherwise an L3 flow may borrow the host policing slot of its
 * source (ingress) or destination (egress) MAC.  Bridge flows are not bound:
 * their MAC pair is learned, not configured.
 */
static int rtl_fe_police_bind(struct rtl_fe *fe, struct rtl_fe_rule *data,
			      struct rtl_fe_flow *flow)
{
	if (flow->pol_owned) {
		/* An explicit slot is a binding too, it just has another owner. */
		flow->pol_bound = true;
		return 0;
	}
	if (!fe->te || data->kind == RTL_FE_RULE_BRIDGE)
		return 0;
	return rtl_fe_hostpol_bind(fe, data, flow);
}

static u16 rtl_fe_tunnel_busy_phys_mask(const struct rtl_fe *fe)
{
	u16 mask = 0;
	unsigned int d;

	for (d = 0; d < RTL_FE_DUAL_CONTROL_ENTRIES; d++) {
		unsigned int phys = RTL_FE_EIF_ALIAS_BASE + d;

		if (fe->interfaces[phys].users || test_bit(phys, fe->counters))
			mask |= BIT(d);
	}
	return mask;
}

static int rtl_fe_l2tp_sport_get(struct rtl_fe *fe,
				 const struct rtl_fe_rule *data,
				 struct rtl_fe_flow *flow)
{
	u16 alt_sport = 0;

	if (data->tunnel_type != RTL_FE_DUAL_TYPE_L2TP)
		return 0;
	if (data->tunnel_sport != RTL_FE_L3PP_L2TP_SPORT_DFT)
		alt_sport = data->tunnel_sport;
	else if (data->tunnel_dport != RTL_FE_L3PP_L2TP_SPORT_DFT)
		alt_sport = data->tunnel_dport;
	if (!alt_sport)
		return 0;

	if (fe->l2tp_alt_refs && fe->l2tp_alt_sport != alt_sport)
		return -EBUSY;
	if (!fe->l2tp_alt_refs) {
		fe->l2tp_alt_sport = alt_sport;
		rtl_fe_write_l2tp_sport(fe);
	}
	fe->l2tp_alt_refs++;
	flow->l2tp_alt_sport = alt_sport;
	return 0;
}

static int rtl_fe_install_tunnel_cls(struct rtl_fe *fe,
				     const struct rtl_fe_rule *data,
				     const struct rtl_fe_tunnel_binding *b)
{
	bool ipv6 = data->tunnel_af == AF_INET6;

	switch (data->tunnel_type) {
	case RTL_FE_DUAL_TYPE_IP4INIP6:
		return rtl_fe_cls_write_dslite(fe, b->cls_index,
					       data->tunnel_src, b->hash_eif);
	case RTL_FE_DUAL_TYPE_6RD:
		return rtl_fe_cls_write_6rd(fe, b->cls_index,
					    data->tunnel_src, data->tunnel_dst,
					    b->hash_eif);
	case RTL_FE_DUAL_TYPE_L2TP:
		return rtl_fe_cls_write_l2tp(fe, b->cls_index, ipv6,
					     data->tunnel_dst, data->tunnel_tid,
					     data->tunnel_sid, b->hash_eif);
	case RTL_FE_DUAL_TYPE_PPTP:
		return rtl_fe_cls_write_pptp(fe, b->cls_index, ipv6,
					     data->tunnel_dst, data->tunnel_tid,
					     b->hash_eif);
	default:
		return -EOPNOTSUPP;
	}
}

static bool rtl_fe_tunnel_type_enabled(u8 type)
{
	switch (type) {
	case RTL_FE_DUAL_TYPE_L2TP:
	case RTL_FE_DUAL_TYPE_PPTP:
		return rtl_fe_vpn_offload_enabled();
	case RTL_FE_DUAL_TYPE_IP4INIP6:
	case RTL_FE_DUAL_TYPE_6RD:
		return rtl_fe_tunnel_flow_enabled();
	default:
		return false;
	}
}

static int rtl_fe_install_tunnel(struct rtl_fe *fe,
				 const struct rtl_fe_rule *data,
				 struct rtl_fe_flow *flow,
				 struct rtl_fe_tunnel_binding *binding)
{
	struct rtl_fe_tunnel_key key = { };
	size_t addr_len;
	int ret;

	if (!fe->dual_hdr_active) {
		rtl_fe_save_dual_hdr(fe);
		rtl_fe_program_dual_hdr(fe);
		fe->dual_hdr_active = true;
	}

	if (data->tunnel_decap) {
		ret = rtl_fe_interface_get(fe, flow);
		if (ret)
			return ret;
	}

	ret = rtl_fe_l2tp_sport_get(fe, data, flow);
	if (ret)
		return ret;

	key.type = data->tunnel_type;
	key.decap = data->tunnel_decap;
	key.af = data->tunnel_af;
	key.ppp_len = data->tunnel_ppp_len;
	key.flags = data->tunnel_flags;
	key.sport = data->tunnel_sport;
	key.dport = data->tunnel_dport;
	key.tid = data->tunnel_tid;
	key.sid = data->tunnel_sid;
	key.peer_tid = data->tunnel_peer_tid;
	key.peer_sid = data->tunnel_peer_sid;
	key.gre_seq = data->tunnel_gre_seq;
	key.gre_ack = data->tunnel_gre_ack;
	key.cls_profile = data->input->source == FE_SOURCE_PON ? 0 : 1;
	key.param_hash = data->tunnel_decap ? 0 : rtl_fe_interface_value(flow);
	if (data->tunnel_type == RTL_FE_DUAL_TYPE_IP4INIP6 ||
	    data->tunnel_af == AF_INET6)
		addr_len = RTL_FE_DUAL_DSLITE_ADDR_LEN;
	else
		addr_len = RTL_FE_DUAL_6RD_ADDR_LEN;
	memcpy(key.src, data->tunnel_src, addr_len);
	memcpy(key.dst, data->tunnel_dst, addr_len);

	ret = rtl_fe_tunnel_flow_get(fe, &key,
				     rtl_fe_tunnel_busy_phys_mask(fe),
				     binding);
	if (ret)
		return ret;

	flow->tunnel_owned = true;
	flow->dual_index = binding->dual_index;

	if (!data->tunnel_decap) {
		ret = rtl_fe_interface_bind_slot(fe, flow, binding->phys_eif);
		if (ret)
			return ret;
	}

	if (binding->need_cls_program) {
		ret = rtl_fe_install_tunnel_cls(fe, data, binding);
		if (ret)
			return ret;
		ret = rtl_fe_tunnel_flow_mark_cls(fe, flow->dual_index);
		if (ret) {
			rtl_fe_cls_clear_pair(fe, binding->cls_index);
			return ret;
		}
	}

	return 0;
}

int rtl_fe_install(struct rtl_fe_port *port, struct rtl_fe_rule *data,
		   unsigned long cookie, struct rtl_fe_flow **out)
{
	struct rtl_fe_tunnel_binding tunnel = { };
	u32 words[RTL_FE_KEY_WORDS], action[RTL_FE_ACTION_MAX_WORDS];
	u32 counters[RTL_FE_COUNTER_WORDS], crc32;
	struct rtl_fe *fe = port->fe;
	struct rtl_fe_flow *flow;
	unsigned int index, start;
	u16 crc16;
	int ret;

	if (port->retired)
		return -ENODEV;
	if (!rtl_fe_port_ready(data->input) || !rtl_fe_port_ready(data->output))
		return -EOPNOTSUPP;
	if (!fe->enabled || !netif_running(data->input->netdev) ||
	    !netif_running(data->output->netdev) ||
	    !netif_carrier_ok(data->input->netdev) ||
	    !netif_carrier_ok(data->output->netdev))
		return -EAGAIN;
	if (cookie && rtl_fe_flow_find(fe, cookie))
		return -EEXIST;
	if (data->tunnel_type &&
	    (data->kind == RTL_FE_RULE_BRIDGE ||
	     !rtl_fe_tunnel_type_enabled(data->tunnel_type)))
		return -EOPNOTSUPP;

	rtl_fe_rule_key(data, words);
	rtl_fe_key_crc(words, &crc32, &crc16);
	start = rtl_fe_hash_bucket(crc16);
	for (index = start; index < start + RTL_FE_HASH_BUCKET; index++)
		if (!test_bit(index, fe->hashes))
			break;
	if (index == start + RTL_FE_HASH_BUCKET)
		return -ENOSPC;

	flow = kzalloc(sizeof(*flow), GFP_KERNEL);
	if (!flow)
		return -ENOMEM;
	INIT_LIST_HEAD(&flow->retry);
	INIT_LIST_HEAD(&flow->bridge_list);
	flow->cookie = cookie ?: (unsigned long)flow;
	flow->hash_index = index;
	flow->crc16 = crc16;
	flow->kind = data->kind;
	flow->input = data->input;
	flow->output = data->output;
	flow->lastused = jiffies;
	flow->ethernet_bytes = data->ethernet_bytes;
	flow->cos = data->action.cos;
	flow->cos_set = data->cos_set;
	flow->protocol = data->protocol;
	flow->pppoe = data->egress_pppoe;
	flow->ingress_session = data->pppoe_session;
	flow->ethernet_protocol = data->ethernet_protocol;
	ether_addr_copy(flow->ethernet_source, data->source);

	__set_bit(index, fe->hashes);
	ret = rhashtable_insert_fast(&fe->cookies, &flow->node, rtl_fe_flow_params);
	if (ret) {
		__clear_bit(index, fe->hashes);
		kfree(flow);
		return ret;
	}
	flow->hashed = true;
	list_add_tail(&flow->list, &fe->flows);

	if (data->kind != RTL_FE_RULE_BRIDGE) {
		ret = rtl_fe_smac_get(fe, data->source, &flow->source_mac);
		if (ret)
			goto rollback;
		flow->source_owned = true;
		ret = rtl_fe_destination_get(fe, flow, data->destination);
		if (ret)
			goto rollback;
		if (data->tunnel_type) {
			ret = rtl_fe_install_tunnel(fe, data, flow, &tunnel);
			if (ret)
				goto rollback;
		} else {
			ret = rtl_fe_interface_get(fe, flow);
			if (ret)
				goto rollback;
		}
	} else {
		ether_addr_copy(flow->destination, data->destination);
	}

	if (data->mtu) {
		ret = rtl_fe_mtu_get(fe, flow, data->mtu);
		if (ret)
			goto rollback;
		data->action.mtu_set = 1;
		data->action.mtu_index = flow->mtu_index;
	}

	if (flow->counted) {
		ret = rtl_fe_counter_read(fe, flow->counter, counters);
		if (ret)
			goto rollback;
		flow->bytes = (u64)counters[1] << 32 | counters[0];
		flow->packets = counters[2] + counters[3];
	}

	if (data->police) {
		ret = rtl_fe_police_alloc(fe->te, data->police_kbps,
					  data->police_burst, &flow->pol_slot,
					  &flow->pol_owned);
		if (ret)
			goto rollback;
	}

	/* Host Policing fallback for L3 flows without an explicit police action. */
	if (!flow->pol_owned && fe->te && data->kind != RTL_FE_RULE_BRIDGE)
		rtl_fe_resolve_hostpol_source(data);
	ether_addr_copy(flow->hostpol_source, data->source);
	ret = rtl_fe_police_bind(fe, data, flow);
	if (ret)
		goto rollback;

	data->action.mac_da_index = flow->destination_index;
	data->action.egress_interface =
		flow->tunnel_owned && !data->tunnel_decap ?
		tunnel.hash_eif : flow->counter;
	data->action.ingress_interface =
		flow->tunnel_owned && data->tunnel_decap ?
		tunnel.hash_eif : 0;
	if (flow->tunnel_owned && !data->tunnel_decap &&
	    data->tunnel_type == RTL_FE_DUAL_TYPE_IP4INIP6) {
		data->action.dip_remap_vld = 1;
		data->action.dip_remap = RTL_FE_DUAL_FMR_NONE;
	}
	data->action.mirror = READ_ONCE(fe->mirror_enabled);
	rtl_fe_action_build_mode(action, &data->action, fe->napt6_active);
	rtl_fe_action_set_mode(action, RTL_FE_L3_IF_COUNTER_EN, flow->counted,
			       fe->napt6_active);
	/* POL_VLD marks the action as carrying a policer, POL_EN makes it
	 * enforce.  pol_owned is an explicit FLOW_ACTION_POLICE slot, pol_bound a
	 * borrowed Host Policing slot; both mean "point POL_ID at pol_slot".
	 */
	if (flow->pol_owned || flow->pol_bound) {
		rtl_fe_action_set_mode(action, RTL_FE_POL_VLD, 1,
				       fe->napt6_active);
		rtl_fe_action_set_mode(action, RTL_FE_POL_EN, 1,
				       fe->napt6_active);
		rtl_fe_action_set_mode(action, RTL_FE_POL_ID, flow->pol_slot,
				       fe->napt6_active);
	}
	((__le32 *)fe->memory)[index] = cpu_to_le32(crc32);
	memcpy(fe->memory + RTL_FE_ACTION_OFFSET + index * RTL_FE_ACTION_BYTES,
	       action, sizeof(action));
	dma_wmb();
	flow->published = true;
	ret = rtl_fe_age_write(fe, index, 1);
	if (ret)
		goto rollback;

	flow->installed = true;
	fe->installs++;
	if (out)
		*out = flow;
	return 0;

rollback:
	if (rtl_fe_delete(fe, flow))
		fe->error = ret;
	return ret;
}

static int rtl_fe_replace(struct rtl_fe_port *port, struct flow_cls_offload *cls)
{
	struct rtl_fe_rule data = { };
	int ret;

	ret = rtl_fe_parse(port, cls, &data);
	if (ret)
		return ret;
	return rtl_fe_install(port, &data, cls->cookie, NULL);
}

static int rtl_fe_stats(struct rtl_fe *fe, struct rtl_fe_flow *flow,
			struct flow_cls_offload *cls)
{
	u32 words[RTL_FE_COUNTER_WORDS], packets, packet_delta;
	unsigned int age;
	u64 bytes, byte_delta;
	int ret;

	if (!flow->published)
		return -ENOENT;

	ret = rtl_fe_age_read(fe, flow->hash_index, &age);
	if (ret)
		return ret;
	if (age > 1) {
		ret = rtl_fe_age_write(fe, flow->hash_index, 1);
		if (ret)
			return ret;
		flow->lastused = jiffies;
	}

	if (!flow->counted) {
		flow_stats_update(&cls->stats, 0, 0, 0, flow->lastused,
				  FLOW_ACTION_HW_STATS_DELAYED);
		return 0;
	}

	ret = rtl_fe_counter_read(fe, flow->counter, words);
	if (ret)
		return ret;
	bytes = (u64)words[1] << 32 | words[0];
	packets = words[2] + words[3];
	if (bytes != flow->bytes || packets != flow->packets)
		flow->lastused = jiffies;
	byte_delta = bytes - flow->bytes;
	packet_delta = packets - flow->packets;
	flow_stats_update(&cls->stats,
			  byte_delta - (u64)packet_delta * flow->ethernet_bytes,
			  packet_delta, 0, flow->lastused,
			  FLOW_ACTION_HW_STATS_DELAYED);
	flow->bytes = bytes;
	flow->packets = packets;
	return 0;
}

static int rtl_fe_block_cb(enum tc_setup_type type, void *type_data, void *priv)
{
	struct flow_cls_offload *cls = type_data;
	struct rtl_fe_port *port = priv;
	struct rtl_fe *fe = port->fe;
	struct rtl_fe_flow *flow, *next;
	int ret;

	if (type != TC_SETUP_CLSFLOWER || cls->common.chain_index)
		return -EOPNOTSUPP;

	/* Filter non-matching ports before taking fe->lock: nf_flow_table
	 * broadcasts every tuple command across all bound flowtable ports.
	 */
	if (cls->command == FLOW_CLS_REPLACE) {
		struct flow_rule *rule = flow_cls_offload_flow_rule(cls);
		struct flow_match_meta meta;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META))
			return -EOPNOTSUPP;
		flow_rule_match_meta(rule, &meta);
		if (meta.key->ingress_ifindex != port->netdev->ifindex)
			return -EOPNOTSUPP;
	} else if (cls->command == FLOW_CLS_DESTROY ||
		   cls->command == FLOW_CLS_STATS) {
		bool owned;

		rcu_read_lock();
		flow = rhashtable_lookup(&fe->cookies, &cls->cookie,
					 rtl_fe_flow_params);
		owned = flow && READ_ONCE(flow->input) == port;
		rcu_read_unlock();
		if (!owned)
			return -ENOENT;
	} else {
		return -EOPNOTSUPP;
	}

	mutex_lock(&fe->lock);
	list_for_each_entry_safe(flow, next, &fe->retries, retry) {
		ret = rtl_fe_delete(fe, flow);
		if (ret)
			goto unlock;
	}

	flow = rtl_fe_flow_find(fe, cls->cookie);
	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		ret = rtl_fe_replace(port, cls);
		break;
	case FLOW_CLS_DESTROY:
		ret = flow && flow->input == port ? rtl_fe_delete(fe, flow) : -ENOENT;
		break;
	case FLOW_CLS_STATS:
		ret = flow && flow->input == port ? rtl_fe_stats(fe, flow, cls) : -ENOENT;
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	if (ret && ret != -EOPNOTSUPP && ret != -ENOENT && ret != -EEXIST)
		fe->misses++;
	rtl_fe_collect_ports(fe);
unlock:
	mutex_unlock(&fe->lock);
	return ret;
}

static int rtl_fe_flush_locked(struct rtl_fe *fe, struct rtl_fe_port *port)
{
	struct rtl_fe_flow *flow, *next;
	int ret;

	list_for_each_entry_safe(flow, next, &fe->flows, list) {
		if (flow->input != port && flow->output != port)
			continue;
		ret = rtl_fe_delete(fe, flow);
		if (ret)
			return ret;
	}
	return 0;
}

static void rtl_fe_block_release(void *priv)
{
	struct rtl_fe_port *port = priv;
	struct rtl_fe *fe = port->fe;
	int ret;

	mutex_lock(&fe->lock);
	port->bindings--;
	WRITE_ONCE(fe->bindings, fe->bindings - 1);
	if (!port->bindings) {
		ret = rtl_fe_flush_locked(fe, port);
		if (ret)
			fe->error = ret;
	}
	rtl_fe_collect_ports(fe);
	mutex_unlock(&fe->lock);
}

int rtl_fe_setup_tc(struct rtl_fe *fe, struct net_device *netdev,
		    enum tc_setup_type type, void *data)
{
	struct flow_block_offload *block = data;
	struct rtl_fe_port *port, *candidate;
	struct flow_block_cb *cb;
	int ret = 0;

	if (!fe || type != TC_SETUP_FT)
		return -EOPNOTSUPP;
	if (block->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	mutex_lock(&fe->lock);
	port = rtl_fe_port_find(fe, netdev);
	if (!port && block->command == FLOW_BLOCK_UNBIND) {
		list_for_each_entry(candidate, &fe->ports, list) {
			if (candidate->retired && candidate->netdev == netdev) {
				port = candidate;
				break;
			}
		}
	}
	if (!port) {
		ret = -ENODEV;
		goto unlock;
	}

	block->driver_block_list = &fe->blocks;
	cb = flow_block_cb_lookup(block->block, rtl_fe_block_cb, port);
	switch (block->command) {
	case FLOW_BLOCK_BIND:
		if (cb) {
			flow_block_cb_incref(cb);
			break;
		}
		if (!tc_can_offload(netdev)) {
			ret = -EOPNOTSUPP;
			break;
		}
		cb = flow_block_cb_alloc(rtl_fe_block_cb, port, port,
					 rtl_fe_block_release);
		if (IS_ERR(cb)) {
			ret = PTR_ERR(cb);
			break;
		}
		flow_block_cb_incref(cb);
		flow_block_cb_add(cb, block);
		list_add_tail(&cb->driver_list, &fe->blocks);
		port->bindings++;
		WRITE_ONCE(fe->bindings, fe->bindings + 1);
		break;
	case FLOW_BLOCK_UNBIND:
		if (!cb) {
			ret = -ENOENT;
			break;
		}
		if (!flow_block_cb_decref(cb)) {
			flow_block_cb_remove(cb, block);
			list_del(&cb->driver_list);
		}
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
unlock:
	mutex_unlock(&fe->lock);
	return ret;
}

int rtl_fe_add_port(struct rtl_fe *fe, struct net_device *netdev,
		    unsigned int source, unsigned int destination,
		    unsigned int llid)
{
	struct rtl_fe_port *port;

	if (!fe)
		return 0;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	port->fe = fe;
	port->netdev = netdev;
	port->source = source;
	port->destination = destination;
	port->llid = llid;

	mutex_lock(&fe->lock);
	list_add_tail(&port->list, &fe->ports);
	netdev->hw_features |= NETIF_F_HW_TC;
	netdev->features |= NETIF_F_HW_TC;
	mutex_unlock(&fe->lock);
	return 0;
}

void rtl_fe_remove_port(struct rtl_fe *fe, struct net_device *netdev)
{
	struct rtl_fe_port *port;
	struct rtl_fe_flow *flow, *next;
	int ret;

	if (!fe)
		return;

	mutex_lock(&fe->lock);
	port = rtl_fe_port_find(fe, netdev);
	if (port) {
		port->retired = true;
		list_for_each_entry_safe(flow, next, &fe->flows, list) {
			if (flow->input != port && flow->output != port)
				continue;
			ret = rtl_fe_delete(fe, flow);
			if (ret)
				fe->error = ret;
		}
		rtl_fe_collect_ports(fe);
	}
	mutex_unlock(&fe->lock);
}

int rtl_fe_flush_port(struct rtl_fe *fe, struct net_device *netdev)
{
	int ret;

	if (!fe)
		return 0;

	mutex_lock(&fe->lock);
	ret = rtl_fe_flush_locked(fe, rtl_fe_port_find(fe, netdev));
	if (ret)
		fe->error = ret;
	mutex_unlock(&fe->lock);
	return ret;
}

int rtl_fe_set_itu_paths(struct rtl_fe *fe, struct net_device *netdev,
			 const struct rtl_itu_paths *paths)
{
	struct rtl_itu_paths *copy = NULL;
	int ret;

	if (!fe)
		return paths ? -EOPNOTSUPP : 0;

	if (paths) {
		copy = kvmemdup(paths, rtl_itu_paths_bytes(paths), GFP_KERNEL);
		if (!copy)
			return -ENOMEM;
	}

	mutex_lock(&fe->lock);
	ret = rtl_fe_flush_locked(fe, rtl_fe_port_find(fe, netdev));
	if (!ret) {
		kvfree(fe->itu_paths);
		fe->itu_paths = copy;
		fe->itu_netdev = paths ? netdev : NULL;
		copy = NULL;
	}
	mutex_unlock(&fe->lock);
	kvfree(copy);
	return ret;
}

static int rtl_fe_disable_locked(struct rtl_fe *fe)
{
	struct rtl_fe_flow *flow, *next;
	unsigned int i;
	int ret = 0;

	WRITE_ONCE(fe->enabled, false);
	if (fe->prepared) {
		for (i = 0; i < FE_LAN_PORTS; i++)
			rtl_fe_write(fe, RTL_NI_PORT_RX_CTRL(i), fe->lan_saved[i]);
		rtl_fe_write(fe, RTL_NI_XGE_BYPASS_CFG, fe->xge_saved);
		usleep_range(1000, 2000);
		list_for_each_entry_safe(flow, next, &fe->flows, list) {
			ret = rtl_fe_delete(fe, flow);
			if (ret)
				return ret;
		}
		ret = rtl_fe_restore(fe);
	}
	rtl_fe_collect_ports(fe);
	return ret;
}

static int rtl_fe_enable_locked(struct rtl_fe *fe)
{
	struct rtl_fe_port *port;
	unsigned int i;
	int ret;

	if (fe->prepared) {
		ret = rtl_fe_disable_locked(fe);
		if (ret)
			return ret;
	}
	ret = rtl_fe_prepare(fe);
	if (ret) {
		rtl_fe_restore(fe);
		return ret;
	}

	for (i = 0; i < FE_LAN_PORTS; i++)
		rtl_fe_rmw(fe, RTL_NI_PORT_RX_CTRL(i),
			   RTL_NI_PORT_RX_CTRL_BYP_DPID,
			   FIELD_PREP(RTL_NI_PORT_RX_CTRL_BYP_DPID, 25));
	list_for_each_entry(port, &fe->ports, list) {
		if (port->retired)
			continue;
		if (port->source < FE_LAN_PORTS) {
			rtl_fe_rmw(fe, RTL_NI_PORT_RX_CTRL(port->source),
				   RTL_NI_PORT_RX_CTRL_BYP_EN |
				   RTL_NI_PORT_RX_CTRL_BYP_DPID,
				   RTL_NI_PORT_RX_CTRL_BYP_EN |
				   FIELD_PREP(RTL_NI_PORT_RX_CTRL_BYP_DPID, 25));
		} else if (port->source == FE_SOURCE_P6) {
			rtl_fe_rmw(fe, RTL_NI_XGE_BYPASS_CFG,
				   RTL_NI_XGE_BYPASS_FIELDS,
				   RTL_NI_XGE_BYPASS_EN |
				   FIELD_PREP(RTL_NI_XGE_BYPASS_DPID, 25));
		}
	}
	WRITE_ONCE(fe->enabled, true);
	return 0;
}

static int rtl_fe_flush_source_locked(struct rtl_fe *fe, unsigned int source)
{
	struct rtl_fe_port *port;
	int ret;

	list_for_each_entry(port, &fe->ports, list) {
		if (port->source != source)
			continue;
		ret = rtl_fe_flush_locked(fe, port);
		if (ret)
			return ret;
	}
	return 0;
}

int rtl_fe_pon_attach(struct rtl_fe *fe, bool attach)
{
	int ret = 0;

	if (!fe)
		return attach ? -EOPNOTSUPP : 0;

	mutex_lock(&fe->lock);
	fe->pon_attached = attach;
	if (!attach)
		ret = rtl_fe_flush_source_locked(fe, FE_SOURCE_PON);
	if (ret)
		fe->error = ret;
	mutex_unlock(&fe->lock);
	return ret;
}

static int rtl_fe_hostpol_rebind_flow(struct rtl_fe *fe,
				      struct rtl_fe_flow *flow)
{
	u32 words[RTL_FE_ACTION_MAX_WORDS];
	bool want_bound = false, want_host = false;
	u8 want_slot = 0;
	__le32 *slot;
	int ret;

	if (!flow->published || flow->deleting ||
	    flow->kind == RTL_FE_RULE_BRIDGE || flow->pol_owned || !fe->te)
		return 0;

	ret = rtl_fe_hostpol_decide(fe, flow->hostpol_source,
				    flow->destination, &want_slot);
	if (ret && ret != -ENOENT)
		return ret;
	if (!ret) {
		want_bound = true;
		want_host = true;
	} else {
		want_slot = 0;
	}

	if (flow->pol_bound == want_bound && flow->pol_host == want_host &&
	    flow->pol_slot == want_slot)
		return 0;

	slot = (__le32 *)(fe->memory + RTL_FE_ACTION_OFFSET +
			  flow->hash_index * RTL_FE_ACTION_BYTES);
	memcpy(words, slot, sizeof(words));
	rtl_fe_action_set_mode(words, RTL_FE_POL_VLD, want_bound,
			       fe->napt6_active);
	rtl_fe_action_set_mode(words, RTL_FE_POL_EN, want_bound,
			       fe->napt6_active);
	rtl_fe_action_set_mode(words, RTL_FE_POL_ID, want_slot,
			       fe->napt6_active);
	memcpy(slot, words, sizeof(words));
	dma_wmb();

	flow->pol_bound = want_bound;
	flow->pol_host = want_host;
	flow->pol_slot = want_slot;

	ret = rtl_fe_cache_command(fe, flow->hash_index, flow->crc16, 1);
	if (ret)
		return ret;
	return rtl_fe_age_write(fe, flow->hash_index, 1);
}

void rtl_fe_hostpol_sync(struct rtl_fe *fe)
{
	struct rtl_fe_flow *flow;
	int ret;

	if (!fe)
		return;

	mutex_lock(&fe->lock);
	if (!fe->enabled)
		goto unlock;
	list_for_each_entry(flow, &fe->flows, list) {
		ret = rtl_fe_hostpol_rebind_flow(fe, flow);
		if (ret) {
			fe->error = ret;
			break;
		}
	}
unlock:
	mutex_unlock(&fe->lock);
}

void rtl_fe_start(struct rtl_fe *fe)
{
	int ret;

	if (!fe)
		return;

	mutex_lock(&fe->lock);
	ret = rtl_fe_enable_locked(fe);
	fe->error = ret;
	mutex_unlock(&fe->lock);
	if (ret)
		dev_err(fe->dev, "failed to start flow engine: %d\n", ret);
}

void rtl_fe_stop(struct rtl_fe *fe)
{
	int ret;

	if (!fe)
		return;

	mutex_lock(&fe->lock);
	ret = rtl_fe_disable_locked(fe);
	fe->error = ret;
	mutex_unlock(&fe->lock);
	if (ret)
		dev_err(fe->dev, "failed to stop flow engine: %d\n", ret);
}

int rtl_fe_napt6_reload(struct rtl_fe *fe)
{
	bool want = rtl_fe_napt6_mode_enabled();
	int ret = 0;

	if (!fe)
		return 0;

	mutex_lock(&fe->lock);
	if (fe->enabled && fe->napt6_active != want) {
		ret = rtl_fe_enable_locked(fe);
		fe->error = ret;
	}
	mutex_unlock(&fe->lock);
	if (ret)
		dev_err(fe->dev, "failed to switch NAPT6 mode (%u): %d\n",
			want, ret);
	return ret;
}

static int rtl_fe_state_print(struct rtl_fe *fe, char *buffer, size_t size)
{
	unsigned int flows = 0, counted = 0, learned = 0;
	struct rtl_fe_flow *flow;
	int len;

	mutex_lock(&fe->lock);
	list_for_each_entry(flow, &fe->flows, list) {
		if (!flow->published)
			continue;
		flows++;
		counted += flow->counted;
		learned += flow->learned;
	}
	len = scnprintf(buffer, size,
			"available=1 enabled=%u prepared=%u pon_attached=%u bindings=%u error=%d flows=%u hash_entries=%u counter_slots=%u counted_flows=%u activity_flows=%u installs=%llu deletes=%llu rejected=%llu bridge_flows=%u\n",
			fe->enabled, fe->prepared, fe->pon_attached, fe->bindings,
			fe->error, flows, RTL_FE_HASH_ENTRIES, FE_COUNTERS,
			counted, flows - counted, fe->installs, fe->deletes,
			fe->misses, learned);
	mutex_unlock(&fe->lock);
	return len;
}

ssize_t rtl_fe_state(struct rtl_fe *fe, char *buffer)
{
	if (!fe)
		return sysfs_emit(buffer, "available=0\n");
	return rtl_fe_state_print(fe, buffer, PAGE_SIZE);
}

static int rtl_fe_state_show(struct seq_file *s, void *unused)
{
	char buffer[384];

	rtl_fe_state_print(s->private, buffer, sizeof(buffer));
	seq_puts(s, buffer);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl_fe_state);

static int rtl_fe_flows_show(struct seq_file *s, void *unused)
{
	static const char * const kind[] = { "ipv4", "ipv6", "bridge" };
	struct rtl_fe *fe = s->private;
	struct rtl_fe_flow *flow;

	mutex_lock(&fe->lock);
	list_for_each_entry(flow, &fe->flows, list) {
		seq_printf(s, "%s cookie=%lx in=%s out=%s hash=%u counter=%u cos=%u cos_set=%u proto=%u packets=%u bytes=%llu learned=%u deleting=%u pol=%d host=%u",
			   kind[flow->kind], flow->cookie,
			   flow->input->retired ? "(retired)" : flow->input->netdev->name,
			   flow->output->retired ? "(retired)" : flow->output->netdev->name,
			   flow->hash_index, flow->counter, flow->cos,
			   flow->cos_set, flow->protocol, flow->packets,
			   flow->bytes, flow->learned, flow->deleting,
			   rtl_fe_flow_pol_id(flow), flow->pol_host);
		if (flow->mcast)
			seq_printf(s, " mce=%u", flow->mcast_hw_index);
		if (flow->tunnel_owned)
			seq_printf(s, " dual=%u", flow->dual_index);
		seq_putc(s, '\n');
	}
	mutex_unlock(&fe->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl_fe_flows);

static int rtl_fe_resources_show(struct seq_file *s, void *unused)
{
	unsigned int i, smac = 0, intf = 0, mtu = 0;
	struct rtl_fe *fe = s->private;

	mutex_lock(&fe->lock);
	for (i = 0; i < FE_SOURCE_MACS; i++)
		smac += !!fe->source_macs[i].users;
	for (i = 0; i < FE_INTERFACES; i++)
		intf += !!fe->interfaces[i].users;
	for (i = 0; i < FE_MTUS; i++)
		mtu += !!fe->mtus[i].users;
	seq_printf(s, "source_mac=%u/%u\n", smac, FE_SOURCE_MACS);
	seq_printf(s, "interface=%u/%u\n", intf, FE_INTERFACES);
	seq_printf(s, "mtu=%u/%u\n", mtu, FE_MTUS);
	seq_printf(s, "hash=%u/%u\n", bitmap_weight(fe->hashes, RTL_FE_HASH_ENTRIES),
		   RTL_FE_HASH_ENTRIES);
	seq_printf(s, "counted=%u/%u\n", bitmap_weight(fe->counters, FE_INTERFACES),
		   FE_COUNTERS);
	mutex_unlock(&fe->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtl_fe_resources);

/* Re-apply the per-flow MRR_VLD/MRR_EN bits on every published L3 entry.
 * A flow's action words live in the shared DMA action table, so toggling the
 * global mirror has to walk the live entries; there is no per-flow state to
 * unwind on delete because the bits disappear with the action slot.
 */
static int rtl_fe_mirror_flows(struct rtl_fe *fe, bool enable)
{
	struct rtl_fe_flow *flow;
	int ret;

	list_for_each_entry(flow, &fe->flows, list) {
		u32 words[RTL_FE_ACTION_MAX_WORDS];
		__le32 *slot;

		if (!flow->published || flow->kind == RTL_FE_RULE_BRIDGE)
			continue;
		slot = (__le32 *)(fe->memory + RTL_FE_ACTION_OFFSET +
				  flow->hash_index * RTL_FE_ACTION_BYTES);
		memcpy(words, slot, sizeof(words));
		rtl_fe_action_set_mode(words, RTL_FE_MRR_VLD, enable,
				       fe->napt6_active);
		rtl_fe_action_set_mode(words, RTL_FE_MRR_EN, enable,
				       fe->napt6_active);
		memcpy(slot, words, sizeof(words));
		dma_wmb();
		/* Same re-arm the install path uses so the entry is reloaded. */
		ret = rtl_fe_age_write(fe, flow->hash_index, 1);
		if (ret)
			return ret;
	}
	return 0;
}

static int rtl_fe_mirror_update(struct rtl_fe *fe, bool enable,
				unsigned int arg0, unsigned int arg1,
				unsigned int arg2, unsigned int len)
{
	int ret;

	if (arg0 > RTL_FE_MIRROR_ARG0_MAX || arg1 > RTL_FE_MIRROR_ARG1_MAX ||
	    arg2 > RTL_FE_MIRROR_ARG2_MAX || len > RTL_FE_MIRROR_LEN_MAX)
		return -EINVAL;
	/*
	 * A zero length would very likely truncate the mirrored copy to nothing
	 * (the field is the only length-like field the OEM exposes), so enabling
	 * without an explicit length falls back to the maximum instead of
	 * silently producing empty copies.  The real meaning of the field is
	 * still the open question recorded in fe_hw.h.
	 */
	if (enable && !len)
		len = RTL_FE_MIRROR_LEN_MAX;

	mutex_lock(&fe->lock);
	fe->mirror_enabled = enable;
	fe->mirror_arg0 = arg0;
	fe->mirror_arg1 = arg1;
	fe->mirror_arg2 = arg2;
	fe->mirror_len = len;
	/*
	 * A disabled mirror clears both registers, so no destination or length
	 * survives into the next enable.
	 */
	rtl_fe_mirror_apply(fe, enable, fe->mirror_arg0, fe->mirror_arg1,
			    fe->mirror_arg2, fe->mirror_len);
	ret = rtl_fe_mirror_flows(fe, enable);
	mutex_unlock(&fe->lock);
	return ret;
}

static int rtl_fe_mirror_show(struct seq_file *s, void *unused)
{
	struct rtl_fe *fe = s->private;

	mutex_lock(&fe->lock);
	seq_printf(s,
		   "enabled=%u arg0=%u arg1=%u arg2=%u len=%u ctrl=0x%08x mirror_len=0x%08x\n",
		   fe->mirror_enabled, fe->mirror_arg0, fe->mirror_arg1,
		   fe->mirror_arg2, fe->mirror_len,
		   rtl_fe_read(fe, RTL_FE_MIRROR_CTRL),
		   rtl_fe_read(fe, RTL_FE_MIRROR_LEN));
	mutex_unlock(&fe->lock);
	return 0;
}

static char *rtl_fe_token(char **cursor)
{
	char *token, *p = *cursor;

	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p) {
		*cursor = p;
		return NULL;
	}
	token = p;
	while (*p && *p != ' ' && *p != '\t')
		p++;
	if (*p)
		*p++ = '\0';
	*cursor = p;
	return token;
}

/* Usage, one command per write:
 *   raw <arg0 6bit> <arg1 3bit> <arg2 8bit>   program all three fields
 *   port|enable <arg0>                        convenience: arg0 only
 *   len <bytes 12bit>                         mirror copy length
 *   disable                                   clear both registers
 * The three fields are the OEM bit fields verbatim; their meaning is not
 * recoverable from ca-ne.ko (orphaned API), so "port" only claims to write
 * arg0 and nothing here assigns a semantic to arg1/arg2.
 */
static ssize_t rtl_fe_mirror_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct rtl_fe *fe = s->private;
	char buf[80], *cursor, *cmd, *a0, *a1, *a2;
	unsigned int arg0, arg1, arg2, len;
	int ret;

	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	cursor = buf;
	cmd = rtl_fe_token(&cursor);
	if (!cmd)
		return -EINVAL;

	if (sysfs_streq(cmd, "disable")) {
		ret = rtl_fe_mirror_update(fe, false, 0, 0, 0, 0);
	} else if (sysfs_streq(cmd, "raw")) {
		a0 = rtl_fe_token(&cursor);
		a1 = rtl_fe_token(&cursor);
		a2 = rtl_fe_token(&cursor);
		if (!a0 || !a1 || !a2 || kstrtouint(a0, 0, &arg0) ||
		    kstrtouint(a1, 0, &arg1) || kstrtouint(a2, 0, &arg2))
			return -EINVAL;
		ret = rtl_fe_mirror_update(fe, true, arg0, arg1, arg2,
					   READ_ONCE(fe->mirror_len));
	} else if (sysfs_streq(cmd, "port") || sysfs_streq(cmd, "enable")) {
		a0 = rtl_fe_token(&cursor);
		if (!a0 || kstrtouint(a0, 0, &arg0))
			return -EINVAL;
		/* Mirroring live L3FE flows to CPU LDPIDs (16..23) feeds
		 * mirrored frames back into the Linux RX path and can trigger
		 * a forwarding loop / CPU storm.
		 */
		if (arg0 >= 16 && arg0 <= 23)
			return -EINVAL;
		ret = rtl_fe_mirror_update(fe, true, arg0,
					   READ_ONCE(fe->mirror_arg1),
					   READ_ONCE(fe->mirror_arg2),
					   READ_ONCE(fe->mirror_len));
	} else if (sysfs_streq(cmd, "len")) {
		a0 = rtl_fe_token(&cursor);
		if (!a0 || kstrtouint(a0, 0, &len))
			return -EINVAL;
		ret = rtl_fe_mirror_update(fe, READ_ONCE(fe->mirror_enabled),
					   READ_ONCE(fe->mirror_arg0),
					   READ_ONCE(fe->mirror_arg1),
					   READ_ONCE(fe->mirror_arg2), len);
	} else {
		return -EINVAL;
	}

	return ret ? ret : count;
}

static int rtl_fe_mirror_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl_fe_mirror_show, inode->i_private);
}

static const struct file_operations rtl_fe_mirror_fops = {
	.owner = THIS_MODULE,
	.open = rtl_fe_mirror_open,
	.read = seq_read,
	.write = rtl_fe_mirror_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void rtl_fe_debugfs_init(struct rtl_fe *fe)
{
	fe->debugfs = debugfs_create_dir(dev_name(fe->dev), NULL);
	debugfs_create_file("state", 0400, fe->debugfs, fe, &rtl_fe_state_fops);
	debugfs_create_file("flows", 0400, fe->debugfs, fe, &rtl_fe_flows_fops);
	debugfs_create_file("resources", 0400, fe->debugfs, fe,
			    &rtl_fe_resources_fops);
	debugfs_create_file("mirror", 0600, fe->debugfs, fe,
			    &rtl_fe_mirror_fops);
	rtl_fe_mcast_debugfs_init(fe);
	rtl_fe_tunnel_debugfs_init(fe);
	rtl_fe_napt6_debugfs_init(fe);
}

static int rtl_fe_netdev_event(struct notifier_block *nb, unsigned long event,
			       void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct rtl_fe *fe = container_of(nb, struct rtl_fe, netdev_nb);
	struct rtl_fe_port *port;
	int ret;

	if (event != NETDEV_FEAT_CHANGE || tc_can_offload(dev))
		return NOTIFY_DONE;

	mutex_lock(&fe->lock);
	port = rtl_fe_port_find(fe, dev);
	if (port) {
		ret = rtl_fe_flush_locked(fe, port);
		if (ret)
			fe->error = ret;
	}
	mutex_unlock(&fe->lock);
	return NOTIFY_DONE;
}

static void rtl_fe_axi_reorder(struct rtl_fe *fe)
{
	writel(15, fe->axi + FE_REORDER_CTRL);
	writel(BIT(31) | 8, fe->axi + FE_REORDER_USER);
	writel(BIT(28), fe->axi + FE_REORDER_ATTR);
	writel(BIT(28), fe->axi + FE_REORDER_ATTR2);
	writel(BIT(31) | 9, fe->axi + FE_REORDER_ID);
	readl(fe->axi + FE_REORDER_ID);
}

struct rtl_fe *rtl_fe_create(struct device *dev, struct regmap *regs,
			     void __iomem *axi, bool lan_deepq,
			     struct rtl_te_flow *te)
{
	struct rtl_fe *fe;
	u32 *defaults;
	unsigned int i;
	int ret;

	fe = kzalloc(sizeof(*fe), GFP_KERNEL);
	if (!fe)
		return ERR_PTR(-ENOMEM);
	fe->dev = dev;
	fe->regs = regs;
	fe->axi = axi;
	fe->lan_deepq = lan_deepq;
	fe->te = te;
	mutex_init(&fe->lock);
	INIT_DELAYED_WORK(&fe->cleanup_work, rtl_fe_cleanup_work);
	INIT_LIST_HEAD(&fe->ports);
	INIT_LIST_HEAD(&fe->flows);
	INIT_LIST_HEAD(&fe->retries);
	INIT_LIST_HEAD(&fe->bridge_flows);
	INIT_LIST_HEAD(&fe->dest_macs);
	INIT_LIST_HEAD(&fe->blocks);

	fe->saved = kcalloc(ARRAY_SIZE(rtl_fe_saved_regs), sizeof(*fe->saved),
			    GFP_KERNEL);
	if (!fe->saved) {
		ret = -ENOMEM;
		goto free_fe;
	}

	ret = rhashtable_init(&fe->cookies, &rtl_fe_flow_params);
	if (ret)
		goto free_saved;

	fe->memory = dma_alloc_coherent(dev, RTL_FE_MEMORY_BYTES, &fe->dma,
					GFP_KERNEL);
	if (!fe->memory) {
		ret = -ENOMEM;
		goto destroy_hash;
	}
	defaults = fe->memory + RTL_FE_DEFAULT_OFFSET;
	for (i = 0; i < RTL_FE_DEFAULT_ENTRIES; i++)
		rtl_fe_cpu_miss(defaults + i * 4);
	dma_wmb();

	rtl_fe_axi_reorder(fe);
	/*
	 * The multicast table manager is best effort: a hardware table that is
	 * not ready (or a failed allocation) must not block FE creation.
	 */
	ret = rtl_fe_mcast_init(fe);
	if (ret)
		dev_warn(dev, "multicast table manager unavailable: %d\n", ret);
	/* Same best-effort contract for the dual-header tables: FE keeps
	 * working when the OEM table block is not usable.
	 */
	ret = rtl_fe_tunnel_init(fe);
	if (ret)
		dev_warn(dev, "dual-header table manager unavailable: %d\n",
			 ret);
	/* NAPT6 action/NPTv6 prefix manager; a failed allocation must not block FE. */
	ret = rtl_fe_napt6_init(fe);
	if (ret)
		dev_warn(dev, "NAPT6 manager unavailable: %d\n", ret);

	fe->bridge = rtl_fe_bridge_start(fe);
	if (IS_ERR(fe->bridge)) {
		ret = PTR_ERR(fe->bridge);
		fe->bridge = NULL;
		goto exit_subsystems;
	}

	fe->netdev_nb.notifier_call = rtl_fe_netdev_event;
	ret = register_netdevice_notifier(&fe->netdev_nb);
	if (ret)
		goto stop_bridge;

	rtl_fe_debugfs_init(fe);
	return fe;

stop_bridge:
	rtl_fe_bridge_stop(fe->bridge);
exit_subsystems:
	rtl_fe_napt6_exit(fe);
	rtl_fe_tunnel_exit(fe);
	rtl_fe_mcast_exit(fe);
	dma_free_coherent(dev, RTL_FE_MEMORY_BYTES, fe->memory, fe->dma);
destroy_hash:
	rhashtable_destroy(&fe->cookies);
free_saved:
	kfree(fe->saved);
free_fe:
	kfree(fe);
	return ERR_PTR(ret);
}

void rtl_fe_destroy(struct rtl_fe *fe)
{
	struct rtl_fe_dest_mac *dm, *dm_next;
	struct rtl_fe_flow *flow, *flow_next;
	struct rtl_fe_port *port, *next;

	if (!fe)
		return;

	debugfs_remove_recursive(fe->debugfs);
	unregister_netdevice_notifier(&fe->netdev_nb);
	rtl_fe_bridge_stop(fe->bridge);
	mutex_lock(&fe->lock);
	rtl_fe_disable_locked(fe);
	mutex_unlock(&fe->lock);
	rtl_fe_napt6_exit(fe);
	cancel_delayed_work_sync(&fe->cleanup_work);
	if (fe->mirror_enabled) {
		mutex_lock(&fe->lock);
		fe->mirror_enabled = false;
		rtl_fe_mirror_apply(fe, false, 0, 0, 0, 0);
		mutex_unlock(&fe->lock);
	}
	/*
	 * Flows are stopped above; release every remaining MCGID through the
	 * reverse-order teardown (ARB MC FIB -> MCE -> MCGID).
	 */
	rtl_fe_mcast_exit(fe);
	rtl_fe_tunnel_exit(fe);
	list_for_each_entry_safe(flow, flow_next, &fe->flows, list) {
		list_del_init(&flow->retry);
		list_del_init(&flow->bridge_list);
		list_del(&flow->list);
		kfree(flow);
	}
	list_for_each_entry_safe(dm, dm_next, &fe->dest_macs, list) {
		list_del(&dm->list);
		kfree(dm);
	}
	rhashtable_destroy(&fe->cookies);
	rcu_barrier();
	list_for_each_entry_safe(port, next, &fe->ports, list) {
		list_del(&port->list);
		kfree(port);
	}
	dma_free_coherent(fe->dev, RTL_FE_MEMORY_BYTES, fe->memory, fe->dma);
	kvfree(fe->itu_paths);
	kfree(fe->saved);
	kfree(fe);
}
