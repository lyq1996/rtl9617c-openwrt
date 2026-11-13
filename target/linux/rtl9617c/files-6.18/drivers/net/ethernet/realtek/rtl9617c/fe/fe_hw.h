/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL9617C_FE_HW_H
#define RTL9617C_FE_HW_H

#include <linux/types.h>

#define RTL_FE_ACTION_WORDS		8
#define RTL_FE_KEY_WORDS		23
#define RTL_FE_MASK_WORDS		8
#define RTL_FE_CLASSIFIER_WORDS		7
#define RTL_FE_LPB_WORDS		3
#define RTL_FE_FDB_WORDS		4
#define RTL_FE_SMAC_WORDS		5
#define RTL_FE_COUNTER_WORDS		7
#define RTL_FE_CAM_WORDS		5

#define RTL_FE_HASH_ENTRIES		65536
#define RTL_FE_HASH_BUCKET		32
#define RTL_FE_ACTION_BYTES		48
#define RTL_FE_DEFAULT_ENTRIES		32
#define RTL_FE_OVERFLOW_ENTRIES		64
#define RTL_FE_HASH_BYTES		(RTL_FE_HASH_ENTRIES * 4)
#define RTL_FE_ACTION_OFFSET		RTL_FE_HASH_BYTES
#define RTL_FE_OVERFLOW_OFFSET		(RTL_FE_ACTION_OFFSET + \
					 RTL_FE_HASH_ENTRIES * RTL_FE_ACTION_BYTES)
#define RTL_FE_DEFAULT_OFFSET		(RTL_FE_OVERFLOW_OFFSET + \
					 RTL_FE_OVERFLOW_ENTRIES * RTL_FE_ACTION_BYTES)
#define RTL_FE_MEMORY_BYTES		ALIGN(RTL_FE_DEFAULT_OFFSET + \
					      RTL_FE_DEFAULT_ENTRIES * 16, 4096)

#define RTL_FE_MASK_L3			63
#define RTL_FE_MASK_BRIDGE		61
#define RTL_FE_MASK_LEARN		59
#define RTL_FE_MASK_LEARN_PPPOE		57

#define RTL_FE_ACTION_GROUPS		0x00140000U

#define RTL_FE_KEY_IP_WORD_BITS		32
#define RTL_FE_KEY_MAC_SA_BIT		497
#define RTL_FE_KEY_MAC_DA_BIT		552

enum rtl_fe_rule_kind {
	RTL_FE_RULE_IPV4,
	RTL_FE_RULE_IPV6,
	RTL_FE_RULE_BRIDGE,
};

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
	RTL_FE_L3_IF_VLD,
	RTL_FE_SMAC_TRANS,
	RTL_FE_IGR_L3_IF_IDX,
	RTL_FE_EGR_L3_IF_IDX,
	RTL_FE_L3_IF_COUNTER_EN,
	RTL_FE_IP_TTL_DEC,
	RTL_FE_IP_TTL_ZERO_DISCARD_EN,
	RTL_FE_IP_ADDR_VLD,
	RTL_FE_IP_TYPE,
	RTL_FE_IP_ADDR,
	RTL_FE_IP_ADDR_NAPT6,
	RTL_FE_MAC_DA_IDX_VLD,
	RTL_FE_MAC_DA_IDX,
	RTL_FE_CHK_MSK_PTR,
	RTL_FE_CACHE_CTRL,
	RTL_FE_POP_L3_VLD,
	RTL_FE_POP_L3_CHK_ECN_EN,
	RTL_FE_POP_L3_EN,
	RTL_FE_T2_CTRL_VLD,
	RTL_FE_T2_CTRL,
	RTL_FE_LDPID_OFFSET_MSB,
	RTL_FE_IP_DSCP_UPDATE_EN,
	RTL_FE_IP_DSCP,
	RTL_FE_COS_UPDATE_EN,
	RTL_FE_COS,
	RTL_FE_INNER_8021P_UPDATE_EN,
	RTL_FE_INNER_8021P,
	RTL_FE_TOP_8021P_UPDATE_EN,
	RTL_FE_TOP_8021P,
	RTL_FE_INNER_DEI,
	RTL_FE_INNER_VID,
	RTL_FE_INNER_TPID_ENC,
	RTL_FE_TOP_DEI,
	RTL_FE_TOP_VID,
	RTL_FE_TOP_TPID_ENC,
	RTL_FE_VLAN_CNT,
	RTL_FE_VLAN_VLD,
	RTL_FE_POL_VLD,
	RTL_FE_POL_EN,
	RTL_FE_POL_ID,
	RTL_FE_POL2_ID_EN,
	RTL_FE_POL2_ID,
	RTL_FE_POL3_ID_EN,
	RTL_FE_POL3_ID,
	RTL_FE_PPPOE_VLD,
	RTL_FE_PPPOE_SET,
	RTL_FE_L4_PORT,
	RTL_FE_IP_MTU_ENC_VLD,
	RTL_FE_IP_MTU_ENC,
	RTL_FE_MODIFY_VLAN_ONLY_VLD,
	RTL_FE_MODIFY_VLAN_ONLY,
	RTL_FE_DIP_REMAP_VLD,
	RTL_FE_DIP_REMAP,
	RTL_FE_VXLAN_SPORT_MSB,
	RTL_FE_VXLAN_SPORT_UPDATE_EN,
	RTL_FE_ACTION_FIELDS,
};

enum rtl_fe_key_field {
	RTL_FE_KEY_L4_CHKSUM_ZERO,
	RTL_FE_KEY_TCP_RDP_CTRL,
	RTL_FE_KEY_L4_DP,
	RTL_FE_KEY_L4_SP,
	RTL_FE_KEY_L3_CHKSUM_ERR,
	RTL_FE_KEY_IP_FRAGMENT,
	RTL_FE_KEY_IP_OPTIONS,
	RTL_FE_KEY_IP_DA,
	RTL_FE_KEY_IP_SA,
	RTL_FE_KEY_IP_PROTOCOL,
	RTL_FE_KEY_IP_VER,
	RTL_FE_KEY_IP_VLD,
	RTL_FE_KEY_PPP_PROTOCOL,
	RTL_FE_KEY_PPPOE_SESSION,
	RTL_FE_KEY_PPPOE_TYPE,
	RTL_FE_KEY_INNER_DEI,
	RTL_FE_KEY_INNER_PCP,
	RTL_FE_KEY_INNER_VID,
	RTL_FE_KEY_INNER_TPID_ENC,
	RTL_FE_KEY_TOP_DEI,
	RTL_FE_KEY_TOP_PCP,
	RTL_FE_KEY_TOP_VID,
	RTL_FE_KEY_TOP_TPID_ENC,
	RTL_FE_KEY_VLAN_CNT,
	RTL_FE_KEY_ETHERTYPE,
	RTL_FE_KEY_MDATA_BYTE,
	RTL_FE_KEY_POL_ID,
	RTL_FE_KEY_ORIG_LSPID,
	RTL_FE_KEY_LSPID,
	RTL_FE_KEY_HKEY_ID,
	RTL_FE_KEY_CTRL_SET_ID,
	RTL_FE_KEY_TABLE_ID,
	RTL_FE_KEY_FIELDS,
};

struct rtl_fe_field_layout {
	u16 bit;
	u8 width;
};

struct rtl_fe_vlan {
	u16 vid;
	u8 tpid;
	u8 priority;
	u8 dei;
};

struct rtl_fe_action {
	u32 address;
	u16 port;
	u16 destination;
	u16 mac_da_index;
	u8 ingress_interface;
	u8 egress_interface;
	u8 cos;
	u8 check_mask;
	u8 translate;
	u8 translate_destination;
	u8 deep_queue;
	u8 vlan_set;
	u8 vlan_count;
	u8 pppoe_pop;
	u8 mtu_set;
	u8 mtu_index;
	u8 gem_set;
	u8 gem_index;
	u8 bridge;
	struct rtl_fe_vlan vlan[2];
};

struct rtl_fe_key {
	u32 source[4];
	u32 destination[4];
	u16 source_port;
	u16 destination_port;
	u16 llid;
	u8 protocol;
	u8 ingress;
	u8 profile;
	u8 mask;
};

struct rtl_fe;
struct rtl_fe_cam_protocol;

void rtl_fe_bits_set(u32 *words, unsigned int bit, unsigned int width, u32 val);
void rtl_fe_action_set(u32 words[RTL_FE_ACTION_WORDS],
		       enum rtl_fe_action_field field, u32 val);
void rtl_fe_action_build(u32 words[RTL_FE_ACTION_WORDS],
			 const struct rtl_fe_action *action);
void rtl_fe_key_set(u32 words[RTL_FE_KEY_WORDS], enum rtl_fe_key_field field,
		    u32 val);
void rtl_fe_key_vlan(u32 words[RTL_FE_KEY_WORDS],
		     const struct rtl_fe_vlan vlan[2], unsigned int count);
void rtl_fe_key_ip(u32 words[RTL_FE_KEY_WORDS], const struct rtl_fe_key *key,
		   bool ipv6);
void rtl_fe_key_bridge(u32 words[RTL_FE_KEY_WORDS], const u8 *source,
		       const u8 *destination, u16 protocol,
		       const struct rtl_fe_key *key);
void rtl_fe_key_crc(const u32 words[RTL_FE_KEY_WORDS], u32 *crc32, u16 *crc16);
void rtl_fe_mask_l3(u32 words[RTL_FE_MASK_WORDS]);
void rtl_fe_mask_bridge(u32 words[RTL_FE_MASK_WORDS]);
unsigned int rtl_fe_hash_bucket(u16 crc16);
void rtl_fe_cpu_miss(u32 words[4]);
void rtl_fe_classify_hash(u32 words[RTL_FE_CLASSIFIER_WORDS],
			  unsigned int profile, unsigned int source);
void rtl_fe_ingress_lpb(u32 words[RTL_FE_LPB_WORDS], unsigned int source,
			unsigned int destination);
void rtl_fe_fdb_key(u32 words[RTL_FE_FDB_WORDS], const u8 *mac);

int rtl_fe_indirect(struct rtl_fe *fe, unsigned int reg, u32 cmd);
int rtl_fe_mask_write(struct rtl_fe *fe, unsigned int index,
		      const u32 words[RTL_FE_MASK_WORDS]);
int rtl_fe_mask_read(struct rtl_fe *fe, unsigned int index,
		     u32 words[RTL_FE_MASK_WORDS]);
void rtl_fe_dma_tables(struct rtl_fe *fe, u32 address, u32 address_high);
void rtl_fe_cache_start(struct rtl_fe *fe);
int rtl_fe_age_write(struct rtl_fe *fe, unsigned int index, unsigned int age);
int rtl_fe_age_read(struct rtl_fe *fe, unsigned int index, unsigned int *age);
int rtl_fe_cache_command(struct rtl_fe *fe, unsigned int index, u16 crc16,
			 unsigned int command);
int rtl_fe_cam_protocol_get(struct rtl_fe *fe, struct rtl_fe_cam_protocol *cam,
			    unsigned int bank, unsigned int protocol);
int rtl_fe_cam_protocol_put(struct rtl_fe *fe, struct rtl_fe_cam_protocol *cam);
int rtl_fe_classifier_read(struct rtl_fe *fe, unsigned int index,
			   u32 words[RTL_FE_CLASSIFIER_WORDS]);
int rtl_fe_classifier_write(struct rtl_fe *fe, unsigned int index,
			    const u32 words[RTL_FE_CLASSIFIER_WORDS]);
int rtl_fe_smac_read(struct rtl_fe *fe, unsigned int index,
		     u32 words[RTL_FE_SMAC_WORDS]);
int rtl_fe_smac_write(struct rtl_fe *fe, unsigned int index,
		      const u32 words[RTL_FE_SMAC_WORDS]);
int rtl_fe_counter_read(struct rtl_fe *fe, unsigned int index,
			u32 words[RTL_FE_COUNTER_WORDS]);
int rtl_fe_fdb_command(struct rtl_fe *fe, unsigned int command,
		       u32 words[RTL_FE_FDB_WORDS], u32 *result);

#endif
