/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL8221B_RTL9617C_H
#define RTL8221B_RTL9617C_H

#include <linux/phy.h>

int rtl8221b_read_mmd(struct phy_device *phydev, int devnum, u16 reg);
int rtl8221b_write_mmd(struct phy_device *phydev, int devnum, u16 reg, u16 value);
int rtl8221b_rtl9617c_match(struct phy_device *phydev, const struct phy_driver *driver);
int rtl8221b_rtl9617c_get_features(struct phy_device *phydev);
int rtl8221b_rtl9617c_config_init(struct phy_device *phydev);
int rtl8221b_rtl9617c_config_aneg(struct phy_device *phydev);
int rtl8221b_rtl9617c_resume(struct phy_device *phydev);

#endif
