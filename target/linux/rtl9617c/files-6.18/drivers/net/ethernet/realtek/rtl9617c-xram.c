// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RTL9617C management XRAM Ethernet path, ported to Linux netdev/NAPI/phylib.
 * Packet format and sequences: Realtek Semiconductor Corp. 2022, GPL-2.0+,
 * GPL-for-GP3000 5090fbcb6ba743dd7b1314811ef557bad0460147,
 * bootloader/u-boot-2022.10/board/realtek_10G_PON/common/eth_ni.c.
 * This first LAN path retains the PHY setup performed by the stock bootloader.
 */
#include <linux/bitfield.h>
#include <linux/completion.h>
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
#include <linux/tyfza1-led.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "rtl9617c-xram-format.h"
#include "rtl9617c-dma.h"
#include "rtl9617c-qm.h"
#include "rtl9617c-ni.h"
#include <linux/rtl9617c-wifi.h>
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

/*
 * The 2.5G jack is the P6 XGE MAC, not a row in the NI port table: its MAC
 * lives at 0xf430b200..0xf430b2ac and its lane is the QX SerDes block at
 * 0xf550d000, configured exactly like the stock aal_sds_mode_set() path
 * (lane 1 + speed 1 = sgmii_2g5): release the shared serdes reset, power the
 * QX lane, write the interface mode word, replay the vendor serdes table from
 * /etc/serdes, wait for the serdes clock, then program the XGE MAC with the
 * 1G sequence (2.5G reuses it; only the lane table differs).
 */
#define RTL_P6_QX_BASE		0xf550d000UL
#define RTL_P6_MAX_WRITES	400
#define RTL_P6_MODE_SGMII_1G	0x4cf6
#define RTL_P6_MODE_SGMII_2G5	0x4cf5
#define RTL_P6_CLOCK_BIT	BIT(13)		/* QX_SDS_STATE ready */
#define RTL_P6_CLOCK_TRIES	1501
#define RTL_P6_CLOCK_STEP_MS	10

#define XGE_MAC_CFG_COM		0xb200
#define XGE_MAC_CFG_TX		0xb208
#define XGE_MAC_CFG_TX_2	0xb20c
#define XGE_MISC_CONFIG		0xb2a8
#define NITX_MISC_CFG		0xa1f4
#define L2TM_ES_CTRL		0x2300
#define QM_ES_CTRL		0x6108

struct rtl_p6_profile {
	const char *path;
	unsigned int speed;
	unsigned int count;
	int status;			/* 0 unread, 1 ready, <0 failed */
	u32 word[RTL_P6_MAX_WRITES][2];
};

struct rtl_xram {
	/* Primary netdev embeds the shared engine;other LANs only carry this
	 * initial per-port member and use the same NI/DMA/QM owner. */
	struct rtl_lan lan;
	struct net_device *netdev;
	struct net_device *ports[5];
	unsigned int registered_ports;
	struct work_struct lan4_work;
	struct completion lan4_ready;
	struct device *lan4_parent;
	struct device_node *lan4_phy;
	void __iomem *ni;
	void __iomem *xram;
	void __iomem *p6_serdes;
	struct rtl_p6_profile p6_profile[2];
	bool p6_started;
	struct phy *sgmii;
	unsigned int sgmii_speed;
	struct tyfza1_led phy_led;
	bool phy_led_registered;
	bool native_groups_created;
	bool wifi_started;
	struct napi_struct napi;
	struct hrtimer timer;
	spinlock_t tx_lock;
	spinlock_t rx_lock;
	struct mutex mib_lock;
	struct delayed_work mib_work;
	unsigned long long mib_total[5][RTL_NI_MIB_COUNTERS];
	bool rx_active;
	u32 port;
	bool tx_pending;
	unsigned int tx_length;
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

	/* Extend narrow counters independently of OAM/PM enable and client reads. */
	for (port = 0; port < ARRAY_SIZE(priv->ports); port++) {
		int ret;

		if (!(READ_ONCE(priv->registered_ports) & BIT(port)))
			continue;
		ret = rtl9617c_ni_pm_read(priv->netdev->dev.parent, port, counters);

		if (ret)
			dev_err_ratelimited(priv->netdev->dev.parent, "NI port%u MIB read failed: %d\n", port, ret);
	}
	schedule_delayed_work(&priv->mib_work, HZ);
}

int rtl9617c_ni_fe_add_port(struct device *dev,struct net_device *netdev,unsigned int llid)
{
	struct rtl_xram *priv=netdev_priv(dev_get_drvdata(dev));
	return rtl_fe_add_port(priv->fe,netdev,7,32+llid,llid);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_add_port);
int rtl9617c_ni_itu_paths(struct device *dev, struct net_device *netdev, const struct rtl_itu_paths *paths)
{
	struct rtl_xram *priv=netdev_priv(dev_get_drvdata(dev));
	return rtl_fe_set_itu_paths(priv->fe,netdev,paths);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_itu_paths);
void rtl9617c_ni_fe_remove_port(struct device *dev,struct net_device *netdev)
{
	struct rtl_xram *priv=netdev_priv(dev_get_drvdata(dev));
	rtl_fe_remove_port(priv->fe,netdev);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_remove_port);
int rtl9617c_ni_fe_setup_tc(struct device *dev,struct net_device *netdev,
	enum tc_setup_type type,void *data)
{
	struct rtl_xram *priv=netdev_priv(dev_get_drvdata(dev));
	return rtl_fe_setup_tc(priv->fe,netdev,type,data);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_setup_tc);
int rtl9617c_ni_fe_enable(struct device *dev)
{
	struct rtl_xram *priv=netdev_priv(dev_get_drvdata(dev));
	return rtl_fe_enable(priv->fe);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_enable);
int rtl9617c_ni_fe_disable(struct device *dev)
{
	struct rtl_xram *priv=netdev_priv(dev_get_drvdata(dev));
	return rtl_fe_disable(priv->fe);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_fe_disable);
ssize_t rtl9617c_ni_fe_state(struct device *dev,char *buffer)
{
	struct rtl_xram *priv=netdev_priv(dev_get_drvdata(dev));
	return rtl_fe_state(priv->fe,buffer);
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

/* Original aal_l3_tm_es_ni_port_ena_set: PON ports8..23 use bits0..15. */
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

static void rtl_p6_set_speed(struct rtl_xram *priv, unsigned int speed);

static void rtl_adjust_link(struct net_device *dev)
{
	struct rtl_lan *lan = netdev_priv(dev);
	struct phy_device *phy = dev->phydev;

	if (phy->link) {
		/* Port4's SGMII PCS requires GMII even at 10/100 Mbps. */
		rtl_update(lan->shared, PORT_STATIC + lan->port * PORT_STRIDE,
			   GENMASK(3, 0),
			   lan->port == 4 || phy->speed >= SPEED_1000 ? 0 : 1);
		rtl_update(lan->shared, PORT_GLB + lan->port * PORT_STRIDE,
			   BIT(0) | BIT(1), (phy->speed == SPEED_10 ? BIT(0) : 0) |
			   (phy->duplex == DUPLEX_HALF ? BIT(1) : 0));
		if (lan->port == 4 && lan->shared->sgmii_speed != phy->speed) {
			int ret;

			rtl_p6_set_speed(lan->shared, phy->speed);
			ret = phy_set_speed(lan->shared->sgmii, phy->speed);

			if (ret) {
				netdev_err(dev, "S3 speed %d failed: %d\n", phy->speed, ret);
				phy_error(phy);
				return;
			}
			lan->shared->sgmii_speed = phy->speed;
		}
	}
	phy_print_status(dev->phydev);
}

static void rtl_rx_mode(struct net_device *dev)
{
	struct rtl_xram *priv = rtl_shared(dev);
	bool accept_all = (dev->flags & (IFF_PROMISC | IFF_ALLMULTI)) ||
			  !netdev_mc_empty(dev);

	if (priv->qm_rx)
		return;
	/* Taurus's management filter uses mode 1 for promiscuous receive. */
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
		/* Complete and rearm form one critical section with engine shutdown. */
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
		if (rtl_dma_tx_available(&priv->dma)) {
			unsigned int port;

			for (port = 0; port < ARRAY_SIZE(priv->ports); port++)
				if (priv->ports[port] && netif_running(priv->ports[port]) &&
				    netif_queue_stopped(priv->ports[port]))
					netif_wake_queue(priv->ports[port]);
		}
	} else if (priv->tx_pending && rtl_pointer(priv, TX_READ) == rtl_pointer(priv, TX_WRITE)) {
		priv->tx_pending = false;
		priv->netdev->stats.tx_packets++;
		priv->netdev->stats.tx_bytes += priv->tx_length;
		netif_wake_queue(priv->netdev);
	}
	spin_unlock(&priv->tx_lock);
	hrtimer_forward_now(timer, ms_to_ktime(1));
	return HRTIMER_RESTART;
}

static irqreturn_t rtl_rx_irq(int irq, void *data)
{
	struct rtl_xram *priv = data;

	rtl_update(priv, 0x6110, 0xff, 0);
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
		const struct rtl9617c_ni_frame frame = {
			.header_high = BIT(13),
			.header_low = (16 << 9) | (lan->port << 3) | BIT(29),
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
	netif_stop_queue(dev);
	wmb();
	writel(rtl_xram_tx_next(pointer, wire), priv->ni + TX_WRITE);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
}

/* One decimal/hex token per call. kstrtoul() rejects trailing text, and a
 * serdes line carries four fields, so the number is copied out first. */
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
	/* kernel_read_file_from_path() allocates the buffer itself and only
	 * does so when *buf is NULL: an uninitialised pointer is used as the
	 * read destination and faults inside copy_page_to_iter(). */
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

/* Replaying the table is what the stock device tree calls a lane load: each
 * line writes QX_REG_DATAIN, then QX_REG_ACCESS to trigger it. */
static void rtl_p6_apply(struct rtl_xram *priv, struct rtl_p6_profile *profile)
{
	unsigned int i;

	for (i = 0; i < profile->count; i++)
		writel(profile->word[i][1], priv->p6_serdes + profile->word[i][0]);
}

static bool rtl_p6_clock_ready(struct rtl_xram *priv)
{
	return readl(priv->p6_serdes + 0x58) & RTL_P6_CLOCK_BIT;
}

static void rtl_p6_mode_word(struct rtl_xram *priv, u32 word)
{
	writel((readl(priv->p6_serdes + 0x8) & ~0x1ffffU) | (word & 0x1ffffU),
	       priv->p6_serdes + 0x8);
}

/* __p6_mac_1g_config(): the XGE MAC sequence shared by 1G and 2.5G. */
static void rtl_p6_mac_config(struct rtl_xram *priv)
{
	void __iomem *ni = priv->ni;
	u32 es, qm, value;

	/* Hold L2 learning/forwarding while the MAC is reprogrammed. */
	es = readl(ni + L2TM_ES_CTRL);
	writel(es & ~BIT(6), ni + L2TM_ES_CTRL);
	qm = readl(ni + QM_ES_CTRL);
	writel((qm & 0xffff0000U) | (qm & 0xffU) | ((qm >> 8) & 0xbfU) << 8,
	       ni + QM_ES_CTRL);
	writel(readl(ni + NITX_MISC_CFG) | BIT(20), ni + NITX_MISC_CFG);

	value = readl(ni + XGE_MISC_CONFIG) & 0xcfffffffU;
	writel(value | 0x80000000U, ni + XGE_MISC_CONFIG);
	writel(value | 0xb0000000U, ni + XGE_MISC_CONFIG);
	mdelay(1);

	writel(readl(ni + XGE_MAC_CFG_TX) & ~0x3U, ni + XGE_MAC_CFG_TX);
	writel(readl(ni + XGE_MAC_CFG_TX) | 1, ni + XGE_MAC_CFG_TX);
	value = readl(ni + XGE_MAC_CFG_TX_2);
	writel((value & 0xffffe000U) | (value & 0xe0U) | 0xa00U | 10,
	       ni + XGE_MAC_CFG_TX_2);
	writel((readl(ni + XGE_MAC_CFG_COM) & 0xffffdf8fU) | 0xc000U,
	       ni + XGE_MAC_CFG_COM);
	mdelay(1);
	writel(readl(ni + XGE_MAC_CFG_COM) & 0xffff1f8fU, ni + XGE_MAC_CFG_COM);

	value = readl(ni + XGE_MISC_CONFIG);
	writel((value & 0xcfffffffU) | 0x80000000U, ni + XGE_MISC_CONFIG);
	writel((value & 0xc0000000U) | 0x80000000U | (value & 0x0fffffffU) |
	       0x10000000U, ni + XGE_MISC_CONFIG);
	writel(readl(ni + NITX_MISC_CFG) & 0xffefffffU, ni + NITX_MISC_CFG);

	writel(readl(ni + L2TM_ES_CTRL) | BIT(6), ni + L2TM_ES_CTRL);
	writel((qm & 0xffff0000U) | (qm & 0xffU) | (((qm >> 8) & 0xffU) | 0x40) << 8,
	       ni + QM_ES_CTRL);
}

static void rtl_p6_bringup(struct rtl_xram *priv)
{
	unsigned int i;
	u32 value;

	if (!priv->p6_serdes || priv->p6_started)
		return;
	priv->p6_started = true;

	/* GLOBAL_BLOCK_RESET bit31 pulses the shared SerDes logic. */
	if (priv->qm.block_reset) {
		value = readl(priv->qm.block_reset);
		writel(value | BIT(31), priv->qm.block_reset);
		udelay(1);
		writel(value & ~BIT(31), priv->qm.block_reset);
	}

	/* Power the QX lane and select the SGMII 2.5G interface mode. */
	writel(readl(priv->p6_serdes) | BIT(0), priv->p6_serdes);
	writel(readl(priv->p6_serdes + 0x4) | 0x1f, priv->p6_serdes + 0x4);
	mdelay(1);
	writel((readl(priv->p6_serdes + 0x4) & ~0x1fU) | 2, priv->p6_serdes + 0x4);
	rtl_p6_mode_word(priv, RTL_P6_MODE_SGMII_2G5);

	if (rtl_p6_read(priv, &priv->p6_profile[0]) == 0)
		rtl_p6_apply(priv, &priv->p6_profile[0]);
	else
		dev_warn(priv->netdev->dev.parent, "serdes load %s failed\n",
			 priv->p6_profile[0].path);

	for (i = 0; i < RTL_P6_CLOCK_TRIES && !rtl_p6_clock_ready(priv); i++)
		mdelay(RTL_P6_CLOCK_STEP_MS);
	dev_info(priv->netdev->dev.parent,
		 "P6 serdes: %u writes, clock %s, state %08x\n",
		 priv->p6_profile[0].count,
		 rtl_p6_clock_ready(priv) ? "ready" : "timeout",
		 readl(priv->p6_serdes + 0x58));

	rtl_p6_mac_config(priv);
}

/* A 1G peer needs the matching lane load; 2.5G keeps the sgmii_2g5 profile. */
static void rtl_p6_set_speed(struct rtl_xram *priv, unsigned int speed)
{
	struct rtl_p6_profile *profile = &priv->p6_profile[speed == SPEED_2500 ? 0 : 1];

	if (!priv->p6_serdes)
		return;
	if (!rtl_p6_read(priv, profile) && profile->count)
		rtl_p6_apply(priv, profile);
	rtl_p6_mode_word(priv, speed == SPEED_2500 ? RTL_P6_MODE_SGMII_2G5 :
			 RTL_P6_MODE_SGMII_1G);
}

static int rtl_engine_start(struct rtl_xram *priv)
{
	struct net_device *dev = priv->netdev;
	unsigned int port;
	u32 config;
	int ret;

	/* The 2.5G lane must be alive before the switch fabric forwards to it. */
	if (priv->ports[4])
		rtl_p6_bringup(priv);

	if (priv->dma_tx) {
		/* The bootloader's XRAM path leaves the normal LAN scheduler off. */
		/* Original aal_l2_qm_init: EQ1 has4K64-byte pages, with29 pages
		 * reserved per port so one full frame survives shared-pool pressure. */
		writel(BIT(31) | (4U << 16) | (29U << 20), priv->ni + 0x2208);
		rtl_update(priv, 0x2218, GENMASK(14, 0), 29);
		writel(0x08008021, priv->ni + 0x2210);
		/* Port15 uses scheduler instance14. Leaving it stopped
		 * holds VOQ120..127 buffers until every LAN/PON data path runs out. */
		rtl_update(priv, 0x2304 + 12 * 14, 0xff, 0xff);
		rtl_update(priv, 0x2300, BIT(31) | BIT(15), BIT(31) | BIT(15));
		for (port = 0; port < ARRAY_SIZE(priv->ports); port++)
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
			rtl_dma_tx_stop(&priv->dma);
			return ret;
		}
		for (port = 0; port < ARRAY_SIZE(priv->ports); port++)
			if (priv->ports[port])
				rtl_update(priv, 0xa5f8 + PORT_STRIDE * port,
					   BIT(28) | GENMASK(21, 16) | BIT(31), BIT(28) | (16 << 16));
	}

	rtl_update(priv, GLB_STATIC, PORT_TO_CPU, priv->dma_tx ? 0xf : priv->port);
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
	if (priv->qm_rx) {
		spin_lock_irqsave(&priv->rx_lock, flags);
		priv->rx_active = false;
		rtl_update(priv, 0x6110, 0xff, 0);
		spin_unlock_irqrestore(&priv->rx_lock, flags);
		synchronize_irq(priv->rx_irq);
	}
	napi_disable(&priv->napi);
	if (priv->qm_rx) {
		/* A completing poll may release NAPI ownership before dropping rx_lock. */
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

	/* Both netdev open/stop paths are serialized by RTNL. */
	if (priv->engine_users) {
		priv->engine_users++;
		return 0;
	}
	/* QM ingress and schedulers only cover ports published before the first start. */
	wait_for_completion(&priv->lan4_ready);
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
	/* Stock aal_set_active_port releases this port's interface and RX/TX resets. */
	rtl_update(priv, 0xa008, BIT(lan->port) | BIT(lan->port + 6) | BIT(lan->port + 12), 0);
	rtl_update(priv, PORT_STATIC + lan->port * PORT_STRIDE,
		   GENMASK(31, 24), (u32)dev->dev_addr[5] << 24);
	rtl_update(priv, PORT_RX + lan->port * PORT_STRIDE, BIT(0), BIT(0));
	rtl_update(priv, PORT_TX + lan->port * PORT_STRIDE, BIT(0), BIT(0));
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
	rtl_update(priv, PORT_RX + lan->port * PORT_STRIDE, BIT(0), 0);
	rtl_update(priv, PORT_TX + lan->port * PORT_STRIDE, BIT(0), 0);
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

	rtl_fe_flush_port(priv->fe,owner);
	spin_lock_irqsave(&priv->tx_lock, flags);
	rtl_dma_tx_forget(&priv->dma, owner);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	rtl_engine_put(priv);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_close);

netdev_tx_t rtl9617c_ni_transmit(struct device *dev, struct net_device *owner,
			       struct sk_buff *skb, const struct rtl9617c_ni_frame *frame)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned long flags;
	netdev_tx_t result;

	spin_lock_irqsave(&priv->tx_lock, flags);
	result = rtl_dma_tx_frame(&priv->dma, skb, owner, frame);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return result;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_transmit);

int rtl9617c_ni_wifi_lookup(struct device *dev, struct sk_buff *skb, unsigned int id)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&priv->tx_lock, flags);
	ret = rtl_dma_tx_wifi(&priv->dma, priv->netdev, skb, id);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_wifi_lookup);

int rtl9617c_ni_wifi_attach(struct device *dev, struct rtl9617c_ni_wifi_endpoint *endpoint)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned int id;
	int ret;

	ASSERT_RTNL();
	if (!priv->dma_tx || !priv->qm_rx || !priv->fe) return -EOPNOTSUPP;
	for (id = 1; id <= RTL_WIFI_ID_MAX; id++)
		if (!rcu_access_pointer(priv->qm.wifi[id])) break;
	if (id > RTL_WIFI_ID_MAX) return -ENOSPC;
	ret = rtl_engine_get(priv);
	if (ret) return ret;
	ret = rtl_fe_add_wifi_port(priv->fe, endpoint->netdev, id);
	if (ret) { rtl_engine_put(priv); return ret; }
	dev_hold(endpoint->netdev);
	rcu_assign_pointer(priv->qm.wifi[id], endpoint);
	return id;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_wifi_attach);

int rtl9617c_ni_wifi_id(struct device *dev, struct net_device *netdev)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned int id;
	int ret = -ENODEV;
	rcu_read_lock();
	for (id = 1; id <= RTL_WIFI_ID_MAX; id++) {
		struct rtl9617c_ni_wifi_endpoint *endpoint = rcu_dereference(priv->qm.wifi[id]);
		if (endpoint && endpoint->netdev == netdev) { ret = id; break; }
	}
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_wifi_id);

int rtl9617c_ni_wifi_detach(struct device *dev, unsigned int id)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	struct rtl9617c_ni_wifi_endpoint *endpoint = rcu_access_pointer(priv->qm.wifi[id]);
	u32 status;
	unsigned long flags;
	int ret;

	ASSERT_RTNL();
	/* The caller has removed its ingress hook. Withdraw rules, drain copied
	 * NI submissions and old EPPs, then release the ID and netdev reference. */
	/* Keep the flow-block owner across NETDEV_DOWN/UP; UNREGISTER retires it. */
	ret = rtl_fe_flush_port(priv->fe, endpoint->netdev);
	if (ret && !priv->removing) return ret;
	spin_lock_irqsave(&priv->tx_lock, flags);
	ret = readl_poll_timeout_atomic(priv->dma.registers + 0x100 + priv->dma.vp * 0xa0 + 0x1c,
		status, (status & 63) == priv->dma.write, 1, 100000);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	/* A busy shared queue retains this identity. Removing the NI supplier
	 * releases all identities together; none can be reassigned afterward. */
	if (ret && !priv->removing) return ret;
	usleep_range(1000, 2000);
	napi_disable(&priv->napi);
	local_bh_disable();
	rtl_qm_rx_poll(&priv->qm, &priv->napi, 1024);
	local_bh_enable();
	rcu_assign_pointer(priv->qm.wifi[id], NULL);
	napi_enable(&priv->napi);
	/* An IRQ during napi_disable can mask RX without scheduling a poll. */
	napi_schedule(&priv->napi);
	synchronize_net();
	dev_put(endpoint->netdev);
	rtl_engine_put(priv);
	return 0;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_wifi_detach);

bool rtl9617c_ni_wifi_enabled(struct device *dev)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	return rtl_fe_enabled(priv->fe);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_wifi_enabled);

int rtl9617c_ni_wifi_peer_route(struct device *dev, struct net_device *netdev,
	const u8 *address, unsigned int destination)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	return rtl_fe_wifi_peer_route(priv->fe, netdev, address, destination);
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_wifi_peer_route);

int rtl9617c_ni_oam_configure(struct device *dev)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	static const unsigned int index[] = {15, 0x8f, 0x4f, 0xcf};
	u32 status;
	unsigned int i;
	int ret;

	/* aal_port: NI7 ninth queue (LDPID15) enters the OAM physical port12. */
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

static int rtl_setup_tc(struct net_device *netdev,enum tc_setup_type type,void *data)
{
	struct rtl_xram *priv=rtl_shared(netdev);
	return rtl_fe_setup_tc(priv->fe,netdev,type,data);
}
static const struct net_device_ops rtl_netdev_ops = {
	.ndo_setup_tc = rtl_setup_tc,
	.ndo_open = rtl_open,
	.ndo_stop = rtl_stop,
	.ndo_start_xmit = rtl_xmit,
	.ndo_set_rx_mode = rtl_rx_mode,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_eth_ioctl = phy_do_ioctl_running,
};

static const struct ethtool_ops rtl_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = phy_ethtool_get_link_ksettings,
	.set_link_ksettings = phy_ethtool_set_link_ksettings,
	.nway_reset = phy_ethtool_nway_reset,
};

static int rtl_phy_led_output(struct tyfza1_led *led, unsigned int value)
{
	struct rtl_xram *priv = container_of(led, struct rtl_xram, phy_led);
	/* Original LED0: 0=off, 1=on, 2=PHY link/activity (LCR7 bits0/4). */
	return phy_modify_mmd(priv->ports[4]->phydev, MDIO_MMD_VEND2,
			      0xd044, 0x11, value == 2 ? 0x10 : value);
}

static void rtl_unregister_lan(struct rtl_xram *priv, unsigned int port)
{
	struct net_device *dev = priv->ports[port];
	unsigned long flags;

	if (!dev)
		return;
	if (port == 4 && priv->phy_led_registered) {
		tyfza1_led_unregister(&priv->phy_led);
		priv->phy_led_registered = false;
	}
	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->ports[port] = NULL;
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

static void rtl_sgmii_power_off(void *data)
{
	phy_power_off(data);
}

static int rtl_register_lan(struct rtl_xram *priv, struct device *parent,
			    unsigned int port, struct device_node *phy)
{
	struct phy_device *phydev;
	struct net_device *dev = NULL;
	struct rtl_lan *lan;
	bool sgmii_powered = false, registration_locked = false;
	unsigned int index;
	u8 mac[ETH_ALEN];
	int ret;

	/*
	 * Port 4 is the SGMII/2.5G jack. Measured with the ports_state dump on
	 * this board: its port block at 0xa5c0 + 0x90 * 4 reads back port 0's
	 * values (same static word including port 0's MAC byte, RXMAC 0x1100
	 * with rx_en clear), so the NI port-static table only decodes four
	 * ports and anything written for port 4 aliases port 0. The 2.5G MAC
	 * therefore still needs its own receive setup somewhere else; until
	 * that is found this port links through the PHY but passes no frames.
	 */
	if (port == 4) {
		priv->sgmii = devm_phy_get(parent, "sgmii");
		if (IS_ERR(priv->sgmii)) {
			ret = PTR_ERR(priv->sgmii);
			goto put_phy;
		}
		ret = phy_power_on(priv->sgmii);
		if (ret)
			goto put_phy;
		ret = devm_add_action_or_reset(parent, rtl_sgmii_power_off, priv->sgmii);
		if (ret)
			goto put_phy;
		sgmii_powered = true;
	}
	dev = port == priv->port ? priv->netdev : alloc_etherdev(sizeof(*lan));
	if (!dev) {
		ret = -ENOMEM;
		goto put_phy;
	}
	priv->ports[port] = dev;
	SET_NETDEV_DEV(dev, parent);
	/* Port 4 is the SGMII lane. It is lan4 while port 3 stays unwired (the
	 * TYFZA1 2.5G tower) and lan5 on a board that also wires port 3. */
	index = port == 4 ? 4 : port + 1;
	if (port == 4 && priv->ports[3])
		index = 5;
	snprintf(dev->name, sizeof(dev->name), "lan%u", index);
	lan = netdev_priv(dev);
	lan->shared = priv;
	lan->port = port;
	dev->netdev_ops = &rtl_netdev_ops;
	dev->ethtool_ops = &rtl_ethtool_ops;
	dev->max_mtu = ETH_DATA_LEN;
	ether_addr_copy(mac, priv->netdev->dev_addr);
	mac[5] += port - priv->port;
	eth_hw_addr_set(dev, mac);
	phydev = of_phy_find_device(phy);
	if (!phydev) {
		ret = -ENODEV;
		goto put_phy;
	}
	ret = phy_connect_direct(dev, phydev, rtl_adjust_link,
		port == 4 ? PHY_INTERFACE_MODE_SGMII : PHY_INTERFACE_MODE_GMII);
	put_device(&phydev->mdio.dev);
	if (ret)
		goto put_phy;
	ret = rtl_fe_add_port(priv->fe, dev, port, port, 0);
	if (ret)
		goto put_phy;
	rcu_assign_pointer(priv->qm.lan[port], dev);
	/* Publish LAN4 before taking RTNL for registration: an engine start
	 * waiting on lan4_ready under RTNL must not block this work. */
	if (port == 4) {
		complete_all(&priv->lan4_ready);
		rtnl_lock();
		registration_locked = true;
	}
	/* Synchronous ports are registered under rtl_register_lans()' RTNL. */
	ret = register_netdevice(dev);
	if (ret)
		goto put_phy;
	WRITE_ONCE(priv->registered_ports, priv->registered_ports | BIT(port));
	if (port == 4) {
		priv->phy_led.output = rtl_phy_led_output;
		priv->phy_led.requested = 2;
		priv->phy_led.cdev.name = "LED_8221B_P4_0";
		priv->phy_led.cdev.max_brightness = 2;
		ret = tyfza1_led_register(parent, &priv->phy_led, NULL);
		if (ret)
			dev_warn(parent, "LAN4 2.5G LED initialization failed: %d\n", ret);
		else
			priv->phy_led_registered = true;
	}
	dev_info(parent, "%s: LAN port %u %s, PHY %08x, MAC %pM\n",
		 dev->name, port, priv->qm_rx ? "native NI/DMA" : "management XRAM",
		 dev->phydev->phy_id, dev->dev_addr);
	of_node_put(phy);
	if (registration_locked)
		rtnl_unlock();
	return 0;

put_phy:
	of_node_put(phy);
	if (dev)
		rtl_unregister_lan(priv, port);
	if (sgmii_powered)
		devm_release_action(parent, rtl_sgmii_power_off, priv->sgmii);
	if (registration_locked)
		rtnl_unlock();
	return ret;
}

static void rtl_lan4_work(struct work_struct *work)
{
	struct rtl_xram *priv = container_of(work, struct rtl_xram, lan4_work);
	int ret = rtl_register_lan(priv, priv->lan4_parent, 4, priv->lan4_phy);

	if (ret)
		dev_err(priv->lan4_parent, "LAN4 initialization failed: %d; other ports retained\n", ret);
	complete_all(&priv->lan4_ready);
}

static void rtl_register_lans(struct rtl_xram *priv, struct device *parent)
{
	struct device_node *phy;
	unsigned int port;
	int ret;

	/* Do not expose an openable port until all synchronous ports are
	 * published and LAN4 work is queued (or its completion is signalled). */
	rtnl_lock();
	for (port = 0; port < ARRAY_SIZE(priv->ports); port++) {
		phy = of_parse_phandle(parent->of_node,
			port == priv->port ? "phy-handle" : "realtek,lan-phys",
			port == priv->port ? 0 : port);
		if (!phy) {
			/* A port without a PHY is not registered; say so instead
			 * of leaving ip link without any hint of the cause. */
			dev_warn(parent, "LAN port %u has no %s; not registered\n", port,
				 port == priv->port ? "phy-handle" : "realtek,lan-phys");
			continue;
		}
		if (port == 4) {
			/* Like the OEM rtk_rtl8221b_device_init_thread, set up
			 * the external RTL8221B off the boot path: its HiSGMII
			 * force-down poll can take the full 100 x 100ms. */
			priv->lan4_parent = parent;
			priv->lan4_phy = phy;
			INIT_WORK(&priv->lan4_work, rtl_lan4_work);
			schedule_work(&priv->lan4_work);
			continue;
		}
		ret = rtl_register_lan(priv, parent, port, phy);
		if (ret)
			dev_err(parent, "LAN%u initialization failed: %d; other ports retained\n",
				port + 1, ret);
	}
	if (!priv->lan4_phy)
		complete_all(&priv->lan4_ready);
	rtnl_unlock();
}

static ssize_t xram_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	struct rtl_xram *priv = netdev_priv(netdev);

	return sysfs_emit(buf, "port=%u cfg=%08x static=%08x rx=%u/%u tx=%u/%u pending=%u\n",
		priv->port, readl(priv->ni + XRAM_CFG), readl(priv->ni + GLB_STATIC),
		rtl_pointer(priv, RX_READ), rtl_pointer(priv, RX_WRITE),
		rtl_pointer(priv, TX_READ), rtl_pointer(priv, TX_WRITE), priv->tx_pending);
}
static DEVICE_ATTR_RO(xram_state);

/*
 * Per-port register block, port 0..4 (0xa5c0 + 0x90 * port):
 *   +0x00 static, +0x04 link/speed, +0x08 RXMAC, +0x0c RXMAC WOL,
 *   +0x10 MAC status, +0x14 TXMAC, then the rest of the block.
 * Bring-up aid: the SGMII port's receive path can only be compared against a
 * working GE port if both register sets are visible.
 */
static ssize_t ports_state_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	struct rtl_xram *priv = netdev_priv(netdev);
	unsigned int port, reg;
	ssize_t len = 0;

	for (port = 0; port < ARRAY_SIZE(priv->ports); port++) {
		len += sysfs_emit_at(buf, len, "port%u:", port);
		for (reg = 0; reg < 12; reg++)
			len += sysfs_emit_at(buf, len, " %08x",
					     readl(priv->ni + PORT_STATIC +
						   PORT_STRIDE * port + reg * 4));
		len += sysfs_emit_at(buf, len, "\n");
	}
	len += sysfs_emit_at(buf, len, "glb_static=%08x sel=%08x/%08x/%08x/%08x\n",
			     readl(priv->ni + GLB_STATIC),
			     readl(priv->ni + 0xa1e0), readl(priv->ni + 0xa1e4),
			     readl(priv->ni + 0xa1e8), readl(priv->ni + 0xa1ec));
	len += sysfs_emit_at(buf, len, "xge_com=%08x tx=%08x tx2=%08x misc=%08x nitx=%08x\n",
			     readl(priv->ni + XGE_MAC_CFG_COM),
			     readl(priv->ni + XGE_MAC_CFG_TX),
			     readl(priv->ni + XGE_MAC_CFG_TX_2),
			     readl(priv->ni + XGE_MISC_CONFIG),
			     readl(priv->ni + NITX_MISC_CFG));
	if (priv->p6_serdes)
		len += sysfs_emit_at(buf, len, "qx=%08x/%08x/%08x state=%08x\n",
				     readl(priv->p6_serdes), readl(priv->p6_serdes + 0x4),
				     readl(priv->p6_serdes + 0x8),
				     readl(priv->p6_serdes + 0x58));
	return len;
}
static DEVICE_ATTR_RO(ports_state);

static ssize_t dma_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return priv->dma_tx ? rtl_dma_tx_state(&priv->dma, buf) : sysfs_emit(buf, "XRAM TX\n");
}
static DEVICE_ATTR_RO(dma_state);

static ssize_t datapath_profile_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return sysfs_emit(buf, "enabled=%u host_RX_poll_ns=%llu host_RX_polls=%llu "
		"WiFi_forwarded_packets=%llu WiFi_miss_packets=%llu WiFi_RX_copy_bytes=%llu "
		"WiFi_RX_copy_ns=%llu WiFi_dispatch_ns=%llu "
		"WiFi_lookup_packets=%llu WiFi_TX_copy_bytes=%llu WiFi_TX_prepare_ns=%llu\n",
		priv->qm.profile, priv->qm.host_poll_ns, priv->qm.host_polls,
		priv->qm.wifi_forwarded, priv->qm.wifi_missed, priv->qm.wifi_copy_bytes,
		priv->qm.wifi_copy_ns, priv->qm.wifi_dispatch_ns,
		priv->dma.wifi_packets, priv->dma.wifi_copy_bytes, priv->dma.wifi_prepare_ns);
}

static ssize_t datapath_profile_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	bool enabled;
	int ret = kstrtobool(buf, &enabled);

	if (ret)
		return ret;
	WRITE_ONCE(priv->qm.profile, enabled);
	WRITE_ONCE(priv->dma.profile, enabled);
	return count;
}
static DEVICE_ATTR_RW(datapath_profile);

static ssize_t pipeline_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return sysfs_emit(buf, "l2tm_es=%08x l2tm_port0=%08x qm_eq=%08x ni_internal=%08x ni_nitx=%08x ni_qm=%08x rx_hdr=%08x/%08x/%08x rx_pkts=%u rx_out=%08x/%08x l2qm_free=%08x\n",
		readl(priv->ni + 0x2300), readl(priv->ni + 0x2304),
		readl(priv->ni + 0x2208), readl(priv->ni + 0xa1bc), readl(priv->ni + 0xa1f4),
		readl(priv->ni + 0xa120), readl(priv->ni + 0xa088),
		readl(priv->ni + 0xa08c), readl(priv->ni + 0xa090), readl(priv->ni + 0xa094),
		readl(priv->ni + 0xa10c), readl(priv->ni + 0xa110), readl(priv->ni + 0x2234));
}
static DEVICE_ATTR_RO(pipeline_state);

static ssize_t qm_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return priv->qm_rx ? rtl_qm_rx_state(&priv->qm, buf) : sysfs_emit(buf, "XRAM RX\n");
}
static DEVICE_ATTR_RO(qm_state);

static ssize_t qm_trace_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	return rtl_qm_rx_trace(&priv->qm, buf);
}

static ssize_t qm_trace_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	priv->qm.trace_count = 0;
	return count;
}
static DEVICE_ATTR_RW(qm_trace);

static ssize_t qm_pool_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	return rtl_qm_rx_pool(&priv->qm, buf);
}
static DEVICE_ATTR_RO(qm_pool);

static ssize_t pon_deepq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return rtl_qm_deepq_state(&priv->qm, buf);
}
static DEVICE_ATTR_RO(pon_deepq);

static ssize_t qm_wait_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned int value = FIELD_GET(GENMASK(27, 26), readl(priv->ni + 0x6108));

	return sysfs_emit(buf, "%u\n", value);
}

static ssize_t qm_wait_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	unsigned int value;
	int ret = kstrtouint(buf, 0, &value);

	if (ret)
		return ret;
	if (value > 3)
		return -EINVAL;
	rtl_update(priv, 0x6108, GENMASK(27, 26), FIELD_PREP(GENMASK(27, 26), value));
	return count;
}
static DEVICE_ATTR_RW(qm_wait);

static ssize_t qm_es_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	return sysfs_emit(buf, "%08x\n", readl(priv->ni + 0x6108));
}

static ssize_t qm_es_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	u32 value;
	int ret = kstrtou32(buf, 0, &value);
	if (ret)
		return ret;
	writel(value, priv->ni + 0x6108);
	return count;
}
static DEVICE_ATTR_RW(qm_es);

static ssize_t qm_buffer_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	return sysfs_emit(buf, "%08x\n", readl(priv->ni + 0x68b0));
}

static ssize_t qm_buffer_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	u32 value;
	int ret = kstrtou32(buf, 0, &value);
	if (ret)
		return ret;
	writel(value, priv->ni + 0x68b0);
	return count;
}
static DEVICE_ATTR_RW(qm_buffer);

/* Direct ingress to CPU0 isolates NI/QM from the classification pipeline. */
static ssize_t rx_bypass_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));

	return sysfs_emit(buf, "%08x\n", readl(priv->ni + 0xa5f8 + PORT_STRIDE * priv->port));
}

static ssize_t rx_bypass_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(dev));
	bool enabled;
	int ret = kstrtobool(buf, &enabled);

	if (ret)
		return ret;
	rtl_update(priv, 0xa5f8 + PORT_STRIDE * priv->port,
		   BIT(28) | GENMASK(21, 16) | BIT(31),
		   enabled ? BIT(28) | (16 << 16) : 0);
	return count;
}
static DEVICE_ATTR_RW(rx_bypass);

static struct attribute *rtl_native_attrs[] = {
	&dev_attr_xram_state.attr,
	&dev_attr_ports_state.attr,
	&dev_attr_dma_state.attr,
	&dev_attr_datapath_profile.attr,
	&dev_attr_pipeline_state.attr,
	&dev_attr_qm_state.attr,
	&dev_attr_qm_trace.attr,
	&dev_attr_qm_pool.attr,
	&dev_attr_pon_deepq.attr,
	&dev_attr_qm_wait.attr,
	&dev_attr_qm_es.attr,
	&dev_attr_qm_buffer.attr,
	&dev_attr_rx_bypass.attr,
	NULL,
};
static umode_t rtl_native_visible(struct kobject *kobj, struct attribute *attr, int index)
{
	struct rtl_xram *priv = netdev_priv(dev_get_drvdata(kobj_to_dev(kobj)));

	if (!priv->qm_rx &&
	    (attr == &dev_attr_qm_pool.attr || attr == &dev_attr_pon_deepq.attr || attr == &dev_attr_qm_trace.attr ||
	     attr == &dev_attr_qm_wait.attr || attr == &dev_attr_qm_es.attr ||
	     attr == &dev_attr_qm_buffer.attr))
		return 0;
	return attr->mode;
}

static const struct attribute_group rtl_native_group = {
	.attrs = rtl_native_attrs,
	.is_visible = rtl_native_visible,
};

static const struct attribute_group *rtl_native_groups[] = {
	&rtl_native_group,
	NULL,
};

static int rtl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *netdev;
	struct rtl_xram *priv;
	u8 mac[ETH_ALEN];
	u32 value;
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
	priv->p6_profile[0].speed = SPEED_2500;
	priv->p6_profile[1].path = "/etc/serdes/p6_serdes_sgmii_1g.txt";
	priv->p6_profile[1].speed = SPEED_1000;
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
	snprintf(netdev->name, sizeof(netdev->name), "lan%u", priv->port + 1);
	value = readl(priv->ni + MAC_ADDR0);
	put_unaligned_be32(value, mac);
	mac[4] = readl(priv->ni + MAC_ADDR1);
	mac[5] = readl(priv->ni + PORT_STATIC + priv->port * PORT_STRIDE) >> 24;
	eth_hw_addr_set(netdev, mac);
	netdev->netdev_ops = &rtl_netdev_ops;
	netdev->ethtool_ops = &rtl_ethtool_ops;
	netdev->max_mtu = ETH_DATA_LEN;
	spin_lock_init(&priv->tx_lock);
	spin_lock_init(&priv->rx_lock);
	mutex_init(&priv->mib_lock);
	init_completion(&priv->lan4_ready);
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
	}
	if(priv->qm_rx) {
		priv->fe=rtl_fe_allocate(dev,priv->ni);
		if(IS_ERR(priv->fe)){ret=PTR_ERR(priv->fe);priv->fe=NULL;goto free_dma;}
	}
	platform_set_drvdata(pdev, netdev);
	rtl_register_lans(priv, dev);
	ret = sysfs_create_groups(&dev->kobj, rtl_native_groups);
	if (ret)
		dev_warn(dev, "NI diagnostic attributes unavailable: %d\n", ret);
	else
		priv->native_groups_created = true;
	if (priv->qm_rx && priv->dma_tx) {
		ret = rtl_wifi_start(dev);
		if (ret)
			dev_warn(dev, "NI Wi-Fi notifier unavailable: %d; LAN ports retained\n", ret);
		else
			priv->wifi_started = true;
	}
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

	if (priv->lan4_phy)
		flush_work(&priv->lan4_work);
	cancel_delayed_work_sync(&priv->mib_work);
	priv->removing = true;
	if (priv->wifi_started) rtl_wifi_stop();
	if (priv->native_groups_created)
		sysfs_remove_groups(&pdev->dev.kobj, rtl_native_groups);
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
	/* Board trees name the same engine after the block ("ni"); the PON
	 * driver resolves this node as its realtek,network-engine. */
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
