// SPDX-License-Identifier: GPL-2.0
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb/phy.h>

#include <linux/soc/realtek/taurus-soc.h>

#define TAURUS_USB3PHY_NAME "taurus-usb3phy"
#define TAURUS_USB3PHY_VER_UNKNOWN "unknown"

#define USB3CFG_CNTRL_OFFSET				0x00
#define USB3CFG_PHY_VAUX_RESET				BIT(31)
#define USB3CFG_PHY_VAUX_RESET_U2_PORT1			BIT(9)
#define USB3CFG_PHY_VAUX_RESET_U2_PORT0			BIT(8)

#define USB3CFG_EXT_CTRL_OFFSET				0x00
#define USB3CFG_RX50_TERM_SEL				BIT(28)

#define REG_RX_ENKOFFSET				BIT(15)
#define REG_RX_OFFSET_AUTOK				BIT(13)

#define USB3PHY_RX_OFFSET_RANGE_MASK			GENMASK(3, 2)
#define USB3PHY_RX_OFFSET_CODE_MASK			GENMASK(4, 1)

struct usb3_phy_data {
	int auto_cal;
	int support_xtal_40;
};

static const struct usb3_phy_data taurus_usb_data = {
	.auto_cal = false,
	.support_xtal_40 = false,
};

struct phy_data {
	int size;
	u8 *addr;
	u16 *data;
	const char *ver;
};

struct taurus_usb3_phy {
	struct usb_phy phy;
	struct device *dev;
	void __iomem *phy_regbase;
	void __iomem *ext_regbase;
	int phy_mask;
	struct gpio_desc *vbus;
	struct phy_data *phy_data;
	void __iomem *serdes_regbase;
	struct phy *serdes_phy;
	int auto_cal;
	int support_xtal_40;
	struct clk *bus_clk;
	spinlock_t lock;
};

static inline u32 taurus_usb3_phy_read(struct taurus_usb3_phy *taurus_phy, u8 addr)
{
	u32 offset = addr << 2;

	return readl(taurus_phy->serdes_regbase + offset);
}

static inline void taurus_usb3_phy_write(struct taurus_usb3_phy *taurus_phy,
					 u8 addr, u16 data)
{
	u32 offset = addr << 2;

	writel(data, taurus_phy->serdes_regbase + offset);
}

static void taurus_usb3_host_reset(struct taurus_usb3_phy *taurus_phy)
{
	u32 reg_val = 0;
	unsigned long flags;

	phy_power_on(taurus_phy->serdes_phy);
	msleep(20);

	spin_lock_irqsave(&taurus_phy->lock, flags);
	reg_val = readl(taurus_phy->phy_regbase + USB3CFG_CNTRL_OFFSET);
	reg_val |= (USB3CFG_PHY_VAUX_RESET |
		    USB3CFG_PHY_VAUX_RESET_U2_PORT0 |
		    USB3CFG_PHY_VAUX_RESET_U2_PORT1);
	writel(reg_val, taurus_phy->phy_regbase + USB3CFG_CNTRL_OFFSET);
	spin_unlock_irqrestore(&taurus_phy->lock, flags);
}

static void taurus_usb3_rx_term_setup(struct taurus_usb3_phy *taurus_phy)
{
	u32 reg_val = 0;
	unsigned long flags;

	spin_lock_irqsave(&taurus_phy->lock, flags);
	reg_val = readl(taurus_phy->ext_regbase + USB3CFG_EXT_CTRL_OFFSET);
	reg_val &= ~USB3CFG_RX50_TERM_SEL;
	writel(reg_val, taurus_phy->ext_regbase + USB3CFG_EXT_CTRL_OFFSET);
	spin_unlock_irqrestore(&taurus_phy->lock, flags);
}

static int taurus_usb3_phy_calibration(struct taurus_usb3_phy *taurus_phy)
{
	int range, code;
	u32 reg;

	reg = taurus_usb3_phy_read(taurus_phy, 0x0d);

	if ((~reg & (REG_RX_ENKOFFSET | REG_RX_OFFSET_AUTOK))) {
		dev_err(taurus_phy->dev,
			"failed to calibrate U3 phy, auto calibration off\n");
		return 0;
	}

	reg = taurus_usb3_phy_read(taurus_phy, 0x1f);
	code = FIELD_GET(USB3PHY_RX_OFFSET_CODE_MASK, reg);

	if (code != 0x00 && code != 0x0f) {
		dev_info(taurus_phy->dev, "calibration code: %d\n", code);
		return 0;
	}

	do {
		reg = taurus_usb3_phy_read(taurus_phy, 0x0b);
		range = FIELD_GET(USB3PHY_RX_OFFSET_RANGE_MASK, reg);
		range++;
		reg &= ~USB3PHY_RX_OFFSET_RANGE_MASK;
		reg |= FIELD_PREP(USB3PHY_RX_OFFSET_RANGE_MASK, range);
		taurus_usb3_phy_write(taurus_phy, 0x0b, reg);

		reg = taurus_usb3_phy_read(taurus_phy, 0x09);
		reg &= ~0x200;
		taurus_usb3_phy_write(taurus_phy, 0x09, reg);
		reg |= 0x200;
		taurus_usb3_phy_write(taurus_phy, 0x09, reg);

		reg = taurus_usb3_phy_read(taurus_phy, 0x1f);
		code = FIELD_GET(USB3PHY_RX_OFFSET_CODE_MASK, reg);

		if (code != 0x00 && code != 0x0f) {
			dev_info(taurus_phy->dev,
				 "calibration range: %d code: %d\n", range,
				 code);
			break;
		}
	} while (range < 3);

	if (code == 0x00 || code == 0x0f)
		dev_err(taurus_phy->dev,
			"failed to calibrate U3 phy, code: %d\n", code);

	return 0;
}

static int taurus_usb3_phy_init(struct usb_phy *phy)
{
	struct taurus_usb3_phy *taurus_phy = (struct taurus_usb3_phy *)phy;
	u8 *addr = taurus_phy->phy_data->addr;
	u16 *data = taurus_phy->phy_data->data;
	int size = taurus_phy->phy_data->size;
	int index;

	dev_info(phy->dev, "USB3 PHY version: %s\n", taurus_phy->phy_data->ver);

	for (index = 0; index < size; index++)
		taurus_usb3_phy_write(taurus_phy, *(addr + index),
				      *(data + index));

	taurus_usb3_rx_term_setup(taurus_phy);

	usb_phy_vbus_on(phy);

	if (taurus_phy->auto_cal)
		taurus_usb3_phy_calibration(taurus_phy);

	return 0;
}

static void taurus_usb3_phy_shutdown(struct usb_phy *phy)
{
	struct taurus_usb3_phy *taurus_phy = (struct taurus_usb3_phy *)phy;

	phy_power_off(taurus_phy->serdes_phy);
}

static int taurus_usb3_phy_set_suspend(struct usb_phy *phy, int suspend)
{
	return 0;
}

static int taurus_usb3_phy_set_wakeup(struct usb_phy *phy, bool enabled)
{
	return 0;
}

static int taurus_usb3_phy_set_vbus(struct usb_phy *phy, int on)
{
	struct taurus_usb3_phy *taurus_phy = (struct taurus_usb3_phy *)phy;

	if (on)
		gpiod_set_value_cansleep(taurus_phy->vbus, 1);
	else
		gpiod_set_value_cansleep(taurus_phy->vbus, 0);
	msleep(100);

	return 0;
}

enum {
	REV_A = 0,
	REV_B,
	REV_A_40M,
	REV_B_40M,
	REV_CNT,
};

static const char *phy_ver_str[] = {
	"phy_data_verA",
	"phy_data_verB",
	"phy_data_verA_40M",
	"phy_data_verB_40M",
};

static const char *phy_size_str[] = {
	"phy_data_sizeA",
	"phy_data_sizeB",
	"phy_data_sizeA_40M",
	"phy_data_sizeB_40M",
};

static const char *phy_addr_str[] = {
	"phy_data_addrA",
	"phy_data_addrB",
	"phy_data_addrA_40M",
	"phy_data_addrB_40M",
};

static const char *phy_val_str[] = {
	"phy_data_arrayA",
	"phy_data_arrayB",
	"phy_data_arrayA_40M",
	"phy_data_arrayB_40M",
};

static int taurus_usb3_phy_get_dts_label(struct taurus_usb3_phy *taurus_phy,
					 const char **s, const char **a,
					 const char **v, const char **r)
{
	struct taurus_soc_data soc;
	int rev, ret;
	unsigned int soc_clk;

	ret = taurus_soc_data_get(&soc);
	if (ret)
		soc.chip_revision = 'A';

	soc_clk = taurus_soc_refclk_khz();

	switch (soc.chip_revision) {
	case 'A':
		rev = REV_A;
		if (taurus_phy->support_xtal_40 && (soc_clk == 40000))
			rev = REV_A_40M;
		break;
	case 'B':
	default:
		rev = REV_B;
		if (taurus_phy->support_xtal_40 && (soc_clk == 40000))
			rev = REV_B_40M;
		break;
	}
	dev_info(taurus_phy->dev, "chip revision %d using params %s\n",
		 soc.chip_revision, phy_ver_str[rev]);

	*s = phy_size_str[rev];
	*a = phy_addr_str[rev];
	*v = phy_val_str[rev];
	*r = phy_ver_str[rev];

	return 0;
}

static int taurus_usb3_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct taurus_usb3_phy *taurus_usb_phy;
	struct phy_data *phy_data;
	struct resource *res;
	int ret = 0;
	const char *size, *addr, *val, *ver;
	int addr_size, data_size;
	const struct usb3_phy_data *driver_data;

	taurus_usb_phy = devm_kzalloc(dev, sizeof(*taurus_usb_phy), GFP_KERNEL);
	if (!taurus_usb_phy)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "u3host_reg");
	taurus_usb_phy->phy_regbase = devm_ioremap_resource(dev, res);
	if (IS_ERR(taurus_usb_phy->phy_regbase))
		return PTR_ERR(taurus_usb_phy->phy_regbase);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "u3ext_reg");
	taurus_usb_phy->ext_regbase = devm_ioremap_resource(dev, res);
	if (IS_ERR(taurus_usb_phy->ext_regbase))
		return PTR_ERR(taurus_usb_phy->ext_regbase);

	taurus_usb_phy->vbus = devm_gpiod_get_optional(dev, "usb-vbus",
						      GPIOD_OUT_LOW);
	if (IS_ERR(taurus_usb_phy->vbus)) {
		if (PTR_ERR(taurus_usb_phy->vbus) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_info(dev, "USB3-port VBUS GPIO get failed, ignored\n");
		taurus_usb_phy->vbus = NULL;
	}

	taurus_usb_phy->bus_clk = devm_clk_get_optional_enabled(dev, NULL);
	if (IS_ERR(taurus_usb_phy->bus_clk)) {
		if (PTR_ERR(taurus_usb_phy->bus_clk) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_info(dev, "get usb bus clk failed and just ignored\n");
		taurus_usb_phy->bus_clk = NULL;
	}
	driver_data = of_device_get_match_data(dev);
	taurus_usb_phy->auto_cal = (int)driver_data->auto_cal;
	taurus_usb_phy->support_xtal_40 = (int)driver_data->support_xtal_40;

	taurus_usb_phy->dev = dev;
	taurus_usb_phy->phy.dev = taurus_usb_phy->dev;
	taurus_usb_phy->phy.label = TAURUS_USB3PHY_NAME;
	taurus_usb_phy->phy.init = taurus_usb3_phy_init;
	taurus_usb_phy->phy.shutdown = taurus_usb3_phy_shutdown;
	taurus_usb_phy->phy.set_suspend = taurus_usb3_phy_set_suspend;
	taurus_usb_phy->phy.set_wakeup = taurus_usb3_phy_set_wakeup;
	taurus_usb_phy->phy.set_vbus = taurus_usb3_phy_set_vbus;
	taurus_usb_phy->phy.type = USB_PHY_TYPE_USB3;

	spin_lock_init(&taurus_usb_phy->lock);

	phy_data = devm_kzalloc(dev, sizeof(*phy_data), GFP_KERNEL);
	if (!phy_data)
		return -ENOMEM;

	taurus_usb3_phy_get_dts_label(taurus_usb_phy, &size, &addr, &val, &ver);

	ret = of_property_read_u32_index(node, "phymask", 0,
					 &taurus_usb_phy->phy_mask);
	if (ret)
		goto err;
	if (taurus_usb_phy->phy_mask != 0x1 &&
	    taurus_usb_phy->phy_mask != 0x2) {
		dev_err(dev,
			"Invalid PHY mask (%d), should be 1(S2) or 2(S3)\n",
			taurus_usb_phy->phy_mask);
		return -EINVAL;
	}

	ret = of_property_read_u32_index(node, size, 0, &phy_data->size);
	if (!ret)
		dev_warn(dev, "phy data size from DTS was ignored\n");

	addr_size = of_property_count_elems_of_size(node, addr, sizeof(u8));
	data_size = of_property_count_elems_of_size(node, val, sizeof(u16));
	if (addr_size <= 0 || addr_size != data_size) {
		dev_err(dev,
			"phy address size doesn't match data size (%d != %d)\n",
			addr_size, data_size);
		return -EINVAL;
	}

	phy_data->size = data_size;
	phy_data->addr = devm_kzalloc(dev, sizeof(u8) * phy_data->size,
				      GFP_KERNEL);
	if (!phy_data->addr)
		return -ENOMEM;
	phy_data->data = devm_kzalloc(dev, sizeof(u16) * phy_data->size,
				      GFP_KERNEL);
	if (!phy_data->data)
		return -ENOMEM;
	ret = of_property_read_u8_array(node, addr, phy_data->addr,
					phy_data->size);
	if (ret)
		goto err;
	ret = of_property_read_u16_array(node, val, phy_data->data,
					 phy_data->size);
	if (ret)
		goto err;
	ret = of_property_read_string(node, ver, &phy_data->ver);
	if (ret)
		phy_data->ver = TAURUS_USB3PHY_VER_UNKNOWN;
	taurus_usb_phy->phy_data = phy_data;

	if (taurus_usb_phy->phy_mask == 0x1) {
		taurus_usb_phy->serdes_phy = devm_phy_get(dev, "s2usb3-phy");
		if (IS_ERR(taurus_usb_phy->serdes_phy)) {
			ret = PTR_ERR(taurus_usb_phy->serdes_phy);
			dev_err(dev, "Failed to get serdes_phy\n");
			return ret;
		}

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   "s2usbphy_u3port");
		taurus_usb_phy->serdes_regbase = devm_ioremap_resource(dev, res);
		if (IS_ERR(taurus_usb_phy->serdes_regbase))
			return PTR_ERR(taurus_usb_phy->serdes_regbase);
	} else if (taurus_usb_phy->phy_mask == 0x2) {
		taurus_usb_phy->serdes_phy = devm_phy_get(dev, "s3usb3-phy");
		if (IS_ERR(taurus_usb_phy->serdes_phy)) {
			ret = PTR_ERR(taurus_usb_phy->serdes_phy);
			dev_err(dev, "Failed to get serdes_phy\n");
			return ret;
		}

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   "s3usbphy_u3port");
		taurus_usb_phy->serdes_regbase = devm_ioremap_resource(dev, res);
		if (IS_ERR(taurus_usb_phy->serdes_regbase))
			return PTR_ERR(taurus_usb_phy->serdes_regbase);
	}

	platform_set_drvdata(pdev, taurus_usb_phy);
	taurus_usb3_host_reset(taurus_usb_phy);

	ret = usb_add_phy_dev(&taurus_usb_phy->phy);
	if (ret)
		goto err;

err:
	return ret;
}

static void taurus_usb3_phy_remove(struct platform_device *pdev)
{
	struct taurus_usb3_phy *taurus_usb_phy = platform_get_drvdata(pdev);

	usb_remove_phy(&taurus_usb_phy->phy);
}

static const struct of_device_id usbphy_taurus_dt_match[] = {
	{ .compatible = "realtek,taurus-usb3phy", .data = &taurus_usb_data },
	{ },
};
MODULE_DEVICE_TABLE(of, usbphy_taurus_dt_match);

static struct platform_driver taurus_usb3_phy_driver = {
	.probe = taurus_usb3_phy_probe,
	.remove = taurus_usb3_phy_remove,
	.driver = {
		.name = TAURUS_USB3PHY_NAME,
		.of_match_table = usbphy_taurus_dt_match,
	},
};
module_platform_driver(taurus_usb3_phy_driver);

MODULE_DESCRIPTION("Realtek Taurus USB3 PHY driver");
MODULE_LICENSE("GPL");
