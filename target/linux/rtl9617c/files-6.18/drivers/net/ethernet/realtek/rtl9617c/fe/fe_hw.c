// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitrev.h>
#include <linux/crc32poly.h>
#include <linux/kernel.h>

#include "fe_priv.h"

#define FE_FDB_KEY_INIT		(BIT(9) | (255U << 11) | BIT(19) | BIT(20) | BIT(21))
#define FE_FDB_MAC_BIT		51
#define FE_CACHE_VALID		(3U << 26)
#define FE_AGE_SLOTS		32
#define FE_AGE_PER_REG		8
#define FE_AGE_BITS		4

static const struct rtl_fe_field_layout rtl_fe_action_fields[RTL_FE_ACTION_FIELDS] = {
	[RTL_FE_MRR_VLD] = { 0, 1 },
	[RTL_FE_MRR_EN] = { 1, 1 },
	[RTL_FE_NO_DROP_VLD] = { 2, 1 },
	[RTL_FE_NO_DROP] = { 3, 1 },
	[RTL_FE_DPID_VLD] = { 4, 1 },
	[RTL_FE_DPID_PRI] = { 5, 1 },
	[RTL_FE_PERMIT] = { 6, 1 },
	[RTL_FE_DEEPQ] = { 7, 1 },
	[RTL_FE_MCGID] = { 8, 10 },
	[RTL_FE_MC] = { 18, 1 },
	[RTL_FE_MDATA_BYTE_VLD] = { 19, 1 },
	[RTL_FE_MDATA_BYTE] = { 20, 8 },
	[RTL_FE_L3_IF_VLD] = { 28, 1 },
	[RTL_FE_SMAC_TRANS] = { 29, 1 },
	[RTL_FE_IGR_L3_IF_IDX] = { 30, 6 },
	[RTL_FE_EGR_L3_IF_IDX] = { 36, 6 },
	[RTL_FE_L3_IF_COUNTER_EN] = { 42, 1 },
	[RTL_FE_IP_TTL_DEC] = { 43, 1 },
	[RTL_FE_IP_TTL_ZERO_DISCARD_EN] = { 44, 1 },
	[RTL_FE_IP_ADDR_VLD] = { 45, 1 },
	[RTL_FE_IP_TYPE] = { 46, 1 },
	[RTL_FE_IP_ADDR] = { 47, 32 },
	[RTL_FE_IP_ADDR_NAPT6] = { 79, 1 },
	[RTL_FE_MAC_DA_IDX_VLD] = { 80, 1 },
	[RTL_FE_MAC_DA_IDX] = { 81, 13 },
	[RTL_FE_CHK_MSK_PTR] = { 94, 6 },
	[RTL_FE_CACHE_CTRL] = { 100, 2 },
	[RTL_FE_POP_L3_VLD] = { 102, 1 },
	[RTL_FE_POP_L3_CHK_ECN_EN] = { 103, 1 },
	[RTL_FE_POP_L3_EN] = { 104, 1 },
	[RTL_FE_T2_CTRL_VLD] = { 105, 1 },
	[RTL_FE_T2_CTRL] = { 106, 4 },
	[RTL_FE_LDPID_OFFSET_MSB] = { 110, 1 },
	[RTL_FE_IP_DSCP_UPDATE_EN] = { 111, 1 },
	[RTL_FE_IP_DSCP] = { 112, 6 },
	[RTL_FE_COS_UPDATE_EN] = { 118, 1 },
	[RTL_FE_COS] = { 119, 3 },
	[RTL_FE_INNER_8021P_UPDATE_EN] = { 122, 1 },
	[RTL_FE_INNER_8021P] = { 123, 3 },
	[RTL_FE_TOP_8021P_UPDATE_EN] = { 126, 1 },
	[RTL_FE_TOP_8021P] = { 127, 3 },
	[RTL_FE_INNER_DEI] = { 130, 1 },
	[RTL_FE_INNER_VID] = { 131, 12 },
	[RTL_FE_INNER_TPID_ENC] = { 143, 3 },
	[RTL_FE_TOP_DEI] = { 146, 1 },
	[RTL_FE_TOP_VID] = { 147, 12 },
	[RTL_FE_TOP_TPID_ENC] = { 159, 3 },
	[RTL_FE_VLAN_CNT] = { 162, 2 },
	[RTL_FE_VLAN_VLD] = { 164, 1 },
	[RTL_FE_POL_VLD] = { 165, 1 },
	[RTL_FE_POL_EN] = { 166, 1 },
	[RTL_FE_POL_ID] = { 167, 8 },
	[RTL_FE_POL2_ID_EN] = { 175, 1 },
	[RTL_FE_POL2_ID] = { 176, 6 },
	[RTL_FE_POL3_ID_EN] = { 182, 1 },
	[RTL_FE_POL3_ID] = { 183, 6 },
	[RTL_FE_PPPOE_VLD] = { 189, 1 },
	[RTL_FE_PPPOE_SET] = { 190, 1 },
	[RTL_FE_L4_PORT] = { 191, 16 },
	[RTL_FE_IP_MTU_ENC_VLD] = { 207, 1 },
	[RTL_FE_IP_MTU_ENC] = { 208, 4 },
	[RTL_FE_MODIFY_VLAN_ONLY_VLD] = { 212, 1 },
	[RTL_FE_MODIFY_VLAN_ONLY] = { 213, 1 },
	[RTL_FE_DIP_REMAP_VLD] = { 214, 1 },
	[RTL_FE_DIP_REMAP] = { 215, 2 },
	[RTL_FE_VXLAN_SPORT_MSB] = { 217, 6 },
	[RTL_FE_VXLAN_SPORT_UPDATE_EN] = { 223, 1 },
};

static const struct rtl_fe_field_layout rtl_fe_key_fields[RTL_FE_KEY_FIELDS] = {
	[RTL_FE_KEY_L4_CHKSUM_ZERO] = { 0, 1 },
	[RTL_FE_KEY_TCP_RDP_CTRL] = { 1, 9 },
	[RTL_FE_KEY_L4_DP] = { 10, 16 },
	[RTL_FE_KEY_L4_SP] = { 26, 16 },
	[RTL_FE_KEY_L3_CHKSUM_ERR] = { 42, 1 },
	[RTL_FE_KEY_IP_FRAGMENT] = { 92, 1 },
	[RTL_FE_KEY_IP_OPTIONS] = { 94, 1 },
	[RTL_FE_KEY_IP_DA] = { 123, 32 },
	[RTL_FE_KEY_IP_SA] = { 251, 32 },
	[RTL_FE_KEY_IP_PROTOCOL] = { 382, 8 },
	[RTL_FE_KEY_IP_VER] = { 398, 1 },
	[RTL_FE_KEY_IP_VLD] = { 399, 1 },
	[RTL_FE_KEY_PPP_PROTOCOL] = { 400, 4 },
	[RTL_FE_KEY_PPPOE_SESSION] = { 404, 16 },
	[RTL_FE_KEY_PPPOE_TYPE] = { 424, 2 },
	[RTL_FE_KEY_INNER_DEI] = { 426, 1 },
	[RTL_FE_KEY_INNER_PCP] = { 427, 3 },
	[RTL_FE_KEY_INNER_VID] = { 430, 12 },
	[RTL_FE_KEY_INNER_TPID_ENC] = { 442, 3 },
	[RTL_FE_KEY_TOP_DEI] = { 445, 1 },
	[RTL_FE_KEY_TOP_PCP] = { 446, 3 },
	[RTL_FE_KEY_TOP_VID] = { 449, 12 },
	[RTL_FE_KEY_TOP_TPID_ENC] = { 461, 3 },
	[RTL_FE_KEY_VLAN_CNT] = { 464, 2 },
	[RTL_FE_KEY_ETHERTYPE] = { 481, 16 },
	[RTL_FE_KEY_MDATA_BYTE] = { 614, 8 },
	[RTL_FE_KEY_POL_ID] = { 682, 9 },
	[RTL_FE_KEY_ORIG_LSPID] = { 706, 6 },
	[RTL_FE_KEY_LSPID] = { 712, 6 },
	[RTL_FE_KEY_HKEY_ID] = { 718, 6 },
	[RTL_FE_KEY_CTRL_SET_ID] = { 724, 4 },
	[RTL_FE_KEY_TABLE_ID] = { 728, 4 },
};

void rtl_fe_bits_set(u32 *words, unsigned int bit, unsigned int width, u32 val)
{
	unsigned int word = bit / 32, shift = bit % 32;
	u32 mask = width == 32 ? ~0U : BIT(width) - 1;

	val &= mask;
	words[word] = (words[word] & ~(mask << shift)) | (val << shift);
	if (shift + width > 32)
		words[word + 1] = (words[word + 1] & ~(mask >> (32 - shift))) |
				  (val >> (32 - shift));
}

void rtl_fe_action_set(u32 words[RTL_FE_ACTION_WORDS],
		       enum rtl_fe_action_field field, u32 val)
{
	const struct rtl_fe_field_layout *f = &rtl_fe_action_fields[field];

	rtl_fe_bits_set(words, f->bit, f->width, val);
}

void rtl_fe_action_build(u32 words[RTL_FE_ACTION_WORDS],
			 const struct rtl_fe_action *action)
{
	bool l3 = !action->bridge;
	unsigned int i;

	memset(words, 0, RTL_FE_ACTION_WORDS * sizeof(*words));
	rtl_fe_action_set(words, RTL_FE_DPID_VLD, 1);
	rtl_fe_action_set(words, RTL_FE_DPID_PRI, 1);
	rtl_fe_action_set(words, RTL_FE_PERMIT, 1);
	rtl_fe_action_set(words, RTL_FE_DEEPQ, action->deep_queue);
	rtl_fe_action_set(words, RTL_FE_MCGID, action->destination);
	rtl_fe_action_set(words, RTL_FE_L3_IF_VLD, l3);
	rtl_fe_action_set(words, RTL_FE_SMAC_TRANS, action->bridge);
	rtl_fe_action_set(words, RTL_FE_IGR_L3_IF_IDX, action->ingress_interface);
	rtl_fe_action_set(words, RTL_FE_EGR_L3_IF_IDX, action->egress_interface);
	rtl_fe_action_set(words, RTL_FE_IP_TTL_DEC, l3);
	rtl_fe_action_set(words, RTL_FE_IP_TTL_ZERO_DISCARD_EN, l3);
	rtl_fe_action_set(words, RTL_FE_MAC_DA_IDX_VLD, l3);
	rtl_fe_action_set(words, RTL_FE_MAC_DA_IDX, action->mac_da_index);
	rtl_fe_action_set(words, RTL_FE_CHK_MSK_PTR, action->check_mask);
	rtl_fe_action_set(words, RTL_FE_CACHE_CTRL, 1);
	rtl_fe_action_set(words, RTL_FE_COS_UPDATE_EN, 1);
	rtl_fe_action_set(words, RTL_FE_COS, action->cos);
	rtl_fe_action_set(words, RTL_FE_IP_ADDR_VLD, action->translate);
	rtl_fe_action_set(words, RTL_FE_IP_TYPE, action->translate_destination);
	rtl_fe_action_set(words, RTL_FE_IP_ADDR, action->address);
	rtl_fe_action_set(words, RTL_FE_L4_PORT, action->port);
	rtl_fe_action_set(words, RTL_FE_PPPOE_SET, action->pppoe_pop);
	rtl_fe_action_set(words, RTL_FE_PPPOE_VLD, 0);
	rtl_fe_action_set(words, RTL_FE_IP_MTU_ENC_VLD, action->mtu_set);
	rtl_fe_action_set(words, RTL_FE_IP_MTU_ENC, action->mtu_index);

	if (action->gem_set) {
		rtl_fe_action_set(words, RTL_FE_MCGID, action->gem_index);
		rtl_fe_action_set(words, RTL_FE_MC, 1);
		rtl_fe_action_set(words, RTL_FE_DEEPQ, 1);
		rtl_fe_action_set(words, RTL_FE_POP_L3_VLD, 1);
		rtl_fe_action_set(words, RTL_FE_T2_CTRL, action->destination & 15);
		rtl_fe_action_set(words, RTL_FE_LDPID_OFFSET_MSB,
				  (action->destination >> 4) & 1);
	}

	if (!action->vlan_set)
		return;

	rtl_fe_action_set(words, RTL_FE_VLAN_VLD, 1);
	rtl_fe_action_set(words, RTL_FE_VLAN_CNT, action->vlan_count);
	for (i = 0; i < action->vlan_count; i++) {
		const struct rtl_fe_vlan *vlan = &action->vlan[i];

		rtl_fe_action_set(words, i ? RTL_FE_INNER_VID : RTL_FE_TOP_VID,
				  vlan->vid);
		rtl_fe_action_set(words, i ? RTL_FE_INNER_TPID_ENC :
				  RTL_FE_TOP_TPID_ENC, vlan->tpid);
		rtl_fe_action_set(words, i ? RTL_FE_INNER_8021P : RTL_FE_TOP_8021P,
				  vlan->priority);
		rtl_fe_action_set(words, i ? RTL_FE_INNER_8021P_UPDATE_EN :
				  RTL_FE_TOP_8021P_UPDATE_EN, 1);
		rtl_fe_action_set(words, i ? RTL_FE_INNER_DEI : RTL_FE_TOP_DEI,
				  vlan->dei);
	}
}

void rtl_fe_key_set(u32 words[RTL_FE_KEY_WORDS], enum rtl_fe_key_field field,
		    u32 val)
{
	const struct rtl_fe_field_layout *f = &rtl_fe_key_fields[field];

	rtl_fe_bits_set(words, f->bit, f->width, val);
}

void rtl_fe_key_vlan(u32 words[RTL_FE_KEY_WORDS],
		     const struct rtl_fe_vlan vlan[2], unsigned int count)
{
	unsigned int i;

	rtl_fe_key_set(words, RTL_FE_KEY_VLAN_CNT, count);
	for (i = 0; i < count; i++) {
		rtl_fe_key_set(words, i ? RTL_FE_KEY_INNER_VID : RTL_FE_KEY_TOP_VID,
			       vlan[i].vid);
		rtl_fe_key_set(words, i ? RTL_FE_KEY_INNER_TPID_ENC :
			       RTL_FE_KEY_TOP_TPID_ENC, vlan[i].tpid);
	}
}

static void rtl_fe_key_common(u32 words[RTL_FE_KEY_WORDS],
			      const struct rtl_fe_key *key)
{
	rtl_fe_key_set(words, RTL_FE_KEY_POL_ID, key->llid);
	rtl_fe_key_set(words, RTL_FE_KEY_LSPID, key->ingress);
	rtl_fe_key_set(words, RTL_FE_KEY_HKEY_ID, key->mask);
	rtl_fe_key_set(words, RTL_FE_KEY_TABLE_ID, 1);
}

void rtl_fe_key_ip(u32 words[RTL_FE_KEY_WORDS], const struct rtl_fe_key *key,
		   bool ipv6)
{
	const struct rtl_fe_field_layout *da = &rtl_fe_key_fields[RTL_FE_KEY_IP_DA];
	const struct rtl_fe_field_layout *sa = &rtl_fe_key_fields[RTL_FE_KEY_IP_SA];
	unsigned int i;

	memset(words, 0, RTL_FE_KEY_WORDS * sizeof(*words));
	rtl_fe_key_set(words, RTL_FE_KEY_L4_SP, key->source_port);
	rtl_fe_key_set(words, RTL_FE_KEY_L4_DP, key->destination_port);
	rtl_fe_key_set(words, RTL_FE_KEY_IP_SA, key->source[3]);
	rtl_fe_key_set(words, RTL_FE_KEY_IP_DA, key->destination[3]);
	rtl_fe_key_set(words, RTL_FE_KEY_IP_PROTOCOL, key->protocol);
	rtl_fe_key_set(words, RTL_FE_KEY_IP_VLD, 1);
	rtl_fe_key_set(words, RTL_FE_KEY_ORIG_LSPID, key->ingress);
	rtl_fe_key_set(words, RTL_FE_KEY_CTRL_SET_ID, key->profile);
	rtl_fe_key_common(words, key);

	if (!ipv6)
		return;

	rtl_fe_key_set(words, RTL_FE_KEY_IP_VER, 1);
	for (i = 1; i < 4; i++) {
		rtl_fe_bits_set(words, da->bit + i * RTL_FE_KEY_IP_WORD_BITS,
				RTL_FE_KEY_IP_WORD_BITS, key->destination[3 - i]);
		rtl_fe_bits_set(words, sa->bit + i * RTL_FE_KEY_IP_WORD_BITS,
				RTL_FE_KEY_IP_WORD_BITS, key->source[3 - i]);
	}
}

void rtl_fe_key_bridge(u32 words[RTL_FE_KEY_WORDS], const u8 *source,
		       const u8 *destination, u16 protocol,
		       const struct rtl_fe_key *key)
{
	unsigned int i;

	memset(words, 0, RTL_FE_KEY_WORDS * sizeof(*words));
	for (i = 0; i < ETH_ALEN; i++) {
		rtl_fe_bits_set(words, RTL_FE_KEY_MAC_SA_BIT + (ETH_ALEN - 1 - i) * 8,
				8, source[i]);
		rtl_fe_bits_set(words, RTL_FE_KEY_MAC_DA_BIT + (ETH_ALEN - 1 - i) * 8,
				8, destination[i]);
	}
	rtl_fe_key_set(words, RTL_FE_KEY_ETHERTYPE, protocol);
	rtl_fe_key_common(words, key);
}

void rtl_fe_key_crc(const u32 words[RTL_FE_KEY_WORDS], u32 *crc32, u16 *crc16)
{
	u32 c32 = ~0U, c16 = 0xffff;
	unsigned int byte, bit;

	for (byte = RTL_FE_KEY_WORDS * 4; byte; byte--) {
		unsigned int index = byte - 1;
		u8 val = bitrev8((words[index / 4] >> ((index % 4) * 8)) & 0xff);

		c32 ^= val;
		c16 ^= val;
		for (bit = 0; bit < 8; bit++) {
			c32 = (c32 >> 1) ^ ((c32 & 1) ? CRC32_POLY_LE : 0);
			c16 = (c16 >> 1) ^ ((c16 & 1) ? 0x8408 : 0);
		}
	}
	*crc32 = bitrev32(c32);
	*crc16 = bitrev16(c16);
}

void rtl_fe_mask_l3(u32 words[RTL_FE_MASK_WORDS])
{
	unsigned int i;

	for (i = 0; i < 7; i++)
		words[i] = ~0U;
	words[7] = 0;

	words[0] &= ~(GENMASK(26, 10) | GENMASK(31, 27));
	words[1] &= ~(GENMASK(24, 23) | GENMASK(11, 0) | BIT(12) | BIT(20) |
		      BIT(22));
	words[2] = (words[2] & ~GENMASK(11, 3)) | BIT(8);
	words[1] = (words[1] & ~GENMASK(31, 26)) | BIT(31);
	words[2] &= ~(GENMASK(2, 0) | BIT(13) | BIT(23) | BIT(22));
	words[6] &= ~(BIT(23) | BIT(24) | BIT(9) | BIT(26));
	rtl_fe_bits_set(words, 217, 1, 0);

	rtl_fe_bits_set(words, 58, 9, 128);
	rtl_fe_bits_set(words, 67, 9, 128);
}

void rtl_fe_mask_bridge(u32 words[RTL_FE_MASK_WORDS])
{
	static const struct rtl_fe_field_layout clear[] = {
		{ 55, 2 }, { 58, 9 }, { 67, 9 }, { 89, 1 }, { 91, 1 }, { 94, 2 },
		{ 98, 3 }, { 109, 7 }, { 120, 6 }, { 201, 1 }, { 216, 1 },
		{ 217, 1 },
	};
	unsigned int i;

	for (i = 0; i < 7; i++)
		words[i] = ~0U;
	words[7] = 0;
	for (i = 0; i < ARRAY_SIZE(clear); i++)
		rtl_fe_bits_set(words, clear[i].bit, clear[i].width, 0);
}

unsigned int rtl_fe_hash_bucket(u16 crc16)
{
	return crc16 & 0xffe0;
}

void rtl_fe_cpu_miss(u32 words[4])
{
	memset(words, 0, 4 * sizeof(*words));
	rtl_fe_bits_set(words, 54, 1, 1);
	rtl_fe_bits_set(words, 55, 1, 1);
}

void rtl_fe_classify_hash(u32 words[RTL_FE_CLASSIFIER_WORDS],
			  unsigned int profile, unsigned int source)
{
	memset(words, 0, RTL_FE_CLASSIFIER_WORDS * sizeof(*words));
	rtl_fe_bits_set(words, 203, 1, 1);
	rtl_fe_bits_set(words, 204, 4, profile);
	rtl_fe_bits_set(words, 186, 1, 1);
	rtl_fe_bits_set(words, 187, 1, 0);
	rtl_fe_bits_set(words, 154, 1, 1);
	rtl_fe_bits_set(words, 155, 1, 1);
	rtl_fe_bits_set(words, 156, 1, 1);
	rtl_fe_bits_set(words, 158, 10, 16);
	rtl_fe_bits_set(words, 208, 1, 1);
	rtl_fe_bits_set(words, 209, 6, source);
}

void rtl_fe_ingress_lpb(u32 words[RTL_FE_LPB_WORDS], unsigned int source,
			unsigned int destination)
{
	memset(words, 0, RTL_FE_LPB_WORDS * sizeof(*words));
	rtl_fe_bits_set(words, 24, 14, 1700);
	rtl_fe_bits_set(words, 38, 4, 15);
	rtl_fe_bits_set(words, 42, 4, 0);
	rtl_fe_bits_set(words, 68, 6, destination);
	rtl_fe_bits_set(words, 88, 6, source);
}

void rtl_fe_fdb_key(u32 words[RTL_FE_FDB_WORDS], const u8 *mac)
{
	unsigned int i;

	memset(words, 0, RTL_FE_FDB_WORDS * sizeof(*words));
	for (i = 0; i < ETH_ALEN; i++)
		rtl_fe_bits_set(words, FE_FDB_MAC_BIT + (ETH_ALEN - 1 - i) * 8, 8,
				mac[i]);
	words[0] = FE_FDB_KEY_INIT;
}

int rtl_fe_indirect(struct rtl_fe *fe, unsigned int reg, u32 cmd)
{
	u32 busy = (reg == RTL_FE_CACHE_CMD || reg == RTL_FE_SYNC_CMD) ?
		   RTL_NI_INDIRECT_BUSY_LOW : RTL_NI_INDIRECT_BUSY;

	return rtl_ne_indirect(fe->regs, reg, cmd, busy);
}

static int rtl_fe_table_read(struct rtl_fe *fe, unsigned int cmd_reg, u32 cmd,
			     unsigned int data_reg, u32 *words, unsigned int count)
{
	unsigned int i;
	int ret;

	ret = rtl_fe_indirect(fe, cmd_reg, RTL_NI_INDIRECT_ACCESS | cmd);
	if (ret)
		return ret;
	for (i = 0; i < count; i++)
		words[i] = rtl_fe_read(fe, data_reg - i * 4);
	return 0;
}

static int rtl_fe_table_write(struct rtl_fe *fe, unsigned int cmd_reg, u32 cmd,
			      unsigned int data_reg, const u32 *words,
			      unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		rtl_fe_write(fe, data_reg - i * 4, words[i]);
	return rtl_fe_indirect(fe, cmd_reg, RTL_NI_INDIRECT_ACCESS |
			       RTL_NI_INDIRECT_WRITE | cmd);
}

int rtl_fe_mask_write(struct rtl_fe *fe, unsigned int index,
		      const u32 words[RTL_FE_MASK_WORDS])
{
	unsigned int bank;
	int ret;

	for (bank = 0; bank < 2; bank++) {
		ret = rtl_fe_table_write(fe, RTL_FE_MASK_CMD, (bank << 6) | index,
					 RTL_FE_MASK_DATA, words + bank * 4, 4);
		if (ret)
			return ret;
	}
	return 0;
}

int rtl_fe_mask_read(struct rtl_fe *fe, unsigned int index,
		     u32 words[RTL_FE_MASK_WORDS])
{
	unsigned int bank;
	int ret;

	for (bank = 0; bank < 2; bank++) {
		ret = rtl_fe_table_read(fe, RTL_FE_MASK_CMD, (bank << 6) | index,
					RTL_FE_MASK_DATA, words + bank * 4, 4);
		if (ret)
			return ret;
	}
	return 0;
}

void rtl_fe_dma_tables(struct rtl_fe *fe, u32 address, u32 address_high)
{
	rtl_fe_write(fe, RTL_FE_HASH_ENABLE, 0);
	rtl_fe_write(fe, RTL_FE_HASH_BASE_HIGH, address_high);
	rtl_fe_write(fe, RTL_FE_HASH_BASE_LOW, address);
	rtl_fe_write(fe, RTL_FE_ACTION_BASE_HIGH, address_high);
	rtl_fe_write(fe, RTL_FE_ACTION_BASE_LOW, address + RTL_FE_ACTION_OFFSET);
	rtl_fe_write(fe, RTL_FE_OVERFLOW_BASE_HIGH, address_high);
	rtl_fe_write(fe, RTL_FE_OVERFLOW_BASE_LOW, address + RTL_FE_OVERFLOW_OFFSET);
	rtl_fe_write(fe, RTL_FE_DEFAULT_BASE_HIGH, address_high);
	rtl_fe_write(fe, RTL_FE_DEFAULT_BASE_LOW, address + RTL_FE_DEFAULT_OFFSET);
	rtl_fe_rmw(fe, RTL_FE_BUCKET_CFG, 0x300ff, 3 | (7 << 2) | (4 << 5));
	rtl_fe_write(fe, RTL_FE_ACTION_SLOT_CFG, 4);
	rtl_fe_write(fe, RTL_FE_DEFAULT_SLOT_CFG, 1);
	rtl_fe_write(fe, RTL_FE_HASH_CFG, BIT(18) | BIT(16) | (3 << 8) | 4);
}

void rtl_fe_cache_start(struct rtl_fe *fe)
{
	rtl_fe_rmw(fe, RTL_FE_AXI_PATCH, 0, 0x80000001);
	rtl_fe_rmw(fe, RTL_FE_AXI_ID1_LIMIT, 0, 1);
	rtl_fe_rmw(fe, RTL_FE_AXI_OUTSTANDING, 0, 255);
	rtl_fe_rmw(fe, RTL_FE_MISS_TEMPLATE, GENMASK(15, 0), 0xd0d0);
	rtl_fe_write(fe, RTL_FE_CACHE_CFG, 0);
	rtl_fe_write(fe, RTL_FE_KEY_MASK_0, ~0U);
	rtl_fe_write(fe, RTL_FE_KEY_MASK_1, ~0U);
	rtl_fe_write(fe, RTL_FE_HASH_ENABLE, 0xa0000000);
}

static unsigned int rtl_fe_age_reg(unsigned int index)
{
	return RTL_FE_AGE_DATA - ((index % FE_AGE_SLOTS) / FE_AGE_PER_REG) * 4;
}

static unsigned int rtl_fe_age_shift(unsigned int index)
{
	return (index % FE_AGE_PER_REG) * FE_AGE_BITS;
}

int rtl_fe_age_write(struct rtl_fe *fe, unsigned int index, unsigned int age)
{
	unsigned int row = index / FE_AGE_SLOTS;
	int ret;

	ret = rtl_fe_indirect(fe, RTL_FE_AGE_CMD, RTL_NI_INDIRECT_ACCESS | row);
	if (ret)
		return ret;
	rtl_fe_rmw(fe, rtl_fe_age_reg(index), GENMASK(3, 0) << rtl_fe_age_shift(index),
		   (age & GENMASK(3, 0)) << rtl_fe_age_shift(index));
	return rtl_fe_indirect(fe, RTL_FE_AGE_CMD, RTL_NI_INDIRECT_ACCESS |
			       RTL_NI_INDIRECT_WRITE | row);
}

int rtl_fe_age_read(struct rtl_fe *fe, unsigned int index, unsigned int *age)
{
	int ret;

	ret = rtl_fe_indirect(fe, RTL_FE_AGE_CMD, RTL_NI_INDIRECT_ACCESS |
			      index / FE_AGE_SLOTS);
	if (ret)
		return ret;
	*age = (rtl_fe_read(fe, rtl_fe_age_reg(index)) >> rtl_fe_age_shift(index)) &
	       GENMASK(3, 0);
	return 0;
}

int rtl_fe_cache_command(struct rtl_fe *fe, unsigned int index, u16 crc16,
			 unsigned int command)
{
	rtl_fe_write(fe, RTL_FE_CACHE_DATA, (index % FE_AGE_SLOTS) |
		     ((u32)crc16 << 5) | FE_CACHE_VALID | (command << 30));
	return rtl_fe_indirect(fe, RTL_FE_CACHE_CMD, 1);
}

int rtl_fe_cam_protocol_get(struct rtl_fe *fe, struct rtl_fe_cam_protocol *cam,
			    unsigned int bank, unsigned int protocol)
{
	unsigned int count = bank == 2 ? 63 : 15;
	unsigned int width = bank == 2 ? 5 : 3;
	unsigned int index, free = count, i;
	u32 words[RTL_FE_CAM_WORDS];
	int ret;

	cam->bank = bank;
	cam->created = false;
	for (index = 0; index < count; index++) {
		ret = rtl_fe_table_read(fe, RTL_FE_CAM_CMD, (bank << 16) | index,
					RTL_FE_CAM_DATA, words, width);
		if (ret)
			return ret;
		if (words[0] & BIT(16)) {
			if ((words[0] & 0xffff) == protocol) {
				cam->index = index;
				return index + 1;
			}
		} else if (free == count) {
			free = index;
			memcpy(cam->saved, words, width * sizeof(*words));
		}
	}
	if (free == count)
		return -ENOSPC;

	cam->index = free;
	cam->created = true;
	memset(words, 0, sizeof(words));
	words[0] = protocol | BIT(16);
	for (i = 0; i < width; i++)
		rtl_fe_write(fe, RTL_FE_CAM_DATA - i * 4, words[i]);
	ret = rtl_fe_indirect(fe, RTL_FE_CAM_CMD, RTL_NI_INDIRECT_ACCESS |
			      RTL_NI_INDIRECT_WRITE | (bank << 16) | free);
	if (ret)
		return ret;
	return free + 1;
}

int rtl_fe_cam_protocol_put(struct rtl_fe *fe, struct rtl_fe_cam_protocol *cam)
{
	int ret;

	if (!cam->created)
		return 0;
	ret = rtl_fe_table_write(fe, RTL_FE_CAM_CMD, (cam->bank << 16) | cam->index,
				 RTL_FE_CAM_DATA, cam->saved,
				 cam->bank == 2 ? 5 : 3);
	if (!ret)
		cam->created = false;
	return ret;
}

int rtl_fe_classifier_read(struct rtl_fe *fe, unsigned int index,
			   u32 words[RTL_FE_CLASSIFIER_WORDS])
{
	return rtl_fe_table_read(fe, RTL_FE_CLASSIFIER_CMD, index,
				 RTL_FE_CLASSIFIER_DATA, words,
				 RTL_FE_CLASSIFIER_WORDS);
}

int rtl_fe_classifier_write(struct rtl_fe *fe, unsigned int index,
			    const u32 words[RTL_FE_CLASSIFIER_WORDS])
{
	return rtl_fe_table_write(fe, RTL_FE_CLASSIFIER_CMD, index,
				  RTL_FE_CLASSIFIER_DATA, words,
				  RTL_FE_CLASSIFIER_WORDS);
}

int rtl_fe_smac_read(struct rtl_fe *fe, unsigned int index,
		     u32 words[RTL_FE_SMAC_WORDS])
{
	return rtl_fe_table_read(fe, RTL_FE_CAM_CMD, (3 << 16) | index,
				 RTL_FE_CAM_DATA, words, RTL_FE_SMAC_WORDS);
}

int rtl_fe_smac_write(struct rtl_fe *fe, unsigned int index,
		      const u32 words[RTL_FE_SMAC_WORDS])
{
	return rtl_fe_table_write(fe, RTL_FE_CAM_CMD, (3 << 16) | index,
				  RTL_FE_CAM_DATA, words, RTL_FE_SMAC_WORDS);
}

int rtl_fe_counter_read(struct rtl_fe *fe, unsigned int index,
			u32 words[RTL_FE_COUNTER_WORDS])
{
	return rtl_fe_table_read(fe, RTL_FE_COUNTER_CMD, index,
				 RTL_FE_COUNTER_DATA, words, RTL_FE_COUNTER_WORDS);
}

int rtl_fe_fdb_command(struct rtl_fe *fe, unsigned int command,
		       u32 words[RTL_FE_FDB_WORDS], u32 *result)
{
	u32 control = rtl_fe_read(fe, RTL_NI_FDB_CONTROL);
	unsigned int i;
	int ret;

	rtl_fe_write(fe, RTL_NI_FDB_CONTROL, control | BIT(31));
	for (i = 0; i < RTL_FE_FDB_WORDS; i++)
		rtl_fe_write(fe, RTL_NI_FDB_DATA - i * 4, words[i]);
	rtl_fe_write(fe, RTL_NI_FDB_RESULT, 0);
	ret = rtl_fe_indirect(fe, RTL_NI_FDB_CMD, RTL_NI_INDIRECT_ACCESS | command);
	rtl_fe_write(fe, RTL_NI_FDB_CONTROL, control);
	if (ret)
		return ret;

	*result = rtl_fe_read(fe, RTL_NI_FDB_RESULT);
	for (i = 0; i < RTL_FE_FDB_WORDS; i++)
		words[i] = rtl_fe_read(fe, RTL_NI_FDB_DATA - i * 4);
	return 0;
}
