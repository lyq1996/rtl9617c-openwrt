// SPDX-License-Identifier: GPL-2.0
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb/phy.h>

#include <linux/soc/realtek/taurus-soc.h>

#define TAURUS_USB2PHY_NAME "taurus-usb2phy"
#define TAURUS_USB2PHY_VER_UNKNOWN "unknown"

#define TAURUS_USB2_PORTNUM 2

#define USB2CFG_PHY_PORT0_CONFIG_OFFSET		0x00
#define USB2CFG_PHY_PORT1_CONFIG_OFFSET		0x08
#define USB2CFG_PHY_PORT_VSTATUS_IN_MASK(x)	((0xff & (x)) << 24)
#define USB2CFG_PHY_PORT_VCNTRL_MASK(x)		((0xf & (x)) << 20)
#define USB2CFG_PHY_PORT_VLOADM			BIT(19)
#define USB2CFG_PHY_PORT_BY_PASS_ON		BIT(18)
#define USB2CFG_PHY_PORT_LF_PD_R_EN		BIT(9)
#define USB2CFG_PHY_PORT_CLKTSTEN		BIT(8)
#define USB2CFG_PHY_PORT_DPPULLDOWN		BIT(6)
#define USB2CFG_PHY_PORT_DMPULLDOWN		BIT(5)
#define USB2CFG_PHY_PORT_TXBITSTUFF_ENABLE	BIT(4)
#define USB2CFG_PHY_PORT_TXBITSTUFF_ENABLE_H	BIT(3)
#define USB2CFG_PHY_PORT_TX_ENABLE_N		BIT(2)
#define USB2CFG_PHY_PORT_TX_DAT			BIT(1)
#define USB2CFG_PHY_PORT_TX_SE0			BIT(0)

#define USB2CFG_PHY_PORT0_STATUS_OFFSET		0x04
#define USB2CFG_PHY_PORT1_STATUS_OFFSET		0x0c
#define USB2CFG_PHY_PORT_VSTATUS_OUT_MASK(x)	((0xff & (x)) << 24)
#define USB2CFG_PHY_PORT_DEBUG_MASK(x)		(0xff & (x))

#define USB2CFG_PHY_PORT0_FORCE_MODE_OFFSET	0x10
#define USB2CFG_PHY_PORT0_RX_TERM_SEL		BIT(9)
#define USB2CFG_PHY_PORT0_FSUS			BIT(7)
#define USB2CFG_PHY_PORT1_FORCE_MODE_OFFSET	0x14
#define USB2CFG_PHY_PORT1_RX_TERM_SEL		BIT(25)

struct usb2_phy_data {
	int support_xtal_40;
};

static const struct usb2_phy_data taurus_usb_data = {
	.support_xtal_40 = false,
};

struct phy_data {
	int size;
	u8 *addr;
	u8 *data;
	const char *ver;
};

struct taurus_usb2_phy {
	struct usb_phy phy;
	struct device *dev;
	struct phy_data *phy_data;
	void __iomem *phy_regbase;
	int port_mask;
	struct reset_control *usbcore_reset;
	struct reset_control *usb2port1_reset;
	struct gpio_desc *u2port0_vbus;
	int support_xtal_40;
	spinlock_t lock;
};

static void taurus_usb2_host_reset(struct taurus_usb2_phy *taurus_phy)
{
	u32 reg_val = 0;
	unsigned long flags;
	int port_mask = taurus_phy->port_mask;

	reset_control_assert(taurus_phy->usbcore_reset);
	msleep(20);
	reset_control_deassert(taurus_phy->usbcore_reset);
	msleep(20);

	if (port_mask & 0x2) {
		if (reset_control_status(taurus_phy->usb2port1_reset) > 0)
			reset_control_deassert(taurus_phy->usb2port1_reset);
	}

	spin_lock_irqsave(&taurus_phy->lock, flags);

	if (port_mask & 0x1) {
		reg_val = readl(taurus_phy->phy_regbase +
				USB2CFG_PHY_PORT0_CONFIG_OFFSET);
		reg_val |= 0x00080064;
		writel(reg_val, taurus_phy->phy_regbase +
				USB2CFG_PHY_PORT0_CONFIG_OFFSET);
	}

	if (port_mask & 0x2) {
		reg_val = readl(taurus_phy->phy_regbase +
				USB2CFG_PHY_PORT1_CONFIG_OFFSET);
		reg_val |= 0x00080064;
		writel(reg_val, taurus_phy->phy_regbase +
				USB2CFG_PHY_PORT1_CONFIG_OFFSET);
	}

	spin_unlock_irqrestore(&taurus_phy->lock, flags);
}

static void taurus_usb2_rx_term_setup(struct taurus_usb2_phy *taurus_phy)
{
	u32 reg_val = 0;
	unsigned long flags;
	int port_mask = taurus_phy->port_mask;

	spin_lock_irqsave(&taurus_phy->lock, flags);

	if (port_mask & 0x1) {
		reg_val = readl(taurus_phy->phy_regbase +
				USB2CFG_PHY_PORT0_FORCE_MODE_OFFSET);
		reg_val &= ~USB2CFG_PHY_PORT0_RX_TERM_SEL;
		reg_val |= USB2CFG_PHY_PORT0_FSUS;
		writel(reg_val, taurus_phy->phy_regbase +
				USB2CFG_PHY_PORT0_FORCE_MODE_OFFSET);
	}

	if (port_mask & 0x2) {
		reg_val = readl(taurus_phy->phy_regbase +
				USB2CFG_PHY_PORT1_FORCE_MODE_OFFSET);
		reg_val &= ~USB2CFG_PHY_PORT1_RX_TERM_SEL;
		writel(reg_val, taurus_phy->phy_regbase +
				USB2CFG_PHY_PORT1_FORCE_MODE_OFFSET);
	}

	spin_unlock_irqrestore(&taurus_phy->lock, flags);
}

static void taurus_usb2_phy_port_cfg(struct taurus_usb2_phy *taurus_phy,
				     u8 vstat_in_mask, u8 vctrl_mask,
				     int port_id)
{
	unsigned long flags;
	u32 offset = 0;
	u32 reg_val = 0;
	u8 vctrl_mask1, vctrl_mask2;

	vctrl_mask1 = vctrl_mask & 0x0f;
	vctrl_mask2 = (vctrl_mask >> 4) & 0x0f;
	reg_val |= (USB2CFG_PHY_PORT_VSTATUS_IN_MASK(vstat_in_mask) |
		    USB2CFG_PHY_PORT_VCNTRL_MASK(vctrl_mask1) |
		    USB2CFG_PHY_PORT_DPPULLDOWN |
		    USB2CFG_PHY_PORT_DMPULLDOWN |
		    USB2CFG_PHY_PORT_VLOADM |
		    USB2CFG_PHY_PORT_TX_ENABLE_N);

	if (port_id == 0)
		offset = USB2CFG_PHY_PORT0_CONFIG_OFFSET;
	else if (port_id == 1)
		offset = USB2CFG_PHY_PORT1_CONFIG_OFFSET;
	else
		return;

	spin_lock_irqsave(&taurus_phy->lock, flags);
	writel(reg_val, taurus_phy->phy_regbase + offset);

	reg_val &= ~USB2CFG_PHY_PORT_VLOADM;
	writel(reg_val, taurus_phy->phy_regbase + offset);

	reg_val |= USB2CFG_PHY_PORT_VLOADM;
	writel(reg_val, taurus_phy->phy_regbase + offset);

	reg_val &= ~USB2CFG_PHY_PORT_VCNTRL_MASK(0xf);
	reg_val |= USB2CFG_PHY_PORT_VCNTRL_MASK(vctrl_mask2);
	writel(reg_val, taurus_phy->phy_regbase + offset);

	reg_val &= ~USB2CFG_PHY_PORT_VLOADM;
	writel(reg_val, taurus_phy->phy_regbase + offset);

	reg_val |= USB2CFG_PHY_PORT_VLOADM;
	writel(reg_val, taurus_phy->phy_regbase + offset);
	spin_unlock_irqrestore(&taurus_phy->lock, flags);
}

static int taurus_usb2_phy_init(struct usb_phy *phy)
{
	struct taurus_usb2_phy *taurus_phy = (struct taurus_usb2_phy *)phy;
	struct phy_data *phy_data = taurus_phy->phy_data;
	u8 *addr;
	u8 *data;
	int size;
	int port_mask = taurus_phy->port_mask;
	int index, port_id;

	dev_info_once(phy->dev, "USB2 PHY version: %s\n", phy_data->ver);
	for (port_id = 0; port_id < TAURUS_USB2_PORTNUM; port_id++) {
		addr = phy_data->addr;
		data = phy_data->data;
		size = phy_data->size;

		if (1 << port_id & port_mask) {
			for (index = 0; index < size; index++) {
				taurus_usb2_phy_port_cfg(taurus_phy,
							 *(data + index),
							 *(addr + index),
							 port_id);
			}
		}
		phy_data++;
	}

	taurus_usb2_rx_term_setup(taurus_phy);

	return 0;
}

static void taurus_usb2_phy_shutdown(struct usb_phy *phy)
{
}

static int taurus_usb2_phy_set_suspend(struct usb_phy *phy, int suspend)
{
	return 0;
}

static int taurus_usb2_phy_set_wakeup(struct usb_phy *phy, bool enabled)
{
	return 0;
}

static int taurus_usb2_phy_set_vbus(struct usb_phy *phy, int on)
{
	struct taurus_usb2_phy *taurus_phy = (struct taurus_usb2_phy *)phy;

	if (on)
		gpiod_set_value_cansleep(taurus_phy->u2port0_vbus, 0);
	else
		gpiod_set_value_cansleep(taurus_phy->u2port0_vbus, 1);
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

static int taurus_usb2_phy_get_dts_label(struct taurus_usb2_phy *taurus_phy,
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

static int taurus_usb2_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node, *phy_node;
	struct taurus_usb2_phy *taurus_usb_phy;
	struct phy_data *phy_data;
	struct resource *res;
	const char *size, *addr, *val, *ver;
	int addr_size, data_size;
	int port_id = 0;
	int ret = 0;

	const struct usb2_phy_data *driver_data;

	taurus_usb_phy = devm_kzalloc(dev, sizeof(*taurus_usb_phy), GFP_KERNEL);
	if (!taurus_usb_phy)
		return -ENOMEM;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "u2host_reg");
	taurus_usb_phy->phy_regbase = devm_ioremap_resource(dev, res);
	if (IS_ERR(taurus_usb_phy->phy_regbase))
		return PTR_ERR(taurus_usb_phy->phy_regbase);

	taurus_usb_phy->u2port0_vbus = devm_gpiod_get_optional(dev, "u2port0-vbus",
							      GPIOD_OUT_HIGH);
	if (IS_ERR(taurus_usb_phy->u2port0_vbus)) {
		if (PTR_ERR(taurus_usb_phy->u2port0_vbus) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_info(dev, "USB2-port0 VBUS GPIO get failed, ignored\n");
		taurus_usb_phy->u2port0_vbus = NULL;
	}

	driver_data = of_device_get_match_data(dev);
	taurus_usb_phy->support_xtal_40 = (int)driver_data->support_xtal_40;

	taurus_usb_phy->dev = dev;
	taurus_usb_phy->phy.dev = taurus_usb_phy->dev;
	taurus_usb_phy->phy.label = TAURUS_USB2PHY_NAME;
	taurus_usb_phy->phy.init = taurus_usb2_phy_init;
	taurus_usb_phy->phy.shutdown = taurus_usb2_phy_shutdown;
	taurus_usb_phy->phy.set_suspend = taurus_usb2_phy_set_suspend;
	taurus_usb_phy->phy.set_wakeup = taurus_usb2_phy_set_wakeup;
	taurus_usb_phy->phy.set_vbus = taurus_usb2_phy_set_vbus;
	taurus_usb_phy->phy.type = USB_PHY_TYPE_USB2;

	spin_lock_init(&taurus_usb_phy->lock);

	phy_data = devm_kzalloc(dev, sizeof(*phy_data) * TAURUS_USB2_PORTNUM,
				GFP_KERNEL);
	if (!phy_data)
		return -ENOMEM;

	taurus_usb_phy->usbcore_reset = devm_reset_control_get_exclusive(dev,
							     "usbcore_reset");
	if (IS_ERR(taurus_usb_phy->usbcore_reset)) {
		ret = PTR_ERR(taurus_usb_phy->usbcore_reset);
		dev_err(dev, "Failed to get usbcore_reset\n");
		return ret;
	}

	taurus_usb_phy->usb2port1_reset = devm_reset_control_get_exclusive(dev,
							       "usb2port1_reset");
	if (IS_ERR(taurus_usb_phy->usb2port1_reset)) {
		ret = PTR_ERR(taurus_usb_phy->usb2port1_reset);
		dev_err(dev, "Failed to get usb2port1_reset\n");
		return ret;
	}

	ret = of_property_read_u32_index(node, "portmask", 0,
					 &taurus_usb_phy->port_mask);
	if (ret)
		goto err;

	taurus_usb_phy->phy_data = phy_data;

	taurus_usb2_phy_get_dts_label(taurus_usb_phy, &size, &addr, &val, &ver);

	for (port_id = 0; port_id < TAURUS_USB2_PORTNUM; port_id++) {
		ret = of_property_read_string(node, ver, &phy_data->ver);
		if (ret)
			phy_data->ver = TAURUS_USB2PHY_VER_UNKNOWN;

		if (1 << port_id & taurus_usb_phy->port_mask) {
			phy_node = of_parse_phandle(node, "phy_sel", port_id);
			if (!phy_node) {
				dev_err(dev, "USB2 port%d missing phy_sel\n",
					port_id);
				return -ENODEV;
			}
			dev_info(dev, "USB2 port%d get %s\n", port_id,
				 phy_node->name);

			ret = of_property_read_u32_index(phy_node, size, 0,
							 &phy_data->size);
			if (!ret)
				dev_warn(dev, "phy data size from DTS was ignored\n");

			addr_size = of_property_count_elems_of_size(phy_node,
								    addr,
								    sizeof(u8));
			data_size = of_property_count_elems_of_size(phy_node,
								    val,
								    sizeof(u8));

			if (addr_size <= 0 || addr_size != data_size) {
				dev_err(dev,
					"phy address size doesn't match data size (%d != %d)\n",
					addr_size, data_size);
				of_node_put(phy_node);
				return -EINVAL;
			}

			phy_data->size = data_size;
			phy_data->addr = devm_kzalloc(dev,
						      sizeof(u8) * phy_data->size,
						      GFP_KERNEL);
			if (!phy_data->addr) {
				of_node_put(phy_node);
				return -ENOMEM;
			}

			phy_data->data = devm_kzalloc(dev,
						      sizeof(u8) * phy_data->size,
						      GFP_KERNEL);
			if (!phy_data->data) {
				of_node_put(phy_node);
				return -ENOMEM;
			}

			ret = of_property_read_u8_array(phy_node, addr,
							phy_data->addr,
							phy_data->size);
			if (ret) {
				of_node_put(phy_node);
				goto err;
			}

			ret = of_property_read_u8_array(phy_node, val,
							phy_data->data,
							phy_data->size);
			of_node_put(phy_node);
			if (ret)
				goto err;
		}
		phy_data++;
	}
	platform_set_drvdata(pdev, taurus_usb_phy);

	taurus_usb2_host_reset(taurus_usb_phy);

	ret = usb_add_phy_dev(&taurus_usb_phy->phy);
	if (ret)
		goto err;

err:
	return ret;
}

static void taurus_usb2_phy_remove(struct platform_device *pdev)
{
	struct taurus_usb2_phy *taurus_usb_phy = platform_get_drvdata(pdev);

	usb_remove_phy(&taurus_usb_phy->phy);
}

static const struct of_device_id usbphy_taurus_dt_match[] = {
	{ .compatible = "realtek,taurus-usb2phy", .data = &taurus_usb_data },
	{ },
};
MODULE_DEVICE_TABLE(of, usbphy_taurus_dt_match);

static struct platform_driver taurus_usb2_phy_driver = {
	.probe = taurus_usb2_phy_probe,
	.remove = taurus_usb2_phy_remove,
	.driver = {
		.name = TAURUS_USB2PHY_NAME,
		.of_match_table = usbphy_taurus_dt_match,
	},
};
module_platform_driver(taurus_usb2_phy_driver);

MODULE_DESCRIPTION("Realtek Taurus USB2 PHY driver");
MODULE_LICENSE("GPL");
