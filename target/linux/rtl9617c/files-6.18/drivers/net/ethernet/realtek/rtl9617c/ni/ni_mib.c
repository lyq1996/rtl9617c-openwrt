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

int rtl_ni_mib_update(struct rtl9617c_ni *ni, unsigned int port)
{
	u64 raw[RTL9617C_NI_MIB_COUNTERS];
	unsigned int i;
	int ret;

	if (port >= RTL_NI_GE_PORTS)
		return -EINVAL;

	lockdep_assert_held(&ni->mib_lock);
	ret = rtl_ni_mib_read_raw(ni, port, raw);
	if (ret)
		return ret;

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

	if (count) {
		mutex_lock(&ni->mib_lock);
		rtl_ni_mib_update(ni, port->id);
		memcpy(data, ni->mib[port->id], count * sizeof(*data));
		mutex_unlock(&ni->mib_lock);
	}
	data[count] = atomic_long_read(&ni->rx_errors);
	data[count + 1] = atomic_long_read(&ni->rx_dropped);
}
