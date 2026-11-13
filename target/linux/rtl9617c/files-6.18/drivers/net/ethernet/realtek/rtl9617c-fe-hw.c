/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rtl9617c-fe-format.h"
#include "rtl9617c-fe-hw.h"


const struct rtl_fe_field_layout rtl_fe_action_fields[RTL_FE_FIELD_COUNT] = {
	[RTL_FE_MRR_VLD] = {0, 1},
	[RTL_FE_MRR_EN] = {1, 1},
	[RTL_FE_NO_DROP_VLD] = {2, 1},
	[RTL_FE_NO_DROP] = {3, 1},
	[RTL_FE_DPID_VLD] = {4, 1},
	[RTL_FE_DPID_PRI] = {5, 1},
	[RTL_FE_PERMIT] = {6, 1},
	[RTL_FE_DEEPQ] = {7, 1},
	[RTL_FE_MCGID] = {8, 10},
	[RTL_FE_MC] = {18, 1},
	[RTL_FE_MDATA_BYTE_VLD] = {19, 1},
	[RTL_FE_MDATA_BYTE] = {20, 8},
	[RTL_FE_L3_IF_VLD1] = {28, 1},
	[RTL_FE_SMAC_TRANS] = {29, 1},
	[RTL_FE_IGR_L3_IF_IDX1] = {30, 6},
	[RTL_FE_EGR_L3_IF_IDX1] = {36, 6},
	[RTL_FE_L3_IF_COUNTER_EN] = {42, 1},
	[RTL_FE_IP_TTL_DEC1] = {43, 1},
	[RTL_FE_IP_TTL_ZERO_DISCARD_EN1] = {44, 1},
	[RTL_FE_IP_ADDR_VLD] = {45, 1},
	[RTL_FE_IP_TYPE] = {46, 1},
	[RTL_FE_IP_ADDR] = {47, 32},
	[RTL_FE_IP_ADDR_NAPT6] = {79, 1},
	[RTL_FE_MAC_DA_IDX_VLD] = {80, 1},
	[RTL_FE_MAC_DA_IDX] = {81, 13},
	[RTL_FE_CHK_MSK_PTR1] = {94, 6},
	[RTL_FE_CACHE_CTRL1] = {100, 2},
	[RTL_FE_POP_L3_VLD1] = {102, 1},
	[RTL_FE_POP_L3_CHK_ECN_EN1] = {103, 1},
	[RTL_FE_POP_L3_EN1] = {104, 1},
	[RTL_FE_T2_CTRL_VLD1] = {105, 1},
	[RTL_FE_T2_CTRL1] = {106, 4},
	[RTL_FE_LDPID_OFFSET_MSB] = {110, 1},
	[RTL_FE_IP_DSCP_UPDATE_EN1] = {111, 1},
	[RTL_FE_IP_DSCP1] = {112, 6},
	[RTL_FE_COS_UPDATE_EN] = {118, 1},
	[RTL_FE_COS1] = {119, 3},
	[RTL_FE_INNER_802_1P_UPDATE_EN] = {122, 1},
	[RTL_FE_INNER_802_1P1] = {123, 3},
	[RTL_FE_TOP_802_1P_UPDATE_EN] = {126, 1},
	[RTL_FE_TOP_802_1P1] = {127, 3},
	[RTL_FE_INNER_DEI1] = {130, 1},
	[RTL_FE_INNER_VID1] = {131, 12},
	[RTL_FE_INNER_TPID_ENC1] = {143, 3},
	[RTL_FE_TOP_DEI1] = {146, 1},
	[RTL_FE_TOP_VID1] = {147, 12},
	[RTL_FE_TOP_TPID_ENC1] = {159, 3},
	[RTL_FE_VLAN_CNT1] = {162, 2},
	[RTL_FE_VLAN_VLD1] = {164, 1},
	[RTL_FE_POL_VLD1] = {165, 1},
	[RTL_FE_POL_EN1] = {166, 1},
	[RTL_FE_POL_ID1] = {167, 8},
	[RTL_FE_POL2_ID_ENABLE1] = {175, 1},
	[RTL_FE_POL2_ID1] = {176, 6},
	[RTL_FE_POL3_ID_ENABLE1] = {182, 1},
	[RTL_FE_POL3_ID1] = {183, 6},
	[RTL_FE_PPPOE_VLD1] = {189, 1},
	[RTL_FE_PPPOE_SET1] = {190, 1},
	[RTL_FE_L4_PORT] = {191, 16},
	[RTL_FE_IP_MTU_ENC_VLD1] = {207, 1},
	[RTL_FE_IP_MTU_ENC1] = {208, 4},
	[RTL_FE_MODIFY_VLAN_ONLY_VLD] = {212, 1},
	[RTL_FE_MODIFY_VLAN_ONLY] = {213, 1},
	[RTL_FE_SIX_RD_DIP_REMAP_OR_FMR_IDX_VLD] = {214, 1},
	[RTL_FE_SIX_RD_DIP_REMAP_OR_FMR_IDX] = {215, 2},
	[RTL_FE_VXLAN_SPORT_MSB15] = {217, 6},
	[RTL_FE_VXLAN_SPORT_UPDATE_EN] = {223, 1},
};

void rtl_fe_bits_set(unsigned int *words, unsigned int bit,
	unsigned int width, unsigned int value)
{
	unsigned int word = bit / 32, shift = bit % 32;
	unsigned int mask = width == 32 ? ~0U : (1U << width) - 1;

	value &= mask;
	words[word] = (words[word] & ~(mask << shift)) | (value << shift);
	if (shift + width > 32)
		words[word + 1] = (words[word + 1] & ~(mask >> (32 - shift))) | (value >> (32 - shift));
}

void rtl_fe_action_set(unsigned int words[RTL_FE_ACTION_WORDS],
	enum rtl_fe_action_field field, unsigned int value)
{
	const struct rtl_fe_field_layout *layout = &rtl_fe_action_fields[field];
	rtl_fe_bits_set(words, layout->bit, layout->width, value);
}

void rtl_fe_ipv4_action(unsigned int words[RTL_FE_ACTION_WORDS],
	const struct rtl_fe_ipv4_action *action)
{
	unsigned int i;

	for (i = 0; i < RTL_FE_ACTION_WORDS; i++)
		words[i] = 0;
	rtl_fe_action_set(words, RTL_FE_DPID_VLD, 1);
	rtl_fe_action_set(words, RTL_FE_DPID_PRI, 1);
	rtl_fe_action_set(words, RTL_FE_PERMIT, 1);
	rtl_fe_action_set(words, RTL_FE_DEEPQ, action->deep_queue);
	rtl_fe_action_set(words, RTL_FE_MCGID, action->destination);
	rtl_fe_action_set(words, RTL_FE_L3_IF_VLD1, !action->bridge);
	rtl_fe_action_set(words, RTL_FE_SMAC_TRANS, action->bridge); 
	rtl_fe_action_set(words, RTL_FE_IGR_L3_IF_IDX1, action->ingress_interface);
	rtl_fe_action_set(words, RTL_FE_EGR_L3_IF_IDX1, action->egress_interface);
	rtl_fe_action_set(words, RTL_FE_IP_TTL_DEC1, !action->bridge);
	rtl_fe_action_set(words, RTL_FE_IP_TTL_ZERO_DISCARD_EN1, !action->bridge);
	rtl_fe_action_set(words, RTL_FE_MAC_DA_IDX_VLD, !action->bridge);
	rtl_fe_action_set(words, RTL_FE_MAC_DA_IDX, action->mac_da_index);
	rtl_fe_action_set(words, RTL_FE_CHK_MSK_PTR1, action->check_mask);
	rtl_fe_action_set(words, RTL_FE_CACHE_CTRL1, 1); 
	rtl_fe_action_set(words, RTL_FE_COS_UPDATE_EN, 1);
	rtl_fe_action_set(words, RTL_FE_COS1, action->cos);
	rtl_fe_action_set(words, RTL_FE_IP_ADDR_VLD, action->translate);
	rtl_fe_action_set(words, RTL_FE_IP_TYPE, action->translate_destination);
	rtl_fe_action_set(words, RTL_FE_IP_ADDR, action->address);
	rtl_fe_action_set(words, RTL_FE_L4_PORT, action->port);
	rtl_fe_action_set(words, RTL_FE_PPPOE_SET1, action->pppoe_pop);
	rtl_fe_action_set(words, RTL_FE_PPPOE_VLD1, 0);
	rtl_fe_action_set(words, RTL_FE_IP_MTU_ENC_VLD1, action->mtu_set);
	rtl_fe_action_set(words, RTL_FE_IP_MTU_ENC1, action->mtu_index);
	if(action->gem_set) {
		
		rtl_fe_action_set(words,RTL_FE_MCGID,action->gem_index);
		rtl_fe_action_set(words,RTL_FE_MC,1);
		rtl_fe_action_set(words,RTL_FE_DEEPQ,1);
		rtl_fe_action_set(words,RTL_FE_POP_L3_VLD1,1);
		rtl_fe_action_set(words,RTL_FE_T2_CTRL1,action->destination&15);
		rtl_fe_action_set(words,RTL_FE_LDPID_OFFSET_MSB,(action->destination>>4)&1);
	}
	if(action->vlan_set) {
		rtl_fe_action_set(words,RTL_FE_VLAN_VLD1,1);
		rtl_fe_action_set(words,RTL_FE_VLAN_CNT1,action->vlan_count);
		for(i=0;i<action->vlan_count;i++) {
			rtl_fe_action_set(words,i?RTL_FE_INNER_VID1:RTL_FE_TOP_VID1,action->vlan[i].vid);
			rtl_fe_action_set(words,i?RTL_FE_INNER_TPID_ENC1:RTL_FE_TOP_TPID_ENC1,action->vlan[i].tpid);
			rtl_fe_action_set(words,i?RTL_FE_INNER_802_1P1:RTL_FE_TOP_802_1P1,action->vlan[i].priority);
			rtl_fe_action_set(words,i?RTL_FE_INNER_802_1P_UPDATE_EN:RTL_FE_TOP_802_1P_UPDATE_EN,1);
			rtl_fe_action_set(words,i?RTL_FE_INNER_DEI1:RTL_FE_TOP_DEI1,action->vlan[i].dei);
		}
	}
}

const struct rtl_fe_field_layout rtl_fe_key_fields[RTL_FE_KEY_FIELD_COUNT] = {
	[RTL_FE_KEY_L4_CHKSUM_ZERO] = {0, 1},
	[RTL_FE_KEY_TCP_RDP_CTRL] = {1, 9},
	[RTL_FE_KEY_L4_DP_EXACT_RANGE] = {10, 16},
	[RTL_FE_KEY_L4_SP_EXACT_RANGE] = {26, 16},
	[RTL_FE_KEY_L3_CHKSUM_ERR] = {42, 1},
	[RTL_FE_KEY_IP_FRAGMENT_FLAG] = {92, 1},
	[RTL_FE_KEY_IP_OPTIONS] = {94, 1},
	[RTL_FE_KEY_IP_DA_0] = {123, 32},
	[RTL_FE_KEY_IP_DA_1] = {155, 32},
	[RTL_FE_KEY_IP_DA_2] = {187, 32},
	[RTL_FE_KEY_IP_DA_3] = {219, 32},
	[RTL_FE_KEY_IP_SA_0] = {251, 32},
	[RTL_FE_KEY_IP_SA_1] = {283, 32},
	[RTL_FE_KEY_IP_SA_2] = {315, 32},
	[RTL_FE_KEY_IP_SA_3] = {347, 32},
	[RTL_FE_KEY_IP_PROTOCOL] = {382, 8},
	[RTL_FE_KEY_IP_VER] = {398, 1},
	[RTL_FE_KEY_IP_VLD] = {399, 1},
	[RTL_FE_KEY_PPPOE_SESSION_ID] = {404, 16},
	[RTL_FE_KEY_PPPOE_TYPE] = {424, 2},
	[RTL_FE_KEY_INNER_VID] = {430, 12},
	[RTL_FE_KEY_INNER_TPID_ENC] = {442, 3},
	[RTL_FE_KEY_TOP_VID] = {449, 12},
	[RTL_FE_KEY_TOP_TPID_ENC] = {461, 3},
	[RTL_FE_KEY_VLAN_CNT] = {464, 2},
	[RTL_FE_KEY_POL_ID] = {682, 9},
	[RTL_FE_KEY_ORIG_LSPID] = {706, 6},
	[RTL_FE_KEY_LSPID] = {712, 6},
	[RTL_FE_KEY_HKEY_ID] = {718, 6},
	[RTL_FE_KEY_CTRL_SET_ID] = {724, 4},
	[RTL_FE_KEY_TABLE_ID] = {728, 4},
	[RTL_FE_KEY_MDATA_BYTE] = {614, 8},
	[RTL_FE_KEY_ETHERTYPE] = {481, 16},
};

void rtl_fe_key_set(unsigned int words[RTL_FE_KEY_WORDS],
	enum rtl_fe_key_field field, unsigned int value)
{
	const struct rtl_fe_field_layout *layout = &rtl_fe_key_fields[field];
	rtl_fe_bits_set(words, layout->bit, layout->width, value);
}

void rtl_fe_vlan_key(unsigned int words[RTL_FE_KEY_WORDS],
	const struct rtl_fe_vlan vlan[2],unsigned int count)
{
	unsigned int i;
	rtl_fe_key_set(words,RTL_FE_KEY_VLAN_CNT,count);
	for(i=0;i<count;i++) {
		rtl_fe_key_set(words,i?RTL_FE_KEY_INNER_VID:RTL_FE_KEY_TOP_VID,vlan[i].vid);
		rtl_fe_key_set(words,i?RTL_FE_KEY_INNER_TPID_ENC:RTL_FE_KEY_TOP_TPID_ENC,vlan[i].tpid);
	}
}

void rtl_fe_ipv4_key(unsigned int words[RTL_FE_KEY_WORDS],
	const struct rtl_fe_ipv4_key *key)
{
	unsigned int i;

	for (i = 0; i < RTL_FE_KEY_WORDS; i++)
		words[i] = 0;
	rtl_fe_key_set(words, RTL_FE_KEY_L4_SP_EXACT_RANGE, key->source_port);
	rtl_fe_key_set(words, RTL_FE_KEY_L4_DP_EXACT_RANGE, key->destination_port);
	rtl_fe_key_set(words, RTL_FE_KEY_IP_SA_0, key->source);
	rtl_fe_key_set(words, RTL_FE_KEY_IP_DA_0, key->destination);
	rtl_fe_key_set(words, RTL_FE_KEY_IP_PROTOCOL, key->protocol);
	rtl_fe_key_set(words, RTL_FE_KEY_IP_VLD, 1);
	rtl_fe_key_set(words, RTL_FE_KEY_POL_ID, key->llid);
	rtl_fe_key_set(words, RTL_FE_KEY_ORIG_LSPID, key->ingress);
	rtl_fe_key_set(words, RTL_FE_KEY_LSPID, key->ingress);
	rtl_fe_key_set(words, RTL_FE_KEY_HKEY_ID, key->mask);
	rtl_fe_key_set(words, RTL_FE_KEY_CTRL_SET_ID, key->profile);
	rtl_fe_key_set(words, RTL_FE_KEY_TABLE_ID, 1);
}

void rtl_fe_ipv6_key(unsigned int words[RTL_FE_KEY_WORDS],
	const struct rtl_fe_ipv6_key *key)
{
	struct rtl_fe_ipv4_key low={.source=key->source[3],.destination=key->destination[3],
		.source_port=key->source_port,.destination_port=key->destination_port,
		.llid=key->llid,.protocol=key->protocol,.ingress=key->ingress,.profile=key->profile,.mask=key->mask};
	unsigned int i;

	rtl_fe_ipv4_key(words,&low);
	rtl_fe_key_set(words,RTL_FE_KEY_IP_VER,1);
	for(i=1;i<4;i++) {
		rtl_fe_bits_set(words,123+i*32,32,key->destination[3-i]);
		rtl_fe_bits_set(words,251+i*32,32,key->source[3-i]);
	}
}

unsigned int rtl_fe_reverse(unsigned int value, unsigned int bits)
{
	unsigned int result = 0, i;

	for (i = 0; i < bits; i++) {
		result = (result << 1) | (value & 1);
		value >>= 1;
	}
	return result;
}

void rtl_fe_key_crc(const unsigned int words[RTL_FE_KEY_WORDS],
	unsigned int *crc32, unsigned short *crc16)
{
	unsigned int c32 = ~0U, c16 = 0xffffU, byte, bit;

	for (byte = RTL_FE_KEY_WORDS * 4; byte; byte--) {
		unsigned int index = byte - 1;
		unsigned int value = rtl_fe_reverse((words[index / 4] >> ((index % 4) * 8)) & 255, 8);

		c32 ^= value;
		c16 ^= value;
		for (bit = 0; bit < 8; bit++) {
			c32 = (c32 >> 1) ^ ((c32 & 1) ? 0xedb88320U : 0);
			c16 = (c16 >> 1) ^ ((c16 & 1) ? 0x8408U : 0);
		}
	}
 *crc32 = rtl_fe_reverse(c32, 32);
 *crc16 = rtl_fe_reverse(c16, 16);
}

void rtl_fe_ipv4_mask(unsigned int words[RTL_FE_MASK_WORDS])
{
	unsigned int i;
	for (i = 0; i < 7; i++)
		words[i] = ~0U;
	words[7] = 0;
	
	words[1] &= ~(3U << 23);
	words[0] = (words[0] & ~0x07fffc00U) | 0x00000000U; 
	words[0] = (words[0] & ~0xf8000000U) | 0x00000000U; 
	words[1] = (words[1] & ~0x00000fffU) | 0x00000000U;
	words[1] = (words[1] & ~0x00001000U) | 0x00000000U; 
	words[1] = (words[1] & ~0x00100000U) | 0x00000000U; 
	words[1] = (words[1] & ~0x00400000U) | 0x00000000U; 
	words[2] = (words[2] & ~0x00000ff8U) | 0x00000100U; 
	words[1] = (words[1] & ~0xfc000000U) | 0x80000000U; 
	words[2] = (words[2] & ~0x00000007U) | 0x00000000U;
	words[2] = (words[2] & ~0x00002000U) | 0x00000000U; 
	words[2] = (words[2] & ~0x00800000U) | 0x00000000U; 
	words[2] = (words[2] & ~0x00400000U) | 0x00000000U; 
	words[6] = (words[6] & ~0x00800000U) | 0x00000000U; 
	words[6] = (words[6] & ~0x01000000U) | 0x00000000U; 
	words[6] = (words[6] & ~0x00000200U) | 0x00000000U; 
	words[6] = (words[6] & ~0x04000000U) | 0x00000000U; 
	
	rtl_fe_bits_set(words,217,1,0); 
}

void rtl_fe_ipv6_mask(unsigned int words[RTL_FE_MASK_WORDS])
{
	rtl_fe_ipv4_mask(words);
	rtl_fe_bits_set(words,58,9,128); 
	rtl_fe_bits_set(words,67,9,128); 
}

void rtl_fe_bridge_mask(unsigned int words[RTL_FE_MASK_WORDS])
{
	unsigned int i;

	for(i=0;i<7;i++)words[i]=~0U;
	words[7]=0;
	rtl_fe_bits_set(words,55,2,0); 
	rtl_fe_bits_set(words,58,9,0); 
	rtl_fe_bits_set(words,67,9,0);
	rtl_fe_bits_set(words,89,1,0); 
	rtl_fe_bits_set(words,91,1,0); 
	rtl_fe_bits_set(words,94,2,0); 
	rtl_fe_bits_set(words,98,3,0); 
	rtl_fe_bits_set(words,109,7,0); 
	rtl_fe_bits_set(words,120,6,0); 
	rtl_fe_bits_set(words,201,1,0); 
	rtl_fe_bits_set(words,216,1,0); 
	rtl_fe_bits_set(words,217,1,0); 
}

void rtl_fe_bridge_key(unsigned int words[RTL_FE_KEY_WORDS],
	const unsigned char source[6],const unsigned char destination[6],
	unsigned int protocol,unsigned int ingress,unsigned int llid,unsigned int mask)
{
	unsigned int i;

	for(i=0;i<RTL_FE_KEY_WORDS;i++)words[i]=0;
	for(i=0;i<6;i++) {
		rtl_fe_bits_set(words,497+(5-i)*8,8,source[i]);
		rtl_fe_bits_set(words,552+(5-i)*8,8,destination[i]);
	}
	rtl_fe_key_set(words,RTL_FE_KEY_ETHERTYPE,protocol);
	rtl_fe_key_set(words,RTL_FE_KEY_LSPID,ingress);
	rtl_fe_key_set(words,RTL_FE_KEY_POL_ID,llid);
	rtl_fe_key_set(words,RTL_FE_KEY_HKEY_ID,mask);
	rtl_fe_key_set(words,RTL_FE_KEY_TABLE_ID,1);
}

void rtl_fe_header_ipv4(unsigned int words[RTL_FE_HEADER_WORDS],
	const struct rtl_fe_ipv4_key *key)
{
	unsigned int i;
	for (i = 0; i < RTL_FE_HEADER_WORDS; i++)
		words[i] = 0;
	rtl_fe_bits_set(words, 74, 16, key->destination_port); 
	rtl_fe_bits_set(words, 90, 16, key->source_port); 
	rtl_fe_bits_set(words, 233, 32, key->destination); 
	rtl_fe_bits_set(words, 361, 32, key->source); 
	rtl_fe_bits_set(words, 492, 8, key->protocol); 
	rtl_fe_bits_set(words, 510, 1, 1); 
	rtl_fe_bits_set(words, 802, 8, key->llid); 
	rtl_fe_bits_set(words, 831, 6, key->ingress); 
	rtl_fe_bits_set(words, 837, 6, key->ingress); 
	rtl_fe_bits_set(words, 978, 4, key->profile); 
}

void rtl_fe_header_ipv6(unsigned int words[RTL_FE_HEADER_WORDS],
	const struct rtl_fe_ipv6_key *key)
{
	struct rtl_fe_ipv4_key low={.source=key->source[3],.destination=key->destination[3],
		.source_port=key->source_port,.destination_port=key->destination_port,
		.llid=key->llid,.protocol=key->protocol,.ingress=key->ingress,.profile=key->profile};
	unsigned int i;

	rtl_fe_header_ipv4(words,&low);
	rtl_fe_bits_set(words,489,3,6); 
	rtl_fe_bits_set(words,509,1,1); 
	for(i=1;i<4;i++) {
		rtl_fe_bits_set(words,233+i*32,32,key->destination[3-i]);
		rtl_fe_bits_set(words,361+i*32,32,key->source[3-i]);
	}
}

unsigned int rtl_fe_hash_bucket(unsigned short crc16)
{
	
	return crc16&0xffe0U;
}

void rtl_fe_cpu_miss(unsigned int words[4])
{
	unsigned int i;
	for (i = 0; i < 4; i++)
		words[i] = 0;
	rtl_fe_bits_set(words, 41 + 13, 1, 1); 
	rtl_fe_bits_set(words, 41 + 14, 1, 1); 
}

void rtl_fe_classify_hash(unsigned int words[7],unsigned int profile,unsigned int source)
{
	unsigned int i;
	for(i=0;i<7;i++)words[i]=0;
	rtl_fe_bits_set(words,203,1,1); 
	rtl_fe_bits_set(words,204,4,profile); 
	rtl_fe_bits_set(words,186,1,1); 
	rtl_fe_bits_set(words,187,1,0); 
	
	rtl_fe_bits_set(words,154,1,1); 
	rtl_fe_bits_set(words,155,1,1); 
	rtl_fe_bits_set(words,156,1,1); 
	rtl_fe_bits_set(words,158,10,16); 
	rtl_fe_bits_set(words,208,1,1); 
	rtl_fe_bits_set(words,209,6,source); 
}

void rtl_fe_ingress_lpb(unsigned int words[3],unsigned int source,unsigned int destination)
{
	unsigned int i;
	for(i=0;i<3;i++)words[i]=0;
	rtl_fe_bits_set(words,24,14,1700); 
	rtl_fe_bits_set(words,38,4,15); 
	rtl_fe_bits_set(words,42,4,0); 
	rtl_fe_bits_set(words,68,6,destination); 
	rtl_fe_bits_set(words,88,6,source); 
}

void rtl_fe_fdb_key(unsigned int words[4],const unsigned char mac[6])
{
	unsigned int i;
	for(i=0;i<4;i++)words[i]=0;
	for(i=0;i<6;i++)rtl_fe_bits_set(words,51+(5-i)*8,8,mac[i]);
	words[0]=(1U<<9)|(255U<<11)|(1U<<19)|(1U<<20)|(1U<<21); 
}

#ifndef RTL9617C_FE_HOST_TEST
#include "rtl9617c-fe-priv.h"
#include "rtl9617c-fe-table.h"
#include "rtl9617c-fe-pipeline.h"

int rtl_fe_mask_write(struct rtl_fe *fe, unsigned int index,
	const unsigned int words[RTL_FE_MASK_WORDS])
{
	unsigned int bank, word;
	int ret;

	for (bank = 0; bank < 2; bank++) {
		for (word = 0; word < 4; word++)
			rtl_ni_write(fe->regs, RTL_FE_MASK_DATA - word * 4, words[bank * 4 + word]);
		ret = fe_indirect(fe, RTL_FE_MASK_CMD, 0xc0000000U | (bank << 6) | index);
		if (ret)
			return ret;
	}
	return 0;
}

int rtl_fe_mask_read(struct rtl_fe *fe, unsigned int index,
	unsigned int words[RTL_FE_MASK_WORDS])
{
	unsigned int bank, word;
	int ret;

	for (bank = 0; bank < 2; bank++) {
		ret = fe_indirect(fe, RTL_FE_MASK_CMD, 0x80000000U | (bank << 6) | index);
		if (ret)
			return ret;
		for (word = 0; word < 4; word++)
			words[bank * 4 + word] = rtl_ni_read(fe->regs, RTL_FE_MASK_DATA - word * 4);
	}
	return 0;
}

int rtl_fe_crc_hardware(struct rtl_fe *fe,
	const unsigned int header[RTL_FE_HEADER_WORDS], unsigned int mask,
	unsigned int *crc32, unsigned short *crc16)
{
	unsigned int i, tuple=rtl_ni_read(fe->regs,RTL_FE_SWO_TUPLE);
	int ret;

	rtl_ni_write(fe->regs, RTL_FE_SWO_CLEAR, 0);
	for (i = 0; i < RTL_FE_HEADER_WORDS; i++)
		rtl_ni_write(fe->regs, RTL_FE_SWO_DATA, header[i]);
	rtl_ni_write(fe->regs, RTL_FE_SWO_DATA, mask);
	rtl_ni_write(fe->regs,RTL_FE_SWO_TUPLE,(tuple&~63U)|(mask&63U));
	ret = fe_indirect(fe, RTL_FE_SWO_CMD, 1);
	rtl_ni_write(fe->regs,RTL_FE_SWO_TUPLE,tuple);
	if (ret)
		return ret;
	rtl_ni_write(fe->regs, RTL_FE_SWO_CLEAR, 33);
 *crc32 = rtl_ni_read(fe->regs, RTL_FE_SWO_DATA);
 *crc16 = rtl_ni_read(fe->regs, RTL_FE_SWO_DATA) & 0xffff;
	return 0;
}

void rtl_fe_dma_tables(struct rtl_fe *fe,
	unsigned int address, unsigned int address_high)
{
	rtl_ni_write(fe->regs, RTL_FE_HASH_ENABLE, 0);
	rtl_ni_write(fe->regs, RTL_FE_HASH_BASE_HIGH, address_high);
	rtl_ni_write(fe->regs, RTL_FE_HASH_BASE_LOW, address);
	rtl_ni_write(fe->regs, RTL_FE_ACTION_BASE_HIGH, address_high);
	rtl_ni_write(fe->regs, RTL_FE_ACTION_BASE_LOW, address + RTL_FE_ACTION_OFFSET);
	rtl_ni_write(fe->regs, RTL_FE_OVERFLOW_BASE_HIGH, address_high);
	rtl_ni_write(fe->regs, RTL_FE_OVERFLOW_BASE_LOW, address + RTL_FE_OVERFLOW_OFFSET);
	rtl_ni_write(fe->regs, RTL_FE_DEFAULT_BASE_HIGH, address_high);
	rtl_ni_write(fe->regs, RTL_FE_DEFAULT_BASE_LOW, address + RTL_FE_DEFAULT_OFFSET);
	rtl_ni_write(fe->regs, RTL_FE_BUCKET_CFG,
		(rtl_ni_read(fe->regs, RTL_FE_BUCKET_CFG) & ~0x300ffU) | 3 | (7 << 2) | (4 << 5));
	rtl_ni_write(fe->regs, RTL_FE_ACTION_SLOT_CFG, 4);
	rtl_ni_write(fe->regs, RTL_FE_DEFAULT_SLOT_CFG, 1);
	rtl_ni_write(fe->regs, RTL_FE_HASH_CFG, (1 << 18) | (1 << 16) | (3 << 8) | 4);
}

void rtl_fe_cache_start(struct rtl_fe *fe)
{
	rtl_ni_write(fe->regs,RTL_FE_AXI_PATCH,rtl_ni_read(fe->regs,RTL_FE_AXI_PATCH)|0x80000001U);
	rtl_ni_write(fe->regs,RTL_FE_AXI_ID1_LIMIT,rtl_ni_read(fe->regs,RTL_FE_AXI_ID1_LIMIT)|1U);
	rtl_ni_write(fe->regs,RTL_FE_AXI_OUTSTANDING,rtl_ni_read(fe->regs,RTL_FE_AXI_OUTSTANDING)|255U);
	
	rtl_ni_write(fe->regs,RTL_FE_MISS_TEMPLATE,(rtl_ni_read(fe->regs,RTL_FE_MISS_TEMPLATE)&~0xffffU)|0xd0d0);
	rtl_ni_write(fe->regs,RTL_FE_CACHE_CFG,0);
	rtl_ni_write(fe->regs,RTL_FE_KEY_MASK_0,0xffffffffU);
	rtl_ni_write(fe->regs,RTL_FE_KEY_MASK_1,0xffffffffU);
	rtl_ni_write(fe->regs,RTL_FE_HASH_ENABLE,0xa0000000U);
}



int rtl_fe_main_age(struct rtl_fe *fe,unsigned int index,unsigned int age)
{
	unsigned int row=index>>5,slot=index&31,reg=0x3938-(slot>>3)*4,value;
	int ret=fe_indirect(fe,RTL_FE_AGE_CMD,0x80000000U|row);
	if(ret)return ret;
	value=rtl_ni_read(fe->regs,reg);
	value=(value&~(15U<<((slot&7)*4)))|((age&15)<<((slot&7)*4));
	rtl_ni_write(fe->regs,reg,value);
	return fe_indirect(fe,RTL_FE_AGE_CMD,0xc0000000U|row);
}

int rtl_fe_main_age_read(struct rtl_fe *fe,
	unsigned int index,unsigned int *age)
{
	unsigned int slot=index&31;
	int ret=fe_indirect(fe,RTL_FE_AGE_CMD,0x80000000U|(index>>5));

	if(ret)return ret;
 *age=(rtl_ni_read(fe->regs,RTL_FE_AGE_DATA-(slot>>3)*4)>>((slot&7)*4))&15;
	return 0;
}

int rtl_fe_cache_command(struct rtl_fe *fe,unsigned int index,
	unsigned short crc16,unsigned int command)
{
	rtl_ni_write(fe->regs,RTL_FE_CACHE_DATA,(index&31)|((unsigned int)crc16<<5)|(3U<<26)|(command<<30));
	return fe_indirect(fe,RTL_FE_CACHE_CMD,1);
}

int rtl_fe_cache_read(struct rtl_fe *fe,unsigned int index,int fib,unsigned int words[7])
{
	unsigned int i;
	int ret=fe_indirect(fe,RTL_FE_CACHE_READ_CMD,0x80000000U|index|(fib?1U<<11:0));
	if(ret)return ret;
	for(i=0;i<7;i++)words[i]=rtl_ni_read(fe->regs,RTL_FE_CACHE_READ_DATA-i*4);
	return 0;
}

int rtl_fe_cam_protocol_get(struct rtl_fe *fe,
	struct rtl_fe_cam_protocol *binding,unsigned int bank,unsigned int protocol)
{
	unsigned int count=bank==2?63:15,index,word,words[5],free_index=count;
	unsigned int width=bank==2?5:3;
	int ret;

	binding->bank=bank;binding->created=0;
	for(index=0;index<count;index++) {
		ret=fe_indirect(fe,RTL_FE_CAM_CMD,0x80000000U|(bank<<16)|index);
		if(ret)return ret;
		for(word=0;word<width;word++)words[word]=rtl_ni_read(fe->regs,RTL_FE_CAM_DATA-word*4);
		if(words[0]&(1U<<16)) {
			if((words[0]&65535)==protocol){binding->index=index;return index+1;}
		} else if(free_index==count) {
			free_index=index;
			for(word=0;word<width;word++)binding->saved[word]=words[word];
		}
	}
	if(free_index==count)return -ENOSPC;
	binding->index=free_index;binding->created=1;
	for(word=0;word<width;word++)rtl_ni_write(fe->regs,RTL_FE_CAM_DATA-word*4,word?0:protocol|(1U<<16));
	ret=fe_indirect(fe,RTL_FE_CAM_CMD,0xc0000000U|(bank<<16)|free_index);
	if(ret)return ret;
	return free_index+1;
}

int rtl_fe_cam_protocol_put(struct rtl_fe *fe,struct rtl_fe_cam_protocol *binding)
{
	unsigned int word;
	int ret;

	if(!binding->created)return 0;
	for(word=0;word<(binding->bank==2?5U:3U);word++)rtl_ni_write(fe->regs,RTL_FE_CAM_DATA-word*4,binding->saved[word]);
	ret=fe_indirect(fe,RTL_FE_CAM_CMD,0xc0000000U|(binding->bank<<16)|binding->index);
	if(!ret)binding->created=0;
	return ret;
}



int rtl_fe_classifier_read(struct rtl_fe *fe,unsigned int index,unsigned int words[7])
{
	unsigned int i;
	int ret=fe_indirect(fe,RTL_FE_CLASSIFIER_CMD,0x80000000U|index);
	if(ret)return ret;
	for(i=0;i<7;i++)words[i]=rtl_ni_read(fe->regs,RTL_FE_CLASSIFIER_DATA-i*4);
	return 0;
}

int rtl_fe_classifier_write(struct rtl_fe *fe,unsigned int index,const unsigned int words[7])
{
	unsigned int i;
	for(i=0;i<7;i++)rtl_ni_write(fe->regs,RTL_FE_CLASSIFIER_DATA-i*4,words[i]);
	return fe_indirect(fe,RTL_FE_CLASSIFIER_CMD,0xc0000000U|index);
}

int rtl_fe_source_mac_read(struct rtl_fe *fe,unsigned int index,unsigned int words[5])
{
	unsigned int i;
	int ret=fe_indirect(fe,RTL_FE_CAM_CMD,0x80030000U|index);
	if(ret)return ret;
	for(i=0;i<5;i++)words[i]=rtl_ni_read(fe->regs,RTL_FE_CAM_DATA-i*4);
	return 0;
}

int rtl_fe_source_mac_write(struct rtl_fe *fe,unsigned int index,const unsigned int words[5])
{
	unsigned int i;
	for(i=0;i<5;i++)rtl_ni_write(fe->regs,RTL_FE_CAM_DATA-i*4,words[i]);
	return fe_indirect(fe,RTL_FE_CAM_CMD,0xc0030000U|index);
}

int rtl_fe_tx_counter_read(struct rtl_fe *fe,
	unsigned int index,unsigned int words[7])
{
	unsigned int i;
	int ret=fe_indirect(fe,RTL_FE_COUNTER_CMD,0x80000000U|index);

	if(ret)return ret;
	for(i=0;i<7;i++)words[i]=rtl_ni_read(fe->regs,RTL_FE_COUNTER_DATA-i*4);
	return 0;
}


int rtl_fe_fdb_command(struct rtl_fe *fe,unsigned int command,unsigned int words[4],unsigned int *result)
{
	unsigned int i,control=rtl_ni_read(fe->regs,RTL_NI_FDB_CONTROL);
	int ret;
	rtl_ni_write(fe->regs,RTL_NI_FDB_CONTROL,control|0x80000000U);
	for(i=0;i<4;i++)rtl_ni_write(fe->regs,RTL_NI_FDB_DATA-i*4,words[i]);
	rtl_ni_write(fe->regs,RTL_NI_FDB_RESULT,0);
	ret=fe_indirect(fe,RTL_NI_FDB_CMD,0x80000000U|command);
	rtl_ni_write(fe->regs,RTL_NI_FDB_CONTROL,control);
	if(ret)return ret;
 *result=rtl_ni_read(fe->regs,RTL_NI_FDB_RESULT);
	for(i=0;i<4;i++)words[i]=rtl_ni_read(fe->regs,RTL_NI_FDB_DATA-i*4);
	return 0;
}
#endif
