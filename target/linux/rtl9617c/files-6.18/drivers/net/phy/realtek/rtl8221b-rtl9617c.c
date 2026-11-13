/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/phy.h>

#include "../phylib-internal.h"
#include "rtl8221b-rtl9617c.h"

int rtl822x_c45_get_features(struct phy_device *phydev);
int rtl822x_c45_config_aneg(struct phy_device *phydev);
int rtlgen_c45_resume(struct phy_device *phydev);

/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "../phylib-internal.h"

struct rtl8221b_reg {
	u16 reg, value;
};

#include "rtl8221b-rtl9617c-patch.h"

int rtl8221b_read_mmd(struct phy_device *phydev, int devnum, u16 reg)
{
	return mmd_phy_read(phydev->mdio.bus, phydev->mdio.addr,
			    phydev->is_c45, devnum, reg);
}

int rtl8221b_write_mmd(struct phy_device *phydev, int devnum, u16 reg,
			      u16 val)
{
	return mmd_phy_write(phydev->mdio.bus, phydev->mdio.addr,
			     phydev->is_c45, devnum, reg, val);
}

static int rtl8221b_write_regs(struct phy_device *phydev,
			       const struct rtl8221b_reg *regs, unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, regs[i].reg, regs[i].value);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int rtl8221b_update(struct phy_device *phydev, int mmd, u16 reg,
			  u16 mask, u16 value)
{
	int old = phy_read_mmd(phydev, mmd, reg);

	if (old < 0)
		return old;
	return phy_write_mmd(phydev, mmd, reg, (old & ~mask) | value);
}

static int rtl8221b_wait_ready(struct phy_device *phydev)
{
	int val;

	
	return phy_read_mmd_poll_timeout(phydev, MDIO_MMD_VEND2, 0xa420,
					val, (val & 3) == 3, 2000, 1500000, false);
}

struct rtl8221b_cfg {
	bool hsgmii, hsgmii_init_off, no_vendor_finish, oem_sds_option;
	bool vendor_patch, serdes_clock_enable, vendor_mmd31, vendor_window, vendor_a400;
};

static struct device_node *rtl8221b_cfg_node;
static struct rtl8221b_cfg rtl8221b_cfg_cache;

static struct rtl8221b_cfg *rtl8221b_cfg(struct phy_device *phydev)
{
	struct device_node *np = phydev->mdio.dev.of_node;
	struct rtl8221b_cfg *cfg = &rtl8221b_cfg_cache;

	if (rtl8221b_cfg_node == np)
		return cfg;
	cfg->hsgmii = of_property_read_bool(np, "realtek,hsgmii");
	cfg->hsgmii_init_off = of_property_read_bool(np, "realtek,hsgmii-init-off");
	cfg->no_vendor_finish = of_property_read_bool(np, "realtek,no-vendor-finish");
	cfg->oem_sds_option = of_property_read_bool(np, "realtek,oem-sds-option");
	cfg->vendor_patch = of_property_read_bool(np, "realtek,vendor-patch");
	cfg->serdes_clock_enable = of_property_read_bool(np, "realtek,serdes-clock-enable");
	cfg->vendor_mmd31 = of_property_read_bool(np, "realtek,vendor-mmd31");
	cfg->vendor_window = of_property_read_bool(np, "realtek,vendor-window");
	cfg->vendor_a400 = of_property_read_bool(np, "realtek,vendor-a400");
	rtl8221b_cfg_node = np;
	return cfg;
}

static int rtl8221b_load_patch(struct phy_device *phydev)
{
	int ret, val, rev;

	rev = phy_read_mmd(phydev, MDIO_MMD_PCS, 0x0003);
	if (rev < 0)
		return rev;
	phydev_info(phydev,
		    "vendor patch: MMD3.0x0003=0x%04x (OEM gate wants 0x??48, %s)\n",
		    rev, (rev & 0xff) == 0x48 ? "match" : "mismatch - loading anyway");

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa436, 0x801e);
	if (ret < 0)
		return ret;
	val = phy_read_mmd(phydev, MDIO_MMD_VEND2, 0xa438);
	if (val < 0)
		return val;
	if (val == 0x20)
		return 0;

	ret = rtl8221b_update(phydev, MDIO_MMD_VEND2, 0xb820, 0, BIT(4));
	if (ret < 0)
		return ret;
	ret = phy_read_mmd_poll_timeout(phydev, MDIO_MMD_VEND2, 0xb800,
					val, val & BIT(6), 1000, 100000, false);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa436, 0x8024);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa438, 0x3701);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa436, 0xb82e);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa438, 1);
	if (ret < 0)
		return ret;
	ret = rtl8221b_update(phydev, MDIO_MMD_VEND2, 0xb820, 0, BIT(7));
	if (ret < 0)
		return ret;
	ret = rtl8221b_write_regs(phydev, rtl8221b_patch_ram, ARRAY_SIZE(rtl8221b_patch_ram));
	if (ret < 0)
		return ret;
	ret = rtl8221b_update(phydev, MDIO_MMD_VEND2, 0xb820, BIT(7), 0);
	if (ret < 0)
		return ret;
	ret = rtl8221b_write_regs(phydev, rtl8221b_patch_regs, ARRAY_SIZE(rtl8221b_patch_regs));
	if (ret < 0)
		return ret;
	ret = rtl8221b_update(phydev, MDIO_MMD_VEND2, 0xb896, BIT(0), 0);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xb892, 0);
	if (ret < 0)
		return ret;
	ret = rtl8221b_update(phydev, MDIO_MMD_VEND2, 0xb896, 0, BIT(0));
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa436, 0);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa438, 0);
	if (ret < 0)
		return ret;
	ret = rtl8221b_update(phydev, MDIO_MMD_VEND2, 0xb82e, BIT(0), 0);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa436, 0x8024);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa438, 0);
	if (ret < 0)
		return ret;
	ret = rtl8221b_update(phydev, MDIO_MMD_VEND2, 0xb820, BIT(4), 0);
	if (ret < 0)
		return ret;
	ret = phy_read_mmd_poll_timeout(phydev, MDIO_MMD_VEND2, 0xb800,
					val, !(val & BIT(6)), 1000, 100000, false);
	if (ret < 0)
		return ret;
	ret = rtl8221b_write_regs(phydev, rtl8221b_patch_analog, ARRAY_SIZE(rtl8221b_patch_analog));
	if (ret < 0)
		return ret;
	
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa436, 0x801e);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa438, 0x20);
	if (ret < 0)
		return ret;
	
	if (rtl8221b_cfg(phydev)->vendor_a400)
		return phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa400, 0x9200);
	return 0;
}

static int rtl8221b_serdes_clock_enable(struct phy_device *phydev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 32; i++) {
		ret = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x7581);
		if (ret >= 0 && ret != 0xdead) {
			if (ret & BIT(4)) {
				phydev_info(phydev,
					    "SerDes clock already enabled (0x7581=0x%04x)\n",
					    ret);
				return 0;
			}
			ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x7581,
					    ret | BIT(4));
			if (ret < 0)
				return ret;
			ret = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x7581);
			phydev_info(phydev,
				    "SerDes clock enable: wrote 0x7581 bit4, readback 0x%04x\n",
				    ret);
			return ret < 0 ? ret : 0;
		}
		usleep_range(1000, 2000);
	}
	phydev_warn(phydev, "SerDes clock: MMD30.0x7581 unreachable after 32 tries\n");
	return 0;
}

static const struct rtl8221b_reg rtl8221b_vendor_mmd31_regs[] = {
	{ 0xa46a, 0x0302 },
	{ 0xac46, 0xb794 },
	{ 0xa412, 0x0200 },
	{ 0xa5d4, 0x0081 },
	{ 0xad30, 0x0a57 },
	{ 0xad30, 0x0a55 },
	{ 0xb87c, 0x80f5 },
	{ 0xb87e, 0x760e },
	{ 0xb87c, 0x8107 },
	{ 0xb87e, 0x360e },
	{ 0xb87c, 0x8551 },
	{ 0xb87e, 0x080e },
	{ 0xbf00, 0xb202 },
	{ 0xbf46, 0x0300 },
	{ 0xa4ca, 0x6a50 },
	{ 0xd12a, 0x8000 },
	{ 0xd12c, 0x8000 },
	{ 0xd144, 0x8000 },
	{ 0xd146, 0x8000 },
	{ 0xbf84, 0xac00 },
};

static const struct rtl8221b_reg rtl8221b_vendor_window[] = {
	{ 0x8044, 0x240f }, { 0x804a, 0x240f }, { 0x8050, 0x240f },
	{ 0x8056, 0x240f }, { 0x805c, 0x240f }, { 0x8062, 0x240f },
	{ 0x8068, 0x240f }, { 0x806e, 0x240f }, { 0x8074, 0x240f },
	{ 0x807a, 0x240f },
	{ 0x8045, 0x1700 }, { 0x804b, 0x1700 }, { 0x8051, 0x1700 },
	{ 0x8057, 0x1700 }, { 0x805d, 0x1700 }, { 0x8063, 0x1700 },
	{ 0x8069, 0x1700 }, { 0x806f, 0x1700 }, { 0x8075, 0x1700 },
	{ 0x807b, 0x1700 },
	{ 0x8ff4, 0x0000 }, { 0x8ff6, 0x03e8 }, { 0x8ff8, 0x8000 },
	{ 0x8ffa, 0x8000 }, { 0x8ffc, 0x8000 }, { 0x8ffe, 0x8000 },
	{ 0x8015, 0xf9bf }, { 0x8170, 0xd8a0 },
};

static int rtl8221b_vendor_mmd31_apply(struct phy_device *phydev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(rtl8221b_vendor_mmd31_regs); i++) {
		ret = phy_write_mmd(phydev, MDIO_MMD_VEND2,
				    rtl8221b_vendor_mmd31_regs[i].reg,
				    rtl8221b_vendor_mmd31_regs[i].value);
		if (ret < 0)
			return ret;
	}
	
	if (rtl8221b_cfg(phydev)->vendor_window) {
		for (i = 0; i < ARRAY_SIZE(rtl8221b_vendor_window); i++) {
			ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa436,
					    rtl8221b_vendor_window[i].reg);
			if (ret < 0)
				return ret;
			ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa438,
					    rtl8221b_vendor_window[i].value);
			if (ret < 0)
				return ret;
		}
	}
	phydev_info(phydev, "vendor MMD31 sequence applied (%zu regs)\n",
		    ARRAY_SIZE(rtl8221b_vendor_mmd31_regs));
	return 0;
}

static int rtl8221b_hsgmii(struct phy_device *phydev)
{
	int ret, val;

	
	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2, 0xa468);
	if (ret < 0)
		return ret;
	if (!(ret & 0x0002)) {
		ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa468, ret | 0x0002);
		if (ret < 0)
			return ret;
	}

	
	ret = phy_write(phydev, MDIO_CTRL1, BMCR_RESET);
	if (ret < 0)
		return ret;
	ret = phy_read_poll_timeout(phydev, MDIO_CTRL1, val,
				    !(val & BMCR_RESET), 20000, 10000000, true);
	if (ret < 0)
		phydev_warn(phydev, "BMCR reset did not self-clear (%d), continuing\n",
			    ret);

	
	ret = phy_read(phydev, MDIO_CTRL1);
	if (ret < 0)
		return ret;
	ret = phy_write(phydev, MDIO_CTRL1,
			ret | BMCR_ANENABLE | BMCR_ANRESTART);
	if (ret < 0)
		return ret;

	phydev_info(phydev,
		    "datasheet bring-up done (0xa468=0003, reset, AN restart)\n");

	return 0;
}

static int rtl8221b_rtl9617c_finish(struct phy_device *phydev)
{
	int ret;

	ret = rtl8221b_update(phydev, MDIO_MMD_VEND2, 0xa408, 0x1e0, 0x140);
	if (ret < 0)
		return ret;
	ret = rtl8221b_update(phydev, MDIO_MMD_VEND2, 0xa412, 0x300, 0x200);
	if (ret < 0)
		return ret;
	return rtl8221b_update(phydev, MDIO_MMD_VEND2, 0xa5d4, 0, BIT(7));
}

int rtl8221b_rtl9617c_resume(struct phy_device *phydev)
{
	int ret = rtlgen_c45_resume(phydev);

	if (ret < 0)
		return ret;
	if (rtl8221b_cfg(phydev)->hsgmii_init_off ||
	    rtl8221b_cfg(phydev)->no_vendor_finish)
		return 0;
	return rtl8221b_rtl9617c_finish(phydev);
}

int rtl8221b_rtl9617c_config_aneg(struct phy_device *phydev)
{
	
	if (rtl8221b_cfg(phydev)->no_vendor_finish)
		return rtl822x_c45_config_aneg(phydev);
	if (rtl8221b_cfg(phydev)->hsgmii_init_off)
		return 0;
	
	{
		int ret = rtl8221b_rtl9617c_finish(phydev);

		if (ret < 0)
			return ret;
	}
	return rtl822x_c45_config_aneg(phydev);
}

int rtl8221b_rtl9617c_config_init(struct phy_device *phydev)
{
	int ret;

	
	if (rtl8221b_cfg(phydev)->hsgmii_init_off)
		return 0;

	ret = rtl8221b_wait_ready(phydev);
	if (ret < 0)
		goto error;
	
	if (rtl8221b_cfg(phydev)->oem_sds_option) {
		
		ret = rtl8221b_update(phydev, MDIO_MMD_VEND1, 0x75f3, BIT(0), 0);
		if (ret < 0)
			goto error;
		ret = rtl8221b_update(phydev, MDIO_MMD_VEND1, 0x697a,
				      GENMASK(5, 0), 1);
		if (ret < 0)
			goto error;
		phydev_info(phydev, "SDS option mode set to 1 (HiSGMII+SGMII), 0x697A=0x%04x\n",
			    phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x697a));
	}
	
	if (rtl8221b_cfg(phydev)->vendor_patch) {
		ret = rtl8221b_load_patch(phydev);
		if (ret < 0)
			goto error;
	}
	
	if (rtl8221b_cfg(phydev)->serdes_clock_enable) {
		ret = rtl8221b_serdes_clock_enable(phydev);
		if (ret < 0)
			phydev_warn(phydev,
				    "SerDes clock enable failed (%d), continuing\n",
				    ret);
	}
	
	ret = rtl8221b_update(phydev, MDIO_MMD_PMAPMD, MDIO_CTRL1, 0, MDIO_CTRL1_LPOWER);
	if (ret < 0)
		goto error;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xd032, 0x00a7);
	if (ret < 0)
		goto error;
	ret = rtl8221b_hsgmii(phydev);
	if (ret < 0) {
		
		phydev_warn(phydev, "HiSGMII bring-up incomplete (%d), continuing\n",
			    ret);
		ret = 0;
	}
	
	if (rtl8221b_cfg(phydev)->vendor_mmd31) {
		ret = rtl8221b_vendor_mmd31_apply(phydev);
		if (ret < 0) {
			phydev_warn(phydev,
				    "vendor MMD31 sequence failed (%d), continuing\n",
				    ret);
			ret = 0;
		}
	}
	
	if (rtl8221b_cfg(phydev)->no_vendor_finish)
		return 0;
	
	return rtl8221b_rtl9617c_finish(phydev);
error:
	phydev_err(phydev, "RTL9617C cold initialization failed: %d\n", ret);
	return ret;
}

int rtl8221b_rtl9617c_get_features(struct phy_device *phydev)
{
	int ret = rtl8221b_wait_ready(phydev);

	return ret < 0 ? ret : rtl822x_c45_get_features(phydev);
}

int rtl8221b_rtl9617c_match(struct phy_device *phydev,
			       const struct phy_driver *driver)
{
	if (!rtl8221b_cfg(phydev)->hsgmii)
		return 0;
	
	if (phydev->is_c45)
		return (phydev->c45_ids.device_ids[MDIO_MMD_PMAPMD] & 0xfffffff0) ==
		       0x001cc840;
	return (phydev->phy_id & 0xfffffff0) == 0x001cc840;
}
