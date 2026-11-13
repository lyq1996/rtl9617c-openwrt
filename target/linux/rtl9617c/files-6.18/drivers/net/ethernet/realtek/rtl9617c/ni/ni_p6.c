// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/phy/phy.h>

#include "ni.h"

#define P6_RESET_BLOCKS			0x102001c0
#define P6_RESET_SERDES			BIT(31)
#define XGE_MISC_SEL			GENMASK(29, 28)
#define XGE_MISC_SEL_XFI		(2U << 28)
#define XGE_MISC_SEL_SGMII		BIT(28)
#define XGE_MISC_RESET			BIT(31)
#define XGE_COM_RX_RESET		BIT(15)
#define XGE_COM_TX_RESET		BIT(14)
#define XGE_COM_IFACE_RESET		BIT(13)
#define XGE_COM_MODE			GENMASK(6, 4)
#define XGE_TX_2_KEEP			(GENMASK(31, 13) | GENMASK(7, 5))
#define XGE_TX_2_INIT			0xa0a
#define XGE_CFG_1_CLEAR			(BIT(1) | BIT(4) | BIT(5) | BIT(9))
#define XGE_CFG_1_INIT			0x0bf3
#define XGE_TX_INIT			0x31
#define NITX_MISC_XGE_HOLD		BIT(20)
#define L2TM_ES_P6			BIT(6)
#define QM_ES_P6			BIT(14)

static void rtl_ni_p6_mac_config(struct rtl9617c_ni *ni)
{
	u32 qm, val;

	rtl_ni_rmw(ni, RTL_NI_L2TM_ES_CTRL, L2TM_ES_P6, 0);
	qm = rtl_ni_read(ni, RTL_NI_QM_ES_CTRL);
	rtl_ni_write(ni, RTL_NI_QM_ES_CTRL, qm & ~QM_ES_P6);
	rtl_ni_rmw(ni, RTL_NI_NITX_MISC_CFG, 0, NITX_MISC_XGE_HOLD);

	val = rtl_ni_read(ni, RTL_NI_XGE_MISC_CONFIG) & ~XGE_MISC_SEL;
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, val | XGE_MISC_RESET);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, val | XGE_MISC_RESET | XGE_MISC_SEL);
	usleep_range(10000, 11000);

	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_TX, GENMASK(1, 0), 0);
	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_TX, 0, BIT(0));
	val = rtl_ni_read(ni, RTL_NI_XGE_MAC_CFG_TX_2);
	rtl_ni_write(ni, RTL_NI_XGE_MAC_CFG_TX_2, (val & XGE_TX_2_KEEP) | XGE_TX_2_INIT);

	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_COM, XGE_COM_IFACE_RESET | XGE_COM_MODE,
		   XGE_COM_RX_RESET | XGE_COM_TX_RESET);
	usleep_range(10000, 11000);
	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_COM, XGE_COM_RX_RESET | XGE_COM_TX_RESET |
		   XGE_COM_IFACE_RESET | XGE_COM_MODE, 0);

	val = rtl_ni_read(ni, RTL_NI_XGE_MISC_CONFIG) & ~(XGE_MISC_RESET | XGE_MISC_SEL);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, val);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, val | XGE_MISC_SEL_XFI);
	rtl_ni_rmw(ni, RTL_NI_NITX_MISC_CFG, NITX_MISC_XGE_HOLD, 0);

	rtl_ni_rmw(ni, RTL_NI_L2TM_ES_CTRL, 0, L2TM_ES_P6);
	rtl_ni_write(ni, RTL_NI_QM_ES_CTRL, qm | QM_ES_P6);
}

static void rtl_ni_p6_iface_select(struct rtl9617c_ni *ni)
{
	u32 val;

	rtl_ni_rmw(ni, RTL_NI_NITX_MISC_CFG, 0, NITX_MISC_XGE_HOLD);
	val = rtl_ni_read(ni, RTL_NI_XGE_MISC_CONFIG) & ~(XGE_MISC_RESET | XGE_MISC_SEL);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, val);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG,
		     val | XGE_MISC_RESET | XGE_MISC_SEL_SGMII);
	usleep_range(10000, 11000);

	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_COM, 0, XGE_COM_IFACE_RESET);
	usleep_range(10000, 11000);
	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_COM, XGE_COM_IFACE_RESET, 0);
	rtl_ni_rmw(ni, RTL_NI_NITX_MISC_CFG, NITX_MISC_XGE_HOLD, 0);
}

static int rtl_ni_p6_serdes(struct rtl9617c_ni *ni, phy_interface_t interface)
{
	struct rtl9617c_ni_port *port = ni->ports[RTL_NI_PORT_P6];
	int ret;

	writel(readl(ni->reset) | P6_RESET_SERDES, ni->reset);
	udelay(1);
	writel(readl(ni->reset) & ~P6_RESET_SERDES, ni->reset);

	ret = phy_set_mode_ext(port->serdes, PHY_MODE_ETHERNET, interface);
	if (ret)
		dev_warn(ni->dev, "P6 SerDes %s setup failed: %d\n",
			 phy_modes(interface), ret);
	return ret;
}

int rtl_ni_p6_bringup(struct rtl9617c_ni *ni)
{
	struct rtl9617c_ni_port *port = ni->ports[RTL_NI_PORT_P6];
	int ret;

	if (!port || !port->serdes || ni->p6_ready)
		return 0;

	if (!ni->serdes_on) {
		ret = phy_power_on(port->serdes);
		if (ret) {
			dev_warn(ni->dev, "P6 SerDes power on failed: %d\n", ret);
			return ret;
		}
		ni->serdes_on = true;
	}

	writel(readl(ni->reset) | P6_RESET_BLOCKS, ni->reset);
	writel(readl(ni->reset) | P6_RESET_SERDES, ni->reset);
	msleep(50);
	writel(readl(ni->reset) & ~P6_RESET_SERDES, ni->reset);
	usleep_range(1000, 2000);

	ret = rtl_ni_p6_serdes(ni, port->interface);

	rtl_ni_p6_mac_config(ni);
	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_1, XGE_CFG_1_CLEAR, XGE_CFG_1_INIT);
	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_TX, XGE_TX_INIT, XGE_TX_INIT);
	rtl_ni_write(ni, RTL_NI_GLB_INTF_RST_CONFIG, 0);
	rtl_ni_p6_iface_select(ni);

	ni->p6_ready = !ret;
	return ret;
}

int rtl_ni_p6_set_interface(struct rtl9617c_ni *ni, phy_interface_t interface)
{
	struct rtl9617c_ni_port *port = ni->ports[RTL_NI_PORT_P6];
	int ret;

	if (!port->serdes || port->interface == interface)
		return 0;

	ret = rtl_ni_p6_serdes(ni, interface);
	if (ret)
		return ret;
	port->interface = interface;
	rtl_ni_p6_mac_config(ni);
	rtl_ni_p6_iface_select(ni);
	return 0;
}
