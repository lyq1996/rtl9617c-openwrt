// SPDX-License-Identifier: GPL-2.0-or-later

/* Raw OMCI transport netdev: MI container framing plus the NI handoff. */

#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/unaligned.h>
#include <crypto/hash.h>
#include "pon/pon.h"
#include "pon/pon_priv.h"
#include "pon/pon_debug.h"
#define RTL_OMCI_PREFIX_BYTES 16
#define RTL_OMCI_MAX_BODY 1980

unsigned int rtl_omci_queue;

module_param_named(omci_queue, rtl_omci_queue, uint, 0644);

/* Userspace supplies the OMCI body; the transport appends CRC/MIC. */

static unsigned int rtl_omci_body_length(const unsigned char *data,
					       unsigned int length)
{
	if (length < 8)
		return 0;
	if (data[3] == 0x0a)
		return 44;
	if (data[3] == 0x0b && length >= 10)
		return 10 + ((unsigned int)data[8] << 8 | data[9]);
	return 0;
}

static unsigned int rtl_omci_gpon_crc(const unsigned char *data,
					    unsigned int length)
{
	unsigned int crc = ~0U, byte, bit;

	for (byte = 0; byte < length; byte++) {
		crc ^= (unsigned int)data[byte] << 24;
		for (bit = 0; bit < 8; bit++)
			crc = (crc << 1) ^ (crc & 0x80000000U ? 0x04c11db7U : 0);
	}
	return ~crc;
}

void rtl_omci_prefix(unsigned char prefix[RTL_OMCI_PREFIX_BYTES],
				    unsigned int wire_length, unsigned char priority)
{
	static const unsigned char header[RTL_OMCI_PREFIX_BYTES] = {
		0x00, 0x13, 0x25, 0x00, 0x00, 0x00,
		0x00, 0x13, 0x25, 0x00, 0x00, 0x01,
		0xff, 0xf1, 0x00, 0x00,
	};
	unsigned int byte;

	for (byte = 0; byte < RTL_OMCI_PREFIX_BYTES; byte++)
		prefix[byte] = header[byte];
	prefix[14] = wire_length < 48 ? wire_length : 0;
	prefix[15] = priority >> 7;
}

unsigned int rtl_omci_receive_offset(const unsigned char *data,
						  unsigned int length)
{
	if (length >= 16 && data[12] == 0xff && data[13] == 0xf1)
		return 16;
	if (length >= 20 && data[12] == 0x81 && data[13] == 0 &&
	    data[16] == 0xff && data[17] == 0xf1)
		return 20;
	return 0;
}


static int rtl_omci_open(struct net_device *dev)
{
	struct rtl9617c_pon *pon = *(struct rtl9617c_pon **)netdev_priv(dev);
	int ret = rtl9617c_ni_open(pon->ni_dev);

	if (!ret) {
		ret = rtl9617c_ni_oam_configure(pon->ni_dev);
		if (ret) rtl9617c_ni_close(pon->ni_dev, dev);
	}
	if (!ret) {
		mutex_lock(&pon->lock);
		rtl_pon_dying_gasp_prepare(pon);
		mutex_unlock(&pon->lock);
		netif_start_queue(dev);
	}
	return ret;
}

static int rtl_omci_stop(struct net_device *dev)
{
	struct rtl9617c_pon *pon = *(struct rtl9617c_pon **)netdev_priv(dev);

	netif_stop_queue(dev);
	mutex_lock(&pon->lock);
	rtl_pon_dying_gasp_stop(pon);
	rtl9617c_ni_close(pon->ni_dev, dev);
	mutex_unlock(&pon->lock);
	return 0;
}

static netdev_tx_t rtl_omci_transmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rtl9617c_pon *pon = *(struct rtl9617c_pon **)netdev_priv(dev);
	u8 header[10], prefix[RTL_OMCI_PREFIX_BYTES];
	unsigned int body, wire, padding;
	struct sk_buff *packet;
	netdev_tx_t ret;
	struct rtl9617c_ni_frame frame = {
		.header_high = BIT(13),
		.header_low = 7 | (15 << 3) | (17 << 9) | BIT(29),
		.prefix = prefix,
		.prefix_length = RTL_OMCI_PREFIX_BYTES,
		.raw_payload = true,
	};

	if (rtl_pon_mode_is_epon(READ_ONCE(pon->mode)) || !netif_carrier_ok(dev) ||
	    skb->len < 8 || skb_copy_bits(skb, 0, header, min(skb->len, 10U)))
		goto drop;
	body = rtl_omci_body_length(header, min(skb->len, 10U));
	if (!body || body != skb->len || body > RTL_OMCI_MAX_BODY)
		goto drop;
	wire = body + 4;
	padding = wire < 48 ? 48 - wire : 0;
	packet = skb_copy_expand(skb, 0, 4 + padding, GFP_ATOMIC);
	if (!packet)
		goto drop;
	if (READ_ONCE(pon->mode) == RTL_PON_GPON)
		put_unaligned_be32(rtl_omci_gpon_crc(packet->data, body), skb_put(packet, 4));
	else if (rtl_omci_mic(pon, 2, packet->data, body, skb_put(packet, 4))) {
		dev_kfree_skb_any(packet);
		goto drop;
	}
	if (padding)
		memset(skb_put(packet, padding), 0, padding);
	rtl_omci_prefix(prefix, wire, packet->data[0]);
	frame.header_low |= (wire + RTL_OMCI_PREFIX_BYTES) << 15;
	ret = rtl9617c_ni_transmit(pon->ni_dev, dev, packet, &frame);
	if (ret == NETDEV_TX_BUSY) {
		dev_kfree_skb_any(packet);
		return ret;
	}
	pon_event(pon, PON_EV_OMCI_TX, (u32)header[0], (u32)header[1], (u32)body);
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
drop:
	dev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

void rtl_pon_omci_receive(struct rtl9617c_pon *pon, struct napi_struct *napi,
			  struct sk_buff *skb, unsigned int source)
{
	struct net_device *dev = pon->omci_device;
	unsigned int offset, body;

	pon_event_at(pon, PON_EV_OMCI_RX, 0, (u32)skb->len, (u32)source, 0);

	if (!dev || !netif_running(dev) || rtl_pon_mode_is_epon(READ_ONCE(pon->mode)) ||
	    (source & 255) != 7)
		goto drop;
	offset = rtl_omci_receive_offset(skb->data, skb->len);
	if (!offset)
		goto drop;
	skb_pull(skb, offset);
	body = rtl_omci_body_length(skb->data, skb->len);
	if (!body || body > RTL_OMCI_MAX_BODY || skb->len < body + 4)
		goto drop;
	if (READ_ONCE(pon->mode) == RTL_PON_GPON) {
		if (get_unaligned_be32(skb->data + body) != rtl_omci_gpon_crc(skb->data, body)) {
			dev->stats.rx_crc_errors++;
			goto drop;
		}
	} else {
		u8 mic[4];

		if (rtl_omci_mic(pon, 1, skb->data, body, mic) || memcmp(mic, skb->data + body, 4)) {
			dev->stats.rx_crc_errors++;
			goto drop;
		}
	}
	skb_trim(skb, body);
	skb->dev = dev;
	skb->protocol = htons(ETH_P_802_2);
	skb->pkt_type = PACKET_HOST;
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	dev->stats.rx_packets++;
	dev->stats.rx_bytes += body;
	/* The transport does not decode OMCI type/tid, so the two leading wire
	 * bytes are recorded verbatim together with the body length. */
	pon_event_at(pon, PON_EV_OMCI_RX, 1, (u32)skb->data[0],
		     (u32)skb->data[1], (u32)body);
	netif_receive_skb(skb);
	return;
drop:
	pon_event_at(pon, PON_EV_OMCI_RX, 2, 0, 0, (u32)skb->len);
	if (dev)
		dev->stats.rx_dropped++;
	dev_kfree_skb_any(skb);
}

static const struct net_device_ops rtl_omci_ops = {
	.ndo_open = rtl_omci_open,
	.ndo_stop = rtl_omci_stop,
	.ndo_start_xmit = rtl_omci_transmit,
};

static void rtl_omci_setup(struct net_device *dev)
{
	dev->type = ARPHRD_NONE;
	dev->flags = IFF_NOARP | IFF_POINTOPOINT;
	dev->mtu = RTL_OMCI_MAX_BODY;
	dev->min_mtu = 44;
	dev->max_mtu = RTL_OMCI_MAX_BODY;
	dev->tx_queue_len = 64;
	dev->netdev_ops = &rtl_omci_ops;
}

int rtl_pon_omci_init(struct rtl9617c_pon *pon)
{
	struct net_device *dev = alloc_netdev(sizeof(pon), "pon-omci", NET_NAME_UNKNOWN, rtl_omci_setup);
	int ret;

	if (!dev)
		return -ENOMEM;
	pon->omci_kdf = crypto_alloc_shash("cmac(aes)", 0, 0);
	if (IS_ERR(pon->omci_kdf)) {
		ret = PTR_ERR(pon->omci_kdf);
		goto free_netdev;
	}
	pon->omci_cmac = crypto_alloc_shash("cmac(aes)", 0, 0);
	if (IS_ERR(pon->omci_cmac)) {
		ret = PTR_ERR(pon->omci_cmac);
		goto free_kdf;
	}
	spin_lock_init(&pon->omci_key_lock);
	*(struct rtl9617c_pon **)netdev_priv(dev) = pon;
	SET_NETDEV_DEV(dev, pon->dev);
	netif_carrier_off(dev);
	ret = register_netdev(dev);
	if (ret) {
		crypto_free_shash(pon->omci_cmac);
		goto free_kdf;
	}
	pon->omci_device = dev;
	return 0;
free_kdf:
	crypto_free_shash(pon->omci_kdf);
free_netdev:
	free_netdev(dev);
	return ret;
}

void rtl_pon_omci_remove(struct rtl9617c_pon *pon)
{
	struct net_device *dev = pon->omci_device;

	rtl_pon_dying_gasp_stop(pon);
	WRITE_ONCE(pon->omci_device, NULL);
	if (dev) {
		unregister_netdev(dev);
		free_netdev(dev);
	}
	crypto_free_shash(pon->omci_cmac);
	crypto_free_shash(pon->omci_kdf);
}


static ssize_t omci_broadcast_write(struct file *file, struct kobject *kobj,
	const struct bin_attribute *attr, char *buf, loff_t offset, size_t count)
{
	if (offset) return -EINVAL;
	return rtl_pon_omci_broadcast_key(dev_get_drvdata(kobj_to_dev(kobj)), buf, count);
}
BIN_ATTR(omci_broadcast, 0200, NULL, omci_broadcast_write, 0);
