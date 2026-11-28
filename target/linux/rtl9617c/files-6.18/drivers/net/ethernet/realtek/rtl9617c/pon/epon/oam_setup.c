// SPDX-License-Identifier: GPL-2.0-or-later
/* EPON endpoint allocation, netlink redirect and module init/remove. */
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/netlink.h>
#include <linux/rcupdate.h>
#include <linux/rtnetlink.h>
#include <net/netlink.h>
#include <net/sock.h>
#include "pon/pon.h"
#include "pon/pon_priv.h"
#include "rtl9617c_ni.h"
#include "pon/epon/epon.h"


static struct net_device *rtl_oam_allocate(struct rtl9617c_pon *pon, unsigned int index, bool control)
{
	struct net_device *dev = alloc_etherdev(sizeof(struct rtl_oam_port));
	struct rtl_oam_port *port;
	const u8 *base = pon->onu_mac;
	u8 mac[ETH_ALEN];

	if (!dev)
		return NULL;
	port = netdev_priv(dev);
	port->pon = pon;
	port->llid = index;
	port->control = control;
	SET_NETDEV_DEV(dev, pon->dev);
	/* llid_count is capped at RTL_EPON_MAX_LLIDS, so mask the name index to
	 * keep the formatted name provably inside IFNAMSIZ. */
	index &= RTL_EPON_MAX_LLIDS - 1;
	snprintf(dev->name, sizeof(dev->name), control ? "pon-oam%u" : "pon%u", index);
	dev->netdev_ops = control ? &rtl_oam_ops : &rtl_data_ops;
	dev->sysfs_groups[0] = &rtl_oam_group;
	dev->max_mtu = control ? ETH_DATA_LEN : 1536;
	if (!control) {
		netdev_features_t offload = rtl9617c_ni_tx_offload_features();

		dev->hw_features |= NETIF_F_RXCSUM |
				    RTL9617C_NI_TX_OFFLOAD_FEATURES;
		dev->wanted_features |= RTL9617C_NI_TX_OFFLOAD_FEATURES;
		dev->features |= NETIF_F_RXCSUM | offload;
		netif_set_tso_max_size(dev, rtl9617c_ni_tx_gso_max());
		netif_set_tso_max_segs(dev, MAX_SKB_FRAGS + 1);
	}
	if (control)
		dev->flags |= IFF_NOARP;
	/* Factory identity is supplied before netifd. Until then the zero
	 * address keeps the interface down instead of publishing a random MAC. */
	ether_addr_copy(mac, base);
	if (is_valid_ether_addr(base))
		eth_addr_add(mac, index);
	eth_hw_addr_set(dev, mac);
	netif_carrier_off(dev);
	if(!control && rtl9617c_ni_fe_add_port(pon->ni_dev,dev,index)) {
		free_netdev(dev);
		return NULL;
	}
	return dev;
}
static void rtl_oam_free(struct net_device *dev)
{
	struct rtl_oam_port *port=netdev_priv(dev);
	if(!port->control)rtl9617c_ni_fe_remove_port(port->pon->ni_dev,dev);
	free_netdev(dev);
}

int rtl_pon_oam_resize(struct rtl9617c_pon *pon, unsigned int count)
{
	struct net_device *created[2][32] = {};
	struct net_device __rcu **devices;
	unsigned int role, i;
	int ret = 0;

	rtnl_lock();
	for (role = 0; role < 2; role++) {
		devices = role ? pon->data_devices : pon->oam_devices;
		for (i = 0; i < count; i++) {
			if (rtnl_dereference(devices[i]))
				continue;
			created[role][i] = rtl_oam_allocate(pon, i, role == 0);
			if (!created[role][i]) {
				ret = -ENOMEM;
				goto undo;
			}
			ret = register_netdevice(created[role][i]);
			if (ret) {
				rtl_oam_free(created[role][i]);
				created[role][i] = NULL;
				goto undo;
			}
			rcu_assign_pointer(devices[i], created[role][i]);
		}
	}
	for (role = 0; role < 2; role++) {
		devices = role ? pon->data_devices : pon->oam_devices;
		for (i = count; i < 32; i++) {
			struct net_device *dev = rtnl_dereference(devices[i]);

			if (!dev)
				continue;
			rcu_assign_pointer(devices[i], NULL);
			synchronize_net();
			unregister_netdevice(dev);
			rtl_oam_free(dev);
		}
	}
	rtnl_unlock();
	return 0;
undo:
	for (role = 0; role < 2; role++) {
		devices = role ? pon->data_devices : pon->oam_devices;
		for (i = 0; i < count; i++) {
			if (!created[role][i])
				continue;
			rcu_assign_pointer(devices[i], NULL);
			synchronize_net();
			unregister_netdevice(created[role][i]);
			rtl_oam_free(created[role][i]);
		}
	}
	rtnl_unlock();
	return ret;
}

int rtl_pon_oam_init(struct rtl9617c_pon *pon)
{
	struct net_device *dev = rtl_oam_allocate(pon, 0, true);
	int ret;

	if (!dev)
		return -ENOMEM;
	pon->oam_endpoint.netdev = dev;
	pon->oam_endpoint.receive = rtl_oam_receive;
	pon->oam_endpoint.wake = rtl_oam_wake;
	pon->oam_endpoint.update_features = rtl_oam_update_features;
	rcu_assign_pointer(pon->oam_devices[0], dev);
	ret = rtl9617c_ni_pon_attach(pon->ni_dev, &pon->oam_endpoint);
	if (ret)
		goto free;
	ret = register_netdev(dev);
	if (ret) {
		rtl9617c_ni_pon_detach(pon->ni_dev);
		goto free;
	}
	ret = rtl_pon_oam_resize(pon, 1);
	if (ret) {
		rtl9617c_ni_pon_detach(pon->ni_dev);
		rcu_assign_pointer(pon->oam_devices[0], NULL);
		synchronize_net();
		unregister_netdev(dev);
		rtl_oam_free(dev);
		return ret;
	}
	{
		struct netlink_kernel_cfg cfg = {
			.input = rtl_oam_netlink_receive,
		};
		rcu_assign_pointer(rtl_oam_netlink_pon, pon);
		pon->oam_netlink = netlink_kernel_create(&init_net,
			NETLINK_USERSOCK, &cfg);
		if (!pon->oam_netlink) {
			RCU_INIT_POINTER(rtl_oam_netlink_pon, NULL);
			dev_warn(pon->dev, "OAM netlink redirect unavailable\n");
		}
	}
	dev_info(pon->dev, "%s: native per-LLID EPON OAM transport via NI CPU0/VP8\n", dev->name);
	/*
	 * Hand the PON data path to the flow engine.  rtl_fe_port_ready()
	 * refuses every flow whose input or output maps to the PON source
	 * until fe->pon_attached is set, and this attach call is the only
	 * writer of that flag.  It stays inert while no offload-flagged
	 * flowtable exists, so a failure here only disables hardware
	 * offload and leaves the OAM transport untouched.
	 */
	if (rtl9617c_ni_fe_attach(pon->ni_dev))
		dev_warn(pon->dev, "flow engine PON attach unavailable\n");
	return 0;
free:
	rcu_assign_pointer(pon->oam_devices[0], NULL);
	/* Optical IRQ readers remain active independently of NI attachment. */
	synchronize_net();
	rtl_oam_free(dev);
	return ret;
}

void rtl_pon_oam_remove(struct rtl9617c_pon *pon)
{
	if (rcu_access_pointer(rtl_oam_netlink_pon) == pon)
		RCU_INIT_POINTER(rtl_oam_netlink_pon, NULL);
	if (pon->oam_netlink) {
		netlink_kernel_release(pon->oam_netlink);
		pon->oam_netlink = NULL;
	}
	synchronize_rcu();
	rtl9617c_ni_fe_detach(pon->ni_dev);
	rtl9617c_ni_pon_detach(pon->ni_dev);
	rtl_pon_itu_paths_remove(pon);
	rtl_pon_oam_resize(pon, 0);
}
