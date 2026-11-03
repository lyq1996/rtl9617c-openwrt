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

/*
 * MMD access for this part.
 *
 * Upstream rtl822xb_read_mmd()/rtl822xb_write_mmd() reach MDIO_MMD_VEND2
 * through the RTL821x page-select register (C22 0x1f) for every register
 * outside 0xA400..0xA43C. That window does not carry the RTL8221B SerDes
 * block: reading 0x7580 as page 0x758 / offset 0x10 returns 0, while the
 * IEEE 802.3 MMD indirect sequence (C22 0x0d/0x0e) returns the real value.
 *
 * Measured on the PT1500 at the same instant:
 *	page path:  page=0x758, reg 0x10 -> 0x0000
 *	MMD path :  MMD30.0x7580         -> 0x9412
 *
 * The consequence of the page path is not a cosmetic misread: the PHY state
 * machine's read_status() times out (-110), phydev->link never becomes 1,
 * rtl_adjust_link() therefore never calls phy_set_speed(), and the SoC S3
 * lane keeps whatever the bootloader left behind (sdet=0, jack dead).
 *
 * Use the MMD indirect sequence for every devad. mmd_phy_read()/_write()
 * take bus->mdio_lock themselves, so this composes with phylib.
 */
static int rtl8221b_read_mmd(struct phy_device *phydev, int devnum, u16 reg)
{
	return mmd_phy_read(phydev->mdio.bus, phydev->mdio.addr,
			    phydev->is_c45, devnum, reg);
}

static int rtl8221b_write_mmd(struct phy_device *phydev, int devnum, u16 reg,
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

/*
 * Vendor patch load.
 *
 * OEM Rtl8221_phy_init (rtl8221b.ko+0x4a88) gates this on MMD3.0x0003:
 * it loads the full patch only when (v & 0xF) == 8 and ((v >> 4) & 0x3F) == 4,
 * i.e. v & 0xFF == 0x48. Measured on this unit, MMD3.0x0003 reads 0xC849,
 * whose low byte is 0x49 -- one bit off -- so the OEM gate sends even the
 * stock firmware down the else branch, whose only action is a write to
 * MMD30.0x75B5. That write does not land on this part (every MMD30 register
 * reads back 0xDEAD over both Clause 22 and Clause 45), so on the stock
 * firmware the vendor patch is never applied either.
 *
 * The patch content is not revision-specific: it is a RAM/analog tuning
 * table, and the reachable MMD31 sequence that follows it in the OEM driver
 * is what the part needs to bring its SerDes up. Load it unconditionally and
 * keep the revision in the log so the decision stays visible.
 */
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
	/*
	 * OEM writes 0x75B5/0x75B6/0x75B1 here, but those live in MMD30, which
	 * does not answer on this board (all MMD30 registers read back 0xDEAD
	 * over Clause 22 and Clause 45). The equivalent tuning is reachable in
	 * MMD31 and is applied by rtl8221b_vendor_mmd31() below.
	 */
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa436, 0x801e);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa438, 0x20);
	if (ret < 0)
		return ret;
	/*
	 * OEM closes its init with 0xA400 = 0x9200. In the datasheet 0xA400 is
	 * FEDCR, whose bits 15, 12 and 9 are all Reserved -- 0x9200 sets three
	 * reserved bits and nothing documented. Measured on this unit with it
	 * enabled: BMCR read 0x9000 afterwards (reset bit still asserted,
	 * autoneg off) and the copper link never came back, so it is the prime
	 * suspect for the dead PHY. Keep it behind a property.
	 */
	if (of_property_read_bool(phydev->mdio.dev.of_node,
				  "realtek,vendor-a400"))
		return phy_write_mmd(phydev, MDIO_MMD_VEND2, 0xa400, 0x9200);
	return 0;
}

/*
 * Enable the PHY's SerDes clock.
 *
 * MMD30.0x7581 bit4 is "SERDES Clock Enable": 1 = enabled, 0 = disabled, and
 * the datasheet warns that misuse causes SERDES malfunction. Read once from
 * this unit it came back 0x0000 -- clock off -- while the same read showed
 * 0x7580 = 0x9412 (PHY-side, no isolation, no forced link-down, mode 0x12 =
 * HiSGMII, matching the SoC's sds_mode0) and 0x758D = 0x0002 (bit1 SERDES
 * Link OK set). So the mode and the link are fine and only the clock is off,
 * which is exactly what makes the SoC report sdet=0 while the copper side
 * works.
 *
 * MMD30 is only intermittently reachable on this board -- 7 of 30 reads
 * returned a real value, the rest 0xDEAD or ETIMEDOUT, and 400 later attempts
 * returned none. The reachable window appears shortly after the PHY is
 * powered and the vendor patch has been loaded, so retry here rather than
 * once: config_init runs inside that window.
 */
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

/*
 * The MMD31 half of OEM Rtl8221_phy_init (rtl8221b.ko+0x4a88).
 *
 * The OEM driver issues 110 MMD31 operations and only 5 MMD30 ones; every
 * value below is copied from that sequence in its original order. These are
 * the registers that actually configure the part's analog/PCS blocks, and
 * unlike MMD30 they are all reachable here. Registers the earlier patch
 * tables already cover (0xA436/0xA438 window setup, 0xB820/0xB82E/0xB892/
 * 0xB896, 0xA412, 0xA5D4) are left where they are to keep the ordering that
 * was measured to work; the remainder is added here.
 */
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

/* 0xA436/0xA438 indirect-window writes from the same OEM sequence. */
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
	/*
	 * The 0xA436/0xA438 indirect window writes are skipped: with them the
	 * copper link never returned (PHYSR stuck at 0x1600, MDI Plug clear),
	 * and they touch the analog front end rather than the SerDes mode, so
	 * they are the most likely culprit. The direct MMD31 writes above are
	 * the ones that match the OEM sequence's register set.
	 */
	if (of_property_read_bool(phydev->mdio.dev.of_node,
				  "realtek,vendor-window")) {
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

	/*
	 * Software reset. Measured with the vendor patch loaded: the part needs
	 * well over the 2 s this used to allow before it clears BMCR.RESET, and
	 * the -ETIMEDOUT that produced aborted the whole function, so autoneg
	 * was never restarted and the copper link stayed dead (PHYSR 0x1600,
	 * BMCR 0x9000 with the reset bit still set). Allow 10 s and keep going
	 * even if the bit is still set, so a slow part cannot cost the link.
	 */
	ret = phy_write(phydev, MDIO_CTRL1, BMCR_RESET);
	if (ret < 0)
		return ret;
	ret = phy_read_poll_timeout(phydev, MDIO_CTRL1, val,
				    !(val & BMCR_RESET), 20000, 10000000, true);
	if (ret < 0)
		phydev_warn(phydev, "BMCR reset did not self-clear (%d), continuing\n",
			    ret);

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
				  "realtek,hsgmii-init-off") ||
	    of_property_read_bool(phydev->mdio.dev.of_node,
				  "realtek,no-vendor-finish"))
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
	/*
	 * Match the stock firmware's SDS option mode.
	 *
	 * The PT1500's stock firmware leaves 0x697A at 0x0001, i.e. mode 1 =
	 * "HiSGMII+SGMII", so the SERDES mode follows the copper link speed:
	 * 2.5G uses HiSGMII and 1G/100M/10M fall back to SGMII on their own.
	 * The power-on default is 3 ("HiSGMII only"), which pins the line side
	 * to HiSGMII and leaves no negotiated fallback. Measured on the stock
	 * firmware with the jack linked at 2500 and rx counters climbing:
	 *
	 *	MMD30.0x697A = 0x0001
	 *	MMD30.0x7580 = 0x9412   (mode 0x12 HiSGMII)
	 *	MMD30.0x758C = 0x0001   (SERDES signal detect)
	 *	MMD30.0x758D = 0x801e   (link OK, all channels OK)
	 *
	 * Opt in with realtek,sds-option-mode so trees that want the power-on
	 * value keep it.
	 */
	if (of_property_read_bool(phydev->mdio.dev.of_node,
				  "realtek,oem-sds-option")) {
		/* OEM Rtl8221_serdes_option_set_for_init() @ rtl8221b.ko+0x3de0
		 * writes these five registers in this order. */
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
	/*
	 * Loading the vendor patch is off by default.
	 *
	 * Measured on this board: with the patch loaded BMCR reads 0x9000 (reset
	 * bit stuck) and PHYSR parks at 0x1600 (bit13 MDI Plug clear -- the part
	 * believes no cable is plugged in), and the copper link never comes back.
	 * Without it the same registers reach 0x1140 / 0x13DC with the link up at
	 * 2.5G. Opt in with realtek,vendor-patch when bisecting.
	 */
	if (of_property_read_bool(phydev->mdio.dev.of_node,
				  "realtek,vendor-patch")) {
		ret = rtl8221b_load_patch(phydev);
		if (ret < 0)
			goto error;
	}
	/* The clock write is experimental and is off in the baseline. Enable it
	 * explicitly with realtek,serdes-clock-enable after a locked measurement
	 * proves that MMD30.0x7581 is the missing control. */
	if (of_property_read_bool(phydev->mdio.dev.of_node,
				  "realtek,serdes-clock-enable")) {
		ret = rtl8221b_serdes_clock_enable(phydev);
		if (ret < 0)
			phydev_warn(phydev,
				    "SerDes clock enable failed (%d), continuing\n",
				    ret);
	}
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
	/*
	 * The reachable half of the OEM cold-init sequence.
	 *
	 * Measured on this unit with the sequence enabled: PHYSR went from
	 * 0x13DC (copper link up, 2.5G full) to 0x1600 and then 0x00A0, i.e.
	 * the copper link was lost and never came back, and BMCR read 0x9000
	 * (reset still asserted, autoneg off). So at least one write in here is
	 * wrong for this board. Keep it behind a DT property so the default
	 * build keeps the previously-working copper behaviour and the sequence
	 * can be bisected from the device without a rebuild each time.
	 */
	if (of_property_read_bool(phydev->mdio.dev.of_node,
				  "realtek,vendor-mmd31")) {
		ret = rtl8221b_vendor_mmd31_apply(phydev);
		if (ret < 0) {
			phydev_warn(phydev,
				    "vendor MMD31 sequence failed (%d), continuing\n",
				    ret);
			ret = 0;
		}
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
