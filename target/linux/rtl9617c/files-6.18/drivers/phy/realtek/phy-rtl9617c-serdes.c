// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

#define SDS_CTRL			0x00
#define SDS_CTRL_HOLD			BIT(0)
#define SDS_IFACE			0x04
#define SDS_IFACE_SEL			GENMASK(4, 0)
#define SDS_IFACE_SGMII			0x02
#define SDS_IFACE_2500BASEX		0x12
#define SDS_LANE_MODE(n)		(0x08 + (n) * 4)
#define SDS_LANE_MODE_WORD		GENMASK(16, 0)
#define SDS_LANES			4
#define SDS_MODE_SGMII			0x4cf6
#define SDS_MODE_2500BASEX		0x5cf5
#define SDS_IND_CMD			0x4c
#define SDS_IND_CMD_BUSY		BIT(31)
#define SDS_IND_CMD_WRITE		BIT(30)
#define SDS_IND_CMD_ADDR		GENMASK(10, 0)
#define SDS_IND_DATA			0x50
#define SDS_STATUS			0x58
#define SDS_STATUS_CLK_READY		BIT(13)
#define SDS_PMA_CTRL			0x160
#define SDS_PMA_CTRL_INIT		0x009f
#define SDS_PAGE_REGS			32
#define SDS_WRITE_DELAY_US		10
#define SDS_CLK_POLL_US			(10 * USEC_PER_MSEC)
#define SDS_CLK_TIMEOUT_US		(15010 * USEC_PER_MSEC)
#define SDS_SETTLE_MS			400

#define LANE_CTRL_RATE			GENMASK(26, 24)
#define LANE_CTRL_RATE_2500		BIT(26)
#define LANE_CTRL_RATE_1000		BIT(25)
#define LANE_CTRL_ENABLE		BIT(25)
#define LANE_CTRL_DISABLE		BIT(26)
#define LANE_CTRL_MISC			0x0c
#define LANE_CTRL_MISC_INIT		0x30
#define LANE_CTRL_PCS			0x10
#define LANE_CTRL_PCS_INIT		0x34200248
#define LANE_PCS_RATE			GENMASK(6, 2)
#define LANE_PCS_ABILITY		GENMASK(25, 9)
#define LANE_PCS_SPEED			GENMASK(28, 26)
#define LANE_PCS_2500			BIT(29)
#define LANE_RESET_RX			BIT(5)
#define LANE_RESET_TX			BIT(8)
#define LANE_POWER_ON			0xc0080000
#define LANE_POWER_OFF			0x6020
#define LANE_TX_CTRL			0x1c
#define LANE_TX_IDLE			0x5359
#define LANE_ANALOG_BANK_1000		0x200
#define LANE_ANALOG_BANK_2500		0x400

struct rtl9617c_sds_page {
	u16 page;
	u16 val[SDS_PAGE_REGS];
};

struct rtl9617c_lane_reg {
	u16 offset;
	u32 mask;
	u32 val;
};

struct rtl9617c_serdes_mode {
	phy_interface_t interface;
	u32 mode;
	u32 iface;
	int speed;
	const struct rtl9617c_sds_page *pages;
	unsigned int num_pages;
};

struct rtl9617c_serdes {
	struct device *dev;
	void __iomem *sds;
	void __iomem *ctrl;
	void __iomem *reset;
	void __iomem *power;
	void __iomem *analog;
	void __iomem *tx;
	struct mutex lock;
	int speed;
};

static const struct rtl9617c_sds_page rtl9617c_sds_common[] = {
	{
		.page = 0x21,
		.val = {
			0x4802, 0x0000, 0x0000, 0x0000, 0xf0f0, 0xf0f0, 0xf0f0, 0xf0f0,
			0x0000, 0x0000, 0x0304, 0x0180, 0xffff, 0x0000, 0x0000, 0xf000,
			0x1800, 0xffd0, 0xb87e, 0xe005, 0x0401, 0x1000, 0x4000, 0x0000,
			0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		},
	},
	{
		.page = 0x36,
		.val = {
			0x0000, 0xff00, 0xff01, 0x0000, 0xff00, 0x0000, 0x0000, 0x0000,
			0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
			0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
			0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		},
	},
};

static const struct rtl9617c_sds_page rtl9617c_sds_sgmii[] = {
	{
		.page = 0x24,
		.val = {
			0x5a65, 0x5230, 0xf0f0, 0xf0fd, 0x0240, 0x878c, 0x01c0, 0x3e00,
			0xd125, 0x2504, 0x4006, 0x2424, 0x4012, 0x4f18, 0x007e, 0x5458,
			0x49b3, 0xf0ae, 0xf060, 0x1e32, 0x200a, 0xfb68, 0x1597, 0xf0f0,
			0xf0f2, 0xf0f0, 0x4098, 0x0100, 0xf0f0, 0x0000, 0xa128, 0xc168,
		},
	},
	{
		.page = 0x25,
		.val = {
			0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
			0x0000, 0x5848, 0xff27, 0xd800, 0x0080, 0x8000, 0x4000, 0x180c,
			0x0804, 0x03fc, 0x07fc, 0x0020, 0xf0ff, 0xffff, 0xffff, 0x3e00,
			0x0000, 0x0500, 0x07ff, 0x0fe8, 0x3fdf, 0x001e, 0x8000, 0x0000,
		},
	},
	{
		.page = 0x39,
		.val = {
			0x0018, 0x11e0, 0x03ff, 0x0417, 0x4000, 0x8000, 0x0000, 0x004e,
			0x1000, 0x4000, 0x0000, 0x0280, 0x0280, 0x8007, 0x0012, 0x490b,
			0x034d, 0x0000, 0xfe00, 0x0000, 0x0005, 0x0000, 0x0000, 0x0000,
			0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		},
	},
};

static const struct rtl9617c_sds_page rtl9617c_sds_2500basex[] = {
	{
		.page = 0x28,
		.val = {
			0x5a65, 0x6430, 0xf0f0, 0xf0f9, 0x0a40, 0x878c, 0x01c0, 0x3e00,
			0xd125, 0x2544, 0x4002, 0x2424, 0x4012, 0x4f18, 0x007e, 0x5458,
			0x49b3, 0xf0ae, 0xf060, 0x1e32, 0x600a, 0xfb68, 0x1597, 0xf0f0,
			0xf0f2, 0xf0f0, 0x41b8, 0x0100, 0xf0f0, 0x0000, 0xa128, 0x8168,
		},
	},
	{
		.page = 0x29,
		.val = {
			0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
			0x0000, 0x5848, 0xff27, 0xd800, 0x0080, 0x8000, 0x4000, 0x180c,
			0x0804, 0x03fc, 0x86ff, 0x0020, 0xf0ff, 0xffff, 0xffff, 0x3e00,
			0x0000, 0x0500, 0x07ff, 0x0fe8, 0x3fdf, 0x001e, 0x8000, 0x0000,
		},
	},
	{
		.page = 0x3a,
		.val = {
			0x0018, 0x11e0, 0x03ff, 0x0417, 0x4000, 0x8000, 0x0000, 0x0062,
			0x1800, 0x4000, 0x0000, 0x0258, 0x0258, 0x8007, 0x0012, 0x490b,
			0x0208, 0x0000, 0xfe00, 0x0000, 0x0006, 0x0000, 0x0000, 0x0000,
			0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		},
	},
};

static const struct rtl9617c_lane_reg rtl9617c_lane_1000[] = {
	{ 4, 0x3f, 0x30 },
	{ 12, 0x3e, 0x38 },
	{ 24, 0x1f, 0x1f },
	{ 40, 0x7000, 0x7000 },
	{ 64, ~0U, 0x34aa },
	{ 68, ~0U, 0x88aa },
	{ 72, ~0U, 0x53 },
	{ 124, ~0U, 0x8008 },
	{ 76, 0xf000, 0 },
};

static const struct rtl9617c_lane_reg rtl9617c_lane_2500[] = {
	{ 4, 0x3f, 0x10 },
	{ 12, 0x3e, 0x38 },
	{ 24, 0x1f, 0x1f },
	{ 40, 0x7000, 0x7000 },
	{ 64, ~0U, 0x34aa },
	{ 68, ~0U, 0x88aa },
	{ 72, ~0U, 0x53 },
	{ 124, ~0U, 0x8008 },
	{ 76, 0xf000, 0 },
};

static const struct rtl9617c_serdes_mode rtl9617c_serdes_modes[] = {
	{
		.interface = PHY_INTERFACE_MODE_SGMII,
		.mode = SDS_MODE_SGMII,
		.iface = SDS_IFACE_SGMII,
		.speed = SPEED_1000,
		.pages = rtl9617c_sds_sgmii,
		.num_pages = ARRAY_SIZE(rtl9617c_sds_sgmii),
	},
	{
		.interface = PHY_INTERFACE_MODE_2500BASEX,
		.mode = SDS_MODE_2500BASEX,
		.iface = SDS_IFACE_2500BASEX,
		.speed = SPEED_2500,
		.pages = rtl9617c_sds_2500basex,
		.num_pages = ARRAY_SIZE(rtl9617c_sds_2500basex),
	},
};

static void rtl9617c_rmw(void __iomem *reg, u32 mask, u32 val)
{
	writel((readl(reg) & ~mask) | val, reg);
}

static void rtl9617c_sds_write(struct rtl9617c_serdes *serdes, u32 addr, u16 val)
{
	writel(val, serdes->sds + SDS_IND_DATA);
	usleep_range(SDS_WRITE_DELAY_US, 2 * SDS_WRITE_DELAY_US);
	writel(SDS_IND_CMD_BUSY | SDS_IND_CMD_WRITE |
	       FIELD_PREP(SDS_IND_CMD_ADDR, addr), serdes->sds + SDS_IND_CMD);
	usleep_range(SDS_WRITE_DELAY_US, 2 * SDS_WRITE_DELAY_US);
}

static void rtl9617c_sds_write_pages(struct rtl9617c_serdes *serdes,
				     const struct rtl9617c_sds_page *pages,
				     unsigned int num_pages)
{
	unsigned int i, reg;

	for (i = 0; i < num_pages; i++)
		for (reg = 0; reg < SDS_PAGE_REGS; reg++)
			rtl9617c_sds_write(serdes, pages[i].page * SDS_PAGE_REGS + reg,
					   pages[i].val[reg]);
}

static int rtl9617c_sds_configure(struct rtl9617c_serdes *serdes,
				  const struct rtl9617c_serdes_mode *mode)
{
	unsigned int lane;
	u32 val;
	int ret;

	rtl9617c_rmw(serdes->sds + SDS_CTRL, 0, SDS_CTRL_HOLD);
	rtl9617c_rmw(serdes->sds + SDS_IFACE, 0, SDS_IFACE_SEL);
	usleep_range(1000, 2000);
	rtl9617c_rmw(serdes->sds + SDS_IFACE, SDS_IFACE_SEL, mode->iface);
	for (lane = 0; lane < SDS_LANES; lane++)
		rtl9617c_rmw(serdes->sds + SDS_LANE_MODE(lane), SDS_LANE_MODE_WORD,
			     mode->mode);

	writel(SDS_PMA_CTRL_INIT, serdes->sds + SDS_IND_DATA);
	writel(SDS_IND_CMD_BUSY | SDS_IND_CMD_WRITE | SDS_PMA_CTRL,
	       serdes->sds + SDS_IND_CMD);
	usleep_range(1000, 2000);

	rtl9617c_sds_write_pages(serdes, rtl9617c_sds_common,
				 ARRAY_SIZE(rtl9617c_sds_common));
	rtl9617c_sds_write_pages(serdes, mode->pages, mode->num_pages);

	rtl9617c_rmw(serdes->sds + SDS_CTRL, SDS_CTRL_HOLD, 0);
	ret = readl_poll_timeout(serdes->sds + SDS_STATUS, val,
				 val & SDS_STATUS_CLK_READY, SDS_CLK_POLL_US,
				 SDS_CLK_TIMEOUT_US);
	if (ret)
		return ret;
	msleep(SDS_SETTLE_MS);
	return 0;
}

static u32 rtl9617c_lane_pcs(u32 old, int speed)
{
	u32 code, ability, rate;

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

	old &= ~(LANE_PCS_RATE | LANE_PCS_ABILITY | LANE_PCS_SPEED);
	return old | FIELD_PREP(LANE_PCS_RATE, rate) |
	       FIELD_PREP(LANE_PCS_ABILITY, ability) |
	       FIELD_PREP(LANE_PCS_SPEED, code) |
	       (speed == SPEED_2500 ? LANE_PCS_2500 : 0);
}

static void rtl9617c_lane_rate(struct rtl9617c_serdes *serdes, int speed)
{
	const struct rtl9617c_lane_reg *regs;
	unsigned int bank, count, i;
	u32 val;

	if (speed == SPEED_2500) {
		bank = LANE_ANALOG_BANK_2500;
		regs = rtl9617c_lane_2500;
		count = ARRAY_SIZE(rtl9617c_lane_2500);
	} else {
		bank = LANE_ANALOG_BANK_1000;
		regs = rtl9617c_lane_1000;
		count = ARRAY_SIZE(rtl9617c_lane_1000);
	}

	rtl9617c_rmw(serdes->ctrl, LANE_CTRL_RATE, 0);
	val = readl(serdes->reset);
	writel(val | LANE_RESET_RX | LANE_RESET_TX, serdes->reset);
	usleep_range(10, 20);
	writel(val & ~(LANE_RESET_RX | LANE_RESET_TX), serdes->reset);
	usleep_range(10, 20);
	for (i = 0; i < count; i++)
		rtl9617c_rmw(serdes->analog + bank + regs[i].offset, regs[i].mask,
			     regs[i].val);
	rtl9617c_rmw(serdes->ctrl, LANE_CTRL_RATE,
		     speed == SPEED_2500 ? LANE_CTRL_RATE_2500 : LANE_CTRL_RATE_1000);
	writel(rtl9617c_lane_pcs(readl(serdes->ctrl + LANE_CTRL_PCS), speed),
	       serdes->ctrl + LANE_CTRL_PCS);
	serdes->speed = speed;
}

static int rtl9617c_serdes_power_on(struct phy *phy)
{
	struct rtl9617c_serdes *serdes = phy_get_drvdata(phy);

	mutex_lock(&serdes->lock);
	rtl9617c_rmw(serdes->ctrl, LANE_CTRL_DISABLE, LANE_CTRL_ENABLE);
	rtl9617c_rmw(serdes->power, LANE_POWER_OFF, LANE_POWER_ON);
	writel(LANE_TX_IDLE, serdes->tx + LANE_TX_CTRL);
	writel(LANE_CTRL_MISC_INIT, serdes->ctrl + LANE_CTRL_MISC);
	writel(LANE_CTRL_PCS_INIT, serdes->ctrl + LANE_CTRL_PCS);
	serdes->speed = SPEED_UNKNOWN;
	mutex_unlock(&serdes->lock);
	return 0;
}

static int rtl9617c_serdes_power_off(struct phy *phy)
{
	struct rtl9617c_serdes *serdes = phy_get_drvdata(phy);

	mutex_lock(&serdes->lock);
	rtl9617c_rmw(serdes->ctrl, LANE_CTRL_ENABLE, 0);
	rtl9617c_rmw(serdes->power, LANE_POWER_ON, LANE_POWER_OFF);
	serdes->speed = SPEED_UNKNOWN;
	mutex_unlock(&serdes->lock);
	return 0;
}

static const struct rtl9617c_serdes_mode *
rtl9617c_serdes_find_mode(enum phy_mode mode, int submode)
{
	unsigned int i;

	if (mode != PHY_MODE_ETHERNET)
		return NULL;
	for (i = 0; i < ARRAY_SIZE(rtl9617c_serdes_modes); i++)
		if (rtl9617c_serdes_modes[i].interface == submode)
			return &rtl9617c_serdes_modes[i];
	return NULL;
}

static int rtl9617c_serdes_validate(struct phy *phy, enum phy_mode mode,
				    int submode, union phy_configure_opts *opts)
{
	return rtl9617c_serdes_find_mode(mode, submode) ? 0 : -EOPNOTSUPP;
}

static int rtl9617c_serdes_set_mode(struct phy *phy, enum phy_mode mode,
				    int submode)
{
	struct rtl9617c_serdes *serdes = phy_get_drvdata(phy);
	const struct rtl9617c_serdes_mode *m;
	int ret;

	m = rtl9617c_serdes_find_mode(mode, submode);
	if (!m)
		return -EOPNOTSUPP;

	mutex_lock(&serdes->lock);
	ret = rtl9617c_sds_configure(serdes, m);
	if (!ret)
		rtl9617c_lane_rate(serdes, m->speed);
	mutex_unlock(&serdes->lock);
	return ret;
}

static int rtl9617c_serdes_set_speed(struct phy *phy, int speed)
{
	struct rtl9617c_serdes *serdes = phy_get_drvdata(phy);

	switch (speed) {
	case SPEED_10:
	case SPEED_100:
	case SPEED_1000:
	case SPEED_2500:
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&serdes->lock);
	if (serdes->speed != speed)
		rtl9617c_lane_rate(serdes, speed);
	mutex_unlock(&serdes->lock);
	return 0;
}

static const struct phy_ops rtl9617c_serdes_ops = {
	.power_on = rtl9617c_serdes_power_on,
	.power_off = rtl9617c_serdes_power_off,
	.set_mode = rtl9617c_serdes_set_mode,
	.set_speed = rtl9617c_serdes_set_speed,
	.validate = rtl9617c_serdes_validate,
	.owner = THIS_MODULE,
};

static int rtl9617c_serdes_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct rtl9617c_serdes *serdes;
	struct phy *phy;
	int ret;

	serdes = devm_kzalloc(dev, sizeof(*serdes), GFP_KERNEL);
	if (!serdes)
		return -ENOMEM;
	serdes->dev = dev;
	serdes->speed = SPEED_UNKNOWN;

	serdes->sds = devm_platform_ioremap_resource_byname(pdev, "sds");
	if (IS_ERR(serdes->sds))
		return PTR_ERR(serdes->sds);
	serdes->ctrl = devm_platform_ioremap_resource_byname(pdev, "lane-ctrl");
	if (IS_ERR(serdes->ctrl))
		return PTR_ERR(serdes->ctrl);
	serdes->reset = devm_platform_ioremap_resource_byname(pdev, "lane-reset");
	if (IS_ERR(serdes->reset))
		return PTR_ERR(serdes->reset);
	serdes->power = devm_platform_ioremap_resource_byname(pdev, "lane-power");
	if (IS_ERR(serdes->power))
		return PTR_ERR(serdes->power);
	serdes->analog = devm_platform_ioremap_resource_byname(pdev, "lane-analog");
	if (IS_ERR(serdes->analog))
		return PTR_ERR(serdes->analog);
	serdes->tx = devm_platform_ioremap_resource_byname(pdev, "lane-tx");
	if (IS_ERR(serdes->tx))
		return PTR_ERR(serdes->tx);

	ret = devm_mutex_init(dev, &serdes->lock);
	if (ret)
		return ret;

	phy = devm_phy_create(dev, NULL, &rtl9617c_serdes_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, serdes);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id rtl9617c_serdes_of_match[] = {
	{ .compatible = "realtek,rtl9617c-serdes" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9617c_serdes_of_match);

static struct platform_driver rtl9617c_serdes_driver = {
	.probe = rtl9617c_serdes_probe,
	.driver = {
		.name = "rtl9617c-serdes",
		.of_match_table = rtl9617c_serdes_of_match,
	},
};
module_platform_driver(rtl9617c_serdes_driver);

MODULE_DESCRIPTION("Realtek RTL9617C Ethernet SerDes PHY driver");
MODULE_LICENSE("GPL");
