// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy/phy.h>
#include <linux/phylink.h>
#include <linux/rtnetlink.h>

#include "ni.h"
#include "../fe/fe.h"

#define PORT_STATIC_MAC			GENMASK(31, 24)
#define PORT_STATIC_SPEED		GENMASK(3, 0)
#define PORT_GLB_10M			BIT(0)
#define PORT_GLB_HALF			BIT(1)
#define PORT_ENABLE			BIT(0)
#define TX_HDR_HIGH			BIT(13)
#define TX_HDR_LOW(port)		((16 << 9) | ((port) << 3) | BIT(29))

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

	if (port->id == RTL_NI_PORT_P6)
		rtl_ni_p6_set_interface(port->ni, state->interface);
}

static void rtl_ni_mac_link_up(struct phylink_config *config,
			       struct phy_device *phy, unsigned int mode,
			       phy_interface_t interface, int speed, int duplex,
			       bool tx_pause, bool rx_pause)
{
	struct rtl9617c_ni_port *port = rtl_ni_config_to_port(config);
	struct rtl9617c_ni *ni = port->ni;

	if (!rtl_ni_is_ge(port->id)) {
		if (port->serdes)
			phy_set_speed(port->serdes, speed);
		return;
	}

	rtl_ni_rmw(ni, RTL_NI_PORT_STATIC(port->id), PORT_STATIC_SPEED,
		   speed >= SPEED_1000 ? 0 : 1);
	rtl_ni_rmw(ni, RTL_NI_PORT_GLB(port->id), PORT_GLB_10M | PORT_GLB_HALF,
		   (speed == SPEED_10 ? PORT_GLB_10M : 0) |
		   (duplex == DUPLEX_HALF ? PORT_GLB_HALF : 0));
}

static void rtl_ni_mac_link_down(struct phylink_config *config, unsigned int mode,
				 phy_interface_t interface)
{
}

static const struct phylink_mac_ops rtl_ni_phylink_ops = {
	.mac_config = rtl_ni_mac_config,
	.mac_link_up = rtl_ni_mac_link_up,
	.mac_link_down = rtl_ni_mac_link_down,
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
	netif_start_queue(dev);
	if (READ_ONCE(ni->tx_stopped))
		netif_stop_queue(dev);
	return 0;
}

static int rtl_ni_stop(struct net_device *dev)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	struct rtl9617c_ni *ni = port->ni;

	netif_stop_queue(dev);
	phylink_stop(port->phylink);
	if (rtl_ni_is_ge(port->id)) {
		rtl_ni_rmw(ni, RTL_NI_PORT_RX(port->id), PORT_ENABLE, 0);
		rtl_ni_rmw(ni, RTL_NI_PORT_TX(port->id), PORT_ENABLE, 0);
	}
	rtl_fe_flush_port(ni->fe, dev);
	rtl_ni_engine_put(ni);
	return 0;
}

static netdev_tx_t rtl_ni_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	unsigned int egress = rtl_ni_is_ge(port->id) ? port->id : RTL_NI_PORT_P6;
	const struct rtl9617c_ni_frame frame = {
		.header_high = TX_HDR_HIGH,
		.header_low = TX_HDR_LOW(egress),
	};

	if (skb->len < ETH_HLEN || skb->len > RTL_NI_FRAME_MAX) {
		DEV_STATS_INC(dev, tx_dropped);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	return rtl_ni_xmit_frame(port->ni, dev, skb, &frame);
}

static unsigned int rtl_ni_mac_offset(unsigned int id)
{
	return id + 1;
}

static void rtl_ni_port_mac(struct rtl9617c_ni_port *port)
{
	u8 mac[ETH_ALEN];

	ether_addr_copy(mac, port->ni->mac);
	eth_addr_add(mac, rtl_ni_mac_offset(port->id));
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

	val = ether_addr_to_u64(sa->sa_data) - rtl_ni_mac_offset(port->id);
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
	rtl_ni_tx_restart(port->ni);
}

static int rtl_ni_setup_tc(struct net_device *dev, enum tc_setup_type type,
			   void *data)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	return rtl_fe_setup_tc(port->ni->fe, dev, type, data);
}

static int rtl_ni_eth_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	return phylink_mii_ioctl(port->phylink, ifr, cmd);
}

static const struct net_device_ops rtl_ni_netdev_ops = {
	.ndo_open = rtl_ni_open,
	.ndo_stop = rtl_ni_stop,
	.ndo_start_xmit = rtl_ni_start_xmit,
	.ndo_set_mac_address = rtl_ni_set_mac_address,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_tx_timeout = rtl_ni_tx_timeout,
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
	RCU_INIT_POINTER(ni->rx_dev[port->id], NULL);
	synchronize_net();
	ni->ports[port->id] = NULL;
	if (port->serdes && ni->serdes_on) {
		phy_power_off(port->serdes);
		ni->serdes_on = false;
		ni->p6_ready = false;
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

static int rtl_ni_port_create(struct rtl9617c_ni *ni, struct device_node *np)
{
	struct rtl9617c_ni_port *port;
	struct phylink_config *config;
	phy_interface_t interface;
	struct net_device *dev;
	const char *label;
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

	dev = alloc_etherdev(sizeof(*port));
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
	dev->max_mtu = ETH_DATA_LEN;
	dev->hw_features |= NETIF_F_RXCSUM;
	dev->features |= NETIF_F_RXCSUM;
	rtl_ni_port_mac(port);

	if (!rtl_ni_is_ge(id)) {
		port->serdes = devm_of_phy_optional_get(ni->dev, np, NULL);
		if (IS_ERR(port->serdes)) {
			ret = PTR_ERR(port->serdes);
			port->serdes = NULL;
			goto err;
		}
	}

	config = &port->phylink_config;
	config->dev = &dev->dev;
	config->type = PHYLINK_NETDEV;
	config->mac_capabilities = MAC_SYM_PAUSE | MAC_ASYM_PAUSE | MAC_10 |
				   MAC_100 | MAC_1000;
	if (rtl_ni_is_ge(id)) {
		__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
	} else {
		config->mac_capabilities |= MAC_2500FD;
		__set_bit(PHY_INTERFACE_MODE_SGMII, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);
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

	ni->ports[id] = port;
	rcu_assign_pointer(ni->rx_dev[id], dev);

	ret = register_netdev(dev);
	if (ret)
		goto err;
	port->registered = true;
	return 0;

err:
	rtl_ni_port_destroy(port);
	return ret;
}

int rtl_ni_ports_register(struct rtl9617c_ni *ni)
{
	struct device_node *ports, *np;
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
			break;
		}
		if (ret)
			dev_err(ni->dev, "%pOF: port setup failed: %d\n", np, ret);
		ret = 0;
	}
	of_node_put(ports);

	if (ret)
		rtl_ni_ports_unregister(ni);
	return ret;
}

void rtl_ni_ports_unregister(struct rtl9617c_ni *ni)
{
	unsigned int id;

	for (id = 0; id < RTL_NI_PORTS; id++)
		if (ni->ports[id])
			rtl_ni_port_destroy(ni->ports[id]);
}
