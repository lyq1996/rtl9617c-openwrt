// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) Cortina-Access Limited 2015. All rights reserved.
 * Native mii_bus port of the Taurus MDIO/OCP access in aal_phy.c,
 * GPL-for-GP3000 5090fbcb6ba743dd7b1314811ef557bad0460147.
 * The C22 and internal OCP sequences also match stock ca-ne.ko disassembly.
 * External PHY reset is described by the board's standard MDIO reset binding.
 * No PHY microcode, calibration, NI/DMA or optical initialization.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>

/* MDC prescaler word. The bootloader value is left untouched: U-Boot on this
 * board never programs it and its internal-PHY reads work regardless, and the
 * value is printed by rtl_mdio_detect() so a wrong divider is visible. */
#define MDIO_CFG	0x00
#define MDIO_ADDR	0x04
#define MDIO_PHY	GENMASK(4, 0)
#define MDIO_REG	GENMASK(12, 8)
#define MDIO_READ	BIT(15)
#define MDIO_C45	BIT(16)
#define MDIO_OP		GENMASK(19, 18)
#define MDIO_WRDATA	0x08
#define MDIO_RDDATA	0x0c
#define MDIO_CTRL	0x10
#define MDIO_DONE	BIT(0)
#define MDIO_START	BIT(7)
/* GPHY wrapper register block (reg "gphy-wrap"); read only for the log. */
#define GPHY_CTRL12	0x30
#define GPHY_CTRL13	0x34

struct rtl9617c_mdio {
	struct mii_bus *bus;
	void __iomem *per;
	void __iomem *ocp;
	void __iomem *wrap;
	/* Mapped length of the gphy-ocp window, used to bound vendor accesses. */
	resource_size_t ocp_size;
	/* Addresses served by the internal GPHY register window. */
	u32 ocp_mask;
	/* realtek,external-phys: when both paths answer, prefer the pins. */
	bool prefer_c22;
	/* Result of the last phyreg command, surfaced through sysfs. */
	u32 last;
};

static bool rtl_mdio_internal(struct rtl9617c_mdio *mdio, int phy)
{
	return mdio->ocp_mask & BIT(phy);
}

/*
 * Internal GPHY register window. Taurus maps it linearly: the NI_MDIO_OPER
 * word is reg_base<<12 | phy_addr<<7 | reg_off<<2 with reg_base 0xf4600 for
 * this SoC (taurus_ni.h), so with the window ioremapped the byte offset is
 * just phy*128 + reg*4. Verified against U-Boot's taurus_ni.c, which reads
 * 0x001ccac4 from PHY 1..4 on this board.
 */
static u32 rtl_mdio_ocp_offset(int phy, int reg)
{
	return (phy << 7) | (reg << 2);
}

static int rtl_mdio_xfer(struct rtl9617c_mdio *mdio, u32 address, u16 value,
			 bool read)
{
	u32 status;
	int ret;

	/* DONE is W1C. Do not accept completion left by an earlier command. */
	writel(MDIO_DONE, mdio->per + MDIO_CTRL);
	writel(address, mdio->per + MDIO_ADDR);
	if (!read)
		writel(value, mdio->per + MDIO_WRDATA);
	writel(MDIO_START, mdio->per + MDIO_CTRL);
	/* Stock ca-ne uses 50000 polls separated by ca_udelay(1). */
	ret = readl_poll_timeout(mdio->per + MDIO_CTRL, status,
				status & MDIO_DONE, 1, 50000);
	if (ret)
		return ret;
	value = read ? readl(mdio->per + MDIO_RDDATA) & 0xffff : 0;
	writel(MDIO_DONE, mdio->per + MDIO_CTRL);
	return value;
}

static int rtl_mdio_c22(struct rtl9617c_mdio *mdio, int phy, int reg, u16 value,
			bool read)
{
	/* Clause 22 leaves the operation field at zero; only the direction bit
	 * and the START strobe matter (U-Boot __ca_mdio_c22_read/write). */
	return rtl_mdio_xfer(mdio, FIELD_PREP(MDIO_PHY, phy) |
		FIELD_PREP(MDIO_REG, reg) | (read ? MDIO_READ : 0), value, read);
}

/* A real part never reports an all-zero or all-one identifier half; a dead
 * bus reads back as either. */
static bool rtl_mdio_id_valid(u32 id)
{
	u32 id1 = id >> 16;

	return id1 != 0 && id1 != 0xffff;
}

static u32 rtl_mdio_c22_id(struct rtl9617c_mdio *mdio, int phy, int *err)
{
	int hi = rtl_mdio_c22(mdio, phy, MII_PHYSID1, 0, true);
	int lo = rtl_mdio_c22(mdio, phy, MII_PHYSID2, 0, true);

	*err = hi < 0 ? hi : lo < 0 ? lo : 0;
	return (hi < 0 || lo < 0) ? 0 : ((u32)hi << 16) | lo;
}

static u32 rtl_mdio_ocp_id(struct rtl9617c_mdio *mdio, int phy)
{
	u32 hi = readl(mdio->ocp + rtl_mdio_ocp_offset(phy, MII_PHYSID1));
	u32 lo = readl(mdio->ocp + rtl_mdio_ocp_offset(phy, MII_PHYSID2));

	return ((hi & 0xffff) << 16) | (lo & 0xffff);
}

static int rtl_mdio_read(struct mii_bus *bus, int phy, int reg)
{
	struct rtl9617c_mdio *mdio = bus->priv;

	if (phy < 0 || phy > 31 || reg < 0 || reg > 31)
		return -EINVAL;
	if (rtl_mdio_internal(mdio, phy))
		return readl(mdio->ocp + rtl_mdio_ocp_offset(phy, reg)) & 0xffff;
	return rtl_mdio_c22(mdio, phy, reg, 0, true);
}

static int rtl_mdio_write(struct mii_bus *bus, int phy, int reg, u16 value)
{
	struct rtl9617c_mdio *mdio = bus->priv;

	if (phy < 0 || phy > 31 || reg < 0 || reg > 31)
		return -EINVAL;
	if (rtl_mdio_internal(mdio, phy)) {
		writel(value, mdio->ocp + rtl_mdio_ocp_offset(phy, reg));
		/* Keep the documented Taurus OCP write settling interval. */
		usleep_range(1000, 2000);
		return 0;
	}
	return rtl_mdio_c22(mdio, phy, reg, value, false);
}

static int rtl_mdio_c45_xfer(struct rtl9617c_mdio *mdio, int phy, int devad, int reg,
			     u16 value, bool read)
{
	u32 address;
	int ret;

	if (phy < 0 || phy > 31 || devad < 0 || devad > 31 || reg < 0 || reg > 0xffff)
		return -EINVAL;
	if (rtl_mdio_internal(mdio, phy))
		return -EOPNOTSUPP;
	address = FIELD_PREP(MDIO_PHY, phy) | FIELD_PREP(MDIO_REG, devad) | MDIO_C45;
	/* mii_bus core serializes both phases under its mdio_lock. */
	ret = rtl_mdio_xfer(mdio, address, reg, false);
	if (ret)
		return ret;
	address |= FIELD_PREP(MDIO_OP, read ? 3 : 1) | (read ? MDIO_READ : 0);
	return rtl_mdio_xfer(mdio, address, value, read);
}

static int rtl_mdio_read_c45(struct mii_bus *bus, int phy, int devad, int reg)
{
	return rtl_mdio_c45_xfer(bus->priv, phy, devad, reg, 0, true);
}

/*
 * Indirect MMD access over Clause 22, registers 0x0d/0x0e. The caller must
 * hold bus->mdio_lock across all four frames, not just each individual
 * frame: phylib also changes the address/data selector on the same PHY.
 *
 * Retain the existing 50 us gaps for now so locking is the only transport
 * change under test. Earlier unlocked observations do not establish that
 * a delay is required, or that it fixes any MMD30 access problem.
 */
static int rtl_mdio_mmd(struct rtl9617c_mdio *mdio, int phy, int devad, int reg,
			u16 value, bool read)
{
	int ret;

	ret = rtl_mdio_c22(mdio, phy, MII_MMD_CTRL, devad, false);
	if (ret < 0)
		return ret;
	udelay(50);
	ret = rtl_mdio_c22(mdio, phy, MII_MMD_DATA, reg, false);
	if (ret < 0)
		return ret;
	udelay(50);
	ret = rtl_mdio_c22(mdio, phy, MII_MMD_CTRL, 0x4000 | devad, false);
	if (ret < 0)
		return ret;
	udelay(50);
	return rtl_mdio_c22(mdio, phy, MII_MMD_DATA, value, read);
}

/*
 * Vendor GPHY register window, as OEM ca-ne.ko addresses it in
 * aal_mdio_read/aal_mdio_write (ca-ne.ko+0x15b5c / +0x16784):
 *
 *	addr = gphy_base + ((port - 1) << 18) + 0x29000 + devad * 8
 *
 * This window only covers the SoC's own GPHYs (port 1..4). An external part
 * such as the 2.5G RTL8221B (port 6) does NOT live here: OEM reaches it
 * through aal_mdio_read_indirect, which builds the ordinary Clause 45 word
 * (PRTAD = port, DEVAD = devad) and is therefore the same path this driver
 * already uses. Addressing port 6 through this window computes
 * 5<<18 + 0x29000 = 0x169000, past the end of the 1 MiB mapping, and an
 * access there faults the bus -- measured on this board as an immediate
 * reset. Bound the offset against the mapped size so a mistyped command
 * cannot take the machine down.
 */
static void __iomem *rtl_mdio_gphy(struct rtl9617c_mdio *mdio, int port,
				   int devad)
{
	u32 offset;

	if (!mdio->ocp || port < 1 || port > 4 || devad < 0 || devad > 31)
		return NULL;
	offset = ((u32)(port - 1) << 18) + 0x29000 + (u32)devad * 8;
	if (offset + sizeof(u32) > mdio->ocp_size)
		return NULL;
	return mdio->ocp + offset;
}

/*
 * Board bring-up aid: /sys/bus/platform/devices/<mdio>/phyreg
 *
 *   w  <phy> <reg> <value>            Clause 22 write
 *   r  <phy> <reg>                    Clause 22 read
 *   mw <phy> <devad> <reg> <value>    indirect MMD write
 *   mr <phy> <devad> <reg>            indirect MMD read
 *   cr <phy> <devad> <reg>            direct Clause 45 read
 *   gw <port> <devad> <reg> <value>   vendor GPHY window write
 *   gr <port> <devad> <reg>           vendor GPHY window read
 *
 * The result of the last command is printed when the file is read.
 */
static ssize_t phyreg_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct rtl9617c_mdio *mdio = dev_get_drvdata(dev);

	return sysfs_emit(buf,
			  "last=%08x (r/w p r [v], mr/mw p d r [v], gw/gr port d r [v])\n",
			  mdio->last);
}

static ssize_t phyreg_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct rtl9617c_mdio *mdio = dev_get_drvdata(dev);
	unsigned long phy = 0, first = 0, second = 0, third = 0;
	char op[8] = "";
	int ret, n;

	n = sscanf(buf, "%7s %li %li %li %li", op, &phy, &first, &second,
		   &third);
	/* r/w take a register and an optional value; mr/mw/cr take a device
	 * address and a register; gr/gw take a GPHY port, devad and register.
	 * Only the first two operands are bounded for every command, so the
	 * devad/register limits are checked per branch below. */
	if (n < 3 || phy > 31 || first > 255) {
		mdio->last = 0xdead0000;
		return count;
	}
	/* MMD device addresses and registers are 5 and 16 bits wide; the GPHY
	 * window has its own (much wider) layout and is not bounded here. */
	if (strcmp(op, "gr") && strcmp(op, "gw") && first > 31) {
		mdio->last = 0xdead0000;
		return count;
	}

	/* phy_read_mmd() holds this lock around its complete C22/C45 operation.
	 * phyreg must do the same; otherwise its four-frame MMD sequence can be
	 * interleaved with the PHY state machine and the page selector/data frame
	 * no longer refer to the same register. */
	mutex_lock(&mdio->bus->mdio_lock);
	if (!strcmp(op, "r")) {
		ret = rtl_mdio_c22(mdio, phy, first, 0, true);
	} else if (!strcmp(op, "w") && n >= 4 && second <= 0xffff) {
		ret = rtl_mdio_c22(mdio, phy, first, second, false);
	} else if (!strcmp(op, "mr") && n >= 4 && second <= 0xffff) {
		ret = rtl_mdio_mmd(mdio, phy, first, second, 0, true);
	} else if (!strcmp(op, "cr") && n >= 4 && second <= 0xffff) {
		ret = rtl_mdio_c45_xfer(mdio, phy, first, second, 0, true);
	} else if (!strcmp(op, "mw") && n >= 5 && second <= 0xffff &&
		   third <= 0xffff) {
		ret = rtl_mdio_mmd(mdio, phy, first, second, third, false);
	} else if (!strcmp(op, "gr") && n >= 4 && second <= 0xffff) {
		void __iomem *p = rtl_mdio_gphy(mdio, phy, first);

		ret = p ? (int)(readl(p) & 0xffff) : -ENODEV;
	} else if (!strcmp(op, "gw") && n >= 5 && second <= 0xffff &&
		   third <= 0xffff) {
		void __iomem *p = rtl_mdio_gphy(mdio, phy, first);

		if (p)
			writel(third, p);
		ret = p ? 0 : -ENODEV;
	} else {
		mutex_unlock(&mdio->bus->mdio_lock);
		mdio->last = 0xdead0001;
		return count;
	}
	mutex_unlock(&mdio->bus->mdio_lock);

	mdio->last = ret < 0 ? (0xbad00000 | (u32)-ret) : (u32)ret;
	dev_info(dev, "phyreg %s: %08x\n", op, mdio->last);

	return count;
}
static DEVICE_ATTR_RW(phyreg);

static int rtl_mdio_write_c45(struct mii_bus *bus, int phy, int devad, int reg, u16 value)
{
	return rtl_mdio_c45_xfer(bus->priv, phy, devad, reg, value, false);
}

/*
 * Address 1..4 belong to the internal GPHYs on this SoC (U-Boot reads
 * 0x001ccac4 from all four through the register window), and everything from 5
 * up is an external part on the MDIO pins. The identifier read decides anyway:
 * a board that wires external PHYs at 1..4 would answer on the pins instead.
 */
static void rtl_mdio_detect(struct device *dev, struct rtl9617c_mdio *mdio)
{
	bool use_c22, use_ocp;
	int phy, err, id1, id2;
	u32 c22, ocp;

	dev_info(dev, "MDIO_CFG=%08x GPHY_CTRL12=%08x GPHY_CTRL13=%08x\n",
		 readl(mdio->per + MDIO_CFG), readl(mdio->wrap + GPHY_CTRL12),
		 readl(mdio->wrap + GPHY_CTRL13));

	for (phy = 1; phy <= 4; phy++) {
		ocp = rtl_mdio_ocp_id(mdio, phy);
		use_ocp = rtl_mdio_id_valid(ocp) && !mdio->prefer_c22;
		use_c22 = false;
		if (!use_ocp) {
			c22 = rtl_mdio_c22_id(mdio, phy, &err);
			use_c22 = !err && rtl_mdio_id_valid(c22);
		} else {
			c22 = 0;
			err = 0;
		}
		if (use_ocp)
			mdio->ocp_mask |= BIT(phy);
		dev_info(dev, "phy%d: window=%08x%s%08x -> %s\n", phy, ocp,
			 use_c22 ? " pins=" : "", c22,
			 use_ocp ? "internal GPHY" : use_c22 ? "MDIO pins" : "absent");
	}

	for (phy = 5; phy <= 31; phy++) {
		c22 = rtl_mdio_c22_id(mdio, phy, &err);
		if (err || !rtl_mdio_id_valid(c22))
			continue;
		dev_info(dev, "phy%d: C22=%08x on the MDIO pins\n", phy, c22);
	}

	id1 = rtl_mdio_c45_xfer(mdio, 7, MDIO_MMD_PMAPMD, MII_PHYSID1, 0, true);
	id2 = rtl_mdio_c45_xfer(mdio, 7, MDIO_MMD_PMAPMD, MII_PHYSID2, 0, true);
	dev_info(dev, "phy7: C45 PMAPMD id1=%d id2=%d\n", id1, id2);
}

static int rtl_mdio_reset(struct mii_bus *bus)
{
	struct gpio_desc *enable;

	/* The core pulses GPIO128 first, then runs this callback before discovery.
	 * OEM rtl8221b drives GPIO14 high, releases ownership, and waits 300ms.
	 * The later FPGA loader is allowed to acquire GPIO14 as its RX input. */
	enable = gpiod_get_optional(bus->parent, "external-phy-enable", GPIOD_OUT_HIGH);
	if (IS_ERR(enable))
		return PTR_ERR(enable);
	if (enable) {
		gpiod_put(enable);
		msleep(300);
	}
	return 0;
}

static int rtl_mdio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl9617c_mdio *mdio;
	struct mii_bus *bus;
	int ret;

	bus = devm_mdiobus_alloc_size(dev, sizeof(*mdio));
	if (!bus)
		return -ENOMEM;
	mdio = bus->priv;
	mdio->bus = bus;
	mdio->per = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mdio->per))
		return PTR_ERR(mdio->per);
	mdio->ocp = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(mdio->ocp))
		return PTR_ERR(mdio->ocp);
	mdio->ocp_size = resource_size(platform_get_resource(pdev, IORESOURCE_MEM, 1));
	mdio->wrap = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(mdio->wrap))
		return PTR_ERR(mdio->wrap);
	mdio->prefer_c22 = device_property_read_bool(dev, "realtek,external-phys");
	/*
	 * The bootloader leaves MDIO_CFG (MDC prescaler) and the GPHY wrapper at
	 * values that work: U-Boot on this board reads all four internal PHYs
	 * without touching either, so neither is programmed here. The detect
	 * step prints both for the record.
	 */
	bus->name = "RTL9617C MDIO/OCP";
	bus->parent = dev;
	bus->reset = rtl_mdio_reset;
	bus->read = rtl_mdio_read;
	bus->write = rtl_mdio_write;
	bus->read_c45 = rtl_mdio_read_c45;
	bus->write_c45 = rtl_mdio_write_c45;
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	rtl_mdio_detect(dev, mdio);
	dev_info(dev, "MDIO bus ready, internal OCP addresses mask %08x\n",
		 mdio->ocp_mask);
	platform_set_drvdata(pdev, mdio);
	ret = devm_of_mdiobus_register(dev, bus, dev->of_node);
	if (ret)
		return ret;
	device_create_file(dev, &dev_attr_phyreg);
	return 0;
}

static const struct of_device_id rtl_mdio_match[] = {
	{ .compatible = "realtek,rtl9617c-mdio" }, { }
};
MODULE_DEVICE_TABLE(of, rtl_mdio_match);

static struct platform_driver rtl_mdio_driver = {
	.probe = rtl_mdio_probe,
	.driver = { .name = "rtl9617c-mdio", .of_match_table = rtl_mdio_match },
};
module_platform_driver(rtl_mdio_driver);
MODULE_DESCRIPTION("Native Taurus MDIO and internal copper PHY OCP access");
MODULE_LICENSE("GPL");
