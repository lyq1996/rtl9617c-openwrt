// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/workqueue.h>

#include "ni.h"

#define MIB_RX_BYTES		24
#define MIB_OCTETS		25
#define MIB_TX_RESERVED_A	12
#define MIB_TX_RESERVED_B	23

static const char rtl_ni_mib_names[RTL9617C_NI_MIB_COUNTERS][ETH_GSTRING_LEN] = {
	"rx_unicast", "rx_multicast", "rx_broadcast", "rx_oam", "rx_jumbo",
	"rx_pause", "rx_pfc", "rx_unknown_opcode", "rx_crc_errors",
	"rx_undersize", "rx_runt", "rx_oversize", "rx_jabber", "rx_invalid",
	"rx_invalid_flow", "rx_64", "rx_65_127", "rx_128_255", "rx_256_511",
	"rx_512_1023", "rx_1024_1518", "rx_1519_2100", "rx_2101_9200",
	"rx_9201_max", "rx_bytes_good", "rx_bytes",
	"tx_unicast", "tx_multicast", "tx_broadcast", "tx_oam", "tx_jumbo",
	"tx_pause", "tx_pfc", "tx_crc_errors", "tx_oversize",
	"tx_single_collision", "tx_multiple_collision", "tx_late_collision",
	"tx_reserved_12", "tx_64", "tx_65_127", "tx_128_255", "tx_256_511",
	"tx_512_1023", "tx_1024_1518", "tx_1519_2100", "tx_2101_9200",
	"tx_9201_max", "tx_excessive_collision", "tx_reserved_23",
	"tx_bytes_good", "tx_bytes",
};

static const char rtl_ni_extra_names[][ETH_GSTRING_LEN] = {
	"ni_rx_errors", "ni_rx_dropped",
	"ni_rx_zerocopy", "ni_rx_copybreak", "ni_rx_alloc_fallback",
	"ni_tx_sg", "ni_tx_tso", "ni_tx_bounce",
	"ni_rx_q0", "ni_rx_q1", "ni_rx_q2", "ni_rx_q3",
};

static bool rtl_ni_mib_skip(unsigned int dir, unsigned int index)
{
	if (!dir)
		return index == MIB_RX_BYTES;
	return index == MIB_TX_RESERVED_A || index == MIB_TX_RESERVED_B ||
	       index == MIB_RX_BYTES;
}

static int rtl_ni_mib_read_raw(struct rtl9617c_ni *ni, unsigned int port,
			       u64 raw[RTL9617C_NI_MIB_COUNTERS])
{
	unsigned int dir, index;
	int ret;

	for (dir = 0; dir < 2; dir++) {
		unsigned int access = dir ? RTL_NI_TXMIB_ACCESS : RTL_NI_RXMIB_ACCESS;
		u64 *out = raw + dir * RTL9617C_NI_MIB_RX;

		for (index = 0; index < RTL9617C_NI_MIB_RX; index++) {
			if (rtl_ni_mib_skip(dir, index)) {
				out[index] = 0;
				continue;
			}
			ret = rtl_ne_table_read(ni->regs, access, (port << 5) | index);
			if (ret)
				return ret;
			out[index] = rtl_ni_read(ni, access + RTL_NI_MIB_DATA_LO);
			if (index == MIB_OCTETS)
				out[index] |= (u64)rtl_ni_read(ni, access +
							       RTL_NI_MIB_DATA_HI) << 32;
		}
	}
	return 0;
}

#include <linux/delay.h>
#include <linux/u64_stats_sync.h>
#include "ni_ext.h"

#define RTL_NI_TOTAL_PORT_SLOTS		7
#define RTL_NI_MIB_ACCESS_BIT		BIT(31)
#define RTL_NI_XGE_CNT_MASK		GENMASK_ULL(47, 0)

struct rtl_ni_mib_stats_cache {
	struct u64_stats_sync syncp;
	struct rtnl_link_stats64 stats;
	u64 p6_mib[RTL_NI_XGE_MIB_COUNTERS];
	u64 p6_prev[RTL_NI_XGE_MIB_COUNTERS];
	u64 ge_prev[16];
	bool ge_init;
	bool ge_clr_on_rd;
	bool p6_init;
};

static struct rtl_ni_mib_stats_cache rtl_ni_stats_cache[RTL_NI_TOTAL_PORT_SLOTS];

static const char * const rtl_ni_xge_mib_names[RTL_NI_XGE_MIB_COUNTERS] = {
	"tx_good_frames",
	"tx_good_octets",
	"tx_pause_frames",
	"rx_good_frames",
	"rx_good_octets",
	"rx_pause_frames",
	"rx_crc_err_frames",
	"rx_align_err_frames",
	"tx_ucast_frames",
	"tx_mcast_frames",
	"tx_bcast_frames",
	"tx_err_frames",
	"rx_ucast_frames",
	"rx_mcast_frames",
	"rx_bcast_frames",
	"rx_err_frames",
	"rx_octets",
	"rx_frames",
	"rx_runt_frames",
	"rx_short_frames",
	"rx_undersize_frames",
	"rx_giant_frames",
	"rx_jabber_frames",
	"rx_64b_frames",
	"rx_65b_to_127b_frames",
	"rx_128b_to_255b_frames",
	"rx_256b_to_511b_frames",
	"rx_512b_to_1023b_frames",
	"rx_1024b_to_1518b_frames",
	"rx_1519b_to_maxb_frames",
	"tx_octets",
	"tx_frames",
	"tx_64b_frames",
	"tx_65b_to_127b_frames",
	"tx_128b_to_255b_frames",
	"tx_256b_to_511b_frames",
	"tx_512b_to_1023b_frames",
	"tx_1024b_to_1518b_frames",
	"tx_1519b_frames",
	"drop_rx_crc_err_frames",
	"drop_rx_align_err_frames",
	"rx_pfc_frames",
	"tx_pfc_frames",
};

static int rtl_ni_mib_wait(struct rtl9617c_ni *ni, u32 reg)
{
	int retries = 10000;

	while (rtl_ni_read(ni, reg) & RTL_NI_MIB_ACCESS_BIT) {
		if (!--retries)
			return -ETIMEDOUT;
		cpu_relax();
	}

	return 0;
}

static int rtl_ni_ge_read_cnt_ro(struct rtl9617c_ni *ni, u32 access_reg,
				 unsigned int port, u32 id, u64 *val)
{
	u32 hi, lo;
	int ret;

	rtl_ni_write(ni, access_reg,
		     RTL_NI_MIB_ACCESS_BIT | ((port & 0x7) << 5) | (id & 0x1f));
	ret = rtl_ni_mib_wait(ni, access_reg);
	if (ret)
		return ret;

	hi = rtl_ni_read(ni, access_reg + RTL_NI_MIB_DATA_HI);
	lo = rtl_ni_read(ni, access_reg + RTL_NI_MIB_DATA_LO);
	*val = ((u64)hi << 32) | lo;

	return 0;
}

static void rtl_ni_ge_snapshot_read(struct rtl9617c_ni *ni, unsigned int port,
				    u64 v[16])
{
	static const u8 rx_ids[10] = {
		0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x18,
	};
	static const u8 tx_ids[6] = {
		0x00, 0x01, 0x02, 0x07, 0x08, 0x18,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rx_ids); i++) {
		if (rtl_ni_ge_read_cnt_ro(ni, RTL_NI_RXMIB_ACCESS, port,
					  rx_ids[i], &v[i]))
			v[i] = 0;
	}
	for (i = 0; i < ARRAY_SIZE(tx_ids); i++) {
		if (rtl_ni_ge_read_cnt_ro(ni, RTL_NI_TXMIB_ACCESS, port,
					  tx_ids[i], &v[10 + i]))
			v[10 + i] = 0;
	}
}

static int rtl_ni_p6_read_cnt(struct rtl9617c_ni *ni, u32 cnt_id, u64 *val)
{
	u32 hi, lo;
	int ret;

	rtl_ni_write(ni, RTL_NI_XGE_MAC_STATS_DATA1, 0);
	rtl_ni_write(ni, RTL_NI_XGE_MAC_STATS_DATA0, 0);
	rtl_ni_write(ni, RTL_NI_XGE_MAC_STATS_ACCESS,
		     RTL_NI_MIB_ACCESS_BIT | (cnt_id & 0x3f));
	ret = rtl_ni_mib_wait(ni, RTL_NI_XGE_MAC_STATS_ACCESS);
	if (ret)
		return ret;

	hi = rtl_ni_read(ni, RTL_NI_XGE_MAC_STATS_DATA1) & 0xffff;
	lo = rtl_ni_read(ni, RTL_NI_XGE_MAC_STATS_DATA0);
	*val = ((u64)hi << 32) | lo;

	return 0;
}

static int rtl_ni_mib_p6_update_locked(struct rtl9617c_ni *ni)
{
	struct rtl_ni_mib_stats_cache *c = &rtl_ni_stats_cache[RTL_NI_PORT_P6];
	u64 crc_err, align_err, len_err, err_total;
	unsigned int id;
	int ret;

	lockdep_assert_held(&ni->mib_lock);

	regmap_update_bits(ni->regs, RTL_NI_XGE_MAC_CFG_COM,
			   RTL_NI_XGE_MAC_CFG_COM_CLR_ON_RD |
			   RTL_NI_XGE_MAC_CFG_COM_RT_MIB_EN,
			   RTL_NI_XGE_MAC_CFG_COM_RT_MIB_EN);

	for (id = 0; id < RTL_NI_XGE_MIB_COUNTERS; id++) {
		u64 raw, delta;

		ret = rtl_ni_p6_read_cnt(ni, id, &raw);
		if (ret)
			return ret;

		delta = (raw - c->p6_prev[id]) & RTL_NI_XGE_CNT_MASK;
		c->p6_prev[id] = raw;
		c->p6_mib[id] += delta;
	}

	crc_err = c->p6_mib[0x06];
	align_err = c->p6_mib[0x07];
	len_err = c->p6_mib[0x12] + c->p6_mib[0x13] + c->p6_mib[0x14] +
		  c->p6_mib[0x15] + c->p6_mib[0x16];
	err_total = max_t(u64, c->p6_mib[0x0f], crc_err + align_err + len_err);

	if (!c->p6_init) {
		u64_stats_init(&c->syncp);
		c->p6_init = true;
	}

	u64_stats_update_begin(&c->syncp);
	c->stats.rx_packets = c->p6_mib[0x03];
	c->stats.tx_packets = c->p6_mib[0x00];
	c->stats.rx_bytes = c->p6_mib[0x04];
	c->stats.tx_bytes = c->p6_mib[0x01];
	c->stats.multicast = c->p6_mib[0x0d];
	c->stats.rx_crc_errors = crc_err;
	c->stats.rx_frame_errors = align_err;
	c->stats.rx_length_errors = len_err;
	c->stats.rx_errors = err_total;
	c->stats.rx_dropped = c->p6_mib[0x27] + c->p6_mib[0x28];
	c->stats.tx_errors = c->p6_mib[0x0b];
	u64_stats_update_end(&c->syncp);

	return 0;
}

void rtl_ni_mib_p6_tick(struct rtl9617c_ni *ni)
{
	if (!ni || !READ_ONCE(ni->ports[RTL_NI_PORT_P6]))
		return;

	rtl_ni_mib_p6_update_locked(ni);
}

int rtl_ni_mib_p6_get_all(struct rtl9617c_ni *ni, u64 *dst)
{
	struct rtl_ni_mib_stats_cache *c = &rtl_ni_stats_cache[RTL_NI_PORT_P6];
	int ret;

	mutex_lock(&ni->mib_lock);
	ret = rtl_ni_mib_p6_update_locked(ni);
	memcpy(dst, c->p6_mib, RTL_NI_XGE_MIB_COUNTERS * sizeof(*dst));
	mutex_unlock(&ni->mib_lock);

	return ret;
}

void rtl_ni_mib_p6_names(u8 **data)
{
	unsigned int i;

	for (i = 0; i < RTL_NI_XGE_MIB_COUNTERS; i++)
		ethtool_puts(data, rtl_ni_xge_mib_names[i]);
}

void rtl_ni_mib_record_ge(unsigned int port, const u64 *v)
{
	struct rtl_ni_mib_stats_cache *c;
	u64 rx_pkts, tx_pkts, rx_crc, rx_len, rx_err, tx_err;

	if (port >= RTL_NI_GE_PORTS)
		return;

	c = &rtl_ni_stats_cache[port];
	if (!c->ge_init) {
		u64_stats_init(&c->syncp);
		c->ge_init = true;
	}

	rx_pkts = v[0] + v[1] + v[2];
	rx_crc = v[3];
	rx_len = v[4] + v[5] + v[6] + v[7];
	rx_err = rx_crc + rx_len + v[8];
	tx_pkts = v[10] + v[11] + v[12];
	tx_err = v[13] + v[14];

	u64_stats_update_begin(&c->syncp);
	if (c->ge_clr_on_rd) {
		c->stats.rx_packets += rx_pkts;
		c->stats.tx_packets += tx_pkts;
		c->stats.rx_bytes += v[9];
		c->stats.tx_bytes += v[15];
		c->stats.multicast += v[1];
		c->stats.rx_crc_errors += rx_crc;
		c->stats.rx_length_errors += rx_len;
		c->stats.rx_errors += rx_err;
		c->stats.tx_errors += tx_err;
	} else {
		c->stats.rx_packets = rx_pkts;
		c->stats.tx_packets = tx_pkts;
		c->stats.rx_bytes = v[9];
		c->stats.tx_bytes = v[15];
		c->stats.multicast = v[1];
		c->stats.rx_crc_errors = rx_crc;
		c->stats.rx_length_errors = rx_len;
		c->stats.rx_errors = rx_err;
		c->stats.tx_errors = tx_err;
	}
	u64_stats_update_end(&c->syncp);
}

void rtl_ni_get_stats64(struct net_device *dev,
			struct rtnl_link_stats64 *stats)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	struct rtl_ni_mib_stats_cache *c;
	unsigned int start;

	if (port->id >= RTL_NI_TOTAL_PORT_SLOTS) {
		netdev_stats_to_stats64(stats, &dev->stats);
		return;
	}

	c = &rtl_ni_stats_cache[port->id];
	if (port->id == RTL_NI_PORT_P6 ? !c->p6_init : !c->ge_init) {
		netdev_stats_to_stats64(stats, &dev->stats);
		return;
	}

	do {
		start = u64_stats_fetch_begin(&c->syncp);
		*stats = c->stats;
	} while (u64_stats_fetch_retry(&c->syncp, start));
}

int rtl_ni_mib_update(struct rtl9617c_ni *ni, unsigned int port)
{
	u64 raw[RTL9617C_NI_MIB_COUNTERS];
	u64 pre[16], post[16];
	u64 post_rx_bytes = 0;
	unsigned int i;
	int ret;

	if (port >= RTL_NI_GE_PORTS)
		return -EINVAL;

	lockdep_assert_held(&ni->mib_lock);
	rtl_ni_ge_snapshot_read(ni, port, pre);
	ret = rtl_ni_mib_read_raw(ni, port, raw);
	if (ret)
		return ret;
	if (!rtl_ni_ge_read_cnt_ro(ni, RTL_NI_RXMIB_ACCESS, port, 0x18,
				   &post_rx_bytes) &&
	    pre[9] > 0 && post_rx_bytes < pre[9])
		rtl_ni_stats_cache[port].ge_clr_on_rd = true;
	if (rtl_ni_stats_cache[port].ge_clr_on_rd) {
		rtl_ni_mib_record_ge(port, pre);
	} else {
		rtl_ni_ge_snapshot_read(ni, port, post);
		rtl_ni_mib_record_ge(port, post);
	}

	for (i = 0; i < RTL9617C_NI_MIB_COUNTERS; i++) {
		u64 *total = &ni->mib[port][i];

		if (i % RTL9617C_NI_MIB_RX == MIB_OCTETS)
			*total = raw[i];
		else
			*total += (u32)((u32)raw[i] - (u32)*total);
	}
	return 0;
}

static void rtl_ni_mib_work(struct work_struct *work)
{
	struct rtl9617c_ni *ni = container_of(to_delayed_work(work),
					      struct rtl9617c_ni, mib_work);
	unsigned int port;
	int ret;

	mutex_lock(&ni->mib_lock);
	rtl_ni_mib_p6_tick(ni);
	for (port = 0; port < RTL_NI_GE_PORTS; port++) {
		if (!ni->ports[port])
			continue;
		ret = rtl_ni_mib_update(ni, port);
		if (ret)
			dev_err_ratelimited(ni->dev, "port %u MIB read failed: %d\n",
					    port, ret);
	}
	mutex_unlock(&ni->mib_lock);
	schedule_delayed_work(&ni->mib_work, HZ);
}

void rtl_ni_mib_init(struct rtl9617c_ni *ni)
{
	INIT_DELAYED_WORK(&ni->mib_work, rtl_ni_mib_work);
	schedule_delayed_work(&ni->mib_work, HZ);
}

void rtl_ni_mib_exit(struct rtl9617c_ni *ni)
{
	cancel_delayed_work_sync(&ni->mib_work);
}

static unsigned int rtl_ni_mib_count(struct net_device *dev)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);

	if (port->id == RTL_NI_PORT_P6)
		return RTL_NI_XGE_MIB_COUNTERS;

	return rtl_ni_is_ge(port->id) ? RTL9617C_NI_MIB_COUNTERS : 0;
}

int rtl_ni_get_sset_count(struct net_device *dev, int sset)
{
	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;
	return rtl_ni_mib_count(dev) + ARRAY_SIZE(rtl_ni_extra_names);
}

void rtl_ni_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	unsigned int i;

	if (stringset != ETH_SS_STATS)
		return;
	if (((struct rtl9617c_ni_port *)netdev_priv(dev))->id == RTL_NI_PORT_P6)
		rtl_ni_mib_p6_names(&data);
	else
		for (i = 0; i < rtl_ni_mib_count(dev); i++)
			ethtool_puts(&data, rtl_ni_mib_names[i]);
	for (i = 0; i < ARRAY_SIZE(rtl_ni_extra_names); i++)
		ethtool_puts(&data, rtl_ni_extra_names[i]);
}

void rtl_ni_get_ethtool_stats(struct net_device *dev,
			      struct ethtool_stats *stats, u64 *data)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	struct rtl9617c_ni *ni = port->ni;
	unsigned int count = rtl_ni_mib_count(dev);

	if (port->id == RTL_NI_PORT_P6) {
		rtl_ni_mib_p6_get_all(ni, data);
	} else if (count) {
		mutex_lock(&ni->mib_lock);
		rtl_ni_mib_update(ni, port->id);
		memcpy(data, ni->mib[port->id], count * sizeof(*data));
		mutex_unlock(&ni->mib_lock);
	}
	data[count] = atomic_long_read(&ni->rx_errors);
	data[count + 1] = atomic_long_read(&ni->rx_dropped);
	data[count + 2] = atomic_long_read(&ni->rx_zerocopy_pkts);
	data[count + 3] = atomic_long_read(&ni->rx_copybreak_pkts);
	data[count + 4] = atomic_long_read(&ni->rx_alloc_fallback);
	data[count + 5] = atomic_long_read(&ni->tx_sg_pkts);
	data[count + 6] = atomic_long_read(&ni->tx_tso_pkts);
	data[count + 7] = atomic_long_read(&ni->tx_bounce_pkts);
	BUILD_BUG_ON(RTL_NI_RX_QUEUES != 4);
	data[count + 8] = atomic_long_read(&ni->qm.rxq[0].rx_pkts);
	data[count + 9] = atomic_long_read(&ni->qm.rxq[1].rx_pkts);
	data[count + 10] = atomic_long_read(&ni->qm.rxq[2].rx_pkts);
	data[count + 11] = atomic_long_read(&ni->qm.rxq[3].rx_pkts);
}
