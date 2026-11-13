/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/netdevice.h>

#include "rtl9617c-ni-priv.h"
#include "rtl9617c-ni.h"
#include "rtl9617c-xram-format.h"

unsigned int rtl9617c_ni_pointer(struct rtl9617c_ni *priv, unsigned int reg)
{
	return rtl_ni_read(priv->regs, reg) & RTL_NI_XRAM_POINTER_MASK;
}

int rtl9617c_ni_poll(struct napi_struct *napi, int budget)
{
	struct rtl9617c_ni *priv = container_of(napi, struct rtl9617c_ni, napi);
	struct net_device *dev = priv->ports[priv->port] ? : priv->dummy;
	int packets = 0;

	if (priv->qm_rx) {
		unsigned long flags;

		packets = rtl_qm_rx_poll(&priv->qm, napi, budget);

		spin_lock_irqsave(&priv->rx_lock, flags);
		if (packets < budget && napi_complete_done(napi, packets) && priv->rx_active)
			rtl_update(priv, RTL_NI_QM_EQ_IRQ_ENABLE_LO, 0xff, 0xff);
		spin_unlock_irqrestore(&priv->rx_lock, flags);
		return packets;
	}

	while (packets < budget) {
		unsigned int rd = rtl9617c_ni_pointer(priv, RTL_NI_RX_READ);
		unsigned int wr = rtl9617c_ni_pointer(priv, RTL_NI_RX_WRITE);
		unsigned int offset, next, size, copied;
		struct sk_buff *skb;
		u32 header, status;

		if (rd == wr)
			break;
		header = readl(priv->xram + rd * 8 + 4);
		if (header & RTL_NI_XRAM_OWN)
			break;
		next = header & RTL_NI_XRAM_POINTER_MASK;
		if (next >= RTL_XRAM_RX_END || next == rd) {
			dev->stats.rx_errors++;
			rtl_ni_write(priv->regs, RTL_NI_RX_READ, wr);
			break;
		}
		offset = rtl_xram_word_next(rd * 8 + 4, 0, RTL_XRAM_RX_END * 8);
		status = readl(priv->xram + offset);
		size = FIELD_GET(RTL_NI_XRAM_PKT_SIZE, status);
		packets++;
		if (!(status & RTL_NI_XRAM_PKT_VALID) || (status & RTL_NI_XRAM_PKT_ERRORS) ||
		    size < ETH_HLEN + ETH_FCS_LEN || size > 1522) {
			dev->stats.rx_errors++;
			rtl_ni_write(priv->regs, RTL_NI_RX_READ, next);
			continue;
		}
		size -= ETH_FCS_LEN;
		skb = napi_alloc_skb(napi, size);
		if (!skb) {
			dev->stats.rx_dropped++;
			rtl_ni_write(priv->regs, RTL_NI_RX_READ, next);
			continue;
		}
		offset = rtl_xram_word_next(offset, 0, RTL_XRAM_RX_END * 8);
		offset = rtl_xram_word_next(offset, 0, RTL_XRAM_RX_END * 8);
		for (copied = 0; copied < size; copied += 4) {
			__le32 word = cpu_to_le32(readl(priv->xram + offset));

			memcpy(skb_put(skb, min(4U, size - copied)), &word,
			       min(4U, size - copied));
			offset = rtl_xram_word_next(offset, 0, RTL_XRAM_RX_END * 8);
		}
		rtl_ni_write(priv->regs, RTL_NI_RX_READ, next);
		skb->protocol = eth_type_trans(skb, dev);
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += size;
		napi_gro_receive(napi, skb);
	}
	if (packets < budget)
		napi_complete_done(napi, packets);
	return packets;
}

irqreturn_t rtl9617c_ni_rx_irq(int irq, void *data)
{
	struct rtl9617c_ni *priv = data;

	spin_lock(&priv->rx_lock);
	rtl_update(priv, RTL_NI_QM_EQ_IRQ_ENABLE_LO, 0xff, 0);
	spin_unlock(&priv->rx_lock);
	napi_schedule_irqoff(&priv->napi);
	return IRQ_HANDLED;
}
