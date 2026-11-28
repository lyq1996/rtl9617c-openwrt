// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * L2FE Hardware FDB, Membership/STP/Isolation, MY_MAC, and HW VLAN offload
 * for bridged RTL9617C LAN ports.
 *
 * Scoped strictly to Linux-bridged LAN ports (GE 0..4 and P6 id 6). pon0
 * (id 7), standalone unbridged ports, L3FE bypass/FE fastpath, and DEEPQ
 * remain completely untouched.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/switchdev.h>

#include "../rtl9617c_regs.h"
#include "ni.h"
#include "ni_ext.h"

#define L2FE_POLL_RETRIES		2000
#define L2FE_ACC_BUSY			BIT(31)
#define L2FE_ACC_RBW_WRITE		BIT(30)

/*
 * Legacy L2FE switchdev bridge offload is disabled by default:
 *   - All bridged LAN ports must keep BYPASS_L2FE=1 (DPID 25 -> L3FE) because
 *     ni_qm.c programs L2FE PONCLE catch-all rules to trap LSPID 0..4,6 -> CPU0,
 *     and fe_main.c + fe_bridge.c already offload both L3 routing and L2
 *     bridging inside L3FE.
 *   - L2FE_L2E_ACC (0x1ca0) is also owned by fe_main.c for L3 next-hop MAC_DA
 *     indices under fe->lock; concurrent unlocked switchdev FDB deletes would
 *     corrupt active L3FE flows.
 */
static bool rtl_ni_l2fe_bridge_offload;
module_param_named(l2fe_bridge, rtl_ni_l2fe_bridge_offload, bool, 0444);
MODULE_PARM_DESC(l2fe_bridge,
		 "Enable legacy L2FE switchdev bridge offload (default 0; keep off when L3FE is active)");

/* Port RX L2FE-bypass CSRs */
#define NI_PT_RX_CTRL_REG(id)		(0xa5f8U + (id) * 0x90U)
#define NI_PT_RX_CTRL_BYPASS_L2FE	BIT(28)
#define NI_XGE6_RX_BYPASS_REG		RTL_NI_XGE_BYPASS_CFG
#define NI_XGE6_RX_BYPASS_L2FE		RTL_NI_XGE_BYPASS_EN

/* L2FE PP ILPB indirect table (word1 bits [14:3] hold default_vid) */
#define L2FE_ILPB_ACC			0x11e4U
#define L2FE_ILPB_DATA0			0x11e8U
#define L2FE_ILPB_DATA1			0x11ecU
#define L2FE_ILPB_DATA2			0x11f0U
#define L2FE_ILPB_DATA3			0x11f4U
#define L2FE_ILPB_DATA4			0x11f8U
#define L2FE_ILPB_DFT_VID_MASK		GENMASK(14, 3)

/* L2FE VE global check & VLAN indirect tables */
#define L2FE_VE_GLB_CTRL		0x1500U
#define L2FE_VE_LAN_CHK_EN		BIT(0)
#define L2FE_VE_FIB_ACC			0x1a0cU
#define L2FE_VE_FIB_DATA1		0x1a10U
#define L2FE_VE_FIB_DATA0		0x1a14U
#define L2FE_VE_VID_ACC			0x1a20U
#define L2FE_VE_VID_DATA		0x1a24U

/* L2FE PLE port membership check (BIT(p) = 1 ALLOWS forwarding) */
#define L2FE_PLE_MMSHP_ACC		0x1574U
#define L2FE_PLE_MMSHP_DATA1		0x1578U
#define L2FE_PLE_MMSHP_DATA0		0x157cU

/* L2FE PE MY_MAC table (8 x 64-bit direct slots) */
#define L2FE_MY_MAC_SLOTS		8
#define L2FE_PE_MY_MAC_LOW(idx)		(0x1714U + (idx) * 8U)
#define L2FE_PE_MY_MAC_HIGH(idx)	(0x1718U + (idx) * 8U)

/* L2FE L2E FDB indirect access */
#define L2FE_L2E_STS			0x1c2cU
#define L2FE_L2E_STS_OK			0x5U
#define L2FE_L2E_ACC			0x1ca0U
#define L2FE_L2E_DATA3			0x1ca4U
#define L2FE_L2E_DATA2			0x1ca8U
#define L2FE_L2E_DATA1			0x1cacU
#define L2FE_L2E_DATA0			0x1cb0U
#define L2FE_L2E_CMD_ADD		0x45U
#define L2FE_L2E_CMD_DEL		0x85U

/* Stock L2FE_VE_FIB vlan_memship_map bit indices (NOT L2FE_PLE_MMSHP LDPIDs) */
#define L2FE_CPU0_LDPID			16U
#define L2FE_CPU0_BM_BIT		8U
#define L2FE_L3FE_BM_BIT		15U
#define L2FE_LAN_BM_MASK		0x5fU	/* bits 0..4 (GE) + bit 6 (P6) */
#define L2FE_NON_LAN_LOW_MASK		0x80U	/* preserve non-LAN bits except 0..7 */

#define L2FE_NUM_PORT_SLOTS		7U	/* GE 0..4, gap 5, P6 6 */
#define L2FE_VLAN_FIB_BASE		64U
#define L2FE_VLAN_SLOTS			64U
#define L2FE_MAX_SW_FDB			2048U

#define L2FE_SUPPORTED_BR_FLAGS		(BR_ISOLATED | BR_LEARNING | \
					 BR_FLOOD | BR_MCAST_FLOOD | \
					 BR_BCAST_FLOOD | BR_HAIRPIN_MODE)

struct rtl_l2fe_port_state {
	struct net_device *dev;
	struct net_device *master;
	bool registered;
	bool bridged;
	u8 stp_state;
	bool isolated;
	bool hairpin;
	bool learning;
	bool orig_mmshp_saved;
	u32 orig_mmshp_hi;
	u32 orig_mmshp_lo;
	bool ilpb_vid_saved;
	u16 orig_dft_vid;
	u16 br_pvid;
};

struct rtl_l2fe_bridge_state {
	struct net_device *dev;
	bool vlan_filtering;
	u8 mac[ETH_ALEN];
};

struct rtl_l2fe_mymac_slot {
	bool occupied;
	bool pinned;	/* Pre-populated outside ni_l2fe; never evict */
	u8 mac[ETH_ALEN];
};

struct rtl_l2fe_vlan_slot {
	bool used;
	u16 vid;
	u16 br_member_bm;
	u16 br_untag_bm;
	u16 ndo_member_bm;
	bool br_cpu;
	bool br_cpu_untag;
};

struct rtl_l2fe_fdb_entry {
	struct list_head list;
	u8 mac[ETH_ALEN];
	u16 key_vid;
	u16 orig_vid;
	u16 ldpid;
	int ifindex;
	bool is_static;
	bool is_local;
};

struct rtl_l2fe_fdb_work {
	struct work_struct work;
	struct net_device *dev;
	unsigned long event;
	u8 mac[ETH_ALEN];
	u16 vid;
	bool added_by_user;
	bool is_local;
};

static struct {
	struct rtl9617c_ni *ni;
	bool initialized;
	bool notifiers_registered;
	bool cpu0_mmshp_checked;
	spinlock_t lock;
	struct workqueue_struct *wq;
	struct work_struct reg_work;
	struct rtl_l2fe_port_state ports[L2FE_NUM_PORT_SLOTS];
	struct rtl_l2fe_bridge_state bridges[L2FE_NUM_PORT_SLOTS];
	struct rtl_l2fe_mymac_slot my_mac[L2FE_MY_MAC_SLOTS];
	struct rtl_l2fe_vlan_slot vlans[L2FE_VLAN_SLOTS];
	struct list_head fdb_list;
	u32 fdb_count;
} l2fe_st = {
	.lock = __SPIN_LOCK_UNLOCKED(l2fe_st.lock),
	.fdb_list = LIST_HEAD_INIT(l2fe_st.fdb_list),
};

static bool rtl_l2fe_is_lan_id(unsigned int id)
{
	return id < RTL_NI_GE_PORTS || id == RTL_NI_PORT_P6;
}

static bool rtl_l2fe_is_ni_port_dev(const struct net_device *dev)
{
	return dev && dev->netdev_ops &&
	       dev->netdev_ops->ndo_get_stats64 == rtl_ni_get_stats64;
}

static struct rtl9617c_ni_port *
rtl_l2fe_get_lan_port(const struct net_device *dev)
{
	struct rtl9617c_ni_port *port;

	if (!rtl_l2fe_is_ni_port_dev(dev))
		return NULL;

	port = netdev_priv(dev);
	if (!port || !rtl_l2fe_is_lan_id(port->id))
		return NULL;

	return port;
}

static bool rtl_l2fe_port_dev_check(const struct net_device *dev)
{
	return rtl_l2fe_get_lan_port(dev) != NULL;
}

static int l2fe_wait_busy(struct rtl9617c_ni *ni, u32 reg)
{
	int retries = L2FE_POLL_RETRIES;

	while (rtl_ni_read(ni, reg) & L2FE_ACC_BUSY) {
		if (!--retries)
			return -ETIMEDOUT;
		udelay(1);
	}

	return 0;
}

static bool rtl_l2fe_has_bridge_locked(struct net_device *br_dev)
{
	unsigned int i;

	if (!br_dev)
		return false;

	for (i = 0; i < L2FE_NUM_PORT_SLOTS; i++) {
		if (rtl_l2fe_is_lan_id(i) && l2fe_st.ports[i].bridged &&
		    l2fe_st.ports[i].master == br_dev)
			return true;
	}

	return false;
}

static struct rtl_l2fe_bridge_state *
rtl_l2fe_get_bridge_locked(struct net_device *br_dev, bool create)
{
	int free_idx = -1;
	unsigned int i;

	if (!br_dev)
		return NULL;

	for (i = 0; i < L2FE_NUM_PORT_SLOTS; i++) {
		if (l2fe_st.bridges[i].dev == br_dev)
			return &l2fe_st.bridges[i];
		if (!l2fe_st.bridges[i].dev && free_idx < 0)
			free_idx = i;
	}

	if (!create || free_idx < 0)
		return NULL;

	memset(&l2fe_st.bridges[free_idx], 0,
	       sizeof(l2fe_st.bridges[free_idx]));
	l2fe_st.bridges[free_idx].dev = br_dev;
	ether_addr_copy(l2fe_st.bridges[free_idx].mac, br_dev->dev_addr);
	return &l2fe_st.bridges[free_idx];
}

static void rtl_l2fe_prune_bridges_locked(void)
{
	unsigned int i;

	for (i = 0; i < L2FE_NUM_PORT_SLOTS; i++) {
		if (l2fe_st.bridges[i].dev &&
		    !rtl_l2fe_has_bridge_locked(l2fe_st.bridges[i].dev))
			memset(&l2fe_st.bridges[i], 0,
			       sizeof(l2fe_st.bridges[i]));
	}
}

/* Indirect MMSHP read/write */
static int l2fe_mmshp_read_locked(struct rtl9617c_ni *ni, u32 lspid,
				  u32 *hi, u32 *lo)
{
	int ret;

	ret = l2fe_wait_busy(ni, L2FE_PLE_MMSHP_ACC);
	if (ret)
		return ret;

	rtl_ni_write(ni, L2FE_PLE_MMSHP_ACC, lspid | L2FE_ACC_BUSY);
	ret = l2fe_wait_busy(ni, L2FE_PLE_MMSHP_ACC);
	if (ret)
		return ret;

	*hi = rtl_ni_read(ni, L2FE_PLE_MMSHP_DATA1);
	*lo = rtl_ni_read(ni, L2FE_PLE_MMSHP_DATA0);
	return 0;
}

static int l2fe_mmshp_write_locked(struct rtl9617c_ni *ni, u32 lspid,
				   u32 hi, u32 lo)
{
	int ret;

	ret = l2fe_wait_busy(ni, L2FE_PLE_MMSHP_ACC);
	if (ret)
		return ret;

	rtl_ni_write(ni, L2FE_PLE_MMSHP_DATA1, hi);
	rtl_ni_write(ni, L2FE_PLE_MMSHP_DATA0, lo);
	rtl_ni_write(ni, L2FE_PLE_MMSHP_ACC,
		     lspid | L2FE_ACC_RBW_WRITE | L2FE_ACC_BUSY);
	return l2fe_wait_busy(ni, L2FE_PLE_MMSHP_ACC);
}

/* Indirect ELPB egress STP status RMW (DATA0 BIT(1)) */
static int l2fe_elpb_set_stp_locked(struct rtl9617c_ni *ni, u32 ldpid,
				    bool forward)
{
	u32 d1, d0, new_d0;
	int ret;

	ret = l2fe_wait_busy(ni, RTL_L2FE_PLC_ELPB_ACCESS);
	if (ret)
		return ret;

	rtl_ni_write(ni, RTL_L2FE_PLC_ELPB_ACCESS, ldpid | L2FE_ACC_BUSY);
	ret = l2fe_wait_busy(ni, RTL_L2FE_PLC_ELPB_ACCESS);
	if (ret)
		return ret;

	d1 = rtl_ni_read(ni, RTL_L2FE_PLC_ELPB_DATA1);
	d0 = rtl_ni_read(ni, RTL_L2FE_PLC_ELPB_DATA0);
	new_d0 = forward ? (d0 | RTL_L2FE_PLC_ELPB_STP_STATUS) :
			   (d0 & ~RTL_L2FE_PLC_ELPB_STP_STATUS);
	if (new_d0 == d0)
		return 0;

	rtl_ni_write(ni, RTL_L2FE_PLC_ELPB_DATA1, d1);
	rtl_ni_write(ni, RTL_L2FE_PLC_ELPB_DATA0, new_d0);
	rtl_ni_write(ni, RTL_L2FE_PLC_ELPB_ACCESS,
		     ldpid | L2FE_ACC_RBW_WRITE | L2FE_ACC_BUSY);
	return l2fe_wait_busy(ni, RTL_L2FE_PLC_ELPB_ACCESS);
}

/* Indirect ILPB default_vid read/RMW (word1 bits [14:3]) */
static int l2fe_ilpb_read_dft_vid_locked(struct rtl9617c_ni *ni, u32 ldpid,
					  u16 *vid)
{
	int ret;
	u32 w1;

	ret = l2fe_wait_busy(ni, L2FE_ILPB_ACC);
	if (ret)
		return ret;

	rtl_ni_write(ni, L2FE_ILPB_ACC, ldpid | L2FE_ACC_BUSY);
	ret = l2fe_wait_busy(ni, L2FE_ILPB_ACC);
	if (ret)
		return ret;

	w1 = rtl_ni_read(ni, L2FE_ILPB_DATA1);
	*vid = FIELD_GET(L2FE_ILPB_DFT_VID_MASK, w1);
	return 0;
}

static int l2fe_ilpb_write_dft_vid_locked(struct rtl9617c_ni *ni, u32 ldpid,
					   u16 vid)
{
	u32 w0, w1, w2, w3, w4;
	int ret;

	ret = l2fe_wait_busy(ni, L2FE_ILPB_ACC);
	if (ret)
		return ret;

	rtl_ni_write(ni, L2FE_ILPB_ACC, ldpid | L2FE_ACC_BUSY);
	ret = l2fe_wait_busy(ni, L2FE_ILPB_ACC);
	if (ret)
		return ret;

	w0 = rtl_ni_read(ni, L2FE_ILPB_DATA0);
	w1 = rtl_ni_read(ni, L2FE_ILPB_DATA1);
	w2 = rtl_ni_read(ni, L2FE_ILPB_DATA2);
	w3 = rtl_ni_read(ni, L2FE_ILPB_DATA3);
	w4 = rtl_ni_read(ni, L2FE_ILPB_DATA4);

	w1 = (w1 & ~L2FE_ILPB_DFT_VID_MASK) |
	     FIELD_PREP(L2FE_ILPB_DFT_VID_MASK, vid & 0xfffU);

	rtl_ni_write(ni, L2FE_ILPB_DATA0, w0);
	rtl_ni_write(ni, L2FE_ILPB_DATA1, w1);
	rtl_ni_write(ni, L2FE_ILPB_DATA2, w2);
	rtl_ni_write(ni, L2FE_ILPB_DATA3, w3);
	rtl_ni_write(ni, L2FE_ILPB_DATA4, w4);
	rtl_ni_write(ni, L2FE_ILPB_ACC,
		     ldpid | L2FE_ACC_RBW_WRITE | L2FE_ACC_BUSY);
	return l2fe_wait_busy(ni, L2FE_ILPB_ACC);
}

static void l2fe_ensure_orig_ilpb_locked(struct rtl9617c_ni *ni, unsigned int p)
{
	struct rtl_l2fe_port_state *ps = &l2fe_st.ports[p];
	u16 dft_vid = 0;

	if (ps->ilpb_vid_saved || !rtl_l2fe_is_lan_id(p))
		return;

	if (!l2fe_ilpb_read_dft_vid_locked(ni, p, &dft_vid)) {
		ps->orig_dft_vid = dft_vid;
		ps->ilpb_vid_saved = true;
	}
}

/* PE MY_MAC synchronization (preserves pre-existing pinned HW entries) */
static void l2fe_mymac_write_slot_locked(struct rtl9617c_ni *ni, unsigned int idx,
					 const u8 *mac)
{
	u32 low, high;

	if (mac) {
		low = ((u32)mac[2] << 24) | ((u32)mac[3] << 16) |
		      ((u32)mac[4] << 8) | mac[5];
		high = ((u32)mac[0] << 8) | mac[1];
	} else {
		low = 0;
		high = 0;
	}

	rtl_ni_write(ni, L2FE_PE_MY_MAC_LOW(idx), low);
	rtl_ni_write(ni, L2FE_PE_MY_MAC_HIGH(idx), high);
}

static void l2fe_mymac_snapshot_hw_locked(struct rtl9617c_ni *ni)
{
	unsigned int i;

	for (i = 0; i < L2FE_MY_MAC_SLOTS; i++) {
		u32 low = rtl_ni_read(ni, L2FE_PE_MY_MAC_LOW(i));
		u32 high = rtl_ni_read(ni, L2FE_PE_MY_MAC_HIGH(i)) & 0xffffU;

		if (low || high) {
			l2fe_st.my_mac[i].occupied = true;
			l2fe_st.my_mac[i].pinned = true;
			l2fe_st.my_mac[i].mac[0] = (high >> 8) & 0xff;
			l2fe_st.my_mac[i].mac[1] = high & 0xff;
			l2fe_st.my_mac[i].mac[2] = (low >> 24) & 0xff;
			l2fe_st.my_mac[i].mac[3] = (low >> 16) & 0xff;
			l2fe_st.my_mac[i].mac[4] = (low >> 8) & 0xff;
			l2fe_st.my_mac[i].mac[5] = low & 0xff;
		}
	}
}

static void l2fe_mymac_push_unique(u8 target[][ETH_ALEN], unsigned int *cnt,
				   const u8 *mac)
{
	unsigned int i;

	if (!mac || !is_valid_ether_addr(mac) || *cnt >= L2FE_MY_MAC_SLOTS)
		return;

	for (i = 0; i < *cnt; i++) {
		if (ether_addr_equal(target[i], mac))
			return;
	}

	ether_addr_copy(target[*cnt], mac);
	(*cnt)++;
}

static void l2fe_mymac_sync_locked(void)
{
	struct rtl9617c_ni *ni = l2fe_st.ni;
	u8 target[L2FE_MY_MAC_SLOTS][ETH_ALEN];
	bool keep[L2FE_MY_MAC_SLOTS] = {};
	bool placed[L2FE_MY_MAC_SLOTS] = {};
	unsigned int target_cnt = 0;
	unsigned int i, j;

	if (!ni)
		return;

	for (i = 0; i < L2FE_NUM_PORT_SLOTS; i++) {
		if (l2fe_st.bridges[i].dev)
			l2fe_mymac_push_unique(target, &target_cnt,
					       l2fe_st.bridges[i].mac);
	}

	for (i = 0; i < L2FE_NUM_PORT_SLOTS; i++) {
		struct rtl_l2fe_port_state *ps = &l2fe_st.ports[i];

		if (rtl_l2fe_is_lan_id(i) && ps->bridged && ps->dev)
			l2fe_mymac_push_unique(target, &target_cnt,
					       ps->dev->dev_addr);
	}

	for (i = 0; i < L2FE_MY_MAC_SLOTS; i++) {
		if (l2fe_st.my_mac[i].pinned) {
			keep[i] = true;
			for (j = 0; j < target_cnt; j++) {
				if (ether_addr_equal(l2fe_st.my_mac[i].mac,
						     target[j]))
					placed[j] = true;
			}
			continue;
		}

		if (!l2fe_st.my_mac[i].occupied)
			continue;

		for (j = 0; j < target_cnt; j++) {
			if (!placed[j] &&
			    ether_addr_equal(l2fe_st.my_mac[i].mac,
					     target[j])) {
				keep[i] = true;
				placed[j] = true;
				break;
			}
		}
	}

	for (i = 0; i < L2FE_MY_MAC_SLOTS; i++) {
		if (!keep[i] && l2fe_st.my_mac[i].occupied) {
			l2fe_st.my_mac[i].occupied = false;
			memset(l2fe_st.my_mac[i].mac, 0, ETH_ALEN);
			l2fe_mymac_write_slot_locked(ni, i, NULL);
		}
	}

	for (j = 0; j < target_cnt; j++) {
		if (placed[j])
			continue;

		for (i = 0; i < L2FE_MY_MAC_SLOTS; i++) {
			if (!l2fe_st.my_mac[i].occupied) {
				l2fe_st.my_mac[i].occupied = true;
				l2fe_st.my_mac[i].pinned = false;
				ether_addr_copy(l2fe_st.my_mac[i].mac,
						target[j]);
				l2fe_mymac_write_slot_locked(ni, i, target[j]);
				placed[j] = true;
				break;
			}
		}
	}
}

static bool l2fe_port_l2_active(const struct rtl_l2fe_port_state *ps)
{
	return ps->registered && ps->bridged && ps->dev &&
	       netif_running(ps->dev) &&
	       ps->stp_state == BR_STATE_FORWARDING;
}

static void l2fe_set_rx_bypass_locked(struct rtl9617c_ni *ni, unsigned int p,
				      bool bypass)
{
	/* Never divert LAN ports out of L3FE bypass (DPID 25) when FE is present */
	if (!bypass && ni->fe)
		return;

	if (p < RTL_NI_GE_PORTS) {
		rtl_ni_rmw(ni, NI_PT_RX_CTRL_REG(p),
			   NI_PT_RX_CTRL_BYPASS_L2FE,
			   bypass ? NI_PT_RX_CTRL_BYPASS_L2FE : 0U);
	} else if (p == RTL_NI_PORT_P6) {
		rtl_ni_rmw(ni, NI_XGE6_RX_BYPASS_REG,
			   NI_XGE6_RX_BYPASS_L2FE,
			   bypass ? NI_XGE6_RX_BYPASS_L2FE : 0U);
	}
}

static void l2fe_sync_ports_hw_locked(void)
{
	struct rtl9617c_ni *ni = l2fe_st.ni;
	bool any_vlan_filter = false;
	unsigned int p, q;

	if (!ni)
		return;

	for (p = 0; p < L2FE_NUM_PORT_SLOTS; p++) {
		if (l2fe_st.bridges[p].dev && l2fe_st.bridges[p].vlan_filtering)
			any_vlan_filter = true;
	}

	/* Only toggle VE LAN check BIT(0); WAN/PON BIT(1) stays untouched */
	rtl_ni_rmw(ni, L2FE_VE_GLB_CTRL, L2FE_VE_LAN_CHK_EN,
		   any_vlan_filter ? L2FE_VE_LAN_CHK_EN : 0U);

	if (!l2fe_st.cpu0_mmshp_checked) {
		u32 chi, clo;

		if (!l2fe_mmshp_read_locked(ni, L2FE_CPU0_LDPID, &chi, &clo)) {
			if (!chi && !clo)
				l2fe_mmshp_write_locked(ni, L2FE_CPU0_LDPID,
							~0U,
							~(1U << L2FE_CPU0_LDPID));
			l2fe_st.cpu0_mmshp_checked = true;
		}
	}

	for (p = 0; p < L2FE_NUM_PORT_SLOTS; p++) {
		struct rtl_l2fe_port_state *ps = &l2fe_st.ports[p];
		bool active;
		u32 hi, lo;

		if (!rtl_l2fe_is_lan_id(p) || !ps->registered)
			continue;

		if (!ps->orig_mmshp_saved &&
		    !l2fe_mmshp_read_locked(ni, p, &ps->orig_mmshp_hi,
					    &ps->orig_mmshp_lo)) {
			/*
			 * L2FE_PLE_PORT_MMSHP_CHK resets to 0 (all LDPIDs
			 * blocked); match stock aal_port_init (~(1ULL << p))
			 * if hardware was left uninitialized.
			 */
			if (!ps->orig_mmshp_hi && !ps->orig_mmshp_lo) {
				ps->orig_mmshp_hi = ~0U;
				ps->orig_mmshp_lo = ~(1U << p);
			}
			ps->orig_mmshp_saved = true;
		}

		l2fe_ensure_orig_ilpb_locked(ni, p);

		active = l2fe_port_l2_active(ps);

		/* Bridged forwarding LAN ports enter L2FE; others bypass L2FE */
		l2fe_set_rx_bypass_locked(ni, p, !active);

		if (ps->orig_mmshp_saved) {
			hi = ps->orig_mmshp_hi ? ps->orig_mmshp_hi : ~0U;
			if (!ps->bridged) {
				lo = ps->orig_mmshp_lo;
			} else {
				/* Allow all non-LAN LDPIDs (CPU0 16, L3FE 25, PON 7, WAN 24, etc.) */
				lo = (ps->orig_mmshp_lo | ~L2FE_LAN_BM_MASK) &
				     ~L2FE_LAN_BM_MASK;

				if (active) {
					for (q = 0; q < L2FE_NUM_PORT_SLOTS; q++) {
						struct rtl_l2fe_port_state *pq =
							&l2fe_st.ports[q];

						if (!rtl_l2fe_is_lan_id(q))
							continue;
						if (q == p && !ps->hairpin)
							continue;
						if (!l2fe_port_l2_active(pq))
							continue;
						if (pq->master != ps->master)
							continue;
						if (ps->isolated && pq->isolated)
							continue;

						lo |= BIT(q);
					}
				}
			}
			l2fe_mmshp_write_locked(ni, p, hi, lo);
		}

		l2fe_elpb_set_stp_locked(ni, p, !ps->bridged || active);

		if (ps->ilpb_vid_saved) {
			u16 want_vid = ps->orig_dft_vid;

			if (ps->bridged && ps->master) {
				struct rtl_l2fe_bridge_state *br =
					rtl_l2fe_get_bridge_locked(ps->master,
								   false);

				if (br && br->vlan_filtering && ps->br_pvid)
					want_vid = ps->br_pvid;
			}
			l2fe_ilpb_write_dft_vid_locked(ni, p, want_vid);
		}
	}

	l2fe_mymac_sync_locked();
}

/* L2FE VE HW VLAN management (slots fib_idx = 64..127) */
static struct rtl_l2fe_vlan_slot *l2fe_find_vlan_locked(u16 vid, bool alloc)
{
	int free_idx = -1;
	unsigned int i;

	for (i = 0; i < L2FE_VLAN_SLOTS; i++) {
		if (l2fe_st.vlans[i].used && l2fe_st.vlans[i].vid == vid)
			return &l2fe_st.vlans[i];
		if (!l2fe_st.vlans[i].used && free_idx < 0)
			free_idx = i;
	}

	if (!alloc || free_idx < 0)
		return NULL;

	memset(&l2fe_st.vlans[free_idx], 0, sizeof(l2fe_st.vlans[free_idx]));
	l2fe_st.vlans[free_idx].used = true;
	l2fe_st.vlans[free_idx].vid = vid;
	return &l2fe_st.vlans[free_idx];
}

static int l2fe_program_vlan_slot_locked(struct rtl_l2fe_vlan_slot *slot)
{
	struct rtl9617c_ni *ni = l2fe_st.ni;
	unsigned int idx = slot - l2fe_st.vlans;
	u32 fib_idx = L2FE_VLAN_FIB_BASE + idx;
	u16 member_bm, untag_bm;
	u32 data1, data0;
	int ret;

	if (!ni || slot->vid == 0 || slot->vid >= VLAN_N_VID)
		return -EINVAL;

	member_bm = (slot->br_member_bm | slot->ndo_member_bm) &
		    L2FE_LAN_BM_MASK;
	untag_bm = slot->br_untag_bm & L2FE_LAN_BM_MASK;

	if (!member_bm && !slot->br_cpu) {
		/* Tear down VID mapping cleanly */
		ret = l2fe_wait_busy(ni, L2FE_VE_VID_ACC);
		if (ret)
			return ret;
		rtl_ni_write(ni, L2FE_VE_VID_DATA, 0);
		rtl_ni_write(ni, L2FE_VE_VID_ACC,
			     (slot->vid & 0xfffU) | L2FE_ACC_RBW_WRITE |
			     L2FE_ACC_BUSY);
		ret = l2fe_wait_busy(ni, L2FE_VE_VID_ACC);
		memset(slot, 0, sizeof(*slot));
		return ret;
	}

	if (slot->br_cpu || slot->ndo_member_bm) {
		member_bm |= BIT(L2FE_CPU0_BM_BIT) | BIT(L2FE_L3FE_BM_BIT);
		if (slot->br_cpu_untag && !slot->ndo_member_bm)
			untag_bm |= BIT(L2FE_CPU0_BM_BIT);
	}

	ret = l2fe_wait_busy(ni, L2FE_VE_FIB_ACC);
	if (ret)
		return ret;

	rtl_ni_write(ni, L2FE_VE_FIB_ACC, fib_idx | L2FE_ACC_BUSY);
	ret = l2fe_wait_busy(ni, L2FE_VE_FIB_ACC);
	if (ret)
		return ret;

	data1 = rtl_ni_read(ni, L2FE_VE_FIB_DATA1) & ~GENMASK(9, 0);
	data0 = ((u32)member_bm << 16) | untag_bm;

	rtl_ni_write(ni, L2FE_VE_FIB_DATA1, data1);
	rtl_ni_write(ni, L2FE_VE_FIB_DATA0, data0);
	rtl_ni_write(ni, L2FE_VE_FIB_ACC,
		     fib_idx | L2FE_ACC_RBW_WRITE | L2FE_ACC_BUSY);
	ret = l2fe_wait_busy(ni, L2FE_VE_FIB_ACC);
	if (ret)
		return ret;

	ret = l2fe_wait_busy(ni, L2FE_VE_VID_ACC);
	if (ret)
		return ret;

	rtl_ni_write(ni, L2FE_VE_VID_DATA, BIT(0) | (fib_idx << 1));
	rtl_ni_write(ni, L2FE_VE_VID_ACC,
		     (slot->vid & 0xfffU) | L2FE_ACC_RBW_WRITE |
		     L2FE_ACC_BUSY);
	return l2fe_wait_busy(ni, L2FE_VE_VID_ACC);
}

/* L2FE L2E HW FDB indirect add/del (verified against ca-ne.ko __aal_mac_2_fdb_data) */
static int l2fe_hw_fdb_op_locked(const u8 *mac, u16 vid, u16 ldpid,
				 bool is_static, bool add)
{
	struct rtl9617c_ni *ni = l2fe_st.ni;
	u32 d3, d2, d1, d0;
	u16 kv = vid & 0xfffU;
	int ret;

	if (!ni)
		return -ENODEV;

	d3 = (u32)(mac[0] >> 5);
	d2 = ((u32)(mac[0] & 0x1fU) << 27) | ((u32)mac[1] << 19) |
	     ((u32)mac[2] << 11) | ((u32)mac[3] << 3) | ((u32)mac[4] >> 5);
	d1 = ((((u32)(mac[4] & 0x1fU) << 8) | mac[5]) << 19) |
	     ((u32)kv << 7) | ((u32)kv >> 9);
	d0 = ((u32)(kv & 0x1ffU) << 23) | BIT(21) | BIT(20) |
	     (is_static ? BIT(19) : 0U) | (0xffU << 11) | (ldpid & 0x3ffU);

	ret = l2fe_wait_busy(ni, L2FE_L2E_ACC);
	if (ret)
		return ret;

	rtl_ni_write(ni, L2FE_L2E_STS, 0);
	rtl_ni_write(ni, L2FE_L2E_DATA0, d0);
	rtl_ni_write(ni, L2FE_L2E_DATA1, d1);
	rtl_ni_write(ni, L2FE_L2E_DATA2, d2);
	rtl_ni_write(ni, L2FE_L2E_DATA3, d3);
	rtl_ni_write(ni, L2FE_L2E_ACC,
		     (add ? L2FE_L2E_CMD_ADD : L2FE_L2E_CMD_DEL) |
		     L2FE_ACC_BUSY);

	ret = l2fe_wait_busy(ni, L2FE_L2E_ACC);
	if (ret)
		return ret;

	if (add && (rtl_ni_read(ni, L2FE_L2E_STS) & 0xfU) != L2FE_L2E_STS_OK)
		return -ENOSPC;

	return 0;
}

static struct rtl_l2fe_fdb_entry *
l2fe_sw_fdb_find_locked(const u8 *mac, u16 orig_vid)
{
	struct rtl_l2fe_fdb_entry *ent;

	list_for_each_entry(ent, &l2fe_st.fdb_list, list) {
		if (ent->orig_vid == orig_vid &&
		    ether_addr_equal(ent->mac, mac))
			return ent;
	}

	return NULL;
}

static void l2fe_flush_fdb_by_ifindex_locked(int ifindex, bool include_static)
{
	struct rtl_l2fe_fdb_entry *ent, *tmp;

	list_for_each_entry_safe(ent, tmp, &l2fe_st.fdb_list, list) {
		if (ent->ifindex != ifindex)
			continue;
		if (!include_static && ent->is_static)
			continue;

		l2fe_hw_fdb_op_locked(ent->mac, ent->key_vid, ent->ldpid,
				      ent->is_static, false);
		list_del(&ent->list);
		l2fe_st.fdb_count--;
		kfree(ent);
	}
}

static u16 l2fe_effective_fdb_vid_locked(u16 req_vid, u16 ldpid)
{
	if (req_vid)
		return req_vid & 0xfffU;

	/* Under SVL, L2E masks VID; under IVL, match ingress ILPB default_vid */
	if (ldpid < L2FE_NUM_PORT_SLOTS && rtl_l2fe_is_lan_id(ldpid) &&
	    l2fe_st.ports[ldpid].ilpb_vid_saved)
		return l2fe_st.ports[ldpid].orig_dft_vid & 0xfffU;

	return 0;
}

static void rtl_l2fe_fdb_work(struct work_struct *work)
{
	struct rtl_l2fe_fdb_work *w =
		container_of(work, struct rtl_l2fe_fdb_work, work);
	struct net_device *ndev = w->dev;
	struct rtl9617c_ni_port *port;
	struct net_device *master;
	bool notify_offload = false;
	bool is_lan_bridge_port;
	bool is_br_master;
	u16 target_ldpid;
	unsigned long flags;
	bool is_static;
	int err = 0;

	rtnl_lock();

	port = rtl_l2fe_get_lan_port(ndev);
	is_br_master = netif_is_bridge_master(ndev);
	master = is_br_master ? ndev : netdev_master_upper_dev_get(ndev);

	spin_lock_irqsave(&l2fe_st.lock, flags);
	is_lan_bridge_port = port || (master && netif_is_bridge_master(master) &&
				      rtl_l2fe_has_bridge_locked(master));

	if (is_br_master) {
		if (!rtl_l2fe_has_bridge_locked(ndev)) {
			spin_unlock_irqrestore(&l2fe_st.lock, flags);
			goto out_unlock;
		}
		target_ldpid = L2FE_CPU0_LDPID;
	} else if (port) {
		if (!l2fe_st.ports[port->id].bridged) {
			spin_unlock_irqrestore(&l2fe_st.lock, flags);
			goto out_unlock;
		}
		target_ldpid = port->id;
	} else if (is_lan_bridge_port) {
		/* Foreign port (e.g. Wi-Fi) in br-lan: forward via CPU0 (16) */
		target_ldpid = L2FE_CPU0_LDPID;
	} else {
		spin_unlock_irqrestore(&l2fe_st.lock, flags);
		goto out_unlock;
	}

	/* Local/router MACs stay handled by L2FE PE MY_MAC -> L3FE (25) */
	if (w->is_local || is_br_master) {
		spin_unlock_irqrestore(&l2fe_st.lock, flags);
		goto out_unlock;
	}

	is_static = w->added_by_user;

	if (w->event == SWITCHDEV_FDB_ADD_TO_DEVICE) {
		struct rtl_l2fe_fdb_entry *ent;
		u16 key_vid;

		key_vid = l2fe_effective_fdb_vid_locked(w->vid, target_ldpid);
		ent = l2fe_sw_fdb_find_locked(w->mac, w->vid);

		if (ent && ent->ldpid == target_ldpid &&
		    ent->is_static == is_static &&
		    ent->key_vid == key_vid) {
			notify_offload = !is_br_master;
		} else {
			err = l2fe_hw_fdb_op_locked(w->mac, key_vid,
						    target_ldpid, is_static,
						    true);
			if (!err) {
				if (!ent && l2fe_st.fdb_count < L2FE_MAX_SW_FDB) {
					ent = kzalloc(sizeof(*ent), GFP_ATOMIC);
					if (ent) {
						ether_addr_copy(ent->mac, w->mac);
						ent->orig_vid = w->vid;
						list_add(&ent->list,
							 &l2fe_st.fdb_list);
						l2fe_st.fdb_count++;
					}
				}
				if (ent) {
					ent->key_vid = key_vid;
					ent->ldpid = target_ldpid;
					ent->ifindex = ndev->ifindex;
					ent->is_static = is_static;
					ent->is_local = false;
				}
				notify_offload = !is_br_master;
			}
		}
	} else if (w->event == SWITCHDEV_FDB_DEL_TO_DEVICE) {
		struct rtl_l2fe_fdb_entry *ent;
		u16 key_vid;

		ent = l2fe_sw_fdb_find_locked(w->mac, w->vid);
		key_vid = ent ? ent->key_vid :
			  l2fe_effective_fdb_vid_locked(w->vid, target_ldpid);

		l2fe_hw_fdb_op_locked(w->mac, key_vid, target_ldpid,
				      is_static, false);
		if (ent) {
			list_del(&ent->list);
			l2fe_st.fdb_count--;
			kfree(ent);
		}
	}

	spin_unlock_irqrestore(&l2fe_st.lock, flags);

	if (notify_offload) {
		struct switchdev_notifier_fdb_info info = {};

		info.addr = w->mac;
		info.vid = w->vid;
		info.offloaded = true;
		call_switchdev_notifiers(SWITCHDEV_FDB_OFFLOADED, ndev,
					 &info.info, NULL);
	}

out_unlock:
	rtnl_unlock();
	dev_put(ndev);
	kfree(w);
}

/* Switchdev attribute handler (safe under spinlock in both atomic & blocking) */
static int rtl_l2fe_port_attr_set(struct net_device *ndev, const void *ctx,
				  const struct switchdev_attr *attr,
				  struct netlink_ext_ack *extack)
{
	struct rtl9617c_ni_port *port = rtl_l2fe_get_lan_port(ndev);
	struct rtl_l2fe_port_state *ps;
	unsigned long flags;
	int ret = 0;

	if (!port)
		return -EOPNOTSUPP;

	spin_lock_irqsave(&l2fe_st.lock, flags);
	ps = &l2fe_st.ports[port->id];

	switch (attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_PRE_BRIDGE_FLAGS:
		if (attr->u.brport_flags.mask & ~L2FE_SUPPORTED_BR_FLAGS)
			ret = -EINVAL;
		break;

	case SWITCHDEV_ATTR_ID_PORT_BRIDGE_FLAGS:
		if (attr->u.brport_flags.mask & BR_ISOLATED)
			ps->isolated = !!(attr->u.brport_flags.val & BR_ISOLATED);
		if (attr->u.brport_flags.mask & BR_HAIRPIN_MODE)
			ps->hairpin = !!(attr->u.brport_flags.val &
					 BR_HAIRPIN_MODE);
		if (attr->u.brport_flags.mask & BR_LEARNING)
			ps->learning = !!(attr->u.brport_flags.val & BR_LEARNING);
		l2fe_sync_ports_hw_locked();
		break;

	case SWITCHDEV_ATTR_ID_PORT_STP_STATE:
		ps->stp_state = attr->u.stp_state;
		l2fe_sync_ports_hw_locked();
		break;

	case SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING:
		if (ps->bridged && ps->master) {
			struct rtl_l2fe_bridge_state *br =
				rtl_l2fe_get_bridge_locked(ps->master, true);

			if (br) {
				br->vlan_filtering = attr->u.vlan_filtering;
				l2fe_sync_ports_hw_locked();
			}
		}
		break;

	case SWITCHDEV_ATTR_ID_BRIDGE_AGEING_TIME:
		ret = 0;
		break;

	default:
		ret = -EOPNOTSUPP;
		break;
	}

	spin_unlock_irqrestore(&l2fe_st.lock, flags);
	return ret;
}

static int rtl_l2fe_port_obj_add(struct net_device *ndev, const void *ctx,
				 const struct switchdev_obj *obj,
				 struct netlink_ext_ack *extack)
{
	struct rtl9617c_ni_port *port = rtl_l2fe_get_lan_port(ndev);
	const struct switchdev_obj_port_vlan *vlan;
	struct rtl_l2fe_vlan_slot *slot;
	unsigned long flags;
	bool cpu_port;
	bool untag;
	int ret;

	if (!port || obj->id != SWITCHDEV_OBJ_ID_PORT_VLAN)
		return -EOPNOTSUPP;

	vlan = SWITCHDEV_OBJ_PORT_VLAN(obj);
	if (vlan->vid == 0 || vlan->vid >= VLAN_N_VID)
		return -EINVAL;

	cpu_port = netif_is_bridge_master(vlan->obj.orig_dev);
	untag = !!(vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED);

	spin_lock_irqsave(&l2fe_st.lock, flags);
	slot = l2fe_find_vlan_locked(vlan->vid, true);
	if (!slot) {
		spin_unlock_irqrestore(&l2fe_st.lock, flags);
		return -ENOSPC;
	}

	if (cpu_port) {
		slot->br_cpu = true;
		slot->br_cpu_untag = untag;
	} else {
		slot->br_member_bm |= BIT(port->id);
		if (untag)
			slot->br_untag_bm |= BIT(port->id);
		else
			slot->br_untag_bm &= ~BIT(port->id);

		if (vlan->flags & BRIDGE_VLAN_INFO_PVID) {
			l2fe_st.ports[port->id].br_pvid = vlan->vid;
			l2fe_sync_ports_hw_locked();
		} else if (l2fe_st.ports[port->id].br_pvid == vlan->vid) {
			l2fe_st.ports[port->id].br_pvid = 0;
			l2fe_sync_ports_hw_locked();
		}
	}

	ret = l2fe_program_vlan_slot_locked(slot);
	spin_unlock_irqrestore(&l2fe_st.lock, flags);
	return ret;
}

static int rtl_l2fe_port_obj_del(struct net_device *ndev, const void *ctx,
				 const struct switchdev_obj *obj)
{
	struct rtl9617c_ni_port *port = rtl_l2fe_get_lan_port(ndev);
	const struct switchdev_obj_port_vlan *vlan;
	struct rtl_l2fe_vlan_slot *slot;
	unsigned long flags;
	bool cpu_port;
	int ret;

	if (!port || obj->id != SWITCHDEV_OBJ_ID_PORT_VLAN)
		return -EOPNOTSUPP;

	vlan = SWITCHDEV_OBJ_PORT_VLAN(obj);
	if (vlan->vid == 0 || vlan->vid >= VLAN_N_VID)
		return 0;

	cpu_port = netif_is_bridge_master(vlan->obj.orig_dev);

	spin_lock_irqsave(&l2fe_st.lock, flags);
	slot = l2fe_find_vlan_locked(vlan->vid, false);
	if (!slot) {
		spin_unlock_irqrestore(&l2fe_st.lock, flags);
		return 0;
	}

	if (cpu_port) {
		slot->br_cpu = false;
		slot->br_cpu_untag = false;
	} else {
		slot->br_member_bm &= ~BIT(port->id);
		slot->br_untag_bm &= ~BIT(port->id);
		if (l2fe_st.ports[port->id].br_pvid == vlan->vid) {
			l2fe_st.ports[port->id].br_pvid = 0;
			l2fe_sync_ports_hw_locked();
		}
	}

	ret = l2fe_program_vlan_slot_locked(slot);
	spin_unlock_irqrestore(&l2fe_st.lock, flags);
	return ret;
}

static int rtl_l2fe_switchdev_event(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct net_device *ndev = switchdev_notifier_info_to_dev(ptr);
	const struct switchdev_notifier_fdb_info *fdb;
	struct rtl_l2fe_fdb_work *w;
	struct net_device *master;
	unsigned long flags;
	bool relevant;
	int err;

	if (event == SWITCHDEV_PORT_ATTR_SET) {
		err = switchdev_handle_port_attr_set(ndev, ptr,
						     rtl_l2fe_port_dev_check,
						     rtl_l2fe_port_attr_set);
		return notifier_from_errno(err);
	}

	if (event != SWITCHDEV_FDB_ADD_TO_DEVICE &&
	    event != SWITCHDEV_FDB_DEL_TO_DEVICE)
		return NOTIFY_DONE;

	if (!l2fe_st.wq)
		return NOTIFY_DONE;

	fdb = ptr;
	if (!fdb->addr || is_multicast_ether_addr(fdb->addr))
		return NOTIFY_DONE;

	master = netif_is_bridge_master(ndev) ?
		 ndev : netdev_master_upper_dev_get_rcu(ndev);

	spin_lock_irqsave(&l2fe_st.lock, flags);
	if (rtl_l2fe_get_lan_port(ndev)) {
		relevant = true;
	} else if (master && netif_is_bridge_master(master) &&
		   rtl_l2fe_has_bridge_locked(master)) {
		relevant = true;
	} else {
		relevant = false;
	}
	spin_unlock_irqrestore(&l2fe_st.lock, flags);

	if (!relevant)
		return NOTIFY_DONE;

	w = kzalloc(sizeof(*w), GFP_ATOMIC);
	if (!w)
		return NOTIFY_BAD;

	INIT_WORK(&w->work, rtl_l2fe_fdb_work);
	w->dev = ndev;
	w->event = event;
	ether_addr_copy(w->mac, fdb->addr);
	w->vid = fdb->vid;
	w->added_by_user = fdb->added_by_user;
	w->is_local = fdb->is_local;
	dev_hold(ndev);

	if (!queue_work(l2fe_st.wq, &w->work)) {
		dev_put(ndev);
		kfree(w);
	}

	return NOTIFY_DONE;
}

static int rtl_l2fe_switchdev_blocking_event(struct notifier_block *nb,
					     unsigned long event, void *ptr)
{
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);
	int err;

	switch (event) {
	case SWITCHDEV_PORT_OBJ_ADD:
		err = switchdev_handle_port_obj_add(dev, ptr,
						    rtl_l2fe_port_dev_check,
						    rtl_l2fe_port_obj_add);
		return notifier_from_errno(err);

	case SWITCHDEV_PORT_OBJ_DEL:
		err = switchdev_handle_port_obj_del(dev, ptr,
						    rtl_l2fe_port_dev_check,
						    rtl_l2fe_port_obj_del);
		return notifier_from_errno(err);

	case SWITCHDEV_PORT_ATTR_SET:
		err = switchdev_handle_port_attr_set(dev, ptr,
						     rtl_l2fe_port_dev_check,
						     rtl_l2fe_port_attr_set);
		return notifier_from_errno(err);

	default:
		return NOTIFY_DONE;
	}
}

static void rtl_l2fe_update_port_from_dev_locked(struct net_device *dev,
						 struct rtl9617c_ni_port *port,
						 struct net_device *master,
						 bool registered)
{
	struct rtl_l2fe_port_state *ps = &l2fe_st.ports[port->id];

	ps->dev = registered ? dev : NULL;
	ps->registered = registered;

	if (registered && master && netif_is_bridge_master(master)) {
		struct rtl_l2fe_bridge_state *br =
			rtl_l2fe_get_bridge_locked(master, true);

		ps->bridged = true;
		ps->master = master;
		ps->stp_state = br_port_get_stp_state(dev);
		ps->isolated = br_port_flag_is_set(dev, BR_ISOLATED);
		ps->hairpin = br_port_flag_is_set(dev, BR_HAIRPIN_MODE);
		ps->learning = br_port_flag_is_set(dev, BR_LEARNING);
		if (br)
			ether_addr_copy(br->mac, master->dev_addr);
	} else {
		ps->bridged = false;
		ps->master = NULL;
		ps->stp_state = BR_STATE_DISABLED;
		ps->isolated = false;
		ps->hairpin = false;
		ps->br_pvid = 0;
		rtl_l2fe_prune_bridges_locked();
	}

	l2fe_sync_ports_hw_locked();
}

static int rtl_l2fe_netdev_event(struct notifier_block *nb,
				 unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct rtl9617c_ni_port *port = rtl_l2fe_get_lan_port(dev);
	struct netdev_notifier_changeupper_info *cu;
	struct rtl_l2fe_bridge_state *br;
	unsigned long flags;

	switch (event) {
	case NETDEV_REGISTER:
	case NETDEV_UP:
		if (!port)
			return NOTIFY_DONE;
		spin_lock_irqsave(&l2fe_st.lock, flags);
		rtl_l2fe_update_port_from_dev_locked(dev, port,
						     netdev_master_upper_dev_get(dev),
						     true);
		spin_unlock_irqrestore(&l2fe_st.lock, flags);
		break;

	case NETDEV_DOWN:
		spin_lock_irqsave(&l2fe_st.lock, flags);
		if (port) {
			l2fe_flush_fdb_by_ifindex_locked(dev->ifindex, false);
			l2fe_sync_ports_hw_locked();
		} else {
			struct net_device *master =
				netdev_master_upper_dev_get(dev);

			if (master && rtl_l2fe_has_bridge_locked(master))
				l2fe_flush_fdb_by_ifindex_locked(dev->ifindex,
								 false);
		}
		spin_unlock_irqrestore(&l2fe_st.lock, flags);
		break;

	case NETDEV_UNREGISTER:
		spin_lock_irqsave(&l2fe_st.lock, flags);
		if (port) {
			l2fe_flush_fdb_by_ifindex_locked(dev->ifindex, true);
			rtl_l2fe_update_port_from_dev_locked(dev, port, NULL,
							     false);
		} else {
			l2fe_flush_fdb_by_ifindex_locked(dev->ifindex, true);
		}
		spin_unlock_irqrestore(&l2fe_st.lock, flags);
		break;

	case NETDEV_CHANGEADDR:
		spin_lock_irqsave(&l2fe_st.lock, flags);
		if (port && l2fe_st.ports[port->id].bridged) {
			l2fe_mymac_sync_locked();
		} else if (netif_is_bridge_master(dev)) {
			br = rtl_l2fe_get_bridge_locked(dev, false);
			if (br) {
				ether_addr_copy(br->mac, dev->dev_addr);
				l2fe_mymac_sync_locked();
			}
		}
		spin_unlock_irqrestore(&l2fe_st.lock, flags);
		break;

	case NETDEV_CHANGEUPPER:
		cu = ptr;
		if (!cu->master || !cu->upper_dev ||
		    !netif_is_bridge_master(cu->upper_dev))
			return NOTIFY_DONE;

		spin_lock_irqsave(&l2fe_st.lock, flags);
		if (port) {
			if (!cu->linking)
				l2fe_flush_fdb_by_ifindex_locked(dev->ifindex,
								 true);
			rtl_l2fe_update_port_from_dev_locked(dev, port,
							     cu->linking ?
							     cu->upper_dev :
							     NULL,
							     true);
		} else if (!cu->linking &&
			   rtl_l2fe_has_bridge_locked(cu->upper_dev)) {
			/* Foreign bridge port (e.g. Wi-Fi) left br-lan */
			l2fe_flush_fdb_by_ifindex_locked(dev->ifindex, true);
		}
		spin_unlock_irqrestore(&l2fe_st.lock, flags);
		break;

	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block rtl_l2fe_switchdev_nb = {
	.notifier_call = rtl_l2fe_switchdev_event,
};

static struct notifier_block rtl_l2fe_switchdev_bl_nb = {
	.notifier_call = rtl_l2fe_switchdev_blocking_event,
};

static struct notifier_block rtl_l2fe_netdev_nb = {
	.notifier_call = rtl_l2fe_netdev_event,
};

int rtl_ni_l2fe_vlan_rx_add(struct net_device *dev, __be16 proto, u16 vid)
{
	struct rtl9617c_ni_port *port = rtl_l2fe_get_lan_port(dev);
	struct rtl_l2fe_vlan_slot *slot;
	unsigned long flags;
	int ret;

	if (!rtl_ni_l2fe_bridge_offload)
		return 0;

	/* Preserve pon0 EVTO and stacked SVLAN handling untouched */
	if (!port || proto != htons(ETH_P_8021Q))
		return 0;

	if (vid == 0 || vid >= VLAN_N_VID)
		return 0;

	rtl_ni_l2fe_register_ni(port->ni);

	spin_lock_irqsave(&l2fe_st.lock, flags);
	slot = l2fe_find_vlan_locked(vid, true);
	if (!slot) {
		spin_unlock_irqrestore(&l2fe_st.lock, flags);
		return -ENOSPC;
	}

	slot->ndo_member_bm |= BIT(port->id);
	ret = l2fe_program_vlan_slot_locked(slot);
	spin_unlock_irqrestore(&l2fe_st.lock, flags);

	return ret;
}

int rtl_ni_l2fe_vlan_rx_kill(struct net_device *dev, __be16 proto, u16 vid)
{
	struct rtl9617c_ni_port *port = rtl_l2fe_get_lan_port(dev);
	struct rtl_l2fe_vlan_slot *slot;
	unsigned long flags;
	int ret;

	if (!rtl_ni_l2fe_bridge_offload)
		return 0;

	if (!port || proto != htons(ETH_P_8021Q) ||
	    vid == 0 || vid >= VLAN_N_VID)
		return 0;

	spin_lock_irqsave(&l2fe_st.lock, flags);
	slot = l2fe_find_vlan_locked(vid, false);
	if (!slot) {
		spin_unlock_irqrestore(&l2fe_st.lock, flags);
		return 0;
	}

	slot->ndo_member_bm &= ~BIT(port->id);
	ret = l2fe_program_vlan_slot_locked(slot);
	spin_unlock_irqrestore(&l2fe_st.lock, flags);

	return ret;
}

static int l2fe_register_notifiers(void)
{
	int err;

	err = register_switchdev_notifier(&rtl_l2fe_switchdev_nb);
	if (err) {
		pr_warn("rtl9617c-ni: register_switchdev_notifier failed: %d\n",
			err);
		return err;
	}

	err = register_switchdev_blocking_notifier(&rtl_l2fe_switchdev_bl_nb);
	if (err) {
		pr_warn("rtl9617c-ni: register_switchdev_blocking_notifier failed: %d\n",
			err);
		goto err_sw_nb;
	}

	err = register_netdevice_notifier(&rtl_l2fe_netdev_nb);
	if (err) {
		pr_warn("rtl9617c-ni: register_netdevice_notifier failed: %d\n",
			err);
		goto err_sw_bl_nb;
	}

	l2fe_st.notifiers_registered = true;
	return 0;

err_sw_bl_nb:
	unregister_switchdev_blocking_notifier(&rtl_l2fe_switchdev_bl_nb);
err_sw_nb:
	unregister_switchdev_notifier(&rtl_l2fe_switchdev_nb);
	return err;
}

static void l2fe_register_notifiers_work(struct work_struct *work)
{
	if (!l2fe_st.initialized || l2fe_st.notifiers_registered)
		return;

	l2fe_register_notifiers();
}

void rtl_ni_l2fe_register_ni(struct rtl9617c_ni *ni)
{
	unsigned long flags;
	bool init_needed = false;

	if (!ni || !rtl_ni_l2fe_bridge_offload)
		return;

	spin_lock_irqsave(&l2fe_st.lock, flags);
	if (!l2fe_st.initialized) {
		INIT_LIST_HEAD(&l2fe_st.fdb_list);
		l2fe_st.ni = ni;
		l2fe_mymac_snapshot_hw_locked(ni);
		l2fe_st.initialized = true;
		init_needed = true;
	}
	spin_unlock_irqrestore(&l2fe_st.lock, flags);

	if (!init_needed)
		return;

	INIT_WORK(&l2fe_st.reg_work, l2fe_register_notifiers_work);
	l2fe_st.wq = alloc_ordered_workqueue("rtl9617c_l2fe", 0);
	if (!l2fe_st.wq) {
		pr_warn("rtl9617c-ni: L2FE workqueue allocation failed\n");
		return;
	}

	/*
	 * register_switchdev_blocking_notifier() and
	 * register_netdevice_notifier() acquire rtnl_lock(). Defer if the
	 * first caller is .ndo_open or .ndo_vlan_rx_add_vid under rtnl_lock().
	 */
	if (rtnl_is_locked()) {
		queue_work(l2fe_st.wq, &l2fe_st.reg_work);
		return;
	}

	if (l2fe_register_notifiers()) {
		destroy_workqueue(l2fe_st.wq);
		l2fe_st.wq = NULL;
	}
}

void rtl_ni_l2fe_unregister_ni(void)
{
	struct rtl_l2fe_fdb_entry *ent, *tmp;
	unsigned long flags;
	unsigned int i;

	if (!l2fe_st.initialized)
		return;

	if (l2fe_st.wq)
		cancel_work_sync(&l2fe_st.reg_work);

	if (l2fe_st.notifiers_registered) {
		unregister_netdevice_notifier(&rtl_l2fe_netdev_nb);
		unregister_switchdev_blocking_notifier(&rtl_l2fe_switchdev_bl_nb);
		unregister_switchdev_notifier(&rtl_l2fe_switchdev_nb);
		l2fe_st.notifiers_registered = false;
	}

	if (l2fe_st.wq) {
		destroy_workqueue(l2fe_st.wq);
		l2fe_st.wq = NULL;
	}

	spin_lock_irqsave(&l2fe_st.lock, flags);
	list_for_each_entry_safe(ent, tmp, &l2fe_st.fdb_list, list) {
		l2fe_hw_fdb_op_locked(ent->mac, ent->key_vid, ent->ldpid,
				      ent->is_static, false);
		list_del(&ent->list);
		kfree(ent);
	}
	l2fe_st.fdb_count = 0;

	for (i = 0; i < L2FE_NUM_PORT_SLOTS; i++) {
		l2fe_st.ports[i].bridged = false;
		l2fe_st.ports[i].master = NULL;
		l2fe_st.ports[i].br_pvid = 0;
	}
	memset(l2fe_st.bridges, 0, sizeof(l2fe_st.bridges));
	l2fe_sync_ports_hw_locked();
	l2fe_st.initialized = false;
	l2fe_st.ni = NULL;
	spin_unlock_irqrestore(&l2fe_st.lock, flags);
}

#ifndef MODULE
static int __init rtl_ni_l2fe_late_init(void)
{
	struct rtl9617c_ni *ni = NULL;
	struct net_device *dev;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		struct rtl9617c_ni_port *port = rtl_l2fe_get_lan_port(dev);

		if (port && port->ni) {
			dev->hw_features |= NETIF_F_HW_TC |
					    NETIF_F_HW_VLAN_CTAG_FILTER;
			dev->features |= NETIF_F_HW_TC |
					 NETIF_F_HW_VLAN_CTAG_FILTER;
			if (!ni)
				ni = port->ni;
		}
	}
	rtnl_unlock();

	if (ni) {
		rtl_ni_fc_ext_init(ni);
		rtl_ni_l2fe_register_ni(ni);
	}

	return 0;
}
late_initcall(rtl_ni_l2fe_late_init);
#endif
