// SPDX-License-Identifier: GPL-2.0-or-later
/* EPON business netdev: transmit path, address handling and TC offload hook. */
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/rtnetlink.h>
#include <net/pkt_cls.h>
#include "pon/pon.h"
#include "pon/pon_priv.h"
#include "rtl9617c_ni.h"
#include "pon/epon/epon.h"
#include "pon/pon_debug.h"

extern int rtl_data_queue;

static unsigned int rtl_epon_tx_queue(const struct sk_buff *skb,
				      unsigned int queues)
{
	int forced = READ_ONCE(rtl_data_queue);
	unsigned int cos;

	if (forced >= 0)
		return (unsigned int)forced & (queues - 1);

	cos = skb->priority & RTL_NI_HDR_COS_MASK;
	if (queues < RTL_EPON_QUEUES_LOW_LLID)
		return cos >> 1;
	return cos;
}

static netdev_tx_t rtl_data_transmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rtl_oam_port *port = netdev_priv(dev);
	struct rtl9617c_pon *pon = port->pon;
	unsigned int queues = READ_ONCE(pon->llid_count) > RTL_EPON_QUEUE_SPLIT_LLIDS ?
			      RTL_EPON_QUEUES_HIGH_LLID : RTL_EPON_QUEUES_LOW_LLID;
	unsigned int queue = rtl_epon_tx_queue(skb, queues);
	/* rtl9617c_ni_fe_add_port() registers this netdev as
	 * (source 7, destination 32 + llid, id llid), so a frame for it must
	 * carry ldpid = 32 + llid; putting the bare LLID there leaves the
	 * fabric with no destination rule and the frame never reaches the
	 * PON. The OAM control netdev instead uses LDPID 15. */
	unsigned int data_ldpid = (RTL_NI_HDR_DATA_LDPID_BASE + port->llid) &
				  RTL_NI_HDR_LDPID_MASK;
	/* Data frames reuse the OAM control container because the PUC only
	 * strips the internal Ethernet header on that path; a bare Ethernet
	 * frame leaves the NI without a container. */
	u8 prefix[RTL_OAM_PREFIX_BYTES];
	struct rtl9617c_ni_frame frame = {
		.header_high = RTL_NI_HEADER_HIGH_OAM,
		.header_low = rtl9617c_ni_header_low(data_ldpid, queue),
	};

	rtl_oam_prefix(prefix, port->llid);
	frame.prefix = prefix;
	frame.prefix_length = RTL_OAM_PREFIX_BYTES;
	frame.oam = true;

	if (!rtl_pon_mode_is_epon(READ_ONCE(pon->mode)))
		return rtl_pon_itu_data_transmit(pon, skb, dev);
	if (!netif_carrier_ok(dev) || skb->len < ETH_HLEN ||
	    skb->len > RTL_EPON_MAX_FRAME_BYTES) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	pon_event(pon, PON_EV_OAM_TX, port->llid, skb->len, queue);
	return rtl9617c_ni_transmit(pon->ni_dev, dev, skb, &frame);
}

const struct net_device_ops rtl_oam_ops = {
	.ndo_open = rtl_oam_open,
	.ndo_stop = rtl_oam_stop,
	.ndo_start_xmit = rtl_oam_transmit,
	.ndo_validate_addr = eth_validate_addr,
};

static int rtl_data_set_address(struct net_device *dev, void *address)
{
	struct rtl_oam_port *port = netdev_priv(dev);
	int ret = eth_mac_addr(dev, address);

	if (!ret)
		port->mac_configured = true;
	return ret;
}

static int rtl_data_setup_tc(struct net_device *dev, enum tc_setup_type type, void *data)
{
	struct rtl_oam_port *port = netdev_priv(dev);

	if (type == TC_SETUP_QDISC_ETS) {
		const struct rtl_dq_io io = {
			port->pon, rtl_puc_bp_read, rtl_puc_bp_write,
			rtl_puc_bp_access
		};
		struct tc_ets_qopt_offload *ets = data;
		u8 weights[8];
		int ret;

		if (!ets || ets->parent != TC_H_ROOT)
			return -EOPNOTSUPP;
		ret = rtl9617c_ni_ets_to_weights(ets, weights);
		if (ret)
			return ret;
		mutex_lock(&port->pon->lock);
		if (port->pon->gpon_drain.phase || port->pon->xgpon_recovery_phase) {
			ret = -EBUSY;
		} else if (!port->pon->puc_configured || port->llid >= 32) {
			ret = -EOPNOTSUPP;
		} else {
			u32 en_mask = rtl_pon_rd(port->pon,
						 VALID_VOQ0 - (port->llid / 4) * 4);
			unsigned int enabled = (en_mask >> ((port->llid % 4) * 8)) & 0xffU;

			ret = rtl_itu_puc_schedule(&io, port->llid,
						   enabled ? enabled : 0xffU,
						   weights);
		}
		mutex_unlock(&port->pon->lock);
		return ret;
	}

	return rtl9617c_ni_fe_setup_tc(port->pon->ni_dev, dev, type, data);
}

static netdev_features_t rtl_data_fix_features(struct net_device *dev,
					       netdev_features_t features)
{
	return (features & ~RTL9617C_NI_TX_OFFLOAD_FEATURES) |
	       (features & rtl9617c_ni_tx_offload_features());
}

static netdev_features_t rtl_data_features_check(struct sk_buff *skb,
						 struct net_device *dev,
						 netdev_features_t features)
{
	struct rtl_oam_port *port = netdev_priv(dev);

	if (rtl_pon_mode_is_epon(READ_ONCE(port->pon->mode)))
		return features & ~(NETIF_F_GSO_MASK | NETIF_F_CSUM_MASK);
	if (skb_has_frag_list(skb))
		features &= ~(NETIF_F_GSO_MASK | NETIF_F_SG);
	if (skb_shinfo(skb)->nr_frags > MAX_SKB_FRAGS)
		features &= ~NETIF_F_GSO_MASK;
	if (skb_is_gso(skb) &&
	    (skb->encapsulation || skb->len > rtl9617c_ni_tx_gso_max()))
		features &= ~NETIF_F_GSO_MASK;
	return features;
}

const struct net_device_ops rtl_data_ops = {
	.ndo_setup_tc = rtl_data_setup_tc,
	.ndo_open = rtl_oam_open,
	.ndo_stop = rtl_oam_stop,
	.ndo_start_xmit = rtl_data_transmit,
	.ndo_fix_features = rtl_data_fix_features,
	.ndo_features_check = rtl_data_features_check,
	.ndo_set_mac_address = rtl_data_set_address,
	.ndo_validate_addr = eth_validate_addr,
};


void rtl_pon_data_address(struct rtl9617c_pon *pon)
{
	unsigned int i;

	ASSERT_RTNL();
	lockdep_assert_held(&pon->control_lock);
	for (i = 0; i < 32; i++) {
		struct net_device *dev = rtnl_dereference(pon->data_devices[i]);

		if (dev && !((struct rtl_oam_port *)netdev_priv(dev))->mac_configured)
			rtl_port_address(dev, pon->onu_mac, i);
	}
}
