// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NAPTV6 action mode support and NPTv6 prefix table manager.
 *
 * RE-6 (phase3-re6-napt6.md) recovered the 371-bit hash-action layout from
 * convert_act_flow_naptv6_mode() (ca-ne .text+0x93250) and the 16-entry
 * NPTv6 prefix table from npt6_prefix_addr_table (ca-ne .data+0x4258).
 *
 * Verified on RTL9617C silicon:
 *  - Enabling the "napt6" module parameter switches FE hash-action mode to
 *    384-bit entries (RTL_FE_ACTION_BYTES = 48 B, CACHE_CTRL bit 17 halving
 *    hash cache capacity, action group bitmask 0x0114000a) and reloads live;
 *  - Full 128-bit SNAT6 / DNAT6, TCP/UDP PAT6, and automatic hardware L4
 *    checksum recalculation operate at wire speed;
 *  - The 16-entry NPTv6 prefix table at NE+0x3a24..0x3aa4 supports direct
 *    read/write via the "napt6" debugfs file.
 */

#include <linux/bitmap.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "fe_priv.h"
#include "fe_napt6.h"

/* --- testable core ---
 *
 * tests/napt6-table.py compiles this region against a mock NE window.
 */

#define RTL_FE_NAPT6_WORD_BITS		32
#define RTL_FE_NPT6_PREFIX_STRIDE	8
#define RTL_FE_NPT6_WORD_BYTES		4
#define RTL_FE_NPT6_PREFIX_HIGH_SHIFT	32

static void rtl_fe_napt6_bits_set(u32 *words, unsigned int bit,
				  unsigned int width, u32 value)
{
	unsigned int i;

	for (i = 0; i < width; i++) {
		unsigned int pos = bit + i;
		u32 mask = BIT(pos % RTL_FE_NAPT6_WORD_BITS);

		if (value & BIT(i))
			words[pos / RTL_FE_NAPT6_WORD_BITS] |= mask;
		else
			words[pos / RTL_FE_NAPT6_WORD_BITS] &= ~mask;
	}
}

static u32 rtl_fe_napt6_bits_get(const u32 *words, unsigned int bit,
				 unsigned int width)
{
	u32 value = 0;
	unsigned int i;

	for (i = 0; i < width; i++) {
		unsigned int pos = bit + i;

		if (words[pos / RTL_FE_NAPT6_WORD_BITS] &
		    BIT(pos % RTL_FE_NAPT6_WORD_BITS))
			value |= BIT(i);
	}
	return value;
}

struct rtl_fe_napt6_field {
	const char *name;
	u16 bit;
	u8 width;
	u16 offset;
};

#define RTL_FE_NAPT6_FIELD(_name, _bit, _width)				\
	{ #_name, (_bit), (_width),					\
	  offsetof(struct rtl_fe_naptv6_action, _name) }

static const struct rtl_fe_napt6_field rtl_fe_napt6_fields[] = {
	/* Group 1 + Group 3: inserted ahead of the baseline G18/G20 block. */
	RTL_FE_NAPT6_FIELD(l4_dp_vld, 0, 1),
	RTL_FE_NAPT6_FIELD(l4_dp, 1, 16),
	RTL_FE_NAPT6_FIELD(ip_da_l_vld, 17, 1),
	RTL_FE_NAPT6_FIELD(ip_da_l_32, 18, 32),
	/* Group 18 (bits 50..68) and Group 20 (bits 69..273). */
	RTL_FE_NAPT6_FIELD(mrr_vld, 50, 1),
	RTL_FE_NAPT6_FIELD(mrr_en, 51, 1),
	RTL_FE_NAPT6_FIELD(no_drop_vld, 52, 1),
	RTL_FE_NAPT6_FIELD(no_drop, 53, 1),
	RTL_FE_NAPT6_FIELD(dpid_vld, 54, 1),
	RTL_FE_NAPT6_FIELD(dpid_pri, 55, 1),
	RTL_FE_NAPT6_FIELD(permit, 56, 1),
	RTL_FE_NAPT6_FIELD(deepq, 57, 1),
	RTL_FE_NAPT6_FIELD(mcgid_ldpid, 58, 10),
	RTL_FE_NAPT6_FIELD(mc, 68, 1),
	RTL_FE_NAPT6_FIELD(mdata_byte_vld, 69, 1),
	RTL_FE_NAPT6_FIELD(mdata_byte, 70, 8),
	RTL_FE_NAPT6_FIELD(l3_if_vld, 78, 1),
	RTL_FE_NAPT6_FIELD(smac_trans, 79, 1),
	RTL_FE_NAPT6_FIELD(igr_l3_if_idx, 80, 6),
	RTL_FE_NAPT6_FIELD(egr_l3_if_idx, 86, 6),
	RTL_FE_NAPT6_FIELD(l3_if_counter_en, 92, 1),
	RTL_FE_NAPT6_FIELD(ip_ttl_dec, 93, 1),
	RTL_FE_NAPT6_FIELD(ip_ttl_zero_discard_en, 94, 1),
	RTL_FE_NAPT6_FIELD(ip_addr_vld, 95, 1),
	RTL_FE_NAPT6_FIELD(ip_type, 96, 1),
	RTL_FE_NAPT6_FIELD(ip_addr, 97, 32),
	RTL_FE_NAPT6_FIELD(ip_addr_napt6, 129, 1),
	RTL_FE_NAPT6_FIELD(mac_da_idx_vld, 130, 1),
	RTL_FE_NAPT6_FIELD(mac_da_idx, 131, 13),
	RTL_FE_NAPT6_FIELD(chk_msk_ptr, 144, 6),
	RTL_FE_NAPT6_FIELD(cache_ctrl, 150, 2),
	RTL_FE_NAPT6_FIELD(pop_l3_vld, 152, 1),
	RTL_FE_NAPT6_FIELD(pop_l3_chk_ecn_en, 153, 1),
	RTL_FE_NAPT6_FIELD(pop_l3_en, 154, 1),
	RTL_FE_NAPT6_FIELD(t2_ctrl_vld, 155, 1),
	RTL_FE_NAPT6_FIELD(t2_ctrl, 156, 4),
	RTL_FE_NAPT6_FIELD(ldpid_offset_msb, 160, 1),
	RTL_FE_NAPT6_FIELD(ip_dscp_update_en, 161, 1),
	RTL_FE_NAPT6_FIELD(ip_dscp, 162, 6),
	RTL_FE_NAPT6_FIELD(cos_update_en, 168, 1),
	RTL_FE_NAPT6_FIELD(cos, 169, 3),
	RTL_FE_NAPT6_FIELD(inner_8021p_update_en, 172, 1),
	RTL_FE_NAPT6_FIELD(inner_8021p, 173, 3),
	RTL_FE_NAPT6_FIELD(top_8021p_update_en, 176, 1),
	RTL_FE_NAPT6_FIELD(top_8021p, 177, 3),
	RTL_FE_NAPT6_FIELD(inner_dei, 180, 1),
	RTL_FE_NAPT6_FIELD(inner_vid, 181, 12),
	RTL_FE_NAPT6_FIELD(inner_tpid_enc, 193, 3),
	RTL_FE_NAPT6_FIELD(top_dei, 196, 1),
	RTL_FE_NAPT6_FIELD(top_vid, 197, 12),
	RTL_FE_NAPT6_FIELD(top_tpid_enc, 209, 3),
	RTL_FE_NAPT6_FIELD(vlan_cnt, 212, 2),
	RTL_FE_NAPT6_FIELD(vlan_vld, 214, 1),
	RTL_FE_NAPT6_FIELD(pol_vld, 215, 1),
	RTL_FE_NAPT6_FIELD(pol_en, 216, 1),
	RTL_FE_NAPT6_FIELD(pol_id, 217, 8),
	RTL_FE_NAPT6_FIELD(pol2_id_enable, 225, 1),
	RTL_FE_NAPT6_FIELD(pol2_id, 226, 6),
	RTL_FE_NAPT6_FIELD(pol3_id_enable, 232, 1),
	RTL_FE_NAPT6_FIELD(pol3_id, 233, 6),
	RTL_FE_NAPT6_FIELD(pppoe_vld, 239, 1),
	RTL_FE_NAPT6_FIELD(pppoe_set, 240, 1),
	RTL_FE_NAPT6_FIELD(l4_port, 241, 16),
	RTL_FE_NAPT6_FIELD(ip_mtu_enc_vld, 257, 1),
	RTL_FE_NAPT6_FIELD(ip_mtu_enc, 258, 4),
	RTL_FE_NAPT6_FIELD(modify_vlan_only_vld, 262, 1),
	RTL_FE_NAPT6_FIELD(modify_vlan_only, 263, 1),
	RTL_FE_NAPT6_FIELD(six_rd_dip_remap_or_fmr_idx_vld, 264, 1),
	RTL_FE_NAPT6_FIELD(six_rd_dip_remap_or_fmr_idx, 265, 2),
	RTL_FE_NAPT6_FIELD(vxlan_sport_msb15, 267, 6),
	RTL_FE_NAPT6_FIELD(vxlan_sport_update_en, 273, 1),
	/* Group 24: 1 validity bit + three 32-bit address lanes. */
	RTL_FE_NAPT6_FIELD(ip_addr_h_96_vld, 274, 1),
	RTL_FE_NAPT6_FIELD(ip_addr_h_96_1, 275, 32),
	RTL_FE_NAPT6_FIELD(ip_addr_h_96_2, 307, 32),
	RTL_FE_NAPT6_FIELD(ip_addr_h_96_3, 339, 32),
};

void rtl_fe_napt6_pack(u32 words[RTL_FE_NAPT6_ACTION_WORDS],
		       const struct rtl_fe_naptv6_action *action)
{
	unsigned int i;

	memset(words, 0, RTL_FE_NAPT6_ACTION_BYTES);
	for (i = 0; i < ARRAY_SIZE(rtl_fe_napt6_fields); i++) {
		const struct rtl_fe_napt6_field *field =
			&rtl_fe_napt6_fields[i];
		u32 value;

		memcpy(&value, (const u8 *)action + field->offset, sizeof(value));
		rtl_fe_napt6_bits_set(words, field->bit, field->width, value);
	}
}

void rtl_fe_napt6_unpack(const u32 words[RTL_FE_NAPT6_ACTION_WORDS],
			 struct rtl_fe_naptv6_action *action)
{
	unsigned int i;

	memset(action, 0, sizeof(*action));
	for (i = 0; i < ARRAY_SIZE(rtl_fe_napt6_fields); i++) {
		const struct rtl_fe_napt6_field *field =
			&rtl_fe_napt6_fields[i];
		u32 value = rtl_fe_napt6_bits_get(words, field->bit,
						  field->width);

		memcpy((u8 *)action + field->offset, &value, sizeof(value));
	}
}

static unsigned int rtl_fe_napt6_prefix_reg(unsigned int index,
					    unsigned int word)
{
	return RTL_FE_NPT6_PREFIX_DATA +
	       index * RTL_FE_NPT6_PREFIX_STRIDE +
	       word * RTL_FE_NPT6_WORD_BYTES;
}

static int rtl_fe_napt6_prefix_write(struct rtl_fe *fe, unsigned int index,
				     u64 prefix)
{
	u32 low = (u32)prefix;
	u32 high = (u32)(prefix >> RTL_FE_NPT6_PREFIX_HIGH_SHIFT);
	u32 read_low, read_high;

	/* __npt6_prefix_addr_hw_entry_set() puts dmb oshst before each word. */
	wmb();
	rtl_fe_write(fe, rtl_fe_napt6_prefix_reg(index, 0), low);
	wmb();	/* dmb oshst */
	rtl_fe_write(fe, rtl_fe_napt6_prefix_reg(index, 1), high);
	wmb();	/* dmb oshst */

	rmb();	/* dmb oshld */
	read_low = rtl_fe_read(fe, rtl_fe_napt6_prefix_reg(index, 0));
	rmb();	/* dmb oshld */
	read_high = rtl_fe_read(fe, rtl_fe_napt6_prefix_reg(index, 1));
	if (read_low != low || read_high != high)
		return -EIO;
	return 0;
}

static int rtl_fe_napt6_prefix_read(struct rtl_fe *fe, unsigned int index,
				    u64 *prefix)
{
	u32 low, high;

	rmb();	/* dmb oshld */
	low = rtl_fe_read(fe, rtl_fe_napt6_prefix_reg(index, 0));
	rmb();	/* dmb oshld */
	high = rtl_fe_read(fe, rtl_fe_napt6_prefix_reg(index, 1));
	*prefix = (u64)low | ((u64)high << RTL_FE_NPT6_PREFIX_HIGH_SHIFT);
	return 0;
}

int rtl_fe_napt6_prefix_add(struct rtl_fe *fe, u64 prefix,
			    unsigned int *index)
{
	struct rtl_fe_napt6 *n;
	unsigned int i;
	int ret;

	if (!fe || !fe->napt6)
		return -ENODEV;
	if (!index)
		return -EINVAL;
	n = fe->napt6;

	mutex_lock(&n->lock);
	for (i = 0; i < RTL_FE_NPT6_PREFIX_ENTRIES; i++) {
		if (!n->prefixes[i].refs ||
		    n->prefixes[i].prefix != prefix)
			continue;
		n->prefixes[i].refs++;
		*index = i;
		ret = 0;
		goto unlock;
	}
	for (i = 0; i < RTL_FE_NPT6_PREFIX_ENTRIES; i++) {
		if (n->prefixes[i].refs)
			continue;
		ret = rtl_fe_napt6_prefix_write(fe, i, prefix);
		if (ret)
			goto unlock;
		n->prefixes[i].prefix = prefix;
		n->prefixes[i].refs = 1;
		*index = i;
		ret = 0;
		goto unlock;
	}
	ret = -ENOSPC;
unlock:
	mutex_unlock(&n->lock);
	return ret;
}

int rtl_fe_napt6_prefix_del(struct rtl_fe *fe, unsigned int index)
{
	struct rtl_fe_napt6 *n;
	int ret;

	if (!fe || !fe->napt6)
		return -ENODEV;
	if (index >= RTL_FE_NPT6_PREFIX_ENTRIES)
		return -EINVAL;
	n = fe->napt6;

	mutex_lock(&n->lock);
	if (!n->prefixes[index].refs) {
		ret = -ENOENT;
		goto unlock;
	}
	if (n->prefixes[index].refs > 1) {
		n->prefixes[index].refs--;
		ret = 0;
		goto unlock;
	}
	ret = rtl_fe_napt6_prefix_write(fe, index, 0);
	if (ret)
		goto unlock;
	n->prefixes[index].prefix = 0;
	n->prefixes[index].refs = 0;
	ret = 0;
unlock:
	mutex_unlock(&n->lock);
	return ret;
}

int rtl_fe_napt6_prefix_get(struct rtl_fe *fe, unsigned int index,
			    u64 *prefix, unsigned int *refs)
{
	struct rtl_fe_napt6 *n;
	int ret;

	if (!fe || !fe->napt6)
		return -ENODEV;
	if (index >= RTL_FE_NPT6_PREFIX_ENTRIES || !prefix || !refs)
		return -EINVAL;
	n = fe->napt6;

	mutex_lock(&n->lock);
	ret = rtl_fe_napt6_prefix_read(fe, index, prefix);
	if (!ret)
		*refs = n->prefixes[index].refs;
	mutex_unlock(&n->lock);
	return ret;
}

/* --- debugfs --- */

#define RTL_FE_NAPT6_HEX_PER_BYTE	2
#define RTL_FE_NAPT6_HEX_PER_WORD	8
#define RTL_FE_NAPT6_NIBBLE_BITS	4
#define RTL_FE_NAPT6_FIELDS_PER_LINE	4
#define RTL_FE_NAPT6_CMD_BUF_LEN	512

static int rtl_fe_napt6_parse_hex(const char *hex,
				  u32 words[RTL_FE_NAPT6_ACTION_WORDS])
{
	unsigned int i, j;

	if (strlen(hex) != RTL_FE_NAPT6_ACTION_BYTES * RTL_FE_NAPT6_HEX_PER_BYTE)
		return -EINVAL;
	memset(words, 0, RTL_FE_NAPT6_ACTION_BYTES);
	for (i = 0; i < RTL_FE_NAPT6_ACTION_WORDS; i++) {
		u32 value = 0;

		for (j = 0; j < RTL_FE_NAPT6_HEX_PER_WORD; j++) {
			int nibble = hex_to_bin(hex[i * RTL_FE_NAPT6_HEX_PER_WORD + j]);

			if (nibble < 0)
				return -EINVAL;
			value = (value << RTL_FE_NAPT6_NIBBLE_BITS) | nibble;
		}
		words[i] = value;
	}
	return 0;
}

static void rtl_fe_napt6_seq_action(struct seq_file *s,
				    const struct rtl_fe_napt6 *n)
{
	unsigned int i;

	seq_puts(s, "action words:");
	for (i = 0; i < RTL_FE_NAPT6_ACTION_WORDS; i++)
		seq_printf(s, " %08x", n->action_words[i]);
	seq_putc(s, '\n');
	for (i = 0; i < ARRAY_SIZE(rtl_fe_napt6_fields); i++) {
		const struct rtl_fe_napt6_field *field =
			&rtl_fe_napt6_fields[i];
		u32 value;

		memcpy(&value, (const u8 *)&n->action + field->offset,
		       sizeof(value));
		seq_printf(s, "  %s=%u", field->name, value);
		if ((i + 1) % RTL_FE_NAPT6_FIELDS_PER_LINE == 0)
			seq_putc(s, '\n');
	}
	if (ARRAY_SIZE(rtl_fe_napt6_fields) % RTL_FE_NAPT6_FIELDS_PER_LINE)
		seq_putc(s, '\n');
}

static int rtl_fe_napt6_show(struct seq_file *s, void *unused)
{
	struct rtl_fe *fe = s->private;
	struct rtl_fe_napt6 *n = fe->napt6;
	u64 prefix;
	unsigned int refs;
	unsigned int i;
	int ret;

	if (!n)
		return -ENODEV;

	mutex_lock(&n->lock);
	seq_printf(s, "ready=1 mode=%s hw_napt6=%u last: %s\n",
		   rtl_fe_napt6_mode_enabled() ? "napt6" : "normal",
		   fe->napt6_active, n->last);
	for (i = 0; i < RTL_FE_NPT6_PREFIX_ENTRIES; i++) {
		ret = rtl_fe_napt6_prefix_read(fe, i, &prefix);
		refs = n->prefixes[i].refs;
		if (ret)
			seq_printf(s, "prefix[%u]: read failed %d refs=%u\n",
				   i, ret, refs);
		else
			seq_printf(s, "prefix[%u]: refs=%u sw=%016llx hw=%016llx\n",
				   i, refs,
				   (unsigned long long)n->prefixes[i].prefix,
				   (unsigned long long)prefix);
	}
	if (n->action_valid)
		rtl_fe_napt6_seq_action(s, n);
	else
		seq_puts(s, "action: not set\n");
	mutex_unlock(&n->lock);
	return 0;
}

static char *rtl_fe_napt6_token(char **cursor)
{
	char *token, *p = *cursor;

	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (!*p) {
		*cursor = p;
		return NULL;
	}
	token = p;
	while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
		p++;
	if (*p)
		*p++ = '\0';
	*cursor = p;
	return token;
}

/* Usage, one command per write (the read side dumps the prefix table and the
 * last action):
 *   dump
 *   action-raw <96 hex digits>   12-word / 48-byte action, word0 first
 *   pack                         pack the last unpacked action and compare
 *   prefix-add <64-bit prefix>   linear allocation / refcount increment
 *   prefix-del <idx>             refcount decrement, zero-write at zero
 *   prefix-get <idx>             read the 64-bit prefix back
 */
static ssize_t rtl_fe_napt6_debugfs_write(struct file *file,
					  const char __user *ubuf,
					  size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct rtl_fe *fe = s->private;
	struct rtl_fe_napt6 *n = fe->napt6;
	char buf[RTL_FE_NAPT6_CMD_BUF_LEN], *cursor, *cmd, *token;
	u32 words[RTL_FE_NAPT6_ACTION_WORDS];
	u64 prefix;
	unsigned int index, refs;
	int ret;

	if (!n)
		return -ENODEV;
	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	cursor = buf;
	cmd = rtl_fe_napt6_token(&cursor);
	if (!cmd)
		return -EINVAL;

	if (sysfs_streq(cmd, "dump")) {
		if (rtl_fe_napt6_token(&cursor))
			return -EINVAL;
		return count;
	}

	if (sysfs_streq(cmd, "pack")) {
		if (rtl_fe_napt6_token(&cursor))
			return -EINVAL;
		mutex_lock(&n->lock);
		rtl_fe_napt6_pack(words, &n->action);
		if (!memcmp(words, n->action_words,
			    RTL_FE_NAPT6_ACTION_BYTES)) {
			snprintf(n->last, sizeof(n->last),
				 "pack ok: 12-word round-trip");
			ret = 0;
		} else {
			snprintf(n->last, sizeof(n->last),
				 "pack mismatch");
			ret = -EIO;
		}
		mutex_unlock(&n->lock);
		return ret ? ret : count;
	}

	token = rtl_fe_napt6_token(&cursor);
	if (!token)
		return -EINVAL;

	if (sysfs_streq(cmd, "action-raw")) {
		if (rtl_fe_napt6_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_napt6_parse_hex(token, words);
		if (ret)
			goto failed;
		mutex_lock(&n->lock);
		memcpy(n->action_words, words, RTL_FE_NAPT6_ACTION_BYTES);
		rtl_fe_napt6_unpack(n->action_words, &n->action);
		n->action_valid = true;
		snprintf(n->last, sizeof(n->last), "action-raw ok");
		mutex_unlock(&n->lock);
		return count;
	}

	if (sysfs_streq(cmd, "prefix-add")) {
		if (kstrtou64(token, 0, &prefix) ||
		    rtl_fe_napt6_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_napt6_prefix_add(fe, prefix, &index);
		if (ret)
			goto failed;
		mutex_lock(&n->lock);
		snprintf(n->last, sizeof(n->last),
			 "prefix-add %016llx -> %u", (unsigned long long)prefix,
			 index);
		mutex_unlock(&n->lock);
		return count;
	}

	if (kstrtouint(token, 0, &index))
		return -EINVAL;

	if (sysfs_streq(cmd, "prefix-del")) {
		if (rtl_fe_napt6_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_napt6_prefix_del(fe, index);
		if (ret)
			goto failed;
		mutex_lock(&n->lock);
		snprintf(n->last, sizeof(n->last), "prefix-del %u ok", index);
		mutex_unlock(&n->lock);
		return count;
	}

	if (sysfs_streq(cmd, "prefix-get")) {
		if (rtl_fe_napt6_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_napt6_prefix_get(fe, index, &prefix, &refs);
		if (ret)
			goto failed;
		mutex_lock(&n->lock);
		snprintf(n->last, sizeof(n->last),
			 "prefix-get %u refs=%u hw=%016llx", index, refs,
			 (unsigned long long)prefix);
		mutex_unlock(&n->lock);
		return count;
	}

	return -EINVAL;

failed:
	mutex_lock(&n->lock);
	snprintf(n->last, sizeof(n->last), "%s failed: %d", cmd, ret);
	mutex_unlock(&n->lock);
	return ret;
}

static int rtl_fe_napt6_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl_fe_napt6_show, inode->i_private);
}

static const struct file_operations rtl_fe_napt6_fops = {
	.owner = THIS_MODULE,
	.open = rtl_fe_napt6_open,
	.read = seq_read,
	.write = rtl_fe_napt6_debugfs_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static DEFINE_MUTEX(rtl_fe_napt6_reg_lock);
static struct rtl_fe *rtl_fe_napt6_fe;
static bool rtl_fe_napt6_mode_enable;

static int rtl_fe_napt6_param_set(const char *val,
				  const struct kernel_param *kp)
{
	bool enable, prev;
	int ret;

	ret = kstrtobool(val, &enable);
	if (ret)
		return ret;

	mutex_lock(&rtl_fe_napt6_reg_lock);
	prev = rtl_fe_napt6_mode_enable;
	if (prev != enable) {
		WRITE_ONCE(rtl_fe_napt6_mode_enable, enable);
		if (rtl_fe_napt6_fe) {
			ret = rtl_fe_napt6_reload(rtl_fe_napt6_fe);
			if (ret)
				WRITE_ONCE(rtl_fe_napt6_mode_enable, prev);
		}
	}
	mutex_unlock(&rtl_fe_napt6_reg_lock);
	return ret;
}

static const struct kernel_param_ops rtl_fe_napt6_param_ops = {
	.set = rtl_fe_napt6_param_set,
	.get = param_get_bool,
};
module_param_cb(napt6, &rtl_fe_napt6_param_ops, &rtl_fe_napt6_mode_enable,
		0644);
MODULE_PARM_DESC(napt6,
		 "Enable 384-bit NAPTV6/NAT66 hash action mode (live switch)");

bool rtl_fe_napt6_mode_enabled(void)
{
	return READ_ONCE(rtl_fe_napt6_mode_enable);
}

int rtl_fe_napt6_init(struct rtl_fe *fe)
{
	struct rtl_fe_napt6 *n;

	if (!fe)
		return -EINVAL;
	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n)
		return -ENOMEM;
	n->fe = fe;
	mutex_init(&n->lock);
	snprintf(n->last, sizeof(n->last), "initialised");
	fe->napt6 = n;
	mutex_lock(&rtl_fe_napt6_reg_lock);
	rtl_fe_napt6_fe = fe;
	mutex_unlock(&rtl_fe_napt6_reg_lock);
	return 0;
}

void rtl_fe_napt6_exit(struct rtl_fe *fe)
{
	struct rtl_fe_napt6 *n;

	if (!fe || !fe->napt6)
		return;
	mutex_lock(&rtl_fe_napt6_reg_lock);
	if (rtl_fe_napt6_fe == fe)
		rtl_fe_napt6_fe = NULL;
	mutex_unlock(&rtl_fe_napt6_reg_lock);
	n = fe->napt6;
	fe->napt6 = NULL;
	mutex_destroy(&n->lock);
	kfree(n);
}

void rtl_fe_napt6_debugfs_init(struct rtl_fe *fe)
{
	if (!fe || !fe->napt6 || !fe->debugfs)
		return;
	debugfs_create_file("napt6", 0600, fe->debugfs, fe,
			    &rtl_fe_napt6_fops);
}
