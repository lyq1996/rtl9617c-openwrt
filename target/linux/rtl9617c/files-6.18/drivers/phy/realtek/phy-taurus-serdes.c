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

#define TAURUS_SDS_CTRL			0x00
#define TAURUS_SDS_CTRL_HOLD		BIT(0)
#define TAURUS_SDS_IFACE		0x04
#define TAURUS_SDS_IFACE_SEL		GENMASK(4, 0)
#define TAURUS_SDS_IFACE_SGMII		0x02
#define TAURUS_SDS_IFACE_2500BASEX	0x12
#define TAURUS_SDS_LANE_MODE(n)		(0x08 + (n) * 4)
#define TAURUS_SDS_LANE_MODE_WORD	GENMASK(16, 0)
#define TAURUS_SDS_LANES		4
#define TAURUS_SDS_MODE_SGMII		0x4cf6
#define TAURUS_SDS_MODE_2500BASEX	0x5cf5
#define TAURUS_SDS_IND_CMD		0x4c
#define TAURUS_SDS_IND_CMD_BUSY		BIT(31)
#define TAURUS_SDS_IND_CMD_WRITE	BIT(30)
#define TAURUS_SDS_IND_CMD_ADDR		GENMASK(10, 0)
#define TAURUS_SDS_IND_DATA		0x50
#define TAURUS_SDS_STATUS		0x58
#define TAURUS_SDS_STATUS_CLK_READY	BIT(13)
#define TAURUS_SDS_PMA_CTRL		0x160
#define TAURUS_SDS_PMA_CTRL_INIT	0x009f
#define TAURUS_SDS_PAGE_REGS		32
#define TAURUS_SDS_WRITE_DELAY_US	10
#define TAURUS_SDS_CLK_POLL_US		(10 * USEC_PER_MSEC)
#define TAURUS_SDS_CLK_TIMEOUT_US	(15010 * USEC_PER_MSEC)
#define TAURUS_SDS_SETTLE_MS		400

#define TAURUS_LANE_CTRL_RATE		GENMASK(26, 24)
#define TAURUS_LANE_CTRL_RATE_2500	BIT(26)
#define TAURUS_LANE_CTRL_RATE_1000	BIT(25)
#define TAURUS_LANE_CTRL_ENABLE		BIT(25)
#define TAURUS_LANE_CTRL_DISABLE	BIT(26)
#define TAURUS_LANE_CTRL_MISC		0x0c
#define TAURUS_LANE_CTRL_MISC_INIT	0x30
#define TAURUS_LANE_CTRL_PCS		0x10
#define TAURUS_LANE_CTRL_PCS_INIT	0x34200248
#define TAURUS_LANE_PCS_RATE		GENMASK(6, 2)
#define TAURUS_LANE_PCS_ABILITY		GENMASK(25, 9)
#define TAURUS_LANE_PCS_SPEED		GENMASK(28, 26)
#define TAURUS_LANE_PCS_2500		BIT(29)
#define TAURUS_LANE_RESET_RX		BIT(5)
#define TAURUS_LANE_RESET_TX		BIT(8)
#define TAURUS_LANE_POWER_ON		0xc0080000
#define TAURUS_LANE_POWER_OFF		0x6020
#define TAURUS_LANE_TX_CTRL		0x1c
#define TAURUS_LANE_TX_IDLE		0x5359
#define TAURUS_LANE_ANALOG_BANK_1000	0x200
#define TAURUS_LANE_ANALOG_BANK_2500	0x400

struct taurus_sds_page {
	u16 page;
	u16 val[TAURUS_SDS_PAGE_REGS];
};

struct taurus_lane_reg {
	u16 offset;
	u32 mask;
	u32 val;
};

struct taurus_serdes_mode {
	phy_interface_t interface;
	u32 mode;
	u32 iface;
	int speed;
	const struct taurus_sds_page *pages;
	unsigned int num_pages;
};

struct taurus_serdes {
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

static const struct taurus_sds_page taurus_sds_common[] = {
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
			0xff00, 0xff01, 0x0000, 0xff00, 0x0000, 0x0000, 0x0000, 0x0000,
			0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
			0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
			0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		},
	},
};

static const struct taurus_sds_page taurus_sds_sgmii[] = {
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

static const struct taurus_sds_page taurus_sds_2500basex[] = {
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

static const struct taurus_lane_reg taurus_lane_1000[] = {
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

static const struct taurus_lane_reg taurus_lane_2500[] = {
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

static const struct taurus_serdes_mode taurus_serdes_modes[] = {
	{
		.interface = PHY_INTERFACE_MODE_SGMII,
		.mode = TAURUS_SDS_MODE_SGMII,
		.iface = TAURUS_SDS_IFACE_SGMII,
		.speed = SPEED_1000,
		.pages = taurus_sds_sgmii,
		.num_pages = ARRAY_SIZE(taurus_sds_sgmii),
	},
	{
		.interface = PHY_INTERFACE_MODE_2500BASEX,
		.mode = TAURUS_SDS_MODE_2500BASEX,
		.iface = TAURUS_SDS_IFACE_2500BASEX,
		.speed = SPEED_2500,
		.pages = taurus_sds_2500basex,
		.num_pages = ARRAY_SIZE(taurus_sds_2500basex),
	},
};

static void taurus_sds_rmw(void __iomem *reg, u32 mask, u32 val)
{
	writel((readl(reg) & ~mask) | val, reg);
}

static void taurus_sds_write(struct taurus_serdes *serdes, u32 addr, u16 val)
{
	writel(val, serdes->sds + TAURUS_SDS_IND_DATA);
	usleep_range(TAURUS_SDS_WRITE_DELAY_US, 2 * TAURUS_SDS_WRITE_DELAY_US);
	writel(TAURUS_SDS_IND_CMD_BUSY | TAURUS_SDS_IND_CMD_WRITE |
	       FIELD_PREP(TAURUS_SDS_IND_CMD_ADDR, addr),
	       serdes->sds + TAURUS_SDS_IND_CMD);
	usleep_range(TAURUS_SDS_WRITE_DELAY_US, 2 * TAURUS_SDS_WRITE_DELAY_US);
}

static void taurus_sds_write_pages(struct taurus_serdes *serdes,
				   const struct taurus_sds_page *pages,
				   unsigned int num_pages)
{
	unsigned int i, reg;

	for (i = 0; i < num_pages; i++)
		for (reg = 0; reg < TAURUS_SDS_PAGE_REGS; reg++)
			taurus_sds_write(serdes,
					 pages[i].page * TAURUS_SDS_PAGE_REGS + reg,
					 pages[i].val[reg]);
}

static int taurus_sds_configure(struct taurus_serdes *serdes,
				const struct taurus_serdes_mode *mode)
{
	unsigned int lane;
	u32 val;
	int ret;

	taurus_sds_rmw(serdes->sds + TAURUS_SDS_CTRL, 0, TAURUS_SDS_CTRL_HOLD);
	taurus_sds_rmw(serdes->sds + TAURUS_SDS_IFACE, 0, TAURUS_SDS_IFACE_SEL);
	usleep_range(1000, 2000);
	taurus_sds_rmw(serdes->sds + TAURUS_SDS_IFACE, TAURUS_SDS_IFACE_SEL,
		       mode->iface);
	for (lane = 0; lane < TAURUS_SDS_LANES; lane++)
		taurus_sds_rmw(serdes->sds + TAURUS_SDS_LANE_MODE(lane),
			       TAURUS_SDS_LANE_MODE_WORD, mode->mode);

	writel(TAURUS_SDS_PMA_CTRL_INIT, serdes->sds + TAURUS_SDS_IND_DATA);
	writel(TAURUS_SDS_IND_CMD_BUSY | TAURUS_SDS_IND_CMD_WRITE |
	       TAURUS_SDS_PMA_CTRL, serdes->sds + TAURUS_SDS_IND_CMD);
	usleep_range(1000, 2000);

	taurus_sds_write_pages(serdes, taurus_sds_common,
			       ARRAY_SIZE(taurus_sds_common));
	taurus_sds_write_pages(serdes, mode->pages, mode->num_pages);

	taurus_sds_rmw(serdes->sds + TAURUS_SDS_CTRL, TAURUS_SDS_CTRL_HOLD, 0);
	ret = readl_poll_timeout(serdes->sds + TAURUS_SDS_STATUS, val,
				 val & TAURUS_SDS_STATUS_CLK_READY,
				 TAURUS_SDS_CLK_POLL_US,
				 TAURUS_SDS_CLK_TIMEOUT_US);
	if (ret)
		return ret;
	msleep(TAURUS_SDS_SETTLE_MS);
	return 0;
}

static u32 taurus_lane_pcs(u32 old, int speed)
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

	old &= ~(TAURUS_LANE_PCS_RATE | TAURUS_LANE_PCS_ABILITY |
		 TAURUS_LANE_PCS_SPEED);
	return old | FIELD_PREP(TAURUS_LANE_PCS_RATE, rate) |
	       FIELD_PREP(TAURUS_LANE_PCS_ABILITY, ability) |
	       FIELD_PREP(TAURUS_LANE_PCS_SPEED, code) |
	       (speed == SPEED_2500 ? TAURUS_LANE_PCS_2500 : 0);
}

static void taurus_lane_rate(struct taurus_serdes *serdes, int speed)
{
	const struct taurus_lane_reg *regs;
	unsigned int bank, count, i;
	u32 val;

	if (speed == SPEED_2500) {
		bank = TAURUS_LANE_ANALOG_BANK_2500;
		regs = taurus_lane_2500;
		count = ARRAY_SIZE(taurus_lane_2500);
	} else {
		bank = TAURUS_LANE_ANALOG_BANK_1000;
		regs = taurus_lane_1000;
		count = ARRAY_SIZE(taurus_lane_1000);
	}

	taurus_sds_rmw(serdes->ctrl, TAURUS_LANE_CTRL_RATE, 0);
	val = readl(serdes->reset);
	writel(val | TAURUS_LANE_RESET_RX | TAURUS_LANE_RESET_TX,
	       serdes->reset);
	usleep_range(10, 20);
	writel(val & ~(TAURUS_LANE_RESET_RX | TAURUS_LANE_RESET_TX),
	       serdes->reset);
	usleep_range(10, 20);
	for (i = 0; i < count; i++)
		taurus_sds_rmw(serdes->analog + bank + regs[i].offset,
			       regs[i].mask, regs[i].val);
	taurus_sds_rmw(serdes->ctrl, TAURUS_LANE_CTRL_RATE,
		       speed == SPEED_2500 ? TAURUS_LANE_CTRL_RATE_2500 :
					     TAURUS_LANE_CTRL_RATE_1000);
	writel(taurus_lane_pcs(readl(serdes->ctrl + TAURUS_LANE_CTRL_PCS), speed),
	       serdes->ctrl + TAURUS_LANE_CTRL_PCS);
	serdes->speed = speed;
}

static int taurus_serdes_power_on(struct phy *phy)
{
	struct taurus_serdes *serdes = phy_get_drvdata(phy);

	mutex_lock(&serdes->lock);
	taurus_sds_rmw(serdes->ctrl, TAURUS_LANE_CTRL_DISABLE,
		       TAURUS_LANE_CTRL_ENABLE);
	taurus_sds_rmw(serdes->power, TAURUS_LANE_POWER_OFF,
		       TAURUS_LANE_POWER_ON);
	writel(TAURUS_LANE_TX_IDLE, serdes->tx + TAURUS_LANE_TX_CTRL);
	writel(TAURUS_LANE_CTRL_MISC_INIT, serdes->ctrl + TAURUS_LANE_CTRL_MISC);
	writel(TAURUS_LANE_CTRL_PCS_INIT, serdes->ctrl + TAURUS_LANE_CTRL_PCS);
	serdes->speed = SPEED_UNKNOWN;
	mutex_unlock(&serdes->lock);
	return 0;
}

static int taurus_serdes_power_off(struct phy *phy)
{
	struct taurus_serdes *serdes = phy_get_drvdata(phy);

	mutex_lock(&serdes->lock);
	taurus_sds_rmw(serdes->ctrl, TAURUS_LANE_CTRL_ENABLE, 0);
	taurus_sds_rmw(serdes->power, TAURUS_LANE_POWER_ON,
		       TAURUS_LANE_POWER_OFF);
	serdes->speed = SPEED_UNKNOWN;
	mutex_unlock(&serdes->lock);
	return 0;
}

static const struct taurus_serdes_mode *
taurus_serdes_find_mode(enum phy_mode mode, int submode)
{
	unsigned int i;

	if (mode != PHY_MODE_ETHERNET)
		return NULL;
	for (i = 0; i < ARRAY_SIZE(taurus_serdes_modes); i++)
		if (taurus_serdes_modes[i].interface == submode)
			return &taurus_serdes_modes[i];
	return NULL;
}

static int taurus_serdes_validate(struct phy *phy, enum phy_mode mode,
				  int submode, union phy_configure_opts *opts)
{
	return taurus_serdes_find_mode(mode, submode) ? 0 : -EOPNOTSUPP;
}

static int taurus_serdes_set_mode(struct phy *phy, enum phy_mode mode,
				  int submode)
{
	struct taurus_serdes *serdes = phy_get_drvdata(phy);
	const struct taurus_serdes_mode *m;
	int ret;

	m = taurus_serdes_find_mode(mode, submode);
	if (!m)
		return -EOPNOTSUPP;

	mutex_lock(&serdes->lock);
	ret = taurus_sds_configure(serdes, m);
	if (!ret)
		taurus_lane_rate(serdes, m->speed);
	mutex_unlock(&serdes->lock);
	return ret;
}

static int taurus_serdes_set_speed(struct phy *phy, int speed)
{
	struct taurus_serdes *serdes = phy_get_drvdata(phy);

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
		taurus_lane_rate(serdes, speed);
	mutex_unlock(&serdes->lock);
	return 0;
}

static const struct phy_ops taurus_serdes_ops = {
	.power_on = taurus_serdes_power_on,
	.power_off = taurus_serdes_power_off,
	.set_mode = taurus_serdes_set_mode,
	.set_speed = taurus_serdes_set_speed,
	.validate = taurus_serdes_validate,
	.owner = THIS_MODULE,
};

static void __iomem *taurus_serdes_map(struct platform_device *pdev,
				       const char *name)
{
	struct resource *res;
	void __iomem *base;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);

	base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	return base ?: IOMEM_ERR_PTR(-ENOMEM);
}

static int taurus_serdes_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct taurus_serdes *serdes;
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
	serdes->ctrl = taurus_serdes_map(pdev, "lane-ctrl");
	if (IS_ERR(serdes->ctrl))
		return PTR_ERR(serdes->ctrl);
	serdes->reset = taurus_serdes_map(pdev, "lane-reset");
	if (IS_ERR(serdes->reset))
		return PTR_ERR(serdes->reset);
	serdes->power = taurus_serdes_map(pdev, "lane-power");
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

	phy = devm_phy_create(dev, NULL, &taurus_serdes_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, serdes);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id taurus_serdes_of_match[] = {
	{ .compatible = "realtek,taurus-serdes" },
	{ }
};
MODULE_DEVICE_TABLE(of, taurus_serdes_of_match);

static struct platform_driver taurus_serdes_driver = {
	.probe = taurus_serdes_probe,
	.driver = {
		.name = "taurus-serdes",
		.of_match_table = taurus_serdes_of_match,
	},
};
module_platform_driver(taurus_serdes_driver);

MODULE_DESCRIPTION("Realtek Taurus Ethernet SerDes PHY driver");
MODULE_LICENSE("GPL");
