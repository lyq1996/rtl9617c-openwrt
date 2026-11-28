/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RTL9617C L3TE flow/group policer, WRED and L3 DoS register layer.
 *
 * Every offset below is a byte offset inside the NE register window
 * (g_ne_reg_iobase in the vendor ca-ne.ko), not a standalone IP base.
 * Source of truth: phase0-re2-policer-wred-dos.md sections 1/2/4/5, which cites
 * the vendor functions by address:
 *
 *   __l3_te_policer_flow_profile_access.constprop.0  @0x73e90
 *   __l3_te_policer_flow_grp_profile_access.constprop.0 @0x73600
 *   __l3_te_policer_flow3_profile_access.constprop.0 @0x73af0
 *   aal_l3_te_policer_flow_cfg_set                   @0x6f6c0
 *   aal_l3_te_policer_agr_flow_cfg_set               @0x6f920
 *   __l3_te_wred_profile_access.constprop.0          @0x72d30
 *   aal_l3_te_wred_profile_sel_set                   @0x72bc0
 *   aal_dos_atk_set                                  @0x918c0
 *
 * The shared rtl9617c_regs.h is deliberately left untouched; these
 * definitions belong to the L3TE block only.
 */
#ifndef RTL9617C_NI_TE_FLOW_H
#define RTL9617C_NI_TE_FLOW_H

#include <linux/bitmap.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct rtl9617c_ni;
struct dentry;

/* ------------------------------------------------------------------ */
/* section 1.1 indirect table access protocol (identical for every L3TE table) */
/* ACCESS   : bit31 = BUSY/START, bit30 = 1 write / 0 read,           */
/*            bits[idx_bits-1:0] = entry index                        */
/* DATA0 sits at the highest address of the entry, DATA1 below it ... */
/* ------------------------------------------------------------------ */
#define L3TE_IND_BUSY			BIT(31)
#define L3TE_IND_WRITE			BIT(30)
#define L3TE_IND_POLL_MAX		4096u
#define L3TE_IND_IDX_MASK(bits)		((1u << (bits)) - 1u)

/* ------------------------------------------------------------------ */
/* section 2.1 POL flow profile: 256 slots x 3 data words                     */
/* ------------------------------------------------------------------ */
#define L3TE_POL_FLOW_ACC		0x9128u
#define L3TE_POL_FLOW_DATA2		0x912cu
#define L3TE_POL_FLOW_DATA1		0x9130u
#define L3TE_POL_FLOW_DATA0		0x9134u
#define L3TE_POL_FLOW_SLOTS		256u
#define L3TE_POL_FLOW_IDX_BITS		8u

/* section 2.1 POL2 / group profile: 64 slots (needs GLB_CTRL bit25 = 1) */
#define L3TE_POL_GRP_ACC		0x9164u
#define L3TE_POL_GRP_DATA2		0x9168u
#define L3TE_POL_GRP_DATA1		0x916cu
#define L3TE_POL_GRP_DATA0		0x9170u
#define L3TE_POL_GRP_SLOTS		64u
#define L3TE_POL_GRP_IDX_BITS		6u

/*
 * section 2.1 POL3 / flow3 profile: 64 slots. Only the profile table differs from
 * POL; aal_l3_te_policer_flow3_cfg_set reuses the same 0x9108 cfg bank
 * constants, so POL3 cfg sharing is still U13 and nothing here drives it.
 */
#define L3TE_POL_FLOW3_ACC		0x918cu
#define L3TE_POL_FLOW3_DATA2		0x9190u
#define L3TE_POL_FLOW3_DATA1		0x9194u
#define L3TE_POL_FLOW3_DATA0		0x9198u
#define L3TE_POL_FLOW3_SLOTS		64u
#define L3TE_POL_FLOW3_IDX_BITS		6u

/*
 * section 2.4 flow cfg: 8 registers of 16 bits, each shared by 32 flows
 * (bit field [15:0]; [31:16] is preserved by the vendor read-modify-write).
 * section 2.4 also shows agr_flow_cfg_set banking on (idx >> 5) once GLB_CTRL
 * bit25 = 1, hence the same 32-slot sharing rule for the group table.
 */
#define L3TE_POL_FLOW_CFG(flow)		(0x9108u + (((flow) >> 5) * 4u))
#define L3TE_POL_GRP_CFG(group)		(0x9144u + (((group) >> 5) * 4u))
#define L3TE_POL_BANK_SLOTS		32u
#define L3TE_POL_CFG_TYPE		GENMASK(2, 0)
#define L3TE_POL_CFG_TYPE_OFF		0u
#define L3TE_POL_CFG_TYPE_ON		2u
/* cfg bit4 = rate mode; preserved, measurement semantics still U4 */
#define L3TE_POL_CFG_RATE_MODE		BIT(4)

/* ------------------------------------------------------------------ */
/* sections 2.6 / 2.3 TE global and rate encoding                             */
/* ------------------------------------------------------------------ */
#define L3TE_TE_GLB_CTRL		0x9004u
#define L3TE_TE_GLB_POLICER_EN		BIT(0)
#define L3TE_GLB_CTRL_WRED_EN		BIT(3)
#define L3TE_GLB_CTRL_MARK_ECN_EN	BIT(4)
#define L3TE_GLB_CTRL_POL_ECN_EN	BIT(5)
#define L3TE_GLB_CTRL_WRED_ECN_EN	BIT(6)
#define L3TE_TE_GLB_POL_GRP_64		BIT(25)

/* L2FE ARB global control (NE 0x1600): bit 2 enables IP ECN marking support. */
#define L3TE_L2FE_ARB_CTRL		0x1600u
#define L3TE_L2FE_ARB_CTRL_IP_ECN_EN	BIT(2)

/*
 * OEM fc_8277c.ko rtk_8277c_policer_init (@0xcb780) programs NE 0x9180 and
 * NE 0x30ac alongside L3TE_TE_GLB_CTRL when bringing up L3TE policing.
 */
#define L3TE_POL_GLB_CFG		0x9180u
#define L3TE_POL_GLB_CFG_MASK		(GENMASK(5, 0) | GENMASK(13, 8))
#define L3TE_POL_GLB_CFG_VAL		((0x1fu << 8) | 0x1fu)
/* NE 0x30ac lives in the L3FE register window alongside RTL_FE_TABLE_CFG0/1. */
#define L3TE_FE_POL_GLB_CFG		0x30acu
#define L3TE_FE_POL_GLB_EN		(BIT(8) | BIT(9))

/*
 * Rate fields hold kbps split as rate = 500 * hi + lo (spec section 2.3:
 * fc_mgr rtk_8277c_l3_flow_policer_rate_set @0xc6940). 16 bit hi bounds the
 * encodable rate; the 10 bit lo only ever needs 0..499.
 */
#define L3TE_POL_RATE_MAX_KBPS		32767999u
#define L3TE_POL_RATE_DIVISOR		500u
#define L3TE_POL_BPS_PER_KBPS		1000u
/* 12 bit burst fields, 0 is rejected by the vendor setters (spec section 2.2) */
#define L3TE_POL_BURST_MAX		0xfffu

/* Policer 3-word profile bitfields (spec section 2.2) */
#define L3TE_POL_RATE_LO_MASK		0x3ffu
#define L3TE_POL_RATE_HI_MASK		0xffffu
#define L3TE_POL_BURST_MASK		0xfffu
#define L3TE_POL_W0_CIR_HI_SHIFT	10
#define L3TE_POL_W0_CBS_LO_SHIFT	26
#define L3TE_POL_W0_CBS_LO_BITS		6
#define L3TE_POL_W0_CBS_LO_MASK		0x3fu
#define L3TE_POL_W1_CBS_HI_MASK		0x3fu
#define L3TE_POL_W1_PIR_LO_SHIFT	6
#define L3TE_POL_W1_PIR_HI_SHIFT	16

/* ------------------------------------------------------------------ */
/* sections 4.1/4.2 WRED control, AFGS and profile table (32 x 8 words) */
/* ------------------------------------------------------------------ */
#define L3TE_WRED_CTRL			0x9380u
#define L3TE_WRED_CTRL_FAST_DECAY	BIT(2)
#define L3TE_WRED_CTRL_AFGS_WEIGHT	GENMASK(7, 4)
#define L3TE_WRED_CTRL_AFGS_SEL_WORST	BIT(8)

#define L3TE_WRED_AFGS_ACC		0x9384u
#define L3TE_WRED_AFGS_DATA		0x9388u
#define L3TE_WRED_AFGS_ENTRIES		8u
#define L3TE_WRED_AFGS_IDX_BITS		3u
#define L3TE_WRED_AFGS_DEPTH_MASK	0x3fffu
#define L3TE_WRED_AFGS_EQ1_SHIFT	14u

#define L3TE_WRED_PROF_ACC		0x938cu
#define L3TE_WRED_PROF_DATA(n)		(0x93acu - 4u * (n))	/* n = 0..7 */
#define L3TE_WRED_PROF_WORDS		8u
#define L3TE_WRED_PROFILES		32u
#define L3TE_WRED_PROF_IDX_BITS		5u

/* section 4.3 WRED profile select: 64 port/queue groups x 2 data words */
#define L3TE_WRED_SEL_ACC		0x93b0u
#define L3TE_WRED_SEL_DATA(n)		(0x93b8u - 4u * (n))	/* n = 0..1 */
#define L3TE_WRED_SEL_WORDS		2u
#define L3TE_WRED_QUEUES		64u
#define L3TE_WRED_SEL_IDX_BITS		6u
/*
 * The 2 word select payload packs eight 5-bit profile IDs (sel0..sel7), one
 * per VoQ/CoS 0..7 of that queue group (aal_l3_te_wred_profile_sel_set
 * @0x72bc0): fields 0..5 live in word0 at 5-bit steps, field 6 spans
 * word0[31:30] + word1[2:0] and field 7 sits in word1[7:3].
 */
#define L3TE_WRED_SEL_FIELDS		8u
#define L3TE_WRED_SEL_FIELD_DEFAULT	0u
#define L3TE_WRED_SEL_FIELD_BITS	5u
#define L3TE_WRED_SEL_FIELD_MASK	0x1fu
#define L3TE_WRED_SEL_SPLIT_FIELD	6u
#define L3TE_WRED_SEL_F6_W0_SHIFT	30
#define L3TE_WRED_SEL_F6_W0_BITS	2u
#define L3TE_WRED_SEL_F6_W0_MASK	0x3u
#define L3TE_WRED_SEL_F6_W1_MASK	0x7u
#define L3TE_WRED_SEL_F7_W1_SHIFT	3

/*
 * section 4.4 PIE/WRED per-port enable bitmap (4 x 32-bit = 128 ports,
 * indexed by {dir, ldpid[5:0]}): verified against ca-ne.ko
 * aal_l3_te_pie_reset (@0x76a6c), fc_8277c.ko rtk_fc_qos_aqm_pie_enable_set
 * (@0xb6f1c), and live RTL9617C silicon: 0 = disabled, 1 = WRED/PIE enabled.
 */
#define L3TE_PIE_WRED_PORTS		4u
#define L3TE_PIE_WRED_PORT_EN(p)	(0x9080u + 4u * (p))

/* ------------------------------------------------------------------ */
/* Host Policing (phase2-re10-hostpol-final.md, fc_8277c model)        */
/*                                                                    */
/* 32 entries keyed by MAC (fc_8277c rtk_fc_hostPoliceControl_set     */
/* @0xb05e0: "cmp w21,#0x1f").  An entry owns up to two virtual meters */
/* from a pool of 32 (0..31); a rate limited direction maps 1:1 onto   */
/* the L3TE POL1 slot 0xA0 + vmeter (160..191).  A direction that only */
/* counts uses a fixed per-entry MIB slot instead and consumes no      */
/* vmeter: ingress 0xC0 + idx (192..223), egress 0xE0 + idx            */
/* (224..255).  POL1 0xA0..0xFF is therefore reserved for host        */
/* policing and the flow policer allocates only 0..0x9F.              */
/*                                                                    */
/* The share-meter modes of the vendor (fc_db+156 bit0/bit1) need     */
/* rtk_rate_shareMeter_* which is not part of the shipped package     */
/* (U-1); this driver always programs L3TE POL1 directly.             */
/* ------------------------------------------------------------------ */
#define RTL_TE_HOSTPOL_ENTRIES		32u
#define RTL_TE_HOSTPOL_VMETERS		32u
#define RTL_TE_HOSTPOL_ALEN		6u
#define RTL_TE_HOSTPOL_SLOT_BASE	0xA0u		/* reserved POL1 window */
#define RTL_TE_HOSTPOL_SLOT_END		0xFFu
#define RTL_TE_FLOW_SLOT_COUNT		0xA0u		/* flow policers: 0..159 */
#define RTL_TE_HOSTPOL_RATE_BASE	0xA0u		/* + vmeter */
#define RTL_TE_HOSTPOL_MIB_IN_BASE	0xC0u		/* + entry index */
#define RTL_TE_HOSTPOL_MIB_EG_BASE	0xE0u		/* + entry index */
#define RTL_TE_HOSTPOL_RATE_MAX		0x1F3FFFFu	/* 32767999 kbps */
#define RTL_TE_HOSTPOL_VMETER_NONE	0xFFu
#define RTL_TE_HOSTPOL_VMETER_AUTO	0xFFu
#define RTL_TE_HOSTPOL_DIR_INGRESS	0u		/* SA host policing */
#define RTL_TE_HOSTPOL_DIR_EGRESS	1u		/* DA host policing */
#define RTL_TE_HOSTPOL_DIRS		2u

/*
 * POL1 PM counters: TE_PM_CTRL (0x9700) resets to 0x00001ca0 with
 * mem_disable=1 (bit 7) and mem_auto_clr_on_read=1 (bit 5); ca-ne.ko
 * aal_l3_te_init() (@0x76f18) enables PM by clearing mem_disable while keeping
 * mem_auto_clr_on_read=1, cnt0=(0,0), cnt1=(1,1).
 *
 * aal_l3_te_pm_policer_flow_get() (@0x753b0) unpacks aal_l3_te_pm_policer_t:
 * cnt[0..3] are 32-bit packet counts (total, red, yellow, green) and cnt[4..7]
 * are 36-bit byte counts (total_bytes, red_bytes, yellow_bytes, green_bytes).
 * In fc_8277c.ko _rtk_fc_hwHostPolingMib_get() (@0x4cdd0), ldp x1, x2, [sp,#88]
 * reads struct offsets +0x10/+0x18 = cnt[4] (total_bytes) and cnt[5]
 * (red_bytes) and accumulates total_bytes - red_bytes.
 */
#define L3TE_PM_CTRL			0x9700u
#define L3TE_PM_CTRL_AUTO_CLR		BIT(5)
#define L3TE_PM_CTRL_DISABLE		BIT(7)
#define L3TE_PM_CTRL_CNT0_EVT_SEL	BIT(8)
#define L3TE_PM_CTRL_CNT0_ID_SEL	BIT(9)
#define L3TE_PM_CTRL_CNT1_EVT_SEL	BIT(10)
#define L3TE_PM_CTRL_CNT1_ID_SEL	BIT(11)

/* Global L3TE PM counters (ca-ne.ko aal_l3_te_pm_stats_get @0x6fee0). */
#define L3TE_PM_PCNT			0x9710u
#define L3TE_PM_PRE_MARK_PCNT		0x9714u
#define L3TE_PM_POL_DROP_PCNT		0x9718u
#define L3TE_PM_TAIL_DROP_PCNT		0x971cu
#define L3TE_PM_WRED_Y_DROP_PCNT	0x9720u
#define L3TE_PM_WRED_G_DROP_PCNT	0x9724u
#define L3TE_PM_MARK_PCNT		0x9728u
#define L3TE_PM_POL_MARK_PCNT		0x972cu
#define L3TE_PM_BYPASS_PCNT		0x9730u
#define L3TE_PM_BYPASS_FLOW_PCNT	0x9734u
#define L3TE_PM_CE_PCNT			0x9738u

#define L3TE_POL_MIB_ACC		0x973cu
#define L3TE_POL_MIB_DATA(n)		(0x9740u + 4u * (n))	/* n = 0..8 */
#define L3TE_POL_MIB_WORDS		9u
#define L3TE_POL_MIB_IDX_BITS		11u
#define L3TE_POL_MIB_C4_HI_MASK		0xfu
#define L3TE_POL_MIB_C4_HI_SHIFT	32
#define L3TE_POL_MIB_C4_LO_SHIFT	4
#define L3TE_POL_MIB_C5_HI_MASK		0xffu
#define L3TE_POL_MIB_C5_HI_SHIFT	28
#define L3TE_POL_MIB_C5_LO_SHIFT	8
#define L3TE_POL_MIB_C6_HI_MASK		0xfffu
#define L3TE_POL_MIB_C6_HI_SHIFT	24
#define L3TE_POL_MIB_C6_LO_SHIFT	12
#define L3TE_POL_MIB_C7_HI_MASK		0xffffu
#define L3TE_POL_MIB_C7_HI_SHIFT	20
#define RTL_TE_HOSTPOL_MIB_WORDS	8u

struct rtl_te_hostpol {
	u8 mac[RTL_TE_HOSTPOL_ALEN];
	bool used;
	/* vmeter 0..31 for a rate limited direction, VMETER_NONE when the
	 * direction only counts (fixed MIB slot instead).
	 */
	u8 vmeter[RTL_TE_HOSTPOL_DIRS];
	/* resolved L3TE POL1 slot per direction */
	u16 pol1[RTL_TE_HOSTPOL_DIRS];
	u32 kbps[RTL_TE_HOSTPOL_DIRS];	/* 0 = counting only */
	u64 rx;				/* accumulated ingress MIB delta */
	u64 tx;				/* accumulated egress MIB delta */
	u64 last_delta[RTL_TE_HOSTPOL_DIRS];	/* cnt[4] - cnt[5] last delta */
};

/* Raw counter snapshot of one direction (aal_l3_te_pm_policer_flow_get). */
struct rtl_te_hostpol_mib {
	u64 cnt[RTL_TE_HOSTPOL_MIB_WORDS];
};

/* ------------------------------------------------------------------ */
/* L2/L3 DoS (phase2-re5-dos-attack.md)                                */
/*                                                                    */
/* Both vendor entry points are masked-blit writers, not per-type      */
/* dispatchers: aal_dos_atk_set() reads the whole L3 block into an     */
/* image, copies the bit ranges selected by field_mask from the        */
/* caller image, then writes the block back in ascending address      */
/* order with a store barrier before every word.  The attack type ->   */
/* bit map itself lives in a kernel module that is not part of the     */
/* shipped package (U10-a), so every type-indexed helper below is      */
/* marked CANDIDATE.                                                   */
/* ------------------------------------------------------------------ */

/* L3FE_PP_DOS_ATK_0..8 */
#define L3TE_DOS_L3_BASE		0x32ecu
#define L3TE_DOS_CFG_REGS		9u
#define L3TE_DOS_CFG(n)			(L3TE_DOS_L3_BASE + 4u * (n)) /* 0..8 */
#define L3TE_DOS_POL_ID_BITS		9u
#define L3TE_DOS_TH_BITS		16u
#define L3TE_DOS_EN_BITS		1u
#define L3TE_DOS_FLOOD_CNT		0x3310u
#define L3TE_DOS_FLOOD_CNT_MASK		GENMASK(10, 0)
#define L3TE_DOS_FLOOD_CNT_DEFAULT	0x3fau
#define L3TE_DOS_EVT_TRAP		0x3314u
#define L3TE_DOS_EVT_DROP		0x3318u

/*
 * dos_type_en: one bit per attack type, BIT(k) for type k of the enum below.
 * L3 keeps it in 0x3304[31:16] (cli-drv __cli_aal_dos_atk_fib_dos_type_en_set
 * writes it with strh, phase2-re9 section 4); writing the full 32 bit word
 * would clobber the land_attacks and smac_eq_dmac fields that live in
 * 0x3304[15:0].
 */
#define L3TE_DOS_TYPE_EN		0x3304u
#define L3TE_DOS_TYPE_EN_SHIFT		16u
#define L3TE_DOS_TYPE_EN_MASK		GENMASK(31, 16)
#define L3TE_DOS_TYPE_BITS		16u
#define L3TE_DOS_TYPE_ALL		GENMASK(15, 0)

/* 64 bit port mask: 0x3308 holds the low half, 0x330c the high half. */
#define L3TE_DOS_PORT_MASK_LO		0x3308u
#define L3TE_DOS_PORT_MASK_HI		0x330cu
#define L3TE_DOS_PORT_MASK_ALL		GENMASK_ULL(63, 0)

/* L2FE_PP_DOS_ATK_0..4 */
#define L3TE_DOS_L2_BASE		0x1220u
#define L3TE_DOS_L2_CFG_REGS		5u
#define L3TE_DOS_L2_CFG(n)		(L3TE_DOS_L2_BASE + 4u * (n)) /* 0..4 */
#define L3TE_DOS_L2_FLOOD_CNT		0x1234u
#define L3TE_DOS_L2_FLOOD_CNT_MASK	GENMASK(9, 0)
#define L3TE_DOS_L2_FLOOD_CNT_DEFAULT	0xffu
#define L3TE_DOS_L2_EVT_TRAP		0x1238u
/* L2 dos_type_en: 0x1228[15:0], same type order and bit numbers as L3. */
#define L3TE_DOS_L2_TYPE_EN		0x1228u
#define L3TE_DOS_L2_TYPE_EN_SHIFT	0u
#define L3TE_DOS_L2_TYPE_EN_MASK	GENMASK(15, 0)
/*
 * L2 input port mask lives inside the mirror: 0x1228[31:16] || 0x122c ||
 * 0x1230[15:0] (mask bit 20, the only 64 bit L2 slice)
 */
#define L3TE_DOS_L2_PORT_MASK_W0	0x1228u
#define L3TE_DOS_L2_PORT_MASK_W1	0x122cu
#define L3TE_DOS_L2_PORT_MASK_W2	0x1230u
#define L3TE_DOS_L2_PORT_MASK_W0_SHIFT	16
#define L3TE_DOS_L2_PORT_MASK_W0_MASK	GENMASK(31, 16)
#define L3TE_DOS_L2_PORT_MASK_W2_SHIFT	48
#define L3TE_DOS_L2_PORT_MASK_W2_MASK	GENMASK(15, 0)
#define L3TE_DOS_PORT_MASK_HALF_MASK	0xffffu

/*
 * trap/drop layout: 0x3314 and 0x3318 are identical, the L2 trap register
 * only has the first three fields.  The CLI exposes six timer knobs: t0 is a
 * single bit at [11] and t1..t5 are four bits each (phase2-re9 section 8).
 */
#define L3TE_DOS_EVT_LDPID		GENMASK(5, 0)
#define L3TE_DOS_EVT_LDPID_SHIFT	0
#define L3TE_DOS_EVT_LDPID_MAX		0x3fu
#define L3TE_DOS_EVT_COS		GENMASK(8, 6)
#define L3TE_DOS_EVT_COS_SHIFT		6
#define L3TE_DOS_EVT_COS_MAX		0x7u
#define L3TE_DOS_EVT_KEEP_ORIG		BIT(9)
#define L3TE_DOS_EVT_KEEP_ORIG_SHIFT	9
/*
 * [10] is the CLI "reserved" field: decoded for the dump, but the OEM setter
 * has no mask bit for it (bit 3 of the vendor mask is unused), so it is never
 * written and rtl_te_dos_evt_set() has no selector for it.
 */
#define L3TE_DOS_EVT_RESERVED		BIT(10)
#define L3TE_DOS_EVT_T0			BIT(11)
#define L3TE_DOS_EVT_T0_SHIFT		11
#define L3TE_DOS_EVT_TIMERS		5u	/* t1..t5 */
#define L3TE_DOS_EVT_TIMER_BASE		12
#define L3TE_DOS_EVT_TIMER_BITS		4
#define L3TE_DOS_EVT_TIMER_MAX		0xfu
#define L3TE_DOS_EVT_TIMER(i)		GENMASK(15 + 4 * (i), 12 + 4 * (i))

/* field selectors for rtl_te_dos_evt_set(); bit 3 is intentionally unused */
#define L3TE_DOS_EVT_F_LDPID		BIT(0)
#define L3TE_DOS_EVT_F_COS		BIT(1)
#define L3TE_DOS_EVT_F_KEEP_ORIG	BIT(2)
#define L3TE_DOS_EVT_F_T0		BIT(4)
#define L3TE_DOS_EVT_F_TIMER(i)	BIT(5 + (i))
#define L3TE_DOS_EVT_F_L2_ALL		(L3TE_DOS_EVT_F_LDPID | \
					 L3TE_DOS_EVT_F_COS | \
					 L3TE_DOS_EVT_F_KEEP_ORIG)
#define L3TE_DOS_EVT_F_TIMERS_ALL	(L3TE_DOS_EVT_F_TIMER(0) | \
					 L3TE_DOS_EVT_F_TIMER(1) | \
					 L3TE_DOS_EVT_F_TIMER(2) | \
					 L3TE_DOS_EVT_F_TIMER(3) | \
					 L3TE_DOS_EVT_F_TIMER(4))
#define L3TE_DOS_EVT_F_ALL		(L3TE_DOS_EVT_F_L2_ALL | \
					 L3TE_DOS_EVT_F_T0 | \
					 L3TE_DOS_EVT_F_TIMERS_ALL)

/*
 * CANDIDATE attack type order (U10-a).  The vendor user-space CLI exposes
 * exactly these 16 names but the module that turns a name into a bit is not
 * part of the package, so the index order here is the candidate S order (the
 * string-pool / source order).  The help-registry order R is its exact
 * reverse; if the device experiment described in the W6c report shows R,
 * reversing this enum is the only change needed, because every consumer goes
 * through the enum.
 *
 * "CANDIDATE" applies to the numeric value, not to the names, which are
 * observed in the OEM CLI.
 */
enum rtl_dos_type {
	RTL_DOS_TYPE_ICMP_FLOOD = 0,
	RTL_DOS_TYPE_FIN_FLOOD,
	RTL_DOS_TYPE_SYN_FLOOD,
	RTL_DOS_TYPE_SYN_WITH_DATA,
	RTL_DOS_TYPE_UDP_BOMB,
	RTL_DOS_TYPE_PING_OF_DEATH,
	RTL_DOS_TYPE_ICMP_FRAGMENT,
	RTL_DOS_TYPE_TCP_FRAG_ERROR,
	RTL_DOS_TYPE_TCP_SHORT_HDR,
	RTL_DOS_TYPE_SYN1024,
	RTL_DOS_TYPE_NULL_SCAN,
	RTL_DOS_TYPE_XMA_SCAN,
	RTL_DOS_TYPE_SYN_FIN_SCAN,
	RTL_DOS_TYPE_BLAT_ATTACKS,
	RTL_DOS_TYPE_LAND_ATTACKS,
	RTL_DOS_TYPE_SMAC_EQ_DMAC,
	RTL_DOS_TYPE_MAX,
};

/* trap/drop programming image; fields not selected by field_mask are kept */
struct rtl_te_dos_evt {
	u8 ldpid;		/* [5:0]  */
	u8 cos;			/* [8:6]  */
	bool keep_orig;		/* [9]    */
	bool reserved;		/* [10], decoded only, never written */
	bool t0;		/* [11], single bit */
	u8 timer[L3TE_DOS_EVT_TIMERS];	/* [15:12] .. [31:28], t1..t5 */
};

/*
 * A policer profile entry as unpacked from the 3 word table entry
 * (spec sections 2.2/2.3). Burst units are whatever the caller passes; the vendor
 * callers pass bytes (inferred, not observed).
 */
struct rtl_te_policer_profile {
	u16 cir_lo;	/* CIR % 500           -> DATA0[9:0]		 */
	u16 cir_hi;	/* CIR / 500 (kbps)    -> DATA0[25:10]		 */
	u16 cbs;	/* 1..4095             -> DATA0[31:26] | DATA1[5:0] */
	u16 pir_lo;	/* PIR % 500           -> DATA1[15:6]		 */
	u16 pir_hi;	/* PIR / 500 (kbps)    -> DATA1[31:16]		 */
	u16 pbs;	/* 1..4095             -> DATA2[11:0]		 */
};

struct rtl_te_flow {
	struct device *dev;
	struct regmap *regs;
	spinlock_t lock;
	/* The vendor keeps two independent locks so L2 and L3 DoS programming
	 * can run concurrently; the register blocks do not overlap.
	 */
	spinlock_t dos_l3_lock;
	spinlock_t dos_l2_lock;
	DECLARE_BITMAP(flow_slots, L3TE_POL_FLOW_SLOTS);
	DECLARE_BITMAP(group_slots, L3TE_POL_GRP_SLOTS);
	struct dentry *debugfs;
	bool dos_enabled;
	/* Cached NE 0x9004 bit25 state: false until the 64 slot group table has
	 * been enabled, and set from the reset value by rtl_te_flow_init()
	 * without writing anything.
	 */
	bool group64_ready;
	/* Cached L3TE policer global enable state (NE 0x9004 bit0, 0x9180,
	 * 0x30ac): enabled lazily on the first flow/group/host policer add.
	 */
	bool policer_ready;
	/* Cached TE_PM_CTRL (0x9700) state: false until the first host policing
	 * add or MIB read lazily clears mem_disable and sets auto_clr/cnt sels.
	 */
	bool pm_ready;
	/* Optional FE callback invoked after hostpol rules change (outside
	 * ->lock) so already-offloaded FE flows refresh their POL_VLD/EN/ID.
	 */
	void (*hostpol_sync)(void *data);
	void *hostpol_sync_data;
	/* Host policing table, guarded by ->lock together with the bitmaps
	 * above because both allocate from the same 256 slot POL1 table.
	 */
	struct rtl_te_hostpol hostpol[RTL_TE_HOSTPOL_ENTRIES];
};

int rtl_te_flow_init(struct rtl9617c_ni *ni);
void rtl_te_flow_exit(struct rtl9617c_ni *ni);
void rtl_te_flow_set_sync(struct rtl_te_flow *te, void (*sync)(void *data),
			  void *data);

int rtl_te_policer_alloc(struct rtl_te_flow *te, u64 cir_bps, u64 pir_bps,
			 u32 cbs_bytes, u32 pbs_bytes, u8 *slot);
int rtl_te_policer_update(struct rtl_te_flow *te, u8 slot, u64 cir_bps,
			  u64 pir_bps, u32 cbs_bytes, u32 pbs_bytes);
int rtl_te_policer_free(struct rtl_te_flow *te, u8 slot);

int rtl_te_policer_group_alloc(struct rtl_te_flow *te, u64 cir_bps,
			       u64 pir_bps, u32 cbs_bytes, u32 pbs_bytes,
			       u8 *slot);
int rtl_te_policer_group_update(struct rtl_te_flow *te, u8 slot, u64 cir_bps,
				u64 pir_bps, u32 cbs_bytes, u32 pbs_bytes);
int rtl_te_policer_group_free(struct rtl_te_flow *te, u8 slot);

int rtl_te_wred_profile_write(struct rtl_te_flow *te, u8 profile,
			      const u32 words[8]);
int rtl_te_wred_profile_read(struct rtl_te_flow *te, u8 profile, u32 words[8]);
int rtl_te_wred_bind_field(struct rtl_te_flow *te, u8 queue, u8 field, u8 profile);
int rtl_te_wred_bind(struct rtl_te_flow *te, u8 queue, u8 profile);

/*
 * L3/L2 DoS register layer.
 *
 * rtl_te_dos_l3_set()/rtl_te_dos_l2_set() are the vendor masked-blit
 * operation: only the slices selected by field_mask are taken from img[],
 * every other bit keeps its live value, and the block is written back in
 * ascending address order.  Bit i of field_mask is mask bit i of
 * phase2-re5-dos-attack.md section 2.1 (L3, 0..36) / section 7.1 (L2, 0..20).
 */
int rtl_te_dos_l3_get(struct rtl_te_flow *te, u32 img[L3TE_DOS_CFG_REGS]);
int rtl_te_dos_l3_set(struct rtl_te_flow *te, u64 field_mask,
		      const u32 img[L3TE_DOS_CFG_REGS]);
int rtl_te_dos_l2_get(struct rtl_te_flow *te, u32 img[L3TE_DOS_L2_CFG_REGS]);
int rtl_te_dos_l2_set(struct rtl_te_flow *te, u64 field_mask,
		      const u32 img[L3TE_DOS_L2_CFG_REGS]);

int rtl_te_dos_flood_get(struct rtl_te_flow *te, bool l2, u32 *count);
int rtl_te_dos_flood_clear(struct rtl_te_flow *te, bool l2);
int rtl_te_dos_flood_unit_set(struct rtl_te_flow *te, bool l2, u32 unit);

/* Attack type enable/action helpers (verified against ca-rtk.ko DAL). */
int rtl_te_dos_type_en_get(struct rtl_te_flow *te, bool l2, u32 *bitmap);
int rtl_te_dos_type_en_set(struct rtl_te_flow *te, bool l2, u32 bitmap);
int rtl_te_dos_type_set_action(struct rtl_te_flow *te, bool l2,
			       enum rtl_dos_type type, bool enable, bool trap);
int rtl_te_dos_type_set(struct rtl_te_flow *te, bool l2,
			enum rtl_dos_type type, bool enable);

/*
 * Per-type fields (phase2-re9 sections 3/5/9): every type has a 9 bit pol_id
 * (L3 only) and a 1 bit enable; the three flood types also have a 16 bit
 * threshold.  Fields that cross a register boundary (fin/syn/udp_bomb/
 * xma_scan/land_attacks pol_id, syn_flood threshold) are read-modify-written
 * through the 64 bit (reg, reg+4) window, so neighbouring fields survive.
 */
int rtl_te_dos_pol_id_set(struct rtl_te_flow *te, enum rtl_dos_type type,
			  u32 pol_id);
int rtl_te_dos_th_set(struct rtl_te_flow *te, bool l2, enum rtl_dos_type type,
		      u32 th);

int rtl_te_dos_port_mask_get(struct rtl_te_flow *te, bool l2, u64 *mask);
int rtl_te_dos_port_mask_set(struct rtl_te_flow *te, bool l2, u64 mask);

int rtl_te_dos_evt_get(struct rtl_te_flow *te, bool l2, bool drop, u32 *raw);
int rtl_te_dos_evt_set(struct rtl_te_flow *te, bool l2, bool drop,
		       u32 field_mask, const struct rtl_te_dos_evt *evt);

/*
 * Host policing (MAC keyed, CIR = PIR, kbps).
 *
 * rtl_te_hostpol_add() takes the vmeters it needs (one per rate limited
 * direction, none for a counting direction), programs the two POL1 slots,
 * and marks nothing in the flow bitmap: POL1 0xA0..0xFF is reserved
 * wholesale at init.  A rate of 0 or RTL_TE_HOSTPOL_RATE_MAX means "count
 * only".  An existing MAC is refused with -EEXIST; use _vm to pin the
 * virtual meters (the vendor lets both directions share one meter).
 */
int rtl_te_hostpol_add(struct rtl_te_flow *te, const u8 mac[RTL_TE_HOSTPOL_ALEN],
		       u32 ingress_kbps, u32 egress_kbps);
int rtl_te_hostpol_add_vmeter(struct rtl_te_flow *te,
			      const u8 mac[RTL_TE_HOSTPOL_ALEN],
			      u32 ingress_kbps, u32 egress_kbps,
			      u8 ingress_vmeter, u8 egress_vmeter);
int rtl_te_hostpol_del(struct rtl_te_flow *te, const u8 mac[RTL_TE_HOSTPOL_ALEN]);

/*
 * dir: RTL_TE_HOSTPOL_DIR_INGRESS resolves the SA-side POL1 slot (0xA0 +
 * vmeter when rate limited, or the ingress MIB slot 0xC0 + entry when counting
 * only); RTL_TE_HOSTPOL_DIR_EGRESS resolves the DA-side POL1 slot (0xA0 +
 * vmeter when rate limited, or the egress MIB slot 0xE0 + entry when counting
 * only).  Returns -ENOENT for an unknown MAC.
 */
int rtl_te_hostpol_lookup(struct rtl_te_flow *te, const u8 mac[RTL_TE_HOSTPOL_ALEN],
			  u8 dir, u8 *slot);
int rtl_te_hostpol_lookup_ex(struct rtl_te_flow *te,
			     const u8 mac[RTL_TE_HOSTPOL_ALEN], u8 dir, u8 *slot,
			     bool *rate_limited);

/* One direction of raw counters, and the vendor rx/tx accumulation. */
int rtl_te_hostpol_mib_raw(struct rtl_te_flow *te, u8 entry, u8 dir,
			   struct rtl_te_hostpol_mib *mib);
int rtl_te_hostpol_mib_get(struct rtl_te_flow *te, u8 entry, u64 *rx, u64 *tx);
/* Reads the hardware counters once (accumulating them) and then zeroes the
 * software accumulation; the vendor never writes a hardware clear register.
 */
int rtl_te_hostpol_mib_clear(struct rtl_te_flow *te, u8 entry);

/*
 * Disarm: write all-zero L3 and L2 DoS images.  enable == true is accepted for
 * API compatibility and writes nothing, because arming needs explicit
 * thresholds, attack types, and port masks.
 */
int rtl_te_dos_set(struct rtl_te_flow *te, bool enable);
void rtl_te_flow_debugfs_exit(struct rtl_te_flow *te);

#endif /* RTL9617C_NI_TE_FLOW_H */
