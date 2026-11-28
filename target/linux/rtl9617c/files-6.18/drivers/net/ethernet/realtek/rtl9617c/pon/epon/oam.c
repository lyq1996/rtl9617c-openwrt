// SPDX-License-Identifier: GPL-2.0-or-later
/* Native AF_PACKET EPON OAM transport, from RTL8277C GPL EPON DAL. */
#include <linux/etherdevice.h>
#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/rtnetlink.h>
#include <linux/netlink.h>
#include <net/netlink.h>
#include <net/sock.h>
#include <linux/module.h>
#include "pon/pon.h"
#include "pon/pon_priv.h"
#include "rtl9617c_ni.h"
#include "pon/pon_debug.h"
#include "pon/epon/epon.h"

int rtl_data_queue = -1;
module_param_named(data_queue, rtl_data_queue, int, 0644);
MODULE_PARM_DESC(data_queue, "EPON TX VoQ override (-1=skb->priority, 0..7=fixed)");

struct rtl_oam_nl_frame {
	u16 type;
	u16 llid;
	u32 length;
	u8 data[];
} __packed;

struct rtl9617c_pon __rcu *rtl_oam_netlink_pon;

static ssize_t llid_index_show(struct device *device, struct device_attribute *attribute, char *buf)
{
	struct rtl_oam_port *port = netdev_priv(to_net_dev(device));

	return sysfs_emit(buf, "%u\n", port->llid);
}
DEVICE_ATTR_RO(llid_index);

static struct attribute *rtl_oam_attrs[] = {
	&dev_attr_llid_index.attr,
	NULL,
};
const struct attribute_group rtl_oam_group = {
	.attrs = rtl_oam_attrs,
};

static struct rtl9617c_pon *rtl_oam_pon(struct net_device *dev)
{
	return ((struct rtl_oam_port *)netdev_priv(dev))->pon;
}

int rtl_oam_open(struct net_device *dev)
{
	struct rtl9617c_pon *pon = rtl_oam_pon(dev);
	int ret;

	if (((struct rtl_oam_port *)netdev_priv(dev))->control &&
	    !rtl_pon_mode_is_epon(READ_ONCE(pon->mode)))
		return -EOPNOTSUPP;
	ret = rtl9617c_ni_open(pon->ni_dev);

	if (ret)
		return ret;
	ret = ((struct rtl_oam_port *)netdev_priv(dev))->control ?
		rtl9617c_ni_oam_configure(pon->ni_dev) : 0;
	if (ret) {
		rtl9617c_ni_close(pon->ni_dev, dev);
		return ret;
	}
	netif_start_queue(dev);
	return 0;
}

int rtl_oam_stop(struct net_device *dev)
{
	struct rtl9617c_pon *pon = rtl_oam_pon(dev);

	netif_stop_queue(dev);
	rtl9617c_ni_close(pon->ni_dev, dev);
	return 0;
}

void rtl_oam_prefix(u8 *header, unsigned int llid)
{
	static const u8 value[RTL_OAM_PREFIX_BYTES] = {
		0x00, 0x13, 0x25, 0x00, 0x00, 0x00,
		0x00, 0x13, 0x25, 0x00, 0x00, 0x01,
		0xff, 0xf1, 0x00, 0x00,
	};
	unsigned int i;

	/* RTL8277C EPON DAL: the PUC strips this internal Ethernet container. */
	for (i = 0; i < RTL_OAM_PREFIX_BYTES; i++)
		header[i] = value[i];
	header[5] = llid & 0x3f;
}

static int rtl_oam_is_frame(const unsigned char *data, unsigned int length)
{
	return length >= 15 && data[12] == 0x88 && data[13] == 0x09 && data[14] == 0x03;
}

static unsigned int rtl_oam_receive_offset(const unsigned char *data,
					   unsigned int length, unsigned int *llid)
{
	static const unsigned char destination[6] = {0x00, 0x13, 0x25, 0x00, 0x00, 0x01};
	unsigned int i, offset;

	if (length < RTL_OAM_PREFIX_BYTES + 15)
		return 0;
	for (i = 0; i < 6; i++)
		if (data[i] != destination[i])
			return 0;
	if (data[12] == 0xff && data[13] == 0xf1)
		offset = 16;
	else if (data[12] == 0x81 && data[13] == 0x00 && length >= 35 &&
		 data[16] == 0xff && data[17] == 0xf1)
		offset = 20;
	else
		return 0;
	if (!rtl_oam_is_frame(data + offset, length - offset))
		return 0;
	*llid = data[11] & 0x3f;
	return offset;
}

netdev_tx_t rtl_oam_transmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rtl9617c_pon *pon = rtl_oam_pon(dev);
	u8 prefix[RTL_OAM_PREFIX_BYTES], ethernet[15];
	unsigned int llid = ((struct rtl_oam_port *)netdev_priv(dev))->llid;
	/* Control frames always use the NI7/LDPID15 control container: the PUC
	 * only strips that container, and without it the frame never leaves the
	 * fabric. The fixed, non-tunable encoding replaces the old trial matrix. */
	struct rtl9617c_ni_frame frame = {
		.header_high = RTL_NI_HEADER_HIGH_OAM,
		.header_low = rtl9617c_ni_header_low(RTL_NI_HDR_OAM_LDPID,
						     RTL_NI_HDR_OAM_COS),
		.prefix = prefix,
		.prefix_length = RTL_OAM_PREFIX_BYTES,
		.oam = true,
	};

	if (!rtl_pon_mode_is_epon(READ_ONCE(pon->mode)) || !netif_carrier_ok(dev) ||
	    skb->len < sizeof(ethernet) || skb->len > RTL_EPON_MAX_FRAME_BYTES ||
	    skb_copy_bits(skb, 0, ethernet, sizeof(ethernet)) ||
	    !rtl_oam_is_frame(ethernet, sizeof(ethernet))) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	rtl_oam_prefix(prefix, llid);
	pon_event(pon, PON_EV_OAM_TX, llid, skb->len, RTL_NI_HDR_OAM_COS);
	pon_dbg("oam tx: llid=%u len=%u\n", llid, skb->len);
	return rtl9617c_ni_transmit(pon->ni_dev, dev, skb, &frame);
}

void rtl_oam_netlink_receive(struct sk_buff *skb)
{
	struct rtl9617c_pon *pon;
	struct nlmsghdr *nlh;
	struct rtl_oam_nl_frame *msg;
	struct net_device *dev = NULL;
	struct sk_buff *tx;
	unsigned int llid;

	if (skb->len < NLMSG_HDRLEN || !pskb_may_pull(skb, NLMSG_HDRLEN))
		return;
	nlh = nlmsg_hdr(skb);
	if (!nlmsg_ok(nlh, skb->len) ||
	    nlh->nlmsg_len < NLMSG_HDRLEN + sizeof(*msg) ||
	    !pskb_may_pull(skb, nlh->nlmsg_len))
		return;
	msg = nlmsg_data(nlh);
	if (msg->type != 2 || msg->length > 1522 ||
	    nlh->nlmsg_len - NLMSG_HDRLEN - sizeof(*msg) < msg->length)
		return;
	llid = msg->llid & 31;
	rcu_read_lock();
	pon = rcu_dereference(rtl_oam_netlink_pon);
	if (pon) {
		dev = rcu_dereference(pon->oam_devices[llid]);
		if (dev)
			dev_hold(dev);
	}
	rcu_read_unlock();
	if (!dev)
		return;
	if (!netif_running(dev)) {
		dev_put(dev);
		return;
	}
	tx = alloc_skb(msg->length, GFP_ATOMIC);
	if (!tx) {
		dev_put(dev);
		return;
	}
	skb_put_data(tx, msg->data, msg->length);
	tx->dev = dev;
	tx->protocol = htons(ETH_P_8021Q);
	dev_queue_xmit(tx);
	dev_put(dev);
}

void rtl_oam_receive(struct rtl9617c_ni_endpoint *endpoint, struct napi_struct *napi,
			    struct sk_buff *skb, unsigned int source)
{
	struct rtl9617c_pon *pon = container_of(endpoint, struct rtl9617c_pon, oam_endpoint);
	struct net_device *dev;
	struct net_device *drop_owner = endpoint->netdev;
	unsigned int llid, offset, flow = source >> 8;

	if (!rtl_pon_mode_is_epon(READ_ONCE(pon->mode))) {
		if (rtl_omci_receive_offset(skb->data, skb->len))
			rtl_pon_omci_receive(pon, napi, skb, source);
		else
			rtl_pon_itu_data_receive(pon, napi, skb, source);
		return;
	}
	source &= 255;
	/* PDC preserves the physical PON source. Its disabled-policer POL_ID
	 * tags normal Ethernet frames with their LLID. OAM keeps its container. */
	if (source == 7 && !rtl_oam_receive_offset(skb->data, skb->len, &llid))
		source = flow < 32 ? 32 + flow : 255;
	if (source >= 32 && source < 64) {
		llid = source - 32;
		dev = rcu_dereference(pon->data_devices[llid]);
		if (dev)
			drop_owner = dev;
		if (!dev || !netif_running(dev) || !netif_carrier_ok(dev) || skb->len < ETH_HLEN)
			goto drop;
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += skb->len;
		skb->protocol = eth_type_trans(skb, dev);
		napi_gro_receive(napi, skb);
		return;
	}
	offset = rtl_oam_receive_offset(skb->data, skb->len, &llid);
	if (!offset)
		goto drop;
	if (llid == 63 || (pon->llid_count == 1 && (llid == 8 || llid == 10 || llid == 11)))
		llid = 0;
	dev = llid < 32 ? rcu_dereference(pon->oam_devices[llid]) : NULL;
	if (dev)
		drop_owner = dev;
	if (!dev || !netif_running(dev))
		goto drop;
	WRITE_ONCE(pon->last_oam_llid, llid);
	WRITE_ONCE(pon->last_oam_source, source);
	pon_event(pon, PON_EV_OAM_RX, llid, skb->len, source);
	skb_pull(skb, offset);
	skb->priority = llid << 16 | 7;
	dev->stats.rx_packets++;
	dev->stats.rx_bytes += skb->len;
	skb->protocol = eth_type_trans(skb, dev);
	napi_gro_receive(napi, skb);
	return;
drop:
	drop_owner->stats.rx_dropped++;
	dev_kfree_skb_any(skb);
}

void rtl_oam_wake(struct rtl9617c_ni_endpoint *endpoint)
{
	struct rtl9617c_pon *pon = container_of(endpoint, struct rtl9617c_pon, oam_endpoint);
	unsigned int i;

	if (pon->omci_device && netif_running(pon->omci_device) &&
	    netif_queue_stopped(pon->omci_device))
		netif_wake_queue(pon->omci_device);
	for (i = 0; i < 32; i++) {
		struct net_device *dev = rcu_dereference(pon->oam_devices[i]);

		if (dev && netif_running(dev) && netif_queue_stopped(dev))
			netif_wake_queue(dev);
		dev = rcu_dereference(pon->data_devices[i]);
		if (dev && netif_running(dev) && netif_queue_stopped(dev))
			netif_wake_queue(dev);
	}
}

void rtl_oam_update_features(struct rtl9617c_ni_endpoint *endpoint)
{
	struct rtl9617c_pon *pon = container_of(endpoint, struct rtl9617c_pon, oam_endpoint);
	netdev_features_t wanted = rtl9617c_ni_tx_offload_features();
	unsigned int gso_max = rtl9617c_ni_tx_gso_max();
	unsigned int i;

	ASSERT_RTNL();
	for (i = 0; i < 32; i++) {
		struct net_device *dev = rtnl_dereference(pon->data_devices[i]);

		if (!dev)
			continue;
		netif_set_tso_max_size(dev, gso_max);
		netif_set_tso_max_segs(dev, MAX_SKB_FRAGS + 1);
		dev->wanted_features =
			(dev->wanted_features & ~RTL9617C_NI_TX_OFFLOAD_FEATURES) |
			wanted;
		netdev_update_features(dev);
	}
}

void rtl_pon_oam_carrier(struct rtl9617c_pon *pon)
{
	u32 status = readl(pon->mac + RTL_EPON_0010);
	bool synced = !(status & BIT(0)) &&
		(status & (pon->mode == RTL_PON_EPON_1G ? BIT(2) : BIT(1)));
	unsigned int i;

	rcu_read_lock();
	for (i = 0; i < 32; i++) {
		struct net_device *dev = rcu_dereference(pon->oam_devices[i]);

		if (dev) {
			if (pon->traffic_enabled && pon->mpcp_enabled &&
			    (synced || (pon->epon_holdover_active & BIT(i))) && (pon->registered & BIT(i)))
				netif_carrier_on(dev);
			else
				netif_carrier_off(dev);
		}
		dev = rcu_dereference(pon->data_devices[i]);
		if (dev) {
			if (pon->traffic_enabled && pon->mpcp_enabled && pon->pdc_configured &&
			    pon->puc_configured && (synced || (pon->epon_holdover_active & BIT(i))) &&
			    (pon->registered & BIT(i)))
				netif_carrier_on(dev);
			else
				netif_carrier_off(dev);
		}
	}
	rcu_read_unlock();
}

void rtl_port_address(struct net_device *dev, const u8 *base, unsigned int index)
{
	u8 mac[ETH_ALEN];

	ether_addr_copy(mac, base);
	eth_addr_add(mac, index);
	netdev_lock(dev);
	eth_hw_addr_set(dev, mac);
	dev->addr_assign_type = NET_ADDR_SET;
	call_netdevice_notifiers(NETDEV_CHANGEADDR, dev);
	netdev_unlock(dev);
}

void rtl_pon_oam_address(struct rtl9617c_pon *pon)
{
	unsigned int i;

	ASSERT_RTNL();
	lockdep_assert_held(&pon->control_lock);
	for (i = 0; i < 32; i++) {
		struct net_device *dev = rtnl_dereference(pon->oam_devices[i]);
		if (dev)
			rtl_port_address(dev, pon->onu_mac, i);
	}
}
