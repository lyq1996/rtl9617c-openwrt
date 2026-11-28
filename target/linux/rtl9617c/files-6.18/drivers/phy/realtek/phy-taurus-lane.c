// SPDX-License-Identifier: GPL-2.0
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <dt-bindings/phy/realtek-taurus-phy.h>

#define TAURUS_USB2_LPM_PLL_ALIVE		BIT(4)
#define TAURUS_USB2_PORT_1_CLOSE		BIT(5)
#define TAURUS_USB2_REF_SEL_0			BIT(8)
#define TAURUS_USB2_REF_SEL_1			BIT(9)
#define TAURUS_USB2_REF_SEL_2			BIT(10)
#define TAURUS_USB2_REF_SEL_3			BIT(11)
#define TAURUS_SYS_PCIE2_AND_S2_SEL		BIT(18)
#define TAURUS_SYS_USB3_AND_S2_SEL		BIT(19)
#define TAURUS_S2_PCIE_USB3_SEL			BIT(20)
#define TAURUS_S2_PCIE_USB3_SGMII_SEL_0		BIT(21)
#define TAURUS_S2_PCIE_USB3_SGMII_SEL_1		BIT(22)
#define TAURUS_S3_PCIE_USB3_SEL			BIT(24)
#define TAURUS_S3_PCIE_USB3_SGMII_SEL_0		BIT(25)
#define TAURUS_S3_PCIE_USB3_SGMII_SEL_1		BIT(26)
#define TAURUS_S0_RXAUI_MODE			BIT(27)
#define TAURUS_S0_P_MDIO_ENABLE_REG		BIT(28)
#define TAURUS_S1_P_MDIO_ENABLE_REG		BIT(29)
#define TAURUS_S2_P_MDIO_ENABLE_REG		BIT(30)
#define TAURUS_S3_P_MDIO_ENABLE_REG		BIT(31)

#define TAURUS_PHY_SGMII_MISC_CONTROL		0xc
#define TAURUS_S3_SGMII_CKBUF_EN		BIT(0)
#define TAURUS_S3_SGMII_CLK_MODE_SEL_0		BIT(1)
#define TAURUS_S3_SGMII_CLK_MODE_SEL_1		BIT(2)
#define TAURUS_S3_SGMII_BG_EN			BIT(3)
#define TAURUS_S3_SGMII_MBIAS_EN		BIT(4)
#define TAURUS_S3_SGMII_RX50_LINK		BIT(5)
#define TAURUS_S3_SGMII_TX_DEEMP_SEL		BIT(7)
#define TAURUS_S3_SGMII_EQHOLD			BIT(8)

#define TAURUS_PHY_SGMII_PCS_CONTROL		0x10

#define TAURUS_S0_PCIE2_ISOLATE_0		BIT(0)
#define TAURUS_S0_PCIE2_ISOLATE_1		BIT(1)
#define TAURUS_S1_PCIE2_ISOLATE_0		BIT(2)
#define TAURUS_S1_PCIE2_ISOLATE_1		BIT(3)
#define TAURUS_S2_CPHY_ISOLATE			BIT(4)
#define TAURUS_S3_CPHY_ISOLATE			BIT(5)
#define TAURUS_S0_POW_PCIE2_0			BIT(7)
#define TAURUS_S0_POW_PCIE2_1			BIT(8)
#define TAURUS_S1_POW_PCIE2_0			BIT(9)
#define TAURUS_S1_POW_PCIE2_1			BIT(10)
#define TAURUS_S2_POW_USB3			BIT(11)
#define TAURUS_S2_POW_PCIE2			BIT(12)
#define TAURUS_S3_POW_USB3			BIT(13)
#define TAURUS_S3_POW_PCIE2			BIT(14)
#define TAURUS_S0_ISO_ANA_B			BIT(16)
#define TAURUS_S1_ISO_ANA_B			BIT(17)
#define TAURUS_S2_ISO_ANA_B			BIT(18)
#define TAURUS_S3_ISO_ANA_B			BIT(19)
#define TAURUS_S0_LANE0_CA09PC_EN		BIT(20)
#define TAURUS_S0_LANE0_CA33PC_EN		BIT(21)
#define TAURUS_S0_LANE1_CA09PC_EN		BIT(22)
#define TAURUS_S0_LANE1_CA33PC_EN		BIT(23)
#define TAURUS_S1_LANE0_CA09PC_EN		BIT(24)
#define TAURUS_S1_LANE0_CA33PC_EN		BIT(25)
#define TAURUS_S1_LANE1_CA09PC_EN		BIT(26)
#define TAURUS_S1_LANE1_CA33PC_EN		BIT(27)
#define TAURUS_S2_CA09PC_EN			BIT(28)
#define TAURUS_S2_CA33PC_EN			BIT(29)
#define TAURUS_S3_CA09PC_EN			BIT(30)
#define TAURUS_S3_CA33PC_EN			BIT(31)

struct taurus_lane_cfg {
	u32 addr;
	u32 val;
};

struct taurus_lane {
	struct taurus_lane_core *core;
	struct phy *phy;
	char *dts_name;
	phys_addr_t serdes_addr, serdes_size;
	void __iomem *serdes_base;
	struct taurus_lane_cfg *cfg;
	int cfg_cnt;
	int id;
};

struct taurus_lane_core {
	struct device *dev;
	void __iomem *reg_ctrl;
	void __iomem *reg_pwr;
	struct mutex lock;
	struct taurus_lane lanes[TAURUS_PHY_NUMS];
};

static int taurus_lane_serdes_config(struct taurus_lane *lane, bool enable)
{
	struct device *dev = lane->core->dev;
	u32 off, val;
	int i;

	if (!lane->serdes_addr || !lane->serdes_size)
		return -ENXIO;

	if (!lane->serdes_base) {
		if (!devm_request_mem_region(dev, lane->serdes_addr,
					     lane->serdes_size, lane->dts_name)) {
			dev_err(dev, "can't request region\n");
			return -ENXIO;
		}
		lane->serdes_base = devm_ioremap(dev, lane->serdes_addr,
						 lane->serdes_size);
		if (!lane->serdes_base) {
			dev_err(dev, "ioremap failed\n");
			devm_release_mem_region(dev, lane->serdes_addr,
						lane->serdes_size);
			return -ENXIO;
		}
	}

	if (!enable)
		return 0;

	if (lane->cfg_cnt > 0 && lane->cfg) {
		for (i = 0; i < lane->cfg_cnt; i++) {
			if (lane->cfg[i].addr < lane->serdes_addr)
				continue;
			off = (phys_addr_t)lane->cfg[i].addr - lane->serdes_addr;
			val = lane->cfg[i].val;

			if (off + sizeof(u32) <= lane->serdes_size)
				writel(val, lane->serdes_base + off);
		}
		return 0;
	}

	switch (lane->id) {
	case TAURUS_PHY_S0_PCIE0:
	case TAURUS_PHY_S0_PCIE1:
	case TAURUS_PHY_S1_PCIE0:
	case TAURUS_PHY_S1_PCIE1:
	case TAURUS_PHY_S2_PCIE0:
	case TAURUS_PHY_S2_USB:
	case TAURUS_PHY_S3_USB:
		break;
	case TAURUS_PHY_S3_SGMII:
		writel(0x5359, lane->serdes_base + 0x001c);
		break;
	}

	return 0;
}

static int taurus_lane_sgmii_config(struct taurus_lane *lane, bool enable)
{
	struct taurus_lane_core *core = lane->core;

	if (!enable)
		return 0;

	switch (lane->id) {
	case TAURUS_PHY_S3_SGMII:
		writel(0x00000030, core->reg_ctrl + TAURUS_PHY_SGMII_MISC_CONTROL);
		writel(0x34200248, core->reg_ctrl + TAURUS_PHY_SGMII_PCS_CONTROL);
		break;
	}

	return 0;
}

static int taurus_lane_power_config(struct taurus_lane *lane, bool enable)
{
	struct taurus_lane_core *core = lane->core;
	u32 ctrl_en_set_m, ctrl_en_clr_m;
	u32 pwr_en_set_m, pwr_en_clr_m, pwr_dis_set_m;
	u32 ctrl_v, pwr_v;

	switch (lane->id) {
	case TAURUS_PHY_S0_PCIE0:
		ctrl_en_set_m = 0;
		ctrl_en_clr_m = TAURUS_S0_RXAUI_MODE;
		pwr_en_set_m  = TAURUS_S0_POW_PCIE2_0 | TAURUS_S0_ISO_ANA_B |
				TAURUS_S0_LANE0_CA09PC_EN |
				TAURUS_S0_LANE0_CA33PC_EN;
		pwr_en_clr_m  = TAURUS_S0_PCIE2_ISOLATE_0;
		pwr_dis_set_m = TAURUS_S0_PCIE2_ISOLATE_0;
		break;
	case TAURUS_PHY_S0_PCIE1:
		ctrl_en_set_m = 0;
		ctrl_en_clr_m = 0;
		pwr_en_set_m  = TAURUS_S0_POW_PCIE2_1 |
				TAURUS_S0_LANE1_CA09PC_EN |
				TAURUS_S0_LANE1_CA33PC_EN;
		pwr_en_clr_m  = TAURUS_S0_PCIE2_ISOLATE_1;
		pwr_dis_set_m = TAURUS_S0_PCIE2_ISOLATE_1;
		break;
	case TAURUS_PHY_S0_RXAUI:
		ctrl_en_set_m = TAURUS_S0_RXAUI_MODE;
		ctrl_en_clr_m = 0;
		pwr_en_set_m  = TAURUS_S0_POW_PCIE2_0 | TAURUS_S0_ISO_ANA_B |
				TAURUS_S0_LANE0_CA09PC_EN |
				TAURUS_S0_LANE0_CA33PC_EN;
		pwr_en_clr_m  = TAURUS_S0_PCIE2_ISOLATE_0;
		pwr_en_set_m  |= TAURUS_S0_POW_PCIE2_1 |
				 TAURUS_S0_LANE1_CA09PC_EN |
				 TAURUS_S0_LANE1_CA33PC_EN;
		pwr_en_clr_m  |= TAURUS_S0_PCIE2_ISOLATE_1;
		pwr_dis_set_m = TAURUS_S0_PCIE2_ISOLATE_0 |
				TAURUS_S0_PCIE2_ISOLATE_1;
		break;
	case TAURUS_PHY_S1_PCIE0:
		ctrl_en_set_m = 0;
		ctrl_en_clr_m = 0;
		pwr_en_set_m  = TAURUS_S1_POW_PCIE2_0 | TAURUS_S1_ISO_ANA_B |
				TAURUS_S1_LANE0_CA09PC_EN |
				TAURUS_S1_LANE0_CA33PC_EN;
		pwr_en_clr_m  = TAURUS_S1_PCIE2_ISOLATE_0;
		pwr_dis_set_m = TAURUS_S1_PCIE2_ISOLATE_0;
		break;
	case TAURUS_PHY_S1_PCIE1:
		ctrl_en_set_m = 0;
		ctrl_en_clr_m = 0;
		pwr_en_set_m  = TAURUS_S1_POW_PCIE2_1 |
				TAURUS_S1_LANE1_CA09PC_EN |
				TAURUS_S1_LANE1_CA33PC_EN;
		pwr_en_clr_m  = TAURUS_S1_PCIE2_ISOLATE_1;
		pwr_dis_set_m = TAURUS_S1_PCIE2_ISOLATE_1;
		break;
	case TAURUS_PHY_S2_PCIE0:
		ctrl_en_set_m = TAURUS_SYS_PCIE2_AND_S2_SEL;
		ctrl_en_clr_m = TAURUS_S2_PCIE_USB3_SEL |
				TAURUS_S2_PCIE_USB3_SGMII_SEL_0 |
				TAURUS_S2_PCIE_USB3_SGMII_SEL_1;
		pwr_en_set_m  = TAURUS_S2_POW_PCIE2 | TAURUS_S2_ISO_ANA_B |
				TAURUS_S2_CA09PC_EN | TAURUS_S2_CA33PC_EN;
		pwr_en_clr_m  = TAURUS_S2_CPHY_ISOLATE | TAURUS_S2_POW_USB3;
		pwr_dis_set_m = TAURUS_S2_CPHY_ISOLATE;
		break;
	case TAURUS_PHY_S2_USB:
		ctrl_en_set_m = TAURUS_S2_PCIE_USB3_SEL |
				TAURUS_SYS_USB3_AND_S2_SEL;
		ctrl_en_clr_m = TAURUS_S2_PCIE_USB3_SGMII_SEL_0 |
				TAURUS_S2_PCIE_USB3_SGMII_SEL_1;
		pwr_en_set_m  = TAURUS_S2_POW_USB3 | TAURUS_S2_ISO_ANA_B |
				TAURUS_S2_CA09PC_EN | TAURUS_S2_CA33PC_EN;
		pwr_en_clr_m  = TAURUS_S2_CPHY_ISOLATE | TAURUS_S2_POW_PCIE2;
		pwr_dis_set_m = TAURUS_S2_CPHY_ISOLATE;
		break;
	case TAURUS_PHY_S3_USB:
		ctrl_en_set_m = TAURUS_S3_PCIE_USB3_SEL;
		ctrl_en_clr_m = TAURUS_S3_PCIE_USB3_SGMII_SEL_0 |
				TAURUS_S3_PCIE_USB3_SGMII_SEL_1 |
				TAURUS_SYS_USB3_AND_S2_SEL;
		pwr_en_set_m  = TAURUS_S3_POW_USB3 | TAURUS_S3_ISO_ANA_B |
				TAURUS_S3_CA09PC_EN | TAURUS_S3_CA33PC_EN;
		pwr_en_clr_m  = TAURUS_S3_CPHY_ISOLATE | TAURUS_S3_POW_PCIE2;
		pwr_dis_set_m = TAURUS_S3_CPHY_ISOLATE;
		break;
	case TAURUS_PHY_S3_SGMII:
		ctrl_en_set_m = TAURUS_S3_PCIE_USB3_SGMII_SEL_0;
		ctrl_en_clr_m = TAURUS_S3_PCIE_USB3_SGMII_SEL_1;
		pwr_en_set_m  = TAURUS_S3_ISO_ANA_B | TAURUS_S3_CA09PC_EN |
				TAURUS_S3_CA33PC_EN;
		pwr_en_clr_m  = TAURUS_S3_CPHY_ISOLATE | TAURUS_S3_POW_PCIE2 |
				TAURUS_S3_POW_USB3;
		pwr_dis_set_m = TAURUS_S3_CPHY_ISOLATE;
		break;
	case TAURUS_PHY_S3_HSGMII:
		ctrl_en_set_m = TAURUS_S3_PCIE_USB3_SGMII_SEL_1;
		ctrl_en_clr_m = TAURUS_S3_PCIE_USB3_SGMII_SEL_0;
		pwr_en_set_m  = TAURUS_S3_ISO_ANA_B | TAURUS_S3_CA09PC_EN |
				TAURUS_S3_CA33PC_EN;
		pwr_en_clr_m  = TAURUS_S3_CPHY_ISOLATE | TAURUS_S3_POW_PCIE2 |
				TAURUS_S3_POW_USB3;
		pwr_dis_set_m = TAURUS_S3_CPHY_ISOLATE;
		break;
	default:
		dev_err(core->dev, "PHY %d invalid\n", lane->id);
		return -EINVAL;
	}

	mutex_lock(&core->lock);

	ctrl_v = readl(core->reg_ctrl);
	pwr_v = readl(core->reg_pwr);

	if (enable) {
		ctrl_v |= ctrl_en_set_m;
		ctrl_v &= ~ctrl_en_clr_m;
		pwr_v |= pwr_en_set_m;
		pwr_v &= ~pwr_en_clr_m;
	} else {
		pwr_v &= ~pwr_en_set_m;
		pwr_v |= pwr_dis_set_m;
	}

	writel(ctrl_v, core->reg_ctrl);
	writel(pwr_v, core->reg_pwr);

	mutex_unlock(&core->lock);

	taurus_lane_serdes_config(lane, enable);
	taurus_lane_sgmii_config(lane, enable);

	return 0;
}

static int taurus_lane_power_on(struct phy *p)
{
	struct taurus_lane *lane = phy_get_drvdata(p);

	return taurus_lane_power_config(lane, true);
}

static int taurus_lane_power_off(struct phy *p)
{
	struct taurus_lane *lane = phy_get_drvdata(p);

	return taurus_lane_power_config(lane, false);
}

static const struct phy_ops taurus_lane_ops = {
	.power_on = taurus_lane_power_on,
	.power_off = taurus_lane_power_off,
	.owner = THIS_MODULE,
};

static void taurus_lane_serdes_probe(struct device *dev, struct device_node *np,
				     struct taurus_lane *p)
{
	phys_addr_t serdes[2];
	int ret, size, cnt;

	ret = of_property_read_u64_array(np, "serdes-reg", serdes, 2);
	if (ret) {
		p->dts_name = NULL;
		p->serdes_addr = 0;
		p->serdes_size = 0;
		p->serdes_base = NULL;

		return;
	}

	p->dts_name = (char *)np->name;
	p->serdes_addr = serdes[0];
	p->serdes_size = serdes[1];
	p->serdes_base = NULL;

	size = sizeof(struct taurus_lane_cfg);
	p->cfg_cnt = of_property_count_elems_of_size(np, "serdes-cfg", size);
	if (p->cfg_cnt > 0) {
		p->cfg = devm_kmalloc_array(dev, p->cfg_cnt, size, GFP_KERNEL);
		if (!p->cfg) {
			p->cfg_cnt = 0;
			return;
		}

		cnt = p->cfg_cnt * sizeof(struct taurus_lane_cfg) / sizeof(u32);
		of_property_read_u32_array(np, "serdes-cfg", (u32 *)p->cfg,
					   cnt);
	} else {
		p->cfg_cnt = 0;
		p->cfg = NULL;
	}
}

static int taurus_lane_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct taurus_lane_core *core;
	struct phy_provider *provider;
	struct resource *res;

	if (of_get_child_count(node) == 0) {
		dev_err(dev, "PHY no child node\n");
		return -ENODEV;
	}

	core = devm_kzalloc(dev, sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	core->dev = dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "control");
	if (!res)
		return -EINVAL;
	core->reg_ctrl = devm_ioremap(dev, res->start, resource_size(res));
	if (!core->reg_ctrl)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "power");
	if (!res)
		return -EINVAL;
	core->reg_pwr = devm_ioremap(dev, res->start, resource_size(res));
	if (!core->reg_pwr)
		return -ENOMEM;

	mutex_init(&core->lock);

	for_each_available_child_of_node_scoped(node, child) {
		unsigned int id;
		struct taurus_lane *p;

		if (of_property_read_u32(child, "reg", &id)) {
			dev_err(dev, "missing reg property\n");
			return -EINVAL;
		}

		if (id >= TAURUS_PHY_NUMS) {
			dev_err(dev, "invalid PHY id: %u\n", id);
			return -EINVAL;
		}

		if (core->lanes[id].phy) {
			dev_err(dev, "duplicated PHY id: %u\n", id);
			return -EINVAL;
		}

		p = &core->lanes[id];
		p->phy = devm_phy_create(dev, child, &taurus_lane_ops);
		if (IS_ERR(p->phy))
			return PTR_ERR(p->phy);

		p->core = core;
		p->id = id;

		taurus_lane_serdes_probe(dev, child, p);

		phy_set_drvdata(p->phy, p);
	}

	dev_set_drvdata(dev, core);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider)) {
		dev_err(dev, "failed to register PHY provider\n");
		return PTR_ERR(provider);
	}

	return 0;
}

static const struct of_device_id taurus_lane_match_table[] = {
	{ .compatible = "realtek,taurus-phy" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, taurus_lane_match_table);

static struct platform_driver taurus_lane_driver = {
	.driver = {
		.name = "taurus-lane-phy",
		.of_match_table = taurus_lane_match_table,
	},
	.probe = taurus_lane_probe,
};
module_platform_driver(taurus_lane_driver);

MODULE_DESCRIPTION("Realtek Taurus lane PHY driver");
MODULE_LICENSE("GPL");
