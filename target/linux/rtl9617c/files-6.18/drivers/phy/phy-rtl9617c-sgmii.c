// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

#define RTL_SGMII_MISC		0x0c
#define RTL_SGMII_PCS		0x10
#define RTL_SGMII_SERDES_TX	0x1c

struct rtl_sgmii_lane {
	void __iomem *control;
	void __iomem *reset;
	void __iomem *power;
	void __iomem *analog;
	void __iomem *serdes;
	struct mutex lock;
	struct phy *phy;
};

static unsigned int rtl_sgmii_pcs_value(unsigned int old, unsigned int speed)
{
	unsigned int code = speed == SPEED_2500 ? 5 : speed == SPEED_1000 ? 2 : speed == SPEED_100 ? 1 : 0;
	unsigned int ability = speed == SPEED_2500 ? 0x1001 : 0x14000 | code;

	old &= ~((31U << 2) | (0x1ffffU << 9) | (7U << 26));
	return old | (speed == SPEED_2500 ? 0x12U : 2U) << 2 | ability << 9 | code << 26 |
		(speed == SPEED_2500 ? 1U << 29 : 0);
}

static void rtl_sgmii_rate(struct rtl_sgmii_lane *lane, unsigned int speed)
{
	static const unsigned int regs[] = {4, 12, 24, 40, 64, 68, 72, 124, 76};
	static const unsigned int masks[] = {63, 62, 31, 0x7000, ~0U, ~0U, ~0U, ~0U, 0xf000};
	static const unsigned int values[] = {0, 56, 31, 0x7000, 0x34aa, 0x88aa, 0x53, 0x8008, 0};
	unsigned int bank = speed == SPEED_2500 ? 0x400 : 0x200;
	unsigned int i;
	u32 value;

	writel(readl(lane->control) & ~(BIT(24) | (3U << 25)), lane->control);
	value = readl(lane->reset);
	writel(value | BIT(5) | BIT(8), lane->reset);
	udelay(10);
	writel(value & ~(BIT(5) | BIT(8)), lane->reset);
	udelay(10);
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		unsigned int reg = bank + regs[i];

		value = i ? values[i] : speed == SPEED_2500 ? 0x10 : 0x30;
		writel((readl(lane->analog + reg) & ~masks[i]) | value,
		       lane->analog + reg);
	}
	writel((readl(lane->control) & ~(BIT(24) | (3U << 25))) |
	       (speed == SPEED_2500 ? 2U : 1U) << 25, lane->control);
	writel(rtl_sgmii_pcs_value(readl(lane->control + RTL_SGMII_PCS), speed),
	       lane->control + RTL_SGMII_PCS);
}

static int rtl_sgmii_power_on(struct phy *phy)
{
	struct rtl_sgmii_lane *lane = phy_get_drvdata(phy);
	u32 value;

	mutex_lock(&lane->lock);
	value = readl(lane->control);
	writel((value | BIT(25)) & ~BIT(26), lane->control);
	value = readl(lane->power);
	writel((value | 0xc0080000) & ~0x6020, lane->power);
	writel(0x5359, lane->serdes + RTL_SGMII_SERDES_TX);
	writel(0x30, lane->control + RTL_SGMII_MISC);
	writel(0x34200248, lane->control + RTL_SGMII_PCS);
	mutex_unlock(&lane->lock);
	return 0;
}

static int rtl_sgmii_power_off(struct phy *phy)
{
	struct rtl_sgmii_lane *lane = phy_get_drvdata(phy);
	u32 value;

	mutex_lock(&lane->lock);
	value = readl(lane->control);
	writel(value & ~BIT(25), lane->control);
	value = readl(lane->power);
	writel((value & ~0xc0080000) | 0x6020, lane->power);
	mutex_unlock(&lane->lock);
	return 0;
}

static int rtl_sgmii_set_speed(struct phy *phy, int speed)
{
	struct rtl_sgmii_lane *lane = phy_get_drvdata(phy);
	int ret = 0;

	if (speed != SPEED_10 && speed != SPEED_100 && speed != SPEED_1000 &&
	    speed != SPEED_2500)
		return -EINVAL;
	mutex_lock(&lane->lock);
	rtl_sgmii_rate(lane, speed);
	mutex_unlock(&lane->lock);
	return ret;
}

static const struct phy_ops rtl_sgmii_ops = {
	.power_on = rtl_sgmii_power_on,
	.power_off = rtl_sgmii_power_off,
	.set_speed = rtl_sgmii_set_speed,
	.owner = THIS_MODULE,
};

static int rtl_sgmii_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl_sgmii_lane *lane;

	lane = devm_kzalloc(dev, sizeof(*lane), GFP_KERNEL);
	if (!lane)
		return -ENOMEM;
	lane->control = devm_platform_ioremap_resource_byname(pdev, "control");
	if (IS_ERR(lane->control))
		return PTR_ERR(lane->control);
	lane->reset = devm_platform_ioremap_resource_byname(pdev, "reset");
	if (IS_ERR(lane->reset))
		return PTR_ERR(lane->reset);
	lane->power = devm_platform_ioremap_resource_byname(pdev, "power");
	if (IS_ERR(lane->power))
		return PTR_ERR(lane->power);
	lane->analog = devm_platform_ioremap_resource_byname(pdev, "analog");
	if (IS_ERR(lane->analog))
		return PTR_ERR(lane->analog);
	lane->serdes = devm_platform_ioremap_resource_byname(pdev, "serdes");
	if (IS_ERR(lane->serdes))
		return PTR_ERR(lane->serdes);
	mutex_init(&lane->lock);
	lane->phy = devm_phy_create(dev, NULL, &rtl_sgmii_ops);
	if (IS_ERR(lane->phy))
		return PTR_ERR(lane->phy);
	phy_set_drvdata(lane->phy, lane);
	platform_set_drvdata(pdev, lane);
	return PTR_ERR_OR_ZERO(devm_of_phy_provider_register(dev, of_phy_simple_xlate));
}

static const struct of_device_id rtl_sgmii_match[] = {
	{ .compatible = "realtek,rtl9617c-sgmii" },
	{},
};
MODULE_DEVICE_TABLE(of, rtl_sgmii_match);

static struct platform_driver rtl_sgmii_driver = {
	.probe = rtl_sgmii_probe,
	.driver = {
		.name = "rtl9617c-sgmii",
		.of_match_table = rtl_sgmii_match,
	},
};
module_platform_driver(rtl_sgmii_driver);
MODULE_DESCRIPTION("RTL9617C SGMII SerDes lane");
MODULE_LICENSE("GPL");
