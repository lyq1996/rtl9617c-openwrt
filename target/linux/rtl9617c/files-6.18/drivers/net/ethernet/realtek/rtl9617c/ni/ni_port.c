// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy/phy.h>
#include <linux/phylink.h>
#include <linux/rtnetlink.h>

#include "ni.h"
#include "fe/fe.h"

#define PORT_STATIC_MAC			GENMASK(31, 24)
#define PORT_STATIC_SPEED		GENMASK(3, 0)
#define PORT_GLB_10M			BIT(0)
#define PORT_GLB_HALF			BIT(1)
#define PORT_ENABLE			BIT(0)
#define PORT_RX_FLOW_DISABLE		BIT(8)
#define PORT_TX_FLOW_DISABLE		BIT(12)
#define L2TE_FC_RX_EN			BIT(0)
#define L2TE_FC_TX_EN			BIT(1)
#define XGE6_FC_PAU_TX_EN		BIT(2)
#define XGE6_FC_PAU_RX_EN		BIT(3)
#define XGE_MAC_PQUANTA_MASK		GENMASK(15, 0)
#define XGE_MAC_PQUANTA_DEFAULT		0xffff
/*
 * header_a fields this port puts in play.  header_high is the word the DMA ring
 * carries at +8, i.e. header_a[31:0], where OEM sets bit 13 for control frames.
 * header_low is the word at +12, i.e. header_a[63:32], whose bits [2:0] are cos,
 * [8:3] ldpid, [14:9] lspid and [29] fe_bypass (RE-1 spec 2.5); pkt_size in
 * [28:15] of that word is filled in by ni_dma.c.
 */
#define TX_HDR_HIGH			BIT(13)
#define TX_HDR_LOW(port)		((16 << 9) | ((port) << 3) | BIT(29))
#define GLB_RESET_QXGMII		BIT(31)
#define XGE_COM_RX_RESET		BIT(15)
#define XGE_COM_TX_RESET		BIT(14)
#define XGE_COM_IFACE_RESET		BIT(13)
#define XGE_COM_MODE			GENMASK(6, 4)
#define XGE_TX_2_KEEP			(~(RTL_NI_XGE_TX2_READ_THR | \
					   RTL_NI_XGE_TX2_FIFO_AFULL))
#define XGE_TX_2_INIT			(FIELD_PREP(RTL_NI_XGE_TX2_READ_THR, 10) | \
					 FIELD_PREP(RTL_NI_XGE_TX2_FIFO_AFULL, 10))
#define XGE_RX_CLEAR			(RTL_NI_XGE_RX_DROP_SHORT | \
					 RTL_NI_XGE_RX_PAUSE_TERM | \
					 RTL_NI_XGE_RX_PAUSE_EXTRACT | \
					 RTL_NI_XGE_RX_FCS_CHK_EN)
#define XGE_RX_INIT			(RTL_NI_XGE_RX_PROMIS | \
					 RTL_NI_XGE_RX_DROP_SHORT | \
					 RTL_NI_XGE_RX_PAUSE_TERM | \
					 RTL_NI_XGE_RX_PAUSE_EXTRACT | \
					 RTL_NI_XGE_RX_LEN_CHK_EN | \
					 RTL_NI_XGE_RX_LEN_CHK_PAUSE | \
					 RTL_NI_XGE_RX_LEN_CHK_PAD | \
					 RTL_NI_XGE_RX_FCS_CHK_EN | \
					 RTL_NI_XGE_RX_CRC_ERR_FATAL)
#define XGE_TX_INIT			(FIELD_PREP(RTL_NI_XGE_TX_IFG_MODE, 1) | \
					 RTL_NI_XGE_TX_PAUSE_REQ_EN | \
					 RTL_NI_XGE_TX_PAUSE_AUTO_XON)

static void rtl_ni_p6_mac_config(struct rtl9617c_ni *ni)
{
	u32 qm, val;

	rtl_ni_rmw(ni, RTL_L2TM_ES_CTRL, RTL_L2TM_ES_CTRL_P6, 0);
	qm = rtl_ni_read(ni, RTL_QM_ES_CTRL);
	rtl_ni_write(ni, RTL_QM_ES_CTRL, qm & ~RTL_QM_ES_NI_P6);
	rtl_ni_rmw(ni, RTL_NI_GLB_NITX_MISC_CFG, 0,
		   RTL_NI_GLB_NITX_XGE6_TX_DRAIN);

	/* Toggle gige_mode + inf_sel while resetting XGE MAC */
	val = rtl_ni_read(ni, RTL_NI_XGE_MISC_CONFIG) &
	      ~RTL_NI_XGE_MISC_INF_SEL;
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG,
		     val | RTL_NI_XGE_MISC_GIGE_MODE);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG,
		     val | RTL_NI_XGE_MISC_GIGE_MODE |
		     RTL_NI_XGE_MISC_INF_SEL);
	usleep_range(10000, 11000);

	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_TX, RTL_NI_XGE_TX_IFG_MODE, 0);
	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_TX, 0,
		   FIELD_PREP(RTL_NI_XGE_TX_IFG_MODE, 1));
	val = rtl_ni_read(ni, RTL_NI_XGE_MAC_CFG_TX_2);
	rtl_ni_write(ni, RTL_NI_XGE_MAC_CFG_TX_2,
		     (val & XGE_TX_2_KEEP) | XGE_TX_2_INIT);

	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_COM, XGE_COM_IFACE_RESET | XGE_COM_MODE,
		   XGE_COM_RX_RESET | XGE_COM_TX_RESET);
	usleep_range(10000, 11000);
	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_COM, XGE_COM_RX_RESET | XGE_COM_TX_RESET |
		   XGE_COM_IFACE_RESET | XGE_COM_MODE, 0);

	val = rtl_ni_read(ni, RTL_NI_XGE_MISC_CONFIG) &
	      ~(RTL_NI_XGE_MISC_GIGE_MODE | RTL_NI_XGE_MISC_INF_SEL);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, val);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, val | RTL_NI_XGE_MISC_INF_XFI);
	rtl_ni_rmw(ni, RTL_NI_GLB_NITX_MISC_CFG,
		   RTL_NI_GLB_NITX_XGE6_TX_DRAIN, 0);

	rtl_ni_rmw(ni, RTL_L2TM_ES_CTRL, 0, RTL_L2TM_ES_CTRL_P6);
	rtl_ni_write(ni, RTL_QM_ES_CTRL, qm | RTL_QM_ES_NI_P6);
}

static void rtl_ni_p6_iface_select(struct rtl9617c_ni *ni)
{
	u32 val;

	rtl_ni_rmw(ni, RTL_NI_GLB_NITX_MISC_CFG, 0,
		   RTL_NI_GLB_NITX_XGE6_TX_DRAIN);
	val = rtl_ni_read(ni, RTL_NI_XGE_MISC_CONFIG) &
	      ~(RTL_NI_XGE_MISC_GIGE_MODE | RTL_NI_XGE_MISC_INF_SEL);
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG, val);
	/* Keep gige_mode set for 2.5G SGMII / HiSGMII operation (inf_sel = 1) */
	rtl_ni_write(ni, RTL_NI_XGE_MISC_CONFIG,
		     val | RTL_NI_XGE_MISC_GIGE_MODE |
		     RTL_NI_XGE_MISC_INF_SGMII);
	usleep_range(10000, 11000);

	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_COM, 0, XGE_COM_IFACE_RESET);
	usleep_range(10000, 11000);
	rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_COM, XGE_COM_IFACE_RESET, 0);
	rtl_ni_rmw(ni, RTL_NI_GLB_NITX_MISC_CFG,
		   RTL_NI_GLB_NITX_XGE6_TX_DRAIN, 0);
}

static int rtl_ni_port_serdes_apply(struct rtl9617c_ni_port *port,
				    phy_interface_t interface)
{
	struct rtl9617c_ni *ni = port->ni;
	int ret;

	if (port->id == RTL_NI_PORT_P6) {
		writel(readl(ni->reset) | GLB_RESET_QXGMII, ni->reset);
		udelay(1);
		writel(readl(ni->reset) & ~GLB_RESET_QXGMII, ni->reset);
	}

	ret = phy_set_mode_ext(port->serdes, PHY_MODE_ETHERNET, interface);
	if (ret)
		dev_warn(ni->dev, "%s serdes %s setup failed: %d\n",
			 port->dev->name, phy_modes(interface), ret);
	return ret;
}

static int rtl_ni_port_set_interface(struct rtl9617c_ni_port *port,
				     phy_interface_t interface)
{
	int ret;

	if (!port->serdes || port->interface == interface)
		return 0;

	ret = rtl_ni_port_serdes_apply(port, interface);
	if (ret)
		return ret;
	port->interface = interface;

	if (port->id == RTL_NI_PORT_P6) {
		rtl_ni_p6_mac_config(port->ni);
		rtl_ni_p6_iface_select(port->ni);
	}
	return 0;
}

int rtl_ni_serdes_bringup(struct rtl9617c_ni *ni)
{
	struct rtl9617c_ni_port *port = NULL;
	unsigned int id;
	int ret = 0;

	for (id = 0; id < RTL_NI_PORTS; id++) {
		if (ni->ports[id] && ni->ports[id]->serdes) {
			port = ni->ports[id];
			break;
		}
	}
	if (!port || ni->serdes_ready)
		return 0;

	if (!ni->serdes_on) {
		ret = phy_power_on(port->serdes);
		if (ret) {
			dev_warn(ni->dev, "serdes power on failed: %d\n", ret);
			return ret;
		}
		ni->serdes_on = true;
	}

	writel(readl(ni->reset) | GLB_RESET_QXGMII, ni->reset);
	msleep(50);
	writel(readl(ni->reset) & ~GLB_RESET_QXGMII, ni->reset);
	usleep_range(1000, 2000);

	for (id = 0; id < RTL_NI_PORTS; id++) {
		port = ni->ports[id];
		if (!port || !port->serdes)
			continue;
		ret = rtl_ni_port_serdes_apply(port, port->interface);
		if (ret)
			break;
	}

	if (ni->ports[RTL_NI_PORT_P6]) {
		rtl_ni_p6_mac_config(ni);
		rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_COM,
			   RTL_NI_XGE_MAC_CFG_COM_CLR_ON_RD |
			   RTL_NI_XGE_MAC_CFG_COM_RT_MIB_EN,
			   RTL_NI_XGE_MAC_CFG_COM_RT_MIB_EN);
		rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_RX, XGE_RX_CLEAR, XGE_RX_INIT);
		rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_TX, XGE_TX_INIT, XGE_TX_INIT);
		rtl_ni_write(ni, RTL_NI_GLB_INTF_RST_CONFIG, 0);
		rtl_ni_p6_iface_select(ni);
	}

	ni->serdes_ready = !ret;
	return ret;
}

static struct rtl9617c_ni_port *rtl_ni_config_to_port(struct phylink_config *config)
{
	return netdev_priv(to_net_dev(config->dev));
}

static void rtl_ni_port_set_mac(struct rtl9617c_ni_port *port)
{
	rtl_ni_rmw(port->ni, RTL_NI_PORT_STATIC(port->id), PORT_STATIC_MAC,
		   FIELD_PREP(PORT_STATIC_MAC, port->dev->dev_addr[ETH_ALEN - 1]));
}

static void rtl_ni_mac_config(struct phylink_config *config, unsigned int mode,
			      const struct phylink_link_state *state)
{
	struct rtl9617c_ni_port *port = rtl_ni_config_to_port(config);

	rtl_ni_port_set_interface(port, state->interface);
}

#include "ni_ext.h"

static void rtl_ni_port_set_pause(struct rtl9617c_ni_port *port,
				  bool tx_pause, bool rx_pause)
{
	struct rtl9617c_ni *ni = port->ni;
	u32 l2te_fc = (rx_pause ? L2TE_FC_RX_EN : 0) |
		      (tx_pause ? L2TE_FC_TX_EN : 0);

	rtl_ni_rmw(ni, RTL_L2TE_FC_CTRL(port->id),
		   L2TE_FC_RX_EN | L2TE_FC_TX_EN, l2te_fc);
	rtl_ni_port_fc_ext_set(port, tx_pause, rx_pause);
	rtl_ni_eee_sync_link(port);

	if (rtl_ni_is_ge(port->id)) {
		rtl_ni_rmw(ni, RTL_NI_PORT_RX(port->id), PORT_RX_FLOW_DISABLE,
			   rx_pause ? 0 : PORT_RX_FLOW_DISABLE);
		rtl_ni_rmw(ni, RTL_NI_PORT_TX(port->id), PORT_TX_FLOW_DISABLE,
			   tx_pause ? 0 : PORT_TX_FLOW_DISABLE);
		return;
	}

	if (port->id == RTL_NI_PORT_P6) {
		u32 xge_fc = (tx_pause ? XGE6_FC_PAU_TX_EN : 0) |
			     (rx_pause ? XGE6_FC_PAU_RX_EN : 0);

		rtl_ni_rmw(ni, RTL_NI_XGE6_FC_CFG,
			   XGE6_FC_PAU_TX_EN | XGE6_FC_PAU_RX_EN, xge_fc);
		rtl_ni_rmw(ni, RTL_NI_XGE_MAC_PQUANTA, XGE_MAC_PQUANTA_MASK,
			   FIELD_PREP(XGE_MAC_PQUANTA_MASK,
				      tx_pause ? XGE_MAC_PQUANTA_DEFAULT : 0));
	}
}

static void rtl_ni_mac_link_up(struct phylink_config *config,
			       struct phy_device *phy, unsigned int mode,
			       phy_interface_t interface, int speed, int duplex,
			       bool tx_pause, bool rx_pause)
{
	struct rtl9617c_ni_port *port = rtl_ni_config_to_port(config);
	struct rtl9617c_ni *ni = port->ni;

	if (rtl_ni_is_ge(port->id)) {
		rtl_ni_rmw(ni, RTL_NI_PORT_STATIC(port->id), PORT_STATIC_SPEED,
			   speed >= SPEED_1000 ? 0 : 1);
		rtl_ni_rmw(ni, RTL_NI_PORT_GLB(port->id),
			   PORT_GLB_10M | PORT_GLB_HALF,
			   (speed == SPEED_10 ? PORT_GLB_10M : 0) |
			   (duplex == DUPLEX_HALF ? PORT_GLB_HALF : 0));
	}

	if (port->serdes)
		phy_set_speed(port->serdes, speed);

	rtl_ni_port_set_pause(port, tx_pause, rx_pause);
}

static void rtl_ni_mac_link_down(struct phylink_config *config, unsigned int mode,
				 phy_interface_t interface)
{
	struct rtl9617c_ni_port *port = rtl_ni_config_to_port(config);

	rtl_ni_port_set_pause(port, false, false);
}

static const struct phylink_mac_ops rtl_ni_phylink_ops = {
	.mac_config = rtl_ni_mac_config,
	.mac_link_up = rtl_ni_mac_link_up,
	.mac_link_down = rtl_ni_mac_link_down,
	.mac_enable_tx_lpi = rtl_ni_mac_enable_tx_lpi,
	.mac_disable_tx_lpi = rtl_ni_mac_disable_tx_lpi,
};

static int rtl_ni_open(struct net_device *dev)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	struct rtl9617c_ni *ni = port->ni;
	unsigned int id = port->id;
	int ret;

	ret = rtl_ni_engine_get(ni);
	if (ret)
		return ret;

	if (rtl_ni_is_ge(id)) {
		rtl_ni_rmw(ni, RTL_NI_GLB_INTF_RST_CONFIG,
			   BIT(id) | BIT(id + 6) | BIT(id + 12), 0);
		rtl_ni_port_set_mac(port);
		rtl_ni_rmw(ni, RTL_NI_PORT_RX(id), PORT_ENABLE, PORT_ENABLE);
		rtl_ni_rmw(ni, RTL_NI_PORT_TX(id), PORT_ENABLE, PORT_ENABLE);
	}

	phylink_start(port->phylink);
	netif_tx_start_all_queues(dev);
	for (id = 0; id < min_t(unsigned int, dev->real_num_tx_queues,
				ni->dma.num_rings); id++) {
		unsigned long flags;

		spin_lock_irqsave(&ni->dma.rings[id].lock, flags);
		if (ni->dma.rings[id].stopped)
			netif_tx_stop_queue(netdev_get_tx_queue(dev, id));
		spin_unlock_irqrestore(&ni->dma.rings[id].lock, flags);
	}
	return 0;
}

static int rtl_ni_stop(struct net_device *dev)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	struct rtl9617c_ni *ni = port->ni;

	netif_tx_stop_all_queues(dev);
	rtl_fe_flush_port(ni->fe, dev);
	if ((dev->features & NETIF_F_LOOPBACK) && dev->phydev &&
	    dev->phydev->loopback_enabled) {
		phy_loopback(dev->phydev, false, 0);
		dev->features &= ~NETIF_F_LOOPBACK;
	}
	phylink_stop(port->phylink);
	if (rtl_ni_is_ge(port->id)) {
		if (ni->qm.lan_dq_bufs)
			usleep_range(1000, 2000);
		rtl_ni_rmw(ni, RTL_NI_PORT_RX(port->id), PORT_ENABLE, 0);
		rtl_ni_rmw(ni, RTL_NI_PORT_TX(port->id), PORT_ENABLE, 0);
	}
	rtl_ni_engine_put(ni);
	return 0;
}

/*
 * QM RX runs single-buffer 2048B pools (RTL_NI_QM_DATA_SIZE = 1600, minus up
 * to 16B QM header = 1584B max RX frame). 1536 MTU fits PPPoE-over-1500
 * (1508+) plus ETH_HLEN and QinQ (1558B) safely below that 1584B ceiling.
 */
#define RTL_NI_PORT_MAX_MTU		1536U
#define RTL_NI_TX_FRAME_MAX		(RTL_NI_PORT_MAX_MTU + ETH_HLEN + \
					 2 * VLAN_HLEN)

static netdev_tx_t rtl_ni_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	unsigned int egress = rtl_ni_is_ge(port->id) ? port->id : RTL_NI_PORT_P6;
	const struct rtl9617c_ni_frame frame = {
		.header_high = TX_HDR_HIGH,
		.header_low = TX_HDR_LOW(egress) | (skb->priority & 7U),
	};

	/*
	 * GSO super-packets up to rtl_ni_tx_gso_max() (65500 B with tx_sg=1,
	 * 2032 B linear fallback with tx_sg=0) are accepted; all other frames
	 * stay under the 1558-byte single-frame ceiling.
	 */
	if (skb->len < ETH_HLEN ||
	    skb->len > (skb_is_gso(skb) ? rtl_ni_tx_gso_max() :
					  RTL_NI_TX_FRAME_MAX)) {
		DEV_STATS_INC(dev, tx_dropped);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	return rtl_ni_xmit_frame(port->ni, dev, skb, &frame);
}

static int rtl_ni_port_mac_probe(struct rtl9617c_ni_port *port)
{
	u8 addr[ETH_ALEN];
	int ret;

	ret = of_get_mac_address(port->dev->dev.of_node, addr);
	if (ret == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	if (!ret && is_valid_ether_addr(addr) &&
	    !memcmp(addr, port->ni->mac, ETH_ALEN - 1)) {
		eth_hw_addr_set(port->dev, addr);
		port->mac_offset = addr[ETH_ALEN - 1] -
				   port->ni->mac[ETH_ALEN - 1];
	} else {
		eth_hw_addr_set(port->dev, port->ni->mac);
		port->mac_offset = 0;
	}

	rtl_ni_port_set_mac(port);
	return 0;
}

static void rtl_ni_port_mac(struct rtl9617c_ni_port *port)
{
	u8 mac[ETH_ALEN];

	ether_addr_copy(mac, port->ni->mac);
	eth_addr_add(mac, port->mac_offset);
	eth_hw_addr_set(port->dev, mac);
}

static int rtl_ni_set_mac_address(struct net_device *dev, void *addr)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	struct rtl9617c_ni *ni = port->ni;
	struct sockaddr *sa = addr;
	unsigned int id;
	u64 val;

	if (!is_valid_ether_addr(sa->sa_data))
		return -EADDRNOTAVAIL;

	val = ether_addr_to_u64(sa->sa_data) - port->mac_offset;
	u64_to_ether_addr(val & GENMASK_ULL(47, 0), ni->mac);
	rtl_ni_set_mac(ni);
	for (id = 0; id < RTL_NI_PORTS; id++) {
		struct rtl9617c_ni_port *p = ni->ports[id];

		if (!p)
			continue;
		rtl_ni_port_mac(p);
		if (rtl_ni_is_ge(id))
			rtl_ni_port_set_mac(p);
		if (p != port && p->registered)
			call_netdevice_notifiers(NETDEV_CHANGEADDR, p->dev);
	}
	return 0;
}

static void rtl_ni_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	netdev_err(dev, "transmit timeout\n");
	schedule_work(&port->ni->tx_timeout_work);
}

static int rtl_ni_setup_tc(struct net_device *dev, enum tc_setup_type type,
			   void *data)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	int err;

	if (type == TC_SETUP_FT)
		return rtl_fe_setup_tc(port->ni->fe, dev, type, data);

	err = rtl_ni_qos_setup_tc(port, type, data);
	if (err != -EOPNOTSUPP)
		return err;

	return rtl_fe_setup_tc(port->ni->fe, dev, type, data);
}

static int rtl_ni_eth_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	return phylink_mii_ioctl(port->phylink, ifr, cmd);
}

static int rtl_ni_port_open(struct net_device *dev)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	int ret;

	if (rtl_ni_is_ge(port->id) || port->id == RTL_NI_PORT_P6) {
		dev->hw_features |= NETIF_F_HW_TC | NETIF_F_HW_VLAN_CTAG_FILTER;
		dev->features |= NETIF_F_HW_TC | NETIF_F_HW_VLAN_CTAG_FILTER;
	}

	ret = rtl_ni_open(dev);
	if (!ret) {
		rtl_ni_fc_ext_init(port->ni);
		rtl_ni_l2fe_register_ni(port->ni);
	}

	return ret;
}

static netdev_tx_t rtl_ni_port_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	struct sk_buff *ts_skb = NULL;
	netdev_tx_t ret;

	if (unlikely((skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) &&
		     skb->len >= ETH_HLEN))
		ts_skb = rtl_ni_ptp_tx_prepare(port, skb);
	ret = rtl_ni_start_xmit(skb, dev);
	if (unlikely(ts_skb)) {
		if (ret == NETDEV_TX_OK)
			rtl_ni_ptp_tx_commit(port, ts_skb);
		else
			dev_kfree_skb_any(ts_skb);
	}
	return ret;
}

static netdev_features_t rtl_ni_fix_features(struct net_device *dev,
					     netdev_features_t features)
{
	return (features & ~RTL9617C_NI_TX_OFFLOAD_FEATURES) |
	       (features & rtl9617c_ni_tx_offload_features());
}

static int rtl_ni_set_features(struct net_device *dev,
			       netdev_features_t features)
{
	netdev_features_t changed = dev->features ^ features;

	if (changed & NETIF_F_LOOPBACK) {
		bool enable = !!(features & NETIF_F_LOOPBACK);
		int speed = 0;

		if (!dev->phydev)
			return -EOPNOTSUPP;
		if (enable && !dev->phydev->link)
			speed = SPEED_1000;
		return phy_loopback(dev->phydev, enable, speed);
	}

	return 0;
}

/*
 * Linux's skb_gso_ok() checks gso_type/fraglist/nr_frags but not skb->len against
 * dev->tso_max_size. Force software GSO segmentation (or linearize) whenever an
 * skb exceeds hardware SG/LSO limits rather than dropping in start_xmit().
 */
static netdev_features_t rtl_ni_features_check(struct sk_buff *skb,
					       struct net_device *dev,
					       netdev_features_t features)
{
	if (skb_has_frag_list(skb))
		features &= ~(NETIF_F_GSO_MASK | NETIF_F_SG);
	if (skb_shinfo(skb)->nr_frags > MAX_SKB_FRAGS)
		features &= ~NETIF_F_GSO_MASK;
	if (skb_is_gso(skb) &&
	    (skb->encapsulation || skb->len > rtl_ni_tx_gso_max()))
		features &= ~NETIF_F_GSO_MASK;
	return features;
}

static const struct net_device_ops rtl_ni_netdev_ops = {
	.ndo_open = rtl_ni_port_open,
	.ndo_stop = rtl_ni_stop,
	.ndo_start_xmit = rtl_ni_port_xmit,
	.ndo_fix_features = rtl_ni_fix_features,
	.ndo_set_features = rtl_ni_set_features,
	.ndo_features_check = rtl_ni_features_check,
	.ndo_set_mac_address = rtl_ni_set_mac_address,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_tx_timeout = rtl_ni_tx_timeout,
	.ndo_get_stats64 = rtl_ni_get_stats64,
	.ndo_hwtstamp_get = rtl_ni_ptp_hwtstamp_get,
	.ndo_hwtstamp_set = rtl_ni_ptp_hwtstamp_set,
	.ndo_vlan_rx_add_vid = rtl_ni_l2fe_vlan_rx_add,
	.ndo_vlan_rx_kill_vid = rtl_ni_l2fe_vlan_rx_kill,
	.ndo_setup_tc = rtl_ni_setup_tc,
	.ndo_eth_ioctl = rtl_ni_eth_ioctl,
};

static int rtl_ni_get_link_ksettings(struct net_device *dev,
				     struct ethtool_link_ksettings *cmd)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	return phylink_ethtool_ksettings_get(port->phylink, cmd);
}

static int rtl_ni_set_link_ksettings(struct net_device *dev,
				     const struct ethtool_link_ksettings *cmd)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	return phylink_ethtool_ksettings_set(port->phylink, cmd);
}

static int rtl_ni_nway_reset(struct net_device *dev)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	return phylink_ethtool_nway_reset(port->phylink);
}

static void rtl_ni_get_pauseparam(struct net_device *dev,
				  struct ethtool_pauseparam *pause)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	phylink_ethtool_get_pauseparam(port->phylink, pause);
}

static int rtl_ni_set_pauseparam(struct net_device *dev,
				 struct ethtool_pauseparam *pause)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	return phylink_ethtool_set_pauseparam(port->phylink, pause);
}

static const struct ethtool_ops rtl_ni_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = rtl_ni_get_link_ksettings,
	.set_link_ksettings = rtl_ni_set_link_ksettings,
	.nway_reset = rtl_ni_nway_reset,
	.get_pauseparam = rtl_ni_get_pauseparam,
	.set_pauseparam = rtl_ni_set_pauseparam,
	.get_sset_count = rtl_ni_get_sset_count,
	.get_strings = rtl_ni_get_strings,
	.get_ethtool_stats = rtl_ni_get_ethtool_stats,
	.get_ts_info = rtl_ni_ptp_get_ts_info,
	.get_eee = rtl_ni_eee_get,
	.set_eee = rtl_ni_eee_set,
};

static int rtl_ni_port_phy_connect(struct rtl9617c_ni_port *port)
{
	struct device_node *phy_np, *bus_np;
	struct mii_bus *bus;
	int ret;

	ret = phylink_of_phy_connect(port->phylink, port->np, 0);
	if (ret != -ENODEV)
		return ret;

	phy_np = of_parse_phandle(port->np, "phy-handle", 0);
	if (!phy_np)
		return ret;
	bus_np = of_get_parent(phy_np);
	of_node_put(phy_np);
	bus = bus_np ? of_mdio_find_bus(bus_np) : NULL;
	of_node_put(bus_np);
	if (!bus)
		return -EPROBE_DEFER;
	put_device(&bus->dev);
	return ret;
}

static void rtl_ni_port_destroy(struct rtl9617c_ni_port *port)
{
	struct rtl9617c_ni *ni = port->ni;

	if (port->registered)
		unregister_netdev(port->dev);
	rtl_fe_remove_port(ni->fe, port->dev);
	WRITE_ONCE(ni->ports[port->id], NULL);
	RCU_INIT_POINTER(ni->rx_dev[port->id], NULL);
	synchronize_net();
	if (port->serdes && ni->serdes_on) {
		phy_power_off(port->serdes);
		ni->serdes_on = false;
		ni->serdes_ready = false;
	}
	if (port->phylink) {
		rtnl_lock();
		phylink_disconnect_phy(port->phylink);
		rtnl_unlock();
		phylink_destroy(port->phylink);
	}
	of_node_put(port->np);
	free_netdev(port->dev);
}

/*
 * Advertise TX offloads from the module parameters in ni_dma.c (tx_csum,
 * tso, tso_v6, tx_sg, tx_multivp).  When tx_sg=1 (default), multi-descriptor
 * DMA LSO handles true scatter-gather super-packets up to 65500 B across 4
 * per-CPU DMA VPs; when tx_sg=0, super-packets are capped at RTL_NI_TX_GSO_MAX
 * (2032 B) for the single-descriptor linear bounce path.
 */
static void rtl_ni_port_offload(struct net_device *dev)
{
	unsigned int txqs = READ_ONCE(rtl_ni_tx_multivp) ? RTL_NI_TX_RINGS : 1;

	dev->hw_features |= RTL9617C_NI_TX_OFFLOAD_FEATURES;
	dev->wanted_features |= RTL9617C_NI_TX_OFFLOAD_FEATURES;
	dev->features |= rtl_ni_fix_features(dev, RTL9617C_NI_TX_OFFLOAD_FEATURES);
	netif_set_tso_max_size(dev, rtl_ni_tx_gso_max());
	netif_set_tso_max_segs(dev, MAX_SKB_FRAGS + 1);
	netif_set_real_num_tx_queues(dev, txqs);
}

void rtl_ni_ports_update_features(void)
{
	netdev_features_t wanted = rtl9617c_ni_tx_offload_features();
	unsigned int gso_max = rtl_ni_tx_gso_max();
	unsigned int txqs = READ_ONCE(rtl_ni_tx_multivp) ? RTL_NI_TX_RINGS : 1;
	struct rtl9617c_ni *ni = NULL;
	struct net_device *dev;

	if (system_state < SYSTEM_RUNNING)
		return;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if (dev->netdev_ops == &rtl_ni_netdev_ops) {
			if (!ni)
				ni = ((struct rtl9617c_ni_port *)netdev_priv(dev))->ni;
			netif_set_real_num_tx_queues(dev, txqs);
			netif_set_tso_max_size(dev, gso_max);
			netif_set_tso_max_segs(dev, MAX_SKB_FRAGS + 1);
			dev->wanted_features =
				(dev->wanted_features & ~RTL9617C_NI_TX_OFFLOAD_FEATURES) |
				wanted;
			netdev_update_features(dev);
		}
	}
	rtl_ni_notify_pon_features(ni);
	rtnl_unlock();
}

void rtl_ni_ports_rss_apply(void)
{
	struct rtl9617c_ni *ni = NULL;
	struct net_device *dev;

	if (system_state < SYSTEM_RUNNING)
		return;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if (dev->netdev_ops == &rtl_ni_netdev_ops) {
			ni = ((struct rtl9617c_ni_port *)netdev_priv(dev))->ni;
			break;
		}
	}
	if (ni) {
		mutex_lock(&ni->lock);
		if (ni->qm.ready)
			rtl_ni_qm_rss_apply(ni);
		mutex_unlock(&ni->lock);
	}
	rtnl_unlock();
}

static int rtl_ni_port_create(struct rtl9617c_ni *ni, struct device_node *np)
{
	struct rtl9617c_ni_port *port;
	struct phylink_config *config;
	phy_interface_t interface;
	struct net_device *dev;
	const char *label;
	unsigned int i;
	u32 id;
	int ret;

	ret = of_property_read_u32(np, "reg", &id);
	if (ret)
		return ret;
	if (id >= RTL_NI_PORTS || id == RTL_NI_GE_PORTS || ni->ports[id])
		return -EINVAL;
	ret = of_get_phy_mode(np, &interface);
	if (ret)
		return ret;

	dev = alloc_etherdev_mq(sizeof(*port), RTL_NI_TX_RINGS);
	if (!dev)
		return -ENOMEM;
	SET_NETDEV_DEV(dev, ni->dev);
	dev->dev.of_node = np;

	port = netdev_priv(dev);
	port->ni = ni;
	port->dev = dev;
	port->np = of_node_get(np);
	port->id = id;
	port->interface = interface;

	if (!of_property_read_string(np, "label", &label))
		strscpy(dev->name, label, sizeof(dev->name));
	dev->netdev_ops = &rtl_ni_netdev_ops;
	dev->ethtool_ops = &rtl_ni_ethtool_ops;
	dev->max_mtu = RTL_NI_PORT_MAX_MTU;
	dev->hw_features |= NETIF_F_RXCSUM;
	if (rtl_ni_is_ge(id))
		dev->hw_features |= NETIF_F_LOOPBACK;
	dev->features |= NETIF_F_RXCSUM;
	rtl_ni_port_offload(dev);
	ret = rtl_ni_port_mac_probe(port);
	if (ret)
		goto err;

	port->serdes = devm_of_phy_optional_get(ni->dev, np, NULL);
	if (IS_ERR(port->serdes)) {
		ret = PTR_ERR(port->serdes);
		port->serdes = NULL;
		goto err;
	}

	if (port->serdes) {
		for (i = 0; i < RTL_NI_PORTS; i++) {
			if (ni->ports[i] && ni->ports[i]->serdes) {
				dev_warn(ni->dev,
					 "%s and %s share the serdes lane\n",
					 ni->ports[i]->dev->name, dev->name);
				break;
			}
		}
	}

	config = &port->phylink_config;
	config->dev = &dev->dev;
	config->type = PHYLINK_NETDEV;
	config->mac_capabilities = MAC_SYM_PAUSE | MAC_ASYM_PAUSE | MAC_10 |
				   MAC_100 | MAC_1000;
	if (port->serdes || !rtl_ni_is_ge(id)) {
		config->mac_capabilities |= MAC_2500FD;
		__set_bit(PHY_INTERFACE_MODE_SGMII, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);
	} else {
		__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
		config->lpi_capabilities = MAC_100FD | MAC_1000FD;
		__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->lpi_interfaces);
		__set_bit(PHY_INTERFACE_MODE_GMII, config->lpi_interfaces);
		config->lpi_timer_default = 250000;
	}

	port->phylink = phylink_create(config, of_fwnode_handle(np), interface,
				       &rtl_ni_phylink_ops);
	if (IS_ERR(port->phylink)) {
		ret = PTR_ERR(port->phylink);
		port->phylink = NULL;
		goto err;
	}

	ret = rtl_ni_port_phy_connect(port);
	if (ret)
		goto err;

	ret = rtl_fe_add_port(ni->fe, dev, id, id, 0);
	if (ret)
		goto err;

	WRITE_ONCE(ni->ports[id], port);
	rcu_assign_pointer(ni->rx_dev[id], dev);
	return 0;

err:
	rtl_ni_port_destroy(port);
	return ret;
}

int rtl_ni_ports_register(struct rtl9617c_ni *ni)
{
	struct device_node *ports, *np;
	unsigned int id;
	int ret = 0;

	ports = of_get_child_by_name(ni->dev->of_node, "ethernet-ports");
	if (!ports)
		return -ENODEV;

	for_each_available_child_of_node(ports, np) {
		if (!of_node_name_eq(np, "port"))
			continue;
		ret = rtl_ni_port_create(ni, np);
		if (ret == -EPROBE_DEFER) {
			of_node_put(np);
			of_node_put(ports);
			rtl_ni_ports_unregister(ni);
			return ret;
		}
		if (ret)
			dev_err(ni->dev, "%pOF: port setup failed: %d\n", np, ret);
		ret = 0;
	}
	of_node_put(ports);

	for (id = 0; id < RTL_NI_PORTS; id++) {
		struct rtl9617c_ni_port *port = ni->ports[id];

		if (!port)
			continue;
		ret = register_netdev(port->dev);
		if (ret) {
			dev_err(ni->dev, "port %u register_netdev failed: %d\n",
				id, ret);
			rtl_ni_port_destroy(port);
			ret = 0;
			continue;
		}
		port->registered = true;
	}

	return ret;
}

void rtl_ni_ports_unregister(struct rtl9617c_ni *ni)
{
	unsigned int id;

	for (id = 0; id < RTL_NI_PORTS; id++)
		if (ni->ports[id])
			rtl_ni_port_destroy(ni->ports[id]);
}
