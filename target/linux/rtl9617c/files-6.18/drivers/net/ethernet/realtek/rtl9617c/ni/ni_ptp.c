// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IEEE 1588 PTP Hardware Clock (PHC) and TX/RX timestamp helper for RTL9617C.
 */

#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/net_tstamp.h>
#include <linux/netdevice.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/time64.h>
#include <linux/workqueue.h>

#include "../rtl9617c_regs.h"
#include "ni.h"
#include "ni_ext.h"

#define NI_PTP_PORTS			7
#define NI_PTP_DEFAULT_PERIOD		0x80000000U /* 8.0 ns @ 125 MHz */
#define NI_PTP_NS_MASK			0x3fffffffU
#define NI_PTP_TX_TIMEOUT		msecs_to_jiffies(50)

struct rtl_ni_ptp_state {
	struct rtl9617c_ni *ni;
	void __iomem *ptp_base;
	struct ptp_clock *ptp_clock;
	struct ptp_clock_info caps;
	struct mutex lock;
	spinlock_t tx_lock;
	struct delayed_work tx_work;
	struct sk_buff *tx_skb[NI_PTP_PORTS];
	unsigned long tx_stamp_jiffies[NI_PTP_PORTS];
	bool tx_enable[NI_PTP_PORTS];
	struct kernel_hwtstamp_config cfg[NI_PTP_PORTS];
	bool initialized;
};

static struct rtl_ni_ptp_state ptp_st;

static void rtl_ptp_read_raw(void __iomem *base, struct timespec64 *ts)
{
	u32 ns, secl, sech;

	ns = readl(base + RTL_PTP_TIMER) & NI_PTP_NS_MASK;
	secl = readl(base + RTL_PTP_TIMER_SECL);
	sech = readl(base + RTL_PTP_TIMER_SECH) & 0xffffU;

	ts->tv_sec = (time64_t)(((u64)sech << 32) | secl);
	ts->tv_nsec = ns;
}

static int rtl_ptp_gettime64(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	mutex_lock(&ptp_st.lock);
	if (ptp_st.ptp_base)
		rtl_ptp_read_raw(ptp_st.ptp_base, ts);
	else
		memset(ts, 0, sizeof(*ts));
	mutex_unlock(&ptp_st.lock);

	return 0;
}

static int rtl_ptp_settime64(struct ptp_clock_info *ptp,
			     const struct timespec64 *ts)
{
	void __iomem *base = ptp_st.ptp_base;
	struct timespec64 target = *ts;
	u32 cur_ns, match_ns, cfg3;
	int retries = 1000;
	u64 sec;

	if (!base)
		return -ENODEV;

	mutex_lock(&ptp_st.lock);

	/* Ensure PTP timer is enabled so the match trigger fires cleanly */
	writel(readl(base + RTL_PTP_PPS_CFG_0) | RTL_PTP_PPS_CFG_0_PTP_EN,
	       base + RTL_PTP_PPS_CFG_0);

	cur_ns = readl(base + RTL_PTP_TIMER) & NI_PTP_NS_MASK;
	match_ns = (cur_ns + 25000U) % NSEC_PER_SEC;
	timespec64_add_ns(&target, 25000ULL);
	sec = (u64)target.tv_sec;

	writel(lower_32_bits(sec), base + RTL_PTP_TIMER_SEC_LOADL);
	writel((u32)(sec >> 32) & 0xffffU, base + RTL_PTP_TIMER_SEC_LOADH);
	writel((u32)target.tv_nsec & NI_PTP_NS_MASK, base + RTL_PTP_TIMER_CFG_1);

	writel(match_ns & NI_PTP_NS_MASK, base + RTL_PTP_TIMER_CFG_2);
	cfg3 = readl(base + RTL_PTP_TIMER_CFG_3) | RTL_PTP_TIMER_MATCH_LOAD;
	writel(cfg3, base + RTL_PTP_TIMER_CFG_3);

	while ((readl(base + RTL_PTP_TIMER_CFG_3) & RTL_PTP_TIMER_MATCH_LOAD) &&
	       --retries)
		udelay(2);

	mutex_unlock(&ptp_st.lock);
	return 0;
}

static int rtl_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	void __iomem *base = ptp_st.ptp_base;
	s64 delta, period;

	if (!base)
		return -ENODEV;

	delta = div_s64((s64)scaled_ppm * 32768LL, 1000000LL);
	period = clamp_t(s64, (s64)NI_PTP_DEFAULT_PERIOD + delta,
			 0x40000000LL, 0xc0000000LL);

	mutex_lock(&ptp_st.lock);
	writel((u32)period, base + RTL_PTP_TIMER_CFG_0);
	mutex_unlock(&ptp_st.lock);

	return 0;
}

static int rtl_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct timespec64 ts;

	if (!ptp_st.ptp_base)
		return -ENODEV;

	rtl_ptp_gettime64(ptp, &ts);
	ts = timespec64_add(ts, ns_to_timespec64(delta));
	return rtl_ptp_settime64(ptp, &ts);
}

static void rtl_ptp_tx_work(struct work_struct *work)
{
	struct rtl9617c_ni *ni = ptp_st.ni;
	unsigned long flags;
	bool pending = false;
	u32 intr;
	int p;

	if (!ni)
		return;

	intr = rtl_ni_read(ni, RTL_NI_PTP_INTR);

	for (p = 0; p < NI_PTP_PORTS; p++) {
		struct sk_buff *skb = NULL;
		struct skb_shared_hwtstamps ts;
		bool ready = false, expired = false;

		spin_lock_irqsave(&ptp_st.tx_lock, flags);
		if (ptp_st.tx_skb[p]) {
			if (intr & BIT(p)) {
				skb = ptp_st.tx_skb[p];
				ptp_st.tx_skb[p] = NULL;
				ready = true;
			} else if (time_after(jiffies,
					      ptp_st.tx_stamp_jiffies[p] +
					      NI_PTP_TX_TIMEOUT)) {
				skb = ptp_st.tx_skb[p];
				ptp_st.tx_skb[p] = NULL;
				expired = true;
			} else {
				pending = true;
			}
		}
		spin_unlock_irqrestore(&ptp_st.tx_lock, flags);

		if (ready && skb) {
			u32 ns = rtl_ni_read(ni, RTL_NI_PTP_TX_TS_NS(p));
			u32 sec0 = rtl_ni_read(ni, RTL_NI_PTP_TX_TS_SEC0(p));
			u32 sec1 = rtl_ni_read(ni, RTL_NI_PTP_TX_TS_SEC1(p)) &
				   0xffffU;
			u64 sec = ((u64)sec1 << 32) | sec0;

			rtl_ni_write(ni, RTL_NI_PTP_INTR, BIT(p));
			memset(&ts, 0, sizeof(ts));
			ts.hwtstamp = ns_to_ktime(sec * NSEC_PER_SEC + ns);
			skb_tstamp_tx(skb, &ts);
			consume_skb(skb);
		} else if (expired && skb) {
			consume_skb(skb);
		}
	}

	if (pending)
		schedule_delayed_work(&ptp_st.tx_work, 1);
}

static DEFINE_MUTEX(ptp_init_lock);

void rtl_ni_ptp_init_dev(struct rtl9617c_ni *ni, struct device *dev)
{
	void __iomem *glb;

	if (!ni || !dev || smp_load_acquire(&ptp_st.initialized))
		return;

	mutex_lock(&ptp_init_lock);
	if (ptp_st.initialized)
		goto out;

	ptp_st.ni = ni;
	mutex_init(&ptp_st.lock);
	spin_lock_init(&ptp_st.tx_lock);
	INIT_DELAYED_WORK(&ptp_st.tx_work, rtl_ptp_tx_work);

	glb = devm_ioremap(dev, RTL_GLOBAL_BASE_PHYS, 0x100);
	if (glb) {
		writel(readl(glb + RTL_GLOBAL_PON_CNTL_OFF) |
		       RTL_GLOBAL_PON_CNTL_PTP_RST_N,
		       glb + RTL_GLOBAL_PON_CNTL_OFF);
	}

	ptp_st.ptp_base = devm_ioremap(dev, RTL_PTP_BASE_PHYS, 0x100);
	if (!ptp_st.ptp_base)
		goto out;

	writel(NI_PTP_DEFAULT_PERIOD, ptp_st.ptp_base + RTL_PTP_TIMER_CFG_0);
	writel(readl(ptp_st.ptp_base + RTL_PTP_PPS_CFG_0) |
	       RTL_PTP_PPS_CFG_0_PTP_EN,
	       ptp_st.ptp_base + RTL_PTP_PPS_CFG_0);

	/* Enable NI TX PTP timestamp capture per port; poll INTR status safely */
	rtl_ni_rmw(ni, RTL_NI_PTP_TX_CFG, GENMASK(14, 0), 0x7fU);
	rtl_ni_write(ni, RTL_NI_PTP_INTRE, 0);
	rtl_ni_write(ni, RTL_NI_PTP_INTR, 0x7fU);

	ptp_st.caps = (struct ptp_clock_info) {
		.owner		= THIS_MODULE,
		.name		= "rtl9617c-ptp",
		.max_adj	= 62500000,
		.adjfine	= rtl_ptp_adjfine,
		.adjtime	= rtl_ptp_adjtime,
		.gettime64	= rtl_ptp_gettime64,
		.settime64	= rtl_ptp_settime64,
	};

	ptp_st.ptp_clock = ptp_clock_register(&ptp_st.caps, dev);
	if (IS_ERR(ptp_st.ptp_clock)) {
		dev_warn(dev, "PTP clock registration failed: %ld\n",
			 PTR_ERR(ptp_st.ptp_clock));
		ptp_st.ptp_clock = NULL;
	}
	smp_store_release(&ptp_st.initialized, true);
out:
	mutex_unlock(&ptp_init_lock);
}

void rtl_ni_ptp_cleanup(void)
{
	int p;

	mutex_lock(&ptp_init_lock);
	if (!ptp_st.initialized) {
		mutex_unlock(&ptp_init_lock);
		return;
	}

	cancel_delayed_work_sync(&ptp_st.tx_work);
	for (p = 0; p < NI_PTP_PORTS; p++) {
		if (ptp_st.tx_skb[p]) {
			consume_skb(ptp_st.tx_skb[p]);
			ptp_st.tx_skb[p] = NULL;
		}
	}

	if (ptp_st.ptp_clock) {
		ptp_clock_unregister(ptp_st.ptp_clock);
		ptp_st.ptp_clock = NULL;
	}
	ptp_st.ptp_base = NULL;
	ptp_st.ni = NULL;
	smp_store_release(&ptp_st.initialized, false);
	mutex_unlock(&ptp_init_lock);
}

int rtl_ni_ptp_get_ts_info(struct net_device *dev,
			   struct kernel_ethtool_ts_info *info)
{
	info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
				SOF_TIMESTAMPING_RX_HARDWARE |
				SOF_TIMESTAMPING_TX_HARDWARE |
				SOF_TIMESTAMPING_RAW_HARDWARE;
	info->phc_index = ptp_st.ptp_clock ?
			  ptp_clock_index(ptp_st.ptp_clock) : -1;
	info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);
	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
			   BIT(HWTSTAMP_FILTER_PTP_V1_L4_EVENT) |
			   BIT(HWTSTAMP_FILTER_PTP_V2_L4_EVENT) |
			   BIT(HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
			   BIT(HWTSTAMP_FILTER_PTP_V2_EVENT) |
			   BIT(HWTSTAMP_FILTER_ALL);

	return 0;
}

int rtl_ni_ptp_hwtstamp_get(struct net_device *dev,
			    struct kernel_hwtstamp_config *cfg)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	if (port->id >= NI_PTP_PORTS)
		return -EINVAL;

	*cfg = ptp_st.cfg[port->id];
	return 0;
}

int rtl_ni_ptp_hwtstamp_set(struct net_device *dev,
			    struct kernel_hwtstamp_config *cfg,
			    struct netlink_ext_ack *extack)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	if (port->id >= NI_PTP_PORTS)
		return -EINVAL;

	switch (cfg->tx_type) {
	case HWTSTAMP_TX_OFF:
		ptp_st.tx_enable[port->id] = false;
		break;
	case HWTSTAMP_TX_ON:
		ptp_st.tx_enable[port->id] = true;
		break;
	default:
		return -ERANGE;
	}

	switch (cfg->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		break;
	default:
		return -ERANGE;
	}

	ptp_st.cfg[port->id] = *cfg;
	return 0;
}

struct sk_buff *rtl_ni_ptp_tx_prepare(struct rtl9617c_ni_port *port,
				      struct sk_buff *skb)
{
	struct sk_buff *clone;

	if (!port || !skb || port->id >= NI_PTP_PORTS)
		return NULL;
	if (!smp_load_acquire(&ptp_st.initialized) ||
	    !ptp_st.ptp_clock || !ptp_st.tx_enable[port->id])
		return NULL;
	if (!(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP))
		return NULL;

	clone = skb_clone_sk(skb);
	if (!clone)
		return NULL;

	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
	return clone;
}

void rtl_ni_ptp_tx_commit(struct rtl9617c_ni_port *port,
			  struct sk_buff *ts_skb)
{
	struct sk_buff *old;
	unsigned long flags;

	if (!port || !ts_skb || port->id >= NI_PTP_PORTS) {
		dev_kfree_skb_any(ts_skb);
		return;
	}

	spin_lock_irqsave(&ptp_st.tx_lock, flags);
	old = ptp_st.tx_skb[port->id];
	ptp_st.tx_skb[port->id] = ts_skb;
	ptp_st.tx_stamp_jiffies[port->id] = jiffies;
	spin_unlock_irqrestore(&ptp_st.tx_lock, flags);

	if (old)
		dev_consume_skb_any(old);

	schedule_delayed_work(&ptp_st.tx_work, 1);
}

int rtl_ni_ptp_read_rx_latch(struct rtl9617c_ni_port *port, u64 *ns_out)
{
	u32 ns, secl, sech;
	u64 sec;

	if (!port || !ns_out || !ptp_st.ptp_base || port->id >= NI_PTP_PORTS)
		return -ENODEV;

	ns = readl(ptp_st.ptp_base + RTL_PTP_LATCH_NS(port->id)) &
	     NI_PTP_NS_MASK;
	secl = readl(ptp_st.ptp_base + RTL_PTP_LATCH_SECL(port->id));
	sech = readl(ptp_st.ptp_base + RTL_PTP_LATCH_SECH(port->id)) &
	       0xffffU;
	sec = ((u64)sech << 32) | secl;
	*ns_out = sec * NSEC_PER_SEC + ns;

	return 0;
}
