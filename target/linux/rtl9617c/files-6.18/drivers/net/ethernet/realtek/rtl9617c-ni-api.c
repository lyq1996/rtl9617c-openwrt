/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/etherdevice.h>
#include <linux/export.h>
#include <linux/if_vlan.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/spinlock.h>

#include "rtl9617c-dma.h"
#include "rtl9617c-fe.h"
#include "rtl9617c-ni-priv.h"
#include "rtl9617c-ni.h"

int rtl9617c_ni_fe_add_port(struct device *dev, struct net_device *netdev,
			    unsigned int llid)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);

	return rtl_fe_add_port(priv->fe, netdev, 7, 32 + llid, llid);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_add_port);

int rtl9617c_ni_itu_paths(struct device *dev, struct net_device *netdev,
			  const struct rtl_itu_paths *paths)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);

	return rtl_fe_set_itu_paths(priv->fe, netdev, paths);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_itu_paths);

void rtl9617c_ni_fe_remove_port(struct device *dev, struct net_device *netdev)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);

	rtl_fe_remove_port(priv->fe, netdev);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_remove_port);

int rtl9617c_ni_fe_setup_tc(struct device *dev, struct net_device *netdev,
			    enum tc_setup_type type, void *data)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);

	return rtl_fe_setup_tc(priv->fe, netdev, type, data);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_setup_tc);

int rtl9617c_ni_fe_enable(struct device *dev)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);

	return rtl_fe_enable(priv->fe);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_enable);

int rtl9617c_ni_fe_disable(struct device *dev)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);

	return rtl_fe_disable(priv->fe);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_disable);

ssize_t rtl9617c_ni_fe_state(struct device *dev, char *buffer)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);

	return rtl_fe_state(priv->fe, buffer);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_state);

int rtl9617c_ni_report_configure(struct device *dev, unsigned int low, unsigned int high)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);

	if (!priv)
		return -EPROBE_DEFER;
	rtl_ni_write(priv->regs, RTL_NI_REPORT, (high << 14) | low);
	return 0;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_report_configure);

u32 rtl9617c_ni_report_state(struct device *dev)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);
	return rtl_ni_read(priv->regs, RTL_NI_REPORT);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_report_state);

u32 rtl9617c_ni_pon_gate(struct device *dev, u32 enabled)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);
	unsigned long flags;
	u32 saved;

	spin_lock_irqsave(&priv->tx_lock, flags);
	saved = rtl_ni_read(priv->regs, RTL_NI_PUC_DEQUEUE);
	rtl_ni_write(priv->regs, RTL_NI_PUC_DEQUEUE, (saved & ~0xffffU) | (enabled & 0xffffU));
	rtl_ni_read(priv->regs, RTL_NI_PUC_DEQUEUE);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return saved & 0xffffU;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_pon_gate);

int rtl9617c_ni_pon_attach(struct device *dev, struct rtl9617c_ni_endpoint *endpoint)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);

	if (!priv->qm_rx || !priv->dma_tx)
		return -EOPNOTSUPP;
	rcu_assign_pointer(priv->qm.pon, endpoint);
	return 0;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_pon_attach);

void rtl9617c_ni_pon_detach(struct device *dev)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);

	rcu_assign_pointer(priv->qm.pon, NULL);
	synchronize_net();
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_pon_detach);

int rtl9617c_ni_open(struct device *dev)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);

	return rtl9617c_ni_engine_get(priv);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_open);

void rtl9617c_ni_close(struct device *dev, struct net_device *owner)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);
	unsigned long flags;

	rtl_fe_flush_port(priv->fe, owner);
	if (priv->dma_tx) {
		spin_lock_irqsave(&priv->tx_lock, flags);
		rtl_dma_tx_forget(&priv->dma, owner);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
	}
	rtl9617c_ni_engine_put(priv);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_close);

netdev_tx_t rtl9617c_ni_transmit(struct device *dev, struct net_device *owner,
			       struct sk_buff *skb, const struct rtl9617c_ni_frame *frame)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);
	unsigned long flags;
	netdev_tx_t result;

	if (!priv->dma_tx) {
		owner->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&priv->tx_lock, flags);
	result = rtl_dma_tx_frame(&priv->dma, skb, owner, frame);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return result;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_transmit);

int rtl9617c_ni_oam_configure(struct device *dev)
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);
	static const unsigned int index[] = {15, 0x8f, 0x4f, 0xcf};
	u32 status;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(index); i++) {
		rtl_ni_write(priv->regs, RTL_NI_QM_PORT_MAP_DATA, 12);
		rtl_ni_write(priv->regs, RTL_NI_QM_PORT_MAP_CMD, BIT(31) | BIT(30) | index[i]);
		ret = regmap_read_poll_timeout(priv->regs, RTL_NI_QM_PORT_MAP_CMD, status, !(status & BIT(31)), 1, 100000);
		if (ret)
			return ret;
	}
	rtl_ni_write(priv->regs, RTL_NI_PORT_TABLE_ENTRY(12), 0xff | (6 << 16));
	rtl_update(priv, RTL_NI_L2TM_ES_CTRL, BIT(12), BIT(12));
	return 0;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_oam_configure);
