/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL9617C_NI_PRIV_H
#define RTL9617C_NI_PRIV_H

#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/phylink.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include "rtl9617c-dma.h"
#include "rtl9617c-led.h"
#include "rtl9617c-mib.h"
#include "rtl9617c-ni-regs.h"
#include "rtl9617c-qm.h"

#define RTL_PHY_LED_COUNT	3

struct rtl9617c_ni;
struct rtl9617c_port {
	struct rtl9617c_ni *shared;
	unsigned int port;
	struct phylink *phylink;
	struct phylink_config phylink_config;
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

struct rtl_p6_profile {
	const char *path;
	unsigned int count;
	int status;
	u32 word[RTL_P6_MAX_WRITES][2];
};

static const struct regmap_config rtl_ni_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.max_register = 0xffff,
	.fast_io = true,
};

struct rtl9617c_ni {
	struct device *dev;
	struct net_device *dummy;
	struct net_device *netdev;
	u8 base_mac[ETH_ALEN];
	struct net_device *ports[7];
	unsigned int registered_ports;
	struct regmap *regs;
	void __iomem *ni;
	void __iomem *xram;
	void __iomem *p6_serdes;
	struct rtl_p6_profile p6_profile[2];
	bool p6_started;
	u32 p6_rx_bypass_saved;
	bool p6_rx_bypass_active;
	struct phy *sgmii;
	bool sgmii_on;
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

void rtl9617c_ni_mib_work(struct work_struct *work);
static inline void rtl_update(struct rtl9617c_ni *priv, unsigned int reg, u32 mask, u32 value)
{
	rtl_ni_write(priv->regs, reg, (rtl_ni_read(priv->regs, reg) & ~mask) | value);
}

struct rtl9617c_led;

int rtl9617c_ni_led_output(struct rtl9617c_led *led, unsigned int value);
unsigned int rtl9617c_ni_pointer(struct rtl9617c_ni *priv, unsigned int reg);
int rtl9617c_ni_poll(struct napi_struct *napi, int budget);
irqreturn_t rtl9617c_ni_rx_irq(int irq, void *data);
void rtl9617c_ni_wake_ports(struct rtl9617c_ni *priv);
enum hrtimer_restart rtl9617c_ni_timer(struct hrtimer *timer);
netdev_tx_t rtl9617c_ni_xmit(struct sk_buff *skb, struct net_device *dev);

static inline struct rtl9617c_ni *rtl_shared(struct net_device *dev)
{
	return ((struct rtl9617c_port *)netdev_priv(dev))->shared;
}

extern const struct net_device_ops rtl_netdev_ops;
extern const struct ethtool_ops rtl_ethtool_ops;

void rtl9617c_ni_rx_mode(struct net_device *dev);
void rtl9617c_ni_set_rx_mode(struct rtl9617c_ni *priv);
void rtl_mac_address(struct rtl9617c_ni *priv);
void rtl_unregister_lan(struct rtl9617c_ni *priv, unsigned int port);
int rtl_register_lans(struct rtl9617c_ni *priv, struct device *parent);
int rtl9617c_ni_engine_get(struct rtl9617c_ni *priv);
void rtl9617c_ni_engine_put(struct rtl9617c_ni *priv);

void rtl_p6_bringup(struct rtl9617c_ni *priv);
void rtl_p6_set_speed(struct rtl9617c_ni *priv, unsigned int speed);

#endif
