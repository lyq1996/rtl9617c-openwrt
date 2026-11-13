/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>

#include "rtl9617c-dma.h"
#include "rtl9617c-fe.h"
#include "rtl9617c-led.h"
#include "rtl9617c-ni-priv.h"
#include "rtl9617c-ni.h"

void rtl_mac_address(struct rtl9617c_ni *priv)
{
	const u8 *mac = priv->base_mac;

	rtl_ni_write(priv->regs, RTL_NI_MAC_ADDR0, get_unaligned_be32(mac));
	rtl_update(priv, RTL_NI_MAC_ADDR1, 0xff, mac[4]);
	rtl_update(priv, RTL_NI_PORT_STATIC + priv->port * RTL_NI_PORT_STRIDE,
		   GENMASK(31, 24), (u32)mac[5] << 24);
}

#define RTL_LAN_PORT_MAX	6

static unsigned int rtl_lan_name(unsigned int port)
{
	if (port == 0)
		return 0;
	if (port == 6)
		return 1;
	return port + 1;
}

static unsigned int rtl_lan_offset(unsigned int port)
{
	return port + 1;
}

static bool rtl_lan_base(const u8 *mac, unsigned int port, u8 *base)
{
	unsigned int offset = rtl_lan_offset(port);
	u64 value = 0;
	int i;

	for (i = 0; i < ETH_ALEN; i++)
		value = value << 8 | mac[i];
	if (value < offset)
		return false;
	value -= offset;
	for (i = ETH_ALEN - 1; i >= 0; i--) {
		base[i] = value & 0xff;
		value >>= 8;
	}
	return base[ETH_ALEN - 1] <= 0xff - rtl_lan_offset(RTL_LAN_PORT_MAX);
}

static void rtl_lan_address(const u8 *base, unsigned int port, u8 *mac)
{
	ether_addr_copy(mac, base);
	eth_addr_add(mac, rtl_lan_offset(port));
}

static int rtl_set_mac_address(struct net_device *dev, void *addr)
{
	struct rtl9617c_port *lan = netdev_priv(dev);
	struct rtl9617c_ni *priv = lan->shared;
	struct sockaddr *sa = addr;
	u8 base[ETH_ALEN];
	unsigned int port;

	if (!is_valid_ether_addr(sa->sa_data))
		return -EADDRNOTAVAIL;
	if (!rtl_lan_base(sa->sa_data, lan->port, base))
		return -ERANGE;
	ether_addr_copy(priv->base_mac, base);
	for (port = 0; port < ARRAY_SIZE(priv->ports); port++) {
		struct net_device *lan_dev = priv->ports[port];
		u8 mac[ETH_ALEN];

		if (!lan_dev)
			continue;
		rtl_lan_address(base, port, mac);
		eth_hw_addr_set(lan_dev, mac);
		if (port < 5)
			rtl_update(priv, RTL_NI_PORT_STATIC + port * RTL_NI_PORT_STRIDE,
				   GENMASK(31, 24), (u32)mac[5] << 24);
	}
	rtl_mac_address(priv);
	return 0;
}

static void rtl_mac_config(struct phylink_config *config, unsigned int mode,
			   const struct phylink_link_state *state)
{
}

static void rtl_mac_link_up(struct phylink_config *config, struct phy_device *phy,
			    unsigned int mode, phy_interface_t interface,
			    int speed, int duplex, bool tx_pause, bool rx_pause)
{
	struct net_device *dev = to_net_dev(config->dev);
	struct rtl9617c_port *lan = netdev_priv(dev);

	if (lan->port >= 5) {
		if (lan->shared->p6_serdes && lan->shared->sgmii_speed != speed)
			rtl_p6_set_speed(lan->shared, speed);
		phy_print_status(phy);
		return;
	}
	rtl_update(lan->shared, RTL_NI_PORT_STATIC + lan->port * RTL_NI_PORT_STRIDE,
		   GENMASK(3, 0), speed >= SPEED_1000 ? 0 : 1);
	rtl_update(lan->shared, RTL_NI_PORT_GLB + lan->port * RTL_NI_PORT_STRIDE,
		   BIT(0) | BIT(1), (speed == SPEED_10 ? BIT(0) : 0) |
		   (duplex == DUPLEX_HALF ? BIT(1) : 0));
	phy_print_status(phy);
}

static void rtl_mac_link_down(struct phylink_config *config, unsigned int mode,
			      phy_interface_t interface)
{
}

static const struct phylink_mac_ops rtl_phylink_ops = {
	.mac_config = rtl_mac_config,
	.mac_link_up = rtl_mac_link_up,
	.mac_link_down = rtl_mac_link_down,
};

void rtl9617c_ni_set_rx_mode(struct rtl9617c_ni *priv)
{
	struct net_device *dev = priv->netdev;
	bool accept_all = dev && ((dev->flags & (IFF_PROMISC | IFF_ALLMULTI)) ||
				  !netdev_mc_empty(dev));

	if (priv->qm_rx)
		return;

	rtl_update(priv, RTL_NI_XRAM_CFG, RTL_NI_XRAM_RX_PROMISC,
		   FIELD_PREP(RTL_NI_XRAM_RX_PROMISC, accept_all));
}

void rtl9617c_ni_rx_mode(struct net_device *dev)
{
	rtl9617c_ni_set_rx_mode(rtl_shared(dev));
}



static int rtl_setup_tc(struct net_device *netdev, enum tc_setup_type type,
			void *data)
{
	struct rtl9617c_ni *priv = rtl_shared(netdev);

	return rtl_fe_setup_tc(priv->fe, netdev, type, data);
}
static void rtl_get_stats64(struct net_device *netdev, struct rtnl_link_stats64 *stats)
{
	netdev_stats_to_stats64(stats, &netdev->stats);
}

static void rtl_tx_timeout(struct net_device *netdev, unsigned int queue)
{
	struct rtl9617c_ni *priv = rtl_shared(netdev);

	if (queue)
		return;
	netdev_err(netdev, "transmit timeout\n");
	if (priv->dma_tx) {
		rtl_dma_tx_stop(&priv->dma);
		rtl_dma_tx_start(&priv->dma);
	}
	netif_trans_update(netdev);
	rtl9617c_ni_wake_ports(priv);
}

static int rtl_open(struct net_device *dev)
{
	struct rtl9617c_port *lan = netdev_priv(dev);
	struct rtl9617c_ni *priv = lan->shared;
	int ret = rtl9617c_ni_engine_get(priv);

	if (ret)
		return ret;

	if (lan->port < 5) {
		rtl_update(priv, RTL_NI_GLB_INTF_RST_CONFIG, BIT(lan->port) | BIT(lan->port + 6) |
			   BIT(lan->port + 12), 0);
		rtl_update(priv, RTL_NI_PORT_STATIC + lan->port * RTL_NI_PORT_STRIDE,
			   GENMASK(31, 24), (u32)dev->dev_addr[5] << 24);
		rtl_update(priv, RTL_NI_PORT_RX + lan->port * RTL_NI_PORT_STRIDE, BIT(0), BIT(0));
		rtl_update(priv, RTL_NI_PORT_TX + lan->port * RTL_NI_PORT_STRIDE, BIT(0), BIT(0));
	}
	phylink_start(lan->phylink);
	netif_start_queue(dev);
	return 0;
}

static int rtl_stop(struct net_device *dev)
{
	struct rtl9617c_port *lan = netdev_priv(dev);
	struct rtl9617c_ni *priv = lan->shared;
	unsigned long flags;

	netif_stop_queue(dev);
	phylink_stop(lan->phylink);
	if (lan->port < 5) {
		rtl_update(priv, RTL_NI_PORT_RX + lan->port * RTL_NI_PORT_STRIDE, BIT(0), 0);
		rtl_update(priv, RTL_NI_PORT_TX + lan->port * RTL_NI_PORT_STRIDE, BIT(0), 0);
	}
	rtl_fe_flush_port(priv->fe, dev);
	if (priv->dma_tx) {
		spin_lock_irqsave(&priv->tx_lock, flags);
		rtl_dma_tx_forget(&priv->dma, dev);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
	}
	rtl9617c_ni_engine_put(priv);
	return 0;
}

const struct net_device_ops rtl_netdev_ops = {
	.ndo_tx_timeout = rtl_tx_timeout,
	.ndo_get_stats64 = rtl_get_stats64,
	.ndo_setup_tc = rtl_setup_tc,
	.ndo_open = rtl_open,
	.ndo_stop = rtl_stop,
	.ndo_start_xmit = rtl9617c_ni_xmit,
	.ndo_set_rx_mode = rtl9617c_ni_rx_mode,
	.ndo_set_mac_address = rtl_set_mac_address,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_eth_ioctl = phy_do_ioctl_running,
};

static int rtl_get_link_ksettings(struct net_device *dev,
				  struct ethtool_link_ksettings *cmd)
{
	struct rtl9617c_port *lan = netdev_priv(dev);

	return phylink_ethtool_ksettings_get(lan->phylink, cmd);
}

static int rtl_set_link_ksettings(struct net_device *dev,
				  const struct ethtool_link_ksettings *cmd)
{
	struct rtl9617c_port *lan = netdev_priv(dev);

	return phylink_ethtool_ksettings_set(lan->phylink, cmd);
}

static int rtl_nway_reset(struct net_device *dev)
{
	struct rtl9617c_port *lan = netdev_priv(dev);

	return phylink_ethtool_nway_reset(lan->phylink);
}

const struct ethtool_ops rtl_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = rtl_get_link_ksettings,
	.set_link_ksettings = rtl_set_link_ksettings,
	.nway_reset = rtl_nway_reset,
};

void rtl_unregister_lan(struct rtl9617c_ni *priv, unsigned int port)
{
	struct net_device *dev = priv->ports[port];
	struct rtl9617c_port *lan;
	unsigned long flags;

	if (!dev)
		return;
	lan = netdev_priv(dev);
	if (port == 6 && priv->phy_led_registered) {
		unsigned int ch;

		for (ch = 0; ch < RTL_PHY_LED_COUNT; ch++)
			rtl9617c_led_unregister(&priv->phy_led[ch]);
		priv->phy_led_registered = false;
	}
	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->ports[port] = NULL;
	if (priv->tx_dev == dev)
		priv->tx_dev = NULL;
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	rcu_assign_pointer(priv->qm.lan[port], NULL);
	if (priv->registered_ports & BIT(port))
		unregister_netdev(dev);
	rtl_fe_remove_port(priv->fe, dev);
	synchronize_net();
	if (lan->phylink) {
		phylink_disconnect_phy(lan->phylink);
		phylink_destroy(lan->phylink);
		lan->phylink = NULL;
	}
	if (dev == priv->netdev)
		priv->netdev = NULL;
	free_netdev(dev);
	priv->registered_ports &= ~BIT(port);
}

static int rtl_phy_missing(struct device_node *phy)
{
	struct device_node *bus_np = of_get_parent(phy);
	struct mii_bus *bus = bus_np ? of_mdio_find_bus(bus_np) : NULL;

	of_node_put(bus_np);
	if (!bus)
		return -EPROBE_DEFER;
	put_device(&bus->dev);
	return -ENODEV;
}

static int rtl_register_lan(struct rtl9617c_ni *priv, struct device *parent,
			    unsigned int port, struct device_node *phy)
{
	struct phy_device *phydev;
	struct net_device *dev = NULL;
	struct rtl9617c_port *lan;
	unsigned int index;
	u8 mac[ETH_ALEN];
	int ret;

	dev = alloc_etherdev(sizeof(*lan));
	if (!dev) {
		ret = -ENOMEM;
		goto put_phy;
	}
	priv->ports[port] = dev;
	if (port == priv->port)
		priv->netdev = dev;
	SET_NETDEV_DEV(dev, parent);
	index = rtl_lan_name(port);
	snprintf(dev->name, sizeof(dev->name), "lan%u", index);
	lan = netdev_priv(dev);
	lan->shared = priv;
	lan->port = port;
	dev->netdev_ops = &rtl_netdev_ops;
	dev->ethtool_ops = &rtl_ethtool_ops;
	dev->max_mtu = ETH_DATA_LEN;
	rtl_lan_address(priv->base_mac, port, mac);
	eth_hw_addr_set(dev, mac);
	lan->phylink_config.dev = &dev->dev;
	lan->phylink_config.type = PHYLINK_NETDEV;
	lan->phylink_config.mac_capabilities = MAC_10 | MAC_100 | MAC_1000 |
					       MAC_SYM_PAUSE | MAC_ASYM_PAUSE;
	__set_bit(port == 6 ? PHY_INTERFACE_MODE_SGMII : PHY_INTERFACE_MODE_GMII,
		  lan->phylink_config.supported_interfaces);
	lan->phylink = phylink_create(&lan->phylink_config,
				      of_fwnode_handle(parent->of_node),
				      port == 6 ? PHY_INTERFACE_MODE_SGMII :
						  PHY_INTERFACE_MODE_GMII,
				      &rtl_phylink_ops);
	if (IS_ERR(lan->phylink)) {
		ret = PTR_ERR(lan->phylink);
		lan->phylink = NULL;
		goto put_phy;
	}
	phydev = of_phy_find_device(phy);
	if (!phydev) {
		ret = rtl_phy_missing(phy);
		goto put_phy;
	}
	ret = phylink_connect_phy(lan->phylink, phydev);
	put_device(&phydev->mdio.dev);
	if (ret)
		goto put_phy;
	ret = rtl_fe_add_port(priv->fe, dev, port, port, 0);
	if (ret)
		goto put_phy;
	rcu_assign_pointer(priv->qm.lan[port], dev);
	ret = register_netdevice(dev);
	if (ret)
		goto put_phy;
	WRITE_ONCE(priv->registered_ports, priv->registered_ports | BIT(port));
	if (port == 6) {
		unsigned int ch;
		bool any_ok = false;

		for (ch = 0; ch < RTL_PHY_LED_COUNT; ch++) {
			struct rtl9617c_led *led = &priv->phy_led[ch];

			memset(led, 0, sizeof(*led));
			led->output = rtl9617c_ni_led_output;
			led->drvdata = priv;
			led->channel = ch;
			led->requested = 255;
			led->cdev.name = devm_kasprintf(parent, GFP_KERNEL,
							"LED_8221B_P6_%u", ch);
			if (!led->cdev.name)
				continue;
			led->cdev.max_brightness = 255;
			ret = rtl9617c_led_register(parent, led, NULL);
			if (ret) {
				dev_warn(parent, "LED_8221B_P6_%u registration failed: %d\n",
					 ch, ret);
				continue;
			}
			any_ok = true;
		}
		priv->phy_led_registered = any_ok;
	}
	dev_info(parent, "%s: LAN port %u %s, PHY %08x, MAC %pM\n",
		 dev->name, port, priv->qm_rx ? "native NI/DMA" : "management XRAM",
		 dev->phydev->phy_id, dev->dev_addr);
	of_node_put(phy);
	return 0;

put_phy:
	of_node_put(phy);
	if (dev)
		rtl_unregister_lan(priv, port);
	return ret;
}

int rtl_register_lans(struct rtl9617c_ni *priv, struct device *parent)
{
	struct device_node *phy;
	unsigned int port;
	int ret = 0;

	rtnl_lock();
	for (port = 0; port < 5; port++) {
		if (!priv->dma_tx && port != priv->port)
			continue;
		phy = of_parse_phandle(parent->of_node,
			port == priv->port ? "phy-handle" : "realtek,lan-phys",
			port == priv->port ? 0 : port);
		if (!phy) {
			dev_warn(parent, "LAN port %u has no %s; not registered\n", port,
				 port == priv->port ? "phy-handle" : "realtek,lan-phys");
			continue;
		}
		ret = rtl_register_lan(priv, parent, port, phy);
		if (ret == -EPROBE_DEFER)
			goto unlock;
		if (ret)
			dev_err(parent, "LAN%u initialization failed: %d; other ports retained\n",
				port + 1, ret);
	}
	phy = of_parse_phandle(parent->of_node, "realtek,p6-phy", 0);
	if (phy) {
		ret = rtl_register_lan(priv, parent, 6, phy);
		if (ret == -EPROBE_DEFER)
			goto unlock;
		if (ret)
			dev_err(parent, "P6 initialization failed: %d\n", ret);
	}
	ret = 0;
unlock:
	rtnl_unlock();
	if (ret)
		for (port = 0; port < ARRAY_SIZE(priv->ports); port++)
			rtl_unregister_lan(priv, port);
	return ret;
}
