/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL9617C_NI_PRIV_H
#define RTL9617C_NI_PRIV_H

#include <linux/atomic.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/if_vlan.h>
#include <linux/ioport.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/page_pool/helpers.h>

#include "rtl9617c_ni.h"
#include "ni_te_flow.h"
#include "rtl9617c_regs.h"

#define RTL_NI_PORTS			7
#define RTL_NI_GE_PORTS			5
#define RTL_NI_PORT_P6			6
#define RTL_NI_PORT_PON			7
#define RTL_NI_CPU_PORT			0
#define RTL_NI_FRAME_MAX		(VLAN_ETH_FRAME_LEN + 4)
#define RTL_NI_TX_TIMER_NS		NSEC_PER_MSEC

#define RTL_NI_TX_RINGS			4
#define RTL_NI_DMA_ORDER		9
#define RTL_NI_DMA_ENTRIES		BIT(RTL_NI_DMA_ORDER)
#define RTL_NI_DMA_MASK			(RTL_NI_DMA_ENTRIES - 1)
#define RTL_NI_DMA_BUF_SIZE		2048
#define RTL_NI_DMA_HDR_LEN		16
#define RTL_NI_DMA_VP			8
#define RTL_NI_DMA_GUARD		2
#define RTL_NI_TX_COPYBREAK		128U
#define RTL_NI_TX_MAX_FRAGS		(MAX_SKB_FRAGS + 2)

#define RTL_NI_QM_VOQS			8
#define RTL_NI_QM_RING_SIZE		1024
#define RTL_NI_QM_CMD_SIZE		8
#define RTL_NI_QM_EQ			4
#define RTL_NI_QM_EQS			2
#define RTL_NI_QM_EQ_BUFS		256
#define RTL_NI_QM_BUFS			(RTL_NI_QM_EQS * RTL_NI_QM_EQ_BUFS)
#define RTL_NI_RX_QUEUES		4
#define RTL_NI_QM_CPU_EQS		(RTL_NI_RX_QUEUES * RTL_NI_QM_EQS)
#define RTL_NI_QM_BUF_SIZE		2048
#define RTL_NI_QM_HEADROOM		64
#define RTL_NI_QM_TAILROOM		384
#define RTL_NI_QM_DATA_SIZE		(RTL_NI_QM_BUF_SIZE - RTL_NI_QM_HEADROOM - \
					 RTL_NI_QM_TAILROOM)
#define RTL_NI_QM_SLOT_HASH_BITS	10
#define RTL_NI_QM_SLOT_HASH_SIZE	BIT(RTL_NI_QM_SLOT_HASH_BITS)
#define RTL_NI_QM_SLOT_HASH_MASK	(RTL_NI_QM_SLOT_HASH_SIZE - 1)
#define RTL_NI_RX_COPYBREAK		128U
#define RTL_NI_QM_PE_EQ			6
#define RTL_NI_QM_PE_BUFS		2496
#define RTL_NI_DQ_BUFS			512
#define RTL_NI_DQ_BUF_SIZE		2048
#define RTL_NI_DQ_SIZE			(2 * RTL_NI_DQ_BUFS * RTL_NI_DQ_BUF_SIZE)

struct rtl9617c_ni;
struct rtl_fe;

struct rtl_ni_dma_desc {
	__le32 addr;
	__le32 ctrl;
};

enum rtl_ni_tx_map_type {
	RTL_NI_TX_MAP_NONE = 0,
	RTL_NI_TX_MAP_SINGLE,
	RTL_NI_TX_MAP_PAGE,
};

struct rtl_ni_tx_slot {
	struct sk_buff *skb;
	dma_addr_t dma;
	u32 len;
	u8 map_type;
};

struct rtl_ni_dma_ring {
	spinlock_t lock;
	unsigned int id;
	unsigned int vp;
	struct rtl_ni_dma_desc *desc;
	dma_addr_t desc_dma;
	void *hdrs;
	dma_addr_t hdrs_dma;
	void *bufs;
	dma_addr_t bufs_dma;
	struct rtl_ni_tx_slot *slots;
	unsigned int head;
	unsigned int tail;
	unsigned int zc_in_flight;
	unsigned int zc_skbs;
	bool idle_drain_armed;
	bool stopped;
};

struct rtl_ni_dma {
	struct device *dev;
	void __iomem *base;
	struct rtl_ni_dma_ring rings[RTL_NI_TX_RINGS];
	unsigned int num_rings;
};

/*
 * TX buffer header: the ring descriptor points at a private header that sits
 * immediately in front of the frame bytes and is consumed by the DMA LSO
 * engine before the payload (OEM ca_ni_start_xmit_buf.constprop.0 @0x1aabf0,
 * RE-1 spec 2.2).  The OEM layout is lso_para0 @+0, lso_para1 @+4 and
 * header_a as a single 64-bit little-endian word @+8.
 *
 * The pre-S1 driver wrote the same 16 bytes as {len @+4, word @+8, word @+12}
 * and never named the first word, so the field OEM calls header_a's low half
 * is the member called header_high in struct rtl9617c_ni_frame.  S1 keeps that
 * mapping on purpose: it is what puts cos/ldpid/lspid/pkt_size/fe_bypass on
 * the OEM bit positions.  tests/tx-dma-layout.py asserts byte identity with
 * the pre-S1 encoder.
 */
struct rtl_ni_tx_hdr {
	u32 lso_para0;
	u32 lso_para1;
	u64 header_a;
};

/*
 * lso_para0 bit map (RE-1 2.3).  The S1/S2 encoder leaves every enable bit
 * clear, so it still describes a plain, non-segmented frame; the S4/S5 helper
 * rtl_ni_tx_offload_prepare() is what sets them, and only on request.
 */
#define RTL_NI_LSO_PARA0_SEGMENT_SIZE	GENMASK(13, 0)
#define RTL_NI_LSO_PARA0_UNKNOWN_TAG_EN	BIT(24)
#define RTL_NI_LSO_PARA0_SEGMENT_EN	BIT(25)
#define RTL_NI_LSO_PARA0_IPV4_EN	BIT(26)
#define RTL_NI_LSO_PARA0_IPV6_EN	BIT(27)
#define RTL_NI_LSO_PARA0_TCP_EN		BIT(28)
#define RTL_NI_LSO_PARA0_UDP_EN		BIT(29)
#define RTL_NI_LSO_PARA0_BYPASS_EN	BIT(30)
#define RTL_NI_LSO_PARA0_LENFIX_EN	BIT(31)

/* header_a bit map (RE-1 2.5, names from ca_ni_dump_hdr_a @0x23cc50). */
#define RTL_NI_HDR_A_COS		GENMASK_ULL(34, 32)
#define RTL_NI_HDR_A_LDPID		GENMASK_ULL(40, 35)
#define RTL_NI_HDR_A_LSPID		GENMASK_ULL(46, 41)
#define RTL_NI_HDR_A_PKT_SIZE		GENMASK_ULL(60, 47)
#define RTL_NI_HDR_A_FE_BYPASS		BIT_ULL(61)
#define RTL_NI_HDR_A_HDR_TYPE		GENMASK_ULL(63, 62)
#define RTL_NI_HDR_A_PKT_SIZE_MAX	GENMASK(13, 0)
#define RTL_NI_HDR_A_HDR_TYPE_UC	0	/* 1 = MC, 2 = CPU + learn, 3 = PTP */

/*
 * Classification produced by the L3/L4 walk that OEM implements in
 * rtk_dma_lso_checksum_select @0x1a6470 (RE-1 1.6).  That function writes no
 * register; it only decides whether a frame may be handed to the LSO engine.
 * Offsets are byte offsets into the frame as passed to rtl_ni_dma_xmit().
 * It is the S3 classifier; rtl_ni_tx_offload_prepare() consumes it for S4/S5.
 */
struct rtl_ni_tx_l4info {
	unsigned int l3_off;
	unsigned int l4_off;
	unsigned int l4_len;	/* L4 payload bytes after the L4 header */
	unsigned int ip_len;	/* L3 length, including the L3 header */
	u8 ip_proto;
	bool ipv4;
	bool ipv6;
	bool pppoe;
	bool tcp;
	bool udp;
	bool udp_csum_present;
};

/*
 * One frame's offload request (RE-1 section 6, steps S4 and S5).  The module
 * parameters decide whether the features are advertised at all; this struct is
 * the per-frame decision, so a run-time parameter flip cannot leave the stack
 * with a frame type the driver refuses to handle.
 */
struct rtl_ni_tx_offload {
	bool csum;		/* enable TCP/UDP checksum offload */
	bool segment;		/* segment this frame (it is a GSO super-packet) */
	bool segment_ipv6;	/* accept IPv6/TCP super-packets (tso_v6) */
	unsigned int gso_size;	/* TCP MSS from skb_shinfo(skb)->gso_size */
	unsigned int mtu;	/* egress L3 MTU (0 defaults to 1500) */
};

/*
 * Linear single-buffer bounce ceiling vs multi-descriptor SG ceiling:
 *  - RTL_NI_TX_GSO_MAX (2032 B) is the single-descriptor bounce limit when
 *    tx_sg=0 (header + payload fit one 2048 B bounce buffer).
 *  - RTL_NI_TX_GSO_MAX_SG (65500 B) is the multi-descriptor DMA LSO ceiling
 *    when tx_sg=1 (lso_para1 holds a 16-bit frame length up to 65535 B).
 */
#define RTL_NI_TX_GSO_MAX		(RTL_NI_DMA_BUF_SIZE - RTL_NI_DMA_HDR_LEN)
#define RTL_NI_TX_GSO_MAX_SG		65500U

/* TX offload switches, defined in ni_dma.c; ni_port.c advertises from them. */
extern bool rtl_ni_tx_csum;
extern bool rtl_ni_tso;
extern bool rtl_ni_tso_v6;
extern bool rtl_ni_tx_sg;
extern bool rtl_ni_tx_multivp;

static inline unsigned int rtl_ni_tx_gso_max(void)
{
	return READ_ONCE(rtl_ni_tx_sg) ? RTL_NI_TX_GSO_MAX_SG :
					 RTL_NI_TX_GSO_MAX;
}

struct rtl_ni_qm_slot {
	u32 dma;
	struct page *page;
};

struct rtl_ni_qm_refill {
	u32 addr[RTL_NI_QM_EQS][RTL_NI_QM_EQ_BUFS];
	unsigned int head[RTL_NI_QM_EQS];
	unsigned int count[RTL_NI_QM_EQS];
	unsigned int pending;
};

struct rtl_ni_rxq {
	struct rtl9617c_ni *ni;
	unsigned int id;
	unsigned int eq0;
	unsigned int eq1;
	u16 bid_base;
	int irq;
	char irq_name[24];
	struct napi_struct napi;
	struct page_pool *pool;
	struct rtl_ni_qm_slot *rx_slots;
	void *ring;
	dma_addr_t ring_dma;
	unsigned int read[RTL_NI_QM_VOQS];
	struct rtl_ni_qm_refill refill;
	atomic_long_t rx_pkts;
	bool napi_added;
	bool ready;
};

struct rtl_ni_qm {
	void __iomem *axi;
	void *dq;
	dma_addr_t dq_dma;
	void *lan_dq;
	dma_addr_t lan_dq_dma;
	size_t lan_dq_size;
	unsigned int lan_dq_bufs;
	unsigned int num_rxq;
	struct rtl_ni_rxq rxq[RTL_NI_RX_QUEUES];
	bool ready;
};

struct rtl9617c_ni_port {
	struct rtl9617c_ni *ni;
	struct net_device *dev;
	struct device_node *np;
	struct phylink *phylink;
	struct phylink_config phylink_config;
	struct phy *serdes;
	phy_interface_t interface;
	unsigned int id;
	u8 mac_offset;
	bool registered;
};

struct rtl9617c_ni {
	struct device *dev;
	struct regmap *regs;
	void __iomem *reset;
	void __iomem *fe_axi;
	struct net_device *napi_dev;
	struct hrtimer timer;
	spinlock_t timer_lock;
	spinlock_t tx_lock;
	spinlock_t rx_lock;
	struct mutex lock;
	unsigned int users;
	bool rx_active;
	bool timer_armed;
	bool tx_stopped;
	bool tx_closing;
	atomic_t pon_stopped_mask;
	struct rtl_ni_dma dma;
	struct rtl_ni_qm qm;
	struct rtl_fe *fe;
	struct rtl9617c_ni_port *ports[RTL_NI_PORTS];
	struct net_device __rcu *rx_dev[RTL_NI_PORTS];
	struct rtl9617c_ni_endpoint __rcu *pon;
	u8 mac[ETH_ALEN];
	bool serdes_on;
	bool serdes_ready;
	bool xge_bypass_saved;
	u32 xge_bypass;
	struct mutex mib_lock;
	struct delayed_work mib_work;
	struct work_struct tx_timeout_work;
	u64 mib[RTL_NI_GE_PORTS][RTL9617C_NI_MIB_COUNTERS];
	struct rtl_te_flow te;
	atomic_long_t rx_errors;
	atomic_long_t rx_dropped;
	atomic_long_t rx_zerocopy_pkts;
	atomic_long_t rx_copybreak_pkts;
	atomic_long_t rx_alloc_fallback;
	atomic_long_t tx_sg_pkts;
	atomic_long_t tx_tso_pkts;
	atomic_long_t tx_bounce_pkts;
};

static inline u32 rtl_ni_read(struct rtl9617c_ni *ni, unsigned int reg)
{
	return rtl_ne_read(ni->regs, reg);
}

static inline void rtl_ni_write(struct rtl9617c_ni *ni, unsigned int reg, u32 val)
{
	rtl_ne_write(ni->regs, reg, val);
}

static inline void rtl_ni_rmw(struct rtl9617c_ni *ni, unsigned int reg, u32 clear,
			      u32 set)
{
	rtl_ne_rmw(ni->regs, reg, clear, set);
}

static inline bool rtl_ni_is_ge(unsigned int id)
{
	return id < RTL_NI_GE_PORTS;
}

void rtl_ni_set_mac(struct rtl9617c_ni *ni);
int rtl_ni_engine_get(struct rtl9617c_ni *ni);
void rtl_ni_engine_put(struct rtl9617c_ni *ni);
netdev_tx_t rtl_ni_xmit_frame(struct rtl9617c_ni *ni, struct net_device *dev,
			      struct sk_buff *skb,
			      const struct rtl9617c_ni_frame *frame);
void rtl_ni_tx_restart(struct rtl9617c_ni *ni);
void rtl_ni_notify_pon_features(struct rtl9617c_ni *ni);
int rtl_ni_try_tx_hook(struct net_device *dev, struct sk_buff *skb,
		       const struct rtl9617c_ni_frame *frame);
void rtl_ni_notify_dma_reset(void);

int rtl_ni_dma_alloc(struct rtl9617c_ni *ni);
int rtl_ni_dma_start(struct rtl_ni_dma *dma);
void rtl_ni_dma_stop(struct rtl_ni_dma *dma);
void rtl_ni_dma_ring_reclaim(struct rtl_ni_dma *dma,
			     struct rtl_ni_dma_ring *ring);
bool rtl_ni_dma_ring_reclaim_timer(struct rtl_ni_dma *dma,
				   struct rtl_ni_dma_ring *ring);
bool rtl_ni_dma_ring_avail(const struct rtl_ni_dma_ring *ring,
			   unsigned int needed);
int rtl_ni_dma_prepare_skb(struct sk_buff *skb,
			   const struct rtl9617c_ni_frame *frame,
			   unsigned int mtu);
int rtl_ni_dma_ring_xmit(struct rtl_ni_dma *dma, struct rtl_ni_dma_ring *ring,
			 struct sk_buff *skb,
			 const struct rtl9617c_ni_frame *frame,
			 unsigned int mtu, bool *consumed, bool *used_sg);
void rtl_ni_tx_hdr_build(struct rtl_ni_tx_hdr *hdr,
			 const struct rtl9617c_ni_frame *frame,
			 unsigned int len, bool full_header);
void rtl_ni_tx_hdr_write(u8 *buf, const struct rtl_ni_tx_hdr *hdr,
			 bool oem_layout);
u32 rtl_ni_dma_desc_ctrl(unsigned int seg_len, bool sof, bool eof, bool hp0,
			 bool oam);
int rtl_ni_tx_l4_parse_head(const u8 *data, unsigned int head_len,
			    unsigned int total_len, bool gso,
			    struct rtl_ni_tx_l4info *info);
int rtl_ni_tx_l4_parse(const u8 *data, unsigned int len,
		       struct rtl_ni_tx_l4info *info);
int rtl_ni_tx_offload_prepare(u8 *frame, unsigned int len,
			      const struct rtl_ni_tx_offload *req, u32 *para0);
int rtl_ni_tx_offload_prepare_sg(const u8 *head, unsigned int head_len,
				 unsigned int total_len,
				 const struct rtl_ni_tx_offload *req,
				 u32 *para0);

int rtl_ni_qm_alloc(struct rtl9617c_ni *ni);
void rtl_ni_qm_free(struct rtl9617c_ni *ni);
int rtl_ni_qm_start(struct rtl9617c_ni *ni);
void rtl_ni_qm_stop(struct rtl9617c_ni *ni);
void rtl_ni_qm_drain_slots(struct rtl9617c_ni *ni);
int rtl_ni_qm_poll(struct rtl_ni_rxq *rxq, int budget);
int rtl_ni_qm_rss_apply(struct rtl9617c_ni *ni);
void rtl_ni_rss_dpid_words(unsigned int num_rxq, u32 *dpid0, u32 *dpid1);
unsigned int rtl_ni_rx_queues_get(void);

int rtl_ni_serdes_bringup(struct rtl9617c_ni *ni);

int rtl_ni_ports_register(struct rtl9617c_ni *ni);
void rtl_ni_ports_unregister(struct rtl9617c_ni *ni);
void rtl_ni_ports_update_features(void);
void rtl_ni_ports_rss_apply(void);

void rtl_ni_mib_init(struct rtl9617c_ni *ni);
void rtl_ni_mib_exit(struct rtl9617c_ni *ni);
int rtl_ni_mib_update(struct rtl9617c_ni *ni, unsigned int port);
int rtl_ni_get_sset_count(struct net_device *dev, int sset);
void rtl_ni_get_strings(struct net_device *dev, u32 stringset, u8 *data);
void rtl_ni_get_ethtool_stats(struct net_device *dev,
			      struct ethtool_stats *stats, u64 *data);

#endif
