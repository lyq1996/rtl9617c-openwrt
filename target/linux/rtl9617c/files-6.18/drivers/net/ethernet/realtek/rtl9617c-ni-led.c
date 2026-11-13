/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mdio.h>
#include <linux/phy.h>

#include "rtl9617c-led.h"
#include "rtl9617c-ni-priv.h"
#include "rtl9617c-ni.h"

int rtl9617c_ni_led_output(struct rtl9617c_led *led, unsigned int value)
{
	struct rtl9617c_ni *priv = led->drvdata;
	struct net_device *dev = priv->ports[6];
	unsigned int channel = led->channel;
	unsigned int reg = 0xd032 + channel * 2;

	if (!dev || !dev->phydev)
		return -ENODEV;
	if (channel >= RTL_PHY_LED_COUNT)
		return -EINVAL;
	if (!value)
		return phy_write_mmd(dev->phydev, MDIO_MMD_VEND2, reg, 0);
	if (phy_write_mmd(dev->phydev, MDIO_MMD_VEND2, reg, 0x25) < 0)
		return -EIO;
	if (phy_set_bits_mmd(dev->phydev, MDIO_MMD_VEND2, 0xd040, BIT(channel)) < 0)
		return -EIO;
	return phy_set_bits_mmd(dev->phydev, MDIO_MMD_VEND2, 0xd044, BIT(channel));
}
