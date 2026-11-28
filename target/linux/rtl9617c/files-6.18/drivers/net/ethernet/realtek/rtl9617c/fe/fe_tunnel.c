// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * L3PE dual-header tunnel table manager.
 *
 * The OEM L3PE keeps three small tables that describe how a tunnel header is
 * inserted or rewritten (spec scratch/specs/phase0-re3-dualhdr-mc-napt6.md):
 *
 *   Content (table id 31): 16 x 64 B raw header bytes, opaque to software
 *   Control (table id 32): 16 x 128 bit field set, 34 named fields
 *   FMR     (table id 33):  3 x 64 B MAP-T forwarding mapping rules
 *
 * All three share one 64-byte data window at NE+0x3548 and are accessed
 * through __l3pe_dual_hw_entry_set()/get() (ca-ne.ko .text+0x6a390/0x6a4c0).
 * The window really is shared: the command word selects the table, so a
 * register access must never interleave with another one.  This file owns
 * that serialisation and the 16/16/3 slot bookkeeping.
 *
 * Milestone 1 scope: table management and debugfs only.  Nothing here
 * installs a flow action; W8b will allocate slots with OWNER_FLOW and fill
 * the tunnel parameters from the flow dissector.
 */

#include <linux/bitmap.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "fe_priv.h"
#include "fe_tunnel.h"

/* --- testable core ---
 *
 * tests/tunnel-table.py compiles everything from this marker up to the
 * debugfs marker against a mock NE window.  Keep this region free of
 * seq_file, kzalloc and dev_* calls so the host compiler can build it.
 */

/* Clear-on-free default.  module_param_named(clear_on_free, ...) in the
 * debugfs section exposes it; tests set it directly.
 */
static bool rtl_fe_dual_clear_on_free = true;

#define RTL_FE_DUAL_WORD_BITS		32
#define RTL_FE_DUAL_CLEAR_FILL_FMR	0xffffffffU
#define RTL_FE_DUAL_CLEAR_FILL_ZERO	0U

#define RTL_FE_DUAL_HEX_ALPHA_BASE	10
#define RTL_FE_DUAL_HEX_PER_BYTE	2
#define RTL_FE_DUAL_HEX_PER_WORD	8
#define RTL_FE_DUAL_NIBBLE_BITS		4

#ifndef RTL_FE_EIF_PHYS_ROWS
#define RTL_FE_EIF_PHYS_ROWS		32
#endif
#ifndef RTL_FE_EIF_EXCLUSIVE_ROWS
#define RTL_FE_EIF_EXCLUSIVE_ROWS	16
#endif
#ifndef RTL_FE_EIF_ALIAS_BASE
#define RTL_FE_EIF_ALIAS_BASE		16
#endif
#ifndef RTL_FE_EIF_ALIAS_MASK
#define RTL_FE_EIF_ALIAS_MASK		0xf
#endif

#define RTL_FE_DUAL_DSLITE_HDR_LEN	40
#define RTL_FE_DUAL_DSLITE_INR_TO_OUT	0x30
/* DS-Lite decap requires bit 7 (0x80) in inr_to_out_fld (matching 6RD/PPTP
 * 0xb0, MAP-T 0x80, L2TP 0xf0): with bit 4 set and bit 7 clear (0x30), L3FE
 * STG1 drops DS-Lite decapsulated frames instead of forwarding them to Hash.
 */
#define RTL_FE_DUAL_DSLITE_DECAP_INR_TO_OUT 0xb0
#define RTL_FE_DUAL_DSLITE_SRC_WORD	2
#define RTL_FE_DUAL_DSLITE_DST_WORD	6
#define RTL_FE_DUAL_DSLITE_ADDR_BYTES	16

#define RTL_FE_DUAL_6RD_HDR_LEN		20
#define RTL_FE_DUAL_6RD_PROTO_WORD	2
#define RTL_FE_DUAL_6RD_SRC_WORD	3
#define RTL_FE_DUAL_6RD_DST_WORD	4
#define RTL_FE_DUAL_6RD_ADDR_BYTES	4
#define RTL_FE_DUAL_ALT_EIF_BASE	48
#define RTL_FE_DUAL_6RD_REP_LEN_OST1	2
#define RTL_FE_DUAL_6RD_REP_LEN_DIFF1	20
#define RTL_FE_DUAL_6RD_REP_LEN_DIFF2	16
#define RTL_FE_DUAL_6RD_REP_IP_LEN_DIFF	20
#define RTL_FE_DUAL_6RD_SUF_STR_BIT	32
#define RTL_FE_DUAL_6RD_INR_TO_OUT	0xb0

#define RTL_FE_DUAL_MAPT_GRE_SEQ_ACK_OST 120
#define RTL_FE_DUAL_MAPT_IP6_OFT	30
#define RTL_FE_DUAL_MAPT_PRE_LEN	7
#define RTL_FE_DUAL_MAPT_6RD_PRE_LEN	31
#define RTL_FE_DUAL_MAPT_6RD_SUF_STR_BIT 48
#define RTL_FE_DUAL_MAPT_INR_TO_OUT	0x80

#define RTL_FE_DUAL_IP4_HDR_LEN		20
#define RTL_FE_DUAL_IP6_HDR_LEN		40
#define RTL_FE_DUAL_L2TP_BASE_LEN	16
#define RTL_FE_DUAL_L2TP_INR_TO_OUT	0xf0
#define RTL_FE_DUAL_L2TP_HDR_FLAGS_VER	0x4002U
#define RTL_FE_DUAL_PPTP_BASE_LEN	8
#define RTL_FE_DUAL_PPTP_INR_TO_OUT	0xb0
#define RTL_FE_DUAL_PPTP_FLAGS_VER	0x2001U
#define RTL_FE_DUAL_PPTP_FLAG_SEQ	0x1000U
#define RTL_FE_DUAL_PPTP_FLAG_ACK	0x0080U
#define RTL_FE_DUAL_PPP_MAX_LEN		4
#define RTL_FE_DUAL_IPV4_ID_BASE	0x1000U
#define RTL_FE_DUAL_IP_VERSION_IHL4	0x45
#define RTL_FE_DUAL_IP_DEFAULT_TTL	64
#define RTL_FE_DUAL_PPP_ETH_TYPE	0x880bU

#define RTL_FE_DUAL_FMR_PREFIX_HI_SHIFT	32
#define RTL_FE_DUAL_FMR_PSID_STR_MASK	0xfU
#define RTL_FE_DUAL_FMR_PSID_END_SHIFT	4
#define RTL_FE_DUAL_FMR_PSID_END_MASK	0xfU
#define RTL_FE_DUAL_FMR_PSID_REFILL_SHIFT 8
#define RTL_FE_DUAL_FMR_PSID_REFILL_MASK 0x1U
#define RTL_FE_DUAL_FMR_PREF_LEN_SHIFT	9
#define RTL_FE_DUAL_FMR_PREF_LEN_MASK	0x7fU
#define RTL_FE_DUAL_FMR_DRF_VER_SHIFT	16
#define RTL_FE_DUAL_FMR_DRF_VER_MASK	0x1U

static void rtl_fe_dual_bits_set(u32 *words, unsigned int bit,
				 unsigned int width, u32 value)
{
	unsigned int i;

	for (i = 0; i < width; i++) {
		unsigned int pos = bit + i;
		u32 mask = BIT(pos % RTL_FE_DUAL_WORD_BITS);

		if (value & BIT(i))
			words[pos / RTL_FE_DUAL_WORD_BITS] |= mask;
		else
			words[pos / RTL_FE_DUAL_WORD_BITS] &= ~mask;
	}
}

static u32 rtl_fe_dual_bits_get(const u32 *words, unsigned int bit,
				unsigned int width)
{
	u32 value = 0;
	unsigned int i;

	for (i = 0; i < width; i++) {
		unsigned int pos = bit + i;

		if (words[pos / RTL_FE_DUAL_WORD_BITS] &
		    BIT(pos % RTL_FE_DUAL_WORD_BITS))
			value |= BIT(i);
	}
	return value;
}

int rtl_fe_eif_phys(unsigned int hash)
{
	if (hash >= RTL_FE_EIF_HASH_INDICES)
		return -EINVAL;
	return hash < RTL_FE_EIF_PHYS_ROWS ?
	       hash : RTL_FE_EIF_ALIAS_BASE + (hash & RTL_FE_EIF_ALIAS_MASK);
}

/* Pure software arbitration.  No EIF/hash register is touched here: the OEM
 * never implemented this bookkeeping (RE-7 §5.2), so the open driver owns it
 * and can later decide whether a dual-header flow may share a physical row.
 */
static void rtl_fe_eif_clear_slot(struct rtl_fe_eif_slot *slot)
{
	memset(slot, 0, sizeof(*slot));
}

int rtl_fe_eif_alloc(struct rtl_fe *fe, unsigned int hash, u32 param_hash,
		     unsigned int *phys)
{
	struct rtl_fe_tunnel *t;
	struct rtl_fe_eif_slot *slot;
	unsigned int p, i, refs = 0;
	int ret = 0;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	if (hash >= RTL_FE_EIF_HASH_INDICES || !phys)
		return -EINVAL;
	t = fe->tunnel;
	p = rtl_fe_eif_phys(hash);

	mutex_lock(&t->lock);
	slot = &t->eif[hash];
	if (slot->used) {
		ret = -EBUSY;
		goto unlock;
	}

	if (hash < RTL_FE_EIF_EXCLUSIVE_ROWS) {
		/* Physical rows 0..15 have no alias indices. */
		slot->used = true;
		slot->hash = hash;
		slot->phys = p;
		slot->refs = 1;
		slot->param_hash = param_hash;
		*phys = p;
		goto unlock;
	}

	/* Rows 16..31 are shared by 16+x, 32+x and 48+x. */
	for (i = 0; i < RTL_FE_EIF_HASH_INDICES; i++) {
		if (!t->eif[i].used || t->eif[i].phys != p)
			continue;
		if (t->eif[i].param_hash != param_hash) {
			ret = -EBUSY;
			goto unlock;
		}
		refs++;
	}
	refs++;
	slot->used = true;
	slot->hash = hash;
	slot->phys = p;
	slot->param_hash = param_hash;
	for (i = 0; i < RTL_FE_EIF_HASH_INDICES; i++)
		if (t->eif[i].used && t->eif[i].phys == p)
			t->eif[i].refs = refs;
	*phys = p;
unlock:
	mutex_unlock(&t->lock);
	return ret;
}

int rtl_fe_eif_free(struct rtl_fe *fe, unsigned int hash)
{
	struct rtl_fe_tunnel *t;
	struct rtl_fe_eif_slot *slot;
	unsigned int p, i, refs;
	int ret = 0;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	if (hash >= RTL_FE_EIF_HASH_INDICES)
		return -EINVAL;
	t = fe->tunnel;

	mutex_lock(&t->lock);
	slot = &t->eif[hash];
	if (!slot->used) {
		ret = -ENOENT;
		goto unlock;
	}
	p = slot->phys;
	refs = slot->refs;
	if (refs > 1) {
		refs--;
		rtl_fe_eif_clear_slot(slot);
		for (i = 0; i < RTL_FE_EIF_HASH_INDICES; i++)
			if (t->eif[i].used && t->eif[i].phys == p)
				t->eif[i].refs = refs;
	} else {
		for (i = 0; i < RTL_FE_EIF_HASH_INDICES; i++)
			if (t->eif[i].used && t->eif[i].phys == p)
				rtl_fe_eif_clear_slot(&t->eif[i]);
	}
unlock:
	mutex_unlock(&t->lock);
	return ret;
}

/* The 64-byte Content entry is entered as 128 hex digits, eight per 32-bit
 * word in the same word0..word15 order the OEM print helper uses.
 */
static int rtl_fe_dual_hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + RTL_FE_DUAL_HEX_ALPHA_BASE;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + RTL_FE_DUAL_HEX_ALPHA_BASE;
	return -1;
}

static int rtl_fe_dual_parse_hex(const char *hex, u32 words[RTL_FE_DUAL_WORDS])
{
	unsigned int i, j;

	if (strlen(hex) != RTL_FE_DUAL_BYTES * RTL_FE_DUAL_HEX_PER_BYTE)
		return -EINVAL;
	memset(words, 0, RTL_FE_DUAL_BYTES);
	for (i = 0; i < RTL_FE_DUAL_WORDS; i++) {
		u32 value = 0;

		for (j = 0; j < RTL_FE_DUAL_HEX_PER_WORD; j++) {
			int nibble = rtl_fe_dual_hex_nibble(
				hex[i * RTL_FE_DUAL_HEX_PER_WORD + j]);

			if (nibble < 0)
				return -EINVAL;
			value = (value << RTL_FE_DUAL_NIBBLE_BITS) | nibble;
		}
		words[i] = value;
	}
	return 0;
}

static int rtl_fe_dual_wait(struct rtl_fe *fe)
{
	unsigned int i;

	for (i = 0; i < RTL_FE_DUAL_POLL_COUNT; i++) {
		if (!(rtl_fe_read(fe, RTL_FE_DUAL_CMD) & RTL_FE_DUAL_BUSY))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static u32 rtl_fe_dual_index_mask(u32 select)
{
	return select == RTL_FE_DUAL_SEL_FMR ? RTL_FE_DUAL_FMR_INDEX_MASK :
					       RTL_FE_DUAL_INDEX_MASK;
}

static u32 rtl_fe_dual_swab32(u32 v)
{
	return ((v & 0x000000ffU) << 24) | ((v & 0x0000ff00U) << 8) |
	       ((v & 0x00ff0000U) >> 8) | ((v & 0xff000000U) >> 24);
}

/* ca-ne.ko aal_pe_dual_content_entry_add/get reverse all 16 words and byte-swap
 * each word so DATA(15) at 0x354c holds network-order bytes [0..3] and DATA(0)
 * at 0x3588 holds bytes [60..63].  This involution converts between the logical
 * header image and hardware RAM order.
 */
static void rtl_fe_dual_content_swap(u32 dst[RTL_FE_DUAL_WORDS],
				     const u32 src[RTL_FE_DUAL_WORDS])
{
	unsigned int i;

	for (i = 0; i < RTL_FE_DUAL_WORDS; i++)
		dst[i] = rtl_fe_dual_swab32(src[RTL_FE_DUAL_WORDS - 1 - i]);
}

static int rtl_fe_dual_hw_read(struct rtl_fe *fe, u32 select,
			       unsigned int index, u32 words[RTL_FE_DUAL_WORDS])
{
	unsigned int i;
	int ret;

	rtl_fe_write(fe, RTL_FE_DUAL_CMD,
		     RTL_FE_DUAL_CMD_READ | select |
		     (index & rtl_fe_dual_index_mask(select)));
	ret = rtl_fe_dual_wait(fe);
	if (ret)
		return ret;
	/* __l3pe_dual_hw_entry_get orders the data reads behind a dmb oshld;
	 * rmb() is the Linux load-barrier equivalent.
	 */
	rmb();
	for (i = 0; i < RTL_FE_DUAL_WORDS; i++)
		words[i] = rtl_fe_read(fe, RTL_FE_DUAL_DATA(i));
	return 0;
}

static int rtl_fe_dual_hw_write(struct rtl_fe *fe, u32 select,
				unsigned int index,
				const u32 words[RTL_FE_DUAL_WORDS])
{
	u32 readback[RTL_FE_DUAL_WORDS];
	unsigned int i;
	int ret;

	/* __l3pe_dual_hw_entry_set writes buf[0] at 0x3588 and walks down to
	 * buf[15] at 0x354c; the command word is written last.
	 */
	for (i = 0; i < RTL_FE_DUAL_WORDS; i++)
		rtl_fe_write(fe, RTL_FE_DUAL_DATA(i), words[i]);
	/* The OEM puts a dmb oshst between the data words and the command
	 * word so the table engine cannot latch a half-updated entry.
	 */
	wmb();
	rtl_fe_write(fe, RTL_FE_DUAL_CMD,
		     RTL_FE_DUAL_CMD_WRITE | select |
		     (index & rtl_fe_dual_index_mask(select)));
	ret = rtl_fe_dual_wait(fe);
	if (ret)
		return ret;

	/* The OEM does not read back, but a shared window with three tables is
	 * exactly where a wrong select bit stays silent; verify the entry.
	 * The hardware FMR RAM is 81 bits wide (word0, word1 and word2[16:0]),
	 * so reading back a cleared FMR entry (all 0xffffffff written) yields
	 * 0x0001ffff in word2 and 0 in words 3..15 on real silicon.
	 */
	ret = rtl_fe_dual_hw_read(fe, select, index, readback);
	if (ret)
		return ret;
	if (select == RTL_FE_DUAL_SEL_FMR) {
		if (readback[0] != words[0] || readback[1] != words[1] ||
		    (readback[2] & RTL_FE_DUAL_FMR_W2_VALID_MASK) !=
		    (words[2] & RTL_FE_DUAL_FMR_W2_VALID_MASK))
			return -EIO;
	} else if (memcmp(words, readback, RTL_FE_DUAL_BYTES)) {
		return -EIO;
	}
	return 0;
}

/* __l3pe_dual_hw_entry_clear (ca-ne.ko .text+0x6a270) is the normal write
 * path with a constant payload: zero for Content/Control and 0xffffffff for
 * FMR, followed by the command word and the same 1000 x 1 us busy poll.
 */
static int rtl_fe_dual_hw_clear(struct rtl_fe *fe, u32 select,
				unsigned int index)
{
	u32 words[RTL_FE_DUAL_WORDS];
	u32 fill = select == RTL_FE_DUAL_SEL_FMR ? RTL_FE_DUAL_CLEAR_FILL_FMR :
						   RTL_FE_DUAL_CLEAR_FILL_ZERO;
	unsigned int i;

	for (i = 0; i < RTL_FE_DUAL_WORDS; i++)
		words[i] = fill;
	return rtl_fe_dual_hw_write(fe, select, index, words);
}

/* The Control table packer and unpacker are spelled out field by field;
 * the OEM name of the 6RD fields starts with a digit, so the C
 * spelling is SIXRD/sixrd.
 */
void rtl_fe_dual_control_pack(u32 words[RTL_FE_DUAL_WORDS],
			      const struct rtl_fe_dual_control *control)
{
	memset(words, 0, RTL_FE_DUAL_BYTES);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_IP_LEN_DIFF_PAD_CTRL_BIT,
			     RTL_FE_DUAL_CTRL_REP_IP_LEN_DIFF_PAD_CTRL_WIDTH,
			     control->rep_ip_len_diff_pad_ctrl);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_LEN_DIFF1_PAD_CTRL_BIT,
			     RTL_FE_DUAL_CTRL_REP_LEN_DIFF1_PAD_CTRL_WIDTH,
			     control->rep_len_diff1_pad_ctrl);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_LEN_DIFF2_PAD_CTRL_BIT,
			     RTL_FE_DUAL_CTRL_REP_LEN_DIFF2_PAD_CTRL_WIDTH,
			     control->rep_len_diff2_pad_ctrl);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_HDR_FMT_BIT,
			     RTL_FE_DUAL_CTRL_HDR_FMT_WIDTH,
			     control->hdr_fmt);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_HDR_LEN_BIT,
			     RTL_FE_DUAL_CTRL_HDR_LEN_WIDTH,
			     control->hdr_len);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_MTU_DIFF_BIT,
			     RTL_FE_DUAL_CTRL_MTU_DIFF_WIDTH,
			     control->mtu_diff);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_PPP_IP_VER_OST_BIT,
			     RTL_FE_DUAL_CTRL_PPP_IP_VER_OST_WIDTH,
			     control->ppp_ip_ver_ost);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_IP_EN_BIT,
			     RTL_FE_DUAL_CTRL_REP_IP_EN_WIDTH,
			     control->rep_ip_en);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_IP_VER_BIT,
			     RTL_FE_DUAL_CTRL_REP_IP_VER_WIDTH,
			     control->rep_ip_ver);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_LEN_OST1_BIT,
			     RTL_FE_DUAL_CTRL_REP_LEN_OST1_WIDTH,
			     control->rep_len_ost1);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_LEN_DIFF1_BIT,
			     RTL_FE_DUAL_CTRL_REP_LEN_DIFF1_WIDTH,
			     control->rep_len_diff1);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_LEN_OST2_BIT,
			     RTL_FE_DUAL_CTRL_REP_LEN_OST2_WIDTH,
			     control->rep_len_ost2);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_LEN_DIFF2_BIT,
			     RTL_FE_DUAL_CTRL_REP_LEN_DIFF2_WIDTH,
			     control->rep_len_diff2);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_IP4_OST_BIT,
			     RTL_FE_DUAL_CTRL_IP4_OST_WIDTH,
			     control->ip4_ost);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_GRE_SEQ_ACK_OST_BIT,
			     RTL_FE_DUAL_CTRL_GRE_SEQ_ACK_OST_WIDTH,
			     control->gre_seq_ack_ost);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_GRE_MOD_FLD_BIT,
			     RTL_FE_DUAL_CTRL_GRE_MOD_FLD_WIDTH,
			     control->gre_mod_fld);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_IP6_OFT_BIT,
			     RTL_FE_DUAL_CTRL_IP6_OFT_WIDTH,
			     control->ip6_oft);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_IP_DSCP_EN_BIT,
			     RTL_FE_DUAL_CTRL_REP_IP_DSCP_EN_WIDTH,
			     control->rep_ip_dscp_en);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_IP_ECN_EN_BIT,
			     RTL_FE_DUAL_CTRL_REP_IP_ECN_EN_WIDTH,
			     control->rep_ip_ecn_en);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_IP_LEN_BIT,
			     RTL_FE_DUAL_CTRL_REP_IP_LEN_WIDTH,
			     control->rep_ip_len);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_IP_LEN_DIFF_BIT,
			     RTL_FE_DUAL_CTRL_REP_IP_LEN_DIFF_WIDTH,
			     control->rep_ip_len_diff);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_IP_PTC_BIT,
			     RTL_FE_DUAL_CTRL_REP_IP_PTC_WIDTH,
			     control->rep_ip_ptc);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_REP_IP_TTL_BIT,
			     RTL_FE_DUAL_CTRL_REP_IP_TTL_WIDTH,
			     control->rep_ip_ttl);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_MAPT_PRE_LEN_BIT,
			     RTL_FE_DUAL_CTRL_MAPT_PRE_LEN_WIDTH,
			     control->mapt_pre_len);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_MAP_DRF_VER_BIT,
			     RTL_FE_DUAL_CTRL_MAP_DRF_VER_WIDTH,
			     control->map_drf_ver);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_SIXRD_DIP_PRE_LEN_BIT,
			     RTL_FE_DUAL_CTRL_SIXRD_DIP_PRE_LEN_WIDTH,
			     control->sixrd_dip_pre_len);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_SIXRD_DIP_SUF_STR_BIT_BIT,
			     RTL_FE_DUAL_CTRL_SIXRD_DIP_SUF_STR_BIT_WIDTH,
			     control->sixrd_dip_suf_str_bit);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_DSL_UC_CHK_BIT,
			     RTL_FE_DUAL_CTRL_DSL_UC_CHK_WIDTH,
			     control->dsl_uc_chk);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_DSL_MC_CHK_BIT,
			     RTL_FE_DUAL_CTRL_DSL_MC_CHK_WIDTH,
			     control->dsl_mc_chk);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_L4SUM_REC_BIT,
			     RTL_FE_DUAL_CTRL_L4SUM_REC_WIDTH,
			     control->l4sum_rec);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_VXLAN_SP_OST_BIT,
			     RTL_FE_DUAL_CTRL_VXLAN_SP_OST_WIDTH,
			     control->vxlan_sp_ost);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_INR_TO_OUT_FLD_BIT,
			     RTL_FE_DUAL_CTRL_INR_TO_OUT_FLD_WIDTH,
			     control->inr_to_out_fld);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_TOS_HSH_SEL_BIT,
			     RTL_FE_DUAL_CTRL_TOS_HSH_SEL_WIDTH,
			     control->tos_hsh_sel);
	rtl_fe_dual_bits_set(words, RTL_FE_DUAL_CTRL_MC_MAC_MAP_BIT,
			     RTL_FE_DUAL_CTRL_MC_MAC_MAP_WIDTH,
			     control->mc_mac_map);
}

void rtl_fe_dual_control_unpack(const u32 words[RTL_FE_DUAL_WORDS],
				struct rtl_fe_dual_control *control)
{
	control->rep_ip_len_diff_pad_ctrl =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_IP_LEN_DIFF_PAD_CTRL_BIT,
				     RTL_FE_DUAL_CTRL_REP_IP_LEN_DIFF_PAD_CTRL_WIDTH);
	control->rep_len_diff1_pad_ctrl =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_LEN_DIFF1_PAD_CTRL_BIT,
				     RTL_FE_DUAL_CTRL_REP_LEN_DIFF1_PAD_CTRL_WIDTH);
	control->rep_len_diff2_pad_ctrl =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_LEN_DIFF2_PAD_CTRL_BIT,
				     RTL_FE_DUAL_CTRL_REP_LEN_DIFF2_PAD_CTRL_WIDTH);
	control->hdr_fmt =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_HDR_FMT_BIT,
				     RTL_FE_DUAL_CTRL_HDR_FMT_WIDTH);
	control->hdr_len =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_HDR_LEN_BIT,
				     RTL_FE_DUAL_CTRL_HDR_LEN_WIDTH);
	control->mtu_diff =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_MTU_DIFF_BIT,
				     RTL_FE_DUAL_CTRL_MTU_DIFF_WIDTH);
	control->ppp_ip_ver_ost =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_PPP_IP_VER_OST_BIT,
				     RTL_FE_DUAL_CTRL_PPP_IP_VER_OST_WIDTH);
	control->rep_ip_en =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_IP_EN_BIT,
				     RTL_FE_DUAL_CTRL_REP_IP_EN_WIDTH);
	control->rep_ip_ver =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_IP_VER_BIT,
				     RTL_FE_DUAL_CTRL_REP_IP_VER_WIDTH);
	control->rep_len_ost1 =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_LEN_OST1_BIT,
				     RTL_FE_DUAL_CTRL_REP_LEN_OST1_WIDTH);
	control->rep_len_diff1 =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_LEN_DIFF1_BIT,
				     RTL_FE_DUAL_CTRL_REP_LEN_DIFF1_WIDTH);
	control->rep_len_ost2 =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_LEN_OST2_BIT,
				     RTL_FE_DUAL_CTRL_REP_LEN_OST2_WIDTH);
	control->rep_len_diff2 =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_LEN_DIFF2_BIT,
				     RTL_FE_DUAL_CTRL_REP_LEN_DIFF2_WIDTH);
	control->ip4_ost =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_IP4_OST_BIT,
				     RTL_FE_DUAL_CTRL_IP4_OST_WIDTH);
	control->gre_seq_ack_ost =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_GRE_SEQ_ACK_OST_BIT,
				     RTL_FE_DUAL_CTRL_GRE_SEQ_ACK_OST_WIDTH);
	control->gre_mod_fld =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_GRE_MOD_FLD_BIT,
				     RTL_FE_DUAL_CTRL_GRE_MOD_FLD_WIDTH);
	control->ip6_oft =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_IP6_OFT_BIT,
				     RTL_FE_DUAL_CTRL_IP6_OFT_WIDTH);
	control->rep_ip_dscp_en =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_IP_DSCP_EN_BIT,
				     RTL_FE_DUAL_CTRL_REP_IP_DSCP_EN_WIDTH);
	control->rep_ip_ecn_en =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_IP_ECN_EN_BIT,
				     RTL_FE_DUAL_CTRL_REP_IP_ECN_EN_WIDTH);
	control->rep_ip_len =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_IP_LEN_BIT,
				     RTL_FE_DUAL_CTRL_REP_IP_LEN_WIDTH);
	control->rep_ip_len_diff =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_IP_LEN_DIFF_BIT,
				     RTL_FE_DUAL_CTRL_REP_IP_LEN_DIFF_WIDTH);
	control->rep_ip_ptc =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_IP_PTC_BIT,
				     RTL_FE_DUAL_CTRL_REP_IP_PTC_WIDTH);
	control->rep_ip_ttl =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_REP_IP_TTL_BIT,
				     RTL_FE_DUAL_CTRL_REP_IP_TTL_WIDTH);
	control->mapt_pre_len =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_MAPT_PRE_LEN_BIT,
				     RTL_FE_DUAL_CTRL_MAPT_PRE_LEN_WIDTH);
	control->map_drf_ver =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_MAP_DRF_VER_BIT,
				     RTL_FE_DUAL_CTRL_MAP_DRF_VER_WIDTH);
	control->sixrd_dip_pre_len =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_SIXRD_DIP_PRE_LEN_BIT,
				     RTL_FE_DUAL_CTRL_SIXRD_DIP_PRE_LEN_WIDTH);
	control->sixrd_dip_suf_str_bit =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_SIXRD_DIP_SUF_STR_BIT_BIT,
				     RTL_FE_DUAL_CTRL_SIXRD_DIP_SUF_STR_BIT_WIDTH);
	control->dsl_uc_chk =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_DSL_UC_CHK_BIT,
				     RTL_FE_DUAL_CTRL_DSL_UC_CHK_WIDTH);
	control->dsl_mc_chk =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_DSL_MC_CHK_BIT,
				     RTL_FE_DUAL_CTRL_DSL_MC_CHK_WIDTH);
	control->l4sum_rec =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_L4SUM_REC_BIT,
				     RTL_FE_DUAL_CTRL_L4SUM_REC_WIDTH);
	control->vxlan_sp_ost =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_VXLAN_SP_OST_BIT,
				     RTL_FE_DUAL_CTRL_VXLAN_SP_OST_WIDTH);
	control->inr_to_out_fld =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_INR_TO_OUT_FLD_BIT,
				     RTL_FE_DUAL_CTRL_INR_TO_OUT_FLD_WIDTH);
	control->tos_hsh_sel =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_TOS_HSH_SEL_BIT,
				     RTL_FE_DUAL_CTRL_TOS_HSH_SEL_WIDTH);
	control->mc_mac_map =
		rtl_fe_dual_bits_get(words,
				     RTL_FE_DUAL_CTRL_MC_MAC_MAP_BIT,
				     RTL_FE_DUAL_CTRL_MC_MAC_MAP_WIDTH);
}

/* Defaults shared by all measured profiles: every 7-bit offset field that
 * the OEM leaves unused carries 0x7f.  hdr_fmt/hdr_len and the live fields
 * are filled in per profile below.
 */
static void rtl_fe_dual_control_defaults(struct rtl_fe_dual_control *control)
{
	memset(control, 0, sizeof(*control));
	control->ppp_ip_ver_ost = RTL_FE_DUAL_OST_NONE;
	control->rep_len_ost1 = RTL_FE_DUAL_OST_NONE;
	control->rep_len_ost2 = RTL_FE_DUAL_OST_NONE;
	control->ip4_ost = RTL_FE_DUAL_OST_NONE;
	control->gre_seq_ack_ost = RTL_FE_DUAL_OST_NONE;
	control->vxlan_sp_ost = RTL_FE_DUAL_OST_NONE;
}

/* _rtk_fc_dualHeader_ctrl_setupDsliteMape @ fc_8277c.ko (measured literals). */
static void rtl_fe_dual_control_preset_dslite(struct rtl_fe_dual_control *c)
{
	rtl_fe_dual_control_defaults(c);
	c->hdr_fmt = 0;
	c->hdr_len = RTL_FE_DUAL_DSLITE_HDR_LEN;
	c->mtu_diff = RTL_FE_DUAL_DSLITE_HDR_LEN;
	c->rep_ip_en = 1;
	c->rep_ip_ver = 1;
	c->ip6_oft = 0;
	c->rep_ip_len = 1;
	c->dsl_mc_chk = 1;
	c->inr_to_out_fld = RTL_FE_DUAL_DSLITE_INR_TO_OUT;
	c->mc_mac_map = 1;
}

/* _rtk_fc_dualHeader_ctrl_setup6RD @ fc_8277c.ko (measured literals). */
static void rtl_fe_dual_control_preset_6rd(struct rtl_fe_dual_control *c)
{
	rtl_fe_dual_control_defaults(c);
	c->hdr_fmt = 0;
	c->hdr_len = RTL_FE_DUAL_6RD_HDR_LEN;
	c->mtu_diff = RTL_FE_DUAL_6RD_HDR_LEN;
	c->ppp_ip_ver_ost = RTL_FE_DUAL_OST_NONE;
	c->rep_ip_en = 1;
	c->rep_ip_ver = 0;
	c->rep_len_ost1 = RTL_FE_DUAL_6RD_REP_LEN_OST1;
	c->rep_len_diff1 = RTL_FE_DUAL_6RD_REP_LEN_DIFF1;
	c->rep_len_diff2 = RTL_FE_DUAL_6RD_REP_LEN_DIFF2;
	c->ip4_ost = 0;
	c->ip6_oft = RTL_FE_DUAL_OST_NONE;
	c->rep_ip_dscp_en = 1;
	c->rep_ip_ecn_en = 1;
	c->rep_ip_len = 1;
	c->rep_ip_len_diff = RTL_FE_DUAL_6RD_REP_IP_LEN_DIFF;
	c->sixrd_dip_suf_str_bit = RTL_FE_DUAL_6RD_SUF_STR_BIT;
	c->inr_to_out_fld = RTL_FE_DUAL_6RD_INR_TO_OUT;
}

/* _rtk_fc_dualHeader_ctrl_setupMAPT @ fc_mgr.ko .text+0x995c0 (spec §3.2).
 *
 * hdr_len, rep_ip_en/ver, rep_len_ost1/2, rep_len_diff1/2 and ip4_ost are
 * caller parameters in the OEM function, not literals; this preset leaves
 * the offset sentinels alone and hdr_len at zero.  W8b has to derive those
 * values from the flow's tunnel parameters before installing a MAP-T rule.
 */
static void rtl_fe_dual_control_preset_mapt(struct rtl_fe_dual_control *c)
{
	rtl_fe_dual_control_defaults(c);
	c->hdr_fmt = 0;
	c->gre_seq_ack_ost = RTL_FE_DUAL_MAPT_GRE_SEQ_ACK_OST;
	c->ip6_oft = RTL_FE_DUAL_MAPT_IP6_OFT;
	c->rep_ip_dscp_en = 1;
	c->rep_ip_ecn_en = 1;
	c->rep_ip_len_diff = 1;
	c->rep_ip_ptc = 1;
	c->rep_ip_ttl = 1;
	c->mapt_pre_len = RTL_FE_DUAL_MAPT_PRE_LEN;
	c->map_drf_ver = 1;
	c->sixrd_dip_pre_len = RTL_FE_DUAL_MAPT_6RD_PRE_LEN;
	c->sixrd_dip_suf_str_bit = RTL_FE_DUAL_MAPT_6RD_SUF_STR_BIT;
	/* RE-7 §3.4: the parameter branch decodes inr_to_out_fld=0x80. */
	c->inr_to_out_fld = RTL_FE_DUAL_MAPT_INR_TO_OUT;
}

static void
rtl_fe_dual_control_preset_ppp_common(struct rtl_fe_dual_control *c,
				      bool is_ipv6, unsigned int l4_ppp_len)
{
	unsigned int ip_len = is_ipv6 ? RTL_FE_DUAL_IP6_HDR_LEN :
					RTL_FE_DUAL_IP4_HDR_LEN;
	unsigned int hdr_len = ip_len + l4_ppp_len;

	rtl_fe_dual_control_defaults(c);
	c->hdr_len = hdr_len;
	c->ppp_ip_ver_ost = hdr_len - 1;
	c->rep_ip_en = 1;
	c->rep_ip_ver = is_ipv6 ? 1 : 0;
	c->rep_len_ost1 = ip_len + 4;
	c->ip4_ost = is_ipv6 ? RTL_FE_DUAL_OST_NONE : 0;
	c->ip6_oft = is_ipv6 ? 0 : RTL_FE_DUAL_OST_NONE;
	c->rep_ip_len = 1;
	c->rep_ip_len_diff = is_ipv6 ? l4_ppp_len : hdr_len;
	c->mc_mac_map = 1;
}

static void rtl_fe_dual_control_preset_l2tp(struct rtl_fe_dual_control *c,
					    bool is_ipv6, u8 ppp_len)
{
	unsigned int ip_len = is_ipv6 ? RTL_FE_DUAL_IP6_HDR_LEN :
					RTL_FE_DUAL_IP4_HDR_LEN;

	rtl_fe_dual_control_preset_ppp_common(c, is_ipv6,
					      RTL_FE_DUAL_L2TP_BASE_LEN +
					      ppp_len);
	c->rep_len_diff1 = RTL_FE_DUAL_L2TP_BASE_LEN + ppp_len;
	c->rep_len_ost2 = ip_len + 10;
	c->rep_len_diff2 = 8 + ppp_len;
	c->inr_to_out_fld = RTL_FE_DUAL_L2TP_INR_TO_OUT;
}

static void rtl_fe_dual_control_preset_pptp(struct rtl_fe_dual_control *c,
					    bool is_ipv6, u8 ppp_len, u8 flags)
{
	unsigned int ip_len = is_ipv6 ? RTL_FE_DUAL_IP6_HDR_LEN :
					RTL_FE_DUAL_IP4_HDR_LEN;
	unsigned int gre_len = RTL_FE_DUAL_PPTP_BASE_LEN +
			       ((flags & RTL_FE_PPP_TUN_F_SEQ) ? 4 : 0) +
			       ((flags & RTL_FE_PPP_TUN_F_ACK) ? 4 : 0);

	rtl_fe_dual_control_preset_ppp_common(c, is_ipv6, gre_len + ppp_len);
	c->rep_len_diff1 = ppp_len;
	c->gre_seq_ack_ost = ip_len + 8;
	c->gre_mod_fld = ((flags & RTL_FE_PPP_TUN_F_SEQ) ? 1 : 0) |
			 ((flags & RTL_FE_PPP_TUN_F_ACK) ? 2 : 0);
	c->inr_to_out_fld = RTL_FE_DUAL_PPTP_INR_TO_OUT;
}

static int rtl_fe_dual_control_preset(struct rtl_fe_dual_control *control,
				      unsigned int type)
{
	switch (type) {
	case RTL_FE_DUAL_TYPE_IP4INIP6:
		rtl_fe_dual_control_preset_dslite(control);
		return 0;
	case RTL_FE_DUAL_TYPE_6RD:
		rtl_fe_dual_control_preset_6rd(control);
		return 0;
	case RTL_FE_DUAL_TYPE_MAPT:
		rtl_fe_dual_control_preset_mapt(control);
		return 0;
	case RTL_FE_DUAL_TYPE_L2TP:
		rtl_fe_dual_control_preset_l2tp(control, false,
						RTL_FE_DUAL_PPP_MAX_LEN);
		return 0;
	case RTL_FE_DUAL_TYPE_PPTP:
		rtl_fe_dual_control_preset_pptp(control, false,
						RTL_FE_DUAL_PPP_MAX_LEN,
						RTL_FE_PPP_TUN_F_SEQ);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

void rtl_fe_dual_fmr_pack(u32 words[RTL_FE_DUAL_WORDS],
			  const struct rtl_fe_dual_fmr *fmr)
{
	memset(words, 0, RTL_FE_DUAL_BYTES);
	words[0] = (u32)fmr->prefix;
	words[1] = (u32)(fmr->prefix >> RTL_FE_DUAL_FMR_PREFIX_HI_SHIFT);
	words[2] = (fmr->psid_str_ost & RTL_FE_DUAL_FMR_PSID_STR_MASK) |
		   ((fmr->psid_end_ost & RTL_FE_DUAL_FMR_PSID_END_MASK) <<
		    RTL_FE_DUAL_FMR_PSID_END_SHIFT) |
		   ((fmr->psid_refill & RTL_FE_DUAL_FMR_PSID_REFILL_MASK) <<
		    RTL_FE_DUAL_FMR_PSID_REFILL_SHIFT) |
		   ((fmr->pref_len & RTL_FE_DUAL_FMR_PREF_LEN_MASK) <<
		    RTL_FE_DUAL_FMR_PREF_LEN_SHIFT) |
		   ((fmr->drf_ver & RTL_FE_DUAL_FMR_DRF_VER_MASK) <<
		    RTL_FE_DUAL_FMR_DRF_VER_SHIFT);
}

void rtl_fe_dual_fmr_unpack(const u32 words[RTL_FE_DUAL_WORDS],
			    struct rtl_fe_dual_fmr *fmr)
{
	fmr->prefix = (u64)words[0] |
		      ((u64)words[1] << RTL_FE_DUAL_FMR_PREFIX_HI_SHIFT);
	fmr->psid_str_ost = words[2] & RTL_FE_DUAL_FMR_PSID_STR_MASK;
	fmr->psid_end_ost = (words[2] >> RTL_FE_DUAL_FMR_PSID_END_SHIFT) &
			    RTL_FE_DUAL_FMR_PSID_END_MASK;
	fmr->psid_refill = (words[2] >> RTL_FE_DUAL_FMR_PSID_REFILL_SHIFT) &
			   RTL_FE_DUAL_FMR_PSID_REFILL_MASK;
	fmr->pref_len = (words[2] >> RTL_FE_DUAL_FMR_PREF_LEN_SHIFT) &
			RTL_FE_DUAL_FMR_PREF_LEN_MASK;
	fmr->drf_ver = (words[2] >> RTL_FE_DUAL_FMR_DRF_VER_SHIFT) &
		       RTL_FE_DUAL_FMR_DRF_VER_MASK;
}

static int rtl_fe_dual_table_alloc(struct rtl_fe *fe, unsigned long *bitmap,
				   struct rtl_fe_dual_slot *slots,
				   unsigned int entries, unsigned int owner,
				   unsigned int *index)
{
	struct rtl_fe_tunnel *t = fe->tunnel;
	unsigned int slot;

	if (!index)
		return -EINVAL;
	if (owner != RTL_FE_DUAL_OWNER_DEBUGFS &&
	    owner != RTL_FE_DUAL_OWNER_FLOW)
		return -EINVAL;

	mutex_lock(&t->lock);
	slot = find_first_zero_bit(bitmap, entries);
	if (slot >= entries) {
		mutex_unlock(&t->lock);
		return -ENOSPC;
	}
	__set_bit(slot, bitmap);
	slots[slot].owner = owner;
	slots[slot].type = RTL_FE_DUAL_TYPE_NONE;
	*index = slot;
	mutex_unlock(&t->lock);
	return 0;
}

static int rtl_fe_dual_table_free(struct rtl_fe *fe, unsigned long *bitmap,
				  struct rtl_fe_dual_slot *slots,
				  unsigned int entries, unsigned int index,
				  u32 select, bool clear)
{
	struct rtl_fe_tunnel *t = fe->tunnel;
	int ret;

	if (index >= entries)
		return -EINVAL;

	mutex_lock(&t->lock);
	if (!test_bit(index, bitmap)) {
		ret = -ENOENT;
		goto unlock;
	}
	if (clear) {
		/* Clear the hardware entry before releasing the slot so a
		 * failed clear leaves the slot allocated and can be retried.
		 */
		ret = rtl_fe_dual_hw_clear(fe, select, index);
		if (ret)
			goto unlock;
	}
	__clear_bit(index, bitmap);
	slots[index].owner = RTL_FE_DUAL_OWNER_NONE;
	slots[index].type = RTL_FE_DUAL_TYPE_NONE;
	ret = 0;
unlock:
	mutex_unlock(&t->lock);
	return ret;
}

static int rtl_fe_dual_table_write(struct rtl_fe *fe, unsigned long *bitmap,
				   unsigned int entries, unsigned int index,
				   u32 select,
				   const u32 words[RTL_FE_DUAL_WORDS])
{
	struct rtl_fe_tunnel *t = fe->tunnel;
	int ret;

	if (index >= entries || !words)
		return -EINVAL;

	mutex_lock(&t->lock);
	if (!test_bit(index, bitmap))
		ret = -ENOENT;
	else
		ret = rtl_fe_dual_hw_write(fe, select, index, words);
	mutex_unlock(&t->lock);
	return ret;
}

static int rtl_fe_dual_table_read(struct rtl_fe *fe, unsigned long *bitmap,
				  unsigned int entries, unsigned int index,
				  u32 select, u32 words[RTL_FE_DUAL_WORDS])
{
	struct rtl_fe_tunnel *t = fe->tunnel;
	int ret;

	if (index >= entries || !words)
		return -EINVAL;

	mutex_lock(&t->lock);
	if (!test_bit(index, bitmap))
		ret = -ENOENT;
	else
		ret = rtl_fe_dual_hw_read(fe, select, index, words);
	mutex_unlock(&t->lock);
	return ret;
}

int rtl_fe_dual_content_alloc(struct rtl_fe *fe, unsigned int *index,
			      unsigned int owner)
{
	if (!fe || !fe->tunnel)
		return -ENODEV;
	return rtl_fe_dual_table_alloc(fe, fe->tunnel->content,
				       fe->tunnel->content_slot,
				       RTL_FE_DUAL_CONTENT_ENTRIES, owner, index);
}

int rtl_fe_dual_content_free(struct rtl_fe *fe, unsigned int index)
{
	if (!fe || !fe->tunnel)
		return -ENODEV;
	return rtl_fe_dual_table_free(fe, fe->tunnel->content,
				      fe->tunnel->content_slot,
				      RTL_FE_DUAL_CONTENT_ENTRIES, index,
				      RTL_FE_DUAL_SEL_CONTENT,
				      rtl_fe_dual_clear_on_free);
}

int rtl_fe_dual_content_write(struct rtl_fe *fe, unsigned int index,
			      const u32 words[RTL_FE_DUAL_WORDS])
{
	u32 hw[RTL_FE_DUAL_WORDS];

	if (!fe || !fe->tunnel)
		return -ENODEV;
	if (!words)
		return -EINVAL;
	rtl_fe_dual_content_swap(hw, words);
	return rtl_fe_dual_table_write(fe, fe->tunnel->content,
				       RTL_FE_DUAL_CONTENT_ENTRIES, index,
				       RTL_FE_DUAL_SEL_CONTENT, hw);
}

int rtl_fe_dual_content_read(struct rtl_fe *fe, unsigned int index,
			     u32 words[RTL_FE_DUAL_WORDS])
{
	u32 hw[RTL_FE_DUAL_WORDS];
	int ret;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	if (!words)
		return -EINVAL;
	ret = rtl_fe_dual_table_read(fe, fe->tunnel->content,
				     RTL_FE_DUAL_CONTENT_ENTRIES, index,
				     RTL_FE_DUAL_SEL_CONTENT, hw);
	if (!ret)
		rtl_fe_dual_content_swap(words, hw);
	return ret;
}

int rtl_fe_dual_control_alloc(struct rtl_fe *fe, unsigned int *index,
			      unsigned int owner)
{
	if (!fe || !fe->tunnel)
		return -ENODEV;
	return rtl_fe_dual_table_alloc(fe, fe->tunnel->control,
				       fe->tunnel->control_slot,
				       RTL_FE_DUAL_CONTROL_ENTRIES, owner, index);
}

int rtl_fe_dual_control_free(struct rtl_fe *fe, unsigned int index)
{
	if (!fe || !fe->tunnel)
		return -ENODEV;
	return rtl_fe_dual_table_free(fe, fe->tunnel->control,
				      fe->tunnel->control_slot,
				      RTL_FE_DUAL_CONTROL_ENTRIES, index,
				      RTL_FE_DUAL_SEL_CONTROL,
				      rtl_fe_dual_clear_on_free);
}

int rtl_fe_dual_control_write(struct rtl_fe *fe, unsigned int index,
			      const u32 words[RTL_FE_DUAL_WORDS])
{
	if (!fe || !fe->tunnel)
		return -ENODEV;
	return rtl_fe_dual_table_write(fe, fe->tunnel->control,
				       RTL_FE_DUAL_CONTROL_ENTRIES, index,
				       RTL_FE_DUAL_SEL_CONTROL, words);
}

int rtl_fe_dual_control_read(struct rtl_fe *fe, unsigned int index,
			     u32 words[RTL_FE_DUAL_WORDS])
{
	if (!fe || !fe->tunnel)
		return -ENODEV;
	return rtl_fe_dual_table_read(fe, fe->tunnel->control,
				      RTL_FE_DUAL_CONTROL_ENTRIES, index,
				      RTL_FE_DUAL_SEL_CONTROL, words);
}

int rtl_fe_dual_control_set_type(struct rtl_fe *fe, unsigned int index,
				 unsigned int type)
{
	struct rtl_fe_dual_control control;
	u32 words[RTL_FE_DUAL_WORDS];
	struct rtl_fe_tunnel *t;
	int ret;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	t = fe->tunnel;
	if (index >= RTL_FE_DUAL_CONTROL_ENTRIES)
		return -EINVAL;

	ret = rtl_fe_dual_control_preset(&control, type);
	if (ret)
		return ret;
	rtl_fe_dual_control_pack(words, &control);

	mutex_lock(&t->lock);
	if (!test_bit(index, t->control)) {
		ret = -ENOENT;
	} else {
		ret = rtl_fe_dual_hw_write(fe, RTL_FE_DUAL_SEL_CONTROL, index,
					   words);
		if (!ret)
			t->control_slot[index].type = type;
	}
	mutex_unlock(&t->lock);
	return ret;
}

int rtl_fe_dual_fmr_alloc(struct rtl_fe *fe, unsigned int *index,
			  unsigned int owner)
{
	if (!fe || !fe->tunnel)
		return -ENODEV;
	return rtl_fe_dual_table_alloc(fe, fe->tunnel->fmr,
				       fe->tunnel->fmr_slot,
				       RTL_FE_DUAL_FMR_ENTRIES, owner, index);
}

int rtl_fe_dual_fmr_free(struct rtl_fe *fe, unsigned int index)
{
	if (!fe || !fe->tunnel)
		return -ENODEV;
	return rtl_fe_dual_table_free(fe, fe->tunnel->fmr,
				      fe->tunnel->fmr_slot,
				      RTL_FE_DUAL_FMR_ENTRIES, index,
				      RTL_FE_DUAL_SEL_FMR,
				      rtl_fe_dual_clear_on_free);
}

int rtl_fe_dual_fmr_write(struct rtl_fe *fe, unsigned int index,
			  const u32 words[RTL_FE_DUAL_WORDS])
{
	struct rtl_fe_tunnel *t;
	int ret;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	t = fe->tunnel;
	if (index >= RTL_FE_DUAL_FMR_ENTRIES || !words)
		return -EINVAL;

	mutex_lock(&t->lock);
	if (!test_bit(index, t->fmr)) {
		ret = -ENOENT;
	} else {
		ret = rtl_fe_dual_hw_write(fe, RTL_FE_DUAL_SEL_FMR, index,
					   words);
		/* FMR exists only for MAP-T (spec §1.5), so a programmed rule
		 * always has that type.
		 */
		if (!ret)
			t->fmr_slot[index].type = RTL_FE_DUAL_TYPE_MAPT;
	}
	mutex_unlock(&t->lock);
	return ret;
}

int rtl_fe_dual_fmr_read(struct rtl_fe *fe, unsigned int index,
			 u32 words[RTL_FE_DUAL_WORDS])
{
	if (!fe || !fe->tunnel)
		return -ENODEV;
	return rtl_fe_dual_table_read(fe, fe->tunnel->fmr,
				      RTL_FE_DUAL_FMR_ENTRIES, index,
				      RTL_FE_DUAL_SEL_FMR, words);
}

/* Allocate and program one Control+Content pair for a flow rule.  Both slots
 * are owned by the flow and must be released in Control->Content order; the
 * caller stores the indices in struct rtl_fe_flow.
 */
static int rtl_fe_dual_program(struct rtl_fe *fe, unsigned int type,
			       const u32 control_words[RTL_FE_DUAL_WORDS],
			       const u32 content_words[RTL_FE_DUAL_WORDS],
			       unsigned int *control, unsigned int *content)
{
	struct rtl_fe_tunnel *t;
	unsigned int control_index, content_index;
	int ret;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	if (!control_words || !content_words || !control || !content)
		return -EINVAL;
	t = fe->tunnel;

	ret = rtl_fe_dual_control_alloc(fe, &control_index,
					RTL_FE_DUAL_OWNER_FLOW);
	if (ret)
		return ret;
	ret = rtl_fe_dual_content_alloc(fe, &content_index,
					RTL_FE_DUAL_OWNER_FLOW);
	if (ret)
		goto drop_control;

	ret = rtl_fe_dual_control_write(fe, control_index, control_words);
	if (ret)
		goto drop_both;
	ret = rtl_fe_dual_content_write(fe, content_index, content_words);
	if (ret)
		goto drop_both;

	mutex_lock(&t->lock);
	t->control_slot[control_index].type = type;
	t->content_slot[content_index].type = type;
	mutex_unlock(&t->lock);
	*control = control_index;
	*content = content_index;
	return 0;

drop_both:
	rtl_fe_dual_table_free(fe, t->content, t->content_slot,
			       RTL_FE_DUAL_CONTENT_ENTRIES, content_index,
			       RTL_FE_DUAL_SEL_CONTENT, false);
drop_control:
	rtl_fe_dual_table_free(fe, t->control, t->control_slot,
			       RTL_FE_DUAL_CONTROL_ENTRIES, control_index,
			       RTL_FE_DUAL_SEL_CONTROL, false);
	return ret;
}

static void rtl_fe_tunnel_put_be16(u8 *dst, u16 val)
{
	dst[0] = val >> 8;
	dst[1] = val & 0xff;
}

static void rtl_fe_tunnel_put_ip4(u8 *hdr, u8 proto, const u8 src[4],
				  const u8 dst[4])
{
	hdr[0] = RTL_FE_DUAL_IP_VERSION_IHL4;
	hdr[8] = RTL_FE_DUAL_IP_DEFAULT_TTL;
	hdr[9] = proto;
	memcpy(hdr + 12, src, RTL_FE_DUAL_6RD_ADDR_BYTES);
	memcpy(hdr + 16, dst, RTL_FE_DUAL_6RD_ADDR_BYTES);
}

static void rtl_fe_tunnel_put_ppp(u8 *dst, u8 ppp_len)
{
	if (ppp_len >= 3) {
		dst[0] = 0xff;
		dst[1] = 0x03;
	}
	dst[ppp_len - 1] = 0x21;
}

static int rtl_fe_tunnel_build_l2tp(const struct rtl_fe_tunnel_key *key,
				    u32 control_words[RTL_FE_DUAL_WORDS],
				    u32 content_words[RTL_FE_DUAL_WORDS])
{
	struct rtl_fe_dual_control profile;
	u8 hdr[RTL_FE_DUAL_BYTES] = { };

	if (key->af != AF_INET)
		return -EOPNOTSUPP;
	if (!key->ppp_len || key->ppp_len > RTL_FE_DUAL_PPP_MAX_LEN)
		return -EINVAL;

	rtl_fe_dual_control_preset_l2tp(&profile, false, key->ppp_len);
	rtl_fe_dual_control_pack(control_words, &profile);
	if (key->decap)
		return 0;

	rtl_fe_tunnel_put_ip4(hdr, IPPROTO_UDP, key->src, key->dst);
	rtl_fe_tunnel_put_be16(hdr + RTL_FE_DUAL_IP4_HDR_LEN, key->sport);
	rtl_fe_tunnel_put_be16(hdr + RTL_FE_DUAL_IP4_HDR_LEN + 2, key->dport);
	rtl_fe_tunnel_put_be16(hdr + RTL_FE_DUAL_IP4_HDR_LEN + 8,
			       RTL_FE_DUAL_L2TP_HDR_FLAGS_VER);
	rtl_fe_tunnel_put_be16(hdr + RTL_FE_DUAL_IP4_HDR_LEN + 12,
			       key->peer_tid);
	rtl_fe_tunnel_put_be16(hdr + RTL_FE_DUAL_IP4_HDR_LEN + 14,
			       key->peer_sid);
	rtl_fe_tunnel_put_ppp(hdr + RTL_FE_DUAL_IP4_HDR_LEN +
			      RTL_FE_DUAL_L2TP_BASE_LEN, key->ppp_len);
	memcpy(content_words, hdr, sizeof(hdr));
	return 0;
}

static int rtl_fe_tunnel_build_pptp(const struct rtl_fe_tunnel_key *key,
				    u32 control_words[RTL_FE_DUAL_WORDS],
				    u32 content_words[RTL_FE_DUAL_WORDS])
{
	unsigned int off = RTL_FE_DUAL_IP4_HDR_LEN + RTL_FE_DUAL_PPTP_BASE_LEN;
	u16 flags_ver = RTL_FE_DUAL_PPTP_FLAGS_VER;
	struct rtl_fe_dual_control profile;
	u8 hdr[RTL_FE_DUAL_BYTES] = { };

	if (key->af != AF_INET)
		return -EOPNOTSUPP;
	if (!key->ppp_len || key->ppp_len > RTL_FE_DUAL_PPP_MAX_LEN)
		return -EINVAL;

	rtl_fe_dual_control_preset_pptp(&profile, false, key->ppp_len,
					key->flags);
	rtl_fe_dual_control_pack(control_words, &profile);
	if (key->decap)
		return 0;

	if (key->flags & RTL_FE_PPP_TUN_F_SEQ) {
		flags_ver |= RTL_FE_DUAL_PPTP_FLAG_SEQ;
		off += 4;
	}
	if (key->flags & RTL_FE_PPP_TUN_F_ACK) {
		flags_ver |= RTL_FE_DUAL_PPTP_FLAG_ACK;
		off += 4;
	}
	rtl_fe_tunnel_put_ip4(hdr, IPPROTO_GRE, key->src, key->dst);
	rtl_fe_tunnel_put_be16(hdr + RTL_FE_DUAL_IP4_HDR_LEN, flags_ver);
	rtl_fe_tunnel_put_be16(hdr + RTL_FE_DUAL_IP4_HDR_LEN + 2,
			       RTL_FE_DUAL_PPP_ETH_TYPE);
	rtl_fe_tunnel_put_be16(hdr + RTL_FE_DUAL_IP4_HDR_LEN + 6,
			       key->peer_tid);
	rtl_fe_tunnel_put_ppp(hdr + off, key->ppp_len);
	memcpy(content_words, hdr, sizeof(hdr));
	return 0;
}

static int rtl_fe_tunnel_build_words(const struct rtl_fe_tunnel_key *key,
				     u32 control_words[RTL_FE_DUAL_WORDS],
				     u32 content_words[RTL_FE_DUAL_WORDS])
{
	struct rtl_fe_dual_control profile;

	memset(content_words, 0, RTL_FE_DUAL_BYTES);
	switch (key->type) {
	case RTL_FE_DUAL_TYPE_IP4INIP6:
		rtl_fe_dual_control_preset_dslite(&profile);
		if (key->decap)
			profile.inr_to_out_fld =
				RTL_FE_DUAL_DSLITE_DECAP_INR_TO_OUT;
		rtl_fe_dual_control_pack(control_words, &profile);
		if (!key->decap) {
			content_words[0] = RTL_FE_DUAL_DSLITE_WORD0;
			content_words[1] = RTL_FE_DUAL_DSLITE_WORD1;
			/* The OEM copies raw header bytes; memcpy into the word
			 * array reproduces the exact image at buf+8 and buf+24.
			 */
			memcpy(&content_words[RTL_FE_DUAL_DSLITE_SRC_WORD],
			       key->src, RTL_FE_DUAL_DSLITE_ADDR_BYTES);
			memcpy(&content_words[RTL_FE_DUAL_DSLITE_DST_WORD],
			       key->dst, RTL_FE_DUAL_DSLITE_ADDR_BYTES);
		}
		return 0;
	case RTL_FE_DUAL_TYPE_6RD:
		rtl_fe_dual_control_preset_6rd(&profile);
		rtl_fe_dual_control_pack(control_words, &profile);
		if (!key->decap) {
			content_words[0] = RTL_FE_DUAL_6RD_WORD0;
			content_words[RTL_FE_DUAL_6RD_PROTO_WORD] =
				RTL_FE_DUAL_6RD_WORD2;
			memcpy(&content_words[RTL_FE_DUAL_6RD_SRC_WORD],
			       key->src, RTL_FE_DUAL_6RD_ADDR_BYTES);
			memcpy(&content_words[RTL_FE_DUAL_6RD_DST_WORD],
			       key->dst, RTL_FE_DUAL_6RD_ADDR_BYTES);
		}
		return 0;
	case RTL_FE_DUAL_TYPE_L2TP:
		return rtl_fe_tunnel_build_l2tp(key, control_words,
						content_words);
	case RTL_FE_DUAL_TYPE_PPTP:
		return rtl_fe_tunnel_build_pptp(key, control_words,
						content_words);
	default:
		return -EOPNOTSUPP;
	}
}

int rtl_fe_tunnel_program_dslite(struct rtl_fe *fe, bool decap,
				 const u8 src[RTL_FE_DUAL_DSLITE_ADDR_BYTES],
				 const u8 dst[RTL_FE_DUAL_DSLITE_ADDR_BYTES],
				 unsigned int *control, unsigned int *content)
{
	struct rtl_fe_tunnel_key key = {
		.type = RTL_FE_DUAL_TYPE_IP4INIP6,
		.decap = decap,
	};
	u32 control_words[RTL_FE_DUAL_WORDS];
	u32 content_words[RTL_FE_DUAL_WORDS];
	int ret;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	if (!src || !dst || !control || !content)
		return -EINVAL;

	memcpy(key.src, src, RTL_FE_DUAL_DSLITE_ADDR_BYTES);
	memcpy(key.dst, dst, RTL_FE_DUAL_DSLITE_ADDR_BYTES);
	ret = rtl_fe_tunnel_build_words(&key, control_words, content_words);
	if (ret)
		return ret;

	return rtl_fe_dual_program(fe, RTL_FE_DUAL_TYPE_IP4INIP6,
				   control_words, content_words, control,
				   content);
}

int rtl_fe_tunnel_program_6rd(struct rtl_fe *fe, bool decap,
			      const u8 src[RTL_FE_DUAL_6RD_ADDR_BYTES],
			      const u8 dst[RTL_FE_DUAL_6RD_ADDR_BYTES],
			      unsigned int *control, unsigned int *content)
{
	struct rtl_fe_tunnel_key key = {
		.type = RTL_FE_DUAL_TYPE_6RD,
		.decap = decap,
	};
	u32 control_words[RTL_FE_DUAL_WORDS];
	u32 content_words[RTL_FE_DUAL_WORDS];
	int ret;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	if (!src || !dst || !control || !content)
		return -EINVAL;

	memcpy(key.src, src, RTL_FE_DUAL_6RD_ADDR_BYTES);
	memcpy(key.dst, dst, RTL_FE_DUAL_6RD_ADDR_BYTES);
	ret = rtl_fe_tunnel_build_words(&key, control_words, content_words);
	if (ret)
		return ret;

	return rtl_fe_dual_program(fe, RTL_FE_DUAL_TYPE_6RD, control_words,
				   content_words, control, content);
}

int rtl_fe_tunnel_release(struct rtl_fe *fe, unsigned int control,
			  unsigned int content)
{
	int ret;

	if (!fe || !fe->tunnel)
		return -ENODEV;

	/* Control first: zeroing it stops the hardware from interpreting the
	 * Content bytes with the old profile (RE-7 §7.3).
	 */
	ret = rtl_fe_dual_control_free(fe, control);
	if (ret && ret != -ENOENT)
		return ret;
	ret = rtl_fe_dual_content_free(fe, content);
	if (ret && ret != -ENOENT)
		return ret;
	return 0;
}

static bool rtl_fe_tunnel_phys_busy_locked(const struct rtl_fe_tunnel *t,
					   unsigned int phys)
{
	unsigned int slot;

	if (phys < RTL_FE_EIF_ALIAS_BASE || phys >= RTL_FE_EIF_PHYS_ROWS)
		return false;

	slot = phys - RTL_FE_EIF_ALIAS_BASE;
	return t->sessions[slot].active ||
	       t->eif[phys].used ||
	       t->eif[RTL_FE_DUAL_HASH_EIF_BASE + slot].used ||
	       t->eif[RTL_FE_DUAL_ALT_EIF_BASE + slot].used;
}

bool rtl_fe_tunnel_phys_busy(struct rtl_fe *fe, unsigned int phys)
{
	struct rtl_fe_tunnel *t;
	bool busy;

	if (!fe || !fe->tunnel)
		return false;
	t = fe->tunnel;
	mutex_lock(&t->lock);
	busy = rtl_fe_tunnel_phys_busy_locked(t, phys);
	mutex_unlock(&t->lock);
	return busy;
}

static size_t rtl_fe_tunnel_addr_len(const struct rtl_fe_tunnel_key *key)
{
	if (key->type == RTL_FE_DUAL_TYPE_IP4INIP6)
		return RTL_FE_DUAL_DSLITE_ADDR_BYTES;
	if ((key->type == RTL_FE_DUAL_TYPE_L2TP ||
	     key->type == RTL_FE_DUAL_TYPE_PPTP) && key->af == AF_INET6)
		return RTL_FE_DUAL_DSLITE_ADDR_BYTES;
	return RTL_FE_DUAL_6RD_ADDR_BYTES;
}

static bool rtl_fe_tunnel_session_match(const struct rtl_fe_tunnel_session *s,
					const struct rtl_fe_tunnel_key *key,
					size_t addr_len)
{
	if (!s->active || s->type != key->type || s->decap != key->decap)
		return false;
	if (memcmp(s->src, key->src, addr_len) ||
	    memcmp(s->dst, key->dst, addr_len))
		return false;
	if (key->type == RTL_FE_DUAL_TYPE_L2TP ||
	    key->type == RTL_FE_DUAL_TYPE_PPTP) {
		if (s->af != key->af || s->ppp_len != key->ppp_len ||
		    s->flags != key->flags || s->sport != key->sport ||
		    s->dport != key->dport || s->tid != key->tid ||
		    s->sid != key->sid || s->peer_tid != key->peer_tid ||
		    s->peer_sid != key->peer_sid)
			return false;
	}
	if (key->decap)
		return s->cls_profile == key->cls_profile;
	return s->param_hash == key->param_hash;
}

int rtl_fe_tunnel_flow_get(struct rtl_fe *fe,
			   const struct rtl_fe_tunnel_key *key,
			   u16 busy_phys_mask,
			   struct rtl_fe_tunnel_binding *binding)
{
	u32 control_words[RTL_FE_DUAL_WORDS];
	u32 content_words[RTL_FE_DUAL_WORDS];
	unsigned int i, phys, cls_slot = 0, cls_index = 0;
	struct rtl_fe_tunnel *t;
	size_t addr_len;
	int ret;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	if (!key || !binding)
		return -EINVAL;
	if (key->decap && key->cls_profile >= RTL_FE_DUAL_CLS_PROFILES)
		return -EINVAL;

	ret = rtl_fe_tunnel_build_words(key, control_words, content_words);
	if (ret)
		return ret;

	t = fe->tunnel;
	addr_len = rtl_fe_tunnel_addr_len(key);

	/* Reuse an active paired session matching outer tunnel parameters. */
	mutex_lock(&t->lock);
	for (i = 0; i < RTL_FE_DUAL_CONTROL_ENTRIES; i++) {
		struct rtl_fe_tunnel_session *s = &t->sessions[i];

		if (!rtl_fe_tunnel_session_match(s, key, addr_len))
			continue;
		if (s->refs == (unsigned int)-1) {
			mutex_unlock(&t->lock);
			return -EOVERFLOW;
		}
		s->refs++;
		cls_index = s->cls_index;
		mutex_unlock(&t->lock);

		binding->dual_index = i;
		binding->hash_eif = RTL_FE_DUAL_HASH_EIF_BASE + i;
		binding->phys_eif = RTL_FE_EIF_ALIAS_BASE + i;
		binding->cls_index = cls_index;
		binding->need_cls_program = false;
		return 0;
	}

	/* Otherwise claim one locked dual_idx across Control/Content/EIF/CLS. */
	for (i = 0; i < RTL_FE_DUAL_CONTROL_ENTRIES; i++) {
		if (busy_phys_mask & BIT(i))
			continue;
		if (rtl_fe_tunnel_phys_busy_locked(t, RTL_FE_EIF_ALIAS_BASE + i))
			continue;
		if (!test_bit(i, t->control) && !test_bit(i, t->content))
			break;
	}
	if (i == RTL_FE_DUAL_CONTROL_ENTRIES) {
		mutex_unlock(&t->lock);
		return -ENOSPC;
	}

	if (key->decap) {
		cls_slot = find_first_zero_bit(t->cls_pairs[key->cls_profile],
					       RTL_FE_DUAL_CLS_PAIR_SLOTS);
		if (cls_slot >= RTL_FE_DUAL_CLS_PAIR_SLOTS) {
			mutex_unlock(&t->lock);
			return -ENOSPC;
		}
		__set_bit(cls_slot, t->cls_pairs[key->cls_profile]);
		cls_index = key->cls_profile * RTL_FE_CLS_KEY_PROFILE_ENTRIES +
			    cls_slot * 2;
	}

	__set_bit(i, t->control);
	t->control_slot[i].owner = RTL_FE_DUAL_OWNER_FLOW;
	t->control_slot[i].type = key->type;
	__set_bit(i, t->content);
	t->content_slot[i].owner = RTL_FE_DUAL_OWNER_FLOW;
	t->content_slot[i].type = key->type;

	t->sessions[i].active = true;
	t->sessions[i].decap = key->decap;
	t->sessions[i].type = key->type;
	t->sessions[i].af = key->af;
	t->sessions[i].ppp_len = key->ppp_len;
	t->sessions[i].flags = key->flags;
	t->sessions[i].sport = key->sport;
	t->sessions[i].dport = key->dport;
	t->sessions[i].tid = key->tid;
	t->sessions[i].sid = key->sid;
	t->sessions[i].peer_tid = key->peer_tid;
	t->sessions[i].peer_sid = key->peer_sid;
	t->sessions[i].refs = 1;
	t->sessions[i].cls_profile = key->cls_profile;
	t->sessions[i].cls_index = cls_index;
	t->sessions[i].cls_active = false;
	t->sessions[i].param_hash = key->param_hash;
	memcpy(t->sessions[i].src, key->src, addr_len);
	memcpy(t->sessions[i].dst, key->dst, addr_len);
	mutex_unlock(&t->lock);

	ret = rtl_fe_eif_alloc(fe, RTL_FE_DUAL_HASH_EIF_BASE + i,
			       key->param_hash, &phys);
	if (ret)
		goto err_unreserve;
	if (phys != RTL_FE_EIF_ALIAS_BASE + i) {
		ret = -EINVAL;
		goto err_free_eif;
	}

	ret = rtl_fe_dual_control_write(fe, i, control_words);
	if (ret)
		goto err_clear_ctrl;

	ret = rtl_fe_dual_content_write(fe, i, content_words);
	if (ret)
		goto err_clear_ctrl;

	if (!key->decap && (key->type == RTL_FE_DUAL_TYPE_L2TP ||
			    key->type == RTL_FE_DUAL_TYPE_PPTP)) {
		rtl_fe_write(fe, RTL_FE_L3PE_IPV4_ID(i),
			     RTL_FE_DUAL_IPV4_ID_BASE + i);
		if (key->type == RTL_FE_DUAL_TYPE_PPTP) {
			rtl_fe_write(fe, RTL_FE_L3PE_GRE_SEQ(i), key->gre_seq);
			rtl_fe_write(fe, RTL_FE_L3PE_GRE_ACK(i),
				     (key->flags & RTL_FE_PPP_TUN_F_ACK) ?
				     key->gre_ack : 0);
		}
	}

	binding->dual_index = i;
	binding->hash_eif = RTL_FE_DUAL_HASH_EIF_BASE + i;
	binding->phys_eif = phys;
	binding->cls_index = cls_index;
	binding->need_cls_program = key->decap;
	return 0;

err_clear_ctrl:
	rtl_fe_dual_control_free(fe, i);
err_free_eif:
	rtl_fe_eif_free(fe, RTL_FE_DUAL_HASH_EIF_BASE + i);
err_unreserve:
	mutex_lock(&t->lock);
	if (test_bit(i, t->control) &&
	    t->control_slot[i].owner == RTL_FE_DUAL_OWNER_FLOW) {
		__clear_bit(i, t->control);
		t->control_slot[i].owner = RTL_FE_DUAL_OWNER_NONE;
		t->control_slot[i].type = RTL_FE_DUAL_TYPE_NONE;
	}
	if (test_bit(i, t->content)) {
		__clear_bit(i, t->content);
		t->content_slot[i].owner = RTL_FE_DUAL_OWNER_NONE;
		t->content_slot[i].type = RTL_FE_DUAL_TYPE_NONE;
	}
	if (key->decap)
		__clear_bit(cls_slot, t->cls_pairs[key->cls_profile]);
	memset(&t->sessions[i], 0, sizeof(t->sessions[i]));
	mutex_unlock(&t->lock);
	return ret;
}

int rtl_fe_tunnel_flow_mark_cls(struct rtl_fe *fe, unsigned int dual_index)
{
	struct rtl_fe_tunnel *t;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	if (dual_index >= RTL_FE_DUAL_CONTROL_ENTRIES)
		return -EINVAL;

	t = fe->tunnel;
	mutex_lock(&t->lock);
	if (!t->sessions[dual_index].active) {
		mutex_unlock(&t->lock);
		return -ENOENT;
	}
	t->sessions[dual_index].cls_active = true;
	mutex_unlock(&t->lock);
	return 0;
}

int rtl_fe_tunnel_flow_cls_info(struct rtl_fe *fe, unsigned int dual_index,
				bool *is_last, bool *cls_active,
				unsigned int *cls_index)
{
	struct rtl_fe_tunnel *t;
	struct rtl_fe_tunnel_session *s;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	if (dual_index >= RTL_FE_DUAL_CONTROL_ENTRIES ||
	    !is_last || !cls_active || !cls_index)
		return -EINVAL;

	t = fe->tunnel;
	mutex_lock(&t->lock);
	s = &t->sessions[dual_index];
	if (!s->active || !s->refs) {
		mutex_unlock(&t->lock);
		return -ENOENT;
	}
	*is_last = s->refs == 1;
	*cls_active = s->cls_active;
	*cls_index = s->cls_index;
	mutex_unlock(&t->lock);
	return 0;
}

void rtl_fe_tunnel_flow_cls_cleared(struct rtl_fe *fe, unsigned int dual_index)
{
	struct rtl_fe_tunnel *t;
	struct rtl_fe_tunnel_session *s;
	unsigned int cls_slot;

	if (!fe || !fe->tunnel || dual_index >= RTL_FE_DUAL_CONTROL_ENTRIES)
		return;

	t = fe->tunnel;
	mutex_lock(&t->lock);
	s = &t->sessions[dual_index];
	if (s->active && s->cls_active) {
		cls_slot = (s->cls_index % RTL_FE_CLS_KEY_PROFILE_ENTRIES) / 2;
		if (s->cls_profile < RTL_FE_DUAL_CLS_PROFILES &&
		    cls_slot < RTL_FE_DUAL_CLS_PAIR_SLOTS)
			__clear_bit(cls_slot, t->cls_pairs[s->cls_profile]);
		s->cls_active = false;
	}
	mutex_unlock(&t->lock);
}

int rtl_fe_tunnel_flow_put(struct rtl_fe *fe, unsigned int dual_index)
{
	struct rtl_fe_tunnel *t;
	struct rtl_fe_tunnel_session *s;
	int ret;

	if (!fe || !fe->tunnel)
		return -ENODEV;
	if (dual_index >= RTL_FE_DUAL_CONTROL_ENTRIES)
		return -EINVAL;

	t = fe->tunnel;
	mutex_lock(&t->lock);
	s = &t->sessions[dual_index];
	if (!s->active || !s->refs) {
		mutex_unlock(&t->lock);
		return -ENOENT;
	}
	if (s->refs > 1) {
		s->refs--;
		mutex_unlock(&t->lock);
		return 0;
	}
	if (s->cls_active) {
		mutex_unlock(&t->lock);
		return -EBUSY;
	}
	mutex_unlock(&t->lock);

	ret = rtl_fe_tunnel_release(fe, dual_index, dual_index);
	if (ret)
		return ret;

	ret = rtl_fe_eif_free(fe, RTL_FE_DUAL_HASH_EIF_BASE + dual_index);
	if (ret && ret != -ENOENT)
		return ret;

	mutex_lock(&t->lock);
	s = &t->sessions[dual_index];
	if (s->decap && s->cls_profile < RTL_FE_DUAL_CLS_PROFILES) {
		unsigned int cls_slot =
			(s->cls_index % RTL_FE_CLS_KEY_PROFILE_ENTRIES) / 2;

		if (cls_slot < RTL_FE_DUAL_CLS_PAIR_SLOTS)
			__clear_bit(cls_slot, t->cls_pairs[s->cls_profile]);
	}
	memset(s, 0, sizeof(*s));
	mutex_unlock(&t->lock);
	return 0;
}

/* --- debugfs --- */

#define RTL_FE_DUAL_CMD_BUF_LEN		512

/* The dual-header flow path is opt-in ("dual_hdr=1") so unmodified systems stay
 * on the legacy -EOPNOTSUPP path without programming 0x3348/LPB_TBL_3 or CLS.
 */
static bool rtl_fe_tunnel_flow_enable;
module_param_named(dual_hdr, rtl_fe_tunnel_flow_enable, bool, 0644);
MODULE_PARM_DESC(dual_hdr, "Enable DS-Lite/6RD dual-header hardware flow offload");
static bool rtl_fe_vpn_offload_enable = true;
module_param_named(vpn_offload, rtl_fe_vpn_offload_enable, bool, 0644);
MODULE_PARM_DESC(vpn_offload,
		 "Enable L2TPv2/PPTP cleartext PPP hardware flow offload (default on)");
module_param_named(clear_on_free, rtl_fe_dual_clear_on_free, bool, 0644);
MODULE_PARM_DESC(clear_on_free,
		 "Clear a dual-header entry when its slot is freed (default on)");

bool rtl_fe_tunnel_flow_enabled(void)
{
	return rtl_fe_tunnel_flow_enable;
}

bool rtl_fe_vpn_offload_enabled(void)
{
	return rtl_fe_vpn_offload_enable;
}

static const char *rtl_fe_dual_type_name(unsigned int type)
{
	switch (type) {
	case RTL_FE_DUAL_TYPE_NONE:
		return "none";
	case RTL_FE_DUAL_TYPE_IP4INIP6:
		return "ds-lite";
	case RTL_FE_DUAL_TYPE_VXLAN:
		return "vxlan";
	case RTL_FE_DUAL_TYPE_6RD:
		return "6rd";
	case RTL_FE_DUAL_TYPE_MAPT:
		return "mapt";
	case RTL_FE_DUAL_TYPE_L2TP:
		return "l2tp";
	case RTL_FE_DUAL_TYPE_PPTP:
		return "pptp";
	default:
		return "unknown";
	}
}

static void rtl_fe_dual_format_content(char *buf, size_t size,
				       unsigned int index,
				       const struct rtl_fe_dual_slot *slot,
				       const u32 words[RTL_FE_DUAL_WORDS])
{
	int len;
	unsigned int i;

	len = scnprintf(buf, size, "content %u owner=%u type=%u(%s) words=",
			index, slot->owner, slot->type,
			rtl_fe_dual_type_name(slot->type));
	for (i = 0; i < RTL_FE_DUAL_WORDS; i++)
		len += scnprintf(buf + len, size - len, " %08x", words[i]);
	scnprintf(buf + len, size - len, "\n");
}

static void rtl_fe_dual_format_control(char *buf, size_t size,
				       unsigned int index,
				       const struct rtl_fe_dual_slot *slot,
				       const u32 words[RTL_FE_DUAL_WORDS])
{
	struct rtl_fe_dual_control c;
	int len;

	rtl_fe_dual_control_unpack(words, &c);
	len = scnprintf(buf, size,
			"control %u owner=%u type=%u(%s) raw=%08x,%08x,%08x,%08x\n",
			index, slot->owner, slot->type,
			rtl_fe_dual_type_name(slot->type),
			words[0], words[1], words[2], words[3]);
	len += scnprintf(buf + len, size - len,
			 "  hdr_fmt=%u hdr_len=%u mtu_diff=%u\n",
			 c.hdr_fmt, c.hdr_len, c.mtu_diff);
	len += scnprintf(buf + len, size - len,
			 "  ppp_ip_ver_ost=%u rep_ip_en=%u rep_ip_ver=%u\n",
			 c.ppp_ip_ver_ost, c.rep_ip_en, c.rep_ip_ver);
	len += scnprintf(buf + len, size - len,
			 "  rep_ip_len=%u rep_ip_len_diff=%u\n",
			 c.rep_ip_len, c.rep_ip_len_diff);
	len += scnprintf(buf + len, size - len,
			 "  rep_ip_dscp_en=%u rep_ip_ecn_en=%u\n",
			 c.rep_ip_dscp_en, c.rep_ip_ecn_en);
	len += scnprintf(buf + len, size - len,
			 "  rep_ip_ptc=%u rep_ip_ttl=%u\n",
			 c.rep_ip_ptc, c.rep_ip_ttl);
	len += scnprintf(buf + len, size - len,
			 "  rep_len_ost1=%u rep_len_diff1=%u\n",
			 c.rep_len_ost1, c.rep_len_diff1);
	len += scnprintf(buf + len, size - len,
			 "  rep_len_ost2=%u rep_len_diff2=%u\n",
			 c.rep_len_ost2, c.rep_len_diff2);
	len += scnprintf(buf + len, size - len,
			 "  ip4_ost=%u gre_seq_ack_ost=%u gre_mod_fld=%u\n",
			 c.ip4_ost, c.gre_seq_ack_ost, c.gre_mod_fld);
	len += scnprintf(buf + len, size - len,
			 "  ip6_oft=%u mapt_pre_len=%u map_drf_ver=%u\n",
			 c.ip6_oft, c.mapt_pre_len, c.map_drf_ver);
	len += scnprintf(buf + len, size - len,
			 "  6rd_dip_pre_len=%u 6rd_dip_suf_str_bit=%u\n",
			 c.sixrd_dip_pre_len, c.sixrd_dip_suf_str_bit);
	len += scnprintf(buf + len, size - len,
			 "  dsl_uc_chk=%u dsl_mc_chk=%u l4sum_rec=%u\n",
			 c.dsl_uc_chk, c.dsl_mc_chk, c.l4sum_rec);
	len += scnprintf(buf + len, size - len,
			 "  vxlan_sp_ost=%u inr_to_out_fld=0x%02x\n",
			 c.vxlan_sp_ost, c.inr_to_out_fld);
	scnprintf(buf + len, size - len,
		  "  tos_hsh_sel=%u mc_mac_map=%u pad=%u%u%u\n",
		  c.tos_hsh_sel, c.mc_mac_map, c.rep_ip_len_diff_pad_ctrl,
		  c.rep_len_diff1_pad_ctrl, c.rep_len_diff2_pad_ctrl);
}

static void rtl_fe_dual_format_fmr(char *buf, size_t size, unsigned int index,
				   const struct rtl_fe_dual_slot *slot,
				   const u32 words[RTL_FE_DUAL_WORDS])
{
	struct rtl_fe_dual_fmr f;
	int len;

	rtl_fe_dual_fmr_unpack(words, &f);
	len = scnprintf(buf, size, "fmr %u owner=%u type=%u(%s)\n",
			index, slot->owner, slot->type,
			rtl_fe_dual_type_name(slot->type));
	len += scnprintf(buf + len, size - len,
			 "  prefix=%016llx psid_str_ost=%u psid_end_ost=%u\n",
			 (unsigned long long)f.prefix, f.psid_str_ost,
			 f.psid_end_ost);
	scnprintf(buf + len, size - len,
		  "  psid_refill=%u pref_len=%u drf_ver=%u word2=%08x\n",
		  f.psid_refill, f.pref_len, f.drf_ver, words[2]);
}

static int rtl_fe_dual_show(struct seq_file *s, void *unused)
{
	struct rtl_fe *fe = s->private;
	struct rtl_fe_tunnel *t = fe->tunnel;
	u32 words[RTL_FE_DUAL_WORDS];
	char line[RTL_FE_DUAL_LAST_LEN];
	unsigned int i, p, refs;
	int ret;

	if (!t)
		return -ENODEV;

	mutex_lock(&t->lock);
	seq_printf(s, "ready=%u last: %s\n", t->ready, t->last);
	seq_puts(s, "content bitmap:");
	for (i = 0; i < BITS_TO_LONGS(RTL_FE_DUAL_CONTENT_ENTRIES); i++)
		seq_printf(s, " %016lx", t->content[i]);
	seq_puts(s, "\ncontrol bitmap:");
	for (i = 0; i < BITS_TO_LONGS(RTL_FE_DUAL_CONTROL_ENTRIES); i++)
		seq_printf(s, " %016lx", t->control[i]);
	seq_puts(s, "\nfmr bitmap:");
	for (i = 0; i < BITS_TO_LONGS(RTL_FE_DUAL_FMR_ENTRIES); i++)
		seq_printf(s, " %016lx", t->fmr[i]);
	seq_putc(s, '\n');

	for (i = 0; i < RTL_FE_DUAL_CONTENT_ENTRIES; i++)
		seq_printf(s, "content[%u]: owner=%u type=%u(%s) %s\n", i,
			   t->content_slot[i].owner, t->content_slot[i].type,
			   rtl_fe_dual_type_name(t->content_slot[i].type),
			   test_bit(i, t->content) ? "allocated" : "free");
	for (i = 0; i < RTL_FE_DUAL_CONTROL_ENTRIES; i++)
		seq_printf(s, "control[%u]: owner=%u type=%u(%s) %s\n", i,
			   t->control_slot[i].owner, t->control_slot[i].type,
			   rtl_fe_dual_type_name(t->control_slot[i].type),
			   test_bit(i, t->control) ? "allocated" : "free");
	for (i = 0; i < RTL_FE_DUAL_FMR_ENTRIES; i++)
		seq_printf(s, "fmr[%u]: owner=%u type=%u(%s) %s\n", i,
			   t->fmr_slot[i].owner, t->fmr_slot[i].type,
			   rtl_fe_dual_type_name(t->fmr_slot[i].type),
			   test_bit(i, t->fmr) ? "allocated" : "free");

	for (i = 0; i < RTL_FE_DUAL_CONTENT_ENTRIES; i++) {
		if (!test_bit(i, t->content))
			continue;
		ret = rtl_fe_dual_hw_read(fe, RTL_FE_DUAL_SEL_CONTENT, i,
					  words);
		if (ret) {
			seq_printf(s, "content %u read failed: %d\n", i, ret);
			continue;
		}
		rtl_fe_dual_format_content(line, sizeof(line), i,
					   &t->content_slot[i], words);
		seq_puts(s, line);
	}
	for (i = 0; i < RTL_FE_DUAL_CONTROL_ENTRIES; i++) {
		if (!test_bit(i, t->control))
			continue;
		ret = rtl_fe_dual_hw_read(fe, RTL_FE_DUAL_SEL_CONTROL, i,
					  words);
		if (ret) {
			seq_printf(s, "control %u read failed: %d\n", i, ret);
			continue;
		}
		rtl_fe_dual_format_control(line, sizeof(line), i,
					   &t->control_slot[i], words);
		seq_puts(s, line);
	}
	for (i = 0; i < RTL_FE_DUAL_FMR_ENTRIES; i++) {
		if (!test_bit(i, t->fmr))
			continue;
		ret = rtl_fe_dual_hw_read(fe, RTL_FE_DUAL_SEL_FMR, i, words);
		if (ret) {
			seq_printf(s, "fmr %u read failed: %d\n", i, ret);
			continue;
		}
		rtl_fe_dual_format_fmr(line, sizeof(line), i,
				       &t->fmr_slot[i], words);
		seq_puts(s, line);
	}
	seq_puts(s, "eif hash slots:\n");
	for (i = 0; i < RTL_FE_EIF_HASH_INDICES; i++)
		seq_printf(s,
			   "  hash %2u: phys=%2u used=%u refs=%u param=%08x\n",
			   i, (unsigned int)t->eif[i].phys,
			   (unsigned int)t->eif[i].used,
			   (unsigned int)t->eif[i].refs,
			   t->eif[i].param_hash);
	seq_puts(s, "eif physical rows 16..31:\n");
	for (p = RTL_FE_EIF_ALIAS_BASE; p < RTL_FE_EIF_PHYS_ROWS; p++) {
		refs = 0;
		seq_printf(s, "  phys %2u:", p);
		for (i = 0; i < RTL_FE_EIF_HASH_INDICES; i++) {
			if (!t->eif[i].used || t->eif[i].phys != p)
				continue;
			refs++;
			seq_printf(s, " hash%u(param=%08x)", i,
				   t->eif[i].param_hash);
		}
		if (!refs)
			seq_puts(s, " free");
		else
			seq_printf(s, " refs=%u", refs);
		seq_putc(s, '\n');
	}
	mutex_unlock(&t->lock);
	return 0;
}

static char *rtl_fe_dual_token(char **cursor)
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

static int rtl_fe_dual_parse_type(const char *token, unsigned int *type)
{
	unsigned int value;

	if (sysfs_streq(token, "DS-Lite") || sysfs_streq(token, "ds-lite") ||
	    sysfs_streq(token, "DSLITE") || sysfs_streq(token, "dslite") ||
	    sysfs_streq(token, "IP4INIP6") || sysfs_streq(token, "ip4inip6")) {
		*type = RTL_FE_DUAL_TYPE_IP4INIP6;
		return 0;
	}
	if (sysfs_streq(token, "6RD") || sysfs_streq(token, "6rd")) {
		*type = RTL_FE_DUAL_TYPE_6RD;
		return 0;
	}
	if (sysfs_streq(token, "MAP-T") || sysfs_streq(token, "map-t") ||
	    sysfs_streq(token, "MAPT") || sysfs_streq(token, "mapt")) {
		*type = RTL_FE_DUAL_TYPE_MAPT;
		return 0;
	}
	if (sysfs_streq(token, "L2TP") || sysfs_streq(token, "l2tp")) {
		*type = RTL_FE_DUAL_TYPE_L2TP;
		return 0;
	}
	if (sysfs_streq(token, "PPTP") || sysfs_streq(token, "pptp")) {
		*type = RTL_FE_DUAL_TYPE_PPTP;
		return 0;
	}
	if (!kstrtouint(token, 0, &value)) {
		*type = value;
		return 0;
	}
	return -EINVAL;
}

/* Usage, one command per write (the read side dumps the three tables and the
 * software EIF alias arbitration state):
 *
 *   content-alloc / content-free <idx>
 *   control-alloc / control-free <idx>
 *   fmr-alloc / fmr-free <idx>
 *   control-set <idx> <DS-Lite|6RD|MAP-T>   measured Control literal set
 *   control-raw <idx> <w0> .. <w15>         raw Control words
 *   control-get <idx>                       read back and decode one entry
 *   content-raw <idx> <128 hex digits>      raw 64-byte Content entry,
 *                                           eight digits per word, word0 first
 *   content-get <idx>
 *   fmr-set <idx> <prefix_hi> <prefix_lo> <word2>
 *   fmr-get <idx>
 *   eif <hash>                              hash index -> physical EIF row
 *   eif-alloc <hash> <param>                software alias arbitration
 *   eif-free <hash>
 *
 * Every index is bounds checked; Content hex must be exactly 128 digits and
 * FMR word2 may only use bits 16:0.  Nothing writes outside its table.
 * content-free/control-free/fmr-free clear the hardware entry first (Content
 * and Control to zero, FMR to 0xffffffff) unless clear_on_free=0; the DS-Lite
 * and 6RD flow parser is only active when the dual_hdr module parameter is set.
 */
static ssize_t rtl_fe_dual_debugfs_write(struct file *file,
					 const char __user *ubuf,
					 size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct rtl_fe *fe = s->private;
	struct rtl_fe_tunnel *t = fe->tunnel;
	char buf[RTL_FE_DUAL_CMD_BUF_LEN], *cursor, *cmd, *token;
	u32 words[RTL_FE_DUAL_WORDS];
	struct rtl_fe_dual_slot slot;
	u32 prefix_hi, prefix_lo, word2, param;
	unsigned int index, phys, type, i;
	int ret;

	if (!t)
		return -ENODEV;
	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	cursor = buf;
	cmd = rtl_fe_dual_token(&cursor);
	if (!cmd)
		return -EINVAL;

	if (sysfs_streq(cmd, "dump")) {
		if (rtl_fe_dual_token(&cursor))
			return -EINVAL;
		return count;
	}

	if (sysfs_streq(cmd, "content-alloc") ||
	    sysfs_streq(cmd, "control-alloc") ||
	    sysfs_streq(cmd, "fmr-alloc")) {
		if (rtl_fe_dual_token(&cursor))
			return -EINVAL;
		if (sysfs_streq(cmd, "content-alloc"))
			ret = rtl_fe_dual_content_alloc(fe, &index,
						RTL_FE_DUAL_OWNER_DEBUGFS);
		else if (sysfs_streq(cmd, "control-alloc"))
			ret = rtl_fe_dual_control_alloc(fe, &index,
						RTL_FE_DUAL_OWNER_DEBUGFS);
		else
			ret = rtl_fe_dual_fmr_alloc(fe, &index,
						RTL_FE_DUAL_OWNER_DEBUGFS);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		snprintf(t->last, sizeof(t->last), "%s -> %u", cmd, index);
		mutex_unlock(&t->lock);
		return count;
	}

	token = rtl_fe_dual_token(&cursor);
	if (!token || kstrtouint(token, 0, &index))
		return -EINVAL;

	if (sysfs_streq(cmd, "content-free")) {
		if (rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_dual_content_free(fe, index);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		snprintf(t->last, sizeof(t->last), "content-free %u ok",
			 index);
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "control-free")) {
		if (rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_dual_control_free(fe, index);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		snprintf(t->last, sizeof(t->last), "control-free %u ok",
			 index);
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "fmr-free")) {
		if (rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_dual_fmr_free(fe, index);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		snprintf(t->last, sizeof(t->last), "fmr-free %u ok", index);
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "content-get")) {
		if (rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_dual_content_read(fe, index, words);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		slot = t->content_slot[index];
		rtl_fe_dual_format_content(t->last, sizeof(t->last), index,
					   &slot, words);
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "control-get")) {
		if (rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_dual_control_read(fe, index, words);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		slot = t->control_slot[index];
		rtl_fe_dual_format_control(t->last, sizeof(t->last), index,
					   &slot, words);
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "fmr-get")) {
		if (rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_dual_fmr_read(fe, index, words);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		slot = t->fmr_slot[index];
		rtl_fe_dual_format_fmr(t->last, sizeof(t->last), index, &slot,
				       words);
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "content-raw")) {
		token = rtl_fe_dual_token(&cursor);
		if (!token || rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_dual_parse_hex(token, words);
		if (ret)
			goto failed;
		ret = rtl_fe_dual_content_write(fe, index, words);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		snprintf(t->last, sizeof(t->last), "content-raw %u ok",
			 index);
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "control-raw")) {
		for (i = 0; i < RTL_FE_DUAL_WORDS; i++) {
			token = rtl_fe_dual_token(&cursor);
			if (!token || kstrtou32(token, 0, &words[i]))
				return -EINVAL;
		}
		if (rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_dual_control_write(fe, index, words);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		snprintf(t->last, sizeof(t->last), "control-raw %u ok",
			 index);
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "control-set")) {
		token = rtl_fe_dual_token(&cursor);
		if (!token || rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_dual_parse_type(token, &type);
		if (ret)
			goto failed;
		ret = rtl_fe_dual_control_set_type(fe, index, type);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		snprintf(t->last, sizeof(t->last), "control-set %u %s ok",
			 index, rtl_fe_dual_type_name(type));
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "fmr-set")) {
		token = rtl_fe_dual_token(&cursor);
		if (!token || kstrtou32(token, 0, &prefix_hi))
			return -EINVAL;
		token = rtl_fe_dual_token(&cursor);
		if (!token || kstrtou32(token, 0, &prefix_lo))
			return -EINVAL;
		token = rtl_fe_dual_token(&cursor);
		if (!token || kstrtou32(token, 0, &word2))
			return -EINVAL;
		if (rtl_fe_dual_token(&cursor) ||
		    (word2 & ~RTL_FE_DUAL_FMR_W2_VALID_MASK))
			return -EINVAL;

		memset(words, 0, sizeof(words));
		words[0] = prefix_lo;
		words[1] = prefix_hi;
		words[2] = word2;
		ret = rtl_fe_dual_fmr_write(fe, index, words);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		snprintf(t->last, sizeof(t->last), "fmr-set %u ok", index);
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "eif-alloc")) {
		token = rtl_fe_dual_token(&cursor);
		if (!token || kstrtou32(token, 0, &param) ||
		    rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_eif_alloc(fe, index, param, &phys);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		snprintf(t->last, sizeof(t->last),
			 "eif-alloc %u -> phys %u param %08x", index, phys,
			 param);
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "eif-free")) {
		if (rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_eif_free(fe, index);
		if (ret)
			goto failed;
		mutex_lock(&t->lock);
		snprintf(t->last, sizeof(t->last), "eif-free %u ok", index);
		mutex_unlock(&t->lock);
		return count;
	}

	if (sysfs_streq(cmd, "eif")) {
		if (rtl_fe_dual_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_eif_phys(index);
		if (ret < 0)
			goto failed;
		mutex_lock(&t->lock);
		snprintf(t->last, sizeof(t->last),
			 "eif hash %u -> physical row %d", index, ret);
		mutex_unlock(&t->lock);
		return count;
	}

	return -EINVAL;

failed:
	mutex_lock(&t->lock);
	snprintf(t->last, sizeof(t->last), "%s failed: %d", cmd, ret);
	mutex_unlock(&t->lock);
	return ret;
}

static int rtl_fe_dual_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl_fe_dual_show, inode->i_private);
}

static const struct file_operations rtl_fe_dual_fops = {
	.owner = THIS_MODULE,
	.open = rtl_fe_dual_open,
	.read = seq_read,
	.write = rtl_fe_dual_debugfs_write,
	.llseek = seq_lseek,
	.release = single_release,
};

int rtl_fe_tunnel_init(struct rtl_fe *fe)
{
	struct rtl_fe_tunnel *t;

	if (!fe)
		return -EINVAL;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->fe = fe;
	mutex_init(&t->lock);
	bitmap_zero(t->content, RTL_FE_DUAL_CONTENT_ENTRIES);
	bitmap_zero(t->control, RTL_FE_DUAL_CONTROL_ENTRIES);
	bitmap_zero(t->fmr, RTL_FE_DUAL_FMR_ENTRIES);
	memset(t->eif, 0, sizeof(t->eif));
	t->ready = true;
	snprintf(t->last, sizeof(t->last), "initialised");
	fe->tunnel = t;
	return 0;
}

void rtl_fe_tunnel_exit(struct rtl_fe *fe)
{
	struct rtl_fe_tunnel *t;

	if (!fe || !fe->tunnel)
		return;
	t = fe->tunnel;
	fe->tunnel = NULL;
	memset(t->eif, 0, sizeof(t->eif));
	mutex_destroy(&t->lock);
	kfree(t);
}

void rtl_fe_tunnel_debugfs_init(struct rtl_fe *fe)
{
	if (!fe || !fe->tunnel || !fe->debugfs)
		return;
	debugfs_create_file("tunnel", 0600, fe->debugfs, fe,
			    &rtl_fe_dual_fops);
}
