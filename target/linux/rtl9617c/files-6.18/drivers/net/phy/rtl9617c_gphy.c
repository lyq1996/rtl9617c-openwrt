// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek RTL9617C / Taurus internal quad Gigabit Ethernet PHY driver
 *
 * Applies chip-revision-specific firmware patch tables, warm-repatch reset
 * handling, BMCR power-up, page 0x0a43 reg24 bit7 clear, optional eFuse
 * nvmem calibration, and internal C22-paged EEE MMD support while preserving
 * Devicetree LED controller settings (page 0x0d04 left untouched).
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mdio.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/unaligned.h>

#include "rtl9617c_gphy_patches.h"

#define RTL9617C_GPHY_PHY_ID		0x001ccac4U

#define RTL9617C_PAGE_SELECT		0x1f
#define RTL9617C_SRAM_ADDR		0x1b
#define RTL9617C_SRAM_DATA		0x1c

#define RTL9617C_GLOBAL_JTAG_ID_PHYS	0xf4320000ULL
#define RTL9617C_WRAP_GPHY_PHYS		0xf4700000ULL
#define RTL9617C_WRAP_GPHY_CTRL13_OFF	0x34
#define RTL9617C_WRAP_PATCH_DONE	BIT(12)

#define RTL9617C_VND2_PCS_EEE_ABLE	0xa5c4
#define RTL9617C_VND2_AN_EEE_ADV	0xa5d0
#define RTL9617C_VND2_AN_EEE_LPABLE	0xa5d2

#define RTL9617C_EFUSE_PARAM_SW_PATCH	BIT(0)
#define RTL9617C_EFUSE_PARAM_RC_L	BIT(16)
#define RTL9617C_EFUSE_PARAM_RC_S	BIT(17)
#define RTL9617C_EFUSE_PARAM_R		BIT(18)
#define RTL9617C_EFUSE_PARAM_AMP	BIT(19)
#define RTL9617C_EFUSE_PARAM_500M	BIT(20)

struct rtl9617c_gphy_efuse_port {
	u16 rc_s;
	u16 amp;
	u16 rc_l;
	u16 r;
};

struct rtl9617c_gphy_efuse {
	bool valid;
	u32 param;
	struct rtl9617c_gphy_efuse_port ports[4];
	u16 r_cal_msb;
};

static DEFINE_MUTEX(rtl9617c_gphy_patch_lock);
static bool rtl9617c_gphy_patched;
static bool rtl9617c_gphy_efuse_loaded;
static struct rtl9617c_gphy_efuse rtl9617c_gphy_efuse_data;

static int rtl9617c_gphy_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, RTL9617C_PAGE_SELECT);
}

static int rtl9617c_gphy_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, RTL9617C_PAGE_SELECT, page);
}

static int rtl9617c_gphy_read_vnd2_unlocked(struct phy_device *phydev, u16 reg)
{
	int oldpage, ret;

	oldpage = __phy_read(phydev, RTL9617C_PAGE_SELECT);
	if (oldpage < 0)
		return oldpage;

	ret = __phy_write(phydev, RTL9617C_PAGE_SELECT, reg >> 4);
	if (ret < 0)
		goto restore;

	ret = __phy_read(phydev, 16 + ((reg & 0xf) >> 1));
restore:
	__phy_write(phydev, RTL9617C_PAGE_SELECT, oldpage);
	return ret;
}

static int rtl9617c_gphy_write_vnd2_unlocked(struct phy_device *phydev, u16 reg,
					     u16 val)
{
	int oldpage, ret;

	oldpage = __phy_read(phydev, RTL9617C_PAGE_SELECT);
	if (oldpage < 0)
		return oldpage;

	ret = __phy_write(phydev, RTL9617C_PAGE_SELECT, reg >> 4);
	if (ret < 0)
		goto restore;

	ret = __phy_write(phydev, 16 + ((reg & 0xf) >> 1), val);
restore:
	__phy_write(phydev, RTL9617C_PAGE_SELECT, oldpage);
	return ret;
}

static int rtl9617c_gphy_map_vnd2_mmd(int devnum, u16 regnum, u16 *vnd2_reg)
{
	if (devnum == MDIO_MMD_PCS && regnum == MDIO_PCS_EEE_ABLE)
		*vnd2_reg = RTL9617C_VND2_PCS_EEE_ABLE;
	else if (devnum == MDIO_MMD_AN && regnum == MDIO_AN_EEE_ADV)
		*vnd2_reg = RTL9617C_VND2_AN_EEE_ADV;
	else if (devnum == MDIO_MMD_AN && regnum == MDIO_AN_EEE_LPABLE)
		*vnd2_reg = RTL9617C_VND2_AN_EEE_LPABLE;
	else
		return -EOPNOTSUPP;

	return 0;
}

static int rtl9617c_gphy_read_mmd(struct phy_device *phydev, int devnum,
				   u16 regnum)
{
	u16 vnd2_reg;

	if (rtl9617c_gphy_map_vnd2_mmd(devnum, regnum, &vnd2_reg))
		return -EOPNOTSUPP;

	return rtl9617c_gphy_read_vnd2_unlocked(phydev, vnd2_reg);
}

static int rtl9617c_gphy_write_mmd(struct phy_device *phydev, int devnum,
				    u16 regnum, u16 val)
{
	u16 vnd2_reg;

	if (rtl9617c_gphy_map_vnd2_mmd(devnum, regnum, &vnd2_reg))
		return -EOPNOTSUPP;

	if (devnum == MDIO_MMD_AN && regnum == MDIO_AN_EEE_LPABLE)
		return -EOPNOTSUPP;

	return rtl9617c_gphy_write_vnd2_unlocked(phydev, vnd2_reg, val);
}

static void rtl9617c_gphy_discover_addrs(struct phy_device *phydev,
					 u8 addrs[4])
{
	struct mii_bus *bus = phydev->mdio.bus;
	unsigned int n = 0;
	int addr;

	for (addr = 0; addr < PHY_MAX_ADDR && n < 4; addr++) {
		struct phy_device *p = mdiobus_get_phy(bus, addr);

		if (p && p->phy_id == RTL9617C_GPHY_PHY_ID)
			addrs[n++] = addr;
	}

	if (n < 4) {
		u8 base = (phydev->mdio.addr >= 1 && phydev->mdio.addr <= 4) ?
			  1 : (phydev->mdio.addr & ~0x3U);

		for (n = 0; n < 4; n++)
			addrs[n] = base + n;
	}
}

static int rtl9617c_gphy_addr_to_slot(struct phy_device *phydev,
				       const u8 addrs[4])
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		if (addrs[i] == phydev->mdio.addr)
			return i;
	}

	if (phydev->mdio.addr >= 1 && phydev->mdio.addr <= 4)
		return phydev->mdio.addr - 1;

	return phydev->mdio.addr & 0x3U;
}

static void rtl9617c_gphy_bus_write_paged(struct mii_bus *bus, u8 addr,
					  u16 page, u16 reg, u16 val)
{
	mutex_lock(&bus->mdio_lock);
	__mdiobus_write(bus, addr, RTL9617C_PAGE_SELECT, page);
	usleep_range(1000, 1200);
	__mdiobus_write(bus, addr, reg, val);
	mutex_unlock(&bus->mdio_lock);
}

static void rtl9617c_gphy_write_sram(struct phy_device *phydev, u16 addr,
				     u16 val)
{
	phy_lock_mdio_bus(phydev);
	__phy_write(phydev, RTL9617C_PAGE_SELECT, 0x0a43);
	__phy_write(phydev, RTL9617C_SRAM_ADDR, addr);
	__phy_write(phydev, RTL9617C_SRAM_DATA, val);
	__phy_write(phydev, RTL9617C_PAGE_SELECT, 0);
	phy_unlock_mdio_bus(phydev);
}

static void *rtl9617c_read_nvmem_pair(struct device_node *start,
				      const char *primary,
				      const char *fallback, size_t *len)
{
	struct device_node *np = of_node_get(start);
	int depth;

	for (depth = 0; np && depth < 4; depth++) {
		struct nvmem_cell *cell;
		void *buf;

		cell = of_nvmem_cell_get(np, primary);
		if (PTR_ERR(cell) == -EPROBE_DEFER) {
			of_node_put(np);
			return ERR_PTR(-EPROBE_DEFER);
		}
		if (IS_ERR(cell) && fallback)
			cell = of_nvmem_cell_get(np, fallback);
		if (PTR_ERR(cell) == -EPROBE_DEFER) {
			of_node_put(np);
			return ERR_PTR(-EPROBE_DEFER);
		}
		if (!IS_ERR(cell)) {
			buf = nvmem_cell_read(cell, len);
			nvmem_cell_put(cell);
			of_node_put(np);
			if (!IS_ERR(buf))
				return buf;
			return NULL;
		}
		np = of_get_next_parent(np);
	}
	of_node_put(np);

	return NULL;
}

static int rtl9617c_load_efuse_once(struct phy_device *phydev)
{
	struct device_node *np = phydev->mdio.dev.of_node;
	typeof(rtl9617c_gphy_efuse_data) tmp = {};
	void *buf;
	size_t len = 0;
	unsigned int i;

	if (rtl9617c_gphy_efuse_loaded || !np)
		return 0;

	buf = rtl9617c_read_nvmem_pair(np, "param", "phy_param", &len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (!buf) {
		rtl9617c_gphy_efuse_loaded = true;
		return 0;
	}
	if (len >= 4) {
		tmp.param = get_unaligned_le32(buf);
		tmp.valid = true;
	}
	kfree(buf);

	if (!tmp.valid) {
		rtl9617c_gphy_efuse_loaded = true;
		return 0;
	}

	buf = rtl9617c_read_nvmem_pair(np, "calibration", "calib", &len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	if (buf) {
		if (len >= 32) {
			const u8 *p = buf;

			for (i = 0; i < 4; i++, p += 8) {
				tmp.ports[i].rc_s = get_unaligned_le16(p);
				tmp.ports[i].amp = get_unaligned_le16(p + 2);
				tmp.ports[i].rc_l = get_unaligned_le16(p + 4);
				tmp.ports[i].r = get_unaligned_le16(p + 6);
			}
		} else {
			tmp.valid = false;
		}
		kfree(buf);
	} else {
		tmp.valid = false;
	}

	if (!tmp.valid) {
		rtl9617c_gphy_efuse_loaded = true;
		return 0;
	}

	buf = rtl9617c_read_nvmem_pair(np, "phy_patch", "patch", &len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	if (buf) {
		if (len >= 16)
			tmp.r_cal_msb =
				get_unaligned_le16((const u8 *)buf + 14);
		kfree(buf);
	}

	rtl9617c_gphy_efuse_data = tmp;
	rtl9617c_gphy_efuse_loaded = true;

	phydev_info(phydev, "GPHY OTP calibration enabled (param=0x%08x)\n",
		    rtl9617c_gphy_efuse_data.param);
	return 0;
}

static bool rtl9617c_is_rev_a(void)
{
	void __iomem *jtag;
	u32 id = 0;

	jtag = ioremap(RTL9617C_GLOBAL_JTAG_ID_PHYS, 4);
	if (!jtag)
		return false;

	id = readl(jtag);
	iounmap(jtag);

	return ((id >> 28) & 0xfU) == 0U;
}

static void rtl9617c_gphy_apply_global_patch(struct phy_device *phydev,
					     const u8 addrs[4])
{
	const struct rtl9617c_gphy_patch_entry *tbl;
	struct mii_bus *bus = phydev->mdio.bus;
	void __iomem *wrap;
	unsigned int len, i;
	bool rev_a;
	u32 wrap13;

	if (rtl9617c_gphy_patched)
		return;

	wrap = ioremap(RTL9617C_WRAP_GPHY_PHYS, 0x40);
	if (!wrap)
		return;

	wrap13 = readl(wrap + RTL9617C_WRAP_GPHY_CTRL13_OFF);
	if (wrap13 & RTL9617C_WRAP_PATCH_DONE) {
		for (i = 0; i < 4; i++) {
			rtl9617c_gphy_bus_write_paged(bus, addrs[i],
						      0x0a40, 16, 0x1940);
			rtl9617c_gphy_bus_write_paged(bus, addrs[i],
						      0x0c41, 20, 0x0001);
			if (i < 3)
				usleep_range(1000, 1200);
			else
				msleep(10);
		}
	}

	rev_a = rtl9617c_is_rev_a();
	tbl = rev_a ? rtl9617c_gphy_patch_rev_a : rtl9617c_gphy_patch_rev_b;
	len = rev_a ? ARRAY_SIZE(rtl9617c_gphy_patch_rev_a) :
		      ARRAY_SIZE(rtl9617c_gphy_patch_rev_b);

	for (i = 0; i < len; i++) {
		unsigned int idx = clamp_t(unsigned int, tbl[i].port, 1U, 4U) - 1U;

		rtl9617c_gphy_bus_write_paged(bus, addrs[idx], tbl[i].page,
					      tbl[i].reg, tbl[i].val);
		usleep_range(1000, 1200);
	}
	for (i = 0; i < 4; i++)
		mdiobus_write(bus, addrs[i], RTL9617C_PAGE_SELECT, 0);

	writel(wrap13 | RTL9617C_WRAP_PATCH_DONE,
	       wrap + RTL9617C_WRAP_GPHY_CTRL13_OFF);
	iounmap(wrap);

	rtl9617c_gphy_patched = true;
	phydev_info(phydev,
		    "RTL9617C quad GPHY patched (%s, %u entries)\n",
		    rev_a ? "revA" : "revB", len);
}

static void rtl9617c_gphy_apply_efuse(struct phy_device *phydev, int slot)
{
	const struct rtl9617c_gphy_efuse *ef = &rtl9617c_gphy_efuse_data;
	const struct rtl9617c_gphy_efuse_port *pe;
	u32 param = ef->param;

	if (!ef->valid || (param & RTL9617C_EFUSE_PARAM_SW_PATCH) ||
	    slot < 0 || slot >= 4)
		return;

	pe = &ef->ports[slot];

	if (param & RTL9617C_EFUSE_PARAM_RC_L) {
		phy_write_paged(phydev, 0x0bcd, 22, pe->rc_l);
		phy_write_paged(phydev, 0x0bcd, 23, pe->rc_l);
		rtl9617c_gphy_write_sram(phydev, 0x8330, pe->rc_l);
	}

	if (param & RTL9617C_EFUSE_PARAM_RC_S)
		rtl9617c_gphy_write_sram(phydev, 0x8332, pe->rc_s);

	if (param & RTL9617C_EFUSE_PARAM_R) {
		u16 r = pe->r;
		u16 msb = ef->r_cal_msb >> (slot * 4);
		u16 r0 = (r & 0xfU) | (((msb >> 0) & 0x1U) << 4);
		u16 r1 = ((r >> 4) & 0xfU) | (((msb >> 1) & 0x1U) << 4);
		u16 r2 = ((r >> 8) & 0xfU) | (((msb >> 2) & 0x1U) << 4);
		u16 r3 = ((r >> 12) & 0xfU) | (((msb >> 3) & 0x1U) << 4);
		u16 v0 = (r1 << 8) | r0;
		u16 v1 = (r3 << 8) | r2;

		phy_write_paged(phydev, 0x0bcf, 21, v0);
		phy_write_paged(phydev, 0x0bcf, 20, v1);
		phy_write_paged(phydev, 0x0bcf, 19, v0);
		phy_write_paged(phydev, 0x0bcf, 18, v1);
	}

	if (param & RTL9617C_EFUSE_PARAM_AMP)
		phy_write_paged(phydev, 0x0bca, 22, pe->amp);

	phy_modify_paged(phydev, 0x0a4a, 17, BIT(8),
			 (param & RTL9617C_EFUSE_PARAM_500M) ? 0 : BIT(8));
}

static int rtl9617c_gphy_config_init(struct phy_device *phydev)
{
	u8 addrs[4] = { };
	int ret;
	int slot;

	mutex_lock(&rtl9617c_gphy_patch_lock);
	rtl9617c_gphy_discover_addrs(phydev, addrs);
	slot = rtl9617c_gphy_addr_to_slot(phydev, addrs);
	rtl9617c_gphy_apply_global_patch(phydev, addrs);
	ret = rtl9617c_load_efuse_once(phydev);
	mutex_unlock(&rtl9617c_gphy_patch_lock);
	if (ret)
		return ret;

	/* Ensure BMCR power-down is cleared and leave page 0x0d04 DT LEDs alone */
	phy_modify(phydev, MII_BMCR, BMCR_PDOWN, 0);
	usleep_range(1000, 1200);

	/* Clear page 0x0a43 reg 24 bit 7 per SDK internal GPHY init */
	phy_modify_paged(phydev, 0x0a43, 24, BIT(7), 0);

	rtl9617c_gphy_apply_efuse(phydev, slot);
	phy_write(phydev, RTL9617C_PAGE_SELECT, 0);

	return 0;
}

static int rtl9617c_gphy_set_loopback(struct phy_device *phydev, bool enable,
				      int speed)
{
	if (enable) {
		if (!speed && phydev->speed == SPEED_UNKNOWN)
			speed = SPEED_1000;
		if (phydev->duplex == DUPLEX_UNKNOWN)
			phydev->duplex = DUPLEX_FULL;
	}

	return genphy_loopback(phydev, enable, speed);
}

static struct phy_driver rtl9617c_gphy_drvs[] = {
	{
		PHY_ID_MATCH_EXACT(RTL9617C_GPHY_PHY_ID),
		.name		= "Realtek RTL9617C Quad GPHY",
		.config_init	= rtl9617c_gphy_config_init,
		.config_aneg	= genphy_config_aneg,
		.read_status	= genphy_read_status,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.set_loopback	= rtl9617c_gphy_set_loopback,
		.read_page	= rtl9617c_gphy_read_page,
		.write_page	= rtl9617c_gphy_write_page,
		.read_mmd	= rtl9617c_gphy_read_mmd,
		.write_mmd	= rtl9617c_gphy_write_mmd,
	},
};

module_phy_driver(rtl9617c_gphy_drvs);

static const struct mdio_device_id __maybe_unused rtl9617c_gphy_tbl[] = {
	{ PHY_ID_MATCH_EXACT(RTL9617C_GPHY_PHY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(mdio, rtl9617c_gphy_tbl);

MODULE_DESCRIPTION("Realtek RTL9617C internal quad Gigabit PHY driver");
MODULE_LICENSE("GPL");
