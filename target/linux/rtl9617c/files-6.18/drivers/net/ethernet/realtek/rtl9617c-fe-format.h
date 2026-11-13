/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef RTL9617C_FE_FORMAT_H
#define RTL9617C_FE_FORMAT_H

#define RTL_FE_ACTION_WORDS 8

enum rtl_fe_rule_kind {
	RTL_FE_RULE_IPV4,
	RTL_FE_RULE_IPV6,
	RTL_FE_RULE_BRIDGE,
};
#define RTL_FE_ACTION_GROUPS 0x00140000U

enum rtl_fe_action_field {
	RTL_FE_MRR_VLD,
	RTL_FE_MRR_EN,
	RTL_FE_NO_DROP_VLD,
	RTL_FE_NO_DROP,
	RTL_FE_DPID_VLD,
	RTL_FE_DPID_PRI,
	RTL_FE_PERMIT,
	RTL_FE_DEEPQ,
	RTL_FE_MCGID,
	RTL_FE_MC,
	RTL_FE_MDATA_BYTE_VLD,
	RTL_FE_MDATA_BYTE,
	RTL_FE_L3_IF_VLD1,
	RTL_FE_SMAC_TRANS,
	RTL_FE_IGR_L3_IF_IDX1,
	RTL_FE_EGR_L3_IF_IDX1,
	RTL_FE_L3_IF_COUNTER_EN,
	RTL_FE_IP_TTL_DEC1,
	RTL_FE_IP_TTL_ZERO_DISCARD_EN1,
	RTL_FE_IP_ADDR_VLD,
	RTL_FE_IP_TYPE,
	RTL_FE_IP_ADDR,
	RTL_FE_IP_ADDR_NAPT6,
	RTL_FE_MAC_DA_IDX_VLD,
	RTL_FE_MAC_DA_IDX,
	RTL_FE_CHK_MSK_PTR1,
	RTL_FE_CACHE_CTRL1,
	RTL_FE_POP_L3_VLD1,
	RTL_FE_POP_L3_CHK_ECN_EN1,
	RTL_FE_POP_L3_EN1,
	RTL_FE_T2_CTRL_VLD1,
	RTL_FE_T2_CTRL1,
	RTL_FE_LDPID_OFFSET_MSB,
	RTL_FE_IP_DSCP_UPDATE_EN1,
	RTL_FE_IP_DSCP1,
	RTL_FE_COS_UPDATE_EN,
	RTL_FE_COS1,
	RTL_FE_INNER_802_1P_UPDATE_EN,
	RTL_FE_INNER_802_1P1,
	RTL_FE_TOP_802_1P_UPDATE_EN,
	RTL_FE_TOP_802_1P1,
	RTL_FE_INNER_DEI1,
	RTL_FE_INNER_VID1,
	RTL_FE_INNER_TPID_ENC1,
	RTL_FE_TOP_DEI1,
	RTL_FE_TOP_VID1,
	RTL_FE_TOP_TPID_ENC1,
	RTL_FE_VLAN_CNT1,
	RTL_FE_VLAN_VLD1,
	RTL_FE_POL_VLD1,
	RTL_FE_POL_EN1,
	RTL_FE_POL_ID1,
	RTL_FE_POL2_ID_ENABLE1,
	RTL_FE_POL2_ID1,
	RTL_FE_POL3_ID_ENABLE1,
	RTL_FE_POL3_ID1,
	RTL_FE_PPPOE_VLD1,
	RTL_FE_PPPOE_SET1,
	RTL_FE_L4_PORT,
	RTL_FE_IP_MTU_ENC_VLD1,
	RTL_FE_IP_MTU_ENC1,
	RTL_FE_MODIFY_VLAN_ONLY_VLD,
	RTL_FE_MODIFY_VLAN_ONLY,
	RTL_FE_SIX_RD_DIP_REMAP_OR_FMR_IDX_VLD,
	RTL_FE_SIX_RD_DIP_REMAP_OR_FMR_IDX,
	RTL_FE_VXLAN_SPORT_MSB15,
	RTL_FE_VXLAN_SPORT_UPDATE_EN,
	RTL_FE_FIELD_COUNT,
};

struct rtl_fe_field_layout { unsigned short bit; unsigned char width; };

struct rtl_fe_vlan {
	unsigned short vid;
	unsigned char tpid, priority, dei;
};
struct rtl_fe_ipv4_action {
	unsigned int address;
	unsigned short port, destination, mac_da_index;
	unsigned char ingress_interface, egress_interface, cos, check_mask;
	unsigned char translate, translate_destination, deep_queue;
	unsigned char vlan_set, vlan_count;
	unsigned char pppoe_pop, mtu_set, mtu_index;
	unsigned char gem_set, gem_index;
	unsigned char bridge;
	struct rtl_fe_vlan vlan[2]; 
};

#define RTL_FE_KEY_WORDS 23
enum rtl_fe_key_field {
	RTL_FE_KEY_L4_CHKSUM_ZERO,
	RTL_FE_KEY_TCP_RDP_CTRL,
	RTL_FE_KEY_L4_DP_EXACT_RANGE,
	RTL_FE_KEY_L4_SP_EXACT_RANGE,
	RTL_FE_KEY_L3_CHKSUM_ERR,
	RTL_FE_KEY_IP_FRAGMENT_FLAG,
	RTL_FE_KEY_IP_OPTIONS,
	RTL_FE_KEY_IP_DA_0,
	RTL_FE_KEY_IP_DA_1,
	RTL_FE_KEY_IP_DA_2,
	RTL_FE_KEY_IP_DA_3,
	RTL_FE_KEY_IP_SA_0,
	RTL_FE_KEY_IP_SA_1,
	RTL_FE_KEY_IP_SA_2,
	RTL_FE_KEY_IP_SA_3,
	RTL_FE_KEY_IP_PROTOCOL,
	RTL_FE_KEY_IP_VER,
	RTL_FE_KEY_IP_VLD,
	RTL_FE_KEY_PPPOE_SESSION_ID,
	RTL_FE_KEY_PPPOE_TYPE,
	RTL_FE_KEY_INNER_VID,
	RTL_FE_KEY_INNER_TPID_ENC,
	RTL_FE_KEY_TOP_VID,
	RTL_FE_KEY_TOP_TPID_ENC,
	RTL_FE_KEY_VLAN_CNT,
	RTL_FE_KEY_POL_ID,
	RTL_FE_KEY_ORIG_LSPID,
	RTL_FE_KEY_LSPID,
	RTL_FE_KEY_HKEY_ID,
	RTL_FE_KEY_CTRL_SET_ID,
	RTL_FE_KEY_TABLE_ID,
	RTL_FE_KEY_MDATA_BYTE,
	RTL_FE_KEY_ETHERTYPE,
	RTL_FE_KEY_FIELD_COUNT,
};

struct rtl_fe_ipv4_key {
	unsigned int source, destination;
	unsigned short source_port, destination_port, llid;
	unsigned char protocol, ingress, profile, mask;
};

struct rtl_fe_ipv6_key {
	unsigned int source[4],destination[4];
	unsigned short source_port,destination_port,llid;
	unsigned char protocol,ingress,profile,mask;
};

#define RTL_FE_MASK_WORDS 8

#define RTL_FE_HEADER_WORDS 32

#include "rtl9617c-fe-hw.h"

#endif
