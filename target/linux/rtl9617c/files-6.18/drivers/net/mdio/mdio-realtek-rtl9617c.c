// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define MDIO_ADDR		0x04
#define MDIO_ADDR_PHY		GENMASK(4, 0)
#define MDIO_ADDR_REG		GENMASK(12, 8)
#define MDIO_ADDR_READ		BIT(15)
#define MDIO_ADDR_C45		BIT(16)
#define MDIO_ADDR_OP		GENMASK(19, 18)
#define MDIO_OP_C45_ADDR	0
#define MDIO_OP_C45_WRITE	1
#define MDIO_OP_C45_READ	3
#define MDIO_WRDATA		0x08
#define MDIO_RDDATA		0x0c
#define MDIO_CTRL		0x10
#define MDIO_CTRL_DONE		BIT(0)
#define MDIO_CTRL_START		BIT(7)
#define MDIO_POLL_US		1
#define MDIO_TIMEOUT_US		50000

#define GPHY_PHY_SHIFT		7
#define GPHY_REG_SHIFT		2

struct rtl9617c_mdio_data {
	u32 gphy_mask;
};

struct rtl9617c_mdio {
	void __iomem *base;
	void __iomem *gphy;
	u32 gphy_mask;
};

static bool rtl9617c_mdio_is_gphy(struct rtl9617c_mdio *mdio, int addr)
{
	return mdio->gphy_mask & BIT(addr);
}

static void __iomem *rtl9617c_mdio_gphy_reg(struct rtl9617c_mdio *mdio,
					    int addr, int regnum)
{
	return mdio->gphy + (addr << GPHY_PHY_SHIFT) + (regnum << GPHY_REG_SHIFT);
}

static int rtl9617c_mdio_xfer(struct rtl9617c_mdio *mdio, u32 cmd, u16 data,
			      bool read)
{
	u32 status;
	int ret;

	writel(MDIO_CTRL_DONE, mdio->base + MDIO_CTRL);
	writel(cmd, mdio->base + MDIO_ADDR);
	if (!read)
		writel(data, mdio->base + MDIO_WRDATA);
	writel(MDIO_CTRL_START, mdio->base + MDIO_CTRL);
	ret = readl_poll_timeout(mdio->base + MDIO_CTRL, status,
				 status & MDIO_CTRL_DONE, MDIO_POLL_US,
				 MDIO_TIMEOUT_US);
	if (ret)
		return ret;
	ret = read ? readl(mdio->base + MDIO_RDDATA) & 0xffff : 0;
	writel(MDIO_CTRL_DONE, mdio->base + MDIO_CTRL);
	return ret;
}

static int rtl9617c_mdio_read(struct mii_bus *bus, int addr, int regnum)
{
	struct rtl9617c_mdio *mdio = bus->priv;

	if (rtl9617c_mdio_is_gphy(mdio, addr))
		return readl(rtl9617c_mdio_gphy_reg(mdio, addr, regnum)) & 0xffff;

	return rtl9617c_mdio_xfer(mdio, FIELD_PREP(MDIO_ADDR_PHY, addr) |
				  FIELD_PREP(MDIO_ADDR_REG, regnum) |
				  MDIO_ADDR_READ, 0, true);
}

static int rtl9617c_mdio_write(struct mii_bus *bus, int addr, int regnum,
			       u16 val)
{
	struct rtl9617c_mdio *mdio = bus->priv;

	if (rtl9617c_mdio_is_gphy(mdio, addr)) {
		writel(val, rtl9617c_mdio_gphy_reg(mdio, addr, regnum));
		usleep_range(1000, 2000);
		return 0;
	}

	return rtl9617c_mdio_xfer(mdio, FIELD_PREP(MDIO_ADDR_PHY, addr) |
				  FIELD_PREP(MDIO_ADDR_REG, regnum), val, false);
}

static int rtl9617c_mdio_c45(struct rtl9617c_mdio *mdio, int addr, int devad,
			     int regnum, u16 val, bool read)
{
	u32 cmd;
	int ret;

	if (rtl9617c_mdio_is_gphy(mdio, addr))
		return -EOPNOTSUPP;

	cmd = FIELD_PREP(MDIO_ADDR_PHY, addr) | FIELD_PREP(MDIO_ADDR_REG, devad) |
	      MDIO_ADDR_C45;
	ret = rtl9617c_mdio_xfer(mdio, cmd | FIELD_PREP(MDIO_ADDR_OP, MDIO_OP_C45_ADDR),
				 regnum, false);
	if (ret)
		return ret;

	if (read)
		cmd |= FIELD_PREP(MDIO_ADDR_OP, MDIO_OP_C45_READ) | MDIO_ADDR_READ;
	else
		cmd |= FIELD_PREP(MDIO_ADDR_OP, MDIO_OP_C45_WRITE);
	return rtl9617c_mdio_xfer(mdio, cmd, val, read);
}

static int rtl9617c_mdio_read_c45(struct mii_bus *bus, int addr, int devad,
				  int regnum)
{
	return rtl9617c_mdio_c45(bus->priv, addr, devad, regnum, 0, true);
}

static int rtl9617c_mdio_write_c45(struct mii_bus *bus, int addr, int devad,
				   int regnum, u16 val)
{
	return rtl9617c_mdio_c45(bus->priv, addr, devad, regnum, val, false);
}

static int rtl9617c_mdio_probe(struct platform_device *pdev)
{
	const struct rtl9617c_mdio_data *data;
	struct device *dev = &pdev->dev;
	struct rtl9617c_mdio *mdio;
	struct mii_bus *bus;

	data = device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	bus = devm_mdiobus_alloc_size(dev, sizeof(*mdio));
	if (!bus)
		return -ENOMEM;

	mdio = bus->priv;
	mdio->gphy_mask = data->gphy_mask;
	mdio->base = devm_platform_ioremap_resource_byname(pdev, "mdio");
	if (IS_ERR(mdio->base))
		return PTR_ERR(mdio->base);
	mdio->gphy = devm_platform_ioremap_resource_byname(pdev, "gphy");
	if (IS_ERR(mdio->gphy))
		return PTR_ERR(mdio->gphy);

	bus->name = "rtl9617c-mdio";
	bus->parent = dev;
	bus->read = rtl9617c_mdio_read;
	bus->write = rtl9617c_mdio_write;
	bus->read_c45 = rtl9617c_mdio_read_c45;
	bus->write_c45 = rtl9617c_mdio_write_c45;
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));

	return devm_of_mdiobus_register(dev, bus, dev->of_node);
}

static const struct rtl9617c_mdio_data rtl9617c_mdio_data = {
	.gphy_mask = GENMASK(4, 1),
};

static const struct of_device_id rtl9617c_mdio_of_match[] = {
	{ .compatible = "realtek,rtl9617c-mdio", .data = &rtl9617c_mdio_data },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9617c_mdio_of_match);

static struct platform_driver rtl9617c_mdio_driver = {
	.probe = rtl9617c_mdio_probe,
	.driver = {
		.name = "rtl9617c-mdio",
		.of_match_table = rtl9617c_mdio_of_match,
	},
};
module_platform_driver(rtl9617c_mdio_driver);

MODULE_DESCRIPTION("Realtek RTL9617C MDIO bus driver");
MODULE_LICENSE("GPL");
