// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RTL9617C L3FE hardware multicast table manager.
 *
 * Three-level indirection recovered from ca-ne.ko (spec
 * phase0-re3-dualhdr-mc-napt6 §4):
 *
 *   software MCGID 0..1023          aal_sw_mcgid_alloc @ .text+0x4ee70
 *     -> MCE index table            256 x 2 words, table id 14, NE+0xaa64
 *                                  aal_hw_mcgid_new  @ .text+0x4fcc0
 *        -> ARB MC FIB              64 x 3 words, table id 15, NE+0x1634
 *                                  aal_arb_mc_fib_set @ .text+0x3ff80
 *
 * The MCE entry payload is the 64-bit bitmap of ARB MC FIB indices that
 * belong to the group: aal_hw_mcgid_free @ .text+0x4fb10 reads the entry,
 * walks 1 << i for i < 0x40, deletes every member entry and only then
 * deletes the MCE index.  The reverse order is mandatory; deleting the
 * MCE first would leave dangling member entries behind.
 *
 * The 76-bit ARB MC FIB field layout across all three words has been decoded
 * from ca-ne.ko (aal_arb_mc_fib_set @ 0x3ff80, _aal_arb_mc_fib_action_add @
 * 0x4f020) and cli-drv.ko (__cli_aal_arb_mc_fib_cfg_*_set), and both MCE
 * (0xaa64..0xaa6c) and ARB MC FIB (0x1634..0x1640) write/read-back cycles
 * have been verified on RTL9617C silicon.  Switchdev MDB events program
 * per-port member FIBs (ldpid, cos_cmd, wan_dst) and the MCE bitmap when
 * mcast_hw=1, while debugfs "set"/"member" still allow raw word overrides.
 */
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <net/switchdev.h>

#include "fe_priv.h"
#include "fe_mcast.h"

static bool mcast_hw;
module_param(mcast_hw, bool, 0644);
MODULE_PARM_DESC(mcast_hw,
		 "Program hardware MCE and ARB MC FIB entries on switchdev MDB events (default 0)");

/* --- mcast-pool-core: begin (tests/mcast-pool.py extracts this block) --- */

#ifndef RTL_FE_MCAST_PORT_NONE
#define RTL_FE_MCAST_PORT_NONE		0xff
#endif
#ifndef RTL_FE_MCAST_MCE_WORDS
#define RTL_FE_MCAST_MCE_WORDS		2
#endif
#ifndef RTL_FE_MCAST_FIB_WORDS
#define RTL_FE_MCAST_FIB_WORDS		3
#endif
#ifndef RTL_FE_MCAST_BITS_PER_BYTE
#define RTL_FE_MCAST_BITS_PER_BYTE	8
#endif
#define RTL_FE_MCAST_WORD_BYTES		4
#define RTL_FE_MCAST_WORD_BITS		32

static bool rtl_fe_mcast_port_test(const u8 ports[RTL_FE_MCAST_MASK_BYTES],
				   unsigned int port)
{
	return !!(ports[port / RTL_FE_MCAST_BITS_PER_BYTE] &
		  BIT(port % RTL_FE_MCAST_BITS_PER_BYTE));
}

static int rtl_fe_mcast_wait(struct rtl_fe *fe, unsigned int reg)
{
	unsigned int retry;

	/* aal_arb_mc_fib_set() polls bit31 of the command register up to
	 * 10000 times before it gives up.
	 */
	for (retry = 0; retry < RTL_FE_MCAST_POLL_COUNT; retry++) {
		if (!(rtl_fe_read(fe, reg) & RTL_NI_INDIRECT_BUSY))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int rtl_fe_mcast_table_read(struct rtl_fe *fe, unsigned int cmd_reg,
				   unsigned int data_reg, unsigned int index,
				   unsigned int index_mask, u32 *words,
				   unsigned int count)
{
	unsigned int i;
	int ret;

	if (index & ~index_mask)
		return -EINVAL;
	rtl_fe_write(fe, cmd_reg, RTL_NI_INDIRECT_ACCESS | index);
	ret = rtl_fe_mcast_wait(fe, cmd_reg);
	if (ret)
		return ret;
	for (i = 0; i < count; i++)
		words[i] = rtl_fe_read(fe,
				       data_reg - RTL_FE_MCAST_WORD_BYTES * i);
	return 0;
}

static int rtl_fe_mcast_table_write(struct rtl_fe *fe, unsigned int cmd_reg,
				    unsigned int data_reg, unsigned int index,
				    unsigned int index_mask, const u32 *words,
				    unsigned int count)
{
	unsigned int i;

	if (index & ~index_mask)
		return -EINVAL;
	for (i = 0; i < count; i++)
		rtl_fe_write(fe, data_reg - RTL_FE_MCAST_WORD_BYTES * i,
			     words[i]);
	rtl_fe_write(fe, cmd_reg, RTL_NI_INDIRECT_ACCESS |
		     RTL_NI_INDIRECT_WRITE | index);
	return rtl_fe_mcast_wait(fe, cmd_reg);
}

/* Every table write is followed by a read-back and a bit-for-bit compare.
 * A mismatch means the hardware did not store what was written; the caller
 * gets -EIO and must treat the entry as not programmed.
 */
static int rtl_fe_mcast_write_verify(struct rtl_fe *fe, unsigned int cmd_reg,
				     unsigned int data_reg,
				     unsigned int index,
				     unsigned int index_mask,
				     const u32 *words, unsigned int count)
{
	u32 readback[RTL_FE_MCAST_FIB_WORDS];
	unsigned int i;
	int ret;

	ret = rtl_fe_mcast_table_write(fe, cmd_reg, data_reg, index,
				       index_mask, words, count);
	if (ret)
		return ret;
	ret = rtl_fe_mcast_table_read(fe, cmd_reg, data_reg, index,
				      index_mask, readback, count);
	if (ret)
		return ret;
	for (i = 0; i < count; i++)
		if (readback[i] != words[i])
			return -EIO;
	return 0;
}

static int rtl_fe_mcast_mce_read(struct rtl_fe *fe, unsigned int index,
				 u32 words[RTL_FE_MCAST_MCE_WORDS])
{
	return rtl_fe_mcast_table_read(fe, RTL_FE_MCE_CMD, RTL_FE_MCE_DATA0,
				       index, RTL_FE_MCE_ENTRIES - 1, words,
				       RTL_FE_MCAST_MCE_WORDS);
}

static int rtl_fe_mcast_mce_write(struct rtl_fe *fe, unsigned int index,
				  const u32 words[RTL_FE_MCAST_MCE_WORDS])
{
	return rtl_fe_mcast_write_verify(fe, RTL_FE_MCE_CMD,
					 RTL_FE_MCE_DATA0, index,
					 RTL_FE_MCE_ENTRIES - 1, words,
					 RTL_FE_MCAST_MCE_WORDS);
}

static int rtl_fe_mcast_fib_write(struct rtl_fe *fe, unsigned int index,
				  const u32 words[RTL_FE_MCAST_FIB_WORDS])
{
	unsigned int i;
	int ret;

	ret = rtl_fe_mcast_write_verify(fe, RTL_FE_ARB_MC_FIB_CMD,
					RTL_FE_ARB_MC_FIB_DATA0, index,
					RTL_FE_ARB_MC_FIB_ENTRIES - 1,
					words, RTL_FE_MCAST_FIB_WORDS);
	if (ret)
		return ret;
	for (i = 0; i < RTL_FE_MCAST_FIB_WORDS; i++)
		fe->mcast->fib[index].words[i] = words[i];
	return 0;
}

/* Deleting an ARB MC FIB member clears all three words and releases the slot;
 * there is no separate valid bit in the 76-bit ARB MC FIB entry, membership
 * is governed by the 64-bit MCE bitmap.
 */
static int rtl_fe_mcast_fib_delete(struct rtl_fe *fe, unsigned int index)
{
	static const u32 zero[RTL_FE_MCAST_FIB_WORDS];
	struct rtl_fe_fib *fib = &fe->mcast->fib[index];
	unsigned int i;
	int ret;

	ret = rtl_fe_mcast_write_verify(fe, RTL_FE_ARB_MC_FIB_CMD,
					RTL_FE_ARB_MC_FIB_DATA0, index,
					RTL_FE_ARB_MC_FIB_ENTRIES - 1,
					zero, RTL_FE_MCAST_FIB_WORDS);
	if (ret)
		return ret;
	fib->used = false;
	fib->mcgid = RTL_FE_MCAST_NONE;
	fib->port = RTL_FE_MCAST_PORT_NONE;
	for (i = 0; i < RTL_FE_MCAST_FIB_WORDS; i++)
		fib->words[i] = 0;
	return 0;
}

static int rtl_fe_mcast_mce_delete(struct rtl_fe *fe, unsigned int index)
{
	static const u32 zero[RTL_FE_MCAST_MCE_WORDS];
	struct rtl_fe_mce *mce = &fe->mcast->mce[index];
	int ret;

	ret = rtl_fe_mcast_write_verify(fe, RTL_FE_MCE_CMD, RTL_FE_MCE_DATA0,
					index, RTL_FE_MCE_ENTRIES - 1,
					zero, RTL_FE_MCAST_MCE_WORDS);
	if (ret)
		return ret;
	mce->used = false;
	mce->mcgid = RTL_FE_MCAST_NONE;
	return 0;
}

static u64 rtl_fe_mcast_members(const struct rtl_fe_mcast *m, u16 mcgid)
{
	u64 members = 0;
	unsigned int i;

	for (i = 0; i < RTL_FE_MCAST_FIBS; i++)
		if (m->fib[i].used && m->fib[i].mcgid == mcgid)
			members |= BIT_ULL(i);
	return members;
}

static int rtl_fe_mcast_mce_write_members(struct rtl_fe *fe,
					  unsigned int index, u64 members)
{
	u32 words[RTL_FE_MCAST_MCE_WORDS];

	words[0] = lower_32_bits(members);
	words[1] = upper_32_bits(members);
	return rtl_fe_mcast_mce_write(fe, index, words);
}

static int rtl_fe_mcast_mce_slot_alloc(struct rtl_fe *fe, u16 mcgid,
				       unsigned int *index)
{
	struct rtl_fe_mcast *m = fe->mcast;
	unsigned int i;

	for (i = 0; i < RTL_FE_MCAST_MCES; i++) {
		if (m->mce[i].used)
			continue;
		m->mce[i].used = true;
		m->mce[i].mcgid = mcgid;
		*index = i;
		return 0;
	}
	return -ENOSPC;
}

static int rtl_fe_mcast_fib_slot_alloc(struct rtl_fe *fe, u16 mcgid,
				       unsigned int port, unsigned int *index)
{
	struct rtl_fe_mcast *m = fe->mcast;
	unsigned int i;

	for (i = 0; i < RTL_FE_MCAST_FIBS; i++) {
		if (m->fib[i].used)
			continue;
		m->fib[i].used = true;
		m->fib[i].mcgid = mcgid;
		m->fib[i].port = port;
		*index = i;
		return 0;
	}
	return -ENOSPC;
}

static int rtl_fe_mcast_alloc_locked(struct rtl_fe *fe, u16 *mcgid)
{
	struct rtl_fe_mcast *m = fe->mcast;
	unsigned int index;

	index = find_first_zero_bit(m->mcgids, RTL_FE_MCAST_MCGIDS);
	if (index >= RTL_FE_MCAST_MCGIDS)
		return -ENOSPC;
	__set_bit(index, m->mcgids);
	m->mcgid[index].users = 1;
	m->mcgid[index].hw_index = RTL_FE_MCAST_NONE;
	m->mcgid[index].prev_index = RTL_FE_MCAST_NONE;
	*mcgid = index;
	return 0;
}

/* Reverse-order teardown of one hardware binding: every ARB MC FIB member
 * first, then the MCE index, then cur/prev = 0xffff.  The member list is
 * read back from the hardware MCE entry instead of trusting software state,
 * exactly like the OEM free path.  A partially completed teardown is
 * idempotent: slots that were already released are skipped.
 */
static int rtl_fe_mcast_binding_free(struct rtl_fe *fe, u16 mcgid,
				     unsigned int index)
{
	u32 entry[RTL_FE_MCAST_MCE_WORDS];
	u64 members;
	unsigned int i;
	int ret, err = 0;

	ret = rtl_fe_mcast_mce_read(fe, index, entry);
	if (ret)
		return ret;
	members = (u64)entry[0] | ((u64)entry[1] << RTL_FE_MCAST_WORD_BITS);

	for (i = 0; i < RTL_FE_MCAST_FIBS; i++) {
		bool owned = fe->mcast->fib[i].used &&
			     fe->mcast->fib[i].mcgid == mcgid;
		bool listed = !!(members & BIT_ULL(i));

		if (!owned) {
			/* A listed bit whose slot is still allocated belongs to
			 * another group: the software and hardware views have
			 * diverged.  A listed bit whose slot is already free was
			 * cleaned up by an earlier interrupted free().
			 */
			if (listed && fe->mcast->fib[i].used)
				err = -EIO;
			continue;
		}
		ret = rtl_fe_mcast_fib_delete(fe, i);
		if (ret && !err)
			err = ret;
	}
	ret = rtl_fe_mcast_mce_delete(fe, index);
	if (ret && !err)
		err = ret;
	if (!err) {
		fe->mcast->mcgid[mcgid].prev_index = RTL_FE_MCAST_NONE;
		fe->mcast->mcgid[mcgid].hw_index = RTL_FE_MCAST_NONE;
	}
	return err;
}

static int rtl_fe_mcast_free_locked(struct rtl_fe *fe, u16 mcgid)
{
	struct rtl_fe_mcgid *entry;
	int ret;

	if (mcgid >= RTL_FE_MCAST_MCGIDS || !test_bit(mcgid, fe->mcast->mcgids))
		return -EINVAL;
	entry = &fe->mcast->mcgid[mcgid];
	if (!entry->users)
		return -EINVAL;
	if (--entry->users)
		return 0;
	if (entry->hw_index != RTL_FE_MCAST_NONE) {
		ret = rtl_fe_mcast_binding_free(fe, mcgid, entry->hw_index);
		if (ret) {
			/* Keep the reference so the caller can retry. */
			entry->users = 1;
			return ret;
		}
	}
	__clear_bit(mcgid, fe->mcast->mcgids);
	return 0;
}

static unsigned int
rtl_fe_mcast_mask_weight(const u8 ports[RTL_FE_MCAST_MASK_BYTES])
{
	unsigned int port, weight = 0;

	for (port = 0; port < RTL_FE_MCAST_PORTS; port++)
		if (rtl_fe_mcast_port_test(ports, port))
			weight++;
	return weight;
}

/* Abort path for a failed program: the MCE entry is detached and every
 * member slot owned by the group is released.  The binding itself stays
 * allocated so the caller can retry.
 */
static void rtl_fe_mcast_group_clear(struct rtl_fe *fe, u16 mcgid)
{
	struct rtl_fe_mcast *m = fe->mcast;
	unsigned int i;

	if (m->mcgid[mcgid].hw_index != RTL_FE_MCAST_NONE)
		rtl_fe_mcast_mce_write_members(fe, m->mcgid[mcgid].hw_index, 0);
	for (i = 0; i < RTL_FE_MCAST_FIBS; i++) {
		if (!m->fib[i].used || m->fib[i].mcgid != mcgid)
			continue;
		if (rtl_fe_mcast_fib_delete(fe, i)) {
			m->fib[i].used = false;
			m->fib[i].mcgid = RTL_FE_MCAST_NONE;
			m->fib[i].port = RTL_FE_MCAST_PORT_NONE;
		}
	}
}

/* Rebuild one group: bind an MCE slot on first use, detach the old member
 * bitmap, release the old member entries, write the new member entries and
 * finally publish the new bitmap.  The live MCE entry never references an
 * entry that has already been cleared.
 */
static int rtl_fe_mcast_program_locked(struct rtl_fe *fe, u16 mcgid,
				       const u8 ports[RTL_FE_MCAST_MASK_BYTES],
				       const u32 *words)
{
	static const u32 zero[RTL_FE_MCAST_FIB_WORDS];
	struct rtl_fe_mcast *m = fe->mcast;
	struct rtl_fe_mcgid *entry;
	unsigned int i, index, used_by_others = 0;
	u64 members;
	int ret;

	if (mcgid >= RTL_FE_MCAST_MCGIDS || !test_bit(mcgid, m->mcgids))
		return -EINVAL;
	entry = &m->mcgid[mcgid];
	if (!entry->users)
		return -EINVAL;

	/* Capacity check before anything is disturbed: a group that cannot
	 * fit must leave the live members untouched.
	 */
	for (i = 0; i < RTL_FE_MCAST_FIBS; i++)
		if (m->fib[i].used && m->fib[i].mcgid != mcgid)
			used_by_others++;
	if (used_by_others + rtl_fe_mcast_mask_weight(ports) >
	    RTL_FE_MCAST_FIBS)
		return -ENOSPC;

	if (entry->hw_index == RTL_FE_MCAST_NONE) {
		ret = rtl_fe_mcast_mce_slot_alloc(fe, mcgid, &index);
		if (ret)
			return ret;
		entry->hw_index = index;
	}

	members = rtl_fe_mcast_members(m, mcgid);
	if (members) {
		ret = rtl_fe_mcast_mce_write_members(fe, entry->hw_index, 0);
		if (ret)
			return ret;
	}
	for (i = 0; i < RTL_FE_MCAST_FIBS; i++) {
		if (!m->fib[i].used || m->fib[i].mcgid != mcgid)
			continue;
		ret = rtl_fe_mcast_fib_delete(fe, i);
		if (ret)
			return ret;
	}
	for (i = 0; i < RTL_FE_MCAST_PORTS; i++) {
		if (!rtl_fe_mcast_port_test(ports, i))
			continue;
		ret = rtl_fe_mcast_fib_slot_alloc(fe, mcgid, i, &index);
		if (ret)
			goto abort;
		ret = rtl_fe_mcast_fib_write(fe, index, words ? words : zero);
		if (ret)
			goto abort;
	}
	ret = rtl_fe_mcast_mce_write_members(fe, entry->hw_index,
					     rtl_fe_mcast_members(m, mcgid));
	if (ret)
		goto abort;
	return 0;

abort:
	rtl_fe_mcast_group_clear(fe, mcgid);
	return ret;
}

static int rtl_fe_mcast_member_set_locked(struct rtl_fe *fe, u16 mcgid,
					  int index,
					  const u32 words[RTL_FE_MCAST_FIB_WORDS])
{
	struct rtl_fe_mcast *m = fe->mcast;
	struct rtl_fe_mcgid *entry;
	unsigned int i;
	bool fresh;
	int ret;

	if (mcgid >= RTL_FE_MCAST_MCGIDS || !test_bit(mcgid, m->mcgids))
		return -EINVAL;
	entry = &m->mcgid[mcgid];
	if (!entry->users)
		return -EINVAL;

	if (index < 0) {
		/* "set" without an explicit slot: the group must own either no
		 * member (a free slot is picked) or exactly one member.
		 */
		int found = -1;

		for (i = 0; i < RTL_FE_MCAST_FIBS; i++) {
			if (!m->fib[i].used || m->fib[i].mcgid != mcgid)
				continue;
			if (found >= 0)
				return -EINVAL;
			found = i;
		}
		if (found >= 0) {
			index = found;
		} else {
			for (i = 0; i < RTL_FE_MCAST_FIBS; i++) {
				if (!m->fib[i].used) {
					index = i;
					break;
				}
			}
			if (i == RTL_FE_MCAST_FIBS)
				return -ENOSPC;
		}
	} else if ((unsigned int)index >= RTL_FE_MCAST_FIBS) {
		return -EINVAL;
	}

	if (m->fib[index].used && m->fib[index].mcgid != mcgid)
		return -EBUSY;

	if (entry->hw_index == RTL_FE_MCAST_NONE) {
		ret = rtl_fe_mcast_mce_slot_alloc(fe, mcgid, &i);
		if (ret)
			return ret;
		entry->hw_index = i;
	}

	fresh = !m->fib[index].used;
	if (fresh) {
		m->fib[index].used = true;
		m->fib[index].mcgid = mcgid;
		m->fib[index].port = RTL_FE_MCAST_PORT_NONE;
	}
	ret = rtl_fe_mcast_fib_write(fe, index, words);
	if (ret) {
		if (fresh) {
			m->fib[index].used = false;
			m->fib[index].mcgid = RTL_FE_MCAST_NONE;
		}
		return ret;
	}
	return rtl_fe_mcast_mce_write_members(fe, entry->hw_index,
					      rtl_fe_mcast_members(m, mcgid));
}

/* --- mcast-pool-core: end --- */

#define RTL_FE_MCAST_CMD_BUF_LEN		128

/* ARB MC FIB word0 (0x1640) field shifts and masks. */
#define RTL_FE_ARB_W0_C_VLAN_CMD_SHIFT		0
#define RTL_FE_ARB_W0_C_VLAN_CMD_MASK		0x3u
#define RTL_FE_ARB_W0_C_VID_SHIFT		2
#define RTL_FE_ARB_W0_C_VID_MASK		0xfffu
#define RTL_FE_ARB_W0_C_TPID_CMD_SHIFT		14
#define RTL_FE_ARB_W0_C_TPID_CMD_MASK		0x1u
#define RTL_FE_ARB_W0_C_TPID_SEL_SHIFT		15
#define RTL_FE_ARB_W0_C_TPID_SEL_MASK		0x1u
#define RTL_FE_ARB_W0_C_DOT1P_CMD_SHIFT		16
#define RTL_FE_ARB_W0_C_DOT1P_CMD_MASK		0x3u
#define RTL_FE_ARB_W0_C_DOT1P_SHIFT		18
#define RTL_FE_ARB_W0_C_DOT1P_MASK		0x7u
#define RTL_FE_ARB_W0_C_DEI_CMD_SHIFT		21
#define RTL_FE_ARB_W0_C_DEI_CMD_MASK		0x1u
#define RTL_FE_ARB_W0_C_DEI_SHIFT		22
#define RTL_FE_ARB_W0_C_DEI_MASK		0x1u
#define RTL_FE_ARB_W0_POL_ID_SHIFT		23
#define RTL_FE_ARB_W0_POL_ID_MASK		0x1ffu

/* ARB MC FIB word1 (0x163c) field shifts and masks. */
#define RTL_FE_ARB_W1_S_VLAN_CMD_SHIFT		0
#define RTL_FE_ARB_W1_S_VLAN_CMD_MASK		0x3u
#define RTL_FE_ARB_W1_S_VID_SHIFT		2
#define RTL_FE_ARB_W1_S_VID_MASK		0xfffu
#define RTL_FE_ARB_W1_S_TPID_CMD_SHIFT		14
#define RTL_FE_ARB_W1_S_TPID_CMD_MASK		0x1u
#define RTL_FE_ARB_W1_S_TPID_SEL_SHIFT		15
#define RTL_FE_ARB_W1_S_TPID_SEL_MASK		0x3u
#define RTL_FE_ARB_W1_DOT1P_CMD_SHIFT		17
#define RTL_FE_ARB_W1_DOT1P_CMD_MASK		0x3u
#define RTL_FE_ARB_W1_DOT1P_SHIFT		19
#define RTL_FE_ARB_W1_DOT1P_MASK		0x7u
#define RTL_FE_ARB_W1_DEI_CMD_SHIFT		22
#define RTL_FE_ARB_W1_DEI_CMD_MASK		0x1u
#define RTL_FE_ARB_W1_DEI_SHIFT			23
#define RTL_FE_ARB_W1_DEI_MASK			0x1u
#define RTL_FE_ARB_W1_LDPID_SHIFT		24
#define RTL_FE_ARB_W1_LDPID_MASK		0x3fu
#define RTL_FE_ARB_W1_COS_CMD_SHIFT		30
#define RTL_FE_ARB_W1_COS_CMD_MASK		0x1u
#define RTL_FE_ARB_W1_WAN_DST_SHIFT		31
#define RTL_FE_ARB_W1_WAN_DST_MASK		0x1u

/* ARB MC FIB word2 (0x1638) field shifts and masks. */
#define RTL_FE_ARB_W2_PERMIT_SPID_SHIFT		0
#define RTL_FE_ARB_W2_PERMIT_SPID_MASK		0x1u
#define RTL_FE_ARB_W2_MAC_SA_CMD_SHIFT		1
#define RTL_FE_ARB_W2_MAC_SA_CMD_MASK		0x1u
#define RTL_FE_ARB_W2_MAC_SA_SEL_SHIFT		2
#define RTL_FE_ARB_W2_MAC_SA_SEL_MASK		0x7u
#define RTL_FE_ARB_W2_COS_SHIFT			5
#define RTL_FE_ARB_W2_COS_MASK			0x7u
#define RTL_FE_ARB_W2_TTL_ZERO_SHIFT		8
#define RTL_FE_ARB_W2_TTL_ZERO_MASK		0x3u
#define RTL_FE_ARB_W2_TTL_DEC_SHIFT		10
#define RTL_FE_ARB_W2_TTL_DEC_MASK		0x1u
#define RTL_FE_ARB_W2_KEEP_O_LSPID_SHIFT	11
#define RTL_FE_ARB_W2_KEEP_O_LSPID_MASK		0x1u

/* --- mcast-fib-codec: begin (tests/mcast-pool.py extracts this block) --- */

void rtl_fe_mcast_fib_pack(const struct rtl_fe_mcast_fib_cfg *cfg,
			   u32 words[RTL_FE_MCAST_FIB_WORDS])
{
	words[0] = ((u32)(cfg->c_vlan_cmd & RTL_FE_ARB_W0_C_VLAN_CMD_MASK) <<
		    RTL_FE_ARB_W0_C_VLAN_CMD_SHIFT) |
		   ((u32)(cfg->c_vid & RTL_FE_ARB_W0_C_VID_MASK) <<
		    RTL_FE_ARB_W0_C_VID_SHIFT) |
		   ((u32)(cfg->c_tpid_cmd & RTL_FE_ARB_W0_C_TPID_CMD_MASK) <<
		    RTL_FE_ARB_W0_C_TPID_CMD_SHIFT) |
		   ((u32)(cfg->c_tpid_sel & RTL_FE_ARB_W0_C_TPID_SEL_MASK) <<
		    RTL_FE_ARB_W0_C_TPID_SEL_SHIFT) |
		   ((u32)(cfg->c_dot1p_cmd & RTL_FE_ARB_W0_C_DOT1P_CMD_MASK) <<
		    RTL_FE_ARB_W0_C_DOT1P_CMD_SHIFT) |
		   ((u32)(cfg->c_dot1p & RTL_FE_ARB_W0_C_DOT1P_MASK) <<
		    RTL_FE_ARB_W0_C_DOT1P_SHIFT) |
		   ((u32)(cfg->c_dei_cmd & RTL_FE_ARB_W0_C_DEI_CMD_MASK) <<
		    RTL_FE_ARB_W0_C_DEI_CMD_SHIFT) |
		   ((u32)(cfg->c_dei & RTL_FE_ARB_W0_C_DEI_MASK) <<
		    RTL_FE_ARB_W0_C_DEI_SHIFT) |
		   ((u32)(cfg->pol_id & RTL_FE_ARB_W0_POL_ID_MASK) <<
		    RTL_FE_ARB_W0_POL_ID_SHIFT);

	words[1] = ((u32)(cfg->s_vlan_cmd & RTL_FE_ARB_W1_S_VLAN_CMD_MASK) <<
		    RTL_FE_ARB_W1_S_VLAN_CMD_SHIFT) |
		   ((u32)(cfg->s_vid & RTL_FE_ARB_W1_S_VID_MASK) <<
		    RTL_FE_ARB_W1_S_VID_SHIFT) |
		   ((u32)(cfg->s_tpid_cmd & RTL_FE_ARB_W1_S_TPID_CMD_MASK) <<
		    RTL_FE_ARB_W1_S_TPID_CMD_SHIFT) |
		   ((u32)(cfg->s_tpid_sel & RTL_FE_ARB_W1_S_TPID_SEL_MASK) <<
		    RTL_FE_ARB_W1_S_TPID_SEL_SHIFT) |
		   ((u32)(cfg->dot1p_cmd & RTL_FE_ARB_W1_DOT1P_CMD_MASK) <<
		    RTL_FE_ARB_W1_DOT1P_CMD_SHIFT) |
		   ((u32)(cfg->dot1p & RTL_FE_ARB_W1_DOT1P_MASK) <<
		    RTL_FE_ARB_W1_DOT1P_SHIFT) |
		   ((u32)(cfg->dei_cmd & RTL_FE_ARB_W1_DEI_CMD_MASK) <<
		    RTL_FE_ARB_W1_DEI_CMD_SHIFT) |
		   ((u32)(cfg->dei & RTL_FE_ARB_W1_DEI_MASK) <<
		    RTL_FE_ARB_W1_DEI_SHIFT) |
		   ((u32)(cfg->ldpid & RTL_FE_ARB_W1_LDPID_MASK) <<
		    RTL_FE_ARB_W1_LDPID_SHIFT) |
		   ((u32)(cfg->cos_cmd & RTL_FE_ARB_W1_COS_CMD_MASK) <<
		    RTL_FE_ARB_W1_COS_CMD_SHIFT) |
		   ((u32)(cfg->wan_dst & RTL_FE_ARB_W1_WAN_DST_MASK) <<
		    RTL_FE_ARB_W1_WAN_DST_SHIFT);

	words[2] = ((u32)(cfg->permit_spid_en & RTL_FE_ARB_W2_PERMIT_SPID_MASK) <<
		    RTL_FE_ARB_W2_PERMIT_SPID_SHIFT) |
		   ((u32)(cfg->mac_sa_cmd & RTL_FE_ARB_W2_MAC_SA_CMD_MASK) <<
		    RTL_FE_ARB_W2_MAC_SA_CMD_SHIFT) |
		   ((u32)(cfg->mac_sa_sel & RTL_FE_ARB_W2_MAC_SA_SEL_MASK) <<
		    RTL_FE_ARB_W2_MAC_SA_SEL_SHIFT) |
		   ((u32)(cfg->cos & RTL_FE_ARB_W2_COS_MASK) <<
		    RTL_FE_ARB_W2_COS_SHIFT) |
		   ((u32)(cfg->ip_ttl_zero_fwd_ctrl &
			  RTL_FE_ARB_W2_TTL_ZERO_MASK) <<
		    RTL_FE_ARB_W2_TTL_ZERO_SHIFT) |
		   ((u32)(cfg->ip_ttl_dec_cmd & RTL_FE_ARB_W2_TTL_DEC_MASK) <<
		    RTL_FE_ARB_W2_TTL_DEC_SHIFT) |
		   ((u32)(cfg->keep_o_lspid & RTL_FE_ARB_W2_KEEP_O_LSPID_MASK) <<
		    RTL_FE_ARB_W2_KEEP_O_LSPID_SHIFT);
}

void rtl_fe_mcast_fib_unpack(const u32 words[RTL_FE_MCAST_FIB_WORDS],
			     struct rtl_fe_mcast_fib_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->c_vlan_cmd = (words[0] >> RTL_FE_ARB_W0_C_VLAN_CMD_SHIFT) &
			  RTL_FE_ARB_W0_C_VLAN_CMD_MASK;
	cfg->c_vid = (words[0] >> RTL_FE_ARB_W0_C_VID_SHIFT) &
		     RTL_FE_ARB_W0_C_VID_MASK;
	cfg->c_tpid_cmd = (words[0] >> RTL_FE_ARB_W0_C_TPID_CMD_SHIFT) &
			  RTL_FE_ARB_W0_C_TPID_CMD_MASK;
	cfg->c_tpid_sel = (words[0] >> RTL_FE_ARB_W0_C_TPID_SEL_SHIFT) &
			  RTL_FE_ARB_W0_C_TPID_SEL_MASK;
	cfg->c_dot1p_cmd = (words[0] >> RTL_FE_ARB_W0_C_DOT1P_CMD_SHIFT) &
			   RTL_FE_ARB_W0_C_DOT1P_CMD_MASK;
	cfg->c_dot1p = (words[0] >> RTL_FE_ARB_W0_C_DOT1P_SHIFT) &
		       RTL_FE_ARB_W0_C_DOT1P_MASK;
	cfg->c_dei_cmd = (words[0] >> RTL_FE_ARB_W0_C_DEI_CMD_SHIFT) &
			 RTL_FE_ARB_W0_C_DEI_CMD_MASK;
	cfg->c_dei = (words[0] >> RTL_FE_ARB_W0_C_DEI_SHIFT) &
		     RTL_FE_ARB_W0_C_DEI_MASK;
	cfg->pol_id = (words[0] >> RTL_FE_ARB_W0_POL_ID_SHIFT) &
		      RTL_FE_ARB_W0_POL_ID_MASK;

	cfg->s_vlan_cmd = (words[1] >> RTL_FE_ARB_W1_S_VLAN_CMD_SHIFT) &
			  RTL_FE_ARB_W1_S_VLAN_CMD_MASK;
	cfg->s_vid = (words[1] >> RTL_FE_ARB_W1_S_VID_SHIFT) &
		     RTL_FE_ARB_W1_S_VID_MASK;
	cfg->s_tpid_cmd = (words[1] >> RTL_FE_ARB_W1_S_TPID_CMD_SHIFT) &
			  RTL_FE_ARB_W1_S_TPID_CMD_MASK;
	cfg->s_tpid_sel = (words[1] >> RTL_FE_ARB_W1_S_TPID_SEL_SHIFT) &
			  RTL_FE_ARB_W1_S_TPID_SEL_MASK;
	cfg->dot1p_cmd = (words[1] >> RTL_FE_ARB_W1_DOT1P_CMD_SHIFT) &
			 RTL_FE_ARB_W1_DOT1P_CMD_MASK;
	cfg->dot1p = (words[1] >> RTL_FE_ARB_W1_DOT1P_SHIFT) &
		     RTL_FE_ARB_W1_DOT1P_MASK;
	cfg->dei_cmd = (words[1] >> RTL_FE_ARB_W1_DEI_CMD_SHIFT) &
		       RTL_FE_ARB_W1_DEI_CMD_MASK;
	cfg->dei = (words[1] >> RTL_FE_ARB_W1_DEI_SHIFT) &
		   RTL_FE_ARB_W1_DEI_MASK;
	cfg->ldpid = (words[1] >> RTL_FE_ARB_W1_LDPID_SHIFT) &
		     RTL_FE_ARB_W1_LDPID_MASK;
	cfg->cos_cmd = (words[1] >> RTL_FE_ARB_W1_COS_CMD_SHIFT) &
		       RTL_FE_ARB_W1_COS_CMD_MASK;
	cfg->wan_dst = (words[1] >> RTL_FE_ARB_W1_WAN_DST_SHIFT) &
		       RTL_FE_ARB_W1_WAN_DST_MASK;

	cfg->permit_spid_en = (words[2] >> RTL_FE_ARB_W2_PERMIT_SPID_SHIFT) &
			      RTL_FE_ARB_W2_PERMIT_SPID_MASK;
	cfg->mac_sa_cmd = (words[2] >> RTL_FE_ARB_W2_MAC_SA_CMD_SHIFT) &
			  RTL_FE_ARB_W2_MAC_SA_CMD_MASK;
	cfg->mac_sa_sel = (words[2] >> RTL_FE_ARB_W2_MAC_SA_SEL_SHIFT) &
			  RTL_FE_ARB_W2_MAC_SA_SEL_MASK;
	cfg->cos = (words[2] >> RTL_FE_ARB_W2_COS_SHIFT) &
		   RTL_FE_ARB_W2_COS_MASK;
	cfg->ip_ttl_zero_fwd_ctrl = (words[2] >> RTL_FE_ARB_W2_TTL_ZERO_SHIFT) &
				    RTL_FE_ARB_W2_TTL_ZERO_MASK;
	cfg->ip_ttl_dec_cmd = (words[2] >> RTL_FE_ARB_W2_TTL_DEC_SHIFT) &
			      RTL_FE_ARB_W2_TTL_DEC_MASK;
	cfg->keep_o_lspid = (words[2] >> RTL_FE_ARB_W2_KEEP_O_LSPID_SHIFT) &
			    RTL_FE_ARB_W2_KEEP_O_LSPID_MASK;
}

/* --- mcast-fib-codec: end --- */

/*
 * Synthesise the 3-word ARB MC FIB entry for an unmodified L2 egress port
 * forward action (cos_cmd=1 preserves incoming CoS; wan_dst=1 marks port 7).
 */
static void rtl_fe_mcast_fib_encode_port(unsigned int port,
					 u32 words[RTL_FE_MCAST_FIB_WORDS])
{
	struct rtl_fe_mcast_fib_cfg cfg = {
		.ldpid = port & RTL_FE_ARB_W1_LDPID_MASK,
		.cos_cmd = 1,
		.wan_dst = (port == FE_SOURCE_PON) ? 1 : 0,
	};

	rtl_fe_mcast_fib_pack(&cfg, words);
}

bool rtl_fe_mcast_hw_enabled(void)
{
	return READ_ONCE(mcast_hw);
}

bool rtl_fe_mcast_lookup_hw(struct rtl_fe *fe, const u8 *addr, u16 vid,
			    u16 *hw_index, u8 *port_count)
{
	struct rtl_fe_mcast_group *group;
	struct rtl_fe_mcast *m;
	u16 idx = RTL_FE_MCAST_NONE;
	u8 count = 0;

	if (!fe || !fe->mcast || !addr || !READ_ONCE(mcast_hw))
		return false;

	m = fe->mcast;
	mutex_lock(&m->lock);
	if (m->ready) {
		list_for_each_entry(group, &m->groups, list) {
			if (group->vid != vid ||
			    !ether_addr_equal(group->addr, addr))
				continue;
			if (group->host_member)
				break;
			count = (u8)rtl_fe_mcast_mask_weight(group->ports);
			if (count)
				idx = m->mcgid[group->mcgid].hw_index;
			break;
		}
	}
	mutex_unlock(&m->lock);

	if (idx == RTL_FE_MCAST_NONE)
		return false;
	if (hw_index)
		*hw_index = idx;
	if (port_count)
		*port_count = count;
	return true;
}

static int rtl_fe_mcast_program_ports_locked(struct rtl_fe *fe, u16 mcgid,
					     const u8 ports[RTL_FE_MCAST_MASK_BYTES])
{
	struct rtl_fe_mcast *m = fe->mcast;
	u32 words[RTL_FE_MCAST_FIB_WORDS];
	unsigned int i;
	int ret;

	ret = rtl_fe_mcast_program_locked(fe, mcgid, ports, NULL);
	if (ret)
		return ret;

	for (i = 0; i < RTL_FE_MCAST_FIBS; i++) {
		if (!m->fib[i].used || m->fib[i].mcgid != mcgid)
			continue;
		rtl_fe_mcast_fib_encode_port(m->fib[i].port, words);
		ret = rtl_fe_mcast_fib_write(fe, i, words);
		if (ret) {
			rtl_fe_mcast_group_clear(fe, mcgid);
			return ret;
		}
	}
	return 0;
}

static int rtl_fe_mcast_fib_read(struct rtl_fe *fe, unsigned int index,
				 u32 words[RTL_FE_MCAST_FIB_WORDS])
{
	return rtl_fe_mcast_table_read(fe, RTL_FE_ARB_MC_FIB_CMD,
				       RTL_FE_ARB_MC_FIB_DATA0, index,
				       RTL_FE_ARB_MC_FIB_ENTRIES - 1, words,
				       RTL_FE_MCAST_FIB_WORDS);
}

static int rtl_fe_mcast_probe(struct rtl_fe *fe)
{
	u32 words[RTL_FE_MCAST_FIB_WORDS];
	int ret;

	/* Read-only probe of both tables.  A timeout here means the L3PE
	 * tables are not clocked yet; init keeps the software manager and the
	 * first program attempt retries.
	 */
	ret = rtl_fe_mcast_mce_read(fe, 0, words);
	if (ret)
		return ret;
	return rtl_fe_mcast_fib_read(fe, 0, words);
}

static int rtl_fe_mcast_ready_locked(struct rtl_fe *fe)
{
	struct rtl_fe_mcast *m = fe->mcast;
	int ret;

	if (m->ready)
		return 0;
	ret = rtl_fe_mcast_probe(fe);
	if (ret)
		return ret;
	m->ready = true;
	return 0;
}

int rtl_fe_mcast_alloc(struct rtl_fe *fe, u16 *mcgid)
{
	struct rtl_fe_mcast *m;
	int ret;

	if (!fe || !fe->mcast || !mcgid)
		return -ENODEV;
	m = fe->mcast;
	mutex_lock(&m->lock);
	ret = rtl_fe_mcast_alloc_locked(fe, mcgid);
	mutex_unlock(&m->lock);
	return ret;
}

int rtl_fe_mcast_free(struct rtl_fe *fe, u16 mcgid)
{
	struct rtl_fe_mcast *m;
	int ret;

	if (!fe || !fe->mcast)
		return -ENODEV;
	m = fe->mcast;
	mutex_lock(&m->lock);
	ret = rtl_fe_mcast_free_locked(fe, mcgid);
	mutex_unlock(&m->lock);
	return ret;
}

int rtl_fe_mcast_program(struct rtl_fe *fe, u16 mcgid,
			 const u8 ports[RTL_FE_MCAST_MASK_BYTES],
			 const u32 *words)
{
	struct rtl_fe_mcast *m;
	int ret;

	if (!fe || !fe->mcast)
		return -ENODEV;
	if (!ports)
		return -EINVAL;
	m = fe->mcast;
	mutex_lock(&m->lock);
	ret = rtl_fe_mcast_ready_locked(fe);
	if (!ret)
		ret = rtl_fe_mcast_program_locked(fe, mcgid, ports, words);
	mutex_unlock(&m->lock);
	return ret;
}

int rtl_fe_mcast_member_set(struct rtl_fe *fe, u16 mcgid, int index,
			    const u32 words[RTL_FE_MCAST_FIB_WORDS])
{
	struct rtl_fe_mcast *m;
	int ret;

	if (!fe || !fe->mcast)
		return -ENODEV;
	if (!words)
		return -EINVAL;
	m = fe->mcast;
	mutex_lock(&m->lock);
	ret = rtl_fe_mcast_ready_locked(fe);
	if (!ret)
		ret = rtl_fe_mcast_member_set_locked(fe, mcgid, index, words);
	mutex_unlock(&m->lock);
	return ret;
}

static int rtl_fe_mcast_elpb_forward(struct rtl_fe *fe, unsigned int port)
{
	u32 d1, d0;
	int ret;

	rtl_fe_write(fe, RTL_L2FE_PLC_ELPB_ACCESS,
		     RTL_NI_INDIRECT_ACCESS | port);
	ret = rtl_fe_mcast_wait(fe, RTL_L2FE_PLC_ELPB_ACCESS);
	if (ret)
		return ret;
	d1 = rtl_fe_read(fe, RTL_L2FE_PLC_ELPB_DATA1);
	d0 = rtl_fe_read(fe, RTL_L2FE_PLC_ELPB_DATA0);
	if (d0 & RTL_L2FE_PLC_ELPB_STP_STATUS)
		return 0;

	rtl_fe_write(fe, RTL_L2FE_PLC_ELPB_DATA1, d1);
	rtl_fe_write(fe, RTL_L2FE_PLC_ELPB_DATA0,
		     d0 | RTL_L2FE_PLC_ELPB_STP_STATUS);
	rtl_fe_write(fe, RTL_L2FE_PLC_ELPB_ACCESS,
		     RTL_NI_INDIRECT_ACCESS | RTL_NI_INDIRECT_WRITE | port);
	return rtl_fe_mcast_wait(fe, RTL_L2FE_PLC_ELPB_ACCESS);
}

static int rtl_fe_mcast_ilpb_forward(struct rtl_fe *fe, unsigned int port)
{
	u32 words[5];
	unsigned int i;
	int ret;

	rtl_fe_write(fe, RTL_L2FE_PP_ILPB_ACCESS,
		     RTL_NI_INDIRECT_ACCESS | port);
	ret = rtl_fe_mcast_wait(fe, RTL_L2FE_PP_ILPB_ACCESS);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(words); i++)
		words[i] = rtl_fe_read(fe, RTL_L2FE_PP_ILPB_DATA(i));

	words[2] |= FIELD_PREP(RTL_L2FE_ILPB_D2_STP_MODE, 3);
	words[3] |= RTL_L2FE_ILPB_D3_SA_MISS_FWD;

	for (i = 0; i < ARRAY_SIZE(words); i++)
		rtl_fe_write(fe, RTL_L2FE_PP_ILPB_DATA(i), words[i]);
	rtl_fe_write(fe, RTL_L2FE_PP_ILPB_ACCESS,
		     RTL_NI_INDIRECT_ACCESS | RTL_NI_INDIRECT_WRITE | port);
	return rtl_fe_mcast_wait(fe, RTL_L2FE_PP_ILPB_ACCESS);
}

static int rtl_fe_mcast_mmshp_init(struct rtl_fe *fe)
{
	unsigned int p;
	int ret;

	for (p = 0; p < RTL_FE_MCAST_PORTS; p++) {
		u64 mask;
		u32 d1, d0;

		rtl_fe_write(fe, RTL_L2FE_PLE_PORT_MMSHP_ACCESS,
			     RTL_NI_INDIRECT_ACCESS | p);
		ret = rtl_fe_mcast_wait(fe, RTL_L2FE_PLE_PORT_MMSHP_ACCESS);
		if (ret)
			return ret;
		d1 = rtl_fe_read(fe, RTL_L2FE_PLE_PORT_MMSHP_DATA1);
		d0 = rtl_fe_read(fe, RTL_L2FE_PLE_PORT_MMSHP_DATA0);
		if (d1 || d0)
			continue;

		mask = ~BIT_ULL(p);
		rtl_fe_write(fe, RTL_L2FE_PLE_PORT_MMSHP_DATA1,
			     upper_32_bits(mask));
		rtl_fe_write(fe, RTL_L2FE_PLE_PORT_MMSHP_DATA0,
			     lower_32_bits(mask));
		rtl_fe_write(fe, RTL_L2FE_PLE_PORT_MMSHP_ACCESS,
			     RTL_NI_INDIRECT_ACCESS | RTL_NI_INDIRECT_WRITE | p);
		ret = rtl_fe_mcast_wait(fe, RTL_L2FE_PLE_PORT_MMSHP_ACCESS);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Program the L2FE / L2TM / MCE replication datapath used by L3FE MC=1 flows:
 *   L3FE (mc_port 0x1b) -> NIRX_L3FE_DEMUX (2=L2FE) -> L2FE PDPID_MAP[0x1b]=11
 *   -> L2TM ES port 11 -> NI_HV_MCE (0xaa60..0xaa6c) -> L2FE ARB MC FIB (0x1634)
 *   -> L2FE PLC/PLE -> L2TM egress LAN/PON ports.
 *
 * Matches OEM ca-ne.ko aal_port_init / aal_arb_init / aal_ni_init defaults
 * while leaving unicast L3FE and L2FE PONCLE rules untouched.
 */
int rtl_fe_mcast_prepare(struct rtl_fe *fe)
{
	static const unsigned int ippb_ports[] = {
		FE_SOURCE_PON, RTL_FE_L3FE_PSPID, RTL_FE_MCE_PSPID,
		RTL_FE_MC_LDPID,
	};
	static const unsigned int ilpb_ports[] = {
		FE_SOURCE_PON, RTL_FE_MC_LDPID,
	};
	static const unsigned int elpb_ports[] = {
		0, 1, 2, 3, 4, FE_SOURCE_P6, FE_SOURCE_PON, RTL_FE_MC_LDPID,
	};
	unsigned int i;
	int ret;

	rtl_fe_rmw(fe, RTL_NI_GLB_NIRX_L3FE_DEMUX_CFG1,
		   RTL_NI_DEMUX_CFG1_MC_MASK,
		   FIELD_PREP(RTL_NI_DEMUX_CFG1_MC_MASK, RTL_NI_DEMUX_L2FE));

	for (i = 0; i < 4; i++) {
		rtl_fe_write(fe, RTL_L2FE_ARB_PDPID_MAP_TBL_DATA,
			     FIELD_PREP(RTL_L2FE_ARB_PDPID_MAP_PDPID,
					RTL_FE_MC_PDPID));
		rtl_fe_write(fe, RTL_L2FE_ARB_PDPID_MAP_TBL_ACCESS,
			     RTL_NI_INDIRECT_ACCESS | RTL_NI_INDIRECT_WRITE |
			     RTL_FE_MC_LDPID | (i << 6));
		ret = rtl_fe_mcast_wait(fe, RTL_L2FE_ARB_PDPID_MAP_TBL_ACCESS);
		if (ret)
			return ret;
	}

	rtl_fe_rmw(fe, RTL_L2TM_ES_CTRL, 0, BIT(RTL_FE_MC_PDPID));
	rtl_fe_write(fe, RTL_L2TM_ES_SCH_CFG0(RTL_FE_MC_PDPID),
		     FIELD_PREP(RTL_L2TM_ES_SCH_VOQ_EN, 0xff) |
		     FIELD_PREP(RTL_L2TM_ES_SCH_SPEED, 6));

	rtl_fe_rmw(fe, RTL_FE_MCE_CTL, RTL_FE_MCE_CTL_FF_THRSHD,
		   FIELD_PREP(RTL_FE_MCE_CTL_FF_THRSHD,
			      RTL_FE_MCE_CTL_FF_THRSHD_INIT));

	for (i = 0; i < ARRAY_SIZE(ippb_ports); i++) {
		unsigned int port = ippb_ports[i];

		rtl_fe_write(fe, RTL_L2FE_PP_IPPB_DATA,
			     FIELD_PREP(RTL_L2FE_PP_IPPB_LSPID, port));
		rtl_fe_write(fe, RTL_L2FE_PP_IPPB_ACCESS,
			     RTL_NI_INDIRECT_ACCESS | RTL_NI_INDIRECT_WRITE |
			     port);
		ret = rtl_fe_mcast_wait(fe, RTL_L2FE_PP_IPPB_ACCESS);
		if (ret)
			return ret;
	}

	for (i = 0; i < ARRAY_SIZE(ilpb_ports); i++) {
		ret = rtl_fe_mcast_ilpb_forward(fe, ilpb_ports[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < ARRAY_SIZE(elpb_ports); i++) {
		ret = rtl_fe_mcast_elpb_forward(fe, elpb_ports[i]);
		if (ret)
			return ret;
	}

	ret = rtl_fe_mcast_mmshp_init(fe);
	if (ret)
		return ret;

	rtl_fe_rmw(fe, RTL_FE_ARB_CTRL, 0, RTL_FE_ARB_CTRL_KEEP_O_LSPID_MC);
	rtl_fe_rmw(fe, RTL_FE_ARB_CTRL_EXT, 0, RTL_FE_ARB_CTRL_EXT_MC_MMSHP);
	return 0;
}

int rtl_fe_mcast_init(struct rtl_fe *fe)
{
	struct rtl_fe_mcast *m;
	unsigned int i;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	mutex_init(&m->lock);
	INIT_LIST_HEAD(&m->groups);
	for (i = 0; i < RTL_FE_MCAST_MCGIDS; i++) {
		m->mcgid[i].hw_index = RTL_FE_MCAST_NONE;
		m->mcgid[i].prev_index = RTL_FE_MCAST_NONE;
	}
	strscpy(m->last, "init", sizeof(m->last));

	m->ready = rtl_fe_mcast_probe(fe) == 0;
	fe->mcast = m;
	if (!m->ready)
		dev_warn(fe->dev,
			 "mcast: L3PE multicast tables not ready; programming deferred\n");
	return 0;
}

void rtl_fe_mcast_exit(struct rtl_fe *fe)
{
	struct rtl_fe_mcast *m;
	struct rtl_fe_mcast_group *group, *next;
	unsigned int i;
	int ret;

	if (!fe || !fe->mcast)
		return;
	m = fe->mcast;

	mutex_lock(&m->lock);
	list_for_each_entry_safe(group, next, &m->groups, list) {
		list_del(&group->list);
		kfree(group);
	}
	/* Release every remaining MCGID through the normal free path, in
	 * reverse index order, after the flows have been stopped by
	 * rtl_fe_destroy().
	 */
	for (i = RTL_FE_MCAST_MCGIDS; i-- > 0;) {
		if (!test_bit(i, m->mcgids))
			continue;
		m->mcgid[i].users = 1;
		ret = rtl_fe_mcast_free_locked(fe, i);
		if (ret)
			dev_warn(fe->dev, "mcast: failed to release MCGID %u: %d\n",
				 i, ret);
	}
	mutex_unlock(&m->lock);
	fe->mcast = NULL;
	kfree(m);
}

static void rtl_fe_mcast_port_set(u8 ports[RTL_FE_MCAST_MASK_BYTES],
				  unsigned int port)
{
	ports[port / RTL_FE_MCAST_BITS_PER_BYTE] |=
		BIT(port % RTL_FE_MCAST_BITS_PER_BYTE);
}

static void rtl_fe_mcast_port_clear(u8 ports[RTL_FE_MCAST_MASK_BYTES],
				    unsigned int port)
{
	ports[port / RTL_FE_MCAST_BITS_PER_BYTE] &=
		~BIT(port % RTL_FE_MCAST_BITS_PER_BYTE);
}

static bool rtl_fe_mcast_mask_empty(const u8 ports[RTL_FE_MCAST_MASK_BYTES])
{
	unsigned int i;

	for (i = 0; i < RTL_FE_MCAST_MASK_BYTES; i++)
		if (ports[i])
			return false;
	return true;
}

bool rtl_fe_mcast_is_data_mac(const u8 *mac)
{
	if (!is_multicast_ether_addr(mac) || is_broadcast_ether_addr(mac))
		return false;
	/* IPv4 multicast (01:00:5e:00:00:00..01:00:5e:7f:ff:ff), excluding
	 * 224.0.0.0/24 link-local control (IGMP, OSPF, mDNS, VRRP).
	 */
	if (mac[0] == 0x01 && mac[1] == 0x00 && mac[2] == 0x5e &&
	    !(mac[3] & 0x80))
		return mac[3] != 0 || mac[4] != 0;
	/* IPv6 multicast (33:33:xx:xx:xx:xx), excluding link-local control
	 * (33:33:00:00:00:xx, MLD/mDNS/OSPFv3) and NDP solicited-node
	 * (33:33:ff:xx:xx:xx).
	 */
	if (mac[0] == 0x33 && mac[1] == 0x33) {
		if (mac[2] == 0xff)
			return false;
		return mac[2] != 0 || mac[3] != 0 || mac[4] != 0;
	}
	return false;
}

/*
 * Switchdev MDB hook.  The bridge defers MDB objects (SWITCHDEV_F_DEFER),
 * so this runs from switchdev's deferred workqueue in process context under
 * rtnl_lock().  PORT_MDB events update the group's L2 port mask and program
 * hardware MCE + ARB MC FIB entries when mcast_hw=1; HOST_MDB events record
 * whether the local bridge host also listens on the group so L3FE bridge
 * offload never bypasses local sockets.
 */
void rtl_fe_mcast_mdb_event(struct rtl_fe *fe, unsigned long event, void *ptr)
{
	struct switchdev_notifier_port_obj_info *info = ptr;
	const struct switchdev_obj_port_mdb *mdb;
	struct rtl_fe_mcast_group *group, *found;
	struct net_device *dev;
	struct rtl_fe_port *port;
	struct rtl_fe_mcast *m;
	unsigned int source = RTL_FE_MCAST_PORTS;
	bool is_host;
	u16 mcgid;
	int ret;

	if (!fe || !fe->mcast || !info || !info->obj)
		return;
	if (info->obj->id == SWITCHDEV_OBJ_ID_PORT_MDB)
		is_host = false;
	else if (info->obj->id == SWITCHDEV_OBJ_ID_HOST_MDB)
		is_host = true;
	else
		return;
	if (event != SWITCHDEV_PORT_OBJ_ADD && event != SWITCHDEV_PORT_OBJ_DEL)
		return;

	mdb = SWITCHDEV_OBJ_PORT_MDB(info->obj);
	if (!rtl_fe_mcast_is_data_mac(mdb->addr))
		return;
	if (!is_host) {
		dev = switchdev_notifier_info_to_dev(&info->info);
		if (!dev)
			return;
		while (is_vlan_dev(dev))
			dev = vlan_dev_real_dev(dev);

		/* Resolve the physical NI port before taking the manager lock;
		 * the FE port list is protected by fe->lock.
		 */
		mutex_lock(&fe->lock);
		port = rtl_fe_port_find(fe, dev);
		source = port ? port->source : RTL_FE_MCAST_PORTS;
		mutex_unlock(&fe->lock);
		if (source >= RTL_FE_MCAST_PORTS)
			return;
	}

	/* Groups are keyed by (MAC, VID) only.  The L2 multicast table is
	 * global to the switch, so the same group/VLAN seen through two
	 * bridges maps onto one hardware entry anyway.
	 */
	m = fe->mcast;
	mutex_lock(&m->lock);
	found = NULL;
	list_for_each_entry(group, &m->groups, list) {
		if (group->vid == mdb->vid &&
		    ether_addr_equal(group->addr, mdb->addr)) {
			found = group;
			break;
		}
	}

	if (event == SWITCHDEV_PORT_OBJ_ADD) {
		if (!found) {
			found = kzalloc(sizeof(*found), GFP_KERNEL);
			if (!found)
				goto unlock;
			ret = rtl_fe_mcast_alloc_locked(fe, &mcgid);
			if (ret) {
				kfree(found);
				goto unlock;
			}
			ether_addr_copy(found->addr, mdb->addr);
			found->vid = mdb->vid;
			found->mcgid = mcgid;
			list_add_tail(&found->list, &m->groups);
			dev_dbg(fe->dev, "mcast: MDB %pM vid %u -> MCGID %u\n",
				mdb->addr, mdb->vid, mcgid);
		}
		if (is_host) {
			found->host_member = true;
		} else {
			rtl_fe_mcast_port_set(found->ports, source);
			if (mcast_hw && !rtl_fe_mcast_ready_locked(fe))
				rtl_fe_mcast_program_ports_locked(fe,
								  found->mcgid,
								  found->ports);
		}
	} else {
		if (!found)
			goto unlock;
		if (is_host)
			found->host_member = false;
		else
			rtl_fe_mcast_port_clear(found->ports, source);
		if (rtl_fe_mcast_mask_empty(found->ports) &&
		    !found->host_member) {
			ret = rtl_fe_mcast_free_locked(fe, found->mcgid);
			if (ret)
				dev_warn(fe->dev,
					 "mcast: MDB %pM vid %u release failed: %d\n",
					 mdb->addr, mdb->vid, ret);
			dev_dbg(fe->dev, "mcast: MDB %pM vid %u -> MCGID %u released\n",
				mdb->addr, mdb->vid, found->mcgid);
			list_del(&found->list);
			kfree(found);
		} else if (!is_host && mcast_hw &&
			   !rtl_fe_mcast_ready_locked(fe)) {
			if (rtl_fe_mcast_mask_empty(found->ports))
				rtl_fe_mcast_group_clear(fe, found->mcgid);
			else
				rtl_fe_mcast_program_ports_locked(fe,
								  found->mcgid,
								  found->ports);
		}
	}
unlock:
	mutex_unlock(&m->lock);
}

/*
 * Drop a port from every software group.  Called from the netdev notifier
 * when the device is unregistered: a stale port bit would otherwise keep a
 * group reference alive and, worse, make a future port that reuses the same
 * source id a member of an old group.
 */
void rtl_fe_mcast_port_gone(struct rtl_fe *fe, struct net_device *dev)
{
	struct rtl_fe_mcast_group *group, *next;
	struct rtl_fe_port *port;
	struct rtl_fe_mcast *m;
	unsigned int source;

	if (!fe || !fe->mcast || !dev)
		return;
	mutex_lock(&fe->lock);
	port = rtl_fe_port_find(fe, dev);
	source = port ? port->source : RTL_FE_MCAST_PORTS;
	mutex_unlock(&fe->lock);
	if (source >= RTL_FE_MCAST_PORTS)
		return;

	m = fe->mcast;
	mutex_lock(&m->lock);
	list_for_each_entry_safe(group, next, &m->groups, list) {
		if (!rtl_fe_mcast_port_test(group->ports, source))
			continue;
		rtl_fe_mcast_port_clear(group->ports, source);
		if (!rtl_fe_mcast_mask_empty(group->ports)) {
			if (mcast_hw && !rtl_fe_mcast_ready_locked(fe))
				rtl_fe_mcast_program_ports_locked(fe,
								  group->mcgid,
								  group->ports);
			continue;
		}
		if (group->host_member) {
			if (mcast_hw && !rtl_fe_mcast_ready_locked(fe))
				rtl_fe_mcast_group_clear(fe, group->mcgid);
			continue;
		}
		if (rtl_fe_mcast_free_locked(fe, group->mcgid))
			dev_warn(fe->dev,
				 "mcast: port %u group %pM vid %u release failed\n",
				 source, group->addr, group->vid);
		list_del(&group->list);
		kfree(group);
	}
	mutex_unlock(&m->lock);
}

static int rtl_fe_mcast_snapshot(struct rtl_fe *fe, u16 mcgid)
{
	struct rtl_fe_mcast *m = fe->mcast;
	struct rtl_fe_mcgid *entry;
	u32 mce[RTL_FE_MCAST_MCE_WORDS], words[RTL_FE_MCAST_FIB_WORDS];
	u64 members;
	unsigned int i;
	int len, ret;

	if (mcgid >= RTL_FE_MCAST_MCGIDS || !test_bit(mcgid, m->mcgids))
		return -EINVAL;
	entry = &m->mcgid[mcgid];
	if (entry->hw_index == RTL_FE_MCAST_NONE) {
		snprintf(m->last, sizeof(m->last), "mcgid %u: no MCE slot bound",
			 mcgid);
		return 0;
	}
	ret = rtl_fe_mcast_mce_read(fe, entry->hw_index, mce);
	if (ret)
		return ret;
	members = (u64)mce[0] | ((u64)mce[1] << RTL_FE_MCAST_WORD_BITS);
	len = scnprintf(m->last, sizeof(m->last),
			"mcgid %u mce %u mce_words %08x %08x members 0x%016llx",
			mcgid, entry->hw_index, mce[0], mce[1],
			(unsigned long long)members);
	for (i = 0; i < RTL_FE_MCAST_FIBS; i++) {
		if (!(members & BIT_ULL(i)))
			continue;
		ret = rtl_fe_mcast_fib_read(fe, i, words);
		if (ret)
			return ret;
		if (len >= (int)sizeof(m->last))
			break;
		len += scnprintf(m->last + len, sizeof(m->last) - len,
				 " fib[%u]=%08x,%08x,%08x",
				 i, words[0], words[1], words[2]);
	}
	return 0;
}

static int rtl_fe_mcast_show(struct seq_file *s, void *unused)
{
	struct rtl_fe *fe = s->private;
	struct rtl_fe_mcast *m = fe->mcast;
	struct rtl_fe_mcast_group *group;
	u64 members;
	unsigned int i;

	if (!m)
		return -ENODEV;
	mutex_lock(&m->lock);
	seq_printf(s, "ready=%u last: %s\n", m->ready, m->last);
	seq_printf(s,
		   "mce_hw: in=%u out=%u zero_drop=%u err=0x%08x ctl=0x%08x arb=0x%08x/%08x ple=0x%08x demux1=0x%08x cfg2=0x%08x\n",
		   rtl_fe_read(fe, RTL_FE_MCE_INGRESS_CNT),
		   rtl_fe_read(fe, RTL_FE_MCE_EGRESS_CNT),
		   rtl_fe_read(fe, RTL_FE_MCE_ZERO_DROP_CNT),
		   rtl_fe_read(fe, RTL_FE_MCE_ERR_STS),
		   rtl_fe_read(fe, RTL_FE_MCE_CTL),
		   rtl_fe_read(fe, RTL_FE_ARB_CTRL),
		   rtl_fe_read(fe, RTL_FE_ARB_CTRL_EXT),
		   rtl_fe_read(fe, RTL_L2FE_PLE_CTL),
		   rtl_fe_read(fe, RTL_NI_GLB_NIRX_L3FE_DEMUX_CFG1),
		   rtl_fe_read(fe, RTL_NI_GLB_INTERNAL_PORT_ID_CFG2));
	seq_printf(s, "mce_hdr: in=%08x:%08x:%08x out=%08x:%08x:%08x\n",
		   rtl_fe_read(fe, RTL_FE_MCE_LAST_IN_HDR2),
		   rtl_fe_read(fe, RTL_FE_MCE_LAST_IN_HDR1),
		   rtl_fe_read(fe, RTL_FE_MCE_LAST_IN_HDR0),
		   rtl_fe_read(fe, RTL_FE_MCE_LAST_OUT_HDR2),
		   rtl_fe_read(fe, RTL_FE_MCE_LAST_OUT_HDR1),
		   rtl_fe_read(fe, RTL_FE_MCE_LAST_OUT_HDR0));
	seq_puts(s, "mcgid bitmap:");
	for (i = 0; i < BITS_TO_LONGS(RTL_FE_MCAST_MCGIDS); i++)
		seq_printf(s, " %016lx", m->mcgids[i]);
	seq_putc(s, '\n');
	seq_puts(s, "allocated:");
	for (i = 0; i < RTL_FE_MCAST_MCGIDS; i++)
		if (test_bit(i, m->mcgids))
			seq_printf(s, " %u", i);
	seq_putc(s, '\n');
	for (i = 0; i < RTL_FE_MCAST_MCES; i++) {
		if (!m->mce[i].used)
			continue;
		members = rtl_fe_mcast_members(m, m->mce[i].mcgid);
		seq_printf(s, "mce %u: mcgid=%u members=0x%016llx\n",
			   i, m->mce[i].mcgid, (unsigned long long)members);
	}
	for (i = 0; i < RTL_FE_MCAST_FIBS; i++) {
		struct rtl_fe_mcast_fib_cfg cfg;

		if (!m->fib[i].used)
			continue;
		rtl_fe_mcast_fib_unpack(m->fib[i].words, &cfg);
		seq_printf(s,
			   "fib %u: mcgid=%u port=%u words=%08x,%08x,%08x ldpid=%u wan=%u cos_cmd=%u s_vlan=%u/%u c_vlan=%u/%u\n",
			   i, m->fib[i].mcgid, m->fib[i].port,
			   m->fib[i].words[0], m->fib[i].words[1],
			   m->fib[i].words[2], cfg.ldpid, cfg.wan_dst,
			   cfg.cos_cmd, cfg.s_vlan_cmd, cfg.s_vid,
			   cfg.c_vlan_cmd, cfg.c_vid);
	}
	list_for_each_entry(group, &m->groups, list) {
		seq_printf(s, "group %pM vid=%u mcgid=%u mce=%u host=%u ports=",
			   group->addr, group->vid, group->mcgid,
			   m->mcgid[group->mcgid].hw_index, group->host_member);
		for (i = 0; i < RTL_FE_MCAST_MASK_BYTES; i++)
			seq_printf(s, "%02x", group->ports[i]);
		seq_putc(s, '\n');
	}
	mutex_unlock(&m->lock);
	return 0;
}

static char *rtl_fe_mcast_token(char **cursor)
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

static int rtl_fe_mcast_parse_words(char **cursor,
				    u32 words[RTL_FE_MCAST_FIB_WORDS])
{
	char *a;
	unsigned int i;

	for (i = 0; i < RTL_FE_MCAST_FIB_WORDS; i++) {
		a = rtl_fe_mcast_token(cursor);
		if (!a || kstrtou32(a, 0, &words[i]))
			return -EINVAL;
	}
	return rtl_fe_mcast_token(cursor) ? -EINVAL : 0;
}

/* Usage, one command per write (reading the file prints the dump):
 *   alloc                                  allocate one software MCGID
 *   free <mcgid>                           drop one reference
 *   dump                                   no-op; the read side dumps state
 *   set <mcgid> <w0> <w1> <w2>             raw ARB MC FIB words for the
 *                                          group's only member, or for a
 *                                          new member when it has none
 *   member <mcgid> <idx> <w0> <w1> <w2>    raw words for one explicit slot
 *   program <mcgid> <mask> <w0> <w1> <w2>  rebuild the port mask; the 64-bit
 *                                          mask is little-endian, bit N is
 *                                          port source N
 *   get <mcgid>                            read MCE + member words back
 *   program-ports <mcgid> <mask>           synthesise per-port ARB MC FIBs
 */
static ssize_t rtl_fe_mcast_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct rtl_fe *fe = s->private;
	struct rtl_fe_mcast *m = fe->mcast;
	char buf[RTL_FE_MCAST_CMD_BUF_LEN], *cursor, *cmd, *a;
	u32 words[RTL_FE_MCAST_FIB_WORDS];
	unsigned int slot;
	u64 mask;
	u16 mcgid;
	int ret;

	if (!m)
		return -ENODEV;
	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	cursor = buf;
	cmd = rtl_fe_mcast_token(&cursor);
	if (!cmd)
		return -EINVAL;

	if (sysfs_streq(cmd, "dump")) {
		if (rtl_fe_mcast_token(&cursor))
			return -EINVAL;
		return count;
	}

	if (sysfs_streq(cmd, "alloc")) {
		if (rtl_fe_mcast_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_mcast_alloc(fe, &mcgid);
		mutex_lock(&m->lock);
		if (!ret) {
			snprintf(m->last, sizeof(m->last), "allocated MCGID %u",
				 mcgid);
			mutex_unlock(&m->lock);
			dev_info(fe->dev, "mcast: allocated MCGID %u\n", mcgid);
		} else {
			snprintf(m->last, sizeof(m->last), "alloc failed: %d",
				 ret);
			mutex_unlock(&m->lock);
		}
		return ret ? ret : count;
	}

	a = rtl_fe_mcast_token(&cursor);
	if (!a || kstrtou16(a, 0, &mcgid))
		return -EINVAL;

	if (sysfs_streq(cmd, "free")) {
		if (rtl_fe_mcast_token(&cursor))
			return -EINVAL;
		ret = rtl_fe_mcast_free(fe, mcgid);
	} else if (sysfs_streq(cmd, "get")) {
		if (rtl_fe_mcast_token(&cursor))
			return -EINVAL;
		mutex_lock(&m->lock);
		ret = rtl_fe_mcast_ready_locked(fe);
		if (!ret)
			ret = rtl_fe_mcast_snapshot(fe, mcgid);
		mutex_unlock(&m->lock);
	} else if (sysfs_streq(cmd, "set")) {
		ret = rtl_fe_mcast_parse_words(&cursor, words);
		if (!ret)
			ret = rtl_fe_mcast_member_set(fe, mcgid, -1, words);
	} else if (sysfs_streq(cmd, "member")) {
		a = rtl_fe_mcast_token(&cursor);
		if (!a || kstrtouint(a, 0, &slot) || slot >= RTL_FE_MCAST_FIBS)
			return -EINVAL;
		ret = rtl_fe_mcast_parse_words(&cursor, words);
		if (!ret)
			ret = rtl_fe_mcast_member_set(fe, mcgid, slot, words);
	} else if (sysfs_streq(cmd, "program")) {
		u8 ports[RTL_FE_MCAST_MASK_BYTES];

		a = rtl_fe_mcast_token(&cursor);
		if (!a || kstrtou64(a, 0, &mask))
			return -EINVAL;
		ret = rtl_fe_mcast_parse_words(&cursor, words);
		if (ret)
			return ret;
		memcpy(ports, &mask, sizeof(ports));
		ret = rtl_fe_mcast_program(fe, mcgid, ports, words);
	} else if (sysfs_streq(cmd, "program-ports")) {
		u8 ports[RTL_FE_MCAST_MASK_BYTES];

		a = rtl_fe_mcast_token(&cursor);
		if (!a || kstrtou64(a, 0, &mask) ||
		    rtl_fe_mcast_token(&cursor))
			return -EINVAL;
		memcpy(ports, &mask, sizeof(ports));
		mutex_lock(&m->lock);
		ret = rtl_fe_mcast_ready_locked(fe);
		if (!ret)
			ret = rtl_fe_mcast_program_ports_locked(fe, mcgid,
								ports);
		mutex_unlock(&m->lock);
	} else {
		return -EINVAL;
	}

	mutex_lock(&m->lock);
	if (ret)
		snprintf(m->last, sizeof(m->last), "%s %u failed: %d", cmd,
			 mcgid, ret);
	else if (!sysfs_streq(cmd, "get"))
		snprintf(m->last, sizeof(m->last), "%s %u ok", cmd, mcgid);
	mutex_unlock(&m->lock);
	return ret ? ret : count;
}

static int rtl_fe_mcast_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl_fe_mcast_show, inode->i_private);
}

static const struct file_operations rtl_fe_mcast_fops = {
	.owner = THIS_MODULE,
	.open = rtl_fe_mcast_open,
	.read = seq_read,
	.write = rtl_fe_mcast_write,
	.llseek = seq_lseek,
	.release = single_release,
};

void rtl_fe_mcast_debugfs_init(struct rtl_fe *fe)
{
	if (!fe || !fe->mcast || !fe->debugfs)
		return;
	debugfs_create_file("mcast", 0600, fe->debugfs, fe, &rtl_fe_mcast_fops);
}
