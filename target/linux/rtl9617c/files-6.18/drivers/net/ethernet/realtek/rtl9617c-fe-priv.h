/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL9617C_FE_PRIV_H
#define RTL9617C_FE_PRIV_H

#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include "rtl9617c-fe-pipeline.h"

#define FE_SAVED_REGS 57

struct rtl_fe_bridge;
struct rtl_itu_paths;

void fe_bridge_flow_removed(struct rtl_fe *fe);
extern unsigned int fe_learned_max;

void fe_debugfs_init(struct rtl_fe *fe);
void fe_debugfs_exit(void);

#define FE_INTERFACES 64
#define FE_COUNTERS 32
#define FE_SOURCE_MACS 15
#define FE_LAN_PORTS 5


struct rtl_fe_port {
	struct list_head list;
	struct rtl_fe *fe;
	struct net_device *netdev;
	unsigned int source, destination, llid, bindings;
	bool retired;
};
struct rtl_fe_mac {
	unsigned int users, saved[5];
	u8 address[ETH_ALEN];
};
struct rtl_fe_interface {
	unsigned int users, saved, source_mac, pppoe;
	bool counted;
};
struct rtl_fe_mtu {
	unsigned int users, size, saved;
};
struct rtl_fe_flow {
	struct rhash_head node;
	struct list_head list;
	unsigned long cookie, lastused;
	struct rtl_fe_port *input, *output;
	unsigned int hash_index, counter, source_mac, destination_index;
	unsigned int pppoe, mtu_index;
	unsigned int ingress_session;
	unsigned int ingress_pol;
	u8 ip_source[16],ip_destination[16],protocol;
	u8 ethernet_source[ETH_ALEN];
	u16 ethernet_protocol;
	u16 source_port,destination_port;
	enum rtl_fe_rule_kind kind;
	bool learned;
	unsigned int bridge_index;
	unsigned int bridge_ingress, bridge_egress;
	u8 bridge_tags;
	struct {u16 tci,protocol;} bridge_tag[2];
	u16 bridge_vid;
	u16 bridge_ppp_protocol;
	unsigned int destination_saved[4];
	u16 crc16;
	unsigned char ethernet_bytes;
	bool destination_created;
	bool mtu_owned;
	bool published, hashed, source_owned, destination_owned, interface_owned, counted, installed, deleting;
	u8 destination[ETH_ALEN];
	u64 bytes;
	u32 packets;
};
static const unsigned int fe_registers[] = {
	RTL_FE_BUCKET_CFG,RTL_FE_HASH_BASE_HIGH,RTL_FE_HASH_BASE_LOW,
	RTL_FE_ACTION_BASE_HIGH,RTL_FE_ACTION_BASE_LOW,RTL_FE_ACTION_SLOT_CFG,
	RTL_FE_OVERFLOW_BASE_HIGH,RTL_FE_OVERFLOW_BASE_LOW,
	RTL_FE_DEFAULT_SLOT_CFG,RTL_FE_DEFAULT_BASE_HIGH,RTL_FE_DEFAULT_BASE_LOW,
	RTL_FE_HASH_CFG,RTL_FE_KEY_MASK_0,RTL_FE_KEY_MASK_1,RTL_FE_HASH_ENABLE,
	RTL_FE_CACHE_CFG,RTL_FE_ACTION_GROUP_CFG,
	RTL_FE_LPB_CFG,RTL_FE_LPB_PROFILE,
	RTL_FE_LPB_TABLE(0, 0),RTL_FE_LPB_TABLE(0, 1),RTL_FE_LPB_TABLE(0, 2),
	RTL_FE_LPB_TABLE(1, 0),RTL_FE_LPB_TABLE(1, 1),RTL_FE_LPB_TABLE(1, 2),
	RTL_FE_HASH_MODE,RTL_FE_SWO_TUPLE,
	RTL_FE_KEY_PROFILE(1),RTL_FE_KEY_PROFILE(2),RTL_FE_KEY_PROFILE(3),
	RTL_FE_KEY_PROFILE(4),RTL_FE_KEY_PROFILE(5),RTL_FE_KEY_PROFILE(6),
	RTL_FE_KEY_PROFILE(7),RTL_FE_PARSER_CFG,
	RTL_FE_ACTION_GROUP(0),RTL_FE_ACTION_GROUP(1),RTL_FE_ACTION_GROUP(2),
	RTL_FE_ACTION_GROUP(3),RTL_FE_ACTION_GROUP(4),
	RTL_NI_L2TM_ES_CTRL,RTL_NI_L2TM_TABLE_0,RTL_NI_L2TM_TABLE_1,
	RTL_NI_NITX_CONTROL,
	RTL_FE_TPID0,RTL_FE_TPID1,RTL_FE_TPID2,
	RTL_FE_PPP_PROTOCOL0,RTL_FE_PPP_PROTOCOL1,RTL_FE_CONTROL,
	RTL_FE_MISS_TEMPLATE,RTL_FE_AXI_PATCH,RTL_FE_AXI_ID1_LIMIT,RTL_FE_AXI_OUTSTANDING,
	RTL_FE_PPP_CONFIG,RTL_FE_TABLE_CFG1,RTL_FE_TABLE_CFG0,RTL_FE_CAM_DATA_AUX,
};
struct rtl_fe {
	struct device *device;
	struct regmap *regs;
	struct mutex lock;
	struct delayed_work cleanup_work;
	struct list_head ports, flows, blocks;
	struct rhashtable cookies;
	void *memory;
	dma_addr_t dma;
	bool enabled, prepared;
	int error;
	u64 installs, deletes, misses;
	unsigned int saved[ARRAY_SIZE(fe_registers)], mask_saved[8];
	unsigned int bridge_mask_saved[3][8];
	struct rtl_fe_cam_protocol protocol_cam[3];
	unsigned char ppp_protocol_ids[2];
	unsigned int lan_saved[FE_LAN_PORTS], xge_saved;
	unsigned int classifier_saved[2][7], map_saved[2][4];
	struct rtl_fe_mac source_macs[FE_SOURCE_MACS];
	struct rtl_fe_interface interfaces[FE_INTERFACES];
	struct rtl_fe_mtu mtus[16];
	struct rtl_itu_paths *itu_paths;
	struct net_device *itu_netdev;
	struct rtl_fe_bridge *bridge;
	DECLARE_BITMAP(hashes,RTL_FE_HASH_ENTRIES);
	DECLARE_BITMAP(counters,FE_INTERFACES);
};
static inline unsigned int fe_read(struct rtl_fe *fe,unsigned int address)
{
	return rtl_ni_read(fe->regs,address);
}
static inline void fe_write(struct rtl_fe *fe,unsigned int address,unsigned int value)
{
	rtl_ni_write(fe->regs,address,value);
}
static inline int fe_indirect(struct rtl_fe *fe,unsigned int address,unsigned int value)
{
	u32 busy=(address==RTL_FE_CACHE_CMD || address==RTL_FE_SYNC_CMD)?
		RTL_NI_INDIRECT_BUSY_LOW:RTL_NI_INDIRECT_BUSY;

	return rtl9617c_ni_indirect(fe->regs,address,value,busy);
}
#endif
