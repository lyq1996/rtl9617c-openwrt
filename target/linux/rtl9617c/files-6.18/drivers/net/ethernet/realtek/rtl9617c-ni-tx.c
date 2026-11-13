/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/crc32.h>
#include <linux/etherdevice.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/unaligned.h>

#include "rtl9617c-dma.h"
#include "rtl9617c-ni-priv.h"
#include "rtl9617c-ni.h"
#include "rtl9617c-xram-format.h"

void rtl9617c_ni_wake_ports(struct rtl9617c_ni *priv)
{
	unsigned int port;

	for (port = 0; port < ARRAY_SIZE(priv->ports); port++)
		if (priv->ports[port] && netif_running(priv->ports[port]) &&
		    netif_queue_stopped(priv->ports[port]))
			netif_wake_queue(priv->ports[port]);
}

enum hrtimer_restart rtl9617c_ni_timer(struct hrtimer *timer)
{
	struct rtl9617c_ni *priv = container_of(timer, struct rtl9617c_ni, timer);

	if (!priv->qm_rx || READ_ONCE(priv->qm.refill.pending))
		napi_schedule(&priv->napi);
	spin_lock(&priv->tx_lock);
	if (priv->dma_tx) {
		struct rtl9617c_ni_endpoint *endpoint;

		rtl_dma_tx_complete(&priv->dma, priv->ports[priv->port] ? : priv->dummy);
		rcu_read_lock();
		endpoint = rcu_dereference(priv->qm.pon);
		if (endpoint && rtl_dma_tx_available(&priv->dma))
			endpoint->wake(endpoint);
		rcu_read_unlock();
		if (rtl_dma_tx_available(&priv->dma))
			rtl9617c_ni_wake_ports(priv);
	} else if (priv->tx_pending && rtl9617c_ni_pointer(priv, RTL_NI_TX_READ) == rtl9617c_ni_pointer(priv, RTL_NI_TX_WRITE)) {
		priv->tx_pending = false;
		if (priv->tx_dev) {
			priv->tx_dev->stats.tx_packets++;
			priv->tx_dev->stats.tx_bytes += priv->tx_length;
			priv->tx_dev = NULL;
		}
		rtl9617c_ni_wake_ports(priv);
	}
	spin_unlock(&priv->tx_lock);
	hrtimer_forward_now(timer, ms_to_ktime(1));
	return HRTIMER_RESTART;
}

netdev_tx_t rtl9617c_ni_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rtl9617c_port *lan = netdev_priv(dev);
	struct rtl9617c_ni *priv = lan->shared;
	unsigned int pointer, offset, wire, i;
	unsigned long flags;
	u32 crc;

	if (skb->len < ETH_HLEN || skb->len > 1522) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&priv->tx_lock, flags);
	if (priv->dma_tx) {

		unsigned int egress = lan->port >= 5 ? 6 : lan->port;
		const struct rtl9617c_ni_frame frame = {
			.header_high = BIT(13),
			.header_low = (16 << 9) | (egress << 3) | BIT(29),
		};
		netdev_tx_t result = rtl_dma_tx_frame(&priv->dma, skb, dev, &frame);

		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return result;
	}
	pointer = rtl9617c_ni_pointer(priv, RTL_NI_TX_WRITE);
	if (priv->tx_pending || rtl9617c_ni_pointer(priv, RTL_NI_TX_READ) != pointer) {
		netif_stop_queue(dev);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}
	wire = rtl_xram_wire_length(skb->len);
	memset(priv->tx_buffer, 0, ALIGN(8 + wire, 4));
	skb_copy_bits(skb, 0, priv->tx_buffer + 8, skb->len);
	crc = crc32_le(~0, priv->tx_buffer + 8, wire - 4) ^ ~0;
	put_unaligned_le32(crc, priv->tx_buffer + 8 + wire - 4);
	offset = pointer * 8 + 4;
	writel(rtl_xram_tx_header(rtl_xram_tx_next(pointer, wire), wire),
	       priv->xram + offset);
	offset = rtl_xram_word_next(offset, RTL_XRAM_TX_START * 8, RTL_XRAM_TX_END * 8);
	for (i = 0; i < wire + 8; i += 4) {
		writel(get_unaligned_le32(priv->tx_buffer + i), priv->xram + offset);
		offset = rtl_xram_word_next(offset, RTL_XRAM_TX_START * 8, RTL_XRAM_TX_END * 8);
	}
	priv->tx_pending = true;
	priv->tx_length = skb->len;
	priv->tx_dev = dev;
	netif_stop_queue(dev);
	wmb();
	rtl_ni_write(priv->regs, RTL_NI_TX_WRITE, rtl_xram_tx_next(pointer, wire));
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
}
