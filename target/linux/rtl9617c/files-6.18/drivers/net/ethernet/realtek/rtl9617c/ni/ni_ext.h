/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RTL9617C_NI_EXT_H
#define _RTL9617C_NI_EXT_H

#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/net_tstamp.h>
#include <linux/phylink.h>
#include <linux/skbuff.h>

struct rtl9617c_ni;
struct rtl9617c_ni_port;

/* Extended MIB (P6 XGE + cached GE/P6 stats64 snapshot) */
#define RTL_NI_XGE_MIB_COUNTERS		43

void rtl_ni_mib_p6_tick(struct rtl9617c_ni *ni);
int rtl_ni_mib_p6_get_all(struct rtl9617c_ni *ni, u64 *dst);
void rtl_ni_mib_p6_names(u8 **data);
void rtl_ni_mib_record_ge(unsigned int port, const u64 *raw);
void rtl_ni_get_stats64(struct net_device *dev,
			struct rtnl_link_stats64 *stats);

/* Flow Control & EEE helpers (implemented in ni_qos.c) */
void rtl_ni_fc_ext_init(struct rtl9617c_ni *ni);
void rtl_ni_port_fc_ext_set(struct rtl9617c_ni_port *port, bool tx_pause,
			    bool rx_pause);
int rtl_ni_eee_get(struct net_device *dev, struct ethtool_keee *eee);
int rtl_ni_eee_set(struct net_device *dev, struct ethtool_keee *eee);
int rtl_ni_mac_enable_tx_lpi(struct phylink_config *config, u32 timer,
			     bool tx_clk_stop);
void rtl_ni_mac_disable_tx_lpi(struct phylink_config *config);
void rtl_ni_eee_sync_link(struct rtl9617c_ni_port *port);

/* QoS / Traffic Engine offload (implemented in ni_qos.c) */
int rtl_ni_qos_setup_tc(struct rtl9617c_ni_port *port, enum tc_setup_type type,
			void *type_data);
struct tc_ets_qopt_offload;
int rtl_ni_qos_ets_to_weights(const struct tc_ets_qopt_offload *qopt,
			      u8 weights[8]);

/* IEEE 1588 PTP PHC & hardware timestamping (implemented in ni_ptp.c) */
void rtl_ni_ptp_init_dev(struct rtl9617c_ni *ni, struct device *dev);
void rtl_ni_ptp_cleanup(void);
int rtl_ni_ptp_get_ts_info(struct net_device *dev,
			   struct kernel_ethtool_ts_info *info);
int rtl_ni_ptp_hwtstamp_get(struct net_device *dev,
			    struct kernel_hwtstamp_config *cfg);
int rtl_ni_ptp_hwtstamp_set(struct net_device *dev,
			    struct kernel_hwtstamp_config *cfg,
			    struct netlink_ext_ack *extack);
struct sk_buff *rtl_ni_ptp_tx_prepare(struct rtl9617c_ni_port *port,
				      struct sk_buff *skb);
void rtl_ni_ptp_tx_commit(struct rtl9617c_ni_port *port,
			  struct sk_buff *ts_skb);
int rtl_ni_ptp_read_rx_latch(struct rtl9617c_ni_port *port, u64 *ns_out);

/* L2FE hardware FDB, membership & VLAN engine (implemented in ni_l2fe.c) */
void rtl_ni_l2fe_register_ni(struct rtl9617c_ni *ni);
void rtl_ni_l2fe_unregister_ni(void);
int rtl_ni_l2fe_vlan_rx_add(struct net_device *dev, __be16 proto, u16 vid);
int rtl_ni_l2fe_vlan_rx_kill(struct net_device *dev, __be16 proto, u16 vid);

#endif /* _RTL9617C_NI_EXT_H */
