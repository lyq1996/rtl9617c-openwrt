// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitfield.h>
#include <linux/crc32.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel_read_file.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/rcupdate.h>
#include <linux/rtnetlink.h>
#include <linux/unaligned.h>
#include "rtl9617c-led.h"
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "rtl9617c-xram-format.h"
#include "rtl9617c-dma.h"
#include "rtl9617c-qm.h"
#include "rtl9617c-ni.h"
#include "rtl9617c-ni-regs.h"
#include "rtl9617c-fe.h"
#include "rtl9617c-mib.h"

#include "rtl9617c-ni-priv.h"

int rtl9617c_ni_indirect(struct regmap *regs, unsigned int reg, u32 value, u32 busy)
{
	unsigned int status;

	regmap_write(regs, reg, value);
	return regmap_read_poll_timeout(regs, reg, status, !(status & busy), 1, 100000);
}

static int rtl_engine_start(struct rtl9617c_ni *priv)
{
	unsigned int port;
	u32 config;
	int ret;

	if (priv->ports[6])
		rtl_p6_bringup(priv);

	if (priv->dma_tx) {

		rtl_ni_write(priv->regs, RTL_NI_NITX_PORT_CFG, BIT(31) | (4U << 16) | (29U << 20));
		rtl_update(priv, RTL_NI_NITX_CPU_PORT, GENMASK(14, 0), 29);
		rtl_ni_write(priv->regs, RTL_NI_NITX_CPU_EN, 0x08008021);

		rtl_update(priv, RTL_NI_PORT_TABLE_ENTRY(14), 0xff, 0xff);
		rtl_update(priv, RTL_NI_L2TM_ES_CTRL, BIT(31) | BIT(15), BIT(31) | BIT(15));
		for (port = 0; port < 5; port++)
			if (priv->ports[port]) {
				rtl_update(priv, RTL_NI_L2TM_ES_CTRL, BIT(31) | BIT(port), BIT(31) | BIT(port));
				rtl_update(priv, RTL_NI_PORT_TABLE_ENTRY(port), 0xff, 0xff);
			}
		if (priv->ports[6])
			rtl_update(priv, RTL_NI_PORT_TABLE_ENTRY(6), 0xff, 0xff);
		ret = rtl_dma_tx_start(&priv->dma);
		if (ret)
			return ret;
	}
	if (priv->qm_rx) {
		ret = rtl_qm_rx_start(&priv->qm, priv->port);
		if (ret) {
			if (priv->dma_tx)
				rtl_dma_tx_stop(&priv->dma);
			return ret;
		}

		for (port = 0; port < 5; port++)
			if (priv->ports[port])
				rtl_update(priv, RTL_NI_LAN_RX_CTRL(port),
					   BIT(28) | GENMASK(21, 16) | BIT(31),
					   BIT(28) | (16U << 16));
		if (priv->ports[6]) {
			priv->p6_rx_bypass_saved = rtl_ni_read(priv->regs, RTL_NI_XGE_BYPASS_CFG);

			rtl_update(priv, RTL_NI_XGE_BYPASS_CFG, RTL_NI_XGE_BYPASS_FIELDS,
				   RTL_NI_XGE_BYPASS_CPU0);
			priv->p6_rx_bypass_active = true;
		}
	}

	rtl_update(priv, RTL_NI_GLB_STATIC, RTL_NI_PORT_TO_CPU, priv->dma_tx ? 0xf : priv->port);

	rtl_update(priv, RTL_NI_L2TM_ES_CTRL, GENMASK(13, 0) | BIT(31),
		   GENMASK(13, 0) | BIT(31));

	rtl_mac_address(priv);
	rtl_ni_write(priv->regs, RTL_NI_RX_ADDR, (RTL_XRAM_RX_END - 1) << 16);
	rtl_ni_write(priv->regs, RTL_NI_TX_ADDR, (RTL_XRAM_TX_END - 1) << 16 | RTL_XRAM_TX_START);
	config = rtl_ni_read(priv->regs, RTL_NI_XRAM_CFG) & ~RTL_NI_XRAM_RX_PROMISC;
	rtl_ni_write(priv->regs, RTL_NI_XRAM_CFG, config | RTL_NI_XRAM_CPU_DISABLE);
	udelay(100);
	rtl_ni_write(priv->regs, RTL_NI_XRAM_CFG,
		     priv->qm_rx ? config | RTL_NI_XRAM_CPU_DISABLE :
				   config & ~RTL_NI_XRAM_CPU_DISABLE);
	rtl9617c_ni_set_rx_mode(priv);
	priv->tx_pending = false;
	priv->tx_dev = NULL;
	napi_enable(&priv->napi);
	if (priv->qm_rx) {
		unsigned long flags;

		spin_lock_irqsave(&priv->rx_lock, flags);
		priv->rx_active = true;
		rtl_update(priv, RTL_NI_QM_EQ_IRQ_ENABLE_LO, 0xff, 0xff);
		spin_unlock_irqrestore(&priv->rx_lock, flags);
	}
	hrtimer_start(&priv->timer, ms_to_ktime(1), HRTIMER_MODE_REL_SOFT);
	return 0;
}

static void rtl_engine_stop(struct rtl9617c_ni *priv)
{
	unsigned long flags;

	hrtimer_cancel(&priv->timer);
	if (priv->p6_rx_bypass_active) {
		rtl_update(priv, RTL_NI_XGE_BYPASS_CFG, RTL_NI_XGE_BYPASS_FIELDS,
			   priv->p6_rx_bypass_saved & RTL_NI_XGE_BYPASS_FIELDS);
		priv->p6_rx_bypass_active = false;
	}
	if (priv->qm_rx) {
		spin_lock_irqsave(&priv->rx_lock, flags);
		priv->rx_active = false;
		rtl_update(priv, RTL_NI_QM_EQ_IRQ_ENABLE_LO, 0xff, 0);
		spin_unlock_irqrestore(&priv->rx_lock, flags);
		synchronize_irq(priv->rx_irq);
	}
	napi_disable(&priv->napi);
	if (priv->qm_rx) {

		spin_lock_irqsave(&priv->rx_lock, flags);
		rtl_qm_rx_stop(&priv->qm);
		spin_unlock_irqrestore(&priv->rx_lock, flags);
	}
	if (priv->dma_tx)
		rtl_dma_tx_stop(&priv->dma);
	rtl_update(priv, RTL_NI_XRAM_CFG, RTL_NI_XRAM_CPU_DISABLE, RTL_NI_XRAM_CPU_DISABLE);
	return;
}

int rtl9617c_ni_engine_get(struct rtl9617c_ni *priv)
{
	int ret;

	if (priv->engine_users) {
		priv->engine_users++;
		return 0;
	}
	ret = rtl_engine_start(priv);
	if (!ret)
		priv->engine_users = 1;
	return ret;
}

void rtl9617c_ni_engine_put(struct rtl9617c_ni *priv)
{
	if (WARN_ON_ONCE(!priv->engine_users))
		return;
	if (!--priv->engine_users)
		rtl_engine_stop(priv);
}

static int rtl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl9617c_ni *priv;
	u8 mac[ETH_ALEN];
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;
	priv->dummy = alloc_netdev_dummy(0);
	if (!priv->dummy)
		return -ENOMEM;
	priv->ni = devm_platform_ioremap_resource(pdev, 0);
	priv->xram = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(priv->ni) || IS_ERR(priv->xram)) {
		ret = IS_ERR(priv->ni) ? PTR_ERR(priv->ni) : PTR_ERR(priv->xram);
		goto free_netdev;
	}
	priv->regs = devm_regmap_init_mmio(dev, priv->ni, &rtl_ni_regmap_config);
	if (IS_ERR(priv->regs)) {
		ret = PTR_ERR(priv->regs);
		goto free_netdev;
	}
	priv->p6_profile[0].path = "/etc/serdes/p6_serdes_sgmii_2g5.txt";
	priv->p6_profile[1].path = "/etc/serdes/p6_serdes_sgmii_1g.txt";
	if (platform_get_resource_byname(pdev, IORESOURCE_MEM, "p6-serdes")) {
		priv->p6_serdes = devm_platform_ioremap_resource_byname(pdev,
								       "p6-serdes");
		if (IS_ERR(priv->p6_serdes)) {
			ret = PTR_ERR(priv->p6_serdes);
			goto free_netdev;
		}
	}
	ret = of_property_read_u32(dev->of_node, "realtek,port", &priv->port);
	if (ret)
		goto free_netdev;
	ret = of_get_mac_address(dev->of_node, mac);
	if (ret)
		goto free_netdev;
	if (!is_valid_ether_addr(mac))
		eth_random_addr(mac);
	ether_addr_copy(priv->base_mac, mac);
	spin_lock_init(&priv->tx_lock);
	spin_lock_init(&priv->rx_lock);
	mutex_init(&priv->mib_lock);
	INIT_DELAYED_WORK(&priv->mib_work, rtl9617c_ni_mib_work);
	netif_napi_add(priv->dummy, &priv->napi, rtl9617c_ni_poll);
	hrtimer_setup(&priv->timer, rtl9617c_ni_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	priv->dma_tx = of_property_read_bool(dev->of_node, "realtek,dma-tx");
	if (priv->dma_tx) {
		priv->dma.registers = devm_platform_ioremap_resource(pdev, 2);
		if (IS_ERR(priv->dma.registers)) {
			ret = PTR_ERR(priv->dma.registers);
			goto del_napi;
		}
		priv->dma.port = priv->port;
		priv->dma.vp = 8;
		ret = rtl_dma_tx_alloc(dev, &priv->dma);
		if (ret)
			goto del_napi;
	}
	priv->qm_rx = of_property_read_bool(dev->of_node, "realtek,qm-rx");
	if (priv->qm_rx) {
		priv->qm.ni = priv->ni;
		priv->qm.regs = priv->regs;
		priv->qm.axi_reorder = devm_platform_ioremap_resource(pdev, 3);
		if (IS_ERR(priv->qm.axi_reorder)) {
			ret = PTR_ERR(priv->qm.axi_reorder);
			goto free_dma;
		}
		priv->qm.block_reset = devm_platform_ioremap_resource(pdev, 4);
		if (IS_ERR(priv->qm.block_reset)) {
			ret = PTR_ERR(priv->qm.block_reset);
			goto free_dma;
		}
		ret = rtl_qm_rx_alloc(dev, &priv->qm);
		if (ret)
			goto free_dma;
		priv->rx_irq = platform_get_irq(pdev, 0);
		if (priv->rx_irq < 0) {
			ret = priv->rx_irq;
			goto free_dma;
		}
		rtl_update(priv, RTL_NI_QM_EQ_IRQ_ENABLE_LO, 0xff, 0);
		ret = request_irq(priv->rx_irq, rtl9617c_ni_rx_irq, 0, "rtl9617c-rx0", priv);
		if (ret)
			goto free_dma;
		priv->irq_requested = true;
		priv->fe = rtl_fe_allocate(dev, priv->regs);
		if (IS_ERR(priv->fe)) {
			ret = PTR_ERR(priv->fe);
			priv->fe = NULL;
			goto free_dma;
		}
	}
	platform_set_drvdata(pdev, priv);
	ret = rtl_register_lans(priv, dev);
	if (ret)
		goto free_dma;
	schedule_delayed_work(&priv->mib_work, HZ);
	return 0;

free_dma:
	rtl_fe_free(priv->fe);
	if (priv->irq_requested)
		free_irq(priv->rx_irq, priv);
	if (priv->qm_rx && priv->qm.memory)
		rtl_qm_rx_free(dev, &priv->qm);
	if (priv->dma_tx)
		rtl_dma_tx_free(dev, &priv->dma);
del_napi:
	netif_napi_del(&priv->napi);
free_netdev:
	free_netdev(priv->dummy);
	return ret;
}

static void rtl_remove(struct platform_device *pdev)
{
	struct rtl9617c_ni *priv = platform_get_drvdata(pdev);
	unsigned int port;

	cancel_delayed_work_sync(&priv->mib_work);
	priv->removing = true;
	for (port = 0; port < ARRAY_SIZE(priv->ports); port++)
		rtl_unregister_lan(priv, port);
	synchronize_net();
	rtl_fe_free(priv->fe);
	if (priv->irq_requested)
		free_irq(priv->rx_irq, priv);
	if (priv->qm_rx)
		rtl_qm_rx_free(&pdev->dev, &priv->qm);
	if (priv->dma_tx)
		rtl_dma_tx_free(&pdev->dev, &priv->dma);
	netif_napi_del(&priv->napi);
	free_netdev(priv->dummy);
}

static const struct of_device_id rtl_match[] = {
	{ .compatible = "realtek,rtl9617c-xram-eth" },

	{ .compatible = "realtek,rtl9617c-ni" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl_match);

static struct platform_driver rtl_driver = {
	.probe = rtl_probe,
	.remove = rtl_remove,
	.driver = { .name = "rtl9617c-xram", .of_match_table = rtl_match },
};
module_platform_driver(rtl_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RTL9617C management XRAM Ethernet");
