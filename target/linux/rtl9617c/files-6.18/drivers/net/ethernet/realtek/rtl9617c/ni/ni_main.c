// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitfield.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <linux/smp.h>
#include <linux/unaligned.h>

#include "ni.h"
#include "ni_ext.h"
#include "fe/fe.h"

#define L2TM_QM_EQ_INIT			(RTL_L2TM_QM_EQ1_EN | \
					 FIELD_PREP(RTL_L2TM_QM_EQ1_PRVT, 29) | \
					 FIELD_PREP(RTL_L2TM_QM_EQ1_BUFNUM, 4))
#define L2TM_QM_PRVT_DEFAULT		29
#define L2TM_QM_GLOB_BUF_INIT		(FIELD_PREP(RTL_L2TM_QM_GLOB_NONCONG, 2048) | \
					 RTL_L2TM_QM_GLOB_DROP_EN | \
					 FIELD_PREP(RTL_L2TM_QM_GLOB_NODROP, 33))
#define L2TM_ES_CPU_PORT		14
#define PORT_RX_CTRL_MASK		(RTL_NI_PORT_RX_CTRL_BYP_NDRP | \
					 RTL_NI_PORT_RX_CTRL_BYP_EN | \
					 RTL_NI_PORT_RX_CTRL_BYP_DPID)
#define PORT_RX_CTRL_CPU		(RTL_NI_PORT_RX_CTRL_BYP_EN | \
					 FIELD_PREP(RTL_NI_PORT_RX_CTRL_BYP_DPID, 16))
#define GLB_PORT_TO_CPU_DMA		0xf
/*
 * Assert global L2TE XOFF well above L2TM_QM_GLOB_BUF_INIT's 33-buffer
 * no-drop floor (0x21) so ingress PAUSE throttles bursts before L2TM drops
 * frames.
 */
#define L2TE_CB_GLB_LTH_BUFS		1024
#define L2TE_CB_GLB_HTH_BUFS		2048
#define RTL_NI_GLB_PKT_LEN_CFG		0xa034
#define RTL_NI_GLB_MIN_PKT_LEN		GENMASK(25, 16)
#define RTL_NI_GLB_MAX_PKT_LEN		GENMASK(13, 0)
#define RTL_NI_GLB_SF_PKT_LEN_CFG	0xa038
#define RTL_NI_XGE_MAC_FRAME_LEN_MAX	0xb230
#define RTL_NI_XGE_MAX_FRAME_LEN_MASK	GENMASK(15, 0)
#define RTL_NI_PKT_MIN_BYTES		64U

static const struct regmap_config rtl_ni_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = RTL_NI_MAX_REGISTER,
	.fast_io = true,
};

/* Caller holds ni->rx_lock with IRQs saved/disabled. */
static void rtl_ni_irq_enable(struct rtl9617c_ni *ni, unsigned int cpu,
			      bool enable)
{
	u32 mask = RTL_QM_EPP64_P_INT_EN(cpu);

	rtl_ni_rmw(ni, RTL_QM_EPP64_P3_0_INT_EN, mask, enable ? mask : 0);
}

static bool rtl_ni_is_port_dev(const struct rtl9617c_ni *ni,
			       const struct net_device *dev)
{
	unsigned int id;

	for (id = 0; id < RTL_NI_PORTS; id++) {
		const struct rtl9617c_ni_port *port = READ_ONCE(ni->ports[id]);

		if (port && port->dev == dev)
			return true;
	}
	return false;
}

/* Caller holds ring->lock. */
static void rtl_ni_wake_ring(struct rtl9617c_ni *ni, struct rtl_ni_dma_ring *ring)
{
	struct rtl9617c_ni_endpoint *endpoint;
	unsigned int id, q = ring->id;
	int old;

	ring->stopped = false;
	for (id = 0; id < RTL_NI_PORTS; id++) {
		struct rtl9617c_ni_port *port = READ_ONCE(ni->ports[id]);

		if (port && netif_running(port->dev) &&
		    q < port->dev->num_tx_queues)
			netif_tx_wake_queue(netdev_get_tx_queue(port->dev, q));
	}

	old = atomic_fetch_andnot(BIT(q), &ni->pon_stopped_mask);
	if ((old & BIT(q)) && !(old & ~BIT(q))) {
		rcu_read_lock();
		endpoint = rcu_dereference(ni->pon);
		if (endpoint)
			endpoint->wake(endpoint);
		rcu_read_unlock();
	}
}

/* Caller holds ring->lock. */
static void rtl_ni_stop_ring(struct rtl9617c_ni *ni, struct rtl_ni_dma_ring *ring,
			     struct net_device *dev)
{
	unsigned int id, q = ring->id;

	ring->stopped = true;
	for (id = 0; id < RTL_NI_PORTS; id++) {
		struct rtl9617c_ni_port *port = READ_ONCE(ni->ports[id]);

		if (port && netif_running(port->dev) &&
		    q < port->dev->num_tx_queues)
			netif_tx_stop_queue(netdev_get_tx_queue(port->dev, q));
	}
	if (dev && !rtl_ni_is_port_dev(ni, dev)) {
		atomic_or(BIT(q), &ni->pon_stopped_mask);
		netif_stop_queue(dev);
	}
}

static void rtl_ni_kick_timer(struct rtl9617c_ni *ni)
{
	unsigned long flags;

	/* Pairs with rtl_ni_timer clearing timer_armed before ring scan. */
	smp_mb();
	if (READ_ONCE(ni->timer_armed) || READ_ONCE(ni->tx_closing))
		return;

	spin_lock_irqsave(&ni->timer_lock, flags);
	if (!ni->tx_closing && !hrtimer_active(&ni->timer)) {
		WRITE_ONCE(ni->timer_armed, true);
		hrtimer_start(&ni->timer, ns_to_ktime(RTL_NI_TX_TIMER_NS),
			      HRTIMER_MODE_REL_SOFT);
	}
	spin_unlock_irqrestore(&ni->timer_lock, flags);
}

static void rtl_ni_tx_rings_lock(struct rtl9617c_ni *ni, unsigned long *flags)
{
	unsigned int q;

	local_irq_save(*flags);
	for (q = 0; q < ni->dma.num_rings; q++)
		spin_lock_nested(&ni->dma.rings[q].lock, q);
}

static void rtl_ni_tx_rings_unlock(struct rtl9617c_ni *ni, unsigned long flags)
{
	int q;

	for (q = ni->dma.num_rings - 1; q >= 0; q--)
		spin_unlock(&ni->dma.rings[q].lock);
	local_irq_restore(flags);
}

static enum hrtimer_restart rtl_ni_timer(struct hrtimer *timer)
{
	struct rtl9617c_ni *ni = container_of(timer, struct rtl9617c_ni, timer);
	enum hrtimer_restart restart = HRTIMER_NORESTART;
	bool need_restart = false;
	unsigned int c, q;
	unsigned long flags;

	if (READ_ONCE(ni->tx_closing)) {
		WRITE_ONCE(ni->timer_armed, false);
		return HRTIMER_NORESTART;
	}

	WRITE_ONCE(ni->timer_armed, false);
	/* Ensure producers observe timer_armed == false before we scan state. */
	smp_mb();

	for (c = 0; c < ni->qm.num_rxq; c++) {
		struct rtl_ni_rxq *rxq = &ni->qm.rxq[c];

		if (READ_ONCE(rxq->refill.pending))
			napi_schedule(&rxq->napi);
	}

	for (q = 0; q < ni->dma.num_rings; q++) {
		struct rtl_ni_dma_ring *ring = &ni->dma.rings[q];

		spin_lock_irqsave(&ring->lock, flags);
		if (ring->stopped || ring->zc_skbs) {
			bool zc_pending = rtl_ni_dma_ring_reclaim_timer(&ni->dma,
									ring);

			if (ring->stopped) {
				if (rtl_ni_dma_ring_avail(ring, 1))
					rtl_ni_wake_ring(ni, ring);
				else
					need_restart = true;
			}
			if (zc_pending)
				need_restart = true;
		}
		spin_unlock_irqrestore(&ring->lock, flags);
	}

	if (need_restart) {
		spin_lock_irqsave(&ni->timer_lock, flags);
		if (!ni->tx_closing) {
			WRITE_ONCE(ni->timer_armed, true);
			hrtimer_forward_now(timer,
					    ns_to_ktime(RTL_NI_TX_TIMER_NS));
			restart = HRTIMER_RESTART;
		}
		spin_unlock_irqrestore(&ni->timer_lock, flags);
	}
	return restart;
}

netdev_tx_t rtl_ni_xmit_frame(struct rtl9617c_ni *ni, struct net_device *dev,
			      struct sk_buff *skb,
			      const struct rtl9617c_ni_frame *frame)
{
	bool multiq = dev && dev->num_tx_queues > 1;
	unsigned int num_rings = ni->dma.num_rings;
	unsigned int len = skb->len;
	bool is_gso = skb_is_gso(skb);
	bool consumed = false, used_sg = false, need_timer = false;
	struct rtl_ni_dma_ring *ring;
	unsigned long flags;
	unsigned int q;
	int ret;

	if (rtl_ni_try_tx_hook(dev, skb, frame) == 0) {
		if (dev) {
			DEV_STATS_INC(dev, tx_packets);
			DEV_STATS_ADD(dev, tx_bytes, len);
		}
		return NETDEV_TX_OK;
	}

	if (frame->oam || frame->raw_payload || frame->prefix_length > 0 ||
	    num_rings <= 1)
		q = 0;
	else if (multiq)
		q = skb_get_queue_mapping(skb) % num_rings;
	else
		q = raw_smp_processor_id() % num_rings;

	ret = rtl_ni_dma_prepare_skb(skb, frame, dev ? dev->mtu : ETH_DATA_LEN);
	if (unlikely(ret)) {
		if (dev)
			DEV_STATS_INC(dev, tx_dropped);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	ring = &ni->dma.rings[q];
	spin_lock_irqsave(&ring->lock, flags);
	ret = rtl_ni_dma_ring_xmit(&ni->dma, ring, skb, frame,
				   dev ? dev->mtu : ETH_DATA_LEN,
				   &consumed, &used_sg);
	if (ret == -ENOSPC) {
		rtl_ni_stop_ring(ni, ring, dev);
		spin_unlock_irqrestore(&ring->lock, flags);
		rtl_ni_kick_timer(ni);
		return NETDEV_TX_BUSY;
	}
	if (!ret && !rtl_ni_dma_ring_avail(ring, 1)) {
		rtl_ni_stop_ring(ni, ring, dev);
		need_timer = true;
	} else if (!ret && ring->zc_skbs) {
		need_timer = true;
	}
	spin_unlock_irqrestore(&ring->lock, flags);

	if (need_timer)
		rtl_ni_kick_timer(ni);

	if (ret) {
		if (dev)
			DEV_STATS_INC(dev, tx_dropped);
		dev_kfree_skb_any(skb);
	} else {
		if (dev) {
			DEV_STATS_INC(dev, tx_packets);
			DEV_STATS_ADD(dev, tx_bytes, len);
		}
		if (used_sg)
			atomic_long_inc(&ni->tx_sg_pkts);
		else
			atomic_long_inc(&ni->tx_bounce_pkts);
		if (is_gso)
			atomic_long_inc(&ni->tx_tso_pkts);
		if (!consumed)
			dev_consume_skb_any(skb);
	}
	return NETDEV_TX_OK;
}

void rtl_ni_tx_restart(struct rtl9617c_ni *ni)
{
	unsigned long flags;
	unsigned int q;
	bool restarted = false;
	int ret = 0;

	mutex_lock(&ni->lock);
	if (!ni->users || READ_ONCE(ni->tx_closing)) {
		mutex_unlock(&ni->lock);
		return;
	}
	rtl_ni_tx_rings_lock(ni, &flags);
	rtl_ni_dma_stop(&ni->dma);
	ret = rtl_ni_dma_start(&ni->dma);
	if (!ret) {
		for (q = 0; q < ni->dma.num_rings; q++)
			rtl_ni_wake_ring(ni, &ni->dma.rings[q]);
		restarted = true;
	}
	rtl_ni_tx_rings_unlock(ni, flags);
	mutex_unlock(&ni->lock);

	if (ret)
		dev_err(ni->dev, "TX DMA restart failed: %d\n", ret);
	else if (restarted)
		rtl_ni_notify_dma_reset();
}

static void rtl_ni_tx_timeout_work_fn(struct work_struct *work)
{
	struct rtl9617c_ni *ni = container_of(work, struct rtl9617c_ni,
					      tx_timeout_work);

	rtl_ni_tx_restart(ni);
}

static int rtl_ni_poll(struct napi_struct *napi, int budget)
{
	struct rtl_ni_rxq *rxq = container_of(napi, struct rtl_ni_rxq, napi);
	struct rtl9617c_ni *ni = rxq->ni;
	unsigned long flags;
	int done;

	if (rxq->id < ni->dma.num_rings) {
		struct rtl_ni_dma_ring *ring = &ni->dma.rings[rxq->id];

		if (READ_ONCE(ring->stopped) || READ_ONCE(ring->zc_skbs)) {
			spin_lock_irqsave(&ring->lock, flags);
			rtl_ni_dma_ring_reclaim(&ni->dma, ring);
			if (ring->stopped && rtl_ni_dma_ring_avail(ring, 1))
				rtl_ni_wake_ring(ni, ring);
			spin_unlock_irqrestore(&ring->lock, flags);
		}
	}

	done = rtl_ni_qm_poll(rxq, budget);
	if (done < budget && napi_complete_done(napi, done)) {
		spin_lock_irqsave(&ni->rx_lock, flags);
		if (ni->rx_active)
			rtl_ni_irq_enable(ni, rxq->id, true);
		spin_unlock_irqrestore(&ni->rx_lock, flags);
	}
	if (READ_ONCE(rxq->refill.pending))
		rtl_ni_kick_timer(ni);
	return done;
}

static irqreturn_t rtl_ni_irq(int irq, void *data)
{
	struct rtl_ni_rxq *rxq = data;
	struct rtl9617c_ni *ni = rxq->ni;

	spin_lock(&ni->rx_lock);
	rtl_ni_irq_enable(ni, rxq->id, false);
	spin_unlock(&ni->rx_lock);
	napi_schedule_irqoff(&rxq->napi);
	return IRQ_HANDLED;
}

void rtl_ni_set_mac(struct rtl9617c_ni *ni)
{
	rtl_ni_write(ni, RTL_NI_GLB_MAC_ADDR_CFG0, get_unaligned_be32(ni->mac));
	rtl_ni_rmw(ni, RTL_NI_GLB_MAC_ADDR_CFG1, RTL_NI_GLB_MAC_ADDR1_MASK,
		   ni->mac[4]);
}

static void rtl_ni_l2te_fc_enable(struct rtl9617c_ni *ni, bool enable)
{
	if (enable) {
		rtl_ni_write(ni, RTL_L2TE_CB_GLB_THRSH,
			     FIELD_PREP(RTL_L2TE_CB_GLB_THRSH_HTH,
					L2TE_CB_GLB_HTH_BUFS) |
			     FIELD_PREP(RTL_L2TE_CB_GLB_THRSH_LTH,
					L2TE_CB_GLB_LTH_BUFS));
		rtl_ni_rmw(ni, RTL_L2TE_FC_GLB_TIMER, 0,
			   RTL_L2TE_FC_GLB_TIMER_EN);
		return;
	}

	rtl_ni_rmw(ni, RTL_L2TE_FC_GLB_TIMER, RTL_L2TE_FC_GLB_TIMER_EN, 0);
}

static int rtl_ni_engine_start(struct rtl9617c_ni *ni)
{
	unsigned long flags;
	unsigned int id;
	u32 cfg;
	int ret;

	rtl_ni_serdes_bringup(ni);

	rtl_ni_write(ni, RTL_L2TM_QM_EQ_CFG, L2TM_QM_EQ_INIT);
	rtl_ni_rmw(ni, RTL_L2TM_QM_PORT_PRVT_PROFILE(0),
		   RTL_L2TM_QM_PRVT_CNT_EVEN,
		   FIELD_PREP(RTL_L2TM_QM_PRVT_CNT_EVEN, L2TM_QM_PRVT_DEFAULT));
	rtl_ni_write(ni, RTL_L2TM_QM_GLOB_BUF_CFG, L2TM_QM_GLOB_BUF_INIT);
	rtl_ni_rmw(ni, RTL_L2TM_ES_SCH_CFG0(L2TM_ES_CPU_PORT),
		   RTL_L2TM_ES_SCH_VOQ_EN, RTL_L2TM_ES_SCH_VOQ_EN);
	rtl_ni_rmw(ni, RTL_L2TM_ES_CTRL,
		   RTL_L2TM_ES_CTRL_TX_EN | RTL_L2TM_ES_CTRL_CPU,
		   RTL_L2TM_ES_CTRL_TX_EN | RTL_L2TM_ES_CTRL_CPU);
	for (id = 0; id < RTL_NI_PORTS; id++) {
		if (!ni->ports[id])
			continue;
		if (rtl_ni_is_ge(id))
			rtl_ni_rmw(ni, RTL_L2TM_ES_CTRL,
				   RTL_L2TM_ES_CTRL_TX_EN | BIT(id),
				   RTL_L2TM_ES_CTRL_TX_EN | BIT(id));
		rtl_ni_rmw(ni, RTL_L2TM_ES_SCH_CFG0(id),
			   RTL_L2TM_ES_SCH_VOQ_EN, RTL_L2TM_ES_SCH_VOQ_EN);
	}

	ret = rtl_ni_dma_start(&ni->dma);
	if (ret)
		return ret;
	ret = rtl_ni_qm_start(ni);
	if (ret) {
		rtl_ni_dma_stop(&ni->dma);
		return ret;
	}

	for (id = 0; id < RTL_NI_GE_PORTS; id++)
		if (ni->ports[id])
			rtl_ni_rmw(ni, RTL_NI_PORT_RX_CTRL(id),
				   PORT_RX_CTRL_MASK, PORT_RX_CTRL_CPU);
	/*
	 * Program NI global/XGE6 packet length limits above 1518/1522 so
	 * bootloader defaults cannot truncate Baby Jumbo (1536+ MTU) frames.
	 */
	if (ni->ports[RTL_NI_PORT_P6]) {
		ni->xge_bypass = rtl_ni_read(ni, RTL_NI_XGE_BYPASS_CFG);
		ni->xge_bypass_saved = true;
		rtl_ni_rmw(ni, RTL_NI_XGE_BYPASS_CFG, RTL_NI_XGE_BYPASS_FIELDS,
			   RTL_NI_XGE_BYPASS_CPU0);
		rtl_ni_rmw(ni, RTL_NI_XGE_MAC_FRAME_LEN_MAX,
			   RTL_NI_XGE_MAX_FRAME_LEN_MASK, RTL_NI_QM_BUF_SIZE);
	}

	rtl_ni_rmw(ni, RTL_NI_GLB_STATIC, RTL_NI_GLB_PORT_TO_CPU, GLB_PORT_TO_CPU_DMA);
	rtl_ni_rmw(ni, RTL_L2TM_ES_CTRL,
		   RTL_L2TM_ES_CTRL_PORTS | RTL_L2TM_ES_CTRL_TX_EN,
		   RTL_L2TM_ES_CTRL_PORTS | RTL_L2TM_ES_CTRL_TX_EN);
	rtl_ni_l2te_fc_enable(ni, true);

	rtl_ni_set_mac(ni);
	rtl_ni_rmw(ni, RTL_NI_GLB_PKT_LEN_CFG,
		   RTL_NI_GLB_MIN_PKT_LEN | RTL_NI_GLB_MAX_PKT_LEN,
		   FIELD_PREP(RTL_NI_GLB_MIN_PKT_LEN, RTL_NI_PKT_MIN_BYTES) |
		   FIELD_PREP(RTL_NI_GLB_MAX_PKT_LEN, RTL_NI_QM_BUF_SIZE));
	mdelay(1);
	rtl_ni_rmw(ni, RTL_NI_GLB_SF_PKT_LEN_CFG, RTL_NI_GLB_MAX_PKT_LEN,
		   FIELD_PREP(RTL_NI_GLB_MAX_PKT_LEN, RTL_NI_QM_BUF_SIZE));
	mdelay(1);
	rtl_ni_write(ni, RTL_NI_RX_ADDR, (RTL_NI_XRAM_RX_END - 1) << 16);
	rtl_ni_write(ni, RTL_NI_TX_ADDR, (RTL_NI_XRAM_TX_END - 1) << 16 |
		     RTL_NI_XRAM_TX_START);
	cfg = rtl_ni_read(ni, RTL_NI_XRAM_CFG) & ~RTL_NI_XRAM_RX_PROMISC;
	rtl_ni_write(ni, RTL_NI_XRAM_CFG, cfg | RTL_NI_XRAM_CPU_DISABLE);
	usleep_range(100, 200);
	rtl_ni_write(ni, RTL_NI_XRAM_CFG, cfg | RTL_NI_XRAM_CPU_DISABLE);

	spin_lock_irqsave(&ni->timer_lock, flags);
	WRITE_ONCE(ni->tx_closing, false);
	WRITE_ONCE(ni->timer_armed, false);
	spin_unlock_irqrestore(&ni->timer_lock, flags);

	atomic_set(&ni->pon_stopped_mask, 0);
	for (id = 0; id < ni->dma.num_rings; id++)
		ni->dma.rings[id].stopped = false;
	for (id = 0; id < ni->qm.num_rxq; id++)
		napi_enable(&ni->qm.rxq[id].napi);

	spin_lock_irqsave(&ni->rx_lock, flags);
	ni->rx_active = true;
	for (id = 0; id < ni->qm.num_rxq; id++)
		rtl_ni_irq_enable(ni, id, true);
	spin_unlock_irqrestore(&ni->rx_lock, flags);
	rtl_fe_start(ni->fe);
	return 0;
}

static void rtl_ni_engine_stop(struct rtl9617c_ni *ni)
{
	unsigned long flags;
	unsigned int c;

	rtl_fe_stop(ni->fe);
	rtl_ni_l2te_fc_enable(ni, false);

	spin_lock_irqsave(&ni->timer_lock, flags);
	WRITE_ONCE(ni->tx_closing, true);
	WRITE_ONCE(ni->timer_armed, false);
	spin_unlock_irqrestore(&ni->timer_lock, flags);
	hrtimer_cancel(&ni->timer);

	if (ni->xge_bypass_saved) {
		rtl_ni_rmw(ni, RTL_NI_XGE_BYPASS_CFG, RTL_NI_XGE_BYPASS_FIELDS,
			   ni->xge_bypass & RTL_NI_XGE_BYPASS_FIELDS);
		ni->xge_bypass_saved = false;
	}

	spin_lock_irqsave(&ni->rx_lock, flags);
	ni->rx_active = false;
	for (c = 0; c < ni->qm.num_rxq; c++)
		rtl_ni_irq_enable(ni, c, false);
	spin_unlock_irqrestore(&ni->rx_lock, flags);

	for (c = 0; c < ni->qm.num_rxq; c++)
		synchronize_irq(ni->qm.rxq[c].irq);
	for (c = 0; c < ni->qm.num_rxq; c++)
		napi_disable(&ni->qm.rxq[c].napi);
	hrtimer_cancel(&ni->timer);

	spin_lock_irqsave(&ni->rx_lock, flags);
	rtl_ni_qm_stop(ni);
	spin_unlock_irqrestore(&ni->rx_lock, flags);
	usleep_range(100, 200);
	rtl_ni_qm_drain_slots(ni);

	rtl_ni_tx_rings_lock(ni, &flags);
	rtl_ni_dma_stop(&ni->dma);
	rtl_ni_tx_rings_unlock(ni, flags);
	rtl_ni_rmw(ni, RTL_NI_XRAM_CFG, RTL_NI_XRAM_CPU_DISABLE,
		   RTL_NI_XRAM_CPU_DISABLE);
}

int rtl_ni_engine_get(struct rtl9617c_ni *ni)
{
	bool started = false;
	int ret = 0;

	mutex_lock(&ni->lock);
	if (!ni->users) {
		ret = rtl_ni_engine_start(ni);
		started = !ret;
	}
	if (!ret)
		ni->users++;
	mutex_unlock(&ni->lock);
	if (started)
		rtl_ni_notify_dma_reset();
	return ret;
}

void rtl_ni_engine_put(struct rtl9617c_ni *ni)
{
	mutex_lock(&ni->lock);
	if (!WARN_ON_ONCE(!ni->users) && !--ni->users)
		rtl_ni_engine_stop(ni);
	mutex_unlock(&ni->lock);
}

static int rtl_ni_base_mac(struct rtl9617c_ni *ni)
{
	int ret;

	ret = of_get_mac_address(ni->dev->of_node, ni->mac);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret || !is_valid_ether_addr(ni->mac))
		eth_random_addr(ni->mac);
	return 0;
}

static void rtl_ni_napi_del(void *data)
{
	struct rtl9617c_ni *ni = data;
	unsigned int c;

	for (c = 0; c < ni->qm.num_rxq; c++) {
		if (!ni->qm.rxq[c].napi_added)
			continue;
		netif_napi_del(&ni->qm.rxq[c].napi);
		ni->qm.rxq[c].napi_added = false;
	}
	free_netdev(ni->napi_dev);
}

static void rtl_ni_qm_release(void *data)
{
	rtl_ni_qm_free(data);
}

static void rtl_ni_irq_affinity_clear(void *data)
{
	struct rtl_ni_rxq *rxq = data;

	irq_update_affinity_hint(rxq->irq, NULL);
}

static void rtl_ni_hostpol_sync(void *data)
{
	rtl_fe_hostpol_sync(data);
}

static int rtl_ni_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int want_rxq = rtl_ni_rx_queues_get();
	unsigned int c, irq_cnt;
	struct rtl9617c_ni *ni;
	void __iomem *base;
	int ret;

	dev_info(dev, "enter rtl_ni_probe\n");

	ni = devm_kzalloc(dev, sizeof(*ni), GFP_KERNEL);
	if (!ni) {
		dev_err(dev, "devm_kzalloc failed\n");
		return -ENOMEM;
	}
	ni->dev = dev;
	spin_lock_init(&ni->timer_lock);
	spin_lock_init(&ni->tx_lock);
	spin_lock_init(&ni->rx_lock);
	ret = devm_mutex_init(dev, &ni->lock);
	if (ret) {
		dev_err(dev, "devm_mutex_init(lock) failed: %d\n", ret);
		return ret;
	}
	ret = devm_mutex_init(dev, &ni->mib_lock);
	if (ret) {
		dev_err(dev, "devm_mutex_init(mib_lock) failed: %d\n", ret);
		return ret;
	}
	atomic_set(&ni->pon_stopped_mask, 0);
	atomic_long_set(&ni->rx_errors, 0);
	atomic_long_set(&ni->rx_dropped, 0);
	atomic_long_set(&ni->rx_zerocopy_pkts, 0);
	atomic_long_set(&ni->rx_copybreak_pkts, 0);
	atomic_long_set(&ni->rx_alloc_fallback, 0);
	atomic_long_set(&ni->tx_sg_pkts, 0);
	atomic_long_set(&ni->tx_tso_pkts, 0);
	atomic_long_set(&ni->tx_bounce_pkts, 0);

	base = devm_platform_ioremap_resource_byname(pdev, "ni");
	if (IS_ERR(base)) {
		dev_err(dev, "ioremap ni failed: %ld\n", PTR_ERR(base));
		return PTR_ERR(base);
	}
	ni->regs = devm_regmap_init_mmio(dev, base, &rtl_ni_regmap_config);
	if (IS_ERR(ni->regs)) {
		dev_err(dev, "regmap_init_mmio failed: %ld\n",
			PTR_ERR(ni->regs));
		return PTR_ERR(ni->regs);
	}
	ni->dma.base = devm_platform_ioremap_resource_byname(pdev, "dma");
	if (IS_ERR(ni->dma.base)) {
		dev_err(dev, "ioremap dma failed: %ld\n",
			PTR_ERR(ni->dma.base));
		return PTR_ERR(ni->dma.base);
	}
	ni->qm.axi = devm_platform_ioremap_resource_byname(pdev, "qm-axi");
	if (IS_ERR(ni->qm.axi)) {
		dev_err(dev, "ioremap qm-axi failed: %ld\n",
			PTR_ERR(ni->qm.axi));
		return PTR_ERR(ni->qm.axi);
	}
	ni->reset = devm_platform_ioremap_resource_byname(pdev, "reset");
	if (IS_ERR(ni->reset)) {
		dev_err(dev, "ioremap reset failed: %ld\n",
			PTR_ERR(ni->reset));
		return PTR_ERR(ni->reset);
	}
	if (IS_ENABLED(CONFIG_RTL9617C_FE)) {
		ni->fe_axi = devm_platform_ioremap_resource_byname(pdev, "fe-axi");
		if (IS_ERR(ni->fe_axi)) {
			dev_err(dev, "ioremap fe-axi failed: %ld\n",
				PTR_ERR(ni->fe_axi));
			return PTR_ERR(ni->fe_axi);
		}
	}

	ni->qm.rxq[0].irq = platform_get_irq(pdev, 0);
	if (ni->qm.rxq[0].irq < 0) {
		dev_err(dev, "platform_get_irq failed: %d\n", ni->qm.rxq[0].irq);
		return ni->qm.rxq[0].irq;
	}
	irq_cnt = 1;
	for (c = 1; c < want_rxq; c++) {
		int irq = platform_get_irq_optional(pdev, c);

		if (irq <= 0)
			break;
		ni->qm.rxq[c].irq = irq;
		irq_cnt++;
	}
	ni->qm.num_rxq = irq_cnt;

	ret = rtl_ni_base_mac(ni);
	if (ret) {
		dev_err(dev, "rtl_ni_base_mac failed: %d\n", ret);
		return ret;
	}

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "dma_set_mask_and_coherent failed: %d\n", ret);
		return ret;
	}
	ret = rtl_ni_dma_alloc(ni);
	if (ret) {
		dev_err(dev, "rtl_ni_dma_alloc failed: %d\n", ret);
		return ret;
	}

	ni->napi_dev = alloc_netdev_dummy(0);
	if (!ni->napi_dev) {
		dev_err(dev, "alloc_netdev_dummy failed\n");
		return -ENOMEM;
	}
	for (c = 0; c < ni->qm.num_rxq; c++) {
		struct rtl_ni_rxq *rxq = &ni->qm.rxq[c];

		rxq->ni = ni;
		rxq->id = c;
		netif_napi_add(ni->napi_dev, &rxq->napi, rtl_ni_poll);
		rxq->napi_added = true;
	}
	ret = devm_add_action_or_reset(dev, rtl_ni_napi_del, ni);
	if (ret) {
		dev_err(dev, "devm_add_action(napi_del) failed: %d\n", ret);
		return ret;
	}

	ret = rtl_ni_qm_alloc(ni);
	if (ret) {
		dev_err(dev, "rtl_ni_qm_alloc failed: %d\n", ret);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, rtl_ni_qm_release, ni);
	if (ret) {
		dev_err(dev, "devm_add_action(qm_release) failed: %d\n", ret);
		return ret;
	}
	hrtimer_setup(&ni->timer, rtl_ni_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);
	INIT_WORK(&ni->tx_timeout_work, rtl_ni_tx_timeout_work_fn);

	rtl_ni_write(ni, RTL_QM_EPP64_P3_0_INT_EN, 0);
	rtl_ni_write(ni, RTL_QM_EPP64_P7_4_INT_EN, 0);
	for (c = 0; c < ni->qm.num_rxq; c++) {
		struct rtl_ni_rxq *rxq = &ni->qm.rxq[c];

		snprintf(rxq->irq_name, sizeof(rxq->irq_name), "%s-rx%u",
			 KBUILD_MODNAME, c);
		ret = devm_request_irq(dev, rxq->irq, rtl_ni_irq, 0,
				       rxq->irq_name, rxq);
		if (ret) {
			dev_err(dev, "devm_request_irq(%d) failed: %d\n",
				rxq->irq, ret);
			return ret;
		}
		if (ni->qm.num_rxq > 1 && c < nr_cpu_ids && cpu_possible(c)) {
			irq_set_affinity_and_hint(rxq->irq, cpumask_of(c));
			ret = devm_add_action_or_reset(dev,
						       rtl_ni_irq_affinity_clear,
						       rxq);
			if (ret)
				return ret;
		}
	}

	ret = rtl_te_flow_init(ni);
	if (ret)
		dev_warn(dev, "rtl_te_flow_init failed: %d\n", ret);

	ni->fe = rtl_fe_create(dev, ni->regs, ni->fe_axi,
			       ni->qm.lan_dq_bufs > 0, &ni->te);
	if (IS_ERR(ni->fe)) {
		dev_err(dev, "rtl_fe_create failed: %ld\n", PTR_ERR(ni->fe));
		rtl_te_flow_exit(ni);
		return PTR_ERR(ni->fe);
	}
	if (ni->fe)
		rtl_te_flow_set_sync(&ni->te, rtl_ni_hostpol_sync, ni->fe);

	rtl_ni_ptp_init_dev(ni, dev);

	platform_set_drvdata(pdev, ni);
	ret = rtl_ni_ports_register(ni);
	if (ret) {
		dev_err(dev, "rtl_ni_ports_register failed: %d\n", ret);
		rtl_ni_ptp_cleanup();
		rtl_te_flow_debugfs_exit(&ni->te);
		rtl_te_flow_set_sync(&ni->te, NULL, NULL);
		rtl_fe_destroy(ni->fe);
		rtl_te_flow_exit(ni);
		return ret;
	}

	rtl_ni_mib_init(ni);

	dev_info(dev, "rtl_ni_probe succeeded (rxq=%u, txq=%u)\n",
		 ni->qm.num_rxq, ni->dma.num_rings);
	return 0;
}

static void rtl_ni_shutdown(struct platform_device *pdev)
{
	struct rtl9617c_ni *ni = platform_get_drvdata(pdev);
	unsigned int id;

	if (!ni)
		return;

	rtl_ni_mib_exit(ni);
	cancel_work_sync(&ni->tx_timeout_work);

	rtnl_lock();
	for (id = 0; id < RTL_NI_PORTS; id++) {
		struct rtl9617c_ni_port *port = ni->ports[id];

		if (port && port->registered && netif_running(port->dev))
			dev_close(port->dev);
	}
	rtnl_unlock();

	mutex_lock(&ni->lock);
	if (ni->users) {
		rtl_ni_engine_stop(ni);
		ni->users = 0;
	}
	mutex_unlock(&ni->lock);
}

static void rtl_ni_remove(struct platform_device *pdev)
{
	struct rtl9617c_ni *ni = platform_get_drvdata(pdev);

	rtl_ni_mib_exit(ni);
	rtl_ni_ports_unregister(ni);
	rtl_ni_ptp_cleanup();
	rtl_ni_l2fe_unregister_ni();
	cancel_work_sync(&ni->tx_timeout_work);
	rtl_te_flow_debugfs_exit(&ni->te);
	rtl_te_flow_set_sync(&ni->te, NULL, NULL);
	rtl_fe_destroy(ni->fe);
	WRITE_ONCE(ni->tx_closing, true);
	hrtimer_cancel(&ni->timer);
	rtl_te_flow_exit(ni);
}

static const struct of_device_id rtl_ni_of_match[] = {
	{ .compatible = "realtek,rtl9617c-ni" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl_ni_of_match);

static struct platform_driver rtl_ni_driver = {
	.probe = rtl_ni_probe,
	.remove = rtl_ni_remove,
	.shutdown = rtl_ni_shutdown,
	.driver = {
		.name = "rtl9617c-ni",
		.of_match_table = rtl_ni_of_match,
	},
};
module_platform_driver(rtl_ni_driver);

MODULE_DESCRIPTION("Realtek RTL9617C network interface driver");
MODULE_LICENSE("GPL");
