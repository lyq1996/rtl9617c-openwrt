// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/mdio.h>
#include <linux/mii.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/slab.h>

#include "realtek.h"
#include "realtek_hisgmii_patch.h"

#define RTL8221B_ID_MASK		0xfffffff0
#define RTL8221B_ID			0x001cc840

#define RTL8221B_VND1_SERDES_OPTION	0x697a
#define RTL8221B_SERDES_OPTION_MODE	GENMASK(5, 0)
#define RTL8221B_VND1_SERDES_CTRL	0x75f3
#define RTL8221B_SERDES_CTRL_OPTION	BIT(0)

#define RTL8221B_VND2_PHY_STATE		0xa420
#define RTL8221B_PHY_STATE_MASK		GENMASK(1, 0)
#define RTL8221B_PHY_STATE_READY	3
#define RTL8221B_VND2_PHY_CTRL		0xa468
#define RTL8221B_PHY_CTRL_HISGMII	BIT(1)
#define RTL8221B_VND2_SRAM_ADDR		0xa436
#define RTL8221B_VND2_SRAM_DATA		0xa438
#define RTL8221B_VND2_PATCH_CTRL	0xb820
#define RTL8221B_PATCH_CTRL_REQ		BIT(4)
#define RTL8221B_PATCH_CTRL_RAM		BIT(7)
#define RTL8221B_VND2_PATCH_STATE	0xb800
#define RTL8221B_PATCH_STATE_READY	BIT(6)
#define RTL8221B_VND2_PATCH_KEY		0xb82e
#define RTL8221B_PATCH_KEY_EN		BIT(0)
#define RTL8221B_VND2_PATCH_LOCK	0xb892
#define RTL8221B_VND2_PATCH_LOCK_CTRL	0xb896
#define RTL8221B_PATCH_LOCK_EN		BIT(0)
#define RTL8221B_VND2_LED_CFG(n)	(0xd032 + (n) * 2)
#define RTL8221B_VND2_LED_EN		0xd040
#define RTL8221B_VND2_LED_ACT		0xd044

#define RTL8221B_SRAM_PATCH_FLAG	0x801e
#define RTL8221B_SRAM_PATCH_DONE	0x0020
#define RTL8221B_SRAM_PATCH_KEY		0x8024
#define RTL8221B_PATCH_KEY		0x3701
#define RTL8221B_LED_COUNT		3
#define RTL8221B_LED_CFG_INIT		0x00a7
#define RTL8221B_LED_CFG_ON		0x0025

struct rtl8221b_hisgmii_priv {
	bool patch;
	bool serdes_option;
	u32 serdes_option_mode;
};

int rtl8221b_hisgmii_match_phy_device(struct phy_device *phydev,
				      const struct phy_driver *phydrv)
{
	u32 id = phydev->is_c45 ? phydev->c45_ids.device_ids[MDIO_MMD_PMAPMD] :
				  phydev->phy_id;

	if ((id & RTL8221B_ID_MASK) != RTL8221B_ID)
		return 0;
	return of_property_read_bool(phydev->mdio.dev.of_node, "realtek,hisgmii");
}

int rtl8221b_hisgmii_probe(struct phy_device *phydev)
{
	struct device_node *np = phydev->mdio.dev.of_node;
	struct rtl8221b_hisgmii_priv *priv;

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	phydev->priv = priv;

	priv->patch = of_property_read_bool(np, "realtek,vendor-patch");
	priv->serdes_option = !of_property_read_u32(np, "realtek,serdes-option-mode",
						    &priv->serdes_option_mode);

	return 0;
}

static int rtl8221b_hisgmii_wait_ready(struct phy_device *phydev)
{
	int val;

	return phy_read_mmd_poll_timeout(phydev, MDIO_MMD_VEND2,
					 RTL8221B_VND2_PHY_STATE, val,
					 (val & RTL8221B_PHY_STATE_MASK) ==
					 RTL8221B_PHY_STATE_READY,
					 2000, 1500000, false);
}

static int rtl8221b_hisgmii_sram_write(struct phy_device *phydev, u16 addr,
				       u16 val)
{
	int ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_SRAM_ADDR, addr);
	if (ret < 0)
		return ret;
	return phy_write_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_SRAM_DATA, val);
}

static int rtl8221b_hisgmii_write_regs(struct phy_device *phydev,
				       const struct rtl8221b_reg *regs,
				       unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, regs[i].reg,
				    regs[i].value);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int rtl8221b_hisgmii_patch_request(struct phy_device *phydev, bool request)
{
	int ret, val;

	if (request)
		ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2,
				       RTL8221B_VND2_PATCH_CTRL,
				       RTL8221B_PATCH_CTRL_REQ);
	else
		ret = phy_clear_bits_mmd(phydev, MDIO_MMD_VEND2,
					 RTL8221B_VND2_PATCH_CTRL,
					 RTL8221B_PATCH_CTRL_REQ);
	if (ret < 0)
		return ret;

	return phy_read_mmd_poll_timeout(phydev, MDIO_MMD_VEND2,
					 RTL8221B_VND2_PATCH_STATE, val,
					 !!(val & RTL8221B_PATCH_STATE_READY) == request,
					 1000, 100000, false);
}

static int rtl8221b_hisgmii_load_patch(struct phy_device *phydev)
{
	int ret, val;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_SRAM_ADDR,
			    RTL8221B_SRAM_PATCH_FLAG);
	if (ret < 0)
		return ret;
	val = phy_read_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_SRAM_DATA);
	if (val < 0)
		return val;
	if (val == RTL8221B_SRAM_PATCH_DONE)
		return 0;

	ret = rtl8221b_hisgmii_patch_request(phydev, true);
	if (ret < 0)
		return ret;
	ret = rtl8221b_hisgmii_sram_write(phydev, RTL8221B_SRAM_PATCH_KEY,
					  RTL8221B_PATCH_KEY);
	if (ret < 0)
		return ret;
	ret = rtl8221b_hisgmii_sram_write(phydev, RTL8221B_VND2_PATCH_KEY,
					  RTL8221B_PATCH_KEY_EN);
	if (ret < 0)
		return ret;

	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_PATCH_CTRL,
			       RTL8221B_PATCH_CTRL_RAM);
	if (ret < 0)
		return ret;
	ret = rtl8221b_hisgmii_write_regs(phydev, rtl8221b_patch_ram,
					  ARRAY_SIZE(rtl8221b_patch_ram));
	if (ret < 0)
		return ret;
	ret = phy_clear_bits_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_PATCH_CTRL,
				 RTL8221B_PATCH_CTRL_RAM);
	if (ret < 0)
		return ret;

	ret = rtl8221b_hisgmii_write_regs(phydev, rtl8221b_patch_regs,
					  ARRAY_SIZE(rtl8221b_patch_regs));
	if (ret < 0)
		return ret;

	ret = phy_clear_bits_mmd(phydev, MDIO_MMD_VEND2,
				 RTL8221B_VND2_PATCH_LOCK_CTRL,
				 RTL8221B_PATCH_LOCK_EN);
	if (ret < 0)
		return ret;
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_PATCH_LOCK, 0);
	if (ret < 0)
		return ret;
	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_PATCH_LOCK_CTRL,
			       RTL8221B_PATCH_LOCK_EN);
	if (ret < 0)
		return ret;

	ret = rtl8221b_hisgmii_sram_write(phydev, 0, 0);
	if (ret < 0)
		return ret;
	ret = phy_clear_bits_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_PATCH_KEY,
				 RTL8221B_PATCH_KEY_EN);
	if (ret < 0)
		return ret;
	ret = rtl8221b_hisgmii_sram_write(phydev, RTL8221B_SRAM_PATCH_KEY, 0);
	if (ret < 0)
		return ret;

	ret = rtl8221b_hisgmii_patch_request(phydev, false);
	if (ret < 0)
		return ret;

	ret = rtl8221b_hisgmii_write_regs(phydev, rtl8221b_patch_analog,
					  ARRAY_SIZE(rtl8221b_patch_analog));
	if (ret < 0)
		return ret;

	return rtl8221b_hisgmii_sram_write(phydev, RTL8221B_SRAM_PATCH_FLAG,
					   RTL8221B_SRAM_PATCH_DONE);
}

static int rtl8221b_hisgmii_enable(struct phy_device *phydev)
{
	int ret, val;

	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_PHY_CTRL,
			       RTL8221B_PHY_CTRL_HISGMII);
	if (ret < 0)
		return ret;

	ret = phy_write(phydev, MII_BMCR, BMCR_RESET);
	if (ret < 0)
		return ret;
	ret = phy_read_poll_timeout(phydev, MII_BMCR, val, !(val & BMCR_RESET),
				    20000, 10000000, true);
	if (ret < 0)
		phydev_warn(phydev, "reset timeout: %d\n", ret);

	return phy_set_bits(phydev, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
}

int rtl8221b_hisgmii_config_init(struct phy_device *phydev)
{
	struct rtl8221b_hisgmii_priv *priv = phydev->priv;
	int ret;

	ret = rtl8221b_hisgmii_wait_ready(phydev);
	if (ret < 0)
		return ret;

	if (priv->serdes_option) {
		ret = phy_clear_bits_mmd(phydev, MDIO_MMD_VEND1,
					 RTL8221B_VND1_SERDES_CTRL,
					 RTL8221B_SERDES_CTRL_OPTION);
		if (ret < 0)
			return ret;
		ret = phy_modify_mmd(phydev, MDIO_MMD_VEND1,
				     RTL8221B_VND1_SERDES_OPTION,
				     RTL8221B_SERDES_OPTION_MODE,
				     FIELD_PREP(RTL8221B_SERDES_OPTION_MODE,
						priv->serdes_option_mode));
		if (ret < 0)
			return ret;
	}

	if (priv->patch) {
		ret = rtl8221b_hisgmii_load_patch(phydev);
		if (ret < 0)
			return ret;
	}

	ret = phy_set_bits_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_CTRL1,
			       MDIO_CTRL1_LPOWER);
	if (ret < 0)
		return ret;
	if (list_empty(&phydev->leds)) {
		ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_LED_CFG(0),
				    RTL8221B_LED_CFG_INIT);
		if (ret < 0)
			return ret;
	}

	ret = rtl8221b_hisgmii_enable(phydev);
	if (ret < 0)
		phydev_warn(phydev, "HiSGMII enable failed: %d\n", ret);

	__set_bit(PHY_INTERFACE_MODE_SGMII, phydev->possible_interfaces);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, phydev->possible_interfaces);
	return 0;
}

int rtl8221b_hisgmii_get_features(struct phy_device *phydev)
{
	int ret;

	ret = rtl8221b_hisgmii_wait_ready(phydev);
	if (ret < 0)
		return ret;
	return rtl822x_c45_get_features(phydev);
}

int rtl8221b_hisgmii_read_status(struct phy_device *phydev)
{
	int ret;

	ret = rtl822x_c45_read_status(phydev);
	if (ret < 0 || !phydev->link)
		return ret;

	if (phydev->speed == SPEED_2500)
		phydev->interface = PHY_INTERFACE_MODE_2500BASEX;
	else
		phydev->interface = PHY_INTERFACE_MODE_SGMII;
	return 0;
}

int rtl8221b_hisgmii_led_brightness_set(struct phy_device *phydev, u8 index,
					enum led_brightness value)
{
	int ret;

	if (index >= RTL8221B_LED_COUNT)
		return -EINVAL;

	if (value == LED_OFF)
		return phy_write_mmd(phydev, MDIO_MMD_VEND2,
				     RTL8221B_VND2_LED_CFG(index), 0);

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_LED_CFG(index),
			    RTL8221B_LED_CFG_ON);
	if (ret < 0)
		return ret;
	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_LED_EN,
			       BIT(index));
	if (ret < 0)
		return ret;
	return phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, RTL8221B_VND2_LED_ACT,
				BIT(index));
}
