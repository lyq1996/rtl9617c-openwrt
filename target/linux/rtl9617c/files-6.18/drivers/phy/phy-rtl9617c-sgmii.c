// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

#define RTL_SGMII_MISC			0x0c
#define RTL_SGMII_PCS			0x10
#define RTL_SGMII_SERDES_TX		0x1c

#define RTL_SGMII_CTRL_RATE		GENMASK(26, 24)
#define RTL_SGMII_CTRL_RATE_2G5		(2U << 25)
#define RTL_SGMII_CTRL_RATE_1G		(1U << 25)
#define RTL_SGMII_CTRL_ENABLE		BIT(25)
#define RTL_SGMII_CTRL_DISABLE		BIT(26)

#define RTL_SGMII_RESET_RX		BIT(5)
#define RTL_SGMII_RESET_TX		BIT(8)

#define RTL_SGMII_POWER_ON		0xc0080000
#define RTL_SGMII_POWER_OFF		0x6020
#define RTL_SGMII_SERDES_TX_IDLE	0x5359
#define RTL_SGMII_MISC_VALUE		0x30
#define RTL_SGMII_PCS_DEFAULT		0x34200248

#define RTL_SGMII_ANALOG_BANK_2G5	0x400
#define RTL_SGMII_ANALOG_BANK_1G	0x200

struct rtl_sgmii_rate_reg {
	unsigned int offset, mask, value;
};

struct rtl_sgmii_lane {
	void __iomem *control;
	void __iomem *reset;
	void __iomem *power;
	void __iomem *analog;
	void __iomem *serdes;
	struct mutex lock;
	struct phy *phy;
};

static const struct rtl_sgmii_rate_reg rtl_sgmii_rate_2g5[] = {
	{ 4, 63, 0x10 }, { 12, 62, 56 }, { 24, 31, 31 }, { 40, 0x7000, 0x7000 },
	{ 64, ~0U, 0x34aa }, { 68, ~0U, 0x88aa }, { 72, ~0U, 0x53 },
	{ 124, ~0U, 0x8008 }, { 76, 0xf000, 0 },
};

static const struct rtl_sgmii_rate_reg rtl_sgmii_rate_1g[] = {
	{ 4, 63, 0x30 }, { 12, 62, 56 }, { 24, 31, 31 }, { 40, 0x7000, 0x7000 },
	{ 64, ~0U, 0x34aa }, { 68, ~0U, 0x88aa }, { 72, ~0U, 0x53 },
	{ 124, ~0U, 0x8008 }, { 76, 0xf000, 0 },
};

static unsigned int rtl_sgmii_pcs_value(unsigned int old, unsigned int speed)
{
	unsigned int code, ability, rate;

	switch (speed) {
	case SPEED_2500:
		code = 5;
		ability = 0x1001;
		rate = 0x12;
		break;
	case SPEED_1000:
		code = 2;
		ability = 0x14000 | code;
		rate = 2;
		break;
	case SPEED_100:
		code = 1;
		ability = 0x14000 | code;
		rate = 2;
		break;
	default:
		code = 0;
		ability = 0x14000;
		rate = 2;
		break;
	}

	old &= ~((31U << 2) | (0x1ffffU << 9) | (7U << 26));
	return old | rate << 2 | ability << 9 | code << 26 |
	       (speed == SPEED_2500 ? BIT(29) : 0);
}

static void rtl_sgmii_rate(struct rtl_sgmii_lane *lane, unsigned int speed)
{
	const struct rtl_sgmii_rate_reg *table;
	unsigned int bank, count, i;
	u32 value;

	if (speed == SPEED_2500) {
		bank = RTL_SGMII_ANALOG_BANK_2G5;
		table = rtl_sgmii_rate_2g5;
		count = ARRAY_SIZE(rtl_sgmii_rate_2g5);
	} else {
		bank = RTL_SGMII_ANALOG_BANK_1G;
		table = rtl_sgmii_rate_1g;
		count = ARRAY_SIZE(rtl_sgmii_rate_1g);
	}

	writel(readl(lane->control) & ~RTL_SGMII_CTRL_RATE, lane->control);
	value = readl(lane->reset);
	writel(value | RTL_SGMII_RESET_RX | RTL_SGMII_RESET_TX, lane->reset);
	udelay(10);
	writel(value & ~(RTL_SGMII_RESET_RX | RTL_SGMII_RESET_TX), lane->reset);
	udelay(10);
	for (i = 0; i < count; i++) {
		unsigned int reg = bank + table[i].offset;

		writel((readl(lane->analog + reg) & ~table[i].mask) | table[i].value,
		       lane->analog + reg);
	}
	writel((readl(lane->control) & ~RTL_SGMII_CTRL_RATE) |
	       (speed == SPEED_2500 ? RTL_SGMII_CTRL_RATE_2G5 : RTL_SGMII_CTRL_RATE_1G),
	       lane->control);
	writel(rtl_sgmii_pcs_value(readl(lane->control + RTL_SGMII_PCS), speed),
	       lane->control + RTL_SGMII_PCS);
}

static int rtl_sgmii_power_on(struct phy *phy)
{
	struct rtl_sgmii_lane *lane = phy_get_drvdata(phy);

	mutex_lock(&lane->lock);
	writel((readl(lane->control) | RTL_SGMII_CTRL_ENABLE) & ~RTL_SGMII_CTRL_DISABLE,
	       lane->control);
	writel((readl(lane->power) | RTL_SGMII_POWER_ON) & ~RTL_SGMII_POWER_OFF, lane->power);
	writel(RTL_SGMII_SERDES_TX_IDLE, lane->serdes + RTL_SGMII_SERDES_TX);
	writel(RTL_SGMII_MISC_VALUE, lane->control + RTL_SGMII_MISC);
	writel(RTL_SGMII_PCS_DEFAULT, lane->control + RTL_SGMII_PCS);
	mutex_unlock(&lane->lock);
	return 0;
}

static int rtl_sgmii_power_off(struct phy *phy)
{
	struct rtl_sgmii_lane *lane = phy_get_drvdata(phy);

	mutex_lock(&lane->lock);
	writel(readl(lane->control) & ~RTL_SGMII_CTRL_ENABLE, lane->control);
	writel((readl(lane->power) & ~RTL_SGMII_POWER_ON) | RTL_SGMII_POWER_OFF, lane->power);
	mutex_unlock(&lane->lock);
	return 0;
}

static int rtl_sgmii_set_speed(struct phy *phy, int speed)
{
	struct rtl_sgmii_lane *lane = phy_get_drvdata(phy);

	if (speed != SPEED_10 && speed != SPEED_100 && speed != SPEED_1000 &&
	    speed != SPEED_2500)
		return -EINVAL;
	mutex_lock(&lane->lock);
	rtl_sgmii_rate(lane, speed);
	mutex_unlock(&lane->lock);
	return 0;
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
	int ret;

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
	ret = devm_mutex_init(dev, &lane->lock);
	if (ret)
		return ret;
	lane->phy = devm_phy_create(dev, NULL, &rtl_sgmii_ops);
	if (IS_ERR(lane->phy))
		return PTR_ERR(lane->phy);
	phy_set_drvdata(lane->phy, lane);
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
