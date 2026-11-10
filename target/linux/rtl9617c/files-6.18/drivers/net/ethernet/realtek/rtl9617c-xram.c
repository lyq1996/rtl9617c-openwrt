// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitfield.h>
#include <linux/crc32.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel_read_file.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/rcupdate.h>
#include <linux/rtnetlink.h>
#include <linux/unaligned.h>
#include "rtl9617c-led.h"
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "rtl9617c-xram-format.h"
#include "rtl9617c-dma.h"
#include "rtl9617c-qm.h"
#include "rtl9617c-ni.h"
#include "rtl9617c-fe.h"
#include "rtl9617c-mib.h"

#define GLB_STATIC 0xa01c
#define PORT_TO_CPU GENMASK(3, 0)
#define MAC_ADDR0 0xa024
#define MAC_ADDR1 0xa028
#define PORT_STATIC 0xa5c0
#define PORT_GLB 0xa5c4
#define PORT_RX 0xa5c8
#define PORT_TX 0xa5d4
#define PORT_STRIDE 0x90

#define RTL_PHY_LED_COUNT	3
#define RX_ADDR 0xaa20
#define TX_ADDR 0xaa24
#define XRAM_CFG 0xaa28
#define RX_READ 0xaa2c
#define RX_WRITE 0xaa30
#define TX_WRITE 0xaa44
#define TX_READ 0xaa48
#define POINTER_MASK GENMASK(9, 0)
#define CPU_DISABLE (BIT(0) | BIT(9))
#define RX_PROMISC GENMASK(15, 14)
#define XR_OWN BIT(31)
#define PKT_SIZE GENMASK(13, 0)
#define PKT_VALID BIT(19)
#define PKT_ERRORS (GENMASK(23, 20) | BIT(25) | BIT(26))

struct rtl_xram;
struct rtl_lan {
	struct rtl_xram *shared;
	unsigned int port;
};

#define RTL_P6_QX_BASE		0xf550d000UL
#define RTL_P6_MAX_WRITES	400
#define RTL_P6_MODE_SGMII_1G	0x4cf6
#define RTL_P6_MODE_SGMII_2G5	0x5cf5
#define RTL_P6_IFACE_SGMII	0x02
#define RTL_P6_IFACE_SGMII_2G5	0x12
#define RTL_P6_PMA_CTRL		0x160
#define RTL_P6_SETTLE_MS	400
#define RTL_P6_PROFILE_STEP_US	10
#define RTL_P6_CLOCK_BIT	BIT(13)
#define RTL_P6_CLOCK_TIMEOUT_MS	15010
#define RTL_P6_CLOCK_STEP_MS	10

#define XGE_MAC_CFG_COM		0xb200
#define XGE_MAC_CFG_TX		0xb208
#define XGE_MAC_CFG_TX_2	0xb20c
#define XGE_MISC_CONFIG		0xb2a8
#define NITX_MISC_CFG		0xa1f4

#define NI_GLB_INTF_RST_CONFIG	0xa008

#define NI_XGE_BYPASS_CFG	0xa040
#define NI_XGE_BYPASS_FIELDS	GENMASK(12, 0)
#define NI_XGE_BYPASS_CPU0	(BIT(12) | (16U << 4) | BIT(3))
#define L2TM_ES_CTRL		0x2300
#define QM_ES_CTRL		0x6108

struct rtl_p6_profile {
	const char *path;
	unsigned int count;
	int status;
	u32 word[RTL_P6_MAX_WRITES][2];
};

struct rtl_xram {

	struct rtl_lan lan;
	struct net_device *netdev;
	u8 base_mac[ETH_ALEN];
	struct net_device *ports[7];
	unsigned int registered_ports;
	void __iomem *ni;
	void __iomem *xram;
	void __iomem *p6_serdes;
	struct rtl_p6_profile p6_profile[2];
	bool p6_started;
	u32 p6_rx_bypass_saved;
	bool p6_rx_bypass_active;
	struct phy *sgmii;
	unsigned int sgmii_speed;
	struct rtl9617c_led phy_led[RTL_PHY_LED_COUNT];
	bool phy_led_registered;
	struct napi_struct napi;
	struct hrtimer timer;
	spinlock_t tx_lock;
	spinlock_t rx_lock;
	struct mutex mib_lock;
	struct delayed_work mib_work;
	unsigned long long mib_total[5][RTL_NI_MIB_COUNTERS];
	bool rx_active;
	u32 port;
	u32 qx_last;
	bool tx_pending;
	unsigned int tx_length;
	struct net_device *tx_dev;
	u8 tx_buffer[2048];
	bool dma_tx;
	struct rtl_dma_tx dma;
	bool qm_rx;
	struct rtl_qm_rx qm;
	int rx_irq;
	bool irq_requested;
	unsigned int engine_users;
	struct rtl_fe *fe;
	bool removing;
};

static struct rtl_xram *rtl_shared(struct net_device *dev)
{
	return ((struct rtl_lan *)netdev_priv(dev))->shared;
}

static unsigned int rtl_mib_read(void *context, unsigned int reg)
{
	struct rtl_xram *priv = context;

	return readl(priv->ni + reg);
}

static void rtl_mib_write(void *context, unsigned int reg, unsigned int value)
{
	struct rtl_xram *priv = context;

	writel(value, priv->ni + reg);
}

static int rtl_mib_indirect(void *context, unsigned int reg, unsigned int command)
{
	struct rtl_xram *priv = context;
	u32 status;

	writel(command, priv->ni + reg);
	return readl_poll_timeout(priv->ni + reg, status, !(status & BIT(31)), 1, 100000);
}

int rtl9617c_ni_pm_read(struct device *dev, unsigned int port, unsigned long long counters[37])
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	const struct rtl_dq_io io = {priv, rtl_mib_read, rtl_mib_write, rtl_mib_indirect};
	unsigned long long raw[RTL_NI_MIB_COUNTERS];
	int ret;

	if (port >= ARRAY_SIZE(priv->mib_total))
		return -EINVAL;
	mutex_lock(&priv->mib_lock);
	ret = rtl_ni_mib_read(&io, port, raw);
	if (!ret)
		rtl_ni_mib_pm(priv->mib_total[port], raw, counters);
	mutex_unlock(&priv->mib_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_pm_read);

static void rtl_mib_work(struct work_struct *work)
{
	struct rtl_xram *priv = container_of(to_delayed_work(work), struct rtl_xram, mib_work);
	unsigned long long counters[37];
	unsigned int port;

	for (port = 0; port < ARRAY_SIZE(priv->ports); port++) {
		int ret;

		if (!(READ_ONCE(priv->registered_ports) & BIT(port)) || port >= 5)
			continue;
		ret = rtl9617c_ni_pm_read(priv->netdev->dev.parent, port, counters);
		if (ret)
			dev_err_ratelimited(priv->netdev->dev.parent, "NI port%u MIB read failed: %d\n", port, ret);
	}
	schedule_delayed_work(&priv->mib_work, HZ);
}

int rtl9617c_ni_fe_add_port(struct device *dev, struct net_device *netdev,
			    unsigned int llid)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return rtl_fe_add_port(priv->fe, netdev, 7, 32 + llid, llid);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_add_port);

int rtl9617c_ni_itu_paths(struct device *dev, struct net_device *netdev,
			  const struct rtl_itu_paths *paths)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return rtl_fe_set_itu_paths(priv->fe, netdev, paths);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_itu_paths);

void rtl9617c_ni_fe_remove_port(struct device *dev, struct net_device *netdev)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	rtl_fe_remove_port(priv->fe, netdev);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_remove_port);

int rtl9617c_ni_fe_setup_tc(struct device *dev, struct net_device *netdev,
			    enum tc_setup_type type, void *data)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return rtl_fe_setup_tc(priv->fe, netdev, type, data);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_setup_tc);

int rtl9617c_ni_fe_enable(struct device *dev)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return rtl_fe_enable(priv->fe);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_enable);

int rtl9617c_ni_fe_disable(struct device *dev)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return rtl_fe_disable(priv->fe);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_disable);

ssize_t rtl9617c_ni_fe_state(struct device *dev, char *buffer)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return rtl_fe_state(priv->fe, buffer);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_state);

int rtl9617c_ni_report_configure(struct device *dev, unsigned int low, unsigned int high)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	struct rtl_xram *priv;

	if (!netdev)
		return -EPROBE_DEFER;
	priv = netdev_priv(netdev);
	writel((high << 14) | low, priv->ni + 0x96d4);
	return 0;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_report_configure);

u32 rtl9617c_ni_report_state(struct device *dev)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	return readl(priv->ni + 0x96d4);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_report_state);

u32 rtl9617c_ni_pon_gate(struct device *dev, u32 enabled)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned long flags;
	u32 saved;

	spin_lock_irqsave(&priv->tx_lock, flags);
	saved = readl(priv->ni + 0x610c);
	writel((saved & ~0xffffU) | (enabled & 0xffffU), priv->ni + 0x610c);
	readl(priv->ni + 0x610c);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return saved & 0xffffU;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_pon_gate);

static unsigned int rtl_pointer(struct rtl_xram *priv, unsigned int reg)
{
	return readl(priv->ni + reg) & POINTER_MASK;
}

static void rtl_update(struct rtl_xram *priv, unsigned int reg, u32 mask, u32 value)
{
	writel((readl(priv->ni + reg) & ~mask) | value, priv->ni + reg);
}

static void rtl_mac_address(struct rtl_xram *priv)
{
	const u8 *mac = priv->netdev->dev_addr;

	writel(get_unaligned_be32(mac), priv->ni + MAC_ADDR0);
	rtl_update(priv, MAC_ADDR1, 0xff, mac[4]);
	rtl_update(priv, PORT_STATIC + priv->port * PORT_STRIDE,
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
	struct rtl_lan *lan = netdev_priv(dev);
	struct rtl_xram *priv = lan->shared;
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
			rtl_update(priv, PORT_STATIC + port * PORT_STRIDE,
				   GENMASK(31, 24), (u32)mac[5] << 24);
	}
	rtl_mac_address(priv);
	return 0;
}

static void rtl_p6_set_speed(struct rtl_xram *priv, unsigned int speed);

static void rtl_adjust_link(struct net_device *dev)
{
	struct rtl_lan *lan = netdev_priv(dev);
	struct phy_device *phy = dev->phydev;

	if (phy->link) {
		if (lan->port >= 5) {
			if (lan->shared->sgmii_speed != phy->speed) {
				rtl_p6_set_speed(lan->shared, phy->speed);
				lan->shared->sgmii_speed = phy->speed;
			}
			phy_print_status(phy);
			return;
		}
		rtl_update(lan->shared, PORT_STATIC + lan->port * PORT_STRIDE,
			   GENMASK(3, 0), phy->speed >= SPEED_1000 ? 0 : 1);
		rtl_update(lan->shared, PORT_GLB + lan->port * PORT_STRIDE,
			   BIT(0) | BIT(1), (phy->speed == SPEED_10 ? BIT(0) : 0) |
			   (phy->duplex == DUPLEX_HALF ? BIT(1) : 0));
	}
	phy_print_status(phy);
}

static void rtl_rx_mode(struct net_device *dev)
{
	struct rtl_xram *priv = rtl_shared(dev);
	bool accept_all = (dev->flags & (IFF_PROMISC | IFF_ALLMULTI)) ||
			  !netdev_mc_empty(dev);

	if (priv->qm_rx)
		return;

	rtl_update(priv, XRAM_CFG, RX_PROMISC, FIELD_PREP(RX_PROMISC, accept_all));
}

static int rtl_poll(struct napi_struct *napi, int budget)
{
	struct rtl_xram *priv = container_of(napi, struct rtl_xram, napi);
	struct net_device *dev = priv->netdev;
	int packets = 0;

	if (priv->qm_rx) {
		unsigned long flags;

		packets = rtl_qm_rx_poll(&priv->qm, napi, budget);

		spin_lock_irqsave(&priv->rx_lock, flags);
		if (packets < budget && napi_complete_done(napi, packets) && priv->rx_active)
			rtl_update(priv, 0x6110, 0xff, 0xff);
		spin_unlock_irqrestore(&priv->rx_lock, flags);
		return packets;
	}

	while (packets < budget) {
		unsigned int rd = rtl_pointer(priv, RX_READ);
		unsigned int wr = rtl_pointer(priv, RX_WRITE);
		unsigned int offset, next, size, copied;
		struct sk_buff *skb;
		u32 header, status;

		if (rd == wr)
			break;
		header = readl(priv->xram + rd * 8 + 4);
		if (header & XR_OWN)
			break;
		next = header & POINTER_MASK;
		if (next >= RTL_XRAM_RX_END || next == rd) {
			dev->stats.rx_errors++;
			writel(wr, priv->ni + RX_READ);
			break;
		}
		offset = rtl_xram_word_next(rd * 8 + 4, 0, RTL_XRAM_RX_END * 8);
		status = readl(priv->xram + offset);
		size = FIELD_GET(PKT_SIZE, status);
		packets++;
		if (!(status & PKT_VALID) || (status & PKT_ERRORS) ||
		    size < ETH_HLEN + ETH_FCS_LEN || size > 1522) {
			dev->stats.rx_errors++;
			writel(next, priv->ni + RX_READ);
			continue;
		}
		size -= ETH_FCS_LEN;
		skb = napi_alloc_skb(napi, size);
		if (!skb) {
			dev->stats.rx_dropped++;
			writel(next, priv->ni + RX_READ);
			continue;
		}
		offset = rtl_xram_word_next(offset, 0, RTL_XRAM_RX_END * 8);
		offset = rtl_xram_word_next(offset, 0, RTL_XRAM_RX_END * 8);
		for (copied = 0; copied < size; copied += 4) {
			__le32 word = cpu_to_le32(readl(priv->xram + offset));

			memcpy(skb_put(skb, min(4U, size - copied)), &word,
			       min(4U, size - copied));
			offset = rtl_xram_word_next(offset, 0, RTL_XRAM_RX_END * 8);
		}
		writel(next, priv->ni + RX_READ);
		skb->protocol = eth_type_trans(skb, dev);
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += size;
		napi_gro_receive(napi, skb);
	}
	if (packets < budget)
		napi_complete_done(napi, packets);
	return packets;
}

static void rtl_wake_ports(struct rtl_xram *priv)
{
	unsigned int port;

	for (port = 0; port < ARRAY_SIZE(priv->ports); port++)
		if (priv->ports[port] && netif_running(priv->ports[port]) &&
		    netif_queue_stopped(priv->ports[port]))
			netif_wake_queue(priv->ports[port]);
}

static enum hrtimer_restart rtl_timer(struct hrtimer *timer)
{
	struct rtl_xram *priv = container_of(timer, struct rtl_xram, timer);

	if (!priv->qm_rx || READ_ONCE(priv->qm.refill.pending))
		napi_schedule(&priv->napi);
	spin_lock(&priv->tx_lock);
	if (priv->dma_tx) {
		struct rtl9617c_ni_endpoint *endpoint;

		rtl_dma_tx_complete(&priv->dma, priv->netdev);
		rcu_read_lock();
		endpoint = rcu_dereference(priv->qm.pon);
		if (endpoint && rtl_dma_tx_available(&priv->dma))
			endpoint->wake(endpoint);
		rcu_read_unlock();
		if (rtl_dma_tx_available(&priv->dma))
			rtl_wake_ports(priv);
	} else if (priv->tx_pending && rtl_pointer(priv, TX_READ) == rtl_pointer(priv, TX_WRITE)) {
		priv->tx_pending = false;
		if (priv->tx_dev) {
			priv->tx_dev->stats.tx_packets++;
			priv->tx_dev->stats.tx_bytes += priv->tx_length;
			priv->tx_dev = NULL;
		}
		rtl_wake_ports(priv);
	}
	spin_unlock(&priv->tx_lock);
	hrtimer_forward_now(timer, ms_to_ktime(1));
	return HRTIMER_RESTART;
}

static irqreturn_t rtl_rx_irq(int irq, void *data)
{
	struct rtl_xram *priv = data;

	spin_lock(&priv->rx_lock);
	rtl_update(priv, 0x6110, 0xff, 0);
	spin_unlock(&priv->rx_lock);
	napi_schedule_irqoff(&priv->napi);
	return IRQ_HANDLED;
}

static netdev_tx_t rtl_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rtl_lan *lan = netdev_priv(dev);
	struct rtl_xram *priv = lan->shared;
	unsigned int pointer, offset, wire, i;
	unsigned long flags;
	u32 crc;

	if (skb->len < ETH_HLEN || skb->len > 1522) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&priv->tx_lock, flags);
	if (priv->dma_tx) {

		unsigned int egress = lan->port >= 5 ? 6 : lan->port;
		const struct rtl9617c_ni_frame frame = {
			.header_high = BIT(13),
			.header_low = (16 << 9) | (egress << 3) | BIT(29),
		};
		netdev_tx_t result = rtl_dma_tx_frame(&priv->dma, skb, dev, &frame);

		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return result;
	}
	pointer = rtl_pointer(priv, TX_WRITE);
	if (priv->tx_pending || rtl_pointer(priv, TX_READ) != pointer) {
		netif_stop_queue(dev);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}
	wire = rtl_xram_wire_length(skb->len);
	memset(priv->tx_buffer, 0, ALIGN(8 + wire, 4));
	skb_copy_bits(skb, 0, priv->tx_buffer + 8, skb->len);
	crc = crc32_le(~0, priv->tx_buffer + 8, wire - 4) ^ ~0;
	put_unaligned_le32(crc, priv->tx_buffer + 8 + wire - 4);
	offset = pointer * 8 + 4;
	writel(rtl_xram_tx_header(rtl_xram_tx_next(pointer, wire), wire),
	       priv->xram + offset);
	offset = rtl_xram_word_next(offset, RTL_XRAM_TX_START * 8, RTL_XRAM_TX_END * 8);
	for (i = 0; i < wire + 8; i += 4) {
		writel(get_unaligned_le32(priv->tx_buffer + i), priv->xram + offset);
		offset = rtl_xram_word_next(offset, RTL_XRAM_TX_START * 8, RTL_XRAM_TX_END * 8);
	}
	priv->tx_pending = true;
	priv->tx_length = skb->len;
	priv->tx_dev = dev;
	netif_stop_queue(dev);
	wmb();
	writel(rtl_xram_tx_next(pointer, wire), priv->ni + TX_WRITE);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
}

static bool rtl_p6_next_ul(const char **line, unsigned long *value)
{
	const char *token = skip_spaces(*line);
	size_t length = strcspn(token, " \t");
	char number[24];

	if (!token || *token == '#' || *token == '\0' || !length ||
	    length >= sizeof(number))
		return false;
	memcpy(number, token, length);
	number[length] = '\0';
	if (kstrtoul(number, 0, value))
		return false;
	*line = token + length;

	return true;
}

static int rtl_p6_read(struct rtl_xram *priv, struct rtl_p6_profile *profile)
{
	size_t size = 0;
	ssize_t ret;

	void *buf = NULL, *end_buf;
	char *line, *next;
	unsigned long offset, value;

	if (profile->status)
		return profile->status > 0 ? 0 : profile->status;
	ret = kernel_read_file_from_path(profile->path, 0, &buf, 0x10000, &size,
					 READING_UNKNOWN);
	if (ret < 0) {
		profile->status = ret;
		return ret;
	}
	if (!size || size > 0x10000) {
		vfree(buf);
		profile->status = -EINVAL;
		return -EINVAL;
	}
	end_buf = buf + size;
	for (line = buf; line < (char *)end_buf; line = next) {
		const char *cursor;

		next = memchr(line, '\n', (char *)end_buf - line);
		if (next)
			*next++ = '\0';
		else
			next = end_buf;
		cursor = line;
		if (!rtl_p6_next_ul(&cursor, &offset))
			continue;
		if (!rtl_p6_next_ul(&cursor, &value))
			continue;
		if (offset < RTL_P6_QX_BASE || offset >= RTL_P6_QX_BASE + 0x1000)
			continue;
		profile->word[profile->count][0] = offset - RTL_P6_QX_BASE;
		profile->word[profile->count][1] = value;
		if (++profile->count == RTL_P6_MAX_WRITES)
			break;
	}
	vfree(buf);
	profile->status = 1;
	return 0;
}

static void rtl_p6_apply(struct rtl_xram *priv, struct rtl_p6_profile *profile)
{
	unsigned int i;

	for (i = 0; i < profile->count; i++) {
		writel(profile->word[i][1], priv->p6_serdes + profile->word[i][0]);
		udelay(RTL_P6_PROFILE_STEP_US);
	}
}

#define RTL_P6_REG_DATAIN	0x50
#define RTL_P6_REG_ACCESS	0x4c
#define RTL_P6_REG_DATAOUT	0x54
#define RTL_P6_REG_STATUS	0x58
#define RTL_P6_SDS_WE		BIT(30)
#define RTL_P6_SDS_ACCESS	BIT(31)

static void rtl_p6_ind_write(struct rtl_xram *priv, u32 addr, u16 data)
{
	writel(data, priv->p6_serdes + RTL_P6_REG_DATAIN);
	writel(RTL_P6_SDS_ACCESS | RTL_P6_SDS_WE | (addr & 0x7ffU),
	       priv->p6_serdes + RTL_P6_REG_ACCESS);

	usleep_range(1000, 2000);
}

#define RTL_P6_LANE_STRIDE	4
#define RTL_P6_LANE_COUNT	4

static void rtl_p6_mode_word(struct rtl_xram *priv, u32 word)
{
	unsigned int lane;

	for (lane = 0; lane < RTL_P6_LANE_COUNT; lane++) {
		void __iomem *reg = priv->p6_serdes + 0x8 +
				    lane * RTL_P6_LANE_STRIDE;

		writel((readl(reg) & ~0x1ffffU) | (word & 0x1ffffU), reg);
	}
}

static void rtl_p6_mac_iface_select(struct rtl_xram *priv);

static void rtl_p6_mac_config(struct rtl_xram *priv)
{
	void __iomem *ni = priv->ni;
	u32 es, qm, value;

	es = readl(ni + L2TM_ES_CTRL);
	writel(es & ~BIT(6), ni + L2TM_ES_CTRL);
	qm = readl(ni + QM_ES_CTRL);
	writel((qm & 0xffff0000U) | (qm & 0xffU) | ((qm >> 8) & 0xbfU) << 8,
	       ni + QM_ES_CTRL);
	writel(readl(ni + NITX_MISC_CFG) | BIT(20), ni + NITX_MISC_CFG);

	value = readl(ni + XGE_MISC_CONFIG) & 0xcfffffffU;
	writel(value | 0x80000000U, ni + XGE_MISC_CONFIG);
	writel(value | 0xb0000000U, ni + XGE_MISC_CONFIG);

	msleep(10);

	writel(readl(ni + XGE_MAC_CFG_TX) & ~0x3U, ni + XGE_MAC_CFG_TX);
	writel(readl(ni + XGE_MAC_CFG_TX) | 1, ni + XGE_MAC_CFG_TX);
	value = readl(ni + XGE_MAC_CFG_TX_2);
	writel((value & 0xffffe000U) | (value & 0xe0U) | 0xa00U | 10,
	       ni + XGE_MAC_CFG_TX_2);

	value = readl(ni + XGE_MAC_CFG_COM);
	value = (value | 0x00008000U | 0x00004000U) & 0xffffdfffU & 0xffffff8fU;
	writel(value, ni + XGE_MAC_CFG_COM);
	msleep(10);
	value = readl(ni + XGE_MAC_CFG_COM);
	value &= 0xffff7fffU & 0xffffbfffU & 0xffffdfffU & 0xffffff8fU;
	writel(value, ni + XGE_MAC_CFG_COM);

	value = readl(ni + XGE_MISC_CONFIG);
	value = (value & 0x7fffffffU) & 0xcfffffffU;
	writel(value, ni + XGE_MISC_CONFIG);
	value &= 0x7fffffffU;
	value = (value & ~(3U << 28)) | (2U << 28);
	writel(value, ni + XGE_MISC_CONFIG);
	writel(readl(ni + NITX_MISC_CFG) & 0xffefffffU, ni + NITX_MISC_CFG);

	writel(readl(ni + L2TM_ES_CTRL) | BIT(6), ni + L2TM_ES_CTRL);
	writel((qm & 0xffff0000U) | (qm & 0xffU) | (((qm >> 8) & 0xffU) | 0x40) << 8,
	       ni + QM_ES_CTRL);
}

static void rtl_p6_serdes_init(struct rtl_xram *priv, u32 word, u32 iface,
			       struct rtl_p6_profile *profile)
{
	u32 value;
	int ret;

	if (priv->qm.block_reset) {
		value = readl(priv->qm.block_reset);
		writel(value | BIT(31), priv->qm.block_reset);
		udelay(1);
		writel(readl(priv->qm.block_reset) & ~BIT(31),
		       priv->qm.block_reset);
	}

	writel(readl(priv->p6_serdes) | BIT(0), priv->p6_serdes);
	writel(readl(priv->p6_serdes + 0x4) | 0x1f, priv->p6_serdes + 0x4);
	usleep_range(1000, 2000);
	writel((readl(priv->p6_serdes + 0x4) & ~0x1fU) | iface,
	       priv->p6_serdes + 0x4);
	rtl_p6_mode_word(priv, word);

	rtl_p6_ind_write(priv, RTL_P6_PMA_CTRL, 0x009f);

	if (rtl_p6_read(priv, profile) == 0)
		rtl_p6_apply(priv, profile);
	else
		dev_warn(priv->netdev->dev.parent, "serdes load %s failed\n",
			 profile->path);

	writel(readl(priv->p6_serdes) & ~BIT(0), priv->p6_serdes);

	ret = readl_poll_timeout(priv->p6_serdes + RTL_P6_REG_STATUS, value,
				 value & RTL_P6_CLOCK_BIT,
				 RTL_P6_CLOCK_STEP_MS * USEC_PER_MSEC,
				 RTL_P6_CLOCK_TIMEOUT_MS * USEC_PER_MSEC);
	if (ret)
		dev_warn(priv->netdev->dev.parent, "P6: SerDes clock not ready\n");
	else
		msleep(RTL_P6_SETTLE_MS);
}

static void rtl_p6_bringup(struct rtl_xram *priv)
{
	u32 value;

	if (!priv->p6_serdes || priv->p6_started)
		return;
	priv->p6_started = true;

	if (!priv->sgmii) {
		priv->sgmii = devm_phy_get(priv->netdev->dev.parent, "sgmii");
		if (IS_ERR(priv->sgmii)) {
			dev_warn(priv->netdev->dev.parent,
				 "P6: no sgmii lane (%ld); continuing without it\n",
				 PTR_ERR(priv->sgmii));
			priv->sgmii = NULL;
		} else {
			int lret = phy_power_on(priv->sgmii);

			if (lret)
				dev_warn(priv->netdev->dev.parent,
					 "P6: S3 lane power-on failed: %d\n", lret);
		}
	}

	if (priv->qm.block_reset) {
		value = readl(priv->qm.block_reset);
		writel(value | 0x102001c0U, priv->qm.block_reset);

		value = readl(priv->qm.block_reset);
		writel(value | BIT(31), priv->qm.block_reset);
		msleep(50);

		value = readl(priv->qm.block_reset);
		writel(value & ~BIT(31), priv->qm.block_reset);
		usleep_range(1000, 2000);
	}

	rtl_p6_serdes_init(priv, RTL_P6_MODE_SGMII_2G5, RTL_P6_IFACE_SGMII_2G5,
			   &priv->p6_profile[0]);

	if (!priv->sgmii) {
		priv->sgmii_speed = SPEED_2500;
	} else if (priv->sgmii_speed != SPEED_2500) {
		int sret = phy_set_speed(priv->sgmii, SPEED_2500);

		if (sret)
			dev_warn(priv->netdev->dev.parent,
				 "P6: S3 lane 2.5G programming failed: %d\n", sret);
		else
			priv->sgmii_speed = SPEED_2500;
	}

	rtl_p6_mac_config(priv);

	rtl_update(priv, 0xb204, BIT(1) | BIT(4) | BIT(5) | BIT(9),
		   0x0bf3);
	rtl_update(priv, 0xb208, BIT(4) | BIT(5) | 0x31, 0x31);

	writel(0, priv->ni + NI_GLB_INTF_RST_CONFIG);

	rtl_p6_mac_iface_select(priv);
}

static void rtl_p6_mac_iface_select(struct rtl_xram *priv)
{
	void __iomem *ni = priv->ni;
	u32 value;

	value = readl(ni + NITX_MISC_CFG);
	writel(value | BIT(20), ni + NITX_MISC_CFG);

	value = readl(ni + XGE_MISC_CONFIG);
	writel(value & ~(BIT(31) | (3U << 28)), ni + XGE_MISC_CONFIG);
	writel((value & ~(BIT(31) | (3U << 28))) | BIT(31) | BIT(28),
	       ni + XGE_MISC_CONFIG);
	msleep(10);

	value = readl(ni + XGE_MAC_CFG_COM);
	writel(value | BIT(13), ni + XGE_MAC_CFG_COM);
	msleep(10);
	value = readl(ni + XGE_MAC_CFG_COM);
	writel(value & ~BIT(13), ni + XGE_MAC_CFG_COM);

	value = readl(ni + NITX_MISC_CFG);
	writel(value & ~BIT(20), ni + NITX_MISC_CFG);
}

static void rtl_p6_set_speed(struct rtl_xram *priv, unsigned int speed)
{
	bool two_g5 = speed == SPEED_2500;
	struct rtl_p6_profile *profile = &priv->p6_profile[two_g5 ? 0 : 1];
	u32 word = two_g5 ? RTL_P6_MODE_SGMII_2G5 : RTL_P6_MODE_SGMII_1G;
	u32 iface = two_g5 ? RTL_P6_IFACE_SGMII_2G5 : RTL_P6_IFACE_SGMII;

	if (!priv->p6_serdes)
		return;

	if (priv->sgmii_speed == speed)
		return;
	priv->sgmii_speed = speed;

	rtl_p6_serdes_init(priv, word, iface, profile);

	if (priv->sgmii) {
		int sret = phy_set_speed(priv->sgmii, speed);

		if (sret)
			dev_warn(priv->netdev->dev.parent,
				 "P6: S3 lane %uM programming failed: %d\n",
				 speed, sret);
	}

	rtl_p6_mac_config(priv);

	rtl_p6_mac_iface_select(priv);
}

static int rtl_engine_start(struct rtl_xram *priv)
{
	struct net_device *dev = priv->netdev;
	unsigned int port;
	u32 config;
	int ret;

	if (priv->ports[6])
		rtl_p6_bringup(priv);

	if (priv->dma_tx) {

		writel(BIT(31) | (4U << 16) | (29U << 20), priv->ni + 0x2208);
		rtl_update(priv, 0x2218, GENMASK(14, 0), 29);
		writel(0x08008021, priv->ni + 0x2210);

		rtl_update(priv, 0x2304 + 12 * 14, 0xff, 0xff);
		rtl_update(priv, 0x2300, BIT(31) | BIT(15), BIT(31) | BIT(15));
		for (port = 0; port < 5; port++)
			if (priv->ports[port]) {
				rtl_update(priv, 0x2300, BIT(31) | BIT(port), BIT(31) | BIT(port));
				rtl_update(priv, 0x2304 + 12 * port, 0xff, 0xff);
			}
		ret = rtl_dma_tx_start(&priv->dma);
		if (ret)
			return ret;
	}
	if (priv->qm_rx) {
		ret = rtl_qm_rx_start(&priv->qm, priv->port);
		if (ret) {
			if (priv->dma_tx)
				rtl_dma_tx_stop(&priv->dma);
			return ret;
		}

		for (port = 0; port < 5; port++)
			if (priv->ports[port])
				rtl_update(priv, 0xa5f8 + PORT_STRIDE * port,
					   BIT(28) | GENMASK(21, 16) | BIT(31),
					   BIT(28) | (16U << 16));
		if (priv->ports[6]) {
			priv->p6_rx_bypass_saved = readl(priv->ni + NI_XGE_BYPASS_CFG);

			rtl_update(priv, NI_XGE_BYPASS_CFG, NI_XGE_BYPASS_FIELDS,
				   NI_XGE_BYPASS_CPU0);
			priv->p6_rx_bypass_active = true;
		}
	}

	rtl_update(priv, GLB_STATIC, PORT_TO_CPU, priv->dma_tx ? 0xf : priv->port);

	rtl_update(priv, 0x2300, GENMASK(13, 0) | BIT(31),
		   GENMASK(13, 0) | BIT(31));

	rtl_mac_address(priv);
	writel((RTL_XRAM_RX_END - 1) << 16, priv->ni + RX_ADDR);
	writel((RTL_XRAM_TX_END - 1) << 16 | RTL_XRAM_TX_START, priv->ni + TX_ADDR);
	config = readl(priv->ni + XRAM_CFG) & ~RX_PROMISC;
	writel(config | CPU_DISABLE, priv->ni + XRAM_CFG);
	udelay(100);
	writel(priv->qm_rx ? config | CPU_DISABLE : config & ~CPU_DISABLE,
	       priv->ni + XRAM_CFG);
	rtl_rx_mode(dev);
	priv->tx_pending = false;
	priv->tx_dev = NULL;
	napi_enable(&priv->napi);
	if (priv->qm_rx) {
		unsigned long flags;

		spin_lock_irqsave(&priv->rx_lock, flags);
		priv->rx_active = true;
		rtl_update(priv, 0x6110, 0xff, 0xff);
		spin_unlock_irqrestore(&priv->rx_lock, flags);
	}
	hrtimer_start(&priv->timer, ms_to_ktime(1), HRTIMER_MODE_REL);
	return 0;
}

static void rtl_engine_stop(struct rtl_xram *priv)
{
	unsigned long flags;

	hrtimer_cancel(&priv->timer);
	if (priv->p6_rx_bypass_active) {
		rtl_update(priv, NI_XGE_BYPASS_CFG, NI_XGE_BYPASS_FIELDS,
			   priv->p6_rx_bypass_saved & NI_XGE_BYPASS_FIELDS);
		priv->p6_rx_bypass_active = false;
	}
	if (priv->qm_rx) {
		spin_lock_irqsave(&priv->rx_lock, flags);
		priv->rx_active = false;
		rtl_update(priv, 0x6110, 0xff, 0);
		spin_unlock_irqrestore(&priv->rx_lock, flags);
		synchronize_irq(priv->rx_irq);
	}
	napi_disable(&priv->napi);
	if (priv->qm_rx) {

		spin_lock_irqsave(&priv->rx_lock, flags);
		rtl_qm_rx_stop(&priv->qm);
		spin_unlock_irqrestore(&priv->rx_lock, flags);
	}
	if (priv->dma_tx)
		rtl_dma_tx_stop(&priv->dma);
	rtl_update(priv, XRAM_CFG, CPU_DISABLE, CPU_DISABLE);
	return;
}

static int rtl_engine_get(struct rtl_xram *priv)
{
	int ret;

	if (priv->engine_users) {
		priv->engine_users++;
		return 0;
	}
	ret = rtl_engine_start(priv);
	if (!ret)
		priv->engine_users = 1;
	return ret;
}

static void rtl_engine_put(struct rtl_xram *priv)
{
	if (!--priv->engine_users)
		rtl_engine_stop(priv);
}

static int rtl_open(struct net_device *dev)
{
	struct rtl_lan *lan = netdev_priv(dev);
	struct rtl_xram *priv = lan->shared;
	int ret = rtl_engine_get(priv);

	if (ret)
		return ret;

	if (lan->port < 5) {
		rtl_update(priv, 0xa008, BIT(lan->port) | BIT(lan->port + 6) |
			   BIT(lan->port + 12), 0);
		rtl_update(priv, PORT_STATIC + lan->port * PORT_STRIDE,
			   GENMASK(31, 24), (u32)dev->dev_addr[5] << 24);
		rtl_update(priv, PORT_RX + lan->port * PORT_STRIDE, BIT(0), BIT(0));
		rtl_update(priv, PORT_TX + lan->port * PORT_STRIDE, BIT(0), BIT(0));
	}
	phy_start(dev->phydev);
	netif_start_queue(dev);
	return 0;
}

static int rtl_stop(struct net_device *dev)
{
	struct rtl_lan *lan = netdev_priv(dev);
	struct rtl_xram *priv = lan->shared;
	unsigned long flags;

	netif_stop_queue(dev);
	phy_stop(dev->phydev);
	if (lan->port < 5) {
		rtl_update(priv, PORT_RX + lan->port * PORT_STRIDE, BIT(0), 0);
		rtl_update(priv, PORT_TX + lan->port * PORT_STRIDE, BIT(0), 0);
	}
	rtl_fe_flush_port(priv->fe, dev);
	if (priv->dma_tx) {
		spin_lock_irqsave(&priv->tx_lock, flags);
		rtl_dma_tx_forget(&priv->dma, dev);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
	}
	rtl_engine_put(priv);
	return 0;
}

int rtl9617c_ni_pon_attach(struct device *dev, struct rtl9617c_ni_endpoint *endpoint)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	if (!priv->qm_rx || !priv->dma_tx)
		return -EOPNOTSUPP;
	rcu_assign_pointer(priv->qm.pon, endpoint);
	return 0;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_pon_attach);

void rtl9617c_ni_pon_detach(struct device *dev)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	rcu_assign_pointer(priv->qm.pon, NULL);
	synchronize_net();
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_pon_detach);

int rtl9617c_ni_open(struct device *dev)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return rtl_engine_get(priv);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_open);

void rtl9617c_ni_close(struct device *dev, struct net_device *owner)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned long flags;

	rtl_fe_flush_port(priv->fe, owner);
	if (priv->dma_tx) {
		spin_lock_irqsave(&priv->tx_lock, flags);
		rtl_dma_tx_forget(&priv->dma, owner);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
	}
	rtl_engine_put(priv);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_close);

netdev_tx_t rtl9617c_ni_transmit(struct device *dev, struct net_device *owner,
			       struct sk_buff *skb, const struct rtl9617c_ni_frame *frame)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned long flags;
	netdev_tx_t result;

	if (!priv->dma_tx) {
		owner->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&priv->tx_lock, flags);
	result = rtl_dma_tx_frame(&priv->dma, skb, owner, frame);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return result;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_transmit);

int rtl9617c_ni_oam_configure(struct device *dev)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	static const unsigned int index[] = {15, 0x8f, 0x4f, 0xcf};
	u32 status;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(index); i++) {
		writel(12, priv->ni + 0x1658);
		writel(BIT(31) | BIT(30) | index[i], priv->ni + 0x1654);
		ret = readl_poll_timeout(priv->ni + 0x1654, status, !(status & BIT(31)), 1, 100000);
		if (ret)
			return ret;
	}
	writel(0xff | (6 << 16), priv->ni + 0x2304 + 12 * 12);
	rtl_update(priv, 0x2300, BIT(12), BIT(12));
	return 0;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_oam_configure);

static int rtl_setup_tc(struct net_device *netdev, enum tc_setup_type type,
			void *data)
{
	struct rtl_xram *priv = rtl_shared(netdev);

	return rtl_fe_setup_tc(priv->fe, netdev, type, data);
}
static const struct net_device_ops rtl_netdev_ops = {
	.ndo_setup_tc = rtl_setup_tc,
	.ndo_open = rtl_open,
	.ndo_stop = rtl_stop,
	.ndo_start_xmit = rtl_xmit,
	.ndo_set_rx_mode = rtl_rx_mode,
	.ndo_set_mac_address = rtl_set_mac_address,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_eth_ioctl = phy_do_ioctl_running,
};

static const struct ethtool_ops rtl_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = phy_ethtool_get_link_ksettings,
	.set_link_ksettings = phy_ethtool_set_link_ksettings,
	.nway_reset = phy_ethtool_nway_reset,
};

static int rtl_phy_led_output(struct rtl9617c_led *led, unsigned int value)
{
	struct rtl_xram *priv = led->drvdata;
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

static void rtl_unregister_lan(struct rtl_xram *priv, unsigned int port)
{
	struct net_device *dev = priv->ports[port];
	unsigned long flags;

	if (!dev)
		return;
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
	if (dev->phydev)
		phy_disconnect(dev->phydev);
	if (dev != priv->netdev)
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

static int rtl_register_lan(struct rtl_xram *priv, struct device *parent,
			    unsigned int port, struct device_node *phy)
{
	struct phy_device *phydev;
	struct net_device *dev = NULL;
	struct rtl_lan *lan;
	unsigned int index;
	u8 mac[ETH_ALEN];
	int ret;

	dev = port == priv->port ? priv->netdev : alloc_etherdev(sizeof(*lan));
	if (!dev) {
		ret = -ENOMEM;
		goto put_phy;
	}
	priv->ports[port] = dev;
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
	phydev = of_phy_find_device(phy);
	if (!phydev) {
		ret = rtl_phy_missing(phy);
		goto put_phy;
	}
	ret = phy_connect_direct(dev, phydev, rtl_adjust_link,
				 port == 6 ? PHY_INTERFACE_MODE_SGMII :
					     PHY_INTERFACE_MODE_GMII);
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
			led->output = rtl_phy_led_output;
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

static int rtl_register_lans(struct rtl_xram *priv, struct device *parent)
{
	struct device_node *phy;
	unsigned int port;
	int ret = 0;

	rtnl_lock();
	for (port = 0; port < 5; port++) {
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

static int rtl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *netdev;
	struct rtl_xram *priv;
	u8 mac[ETH_ALEN];
	int ret;

	netdev = alloc_etherdev(sizeof(*priv));
	if (!netdev)
		return -ENOMEM;
	SET_NETDEV_DEV(netdev, dev);
	priv = netdev_priv(netdev);
	priv->lan.shared = priv;
	priv->netdev = netdev;
	priv->ni = devm_platform_ioremap_resource(pdev, 0);
	priv->xram = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(priv->ni) || IS_ERR(priv->xram)) {
		ret = IS_ERR(priv->ni) ? PTR_ERR(priv->ni) : PTR_ERR(priv->xram);
		goto free_netdev;
	}
	priv->p6_profile[0].path = "/etc/serdes/p6_serdes_sgmii_2g5.txt";
	priv->p6_profile[1].path = "/etc/serdes/p6_serdes_sgmii_1g.txt";
	if (platform_get_resource_byname(pdev, IORESOURCE_MEM, "p6-serdes")) {
		priv->p6_serdes = devm_platform_ioremap_resource_byname(pdev,
								       "p6-serdes");
		if (IS_ERR(priv->p6_serdes)) {
			ret = PTR_ERR(priv->p6_serdes);
			goto free_netdev;
		}
	}
	ret = of_property_read_u32(dev->of_node, "realtek,port", &priv->port);
	if (ret)
		goto free_netdev;
	priv->lan.port = priv->port;
	ret = of_get_mac_address(dev->of_node, mac);
	if (ret)
		goto free_netdev;
	eth_hw_addr_set(netdev, mac);
	ether_addr_copy(priv->base_mac, mac);
	netdev->netdev_ops = &rtl_netdev_ops;
	netdev->ethtool_ops = &rtl_ethtool_ops;
	netdev->max_mtu = ETH_DATA_LEN;
	spin_lock_init(&priv->tx_lock);
	spin_lock_init(&priv->rx_lock);
	mutex_init(&priv->mib_lock);
	INIT_DELAYED_WORK(&priv->mib_work, rtl_mib_work);
	netif_napi_add(netdev, &priv->napi, rtl_poll);
	hrtimer_setup(&priv->timer, rtl_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	priv->dma_tx = of_property_read_bool(dev->of_node, "realtek,dma-tx");
	if (priv->dma_tx) {
		priv->dma.registers = devm_platform_ioremap_resource(pdev, 2);
		if (IS_ERR(priv->dma.registers)) {
			ret = PTR_ERR(priv->dma.registers);
			goto del_napi;
		}
		priv->dma.port = priv->port;
		priv->dma.vp = 8;
		ret = rtl_dma_tx_alloc(dev, &priv->dma);
		if (ret)
			goto del_napi;
	}
	priv->qm_rx = of_property_read_bool(dev->of_node, "realtek,qm-rx");
	if (priv->qm_rx) {
		priv->qm.ni = priv->ni;
		priv->qm.axi_reorder = devm_platform_ioremap_resource(pdev, 3);
		if (IS_ERR(priv->qm.axi_reorder)) {
			ret = PTR_ERR(priv->qm.axi_reorder);
			goto free_dma;
		}
		priv->qm.block_reset = devm_platform_ioremap_resource(pdev, 4);
		if (IS_ERR(priv->qm.block_reset)) {
			ret = PTR_ERR(priv->qm.block_reset);
			goto free_dma;
		}
		ret = rtl_qm_rx_alloc(dev, &priv->qm);
		if (ret)
			goto free_dma;
		priv->rx_irq = platform_get_irq(pdev, 0);
		if (priv->rx_irq < 0) {
			ret = priv->rx_irq;
			goto free_dma;
		}
		rtl_update(priv, 0x6110, 0xff, 0);
		ret = request_irq(priv->rx_irq, rtl_rx_irq, 0, "rtl9617c-rx0", priv);
		if (ret)
			goto free_dma;
		priv->irq_requested = true;
		priv->fe = rtl_fe_allocate(dev, priv->ni);
		if (IS_ERR(priv->fe)) {
			ret = PTR_ERR(priv->fe);
			priv->fe = NULL;
			goto free_dma;
		}
	}
	platform_set_drvdata(pdev, netdev);
	ret = rtl_register_lans(priv, dev);
	if (ret)
		goto free_dma;
	schedule_delayed_work(&priv->mib_work, HZ);
	return 0;

free_dma:
	rtl_fe_free(priv->fe);
	if (priv->irq_requested)
		free_irq(priv->rx_irq, priv);
	if (priv->qm_rx && priv->qm.memory)
		rtl_qm_rx_free(dev, &priv->qm);
	if (priv->dma_tx)
		rtl_dma_tx_free(dev, &priv->dma);
del_napi:
	netif_napi_del(&priv->napi);
free_netdev:
	free_netdev(netdev);
	return ret;
}

static void rtl_remove(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct rtl_xram *priv = netdev_priv(dev);
	unsigned int port;

	cancel_delayed_work_sync(&priv->mib_work);
	priv->removing = true;
	for (port = 0; port < ARRAY_SIZE(priv->ports); port++)
		rtl_unregister_lan(priv, port);
	synchronize_net();
	rtl_fe_free(priv->fe);
	if (priv->irq_requested)
		free_irq(priv->rx_irq, priv);
	if (priv->qm_rx)
		rtl_qm_rx_free(&pdev->dev, &priv->qm);
	if (priv->dma_tx)
		rtl_dma_tx_free(&pdev->dev, &priv->dma);
	netif_napi_del(&priv->napi);
	free_netdev(dev);
}

static const struct of_device_id rtl_match[] = {
	{ .compatible = "realtek,rtl9617c-xram-eth" },

	{ .compatible = "realtek,rtl9617c-ni" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl_match);

static struct platform_driver rtl_driver = {
	.probe = rtl_probe,
	.remove = rtl_remove,
	.driver = { .name = "rtl9617c-xram", .of_match_table = rtl_match },
};
module_platform_driver(rtl_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RTL9617C management XRAM Ethernet");
