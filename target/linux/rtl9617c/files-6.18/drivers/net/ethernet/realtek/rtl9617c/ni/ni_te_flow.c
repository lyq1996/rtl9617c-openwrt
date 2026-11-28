// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RTL9617C L3TE flow/group policer, WRED and L3 DoS register layer.
 *
 * Register facts and the indirect access protocol come from the vendor
 * module ca-ne.ko (see ni_te_flow.h for the function/address citations and
 * phase0-re2-policer-wred-dos.md sections 1/2/4/5). Two behaviours are enforced
 * here because they are hardware constraints rather than policy:
 *
 *  - DATA0 sits at the *highest* address of an indirect entry and the write
 *    order is DATA0 -> DATA1 -> ... -> ACCESS (ca-ne.ko
 *    __l3_te_policer_flow_profile_access.constprop.0 @0x73e90).
 *  - 32 policer slots share one 16 bit cfg field, so the cfg register may
 *    only be rewritten while every other slot of the bank is still at its
 *    idle sentinel (ca-ne.ko aal_l3_te_policer_flow_cfg_set @0x6f6c0 and
 *    fc_mgr rtk_8277c_l3_flow_policer_mode_set @0xc63b4).
 *
 * Unassigned register bits are preserved through read-modify-write.
 */
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/build_bug.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include "ni.h"
#include "ni_te_flow.h"

/* Lock the documented table layouts against the generic address walkers. */
static_assert(L3TE_POL_FLOW_DATA0 - L3TE_POL_FLOW_ACC == 0xc);
static_assert(L3TE_POL_FLOW_DATA1 - L3TE_POL_FLOW_ACC == 0x8);
static_assert(L3TE_POL_FLOW_DATA2 - L3TE_POL_FLOW_ACC == 0x4);
static_assert(L3TE_POL_GRP_DATA0 - L3TE_POL_GRP_ACC == 0xc);
static_assert(L3TE_POL_GRP_DATA1 - L3TE_POL_GRP_ACC == 0x8);
static_assert(L3TE_POL_GRP_DATA2 - L3TE_POL_GRP_ACC == 0x4);
static_assert(L3TE_POL_FLOW3_DATA0 - L3TE_POL_FLOW3_ACC == 0xc);
static_assert(L3TE_POL_FLOW3_DATA1 - L3TE_POL_FLOW3_ACC == 0x8);
static_assert(L3TE_POL_FLOW3_DATA2 - L3TE_POL_FLOW3_ACC == 0x4);
static_assert(L3TE_WRED_AFGS_DATA - L3TE_WRED_AFGS_ACC == 4);
static_assert(L3TE_WRED_PROF_DATA(0) == 0x93ac);
static_assert(L3TE_WRED_PROF_DATA(7) == 0x9390);
static_assert(L3TE_WRED_SEL_DATA(0) == 0x93b8);
static_assert(L3TE_WRED_SEL_DATA(1) == 0x93b4);

/* Register access seams; tests/te-policer.py substitutes these two. */
static u32 te_read(struct rtl_te_flow *te, u32 off)
{
	return rtl_ne_read(te->regs, off);
}

static void te_write(struct rtl_te_flow *te, u32 off, u32 val)
{
	rtl_ne_write(te->regs, off, val);
}

/* ==== BEGIN test-extractable logic (tests/te-policer.py) ==== */

/*
 * kbps = L3TE_POL_RATE_DIVISOR * hi + lo (spec section 2.3). The 16 bit hi field
 * bounds the encodable rate, so anything faster is out of range.
 */
static int te_policer_encode_rate(u64 bps, u16 *hi, u16 *lo)
{
	u64 kbps;

	if (bps > (u64)L3TE_POL_RATE_MAX_KBPS * L3TE_POL_BPS_PER_KBPS)
		return -ERANGE;
	kbps = (bps + L3TE_POL_BPS_PER_KBPS - 1u) / L3TE_POL_BPS_PER_KBPS;
	if (!kbps)
		return -ERANGE;
	*hi = (u16)(kbps / L3TE_POL_RATE_DIVISOR);
	*lo = (u16)(kbps % L3TE_POL_RATE_DIVISOR);
	return 0;
}

static int te_policer_encode(u64 cir_bps, u64 pir_bps, u32 cbs, u32 pbs,
			     struct rtl_te_policer_profile *p)
{
	int ret;

	if (!cbs || cbs > L3TE_POL_BURST_MAX)
		return -EINVAL;
	if (!pbs || pbs > L3TE_POL_BURST_MAX)
		return -EINVAL;
	if (cir_bps > pir_bps)
		return -EINVAL;

	ret = te_policer_encode_rate(cir_bps, &p->cir_hi, &p->cir_lo);
	if (ret)
		return ret;
	ret = te_policer_encode_rate(pir_bps, &p->pir_hi, &p->pir_lo);
	if (ret)
		return ret;
	p->cbs = (u16)cbs;
	p->pbs = (u16)pbs;
	return 0;
}

/* section 2.2 bit field placement, verified instruction by instruction. */
static void te_policer_pack(const struct rtl_te_policer_profile *p,
			    u32 words[3])
{
	words[0] = ((u32)p->cir_lo & L3TE_POL_RATE_LO_MASK) |
		   (((u32)p->cir_hi & L3TE_POL_RATE_HI_MASK) <<
		    L3TE_POL_W0_CIR_HI_SHIFT) |
		   (((u32)p->cbs & L3TE_POL_W0_CBS_LO_MASK) <<
		    L3TE_POL_W0_CBS_LO_SHIFT);
	words[1] = (((u32)p->cbs >> L3TE_POL_W0_CBS_LO_BITS) &
		    L3TE_POL_W1_CBS_HI_MASK) |
		   (((u32)p->pir_lo & L3TE_POL_RATE_LO_MASK) <<
		    L3TE_POL_W1_PIR_LO_SHIFT) |
		   (((u32)p->pir_hi & L3TE_POL_RATE_HI_MASK) <<
		    L3TE_POL_W1_PIR_HI_SHIFT);
	words[2] = (u32)p->pbs & L3TE_POL_BURST_MASK;
}

static void te_policer_unpack(const u32 words[3],
			      struct rtl_te_policer_profile *p)
{
	p->cir_lo = (u16)(words[0] & L3TE_POL_RATE_LO_MASK);
	p->cir_hi = (u16)((words[0] >> L3TE_POL_W0_CIR_HI_SHIFT) &
			  L3TE_POL_RATE_HI_MASK);
	p->cbs = (u16)(((words[0] >> L3TE_POL_W0_CBS_LO_SHIFT) &
			L3TE_POL_W0_CBS_LO_MASK) |
		       ((words[1] & L3TE_POL_W1_CBS_HI_MASK) <<
			L3TE_POL_W0_CBS_LO_BITS));
	p->pir_lo = (u16)((words[1] >> L3TE_POL_W1_PIR_LO_SHIFT) &
			  L3TE_POL_RATE_LO_MASK);
	p->pir_hi = (u16)((words[1] >> L3TE_POL_W1_PIR_HI_SHIFT) &
			  L3TE_POL_RATE_HI_MASK);
	p->pbs = (u16)(words[2] & L3TE_POL_BURST_MASK);
}

/*
 * An unused slot reads back with the four rate fields all ones. This mirrors
 * fc_mgr rtk_8277c_l3_flow_policer_mode_set @0xc63b4, which tests
 * profile[0] == 0x3ff && profile[3] == 0x3ff && both hi fields 0xffff.
 */
static bool te_policer_idle(const u32 words[3])
{
	return (words[0] & L3TE_POL_RATE_LO_MASK) == L3TE_POL_RATE_LO_MASK &&
	       ((words[0] >> L3TE_POL_W0_CIR_HI_SHIFT) &
		L3TE_POL_RATE_HI_MASK) == L3TE_POL_RATE_HI_MASK &&
	       ((words[1] >> L3TE_POL_W1_PIR_LO_SHIFT) &
		L3TE_POL_RATE_LO_MASK) == L3TE_POL_RATE_LO_MASK &&
	       ((words[1] >> L3TE_POL_W1_PIR_HI_SHIFT) &
		L3TE_POL_RATE_HI_MASK) == L3TE_POL_RATE_HI_MASK;
}

static void te_policer_sentinel(u32 words[3])
{
	struct rtl_te_policer_profile p = {
		.cir_lo = L3TE_POL_RATE_LO_MASK,
		.cir_hi = L3TE_POL_RATE_HI_MASK,
		.cbs = L3TE_POL_BURST_MASK,
		.pir_lo = L3TE_POL_RATE_LO_MASK,
		.pir_hi = L3TE_POL_RATE_HI_MASK,
		.pbs = L3TE_POL_BURST_MASK,
	};

	te_policer_pack(&p, words);
}

static int te_wait_idle(struct rtl_te_flow *te, u32 acc)
{
	unsigned int i;

	for (i = 0; i < L3TE_IND_POLL_MAX; i++) {
		if (!(te_read(te, acc) & L3TE_IND_BUSY))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/*
 * Move one entry through the shared ACCESS/DATA window. Data word i lands at
 * acc + 4 * (count - i), i.e. DATA0 at the highest address, and the ACCESS
 * write is fenced so the posted data writes cannot be reordered after it.
 */
static int te_table_access(struct rtl_te_flow *te, u32 acc,
			   unsigned int idx_bits, bool write, u32 idx,
			   u32 *words, unsigned int count)
{
	unsigned int i;
	u32 cmd;
	int ret;

	ret = te_wait_idle(te, acc);
	if (ret)
		return ret;

	if (write) {
		for (i = 0; i < count; i++)
			te_write(te, acc + 4u * (count - i), words[i]);
		wmb(); /* flush data writes before triggering indirect access */
	}

	cmd = L3TE_IND_BUSY | (write ? L3TE_IND_WRITE : 0) |
	      (idx & L3TE_IND_IDX_MASK(idx_bits));
	te_write(te, acc, cmd);

	ret = te_wait_idle(te, acc);
	if (ret)
		return ret;

	if (!write) {
		rmb(); /* order indirect read completion before data reads */
		for (i = 0; i < count; i++)
			words[i] = te_read(te, acc + 4u * (count - i));
	}
	return 0;
}

static int te_pol_profile_read(struct rtl_te_flow *te, u32 acc,
			       unsigned int idx_bits, unsigned int idx,
			       u32 words[3])
{
	return te_table_access(te, acc, idx_bits, false, idx, words, 3);
}

static int te_pol_profile_write(struct rtl_te_flow *te, u32 acc,
				unsigned int idx_bits, unsigned int idx,
				const u32 words[3])
{
	u32 tmp[3];

	memcpy(tmp, words, sizeof(tmp));
	return te_table_access(te, acc, idx_bits, true, idx, tmp, 3);
}

/*
 * True when any other slot of the bank still holds a live profile. The cfg
 * register of a bank is shared by L3TE_POL_BANK_SLOTS slots, so its value may
 * not change while a sibling depends on it.
 */
static int te_bank_scan(struct rtl_te_flow *te, u32 acc, unsigned int idx_bits,
			unsigned int first, unsigned int skip, bool *busy)
{
	unsigned int i;
	u32 words[3];
	int ret;

	*busy = false;
	for (i = 0; i < L3TE_POL_BANK_SLOTS; i++) {
		unsigned int idx = first + i;

		if (idx == skip)
			continue;
		ret = te_pol_profile_read(te, acc, idx_bits, idx, words);
		if (ret)
			return ret;
		if (!te_policer_idle(words)) {
			*busy = true;
			break;
		}
	}
	return 0;
}

/*
 * Program the shared cfg field. Only bits [2:0] (type: 0 off / 2 on) are
 * owned here; every other cfg bit, including rate mode (U4) and the unknown
 * [3],[5],[10:6],[11],[12],[13],[15:14] fields (U3), is preserved.
 */
static int te_bank_cfg_set(struct rtl_te_flow *te, u32 cfg_reg, u32 acc,
			   unsigned int idx_bits, unsigned int slot,
			   bool enable)
{
	unsigned int first = slot & ~(L3TE_POL_BANK_SLOTS - 1u);
	bool busy;
	u32 cfg;
	int ret;

	ret = te_bank_scan(te, acc, idx_bits, first, slot, &busy);
	if (ret)
		return ret;

	cfg = te_read(te, cfg_reg);
	if (enable) {
		if ((cfg & L3TE_POL_CFG_TYPE) == L3TE_POL_CFG_TYPE_ON)
			return 0;
		/* Never rewrite a bank another flow is still using. */
		if (busy)
			return -EBUSY;
		te_write(te, cfg_reg,
			 (cfg & ~L3TE_POL_CFG_TYPE) | L3TE_POL_CFG_TYPE_ON);
		return 0;
	}

	if (busy || !(cfg & L3TE_POL_CFG_TYPE))
		return 0;
	te_write(te, cfg_reg, cfg & ~L3TE_POL_CFG_TYPE);
	return 0;
}

/*
 * Enable the L3TE global policer gate (NE 0x9004 bit0) together with the two
 * SoC policer control registers programmed by OEM fc_8277c.ko
 * rtk_8277c_policer_init (@0xcb780): NE 0x9180[5:0]/[13:8]=0x1f and
 * NE 0x30ac |= BIT(8)|BIT(9).  Done lazily on the first policer activation
 * rather than unconditionally at probe so boot defaults stay intact when no
 * policers are used.
 */
static int te_policer_ready(struct rtl_te_flow *te)
{
	u32 val;

	if (te->policer_ready)
		return 0;

	val = te_read(te, L3TE_TE_GLB_CTRL);
	if (!(val & L3TE_TE_GLB_POLICER_EN)) {
		regmap_update_bits(te->regs, L3TE_TE_GLB_CTRL,
				   L3TE_TE_GLB_POLICER_EN,
				   L3TE_TE_GLB_POLICER_EN);
		val = te_read(te, L3TE_TE_GLB_CTRL);
		if (!(val & L3TE_TE_GLB_POLICER_EN))
			return -EIO;
	}

	val = te_read(te, L3TE_POL_GLB_CFG);
	if ((val & L3TE_POL_GLB_CFG_MASK) != L3TE_POL_GLB_CFG_VAL)
		te_write(te, L3TE_POL_GLB_CFG,
			 (val & ~L3TE_POL_GLB_CFG_MASK) |
			 L3TE_POL_GLB_CFG_VAL);

	val = te_read(te, L3TE_FE_POL_GLB_CFG);
	if ((val & L3TE_FE_POL_GLB_EN) != L3TE_FE_POL_GLB_EN)
		te_write(te, L3TE_FE_POL_GLB_CFG, val | L3TE_FE_POL_GLB_EN);

	te->policer_ready = true;
	return 0;
}

static int te_flow_alloc_locked(struct rtl_te_flow *te, u64 cir_bps,
				u64 pir_bps, u32 cbs, u32 pbs,
				unsigned int *slot)
{
	struct rtl_te_policer_profile p;
	u32 words[3];
	unsigned int idx;
	int ret;

	ret = te_policer_encode(cir_bps, pir_bps, cbs, pbs, &p);
	if (ret)
		return ret;
	te_policer_pack(&p, words);

	/* POL1 0xA0..0xFF is reserved for host policing (rate slots at
	 * 0xA0 + vmeter, fixed MIB slots at 0xC0/0xE0 + index), so the flow
	 * policer only allocates the lower 160 slots.
	 */
	idx = find_first_zero_bit(te->flow_slots, RTL_TE_FLOW_SLOT_COUNT);
	if (idx >= RTL_TE_FLOW_SLOT_COUNT)
		return -ENOSPC;

	ret = te_policer_ready(te);
	if (ret)
		return ret;

	ret = te_pol_profile_write(te, L3TE_POL_FLOW_ACC,
				   L3TE_POL_FLOW_IDX_BITS, idx, words);
	if (ret)
		return ret;

	ret = te_bank_cfg_set(te, L3TE_POL_FLOW_CFG(idx), L3TE_POL_FLOW_ACC,
			      L3TE_POL_FLOW_IDX_BITS, idx, true);
	if (ret) {
		te_policer_sentinel(words);
		(void)te_pol_profile_write(te, L3TE_POL_FLOW_ACC,
					   L3TE_POL_FLOW_IDX_BITS, idx, words);
		return ret;
	}

	__set_bit(idx, te->flow_slots);
	*slot = idx;
	return 0;
}

static int te_flow_update_locked(struct rtl_te_flow *te, unsigned int idx,
				 u64 cir_bps, u64 pir_bps, u32 cbs, u32 pbs)
{
	struct rtl_te_policer_profile p;
	u32 words[3];
	int ret;

	if (idx >= RTL_TE_FLOW_SLOT_COUNT || !test_bit(idx, te->flow_slots))
		return -EINVAL;

	ret = te_policer_encode(cir_bps, pir_bps, cbs, pbs, &p);
	if (ret)
		return ret;
	te_policer_pack(&p, words);

	return te_pol_profile_write(te, L3TE_POL_FLOW_ACC,
				    L3TE_POL_FLOW_IDX_BITS, idx, words);
}

static int te_flow_free_locked(struct rtl_te_flow *te, unsigned int idx)
{
	u32 words[3];
	int ret;

	if (idx >= RTL_TE_FLOW_SLOT_COUNT || !test_bit(idx, te->flow_slots))
		return -EINVAL;

	te_policer_sentinel(words);
	ret = te_pol_profile_write(te, L3TE_POL_FLOW_ACC,
				   L3TE_POL_FLOW_IDX_BITS, idx, words);
	if (ret)
		return ret;

	__clear_bit(idx, te->flow_slots);
	return te_bank_cfg_set(te, L3TE_POL_FLOW_CFG(idx), L3TE_POL_FLOW_ACC,
			       L3TE_POL_FLOW_IDX_BITS, idx, false);
}

/*
 * Enable the 64 slot group table on first use.
 *
 * NE 0x9004 bit25 selects 8 or 64 group entries (ca-ne.ko
 * aal_l3_te_policer_agr_flow_profile_get @0x73820) and also changes how the
 * group cfg register is banked, so it has to be on before any group profile
 * is written.  It is a global hardware state the driver has no boot-time
 * evidence for, so rtl_te_flow_init() only records the reset value and this
 * function performs the (verified) write the first time a group policer is
 * requested.  Nothing else in the driver touches 0x9004.
 *
 * There is no undo: if the read-back does not confirm the new depth the
 * allocation fails with -EIO before any slot, profile or cfg register has
 * been written, which is the only rollback that is safe for a one-way table
 * depth setting.
 */
static int te_group_depth_ready(struct rtl_te_flow *te)
{
	u32 glb;

	if (te->group64_ready)
		return 0;

	glb = te_read(te, L3TE_TE_GLB_CTRL);
	if (!(glb & L3TE_TE_GLB_POL_GRP_64)) {
		regmap_update_bits(te->regs, L3TE_TE_GLB_CTRL,
				   L3TE_TE_GLB_POL_GRP_64,
				   L3TE_TE_GLB_POL_GRP_64);
		glb = te_read(te, L3TE_TE_GLB_CTRL);
		if (!(glb & L3TE_TE_GLB_POL_GRP_64))
			return -EIO;
	}
	te->group64_ready = true;
	return 0;
}

/*
 * Group level uses the POL2 table. Its cfg register is banked on (idx >> 5)
 * once GLB_CTRL bit25 is set, so the same 32 slot sharing rule applies.
 * The cfg "on" value is taken from policer 1 mode_set; POL2 reuses the same
 * 16 bit layout but its own value is still U3.
 */
static int te_group_alloc_locked(struct rtl_te_flow *te, u64 cir_bps,
				 u64 pir_bps, u32 cbs, u32 pbs,
				 unsigned int *slot)
{
	struct rtl_te_policer_profile p;
	u32 words[3];
	unsigned int idx;
	int ret;

	ret = te_group_depth_ready(te);
	if (ret)
		return ret;
	ret = te_policer_ready(te);
	if (ret)
		return ret;

	ret = te_policer_encode(cir_bps, pir_bps, cbs, pbs, &p);
	if (ret)
		return ret;
	te_policer_pack(&p, words);

	idx = find_first_zero_bit(te->group_slots, L3TE_POL_GRP_SLOTS);
	if (idx >= L3TE_POL_GRP_SLOTS)
		return -ENOSPC;

	ret = te_pol_profile_write(te, L3TE_POL_GRP_ACC, L3TE_POL_GRP_IDX_BITS,
				   idx, words);
	if (ret)
		return ret;

	ret = te_bank_cfg_set(te, L3TE_POL_GRP_CFG(idx), L3TE_POL_GRP_ACC,
			      L3TE_POL_GRP_IDX_BITS, idx, true);
	if (ret) {
		te_policer_sentinel(words);
		(void)te_pol_profile_write(te, L3TE_POL_GRP_ACC,
					   L3TE_POL_GRP_IDX_BITS, idx, words);
		return ret;
	}

	__set_bit(idx, te->group_slots);
	*slot = idx;
	return 0;
}

static int te_group_update_locked(struct rtl_te_flow *te, unsigned int idx,
				  u64 cir_bps, u64 pir_bps, u32 cbs, u32 pbs)
{
	struct rtl_te_policer_profile p;
	u32 words[3];
	int ret;

	if (idx >= L3TE_POL_GRP_SLOTS || !test_bit(idx, te->group_slots))
		return -EINVAL;

	ret = te_policer_encode(cir_bps, pir_bps, cbs, pbs, &p);
	if (ret)
		return ret;
	te_policer_pack(&p, words);

	return te_pol_profile_write(te, L3TE_POL_GRP_ACC, L3TE_POL_GRP_IDX_BITS,
				    idx, words);
}

static int te_group_free_locked(struct rtl_te_flow *te, unsigned int idx)
{
	u32 words[3];
	int ret;

	if (idx >= L3TE_POL_GRP_SLOTS || !test_bit(idx, te->group_slots))
		return -EINVAL;

	te_policer_sentinel(words);
	ret = te_pol_profile_write(te, L3TE_POL_GRP_ACC, L3TE_POL_GRP_IDX_BITS,
				   idx, words);
	if (ret)
		return ret;

	__clear_bit(idx, te->group_slots);
	return te_bank_cfg_set(te, L3TE_POL_GRP_CFG(idx), L3TE_POL_GRP_ACC,
			       L3TE_POL_GRP_IDX_BITS, idx, false);
}

/* section 4.3 select payload: eight 5 bit fields packed into two data words. */
static u16 te_wred_sel_field_get(const u32 words[2], unsigned int field)
{
	if (field < L3TE_WRED_SEL_SPLIT_FIELD)
		return (u16)((words[0] >> (L3TE_WRED_SEL_FIELD_BITS * field)) &
			     L3TE_WRED_SEL_FIELD_MASK);
	if (field == L3TE_WRED_SEL_SPLIT_FIELD)
		return (u16)(((words[0] >> L3TE_WRED_SEL_F6_W0_SHIFT) &
			      L3TE_WRED_SEL_F6_W0_MASK) |
			     ((words[1] & L3TE_WRED_SEL_F6_W1_MASK) <<
			      L3TE_WRED_SEL_F6_W0_BITS));
	return (u16)((words[1] >> L3TE_WRED_SEL_F7_W1_SHIFT) &
		     L3TE_WRED_SEL_FIELD_MASK);
}

static void te_wred_sel_field_set(u32 words[2], unsigned int field, u16 value)
{
	u32 shift;

	if (field < L3TE_WRED_SEL_SPLIT_FIELD) {
		shift = L3TE_WRED_SEL_FIELD_BITS * field;
		words[0] = (words[0] & ~(L3TE_WRED_SEL_FIELD_MASK << shift)) |
			   (((u32)value & L3TE_WRED_SEL_FIELD_MASK) << shift);
		return;
	}
	if (field == L3TE_WRED_SEL_SPLIT_FIELD) {
		words[0] = (words[0] & ~(L3TE_WRED_SEL_F6_W0_MASK <<
					 L3TE_WRED_SEL_F6_W0_SHIFT)) |
			   (((u32)value & L3TE_WRED_SEL_F6_W0_MASK) <<
			    L3TE_WRED_SEL_F6_W0_SHIFT);
		words[1] = (words[1] & ~L3TE_WRED_SEL_F6_W1_MASK) |
			   (((u32)value >> L3TE_WRED_SEL_F6_W0_BITS) &
			    L3TE_WRED_SEL_F6_W1_MASK);
		return;
	}
	words[1] = (words[1] & ~(L3TE_WRED_SEL_FIELD_MASK <<
				 L3TE_WRED_SEL_F7_W1_SHIFT)) |
		   (((u32)value & L3TE_WRED_SEL_FIELD_MASK) <<
		    L3TE_WRED_SEL_F7_W1_SHIFT);
}

static int te_wred_sel_update_locked(struct rtl_te_flow *te, unsigned int queue,
				     unsigned int field, u16 value)
{
	u32 words[2];
	int ret;

	ret = te_table_access(te, L3TE_WRED_SEL_ACC, L3TE_WRED_SEL_IDX_BITS,
			      false, queue, words, L3TE_WRED_SEL_WORDS);
	if (ret)
		return ret;

	te_wred_sel_field_set(words, field, value);
	return te_table_access(te, L3TE_WRED_SEL_ACC, L3TE_WRED_SEL_IDX_BITS,
			       true, queue, words, L3TE_WRED_SEL_WORDS);
}

/* ==== END test-extractable logic ==== */

/* ==== BEGIN dos logic (tests/dos-table.py) ==== */

/*
 * L3/L2 DoS programming (phase2-re9-dos-final.md).
 *
 * The block is a 288 bit image: image word w is the register at
 * L3TE_DOS_CFG(w), and bit b of that word is stream bit 32 * w + b.  The
 * vendor entry points are masked-blit writers over that image, so every
 * update below is a read-modify-write of the live registers with a store
 * barrier before each word, in ascending address order.
 *
 * Final field map (RE-9 sections 3/5/9), 16 types in CLI order = type_en bit
 * order:
 *   pol_id 9 bit, th 16 bit (icmp/fin/syn only), en 1 bit, plus dos_type_en
 *   16 bit and a 64 bit port mask.  3*(9+16+1) + 13*(9+1) + 16 + 64 = 288.
 * A field with wide == 1 continues into the next register, so it has to be
 * taken from the 64 bit (reg, reg + 4) window.
 */
struct te_dos_field {
	u16 reg;
	u8 lsb;
	u8 width;
	bool wide;
};

struct te_dos_type_desc {
	struct te_dos_field pol;
	struct te_dos_field th;
	struct te_dos_field en;
};

/* Register addresses of the two mirror blocks. */
#define TE_DOS_L3_BASE	L3TE_DOS_L3_BASE
#define TE_DOS_L2_BASE	L3TE_DOS_L2_BASE

#define TE_DOS_POL(r, lsb, wide)	{ (r), (lsb), L3TE_DOS_POL_ID_BITS, (wide) }
#define TE_DOS_TH(r, lsb, wide)		{ (r), (lsb), L3TE_DOS_TH_BITS, (wide) }
#define TE_DOS_EN(r, lsb)		{ (r), (lsb), L3TE_DOS_EN_BITS, false }
#define TE_DOS_NONE			{ 0, 0, 0, false }

static const struct te_dos_type_desc te_dos_l3_types[RTL_DOS_TYPE_MAX] = {
	/* icmp_flood    */ { TE_DOS_POL(L3TE_DOS_CFG(0),  0, false), TE_DOS_TH(L3TE_DOS_CFG(0),  9, false), TE_DOS_EN(L3TE_DOS_CFG(0), 25) },
	/* fin_flood     */ { TE_DOS_POL(L3TE_DOS_CFG(0), 26, true),  TE_DOS_TH(L3TE_DOS_CFG(1),  3, false), TE_DOS_EN(L3TE_DOS_CFG(1), 19) },
	/* syn_flood     */ { TE_DOS_POL(L3TE_DOS_CFG(1), 20, false), TE_DOS_TH(L3TE_DOS_CFG(1), 29, true),  TE_DOS_EN(L3TE_DOS_CFG(2), 13) },
	/* syn_with_data */ { TE_DOS_POL(L3TE_DOS_CFG(2), 14, false), TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(2), 23) },
	/* udp_bomb      */ { TE_DOS_POL(L3TE_DOS_CFG(2), 24, true),  TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(3),  1) },
	/* ping_of_death */ { TE_DOS_POL(L3TE_DOS_CFG(3),  2, false), TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(3), 11) },
	/* icmp_fragment */ { TE_DOS_POL(L3TE_DOS_CFG(3), 12, false), TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(3), 21) },
	/* tcp_frag_error*/ { TE_DOS_POL(L3TE_DOS_CFG(3), 22, false), TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(3), 31) },
	/* tcp_short_hdr */ { TE_DOS_POL(L3TE_DOS_CFG(4),  0, false), TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(4),  9) },
	/* syn1024       */ { TE_DOS_POL(L3TE_DOS_CFG(4), 10, false), TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(4), 19) },
	/* null_scan     */ { TE_DOS_POL(L3TE_DOS_CFG(4), 20, false), TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(4), 29) },
	/* xma_scan      */ { TE_DOS_POL(L3TE_DOS_CFG(4), 30, true),  TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(5),  7) },
	/* syn_fin_scan  */ { TE_DOS_POL(L3TE_DOS_CFG(5),  8, false), TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(5), 17) },
	/* blat_attacks  */ { TE_DOS_POL(L3TE_DOS_CFG(5), 18, false), TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(5), 27) },
	/* land_attacks  */ { TE_DOS_POL(L3TE_DOS_CFG(5), 28, true),  TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(6),  5) },
	/* smac_eq_dmac  */ { TE_DOS_POL(L3TE_DOS_CFG(6),  6, false), TE_DOS_NONE,                            TE_DOS_EN(L3TE_DOS_CFG(6), 15) },
};

/* L2 has no pol_id: 3 thresholds and 16 enables, 64 bit, phase2-re9 section 9 */
static const struct te_dos_type_desc te_dos_l2_types[RTL_DOS_TYPE_MAX] = {
	/* icmp_flood    */ { TE_DOS_NONE, TE_DOS_TH(L3TE_DOS_L2_CFG(0),  0, false), TE_DOS_EN(L3TE_DOS_L2_CFG(0), 16) },
	/* fin_flood     */ { TE_DOS_NONE, TE_DOS_TH(L3TE_DOS_L2_CFG(0), 17, true),  TE_DOS_EN(L3TE_DOS_L2_CFG(1),  1) },
	/* syn_flood     */ { TE_DOS_NONE, TE_DOS_TH(L3TE_DOS_L2_CFG(1),  2, false), TE_DOS_EN(L3TE_DOS_L2_CFG(1), 18) },
	/* syn_with_data */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 19) },
	/* udp_bomb      */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 20) },
	/* ping_of_death */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 21) },
	/* icmp_fragment */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 22) },
	/* tcp_frag_error*/ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 23) },
	/* tcp_short_hdr */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 24) },
	/* syn1024       */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 25) },
	/* null_scan     */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 26) },
	/* xma_scan      */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 27) },
	/* syn_fin_scan  */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 28) },
	/* blat_attacks  */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 29) },
	/* land_attacks  */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 30) },
	/* smac_eq_dmac  */ { TE_DOS_NONE, TE_DOS_NONE,                              TE_DOS_EN(L3TE_DOS_L2_CFG(1), 31) },
};

/* Names observed in the OEM CLI; the index is the type_en bit number. */
static const char *const te_dos_type_name[RTL_DOS_TYPE_MAX] = {
	[RTL_DOS_TYPE_ICMP_FLOOD] = "icmp_flood",
	[RTL_DOS_TYPE_FIN_FLOOD] = "fin_flood",
	[RTL_DOS_TYPE_SYN_FLOOD] = "syn_flood",
	[RTL_DOS_TYPE_SYN_WITH_DATA] = "syn_with_data",
	[RTL_DOS_TYPE_UDP_BOMB] = "udp_bomb",
	[RTL_DOS_TYPE_PING_OF_DEATH] = "ping_of_death",
	[RTL_DOS_TYPE_ICMP_FRAGMENT] = "icmp_fragment",
	[RTL_DOS_TYPE_TCP_FRAG_ERROR] = "tcp_frag_error",
	[RTL_DOS_TYPE_TCP_SHORT_HDR] = "tcp_short_hdr",
	[RTL_DOS_TYPE_SYN1024] = "syn1024",
	[RTL_DOS_TYPE_NULL_SCAN] = "null_scan",
	[RTL_DOS_TYPE_XMA_SCAN] = "xma_scan",
	[RTL_DOS_TYPE_SYN_FIN_SCAN] = "syn_fin_scan",
	[RTL_DOS_TYPE_BLAT_ATTACKS] = "blat_attacks",
	[RTL_DOS_TYPE_LAND_ATTACKS] = "land_attacks",
	[RTL_DOS_TYPE_SMAC_EQ_DMAC] = "smac_eq_dmac",
};

static __maybe_unused int te_dos_type_index(const char *name)
{
	unsigned int i;

	for (i = 0; i < RTL_DOS_TYPE_MAX; i++)
		if (!strcmp(name, te_dos_type_name[i]))
			return (int)i;
	return -EINVAL;
}

/* Read one field out of a register image snapshot. */
static __maybe_unused u32 te_dos_field_extract(const u32 *img,
					       unsigned int base,
					       const struct te_dos_field *f)
{
	unsigned int i = (f->reg - base) / 4u;
	u64 window;

	if (!f->width)
		return 0;
	if (!f->wide)
		return (img[i] >> f->lsb) &
		       ((1u << f->width) - 1u);

	window = (u64)img[i] | ((u64)img[i + 1] << 32);
	return (u32)((window >> f->lsb) & ((1ull << f->width) - 1ull));
}

/*
 * Copy one field into a two word window: the caller supplies the live value
 * of (reg, reg + 4) as a little endian u64 and gets the updated value back,
 * every other bit untouched.  Pure so the tests can drive it directly.
 */
static void te_dos_field_apply(u64 *window, const struct te_dos_field *f,
			       u32 value)
{
	u64 mask;

	if (!f->width)
		return;
	mask = ((1ull << f->width) - 1ull) << f->lsb;
	*window = (*window & ~mask) | (((u64)value << f->lsb) & mask);
}

/* Read-modify-write one field in the live registers. */
static int te_dos_field_write(struct rtl_te_flow *te,
			      const struct te_dos_field *f, u32 value)
{
	u64 window;

	if (!f->width)
		return -EOPNOTSUPP;
	if (value > ((1u << f->width) - 1u))
		return -EINVAL;

	window = te_read(te, f->reg);
	if (f->wide)
		window |= (u64)te_read(te, f->reg + 4u) << 32;
	te_dos_field_apply(&window, f, value);
	wmb(); /* order register writes in ascending address order */
	te_write(te, f->reg, (u32)window);
	if (f->wide) {
		wmb(); /* order low word write before high word write */
		te_write(te, f->reg + 4u, (u32)(window >> 32));
	}
	return 0;
}

/*
 * Enable or disable one attack type and select its hardware action.
 *
 * Verified against ca-rtk.ko dal_rtl8277c_sec_attackPrevent_get/set
 * (@0xc538/@0xc7a8):
 *   - dos_type_en[type] (L3 0x3304[16+type], L2 0x1228[type]):
 *       0 = disabled (forward), 1 = enabled.
 *   - Per-type 1-bit field (desc[type].en):
 *       0 = ACTION_DROP (zero-CPU hardware drop; L3 -> ldpid 31),
 *       1 = ACTION_TRAP2CPU (trap to CPU0, ldpid 16).
 */
static int te_dos_type_set_action(struct rtl_te_flow *te, bool l2,
				  unsigned int type, bool enable, bool trap)
{
	const struct te_dos_type_desc *desc = l2 ? te_dos_l2_types :
						    te_dos_l3_types;
	u32 reg = l2 ? L3TE_DOS_L2_TYPE_EN : L3TE_DOS_TYPE_EN;
	u32 shift = l2 ? L3TE_DOS_L2_TYPE_EN_SHIFT :
			  L3TE_DOS_TYPE_EN_SHIFT;
	u32 value;
	int ret;

	if (type >= RTL_DOS_TYPE_MAX || !desc[type].en.width)
		return -EOPNOTSUPP;

	ret = te_dos_field_write(te, &desc[type].en, (enable && trap) ? 1u : 0u);
	if (ret)
		return ret;

	value = te_read(te, reg);
	if (enable)
		value |= BIT(type) << shift;
	else
		value &= ~(BIT(type) << shift);
	wmb(); /* flush prior field write before updating type_en */
	te_write(te, reg, value);
	return 0;
}

static __maybe_unused int te_dos_type_enable(struct rtl_te_flow *te, bool l2,
					     unsigned int type, bool enable)
{
	return te_dos_type_set_action(te, l2, type, enable, false);
}

/* dos_type_en accessor: L3 0x3304[31:16], L2 0x1228[15:0]. */
static u32 te_dos_type_en_read(struct rtl_te_flow *te, bool l2)
{
	u32 shift = l2 ? L3TE_DOS_L2_TYPE_EN_SHIFT : L3TE_DOS_TYPE_EN_SHIFT;

	return (te_read(te, l2 ? L3TE_DOS_L2_TYPE_EN : L3TE_DOS_TYPE_EN) >>
		shift) & (u32)L3TE_DOS_TYPE_ALL;
}

static void te_dos_type_en_write(struct rtl_te_flow *te, bool l2, u32 bitmap)
{
	u32 reg = l2 ? L3TE_DOS_L2_TYPE_EN : L3TE_DOS_TYPE_EN;
	u32 shift = l2 ? L3TE_DOS_L2_TYPE_EN_SHIFT : L3TE_DOS_TYPE_EN_SHIFT;
	u32 value = te_read(te, reg);

	value = (value & ~((u32)L3TE_DOS_TYPE_ALL << shift)) |
		((bitmap & (u32)L3TE_DOS_TYPE_ALL) << shift);
	wmb(); /* order read-modify-write of type_en register */
	te_write(te, reg, value);
}

/*
 * Bit slice maps of the two DoS configuration blocks.
 *
 * Entry i is mask bit i of phase2-re5-dos-attack.md section 2.1 (L3, 37
 * entries) and section 7.1 (L2, 21 entries).  "off" is the offset of the
 * slice inside the image bit stream.  Both tables tile their stream exactly
 * (0..287 and 0..143).
 */
struct te_dos_slice {
	u16 off;
	u16 width;
};

#define TE_DOS_L3_MASK_ALL	GENMASK_ULL(36, 0)
#define TE_DOS_L2_MASK_ALL	GENMASK_ULL(20, 0)

static const struct te_dos_slice te_dos_l3_slice[] = {
	{   0,  9 }, {   9, 16 }, {  25,  1 }, {  26,  9 }, {  35, 16 },
	{  51,  1 }, {  52,  9 }, {  61, 16 }, {  77,  1 }, {  78,  9 },
	{  87,  1 }, {  88,  9 }, {  97,  1 }, {  98,  9 }, { 107,  1 },
	{ 108,  9 }, { 117,  1 }, { 118,  9 }, { 127,  1 }, { 128,  9 },
	{ 137,  1 }, { 138,  9 }, { 147,  1 }, { 148,  9 }, { 157,  1 },
	{ 158,  9 }, { 167,  1 }, { 168,  9 }, { 177,  1 }, { 178,  9 },
	{ 187,  1 }, { 188,  9 }, { 197,  1 }, { 198,  9 }, { 207,  1 },
	{ 208, 16 }, { 224, 64 },
};

static const struct te_dos_slice te_dos_l2_slice[] = {
	{   0, 16 }, {  16,  1 }, {  17, 16 }, {  33,  1 }, {  34, 16 },
	{  50,  1 }, {  51,  1 }, {  52,  1 }, {  53,  1 }, {  54,  1 },
	{  55,  1 }, {  56,  1 }, {  57,  1 }, {  58,  1 }, {  59,  1 },
	{  60,  1 }, {  61,  1 }, {  62,  1 }, {  63,  1 },
	{  64, 16 }, {  80, 64 },
};

static void te_dos_img_copy(u32 *dst, const u32 *src, unsigned int off,
			    unsigned int width)
{
	unsigned int bit;

	for (bit = 0; bit < width; bit++) {
		unsigned int pos = off + bit;
		u32 mask = 1u << (pos % 32);

		if (src[pos / 32] & mask)
			dst[pos / 32] |= mask;
		else
			dst[pos / 32] &= ~mask;
	}
}

static void te_dos_blit(u32 *img, const struct te_dos_slice *tab,
			unsigned int count, u64 mask, const u32 *src)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (mask & BIT_ULL(i))
			te_dos_img_copy(img, src, tab[i].off, tab[i].width);
}

static void te_dos_read(struct rtl_te_flow *te, bool l2, u32 *img,
			unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		img[i] = te_read(te, l2 ? L3TE_DOS_L2_CFG(i) : L3TE_DOS_CFG(i));
}

static void te_dos_write_words(struct rtl_te_flow *te, bool l2, const u32 *img,
			       unsigned int first, unsigned int last)
{
	unsigned int i;

	for (i = first; i <= last; i++) {
		wmb();		/* vendor: dmb oshst before every word */
		te_write(te, l2 ? L3TE_DOS_L2_CFG(i) : L3TE_DOS_CFG(i), img[i]);
	}
}

static void te_dos_write(struct rtl_te_flow *te, bool l2, const u32 *img,
			 unsigned int count)
{
	if (count)
		te_dos_write_words(te, l2, img, 0, count - 1);
}

static u32 te_dos_flood_read(struct rtl_te_flow *te, bool l2)
{
	return te_read(te, l2 ? L3TE_DOS_L2_FLOOD_CNT : L3TE_DOS_FLOOD_CNT);
}

/*
 * L3 0x3310[10:0] (default 0x3fa) and L2 0x1234[9:0] (default 0xff) configure
 * the flood detection time-window unit (dal_rtl8277c_sec_attackFloodThreshUnit_set
 * @0xd03c). Preserve reserved upper bits on write.
 */
static int te_dos_flood_unit_write(struct rtl_te_flow *te, bool l2, u32 unit)
{
	u32 reg = l2 ? L3TE_DOS_L2_FLOOD_CNT : L3TE_DOS_FLOOD_CNT;
	u32 mask = l2 ? (u32)L3TE_DOS_L2_FLOOD_CNT_MASK :
			(u32)L3TE_DOS_FLOOD_CNT_MASK;
	u32 value;

	if (unit & ~mask)
		return -EINVAL;

	value = te_read(te, reg);
	wmb(); /* order read-modify-write of flood window unit */
	te_write(te, reg, (value & ~mask) | unit);
	return 0;
}

static void te_dos_flood_clear_reg(struct rtl_te_flow *te, bool l2)
{
	(void)te_dos_flood_unit_write(te, l2, 0);
}

static u32 te_dos_evt_bits(u32 field_mask, const struct rtl_te_dos_evt *evt)
{
	u32 bits = 0;
	unsigned int i;

	if (field_mask & L3TE_DOS_EVT_F_LDPID)
		bits |= ((u32)evt->ldpid & L3TE_DOS_EVT_LDPID_MAX) <<
			L3TE_DOS_EVT_LDPID_SHIFT;
	if (field_mask & L3TE_DOS_EVT_F_COS)
		bits |= ((u32)evt->cos & L3TE_DOS_EVT_COS_MAX) <<
			L3TE_DOS_EVT_COS_SHIFT;
	if (field_mask & L3TE_DOS_EVT_F_KEEP_ORIG)
		bits |= (u32)(!!evt->keep_orig) << L3TE_DOS_EVT_KEEP_ORIG_SHIFT;
	if (field_mask & L3TE_DOS_EVT_F_T0)
		bits |= (u32)(!!evt->t0) << L3TE_DOS_EVT_T0_SHIFT;
	for (i = 0; i < L3TE_DOS_EVT_TIMERS; i++)
		if (field_mask & L3TE_DOS_EVT_F_TIMER(i))
			bits |= ((u32)evt->timer[i] & L3TE_DOS_EVT_TIMER_MAX) <<
				(L3TE_DOS_EVT_TIMER_BASE +
				 L3TE_DOS_EVT_TIMER_BITS * i);
	return bits;
}

static u32 te_dos_evt_mask(u32 field_mask)
{
	u32 mask = 0;
	unsigned int i;

	if (field_mask & L3TE_DOS_EVT_F_LDPID)
		mask |= (u32)L3TE_DOS_EVT_LDPID;
	if (field_mask & L3TE_DOS_EVT_F_COS)
		mask |= (u32)L3TE_DOS_EVT_COS;
	if (field_mask & L3TE_DOS_EVT_F_KEEP_ORIG)
		mask |= (u32)L3TE_DOS_EVT_KEEP_ORIG;
	if (field_mask & L3TE_DOS_EVT_F_T0)
		mask |= (u32)L3TE_DOS_EVT_T0;
	for (i = 0; i < L3TE_DOS_EVT_TIMERS; i++)
		if (field_mask & L3TE_DOS_EVT_F_TIMER(i))
			mask |= (u32)L3TE_DOS_EVT_TIMER(i);
	return mask;
}

static __maybe_unused void te_dos_evt_unpack(u32 raw,
					     struct rtl_te_dos_evt *evt)
{
	unsigned int i;

	evt->ldpid = (u8)((raw >> L3TE_DOS_EVT_LDPID_SHIFT) &
			  L3TE_DOS_EVT_LDPID_MAX);
	evt->cos = (u8)((raw >> L3TE_DOS_EVT_COS_SHIFT) & L3TE_DOS_EVT_COS_MAX);
	evt->keep_orig = !!(raw & L3TE_DOS_EVT_KEEP_ORIG);
	evt->reserved = !!(raw & L3TE_DOS_EVT_RESERVED);
	evt->t0 = !!(raw & L3TE_DOS_EVT_T0);
	for (i = 0; i < L3TE_DOS_EVT_TIMERS; i++)
		evt->timer[i] = (u8)((raw >> (L3TE_DOS_EVT_TIMER_BASE +
					      L3TE_DOS_EVT_TIMER_BITS * i)) &
				     L3TE_DOS_EVT_TIMER_MAX);
}

/* ==== END dos logic ==== */

/* ==== BEGIN hostpol logic (tests/hostpol.py) ==== */

/*
 * Host policing, fc_8277c model (phase2-re10-hostpol-final.md section 4).
 *
 * 32 entries keyed by MAC.  Each entry owns up to two virtual meters out of
 * a pool of 32; a rate limited direction maps onto POL1 slot 0xA0 + vmeter
 * (160..191), a direction that only counts uses the fixed per-entry MIB slot
 * 0xC0 + idx (ingress, 192..223) or 0xE0 + idx (egress, 224..255) and takes
 * no vmeter.  POL1 0xA0..0xFF is reserved for this feature, the flow
 * policer allocates only 0..0x9F.
 */

static bool te_hostpol_mac_eq(const u8 a[RTL_TE_HOSTPOL_ALEN],
			      const u8 b[RTL_TE_HOSTPOL_ALEN])
{
	return !memcmp(a, b, RTL_TE_HOSTPOL_ALEN);
}

static int te_hostpol_find(struct rtl_te_flow *te,
			   const u8 mac[RTL_TE_HOSTPOL_ALEN])
{
	unsigned int i;

	for (i = 0; i < RTL_TE_HOSTPOL_ENTRIES; i++)
		if (te->hostpol[i].used &&
		    te_hostpol_mac_eq(te->hostpol[i].mac, mac))
			return (int)i;
	return -ENOENT;
}

static int te_hostpol_find_free(struct rtl_te_flow *te)
{
	unsigned int i;

	for (i = 0; i < RTL_TE_HOSTPOL_ENTRIES; i++)
		if (!te->hostpol[i].used)
			return (int)i;
	return -ENOSPC;
}

static unsigned int te_hostpol_used(struct rtl_te_flow *te)
{
	unsigned int i, used = 0;

	for (i = 0; i < RTL_TE_HOSTPOL_ENTRIES; i++)
		used += te->hostpol[i].used ? 1u : 0u;
	return used;
}

/* Is vmeter `vm` owned by an entry other than `skip`? */
static bool te_hostpol_vmeter_used(struct rtl_te_flow *te, unsigned int vm,
				   int skip)
{
	unsigned int i, d;

	for (i = 0; i < RTL_TE_HOSTPOL_ENTRIES; i++) {
		if (!te->hostpol[i].used || (int)i == skip)
			continue;
		for (d = 0; d < RTL_TE_HOSTPOL_DIRS; d++)
			if (te->hostpol[i].vmeter[d] == vm)
				return true;
	}
	return false;
}

static __maybe_unused unsigned int te_hostpol_vmeters_used(struct rtl_te_flow *te)
{
	unsigned int vm, used = 0;

	for (vm = 0; vm < RTL_TE_HOSTPOL_VMETERS; vm++)
		if (te_hostpol_vmeter_used(te, vm, -1))
			used++;
	return used;
}

/* First vmeter that no other entry owns and that is not `avoid`. */
static int te_hostpol_vmeter_alloc(struct rtl_te_flow *te, int skip, int avoid)
{
	unsigned int vm;

	for (vm = 0; vm < RTL_TE_HOSTPOL_VMETERS; vm++) {
		if ((int)vm == avoid)
			continue;
		if (!te_hostpol_vmeter_used(te, vm, skip))
			return (int)vm;
	}
	return -ENOSPC;
}

/* Resolve the two POL1 slots of an entry from its rates and vmeters. */
static void te_hostpol_resolve(struct rtl_te_hostpol *h, unsigned int entry)
{
	unsigned int d;

	for (d = 0; d < RTL_TE_HOSTPOL_DIRS; d++) {
		if (h->kbps[d])
			h->pol1[d] = (u16)(RTL_TE_HOSTPOL_RATE_BASE +
					   h->vmeter[d]);
		else
			h->pol1[d] = (u16)((d == RTL_TE_HOSTPOL_DIR_INGRESS ?
					    RTL_TE_HOSTPOL_MIB_IN_BASE :
					    RTL_TE_HOSTPOL_MIB_EG_BASE) +
					   entry);
	}
}

/*
 * Program one direction: read the live profile, replace only CIR/PIR and
 * write it back (the vendor rate path uses profile flags 0x1B, CIR/PIR only,
 * so CBS/PBS are never touched).  A rate of 0 programs the highest encodable
 * rate, which is the numeric value the vendor treats as "no limit" while
 * keeping the slot counting.
 */
static int te_hostpol_program_rate(struct rtl_te_flow *te, unsigned int slot,
				   u32 kbps)
{
	struct rtl_te_policer_profile p;
	u32 words[3];
	u16 hi, lo;
	int ret;

	if (!kbps || kbps >= RTL_TE_HOSTPOL_RATE_MAX)
		kbps = RTL_TE_HOSTPOL_RATE_MAX;

	ret = te_policer_encode_rate((u64)kbps * 1000u, &hi, &lo);
	if (ret)
		return ret;

	ret = te_pol_profile_read(te, L3TE_POL_FLOW_ACC, L3TE_POL_FLOW_IDX_BITS,
				  slot, words);
	if (ret)
		return ret;
	te_policer_unpack(words, &p);
	p.cir_hi = hi;
	p.cir_lo = lo;
	p.pir_hi = hi;
	p.pir_lo = lo;		/* CIR = PIR (_rtk_fc_g3L3Policer_set) */
	te_policer_pack(&p, words);
	return te_pol_profile_write(te, L3TE_POL_FLOW_ACC,
				    L3TE_POL_FLOW_IDX_BITS, slot, words);
}

static void te_hostpol_sentinel(struct rtl_te_flow *te, unsigned int slot)
{
	u32 words[3];

	te_policer_sentinel(words);
	(void)te_pol_profile_write(te, L3TE_POL_FLOW_ACC,
				   L3TE_POL_FLOW_IDX_BITS, slot, words);
}

/*
 * Pack the nine ascending MIB data words (DATA0 at 0x9740) into the eight
 * counters of aal_l3_te_pm_policer_flow_get().
 */
static void te_hostpol_mib_parse(const u32 data[L3TE_POL_MIB_WORDS],
				 u64 out[RTL_TE_HOSTPOL_MIB_WORDS])
{
	out[0] = data[8];
	out[1] = data[7];
	out[2] = data[6];
	out[3] = data[5];
	out[4] = ((u64)(data[3] & L3TE_POL_MIB_C4_HI_MASK) <<
		  L3TE_POL_MIB_C4_HI_SHIFT) | data[4];
	out[5] = ((u64)(data[2] & L3TE_POL_MIB_C5_HI_MASK) <<
		  L3TE_POL_MIB_C5_HI_SHIFT) |
		 (data[3] >> L3TE_POL_MIB_C4_LO_SHIFT);
	out[6] = ((u64)(data[1] & L3TE_POL_MIB_C6_HI_MASK) <<
		  L3TE_POL_MIB_C6_HI_SHIFT) |
		 (data[2] >> L3TE_POL_MIB_C5_LO_SHIFT);
	out[7] = ((u64)(data[0] & L3TE_POL_MIB_C7_HI_MASK) <<
		  L3TE_POL_MIB_C7_HI_SHIFT) |
		 (data[1] >> L3TE_POL_MIB_C6_LO_SHIFT);
}

/*
 * TE PM resets with L3TE_PM_CTRL (0x9700) mem_disable=1 (0x00001ca0).
 * Lazily match ca-ne.ko aal_l3_te_init() (@0x76f18) on first host policing
 * configuration or MIB read without altering default boot register state.
 */
static void te_hostpol_pm_ready(struct rtl_te_flow *te)
{
	u32 val, want;

	if (te->pm_ready)
		return;

	val = te_read(te, L3TE_PM_CTRL);
	want = (val & ~((u32)(L3TE_PM_CTRL_DISABLE |
			       L3TE_PM_CTRL_CNT0_EVT_SEL |
			       L3TE_PM_CTRL_CNT0_ID_SEL))) |
	       (u32)(L3TE_PM_CTRL_AUTO_CLR |
		     L3TE_PM_CTRL_CNT1_EVT_SEL |
		     L3TE_PM_CTRL_CNT1_ID_SEL);
	if (want != val)
		te_write(te, L3TE_PM_CTRL, want);
	te->pm_ready = true;
}

static int te_hostpol_mib_read(struct rtl_te_flow *te, unsigned int idx,
			       u32 data[L3TE_POL_MIB_WORDS])
{
	unsigned int i;
	int ret;

	te_hostpol_pm_ready(te);

	ret = te_wait_idle(te, L3TE_POL_MIB_ACC);
	if (ret)
		return ret;

	wmb(); /* order prior accesses before triggering MIB read */
	te_write(te, L3TE_POL_MIB_ACC,
		 L3TE_IND_BUSY |
		 (idx & L3TE_IND_IDX_MASK(L3TE_POL_MIB_IDX_BITS)));
	ret = te_wait_idle(te, L3TE_POL_MIB_ACC);
	if (ret)
		return ret;

	rmb(); /* order MIB read completion before reading data words */
	for (i = 0; i < L3TE_POL_MIB_WORDS; i++)
		data[i] = te_read(te, L3TE_POL_MIB_DATA(i));
	return 0;
}

/*
 * Bodies of the public API; the caller holds ->lock, which also serialises
 * the shared vmeter pool and the indirect profile window.
 */
static int te_hostpol_add_locked(struct rtl_te_flow *te,
				 const u8 mac[RTL_TE_HOSTPOL_ALEN],
				 u32 ingress_kbps, u32 egress_kbps,
				 u8 ingress_vmeter, u8 egress_vmeter)
{
	struct rtl_te_hostpol *h;
	u32 kbps[RTL_TE_HOSTPOL_DIRS] = { ingress_kbps, egress_kbps };
	u8 want[RTL_TE_HOSTPOL_DIRS] = { ingress_vmeter, egress_vmeter };
	unsigned int d;
	int entry, ret, vm;

	if (ingress_kbps > RTL_TE_HOSTPOL_RATE_MAX ||
	    egress_kbps > RTL_TE_HOSTPOL_RATE_MAX)
		return -EINVAL;

	entry = te_hostpol_find(te, mac);
	if (entry >= 0)
		return -EEXIST;		/* duplicate MAC is refused */

	entry = te_hostpol_find_free(te);
	if (entry < 0)
		return -ENOSPC;

	h = &te->hostpol[entry];
	memset(h, 0, sizeof(*h));

	for (d = 0; d < RTL_TE_HOSTPOL_DIRS; d++) {
		if (!kbps[d]) {
			/* counting only: fixed MIB slot, no vmeter */
			h->vmeter[d] = RTL_TE_HOSTPOL_VMETER_NONE;
			continue;
		}
		if (want[d] != RTL_TE_HOSTPOL_VMETER_AUTO) {
			if (want[d] >= RTL_TE_HOSTPOL_VMETERS) {
				ret = -EINVAL;
				goto fail;
			}
			if (te_hostpol_vmeter_used(te, want[d], entry)) {
				ret = -EBUSY;
				goto fail;
			}
			h->vmeter[d] = want[d];
			continue;
		}
		/* skip the vmeter already picked for the other direction */
		vm = te_hostpol_vmeter_alloc(te, entry,
					     d ? (int)h->vmeter[0] : -1);
		if (vm < 0) {
			ret = -ENOSPC;
			goto fail;
		}
		h->vmeter[d] = (u8)vm;
	}

	for (d = 0; d < RTL_TE_HOSTPOL_DIRS; d++)
		h->kbps[d] = kbps[d];
	te_hostpol_resolve(h, entry);
	ret = te_policer_ready(te);
	if (ret)
		goto fail;
	te_hostpol_pm_ready(te);

	for (d = 0; d < RTL_TE_HOSTPOL_DIRS; d++) {
		ret = te_hostpol_program_rate(te, h->pol1[d], h->kbps[d]);
		if (ret)
			goto fail_prog;
		ret = te_bank_cfg_set(te, L3TE_POL_FLOW_CFG(h->pol1[d]),
				      L3TE_POL_FLOW_ACC,
				      L3TE_POL_FLOW_IDX_BITS, h->pol1[d], true);
		if (ret)
			goto fail_prog;
	}

	h->used = true;
	memcpy(h->mac, mac, RTL_TE_HOSTPOL_ALEN);
	return 0;

fail_prog:
	te_hostpol_sentinel(te, h->pol1[0]);
	te_hostpol_sentinel(te, h->pol1[1]);
	(void)te_bank_cfg_set(te, L3TE_POL_FLOW_CFG(h->pol1[0]),
			      L3TE_POL_FLOW_ACC, L3TE_POL_FLOW_IDX_BITS,
			      h->pol1[0], false);
	(void)te_bank_cfg_set(te, L3TE_POL_FLOW_CFG(h->pol1[1]),
			      L3TE_POL_FLOW_ACC, L3TE_POL_FLOW_IDX_BITS,
			      h->pol1[1], false);
fail:
	memset(h, 0, sizeof(*h));
	return ret;
}

static int te_hostpol_del_locked(struct rtl_te_flow *te,
				 const u8 mac[RTL_TE_HOSTPOL_ALEN])
{
	struct rtl_te_hostpol *h;
	unsigned int d;
	int entry;

	entry = te_hostpol_find(te, mac);
	if (entry < 0)
		return -ENOENT;

	h = &te->hostpol[entry];

	/* Release best effort: both slots go back to the idle sentinel and the
	 * vmeters are freed by simply dropping the entry.
	 */
	for (d = 0; d < RTL_TE_HOSTPOL_DIRS; d++) {
		te_hostpol_sentinel(te, h->pol1[d]);
		(void)te_bank_cfg_set(te, L3TE_POL_FLOW_CFG(h->pol1[d]),
				      L3TE_POL_FLOW_ACC,
				      L3TE_POL_FLOW_IDX_BITS, h->pol1[d],
				      false);
	}
	memset(h, 0, sizeof(*h));
	return 0;
}

/*
 * Resolve the POL1 slot a flow for this MAC must point at.
 *
 * INGRESS is the G3 flow decision for the entry as the SMAC host: an ingress
 * rate limit wins over an egress rate limit, and a counting-only entry binds
 * the egress MIB slot (phase2-re10 section 4.10).  EGRESS is the DA side.
 */
static int te_hostpol_lookup_locked(struct rtl_te_flow *te,
				    const u8 mac[RTL_TE_HOSTPOL_ALEN],
				    u8 dir, u16 *slot, bool *rate_limited)
{
	const struct rtl_te_hostpol *h;
	int entry;

	if (dir >= RTL_TE_HOSTPOL_DIRS)
		return -EINVAL;

	entry = te_hostpol_find(te, mac);
	if (entry < 0)
		return -ENOENT;
	h = &te->hostpol[entry];

	if (dir == RTL_TE_HOSTPOL_DIR_INGRESS) {
		if (h->kbps[RTL_TE_HOSTPOL_DIR_INGRESS]) {
			*slot = h->pol1[RTL_TE_HOSTPOL_DIR_INGRESS];
			*rate_limited = true;
			return 0;
		}
		if (h->kbps[RTL_TE_HOSTPOL_DIR_EGRESS]) {
			*slot = h->pol1[RTL_TE_HOSTPOL_DIR_EGRESS];
			*rate_limited = true;
			return 0;
		}
		*slot = (u16)(RTL_TE_HOSTPOL_MIB_EG_BASE + entry);
		*rate_limited = false;
		return 0;
	}

	*slot = h->pol1[RTL_TE_HOSTPOL_DIR_EGRESS];
	*rate_limited = h->kbps[RTL_TE_HOSTPOL_DIR_EGRESS] != 0;
	return 0;
}

static int te_hostpol_mib_raw_locked(struct rtl_te_flow *te, u8 entry, u8 dir,
				     struct rtl_te_hostpol_mib *mib)
{
	const struct rtl_te_hostpol *h;
	u32 data[L3TE_POL_MIB_WORDS];
	int ret;

	if (entry >= RTL_TE_HOSTPOL_ENTRIES || dir >= RTL_TE_HOSTPOL_DIRS)
		return -EINVAL;
	h = &te->hostpol[entry];
	if (!h->used)
		return -ENOENT;

	ret = te_hostpol_mib_read(te, h->pol1[dir], data);
	if (ret)
		return ret;
	te_hostpol_mib_parse(data, mib->cnt);
	return 0;
}

static int te_hostpol_mib_sample_locked(struct rtl_te_flow *te, u8 entry,
					u64 *rx, u64 *tx,
					struct rtl_te_hostpol_mib *raw)
{
	struct rtl_te_hostpol *h;
	struct rtl_te_hostpol_mib mib;
	unsigned int d;
	u64 delta;
	int ret;

	if (entry >= RTL_TE_HOSTPOL_ENTRIES)
		return -EINVAL;
	h = &te->hostpol[entry];
	if (!h->used)
		return -ENOENT;

	for (d = 0; d < RTL_TE_HOSTPOL_DIRS; d++) {
		ret = te_hostpol_mib_raw_locked(te, entry, (u8)d, &mib);
		if (ret)
			return ret;
		/*
		 * cnt[4] = total_bytes, cnt[5] = red_bytes (36-bit counters
		 * from aal_l3_te_pm_policer_flow_get @0x753b0); subtract them
		 * to accumulate forwarded bytes like _rtk_fc_hwHostPolingMib_get
		 * (@0x4cdd0).  Guard against inverted samples rather than
		 * wrapping a u64 accumulator.
		 */
		delta = mib.cnt[4] > mib.cnt[5] ? mib.cnt[4] - mib.cnt[5] : 0;
		h->last_delta[d] = delta;
		if (d == RTL_TE_HOSTPOL_DIR_INGRESS)
			h->rx += delta;
		else
			h->tx += delta;
		if (raw)
			raw[d] = mib;
	}
	*rx = h->rx;
	*tx = h->tx;
	return 0;
}

static int te_hostpol_mib_get_locked(struct rtl_te_flow *te, u8 entry,
				     u64 *rx, u64 *tx)
{
	return te_hostpol_mib_sample_locked(te, entry, rx, tx, NULL);
}

static int te_hostpol_mib_clear_locked(struct rtl_te_flow *te, u8 entry)
{
	u64 rx, tx;
	int ret;

	/* The vendor clear only reads the hardware once (to drain it into the
	 * software accumulation) and then zeroes that accumulation; there is
	 * no hardware counter-clear register in this path.
	 */
	ret = te_hostpol_mib_get_locked(te, entry, &rx, &tx);
	if (ret)
		return ret;
	te->hostpol[entry].rx = 0;
	te->hostpol[entry].tx = 0;
	return 0;
}

/* ==== END hostpol logic ==== */

int rtl_te_policer_alloc(struct rtl_te_flow *te, u64 cir_bps, u64 pir_bps,
			 u32 cbs_bytes, u32 pbs_bytes, u8 *slot)
{
	unsigned long flags;
	unsigned int idx;
	int ret;

	if (!te || !slot)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_flow_alloc_locked(te, cir_bps, pir_bps, cbs_bytes, pbs_bytes,
				   &idx);
	spin_unlock_irqrestore(&te->lock, flags);
	if (ret)
		return ret;

	*slot = idx;
	return 0;
}

int rtl_te_policer_update(struct rtl_te_flow *te, u8 slot, u64 cir_bps,
			  u64 pir_bps, u32 cbs_bytes, u32 pbs_bytes)
{
	unsigned long flags;
	int ret;

	if (!te)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_flow_update_locked(te, slot, cir_bps, pir_bps, cbs_bytes,
				    pbs_bytes);
	spin_unlock_irqrestore(&te->lock, flags);
	return ret;
}

int rtl_te_policer_free(struct rtl_te_flow *te, u8 slot)
{
	unsigned long flags;
	int ret;

	if (!te)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_flow_free_locked(te, slot);
	spin_unlock_irqrestore(&te->lock, flags);
	return ret;
}

int rtl_te_policer_group_alloc(struct rtl_te_flow *te, u64 cir_bps,
			       u64 pir_bps, u32 cbs_bytes, u32 pbs_bytes,
			       u8 *slot)
{
	unsigned long flags;
	unsigned int idx;
	int ret;

	if (!te || !slot)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_group_alloc_locked(te, cir_bps, pir_bps, cbs_bytes, pbs_bytes,
				    &idx);
	spin_unlock_irqrestore(&te->lock, flags);
	if (ret)
		return ret;

	*slot = idx;
	return 0;
}

int rtl_te_policer_group_update(struct rtl_te_flow *te, u8 slot, u64 cir_bps,
				u64 pir_bps, u32 cbs_bytes, u32 pbs_bytes)
{
	unsigned long flags;
	int ret;

	if (!te)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_group_update_locked(te, slot, cir_bps, pir_bps, cbs_bytes,
				     pbs_bytes);
	spin_unlock_irqrestore(&te->lock, flags);
	return ret;
}

int rtl_te_policer_group_free(struct rtl_te_flow *te, u8 slot)
{
	unsigned long flags;
	int ret;

	if (!te)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_group_free_locked(te, slot);
	spin_unlock_irqrestore(&te->lock, flags);
	return ret;
}

int rtl_te_wred_profile_write(struct rtl_te_flow *te, u8 profile,
			      const u32 words[8])
{
	unsigned long flags;
	u32 tmp[L3TE_WRED_PROF_WORDS];
	int ret;

	if (!te || !words || profile >= L3TE_WRED_PROFILES)
		return -EINVAL;

	memcpy(tmp, words, sizeof(tmp));
	spin_lock_irqsave(&te->lock, flags);
	ret = te_table_access(te, L3TE_WRED_PROF_ACC, L3TE_WRED_PROF_IDX_BITS,
			      true, profile, tmp, L3TE_WRED_PROF_WORDS);
	spin_unlock_irqrestore(&te->lock, flags);
	return ret;
}

int rtl_te_wred_profile_read(struct rtl_te_flow *te, u8 profile, u32 words[8])
{
	unsigned long flags;
	int ret;

	if (!te || !words || profile >= L3TE_WRED_PROFILES)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_table_access(te, L3TE_WRED_PROF_ACC, L3TE_WRED_PROF_IDX_BITS,
			      false, profile, words, L3TE_WRED_PROF_WORDS);
	spin_unlock_irqrestore(&te->lock, flags);
	return ret;
}

int rtl_te_wred_bind_field(struct rtl_te_flow *te, u8 queue, u8 field,
			   u8 profile)
{
	unsigned long flags;
	int ret;

	if (!te || queue >= L3TE_WRED_QUEUES || profile >= L3TE_WRED_PROFILES)
		return -EINVAL;
	if (field >= L3TE_WRED_SEL_FIELDS)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_wred_sel_update_locked(te, queue, field, profile);
	spin_unlock_irqrestore(&te->lock, flags);
	return ret;
}

/*
 * Bind a queue group (0..63) and VoQ/CoS field (0..7) to a WRED profile
 * (0..31).  The 64-bit select payload holds eight 5-bit profile IDs, one
 * per VoQ 0..7 (aal_l3_te_wred_profile_sel_set @0x72bc0).
 */
int rtl_te_wred_bind(struct rtl_te_flow *te, u8 queue, u8 profile)
{
	return rtl_te_wred_bind_field(te, queue,
				      L3TE_WRED_SEL_FIELD_DEFAULT, profile);
}

int rtl_te_dos_l3_get(struct rtl_te_flow *te, u32 img[L3TE_DOS_CFG_REGS])
{
	unsigned long flags;

	if (!te || !img)
		return -EINVAL;

	spin_lock_irqsave(&te->dos_l3_lock, flags);
	te_dos_read(te, false, img, L3TE_DOS_CFG_REGS);
	spin_unlock_irqrestore(&te->dos_l3_lock, flags);
	return 0;
}

int rtl_te_dos_l3_set(struct rtl_te_flow *te, u64 field_mask,
		      const u32 img[L3TE_DOS_CFG_REGS])
{
	u32 live[L3TE_DOS_CFG_REGS];
	unsigned long flags;

	if (!te || !img)
		return -EINVAL;
	if (field_mask & ~TE_DOS_L3_MASK_ALL)
		return -EINVAL;

	spin_lock_irqsave(&te->dos_l3_lock, flags);
	te_dos_read(te, false, live, L3TE_DOS_CFG_REGS);
	te_dos_blit(live, te_dos_l3_slice, ARRAY_SIZE(te_dos_l3_slice),
		    field_mask, img);
	te_dos_write(te, false, live, L3TE_DOS_CFG_REGS);
	spin_unlock_irqrestore(&te->dos_l3_lock, flags);
	return 0;
}

int rtl_te_dos_l2_get(struct rtl_te_flow *te, u32 img[L3TE_DOS_L2_CFG_REGS])
{
	unsigned long flags;

	if (!te || !img)
		return -EINVAL;

	spin_lock_irqsave(&te->dos_l2_lock, flags);
	te_dos_read(te, true, img, L3TE_DOS_L2_CFG_REGS);
	spin_unlock_irqrestore(&te->dos_l2_lock, flags);
	return 0;
}

int rtl_te_dos_l2_set(struct rtl_te_flow *te, u64 field_mask,
		      const u32 img[L3TE_DOS_L2_CFG_REGS])
{
	u32 live[L3TE_DOS_L2_CFG_REGS];
	unsigned long flags;

	if (!te || !img)
		return -EINVAL;
	if (field_mask & ~TE_DOS_L2_MASK_ALL)
		return -EINVAL;

	spin_lock_irqsave(&te->dos_l2_lock, flags);
	te_dos_read(te, true, live, L3TE_DOS_L2_CFG_REGS);
	te_dos_blit(live, te_dos_l2_slice, ARRAY_SIZE(te_dos_l2_slice),
		    field_mask, img);
	te_dos_write(te, true, live, L3TE_DOS_L2_CFG_REGS);
	spin_unlock_irqrestore(&te->dos_l2_lock, flags);
	return 0;
}

int rtl_te_dos_flood_get(struct rtl_te_flow *te, bool l2, u32 *count)
{
	spinlock_t *lock;
	unsigned long flags;
	u32 mask, value;

	if (!te || !count)
		return -EINVAL;

	mask = l2 ? (u32)L3TE_DOS_L2_FLOOD_CNT_MASK :
		    (u32)L3TE_DOS_FLOOD_CNT_MASK;
	lock = l2 ? &te->dos_l2_lock : &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	value = te_dos_flood_read(te, l2);
	spin_unlock_irqrestore(lock, flags);

	*count = value & mask;
	return 0;
}

int rtl_te_dos_flood_clear(struct rtl_te_flow *te, bool l2)
{
	spinlock_t *lock;
	unsigned long flags;

	if (!te)
		return -EINVAL;

	lock = l2 ? &te->dos_l2_lock : &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	te_dos_flood_clear_reg(te, l2);
	spin_unlock_irqrestore(lock, flags);
	return 0;
}

int rtl_te_dos_flood_unit_set(struct rtl_te_flow *te, bool l2, u32 unit)
{
	spinlock_t *lock;
	unsigned long flags;
	int ret;

	if (!te)
		return -EINVAL;

	lock = l2 ? &te->dos_l2_lock : &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	ret = te_dos_flood_unit_write(te, l2, unit);
	spin_unlock_irqrestore(lock, flags);
	return ret;
}

int rtl_te_dos_type_en_get(struct rtl_te_flow *te, bool l2, u32 *bitmap)
{
	spinlock_t *lock;
	unsigned long flags;

	if (!te || !bitmap)
		return -EINVAL;

	lock = l2 ? &te->dos_l2_lock : &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	*bitmap = te_dos_type_en_read(te, l2);
	spin_unlock_irqrestore(lock, flags);
	return 0;
}

int rtl_te_dos_type_en_set(struct rtl_te_flow *te, bool l2, u32 bitmap)
{
	spinlock_t *lock;
	unsigned long flags;

	if (!te)
		return -EINVAL;
	/* the field is 16 bit wide; a wider value would be truncated silently */
	if (bitmap & ~(u32)L3TE_DOS_TYPE_ALL)
		return -EINVAL;

	lock = l2 ? &te->dos_l2_lock : &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	te_dos_type_en_write(te, l2, bitmap);
	spin_unlock_irqrestore(lock, flags);
	return 0;
}

int rtl_te_dos_type_set_action(struct rtl_te_flow *te, bool l2,
			       enum rtl_dos_type type, bool enable, bool trap)
{
	spinlock_t *lock;
	unsigned long flags;
	int ret;

	if (!te)
		return -EINVAL;
	if (type >= RTL_DOS_TYPE_MAX)
		return -EINVAL;

	lock = l2 ? &te->dos_l2_lock : &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	ret = te_dos_type_set_action(te, l2, type, enable, trap);
	spin_unlock_irqrestore(lock, flags);
	return ret;
}

int rtl_te_dos_type_set(struct rtl_te_flow *te, bool l2,
			enum rtl_dos_type type, bool enable)
{
	return rtl_te_dos_type_set_action(te, l2, type, enable, false);
}

int rtl_te_dos_pol_id_set(struct rtl_te_flow *te, enum rtl_dos_type type,
			  u32 pol_id)
{
	spinlock_t *lock;
	unsigned long flags;
	int ret;

	if (!te)
		return -EINVAL;
	if (type >= RTL_DOS_TYPE_MAX)
		return -EINVAL;
	/* L2 has no pol_id field at all (phase2-re9 section 9) */
	if (!te_dos_l3_types[type].pol.width)
		return -EOPNOTSUPP;

	lock = &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	ret = te_dos_field_write(te, &te_dos_l3_types[type].pol, pol_id);
	spin_unlock_irqrestore(lock, flags);
	return ret;
}

int rtl_te_dos_th_set(struct rtl_te_flow *te, bool l2, enum rtl_dos_type type,
		      u32 th)
{
	const struct te_dos_type_desc *desc = l2 ? te_dos_l2_types :
						    te_dos_l3_types;
	spinlock_t *lock;
	unsigned long flags;
	int ret;

	if (!te)
		return -EINVAL;
	if (type >= RTL_DOS_TYPE_MAX)
		return -EINVAL;
	/* only the three flood types carry a threshold */
	if (!desc[type].th.width)
		return -EOPNOTSUPP;

	lock = l2 ? &te->dos_l2_lock : &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	ret = te_dos_field_write(te, &desc[type].th, th);
	spin_unlock_irqrestore(lock, flags);
	return ret;
}

int rtl_te_dos_port_mask_get(struct rtl_te_flow *te, bool l2, u64 *mask)
{
	spinlock_t *lock;
	unsigned long flags;
	u32 w0, w1, w2;

	if (!te || !mask)
		return -EINVAL;

	lock = l2 ? &te->dos_l2_lock : &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	if (l2) {
		w0 = te_read(te, L3TE_DOS_L2_PORT_MASK_W0);
		w1 = te_read(te, L3TE_DOS_L2_PORT_MASK_W1);
		w2 = te_read(te, L3TE_DOS_L2_PORT_MASK_W2);
		*mask = (u64)FIELD_GET(L3TE_DOS_L2_PORT_MASK_W0_MASK, w0) |
			((u64)w1 << L3TE_DOS_L2_PORT_MASK_W0_SHIFT) |
			((u64)FIELD_GET(L3TE_DOS_L2_PORT_MASK_W2_MASK, w2) <<
			 L3TE_DOS_L2_PORT_MASK_W2_SHIFT);
	} else {
		*mask = (u64)te_read(te, L3TE_DOS_PORT_MASK_LO) |
			((u64)te_read(te, L3TE_DOS_PORT_MASK_HI) << 32);
	}
	spin_unlock_irqrestore(lock, flags);
	return 0;
}

int rtl_te_dos_port_mask_set(struct rtl_te_flow *te, bool l2, u64 mask)
{
	spinlock_t *lock;
	unsigned long flags;
	u32 w0, w1, w2;

	if (!te)
		return -EINVAL;

	lock = l2 ? &te->dos_l2_lock : &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	if (l2) {
		/* 0x1228[31:16] || 0x122c || 0x1230[15:0]; the other halves of
		 * the two boundary registers are unrelated fields.
		 */
		w0 = te_read(te, L3TE_DOS_L2_PORT_MASK_W0);
		w1 = (u32)(mask >> L3TE_DOS_L2_PORT_MASK_W0_SHIFT);
		w2 = te_read(te, L3TE_DOS_L2_PORT_MASK_W2);
		wmb(); /* order register writes in ascending address order */
		te_write(te, L3TE_DOS_L2_PORT_MASK_W0,
			 (w0 & ~L3TE_DOS_L2_PORT_MASK_W0_MASK) |
			 FIELD_PREP(L3TE_DOS_L2_PORT_MASK_W0_MASK,
				    mask & L3TE_DOS_PORT_MASK_HALF_MASK));
		wmb(); /* order W0 write before W1 write */
		te_write(te, L3TE_DOS_L2_PORT_MASK_W1, w1);
		wmb(); /* order W1 write before W2 write */
		te_write(te, L3TE_DOS_L2_PORT_MASK_W2,
			 (w2 & ~L3TE_DOS_L2_PORT_MASK_W2_MASK) |
			 FIELD_PREP(L3TE_DOS_L2_PORT_MASK_W2_MASK,
				    (mask >> L3TE_DOS_L2_PORT_MASK_W2_SHIFT) &
				    L3TE_DOS_PORT_MASK_HALF_MASK));
	} else {
		wmb(); /* order register writes in ascending address order */
		te_write(te, L3TE_DOS_PORT_MASK_LO, lower_32_bits(mask));
		wmb(); /* order LO write before HI write */
		te_write(te, L3TE_DOS_PORT_MASK_HI, upper_32_bits(mask));
	}
	spin_unlock_irqrestore(lock, flags);
	return 0;
}

int rtl_te_dos_evt_get(struct rtl_te_flow *te, bool l2, bool drop, u32 *raw)
{
	spinlock_t *lock;
	unsigned long flags;
	u32 reg;

	if (!te || !raw)
		return -EINVAL;
	/* the L2 block has no drop register at all (section 7.3) */
	if (l2 && drop)
		return -EOPNOTSUPP;

	reg = l2 ? L3TE_DOS_L2_EVT_TRAP :
	      (drop ? L3TE_DOS_EVT_DROP : L3TE_DOS_EVT_TRAP);
	lock = l2 ? &te->dos_l2_lock : &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	*raw = te_read(te, reg);
	spin_unlock_irqrestore(lock, flags);
	return 0;
}

int rtl_te_dos_evt_set(struct rtl_te_flow *te, bool l2, bool drop,
		       u32 field_mask, const struct rtl_te_dos_evt *evt)
{
	spinlock_t *lock;
	unsigned long flags;
	u32 reg, mask, value;
	unsigned int i;

	if (!te || !evt)
		return -EINVAL;
	if (l2 && drop)
		return -EOPNOTSUPP;
	if (field_mask & ~L3TE_DOS_EVT_F_ALL)
		return -EINVAL;
	if (l2 && (field_mask & ~L3TE_DOS_EVT_F_L2_ALL))
		return -EOPNOTSUPP;
	if (!field_mask)
		return -EINVAL;

	if ((field_mask & L3TE_DOS_EVT_F_LDPID) &&
	    evt->ldpid > L3TE_DOS_EVT_LDPID_MAX)
		return -EINVAL;
	if ((field_mask & L3TE_DOS_EVT_F_COS) &&
	    evt->cos > L3TE_DOS_EVT_COS_MAX)
		return -EINVAL;
	for (i = 0; i < L3TE_DOS_EVT_TIMERS; i++)
		if ((field_mask & L3TE_DOS_EVT_F_TIMER(i)) &&
		    evt->timer[i] > L3TE_DOS_EVT_TIMER_MAX)
			return -EINVAL;

	reg = l2 ? L3TE_DOS_L2_EVT_TRAP :
	      (drop ? L3TE_DOS_EVT_DROP : L3TE_DOS_EVT_TRAP);
	mask = te_dos_evt_mask(field_mask);
	lock = l2 ? &te->dos_l2_lock : &te->dos_l3_lock;
	spin_lock_irqsave(lock, flags);
	value = te_read(te, reg);
	value = (value & ~mask) | te_dos_evt_bits(field_mask, evt);
	wmb(); /* order read-modify-write of event control register */
	te_write(te, reg, value);
	spin_unlock_irqrestore(lock, flags);
	return 0;
}

int rtl_te_dos_set(struct rtl_te_flow *te, bool enable)
{
	static const u32 zero[L3TE_DOS_CFG_REGS] = { 0 };
	static const u32 zero_l2[L3TE_DOS_L2_CFG_REGS] = { 0 };
	int ret;

	if (!te)
		return -EINVAL;

	/* Arming needs explicit thresholds, types, and port masks; "on"
	 * records software state while "off" clears both L3 and L2 blocks.
	 */
	if (!enable) {
		ret = rtl_te_dos_l3_set(te, TE_DOS_L3_MASK_ALL, zero);
		if (ret)
			return ret;
		ret = rtl_te_dos_l2_set(te, TE_DOS_L2_MASK_ALL, zero_l2);
		if (ret)
			return ret;
	}
	te->dos_enabled = enable;
	return 0;
}

void rtl_te_flow_set_sync(struct rtl_te_flow *te, void (*sync)(void *data),
			  void *data)
{
	unsigned long flags;

	if (!te)
		return;

	spin_lock_irqsave(&te->lock, flags);
	te->hostpol_sync = sync;
	te->hostpol_sync_data = data;
	spin_unlock_irqrestore(&te->lock, flags);
}

int rtl_te_hostpol_add_vmeter(struct rtl_te_flow *te,
			      const u8 mac[RTL_TE_HOSTPOL_ALEN],
			      u32 ingress_kbps, u32 egress_kbps,
			      u8 ingress_vmeter, u8 egress_vmeter)
{
	void (*sync_cb)(void *data);
	void *sync_data;
	unsigned long flags;
	int ret;

	if (!te || !mac)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_hostpol_add_locked(te, mac, ingress_kbps, egress_kbps,
				    ingress_vmeter, egress_vmeter);
	sync_cb = te->hostpol_sync;
	sync_data = te->hostpol_sync_data;
	spin_unlock_irqrestore(&te->lock, flags);
	if (!ret && sync_cb)
		sync_cb(sync_data);
	return ret;
}

int rtl_te_hostpol_add(struct rtl_te_flow *te,
		       const u8 mac[RTL_TE_HOSTPOL_ALEN],
		       u32 ingress_kbps, u32 egress_kbps)
{
	return rtl_te_hostpol_add_vmeter(te, mac, ingress_kbps, egress_kbps,
					 RTL_TE_HOSTPOL_VMETER_AUTO,
					 RTL_TE_HOSTPOL_VMETER_AUTO);
}

int rtl_te_hostpol_del(struct rtl_te_flow *te,
		       const u8 mac[RTL_TE_HOSTPOL_ALEN])
{
	void (*sync_cb)(void *data);
	void *sync_data;
	unsigned long flags;
	int ret;

	if (!te || !mac)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_hostpol_del_locked(te, mac);
	sync_cb = te->hostpol_sync;
	sync_data = te->hostpol_sync_data;
	spin_unlock_irqrestore(&te->lock, flags);
	if (!ret && sync_cb)
		sync_cb(sync_data);
	return ret;
}

int rtl_te_hostpol_lookup_ex(struct rtl_te_flow *te,
			     const u8 mac[RTL_TE_HOSTPOL_ALEN], u8 dir,
			     u8 *slot, bool *rate_limited)
{
	unsigned long flags;
	bool limited = false;
	u16 pol1 = 0;
	int ret;

	if (!te || !mac || !slot)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_hostpol_lookup_locked(te, mac, dir, &pol1, &limited);
	spin_unlock_irqrestore(&te->lock, flags);

	if (ret)
		return ret;
	*slot = (u8)pol1;
	if (rate_limited)
		*rate_limited = limited;
	return 0;
}

int rtl_te_hostpol_lookup(struct rtl_te_flow *te,
			  const u8 mac[RTL_TE_HOSTPOL_ALEN], u8 dir, u8 *slot)
{
	return rtl_te_hostpol_lookup_ex(te, mac, dir, slot, NULL);
}

int rtl_te_hostpol_mib_raw(struct rtl_te_flow *te, u8 entry, u8 dir,
			   struct rtl_te_hostpol_mib *mib)
{
	unsigned long flags;
	int ret;

	if (!te || !mib)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_hostpol_mib_raw_locked(te, entry, dir, mib);
	spin_unlock_irqrestore(&te->lock, flags);
	return ret;
}

int rtl_te_hostpol_mib_get(struct rtl_te_flow *te, u8 entry, u64 *rx, u64 *tx)
{
	unsigned long flags;
	u64 r = 0, t = 0;
	int ret;

	if (!te || !rx || !tx)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_hostpol_mib_get_locked(te, entry, &r, &t);
	spin_unlock_irqrestore(&te->lock, flags);

	if (ret)
		return ret;
	*rx = r;
	*tx = t;
	return 0;
}

int rtl_te_hostpol_mib_clear(struct rtl_te_flow *te, u8 entry)
{
	unsigned long flags;
	int ret;

	if (!te)
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	ret = te_hostpol_mib_clear_locked(te, entry);
	spin_unlock_irqrestore(&te->lock, flags);
	return ret;
}

/* ---------------------------------------------------------------------- */
/* debugfs                                                                 */
/* ---------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_DEBUG_FS)

static char *te_next_token(char **cursor)
{
	char *token;

	while (**cursor == ' ' || **cursor == '\t')
		(*cursor)++;
	if (!**cursor)
		return NULL;

	token = *cursor;
	while (**cursor && **cursor != ' ' && **cursor != '\t')
		(*cursor)++;
	if (**cursor)
		*(*cursor)++ = '\0';

	return token;
}

static int te_parse_u32(const char *token, u32 *value)
{
	if (!token || kstrtou32(token, 0, value))
		return -EINVAL;
	return 0;
}

static int te_parse_rates(char **tok, u32 *cir_kbps, u32 *pir_kbps, u32 *cbs,
			  u32 *pbs)
{
	if (te_parse_u32(tok[1], cir_kbps) || te_parse_u32(tok[2], pir_kbps) ||
	    te_parse_u32(tok[3], cbs) || te_parse_u32(tok[4], pbs))
		return -EINVAL;
	if (!*cir_kbps || *cir_kbps > L3TE_POL_RATE_MAX_KBPS)
		return -EINVAL;
	if (!*pir_kbps || *pir_kbps > L3TE_POL_RATE_MAX_KBPS)
		return -EINVAL;
	if (*cir_kbps > *pir_kbps)
		return -EINVAL;
	if (!*cbs || *cbs > L3TE_POL_BURST_MAX)
		return -EINVAL;
	if (!*pbs || *pbs > L3TE_POL_BURST_MAX)
		return -EINVAL;
	return 0;
}

/*
 * The allocators hand out the lowest free slot, so an explicit slot request
 * is honoured as "update this slot if it is already ours, otherwise take a
 * fresh one and verify it is the requested one".
 */
static int te_cmd_policer(struct rtl_te_flow *te, bool group, char **tok,
			  unsigned int ntok)
{
	u32 slot, cir_kbps, pir_kbps, cbs, pbs;
	u8 out;
	int ret;

	if (ntok != 5)
		return -EINVAL;
	if (te_parse_u32(tok[0], &slot))
		return -EINVAL;
	if (slot >= (group ? L3TE_POL_GRP_SLOTS : L3TE_POL_FLOW_SLOTS))
		return -EINVAL;
	ret = te_parse_rates(tok, &cir_kbps, &pir_kbps, &cbs, &pbs);
	if (ret)
		return ret;

	if (group) {
		ret = rtl_te_policer_group_update(te, slot,
						  (u64)cir_kbps * 1000u,
						  (u64)pir_kbps * 1000u, cbs,
						  pbs);
		if (!ret)
			return 0;
		if (ret != -EINVAL)
			return ret;
		ret = rtl_te_policer_group_alloc(te, (u64)cir_kbps * 1000u,
						 (u64)pir_kbps * 1000u, cbs,
						 pbs, &out);
		if (!ret && out != slot) {
			rtl_te_policer_group_free(te, out);
			return -EBUSY;
		}
		return ret;
	}

	ret = rtl_te_policer_update(te, slot, (u64)cir_kbps * 1000u,
				    (u64)pir_kbps * 1000u, cbs, pbs);
	if (!ret)
		return 0;
	if (ret != -EINVAL)
		return ret;
	ret = rtl_te_policer_alloc(te, (u64)cir_kbps * 1000u,
				   (u64)pir_kbps * 1000u, cbs, pbs, &out);
	if (!ret && out != slot) {
		rtl_te_policer_free(te, out);
		return -EBUSY;
	}
	return ret;
}

static int te_cmd_free(struct rtl_te_flow *te, bool group, char **tok,
		       unsigned int ntok)
{
	u32 slot;

	if (ntok != 1 || te_parse_u32(tok[0], &slot))
		return -EINVAL;
	if (slot >= (group ? L3TE_POL_GRP_SLOTS : L3TE_POL_FLOW_SLOTS))
		return -EINVAL;

	if (group)
		return rtl_te_policer_group_free(te, slot);
	return rtl_te_policer_free(te, slot);
}

static int te_cmd_wred(struct rtl_te_flow *te, char **tok, unsigned int ntok)
{
	u32 queue, profile, field = L3TE_WRED_SEL_FIELD_DEFAULT;
	bool bind_all = false;

	if (ntok != 2 && ntok != 3)
		return -EINVAL;
	if (te_parse_u32(tok[0], &queue) || te_parse_u32(tok[1], &profile))
		return -EINVAL;
	if (ntok == 3) {
		if (!strcmp(tok[2], "all"))
			bind_all = true;
		else if (te_parse_u32(tok[2], &field))
			return -EINVAL;
	}
	if (queue >= L3TE_WRED_QUEUES || profile >= L3TE_WRED_PROFILES ||
	    (!bind_all && field >= L3TE_WRED_SEL_FIELDS))
		return -EINVAL;

	if (bind_all) {
		unsigned int f;
		int ret;

		for (f = 0; f < L3TE_WRED_SEL_FIELDS; f++) {
			ret = rtl_te_wred_bind_field(te, queue, f, profile);
			if (ret)
				return ret;
		}
		return 0;
	}

	return rtl_te_wred_bind_field(te, queue, field, profile);
}

static int te_cmd_wred_prof(struct rtl_te_flow *te, char **tok,
			    unsigned int ntok)
{
	u32 words[L3TE_WRED_PROF_WORDS];
	u32 profile;
	unsigned int i;

	if (ntok != 1 + L3TE_WRED_PROF_WORDS)
		return -EINVAL;
	if (te_parse_u32(tok[0], &profile) || profile >= L3TE_WRED_PROFILES)
		return -EINVAL;
	for (i = 0; i < L3TE_WRED_PROF_WORDS; i++) {
		if (te_parse_u32(tok[1 + i], &words[i]))
			return -EINVAL;
	}

	return rtl_te_wred_profile_write(te, profile, words);
}

static int te_cmd_wred_ctrl(struct rtl_te_flow *te, char **tok,
			    unsigned int ntok)
{
	unsigned long flags;
	u32 ena, ecn = 0, val, arb;

	if (ntok != 1 && ntok != 2)
		return -EINVAL;
	if (te_parse_u32(tok[0], &ena) || ena > 1)
		return -EINVAL;
	if (ntok == 2 && (te_parse_u32(tok[1], &ecn) || ecn > 1))
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	if (ena)
		te_hostpol_pm_ready(te);
	val = 0;
	if (ena)
		val |= L3TE_GLB_CTRL_WRED_EN;
	if (ecn)
		val |= L3TE_GLB_CTRL_WRED_ECN_EN | L3TE_GLB_CTRL_MARK_ECN_EN;
	regmap_update_bits(te->regs, L3TE_TE_GLB_CTRL,
			   L3TE_GLB_CTRL_WRED_EN | L3TE_GLB_CTRL_MARK_ECN_EN |
			   L3TE_GLB_CTRL_WRED_ECN_EN, val);

	arb = te_read(te, L3TE_L2FE_ARB_CTRL);
	if (ecn)
		arb |= L3TE_L2FE_ARB_CTRL_IP_ECN_EN;
	else
		arb &= ~L3TE_L2FE_ARB_CTRL_IP_ECN_EN;
	te_write(te, L3TE_L2FE_ARB_CTRL, arb);
	spin_unlock_irqrestore(&te->lock, flags);
	return 0;
}

static int te_cmd_wred_cfg(struct rtl_te_flow *te, char **tok,
			   unsigned int ntok)
{
	unsigned long flags;
	u32 fast_decay, weight, sel_worst = 0, val;

	if (ntok != 2 && ntok != 3)
		return -EINVAL;
	if (te_parse_u32(tok[0], &fast_decay) || fast_decay > 1)
		return -EINVAL;
	if (te_parse_u32(tok[1], &weight) || weight > 15)
		return -EINVAL;
	if (ntok == 3 && (te_parse_u32(tok[2], &sel_worst) || sel_worst > 1))
		return -EINVAL;

	spin_lock_irqsave(&te->lock, flags);
	val = te_read(te, L3TE_WRED_CTRL);
	val &= ~(L3TE_WRED_CTRL_FAST_DECAY | L3TE_WRED_CTRL_AFGS_WEIGHT |
		 L3TE_WRED_CTRL_AFGS_SEL_WORST);
	if (fast_decay)
		val |= L3TE_WRED_CTRL_FAST_DECAY;
	val |= FIELD_PREP(L3TE_WRED_CTRL_AFGS_WEIGHT, weight);
	if (sel_worst)
		val |= L3TE_WRED_CTRL_AFGS_SEL_WORST;
	te_write(te, L3TE_WRED_CTRL, val);
	spin_unlock_irqrestore(&te->lock, flags);
	return 0;
}

static int te_cmd_wred_afgs(struct rtl_te_flow *te, char **tok,
			    unsigned int ntok)
{
	unsigned long flags;
	u32 idx, depth, depth_eq1 = 0, word;
	int ret;

	if (ntok != 2 && ntok != 3)
		return -EINVAL;
	if (te_parse_u32(tok[0], &idx) || idx >= L3TE_WRED_AFGS_ENTRIES)
		return -EINVAL;
	if (te_parse_u32(tok[1], &depth) || depth > L3TE_WRED_AFGS_DEPTH_MASK)
		return -EINVAL;
	if (ntok == 3 &&
	    (te_parse_u32(tok[2], &depth_eq1) ||
	     depth_eq1 > L3TE_WRED_AFGS_DEPTH_MASK))
		return -EINVAL;

	word = depth | (depth_eq1 << L3TE_WRED_AFGS_EQ1_SHIFT);
	spin_lock_irqsave(&te->lock, flags);
	ret = te_table_access(te, L3TE_WRED_AFGS_ACC, L3TE_WRED_AFGS_IDX_BITS,
			      true, idx, &word, 1);
	spin_unlock_irqrestore(&te->lock, flags);
	return ret;
}

static int te_cmd_wred_dump(struct rtl_te_flow *te, char **tok,
			    unsigned int ntok)
{
	u32 words[L3TE_WRED_PROF_WORDS];
	u32 profile;
	int ret;

	if (ntok != 1 || te_parse_u32(tok[0], &profile))
		return -EINVAL;
	if (profile >= L3TE_WRED_PROFILES)
		return -EINVAL;

	ret = rtl_te_wred_profile_read(te, profile, words);
	if (ret)
		return ret;

	/* The same data is part of "cat te_flow"; this form exists so a single
	 * profile can be sampled without paging the whole dump.
	 */
	dev_info(te->dev,
		 "wred[%02u] %08x %08x %08x %08x %08x %08x %08x %08x\n",
		 profile, words[0], words[1], words[2], words[3], words[4],
		 words[5], words[6], words[7]);
	return 0;
}

static int te_cmd_pie(struct rtl_te_flow *te, char **tok, unsigned int ntok)
{
	unsigned long flags;
	u32 port, value;

	if (ntok != 2)
		return -EINVAL;
	if (te_parse_u32(tok[0], &port) || te_parse_u32(tok[1], &value))
		return -EINVAL;
	if (port >= L3TE_PIE_WRED_PORTS)
		return -EINVAL;

	/* section 4.4: 4 PIE/WRED engine instances (0..3, separate from the
	 * 8 L3TE ports / 64 VOQs); the OEM AAL writes the whole 32-bit word
	 * directly and per-bit field masks are undocumented, so pass through
	 * the operator-supplied value verbatim.
	 */
	spin_lock_irqsave(&te->lock, flags);
	te_write(te, L3TE_PIE_WRED_PORT_EN(port), value);
	spin_unlock_irqrestore(&te->lock, flags);
	return 0;
}

static ssize_t rtl_te_flow_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct rtl_te_flow *te = seq->private;
	char buf[160];
	char *tok[10];
	char *cur, *cmd;
	unsigned int ntok = 0;
	int ret;

	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	strreplace(buf, '\n', '\0');

	cur = buf;
	cmd = te_next_token(&cur);
	if (!cmd)
		return -EINVAL;
	while (ntok < ARRAY_SIZE(tok)) {
		char *token = te_next_token(&cur);

		if (!token)
			break;
		tok[ntok++] = token;
	}
	if (te_next_token(&cur))
		return -EINVAL;

	if (!strcmp(cmd, "dump")) {
		ret = ntok ? -EINVAL : 0;
	} else if (!strcmp(cmd, "policer")) {
		ret = te_cmd_policer(te, false, tok, ntok);
	} else if (!strcmp(cmd, "policer-free")) {
		ret = te_cmd_free(te, false, tok, ntok);
	} else if (!strcmp(cmd, "grp")) {
		ret = te_cmd_policer(te, true, tok, ntok);
	} else if (!strcmp(cmd, "grp-free")) {
		ret = te_cmd_free(te, true, tok, ntok);
	} else if (!strcmp(cmd, "wred")) {
		ret = te_cmd_wred(te, tok, ntok);
	} else if (!strcmp(cmd, "wred-prof")) {
		ret = te_cmd_wred_prof(te, tok, ntok);
	} else if (!strcmp(cmd, "wred-ctrl")) {
		ret = te_cmd_wred_ctrl(te, tok, ntok);
	} else if (!strcmp(cmd, "wred-cfg")) {
		ret = te_cmd_wred_cfg(te, tok, ntok);
	} else if (!strcmp(cmd, "wred-afgs")) {
		ret = te_cmd_wred_afgs(te, tok, ntok);
	} else if (!strcmp(cmd, "wred-dump")) {
		ret = te_cmd_wred_dump(te, tok, ntok);
	} else if (!strcmp(cmd, "pie")) {
		ret = te_cmd_pie(te, tok, ntok);
	} else if (!strcmp(cmd, "dos")) {
		if (ntok != 1)
			ret = -EINVAL;
		else if (!strcmp(tok[0], "on"))
			ret = rtl_te_dos_set(te, true);
		else if (!strcmp(tok[0], "off"))
			ret = rtl_te_dos_set(te, false);
		else
			ret = -EINVAL;
	} else {
		ret = -EINVAL;
	}

	return ret ? ret : (ssize_t)count;
}

static void te_dump_slots(struct seq_file *s, struct rtl_te_flow *te)
{
	unsigned long flow_bm[BITS_TO_LONGS(L3TE_POL_FLOW_SLOTS)];
	unsigned long group_bm[BITS_TO_LONGS(L3TE_POL_GRP_SLOTS)];
	u32 cfg[L3TE_POL_FLOW_SLOTS / L3TE_POL_BANK_SLOTS];
	u32 gcfg[L3TE_POL_GRP_SLOTS / L3TE_POL_BANK_SLOTS];
	u32 pie[L3TE_PIE_WRED_PORTS];
	u32 glb, pol_cfg, fe_pol_cfg, wred_ctrl, arb_ctrl;
	u32 pm_pkts, pm_pol_drop, pm_tail_drop;
	u32 pm_wred_y_drop, pm_wred_g_drop, pm_ce;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&te->lock, flags);
	memcpy(flow_bm, te->flow_slots, sizeof(flow_bm));
	memcpy(group_bm, te->group_slots, sizeof(group_bm));
	for (i = 0; i < ARRAY_SIZE(cfg); i++)
		cfg[i] = te_read(te, L3TE_POL_FLOW_CFG(i * L3TE_POL_BANK_SLOTS));
	for (i = 0; i < ARRAY_SIZE(gcfg); i++)
		gcfg[i] = te_read(te, L3TE_POL_GRP_CFG(i * L3TE_POL_BANK_SLOTS));
	for (i = 0; i < ARRAY_SIZE(pie); i++)
		pie[i] = te_read(te, L3TE_PIE_WRED_PORT_EN(i));
	glb = te_read(te, L3TE_TE_GLB_CTRL);
	pol_cfg = te_read(te, L3TE_POL_GLB_CFG);
	fe_pol_cfg = te_read(te, L3TE_FE_POL_GLB_CFG);
	wred_ctrl = te_read(te, L3TE_WRED_CTRL);
	arb_ctrl = te_read(te, L3TE_L2FE_ARB_CTRL);
	pm_pkts = te_read(te, L3TE_PM_PCNT);
	pm_pol_drop = te_read(te, L3TE_PM_POL_DROP_PCNT);
	pm_tail_drop = te_read(te, L3TE_PM_TAIL_DROP_PCNT);
	pm_wred_y_drop = te_read(te, L3TE_PM_WRED_Y_DROP_PCNT);
	pm_wred_g_drop = te_read(te, L3TE_PM_WRED_G_DROP_PCNT);
	pm_ce = te_read(te, L3TE_PM_CE_PCNT);
	spin_unlock_irqrestore(&te->lock, flags);

	seq_printf(s, "group64_ready=%u group_cfg_depth=%u\n",
		   te->group64_ready, !!(glb & L3TE_TE_GLB_POL_GRP_64));
	seq_printf(s, "flow_slots=%u/%u group_slots=%u/%u\n",
		   bitmap_weight(flow_bm, L3TE_POL_FLOW_SLOTS),
		   L3TE_POL_FLOW_SLOTS,
		   bitmap_weight(group_bm, L3TE_POL_GRP_SLOTS),
		   L3TE_POL_GRP_SLOTS);
	seq_puts(s, "flow_bitmap=");
	for (i = 0; i < ARRAY_SIZE(flow_bm); i++)
		seq_printf(s, "%016lx", flow_bm[i]);
	seq_puts(s, "\ngroup_bitmap=");
	for (i = 0; i < ARRAY_SIZE(group_bm); i++)
		seq_printf(s, "%016lx", group_bm[i]);
	seq_puts(s, "\nflow_cfg=");
	for (i = 0; i < ARRAY_SIZE(cfg); i++)
		seq_printf(s, " %08x", cfg[i]);
	seq_puts(s, "\ngroup_cfg=");
	for (i = 0; i < ARRAY_SIZE(gcfg); i++)
		seq_printf(s, " %08x", gcfg[i]);
	seq_puts(s, "\npie_wred_port_en=");
	for (i = 0; i < ARRAY_SIZE(pie); i++)
		seq_printf(s, " %08x", pie[i]);
	seq_puts(s, "\n");
	seq_printf(s, "l3_glb_ctrl=0x%08x (wred_en=%u ecn=%u)\n",
		   glb, !!(glb & L3TE_GLB_CTRL_WRED_EN),
		   !!(glb & L3TE_GLB_CTRL_WRED_ECN_EN));
	seq_printf(s,
		   "wred_ctrl=0x%08x (fast_decay=%u weight=%lu sel_worst=%u mark_ecn=%u arb_ecn=%u)\n",
		   wred_ctrl, !!(wred_ctrl & L3TE_WRED_CTRL_FAST_DECAY),
		   FIELD_GET(L3TE_WRED_CTRL_AFGS_WEIGHT, wred_ctrl),
		   !!(wred_ctrl & L3TE_WRED_CTRL_AFGS_SEL_WORST),
		   !!(glb & L3TE_GLB_CTRL_MARK_ECN_EN),
		   !!(arb_ctrl & L3TE_L2FE_ARB_CTRL_IP_ECN_EN));
	seq_printf(s,
		   "te_pm: pkts=%u pol_drop=%u tail_drop=%u wred_y_drop=%u wred_g_drop=%u ce_mark=%u\n",
		   pm_pkts, pm_pol_drop, pm_tail_drop,
		   pm_wred_y_drop, pm_wred_g_drop, pm_ce);
	seq_printf(s,
		   "policer_en=%u policer_ready=%u pol_cfg_9180=0x%08x fe_pol_cfg_30ac=0x%08x\n",
		   !!(glb & L3TE_TE_GLB_POLICER_EN), te->policer_ready,
		   pol_cfg, fe_pol_cfg);
}

static void te_dump_policers(struct seq_file *s, struct rtl_te_flow *te)
{
	struct rtl_te_policer_profile p;
	unsigned long flags;
	unsigned int idx;
	u32 words[3];
	bool used;
	int ret;

	for (idx = 0; idx < L3TE_POL_FLOW_SLOTS; idx++) {
		spin_lock_irqsave(&te->lock, flags);
		used = test_bit(idx, te->flow_slots);
		ret = used ? te_pol_profile_read(te, L3TE_POL_FLOW_ACC,
						 L3TE_POL_FLOW_IDX_BITS, idx,
						 words)
			   : -ENOENT;
		spin_unlock_irqrestore(&te->lock, flags);
		if (!used)
			continue;
		if (ret) {
			seq_printf(s, "flow[%3u] read error %d\n", idx, ret);
			continue;
		}
		te_policer_unpack(words, &p);
		seq_printf(s,
			   "flow[%3u] cir=%ukbps pir=%ukbps cbs=%u pbs=%u words=%08x/%08x/%08x\n",
			   idx,
			   p.cir_hi * L3TE_POL_RATE_DIVISOR + p.cir_lo,
			   p.pir_hi * L3TE_POL_RATE_DIVISOR + p.pir_lo,
			   p.cbs, p.pbs, words[0], words[1], words[2]);
	}

	for (idx = 0; idx < L3TE_POL_GRP_SLOTS; idx++) {
		spin_lock_irqsave(&te->lock, flags);
		used = test_bit(idx, te->group_slots);
		ret = used ? te_pol_profile_read(te, L3TE_POL_GRP_ACC,
						 L3TE_POL_GRP_IDX_BITS, idx,
						 words)
			   : -ENOENT;
		spin_unlock_irqrestore(&te->lock, flags);
		if (!used)
			continue;
		if (ret) {
			seq_printf(s, "group[%2u] read error %d\n", idx, ret);
			continue;
		}
		te_policer_unpack(words, &p);
		seq_printf(s,
			   "group[%2u] cir=%ukbps pir=%ukbps cbs=%u pbs=%u\n",
			   idx,
			   p.cir_hi * L3TE_POL_RATE_DIVISOR + p.cir_lo,
			   p.pir_hi * L3TE_POL_RATE_DIVISOR + p.pir_lo,
			   p.cbs, p.pbs);
	}
}

static void te_dump_wred(struct seq_file *s, struct rtl_te_flow *te)
{
	unsigned long flags;
	unsigned int i, j;
	u32 words[L3TE_WRED_PROF_WORDS];
	u32 sel[L3TE_WRED_SEL_WORDS];
	u32 afgs;
	int ret;

	seq_puts(s, "wred_afgs=");
	for (i = 0; i < L3TE_WRED_AFGS_ENTRIES; i++) {
		afgs = 0;
		spin_lock_irqsave(&te->lock, flags);
		ret = te_table_access(te, L3TE_WRED_AFGS_ACC,
				      L3TE_WRED_AFGS_IDX_BITS, false, i,
				      &afgs, 1);
		spin_unlock_irqrestore(&te->lock, flags);
		if (ret)
			seq_puts(s, " err");
		else
			seq_printf(s, " %u/%u",
				   afgs & L3TE_WRED_AFGS_DEPTH_MASK,
				   (afgs >> L3TE_WRED_AFGS_EQ1_SHIFT) &
				   L3TE_WRED_AFGS_DEPTH_MASK);
	}
	seq_puts(s, "\n");

	for (i = 0; i < L3TE_WRED_PROFILES; i++) {
		spin_lock_irqsave(&te->lock, flags);
		ret = te_table_access(te, L3TE_WRED_PROF_ACC,
				      L3TE_WRED_PROF_IDX_BITS, false, i, words,
				      L3TE_WRED_PROF_WORDS);
		spin_unlock_irqrestore(&te->lock, flags);
		if (ret) {
			seq_printf(s, "wred[%02u] read error %d\n", i, ret);
			continue;
		}
		seq_printf(s, "wred[%02u]", i);
		for (j = 0; j < L3TE_WRED_PROF_WORDS; j++)
			seq_printf(s, " %08x", words[j]);
		seq_puts(s, "\n");
	}

	for (i = 0; i < L3TE_WRED_QUEUES; i++) {
		spin_lock_irqsave(&te->lock, flags);
		ret = te_table_access(te, L3TE_WRED_SEL_ACC,
				      L3TE_WRED_SEL_IDX_BITS, false, i, sel,
				      L3TE_WRED_SEL_WORDS);
		spin_unlock_irqrestore(&te->lock, flags);
		if (ret) {
			seq_printf(s, "wred-sel[%02u] read error %d\n", i, ret);
			continue;
		}
		if (!sel[0] && !sel[1])
			continue;
		seq_printf(s, "wred-sel[%02u] %08x %08x fields=", i, sel[0],
			   sel[1]);
		for (j = 0; j < L3TE_WRED_SEL_FIELDS; j++)
			seq_printf(s, "%s%u", j ? "," : "",
				   te_wred_sel_field_get(sel, j));
		seq_puts(s, "\n");
	}
}

static void te_dump_dos(struct seq_file *s, struct rtl_te_flow *te)
{
	u32 flood_l3 = 0, flood_l2 = 0;

	rtl_te_dos_flood_get(te, false, &flood_l3);
	rtl_te_dos_flood_get(te, true, &flood_l2);
	seq_printf(s, "dos_enabled=%u dos_flood_l3=%u dos_flood_l2=%u (full dump: ./dos)\n",
		   te->dos_enabled, flood_l3, flood_l2);
}

static int rtl_te_flow_show(struct seq_file *s, void *unused)
{
	struct rtl_te_flow *te = s->private;

	seq_printf(s, "te_flow: dev=%s\n", dev_name(te->dev));
	te_dump_slots(s, te);
	te_dump_policers(s, te);
	te_dump_wred(s, te);
	te_dump_dos(s, te);
	return 0;
}

static int rtl_te_flow_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl_te_flow_show, inode->i_private);
}

static const struct file_operations rtl_te_flow_fops = {
	.owner = THIS_MODULE,
	.open = rtl_te_flow_open,
	.read = seq_read,
	.write = rtl_te_flow_write,
	.llseek = seq_lseek,
	.release = single_release,
};


static int rtl_te_dos_show(struct seq_file *s, void *unused)
{
	struct rtl_te_flow *te = s->private;
	struct rtl_te_dos_evt evt;
	u32 l3[L3TE_DOS_CFG_REGS], l2[L3TE_DOS_L2_CFG_REGS];
	u32 flood = 0, ten = 0, l2ten = 0, raw = 0;
	u64 mask = 0, l2mask = 0;
	unsigned int i;

	seq_printf(s, "dos: dev=%s (type_en = 0x3304[31:16] / 0x1228[15:0])\n",
		   dev_name(te->dev));

	rtl_te_dos_l3_get(te, l3);
	rtl_te_dos_l2_get(te, l2);
	rtl_te_dos_flood_get(te, false, &flood);
	seq_printf(s, "l3_flood=0x%08x count=%u (0x3310[10:0])\n", flood,
		   flood & (u32)L3TE_DOS_FLOOD_CNT_MASK);
	rtl_te_dos_flood_get(te, true, &flood);
	seq_printf(s, "l2_flood=0x%08x count=%u (0x1234[9:0])\n", flood,
		   flood & (u32)L3TE_DOS_L2_FLOOD_CNT_MASK);

	rtl_te_dos_type_en_get(te, false, &ten);
	rtl_te_dos_type_en_get(te, true, &l2ten);
	seq_printf(s, "l3_type_en=0x%04x l2_type_en=0x%04x\n", ten, l2ten);
	for (i = 0; i < RTL_DOS_TYPE_MAX; i++) {
		const struct te_dos_type_desc *d = &te_dos_l3_types[i];
		const struct te_dos_type_desc *d2 = &te_dos_l2_types[i];
		bool l3_trap = !!te_dos_field_extract(l3, TE_DOS_L3_BASE,
						      &d->en);
		bool l2_trap = !!te_dos_field_extract(l2, TE_DOS_L2_BASE,
						      &d2->en);

		seq_printf(s, "  [%2u] %-15s l3_en=%u l3_pol=0x%03x",
			   i, te_dos_type_name[i], !!(ten & BIT(i)),
			   te_dos_field_extract(l3, TE_DOS_L3_BASE, &d->pol));
		if (d->th.width)
			seq_printf(s, " l3_th=0x%04x",
				   te_dos_field_extract(l3, TE_DOS_L3_BASE,
							&d->th));
		seq_printf(s, " l2_en=%u", !!(l2ten & BIT(i)));
		if (d2->th.width)
			seq_printf(s, " l2_th=0x%04x",
				   te_dos_field_extract(l2, TE_DOS_L2_BASE,
							&d2->th));
		seq_printf(s, " l3_act=%s l2_act=%s\n",
			   l3_trap ? "trap" : "drop",
			   l2_trap ? "trap" : "drop");
	}

	rtl_te_dos_port_mask_get(te, false, &mask);
	rtl_te_dos_port_mask_get(te, true, &l2mask);
	seq_printf(s, "l3_port_mask=0x%016llx l2_port_mask=0x%016llx\n", mask,
		   l2mask);

	if (!rtl_te_dos_evt_get(te, false, false, &raw)) {
		te_dos_evt_unpack(raw, &evt);
		seq_printf(s, "l3_trap=0x%08x ldpid=%u cos=%u keep=%u rsvd=%u t0=%u t1..t5=%u,%u,%u,%u,%u\n",
			   raw, evt.ldpid, evt.cos, evt.keep_orig, evt.reserved,
			   evt.t0, evt.timer[0], evt.timer[1], evt.timer[2],
			   evt.timer[3], evt.timer[4]);
	}
	if (!rtl_te_dos_evt_get(te, false, true, &raw)) {
		te_dos_evt_unpack(raw, &evt);
		seq_printf(s, "l3_drop=0x%08x ldpid=%u cos=%u keep=%u rsvd=%u t0=%u t1..t5=%u,%u,%u,%u,%u\n",
			   raw, evt.ldpid, evt.cos, evt.keep_orig, evt.reserved,
			   evt.t0, evt.timer[0], evt.timer[1], evt.timer[2],
			   evt.timer[3], evt.timer[4]);
	}
	if (!rtl_te_dos_evt_get(te, true, false, &raw)) {
		te_dos_evt_unpack(raw, &evt);
		seq_printf(s, "l2_trap=0x%08x ldpid=%u cos=%u keep=%u\n", raw,
			   evt.ldpid, evt.cos, evt.keep_orig);
	}

	seq_puts(s, "l3_raw=");
	for (i = 0; i < L3TE_DOS_CFG_REGS; i++)
		seq_printf(s, " %08x", l3[i]);
	seq_puts(s, "\nl2_raw=");
	for (i = 0; i < L3TE_DOS_L2_CFG_REGS; i++)
		seq_printf(s, " %08x", l2[i]);
	seq_puts(s, "\n");
	return 0;
}

static int te_dos_cmd_raw(struct rtl_te_flow *te, bool l2, char **tok,
			  unsigned int ntok)
{
	u32 img[L3TE_DOS_CFG_REGS];
	unsigned int i, count = l2 ? L3TE_DOS_L2_CFG_REGS : L3TE_DOS_CFG_REGS;

	if (ntok != count)
		return -EINVAL;
	for (i = 0; i < count; i++)
		if (te_parse_u32(tok[i], &img[i]))
			return -EINVAL;

	if (l2)
		return rtl_te_dos_l2_set(te, TE_DOS_L2_MASK_ALL, img);
	return rtl_te_dos_l3_set(te, TE_DOS_L3_MASK_ALL, img);
}

static int te_dos_cmd_blit(struct rtl_te_flow *te, bool l2, char **tok,
			   unsigned int ntok)
{
	u32 img[L3TE_DOS_CFG_REGS];
	unsigned int i, count = l2 ? L3TE_DOS_L2_CFG_REGS : L3TE_DOS_CFG_REGS;
	u64 mask;

	if (ntok != count + 1)
		return -EINVAL;
	if (kstrtou64(tok[0], 0, &mask))
		return -EINVAL;
	for (i = 0; i < count; i++)
		if (te_parse_u32(tok[1 + i], &img[i]))
			return -EINVAL;

	if (l2)
		return rtl_te_dos_l2_set(te, mask, img);
	return rtl_te_dos_l3_set(te, mask, img);
}

static int te_dos_cmd_type(struct rtl_te_flow *te, bool l2, char **tok,
			   unsigned int ntok)
{
	int type;

	if (ntok != 2)
		return -EINVAL;
	type = te_dos_type_index(tok[0]);
	if (type < 0)
		return -EINVAL;
	if (!strcmp(tok[1], "on") || !strcmp(tok[1], "drop"))
		return rtl_te_dos_type_set_action(te, l2, type, true, false);
	if (!strcmp(tok[1], "trap"))
		return rtl_te_dos_type_set_action(te, l2, type, true, true);
	if (!strcmp(tok[1], "off"))
		return rtl_te_dos_type_set_action(te, l2, type, false, false);
	return -EINVAL;
}

/* th and pol-id write the same candidate 9 bit field: U10-c (is that field a
 * threshold or a policy id?) is still open, and keeping one writer for both
 * is what makes the device diff experiment able to tell them apart.
 */
static int te_dos_cmd_value(struct rtl_te_flow *te, bool l2, bool pol_id,
			    char **tok, unsigned int ntok)
{
	u32 value;
	int type;

	if (ntok != 2)
		return -EINVAL;
	type = te_dos_type_index(tok[0]);
	if (type < 0)
		return -EINVAL;
	if (te_parse_u32(tok[1], &value))
		return -EINVAL;

	if (pol_id) {
		if (l2)
			return -EOPNOTSUPP;	/* L2 has no pol_id */
		return rtl_te_dos_pol_id_set(te, type, value);
	}
	return rtl_te_dos_th_set(te, l2, type, value);
}

static int te_dos_cmd_evt(struct rtl_te_flow *te, bool l2, bool drop,
			  char **tok, unsigned int ntok)
{
	/* "reserved" (bit 10) is decoded by the dump but has no writable
	 * mask bit in the OEM setter, so it is not accepted here (RE-9 s.8).
	 */
	static const char *const field_name[] = {
		"ldpid", "cos", "keep", "t0", "t1", "t2", "t3", "t4", "t5",
	};
	static const u32 field_bit[] = {
		L3TE_DOS_EVT_F_LDPID, L3TE_DOS_EVT_F_COS,
		L3TE_DOS_EVT_F_KEEP_ORIG, L3TE_DOS_EVT_F_T0,
		L3TE_DOS_EVT_F_TIMER(0), L3TE_DOS_EVT_F_TIMER(1),
		L3TE_DOS_EVT_F_TIMER(2), L3TE_DOS_EVT_F_TIMER(3),
		L3TE_DOS_EVT_F_TIMER(4),
	};
	struct rtl_te_dos_evt evt = { };
	u32 value;
	unsigned int i;

	if (ntok != 2 || te_parse_u32(tok[1], &value))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(field_name); i++)
		if (!strcmp(tok[0], field_name[i]))
			break;
	if (i == ARRAY_SIZE(field_name))
		return -EINVAL;
	if (i == 0 && value > L3TE_DOS_EVT_LDPID_MAX)
		return -EINVAL;
	if (i == 1 && value > L3TE_DOS_EVT_COS_MAX)
		return -EINVAL;
	if ((i == 2 || i == 3) && value > 1u)
		return -EINVAL;
	if (i >= 4 && value > L3TE_DOS_EVT_TIMER_MAX)
		return -EINVAL;

	switch (i) {
	case 0:
		evt.ldpid = (u8)value;
		break;
	case 1:
		evt.cos = (u8)value;
		break;
	case 2:
		evt.keep_orig = value;
		break;
	case 3:
		evt.t0 = value;
		break;
	default:
		evt.timer[i - 4] = (u8)value;
		break;
	}

	return rtl_te_dos_evt_set(te, l2, drop, field_bit[i], &evt);
}

static ssize_t rtl_te_dos_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct rtl_te_flow *te = seq->private;
	char buf[192];
	char *tok[12];
	char *cur, *cmd;
	unsigned int ntok = 0;
	u64 wide;
	u32 value;
	int ret;

	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	strreplace(buf, '\n', '\0');

	cur = buf;
	cmd = te_next_token(&cur);
	if (!cmd)
		return -EINVAL;
	while (ntok < ARRAY_SIZE(tok)) {
		char *token = te_next_token(&cur);

		if (!token)
			break;
		tok[ntok++] = token;
	}
	if (te_next_token(&cur))
		return -EINVAL;

	if (!strcmp(cmd, "dump")) {
		ret = ntok ? -EINVAL : 0;
	} else if (!strcmp(cmd, "l3-raw")) {
		ret = te_dos_cmd_raw(te, false, tok, ntok);
	} else if (!strcmp(cmd, "l2-raw")) {
		ret = te_dos_cmd_raw(te, true, tok, ntok);
	} else if (!strcmp(cmd, "l3-blit")) {
		ret = te_dos_cmd_blit(te, false, tok, ntok);
	} else if (!strcmp(cmd, "l2-blit")) {
		ret = te_dos_cmd_blit(te, true, tok, ntok);
	} else if (!strcmp(cmd, "type-en") || !strcmp(cmd, "l2-type-en")) {
		bool l2 = cmd[0] == 'l';

		if (ntok != 1 || te_parse_u32(tok[0], &value))
			ret = -EINVAL;
		else
			ret = rtl_te_dos_type_en_set(te, l2, value);
	} else if (!strcmp(cmd, "type")) {
		ret = te_dos_cmd_type(te, false, tok, ntok);
	} else if (!strcmp(cmd, "l2-type")) {
		ret = te_dos_cmd_type(te, true, tok, ntok);
	} else if (!strcmp(cmd, "th")) {
		ret = te_dos_cmd_value(te, false, false, tok, ntok);
	} else if (!strcmp(cmd, "l2-th")) {
		ret = te_dos_cmd_value(te, true, false, tok, ntok);
	} else if (!strcmp(cmd, "pol-id")) {
		ret = te_dos_cmd_value(te, false, true, tok, ntok);
	} else if (!strcmp(cmd, "port-mask") || !strcmp(cmd, "l2-port-mask")) {
		bool l2 = cmd[0] == 'l';

		if (ntok != 1 || kstrtou64(tok[0], 0, &wide))
			ret = -EINVAL;
		else
			ret = rtl_te_dos_port_mask_set(te, l2, wide);
	} else if (!strcmp(cmd, "trap")) {
		ret = te_dos_cmd_evt(te, false, false, tok, ntok);
	} else if (!strcmp(cmd, "drop")) {
		ret = te_dos_cmd_evt(te, false, true, tok, ntok);
	} else if (!strcmp(cmd, "l2-trap")) {
		ret = te_dos_cmd_evt(te, true, false, tok, ntok);
	} else if (!strcmp(cmd, "flood")) {
		ret = ntok ? -EINVAL : 0;	/* the counters are in the dump */
	} else if (!strcmp(cmd, "l2-flood")) {
		ret = ntok ? -EINVAL : 0;
	} else if (!strcmp(cmd, "flood-clear") ||
		   !strcmp(cmd, "l2-flood-clear")) {
		bool l2 = cmd[0] == 'l';

		ret = ntok ? -EINVAL : rtl_te_dos_flood_clear(te, l2);
	} else if (!strcmp(cmd, "flood-unit") ||
		   !strcmp(cmd, "l2-flood-unit")) {
		bool l2 = cmd[0] == 'l';

		if (ntok != 1 || te_parse_u32(tok[0], &value))
			ret = -EINVAL;
		else
			ret = rtl_te_dos_flood_unit_set(te, l2, value);
	} else {
		ret = -EINVAL;
	}

	return ret ? ret : (ssize_t)count;
}

static int rtl_te_dos_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl_te_dos_show, inode->i_private);
}

static const struct file_operations rtl_te_dos_fops = {
	.owner = THIS_MODULE,
	.open = rtl_te_dos_open,
	.read = seq_read,
	.write = rtl_te_dos_write,
	.llseek = seq_lseek,
	.release = single_release,
};


static int te_parse_mac(const char *token, u8 mac[RTL_TE_HOSTPOL_ALEN])
{
	unsigned int i;

	if (strlen(token) != 17)
		return -EINVAL;

	for (i = 0; i < RTL_TE_HOSTPOL_ALEN; i++) {
		int hi = hex_to_bin(token[i * 3]);
		int lo;

		if (hi < 0)
			return -EINVAL;
		lo = hex_to_bin(token[i * 3 + 1]);
		if (lo < 0)
			return -EINVAL;
		if (i < RTL_TE_HOSTPOL_ALEN - 1 && token[i * 3 + 2] != ':')
			return -EINVAL;
		mac[i] = (u8)((hi << 4) | lo);
	}
	return 0;
}

static int rtl_te_hostpol_show(struct seq_file *s, void *unused)
{
	struct rtl_te_flow *te = s->private;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&te->lock, flags);
	seq_printf(s, "hostpol: %u/%u entries, %u/%u vmeters\n",
		   te_hostpol_used(te), RTL_TE_HOSTPOL_ENTRIES,
		   te_hostpol_vmeters_used(te), RTL_TE_HOSTPOL_VMETERS);
	seq_printf(s, "  reserved pol1 0x%02x..0x%02x, flow pol1 0..0x%02x\n",
		   RTL_TE_HOSTPOL_SLOT_BASE, RTL_TE_HOSTPOL_SLOT_END,
		   RTL_TE_FLOW_SLOT_COUNT - 1u);
	for (i = 0; i < RTL_TE_HOSTPOL_ENTRIES; i++) {
		const struct rtl_te_hostpol *h = &te->hostpol[i];
		const u8 *in_vm = &h->vmeter[RTL_TE_HOSTPOL_DIR_INGRESS];
		const u8 *eg_vm = &h->vmeter[RTL_TE_HOSTPOL_DIR_EGRESS];

		if (!h->used)
			continue;
		seq_printf(s, "[%2u] %pM ingress=%ukbps vm=%d pol1=%u (0x%02x)\n",
			   i, h->mac,
			   h->kbps[RTL_TE_HOSTPOL_DIR_INGRESS],
			   *in_vm == RTL_TE_HOSTPOL_VMETER_NONE ? -1 : (int)*in_vm,
			   h->pol1[RTL_TE_HOSTPOL_DIR_INGRESS],
			   h->pol1[RTL_TE_HOSTPOL_DIR_INGRESS]);
		seq_printf(s, "     egress=%ukbps vm=%d pol1=%u (0x%02x)\n",
			   h->kbps[RTL_TE_HOSTPOL_DIR_EGRESS],
			   *eg_vm == RTL_TE_HOSTPOL_VMETER_NONE ? -1 : (int)*eg_vm,
			   h->pol1[RTL_TE_HOSTPOL_DIR_EGRESS],
			   h->pol1[RTL_TE_HOSTPOL_DIR_EGRESS]);
		seq_printf(s, "     rx=%llu tx=%llu last_delta=%llu/%llu\n",
			   h->rx, h->tx,
			   h->last_delta[RTL_TE_HOSTPOL_DIR_INGRESS],
			   h->last_delta[RTL_TE_HOSTPOL_DIR_EGRESS]);
	}
	spin_unlock_irqrestore(&te->lock, flags);
	return 0;
}

static int te_hostpol_cmd_add(struct rtl_te_flow *te, char **tok,
			      unsigned int ntok)
{
	u8 mac[RTL_TE_HOSTPOL_ALEN];
	u32 ingress, egress;
	u32 ivm = RTL_TE_HOSTPOL_VMETER_AUTO;
	u32 egm = RTL_TE_HOSTPOL_VMETER_AUTO;

	if (ntok != 3 && ntok != 5)
		return -EINVAL;
	if (te_parse_mac(tok[0], mac))
		return -EINVAL;
	if (te_parse_u32(tok[1], &ingress) || te_parse_u32(tok[2], &egress))
		return -EINVAL;
	if (ntok == 5 &&
	    (te_parse_u32(tok[3], &ivm) || te_parse_u32(tok[4], &egm)))
		return -EINVAL;
	if (ivm > U8_MAX || egm > U8_MAX)
		return -EINVAL;

	return rtl_te_hostpol_add_vmeter(te, mac, ingress, egress, (u8)ivm,
					 (u8)egm);
}

static int te_hostpol_cmd_del(struct rtl_te_flow *te, char **tok,
			      unsigned int ntok)
{
	u8 mac[RTL_TE_HOSTPOL_ALEN];

	if (ntok != 1 || te_parse_mac(tok[0], mac))
		return -EINVAL;
	return rtl_te_hostpol_del(te, mac);
}

static int te_hostpol_cmd_lookup(struct rtl_te_flow *te, char **tok,
				 unsigned int ntok)
{
	u8 mac[RTL_TE_HOSTPOL_ALEN];
	bool flow_limited = false, egress_limited = false;
	u8 flow = 0, egress = 0;
	int ret;

	if (ntok != 1 || te_parse_mac(tok[0], mac))
		return -EINVAL;

	ret = rtl_te_hostpol_lookup_ex(te, mac, RTL_TE_HOSTPOL_DIR_INGRESS,
				       &flow, &flow_limited);
	if (ret)
		return ret;
	ret = rtl_te_hostpol_lookup_ex(te, mac, RTL_TE_HOSTPOL_DIR_EGRESS,
				       &egress, &egress_limited);
	if (ret)
		return ret;

	dev_info(te->dev,
		 "hostpol %pM flow_pol1=%u (rate_limited=%u) egress_pol1=%u (rate_limited=%u)\n",
		 mac, flow, flow_limited, egress, egress_limited);
	return 0;
}

static int te_hostpol_cmd_mib(struct rtl_te_flow *te, char **tok,
			      unsigned int ntok)
{
	struct rtl_te_hostpol_mib raw[RTL_TE_HOSTPOL_DIRS];
	unsigned long flags;
	u32 entry;
	u64 rx = 0, tx = 0;
	int ret;

	if (ntok != 1 || te_parse_u32(tok[0], &entry))
		return -EINVAL;
	if (entry >= RTL_TE_HOSTPOL_ENTRIES)
		return -EINVAL;

	/*
	 * Read-clear counters cannot be sampled separately via raw + get
	 * without draining the interval between calls.
	 */
	spin_lock_irqsave(&te->lock, flags);
	ret = te_hostpol_mib_sample_locked(te, (u8)entry, &rx, &tx, raw);
	spin_unlock_irqrestore(&te->lock, flags);
	if (ret)
		return ret;

	dev_info(te->dev,
		 "hostpol[%u] raw ingress bytes=%llu/%llu pkt=%llu/%llu | egress bytes=%llu/%llu pkt=%llu/%llu\n",
		 entry,
		 raw[RTL_TE_HOSTPOL_DIR_INGRESS].cnt[4],
		 raw[RTL_TE_HOSTPOL_DIR_INGRESS].cnt[5],
		 raw[RTL_TE_HOSTPOL_DIR_INGRESS].cnt[0],
		 raw[RTL_TE_HOSTPOL_DIR_INGRESS].cnt[1],
		 raw[RTL_TE_HOSTPOL_DIR_EGRESS].cnt[4],
		 raw[RTL_TE_HOSTPOL_DIR_EGRESS].cnt[5],
		 raw[RTL_TE_HOSTPOL_DIR_EGRESS].cnt[0],
		 raw[RTL_TE_HOSTPOL_DIR_EGRESS].cnt[1]);
	dev_info(te->dev, "hostpol[%u] accumulated rx=%llu tx=%llu\n",
		 entry, rx, tx);
	return 0;
}

static int te_hostpol_cmd_mib_clear(struct rtl_te_flow *te, char **tok,
				    unsigned int ntok)
{
	u32 entry;

	if (ntok != 1 || te_parse_u32(tok[0], &entry))
		return -EINVAL;
	if (entry >= RTL_TE_HOSTPOL_ENTRIES)
		return -EINVAL;
	return rtl_te_hostpol_mib_clear(te, (u8)entry);
}

static ssize_t rtl_te_hostpol_write(struct file *file,
				    const char __user *ubuf, size_t count,
				    loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct rtl_te_flow *te = seq->private;
	char buf[160];
	char *tok[10];
	char *cur, *cmd;
	unsigned int ntok = 0;
	int ret;

	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	strreplace(buf, '\n', '\0');

	cur = buf;
	cmd = te_next_token(&cur);
	if (!cmd)
		return -EINVAL;
	while (ntok < ARRAY_SIZE(tok)) {
		char *token = te_next_token(&cur);

		if (!token)
			break;
		tok[ntok++] = token;
	}
	if (te_next_token(&cur))
		return -EINVAL;

	if (!strcmp(cmd, "dump"))
		ret = ntok ? -EINVAL : 0;
	else if (!strcmp(cmd, "add"))
		ret = te_hostpol_cmd_add(te, tok, ntok);
	else if (!strcmp(cmd, "del"))
		ret = te_hostpol_cmd_del(te, tok, ntok);
	else if (!strcmp(cmd, "lookup"))
		ret = te_hostpol_cmd_lookup(te, tok, ntok);
	else if (!strcmp(cmd, "mib"))
		ret = te_hostpol_cmd_mib(te, tok, ntok);
	else if (!strcmp(cmd, "mib-clear"))
		ret = te_hostpol_cmd_mib_clear(te, tok, ntok);
	else
		ret = -EINVAL;

	return ret ? ret : (ssize_t)count;
}

static int rtl_te_hostpol_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl_te_hostpol_show, inode->i_private);
}

static const struct file_operations rtl_te_hostpol_fops = {
	.owner = THIS_MODULE,
	.open = rtl_te_hostpol_open,
	.read = seq_read,
	.write = rtl_te_hostpol_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void te_flow_debugfs_init(struct rtl_te_flow *te)
{
	struct dentry *dir;

	dir = debugfs_create_dir("rtl9617c_te", NULL);
	if (IS_ERR_OR_NULL(dir))
		return;
	te->debugfs = dir;
	debugfs_create_file("te_flow", 0600, dir, te, &rtl_te_flow_fops);
	debugfs_create_file("dos", 0600, dir, te, &rtl_te_dos_fops);
	debugfs_create_file("hostpol", 0600, dir, te, &rtl_te_hostpol_fops);
}

static void te_flow_debugfs_exit(struct rtl_te_flow *te)
{
	debugfs_remove_recursive(te->debugfs);
	te->debugfs = NULL;
}

#else /* !CONFIG_DEBUG_FS */

static void te_flow_debugfs_init(struct rtl_te_flow *te)
{
}

static void te_flow_debugfs_exit(struct rtl_te_flow *te)
{
}

#endif /* CONFIG_DEBUG_FS */

/* ---------------------------------------------------------------------- */
/* init / exit                                                             */
/* ---------------------------------------------------------------------- */

int rtl_te_flow_init(struct rtl9617c_ni *ni)
{
	struct rtl_te_flow *te = &ni->te;
	u32 glb;

	memset(te, 0, sizeof(*te));
	spin_lock_init(&te->lock);
	spin_lock_init(&te->dos_l3_lock);
	spin_lock_init(&te->dos_l2_lock);
	/* Reserve the whole host policing window in the shared POL1 bitmap. */
	bitmap_set(te->flow_slots, RTL_TE_HOSTPOL_SLOT_BASE,
		   RTL_TE_HOSTPOL_SLOT_END - RTL_TE_HOSTPOL_SLOT_BASE + 1u);
	te->dev = ni->dev;
	te->regs = ni->regs;

	/* Record, do not program.  NE 0x9004 bit25 is global hardware state
	 * and the driver has no boot-time evidence for changing it, so the
	 * default boot leaves this register exactly as the bootloader set it:
	 * the 64 slot depth is enabled on the first group policer allocation
	 * (te_group_depth_ready()) and never otherwise.
	 */
	glb = te_read(te, L3TE_TE_GLB_CTRL);
	te->group64_ready = !!(glb & L3TE_TE_GLB_POL_GRP_64);
	te->policer_ready = (glb & L3TE_TE_GLB_POLICER_EN) &&
		((te_read(te, L3TE_POL_GLB_CFG) & L3TE_POL_GLB_CFG_MASK) ==
		 L3TE_POL_GLB_CFG_VAL) &&
		((te_read(te, L3TE_FE_POL_GLB_CFG) & L3TE_FE_POL_GLB_EN) ==
		 L3TE_FE_POL_GLB_EN);
	dev_dbg(te->dev, "L3TE group policer table depth is %u slots\n",
		te->group64_ready ? 64 : 8);

	te_flow_debugfs_init(te);
	return 0;
}

void rtl_te_flow_debugfs_exit(struct rtl_te_flow *te)
{
	te_flow_debugfs_exit(te);
}

void rtl_te_flow_exit(struct rtl9617c_ni *ni)
{
	struct rtl_te_flow *te = &ni->te;
	unsigned int flow, group, host;

	te_flow_debugfs_exit(te);
	/* POL1 0xA0..0xFF is reserved for host policing and is always marked,
	 * so only the flow half of the bitmap says anything about leaks.
	 */
	flow = bitmap_weight(te->flow_slots, RTL_TE_FLOW_SLOT_COUNT);
	group = bitmap_weight(te->group_slots, L3TE_POL_GRP_SLOTS);
	host = te_hostpol_used(te);
	if (flow || group || host)
		dev_warn(te->dev,
			 "L3TE policer slots still allocated at exit (flow=%u group=%u host=%u vmeter=%u)\n",
			 flow, group, host, te_hostpol_vmeters_used(te));
}
