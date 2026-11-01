/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TYFZA1 RTL8221B cold initialization, rewritten from this board's OEM
 * rtl8221b.ko Rtl8221_phy_init and Rtl8221_serdes_option_set.
 * OEM module SHA256: 0ba23162b42e457b0628b0899e89883198aac919a14ddab91997fd9ef5710652
 * The opaque revision-8 patch words are retained in their original order.
 */
#ifndef RTL8221B_TYFZA1_H
#define RTL8221B_TYFZA1_H

struct rtl8221b_reg {
	u16 reg, value;
};

#include "rtl8221b-tyfza1-patch.h"

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

/* Preserve unrelated bits, but retain the OEM's required write strobes. */
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

	/* The part needs about a second after reset before 0xa420 reports
	 * ready (measured on the PT1500: 0x23 once settled). The 100 ms the
	 * OEM used here aborted config_init with -ETIMEDOUT, which left the
	 * line side of the PHY unconfigured: the copper link came up at 2.5G
	 * while the SoC saw no frames at all. */
	return phy_read_mmd_poll_timeout(phydev, MDIO_MMD_VEND2, 0xa420,
					val, (val & 3) == 3, 2000, 1500000, false);
}

static int rtl8221b_load_patch(struct phy_device *phydev)
{
	int ret, val;

	val = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, MII_PHYSID2);
	if (val < 0)
		return val;
	if ((val & 0x3ff) != 0x048)
		return rtl8221b_update(phydev, MDIO_MMD_VEND1, 0x75b5, 0xf, 4);

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
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x75b5, 0xe086);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x75b6, 0x0024);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x75b1, 0x0013);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa436, 0x801e);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa438, 0x20);
	if (ret < 0)
		return ret;
	return phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa400, 0x9200);
}

static int rtl8221b_hsgmii(struct phy_device *phydev)
{
	int ret, val;

	/*
	 * Bring-up order taken from the RTL8221B(I)-VB/VM datasheet
	 * (Track ID JATR-8275-15 Rev 1.0). The datasheet lists the events that
	 * trigger a full initialization: power-up, hardware reset, software
	 * reset (MMD7.0x0000.15), restart auto-negotiation (MMD7.0x0000.9),
	 * transition from power down to power up, and entering the link fail
	 * state. It also defines PHYSCR (MMD31.0xA468) bit1 as "PHY Special
	 * Config Done", which software must write to 1.
	 *
	 * Measured on this unit before this change: 0xA468 read 0x0001 (bit1
	 * clear) and PHYSR (0xA434) read 0x5602, i.e. ALDPS bit14 set and
	 * MDI Plug bit13 clear -- the part believed no cable was plugged in.
	 * Writing 0xA468.1 and then doing software reset + autoneg restart
	 * moved PHYSR to 0x1b1e: ALDPS cleared, link bit2 set, and the
	 * Speed_h/Speed_l pair decoded as 2.5G full duplex. So the sequence
	 * below is what actually establishes the copper link.
	 *
	 * The HiSGMII mode words are deliberately not written: the datasheet
	 * defines 0x697A[5:0] as the SDS option mode (0: 2500Base-X+SGMII,
	 * 1: HiSGMII+SGMII, 2: 2500Base-X, 3: HiSGMII), and forcing the
	 * HiSGMII variants on this board drops the copper link instead.
	 */
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
				    !(val & BMCR_RESET), 10000, 2000000, true);
	if (ret < 0)
		return ret;

	/* Restart auto-negotiation is itself one of the datasheet's
	 * initialization triggers, and it is what makes the part re-run its
	 * cable/energy detection after the reset. */
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

/* Vendor finish that must run after the PHY is powered and before autoneg.
 * phylib issues suspend/resume between config_init and config_aneg, and the
 * resume path overwrites these registers, so they cannot live only in
 * config_init (measured: 0xa408/0xa412 read back at their defaults). */
static int rtl8221b_tyfza1_finish(struct phy_device *phydev)
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

static int rtl8221b_tyfza1_resume(struct phy_device *phydev)
{
	int ret = rtlgen_c45_resume(phydev);

	if (ret < 0)
		return ret;
	if (of_property_read_bool(phydev->mdio.dev.of_node,
				  "realtek,hsgmii-init-off"))
		return 0;
	return rtl8221b_tyfza1_finish(phydev);
}

static int rtl8221b_tyfza1_config_aneg(struct phy_device *phydev)
{
	/* PT1500 runs the generic aneg path but not the TYFZA1-only finish. */
	if (of_property_read_bool(phydev->mdio.dev.of_node,
				  "realtek,no-vendor-finish"))
		return rtl822x_c45_config_aneg(phydev);
	if (of_property_read_bool(phydev->mdio.dev.of_node,
				  "realtek,hsgmii-init-off"))
		return 0;
	/* config_aneg runs after config_init and before every autoneg restart,
	 * regardless of whether the PHY went through suspend/resume. */
	{
		int ret = rtl8221b_tyfza1_finish(phydev);

		if (ret < 0)
			return ret;
	}
	return rtl822x_c45_config_aneg(phydev);
}

static int rtl8221b_tyfza1_config_init(struct phy_device *phydev)
{
	int ret;

	/* Bring-up switch: on some boards the vendor patch load leaves the part
	 * unreachable (status reads then fail with -EINVAL), so allow the tree
	 * to keep the PHY in its power-on state for inspection. */
	if (of_property_read_bool(phydev->mdio.dev.of_node, "realtek,hsgmii-init-off"))
		return 0;

	ret = rtl8221b_wait_ready(phydev);
	if (ret < 0)
		goto error;
	ret = rtl8221b_load_patch(phydev);
	if (ret < 0)
		goto error;
	/* Original init disables the copper PHY before changing its SerDes mode.
	 * phylib's resume callback enables it after a successful attach. */
	ret = rtl8221b_update(phydev, MDIO_MMD_PMAPMD, MDIO_CTRL1, 0, MDIO_CTRL1_LPOWER);
	if (ret < 0)
		goto error;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xd032, 0x00a7);
	if (ret < 0)
		goto error;
	ret = rtl8221b_hsgmii(phydev);
	if (ret < 0) {
		/* Do not hand -ETIMEDOUT to phylib here: phy_error() parks the
		 * PHY and every later carrier/speed read fails with EINVAL, which
		 * costs the whole port. Keep the failure visible but continue. */
		phydev_warn(phydev, "HiSGMII bring-up incomplete (%d), continuing\n",
			    ret);
		ret = 0;
	}
	/* PT1500 keeps the PHY's own post-link state; the a408/a412/a5d4
	 * finish is only used by the TYFZA1 reference. */
	if (of_property_read_bool(phydev->mdio.dev.of_node,
				  "realtek,no-vendor-finish"))
		return 0;
	/* Also applied again from resume, after phylib powers the part up. */
	return rtl8221b_tyfza1_finish(phydev);
error:
	phydev_err(phydev, "TYFZA1 cold initialization failed: %d\n", ret);
	return ret;
}

static int rtl8221b_tyfza1_get_features(struct phy_device *phydev)
{
	int ret = rtl8221b_wait_ready(phydev);

	return ret < 0 ? ret : rtl822x_c45_get_features(phydev);
}

static int rtl8221b_tyfza1_match(struct phy_device *phydev,
			       const struct phy_driver *driver)
{
	if (!of_property_read_bool(phydev->mdio.dev.of_node, "realtek,hsgmii"))
		return 0;
	/* This board answers Clause 22 at its address (0x001cc849) and nothing
	 * through Clause 45 framing, so both descriptions have to match. */
	if (phydev->is_c45)
		return (phydev->c45_ids.device_ids[MDIO_MMD_PMAPMD] & 0xfffffff0) ==
		       0x001cc840;
	return (phydev->phy_id & 0xfffffff0) == 0x001cc840;
}

#endif
