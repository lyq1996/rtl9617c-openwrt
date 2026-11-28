// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bitfield.h>
#include <linux/ethtool.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>
#include <net/pkt_sched.h>
#include <net/sch_generic.h>

#include "../rtl9617c_regs.h"
#include "fe/fe.h"
#include "ni.h"
#include "ni_ext.h"

#define NI_MAX_PORT_SLOTS		7
#define NI_VOQ_NUM			8

/* NI_HV_PT_RXMAC_CFG / TXMAC_CFG flow-control fields */
#define PT_RXMAC_FC_TO_TX_EN		BIT(12)
#define PT_RXMAC_PFC_DISABLE		BIT(13)
#define PT_TXMAC_PFC_DISABLE		BIT(14)
#define PT_TXMAC_PAU_SEL_MASK		GENMASK(16, 15)
#define PT_TXMAC_BKPRS_EN		BIT(18)
#define PT_TXMAC_AUTO_XON		BIT(26)

/* NI_HV_GLB_RXMUX_PORTn_FC_THRESHOLD_CFG & XGE6_RXPAUSE_CFG */
#define RXMUX_FC_THRSH_SDK_DFT		(FIELD_PREP(GENMASK(19, 10), 130) | \
					 FIELD_PREP(GENMASK(9, 0), 10))
#define XGE6_RXPAUSE_THRSH_DFT		(FIELD_PREP(GENMASK(19, 10), 300) | \
					 FIELD_PREP(GENMASK(9, 0), 100))

/* NI_HV_GLB_XGE6_FLOW_CONTROL_CFG */
#define XGE6_FC_QBB_TX_EN		BIT(0)
#define XGE6_FC_QBB_RX_EN		BIT(1)
#define XGE6_FC_PAU_TX_EN		BIT(2)
#define XGE6_FC_PAU_RX_EN		BIT(3)
#define XGE6_FC_RX_PAUSEREQ_SEL		BIT(30)
#define XGE6_FC_TE_PAUSEREQ_SEL		BIT(31)

/* NI_HV_XGE_MAC_CFG_RX / CFG_TX */
#define XGE_CFG1_PAUSE_TERM		BIT(4)
#define XGE_CFG1_PAUSE_EXTRACT		BIT(5)
#define XGE_CFGTX_PAUSE_REQ_EN		BIT(4)
#define XGE_CFGTX_PAUSE_AUTO_XON	BIT(5)

/* NI_HV_PT_EEE_LPI_CFG0 */
#define EEE_LPI_TX_EN			BIT(0)
#define EEE_LPI_STOP_TXCLK		BIT(2)
#define EEE_LPI_TXFIFO_EMPTY_MASK	GENMASK(31, 12)
#define EEE_LPI_TXFIFO_EMPTY_DFT	0x01387U

/* L2TE / L2TM indirect access helpers */
#define L2TE_ACC_BUSY			BIT(31)
#define L2TE_ACC_RBW_WRITE		BIT(30)

#define POL_CTRL0_TYPE_MASK		GENMASK(2, 0)
#define POL_CTRL0_IPG_MASK		GENMASK(10, 6)
#define POL_CTRL0_UPDATE_MODE		BIT(13)

struct rtl_ni_eee_state {
	bool eee_enabled;
	bool tx_lpi_enabled;
	u32 tx_lpi_timer;
	bool mac_lpi_active;
};

struct rtl_ni_qos_state {
	bool fc_inited;
	unsigned long police_cookie[NI_MAX_PORT_SLOTS];
	bool police_active[NI_MAX_PORT_SLOTS];
	struct rtl_ni_eee_state eee[NI_MAX_PORT_SLOTS];
};

static struct rtl_ni_qos_state qos_st;
static DEFINE_MUTEX(qos_lock);

static int l2te_wait_clear(struct rtl9617c_ni *ni, u32 reg)
{
	int retries = 10000;

	while (rtl_ni_read(ni, reg) & L2TE_ACC_BUSY) {
		if (!--retries)
			return -ETIMEDOUT;
		cpu_relax();
	}

	return 0;
}

void rtl_ni_fc_ext_init(struct rtl9617c_ni *ni)
{
	unsigned int i;

	if (!ni || qos_st.fc_inited)
		return;

	/* Note: L2TE_CB_GLB_THRSH (0x2dcc) & L2TE_FC_GLB_TIMER (0x2914) stay
	 * at ni_main.c DEEPQ / GLB_TIMER settings.
	 */
	for (i = 0; i < 4; i++)
		rtl_ni_write(ni, RTL_NI_RXMUX_FC_THRSH(i),
			     RXMUX_FC_THRSH_SDK_DFT);

	rtl_ni_write(ni, RTL_NI_XGE6_RXPAUSE_CFG, XGE6_RXPAUSE_THRSH_DFT);
	qos_st.fc_inited = true;
}

static void rtl_ni_ge_set_pause_quanta(struct rtl9617c_ni *ni, unsigned int id,
				       bool tx_pause)
{
	u32 val = tx_pause ? 0xffffffffU : 0U;
	unsigned int k;

	for (k = 0; k < 4; k++)
		rtl_ni_write(ni, RTL_NI_PORT_PFC_QUANTA(id, k), val);
}

static void rtl_ni_ge_apply_eee_hw(struct rtl9617c_ni *ni, unsigned int id,
				   bool enable, u32 timer_us)
{
	u32 cnt, val;

	if (!rtl_ni_is_ge(id))
		return;

	cnt = timer_us ? min_t(u32, timer_us * 125U, 0xfffffU) :
			 EEE_LPI_TXFIFO_EMPTY_DFT;
	val = (enable ? (EEE_LPI_TX_EN | EEE_LPI_STOP_TXCLK) : 0U) |
	      FIELD_PREP(EEE_LPI_TXFIFO_EMPTY_MASK, cnt);

	rtl_ni_rmw(ni, RTL_NI_PORT_EEE_LPI_CFG0(id),
		   EEE_LPI_TX_EN | EEE_LPI_STOP_TXCLK |
		   EEE_LPI_TXFIFO_EMPTY_MASK,
		   val);
}

void rtl_ni_port_fc_ext_set(struct rtl9617c_ni_port *port, bool tx_pause,
			    bool rx_pause)
{
	struct rtl9617c_ni *ni = port->ni;
	struct net_device *dev = port->dev;
	bool half_duplex = false;

	rtl_ni_fc_ext_init(ni);

	if (dev && dev->phydev && dev->phydev->link &&
	    dev->phydev->duplex == DUPLEX_HALF)
		half_duplex = true;

	if (rtl_ni_is_ge(port->id)) {
		rtl_ni_rmw(ni, RTL_NI_PORT_RX(port->id),
			   PT_RXMAC_FC_TO_TX_EN | PT_RXMAC_PFC_DISABLE,
			   (rx_pause ? PT_RXMAC_FC_TO_TX_EN : 0U) |
			   PT_RXMAC_PFC_DISABLE);

		rtl_ni_rmw(ni, RTL_NI_PORT_TX(port->id),
			   PT_TXMAC_PFC_DISABLE | PT_TXMAC_PAU_SEL_MASK |
			   PT_TXMAC_BKPRS_EN | PT_TXMAC_AUTO_XON,
			   PT_TXMAC_PFC_DISABLE |
			   FIELD_PREP(PT_TXMAC_PAU_SEL_MASK, tx_pause ? 2 : 0) |
			   (half_duplex ? PT_TXMAC_BKPRS_EN : 0U) |
			   (tx_pause ? PT_TXMAC_AUTO_XON : 0U));

		rtl_ni_ge_set_pause_quanta(ni, port->id, tx_pause);
		return;
	}

	if (port->id == RTL_NI_PORT_P6) {
		rtl_ni_rmw(ni, RTL_NI_XGE6_FC_CFG,
			   XGE6_FC_QBB_TX_EN | XGE6_FC_QBB_RX_EN |
			   XGE6_FC_PAU_TX_EN | XGE6_FC_PAU_RX_EN |
			   XGE6_FC_RX_PAUSEREQ_SEL | XGE6_FC_TE_PAUSEREQ_SEL,
			   (tx_pause ? XGE6_FC_PAU_TX_EN : 0U) |
			   (rx_pause ? XGE6_FC_PAU_RX_EN : 0U) |
			   XGE6_FC_RX_PAUSEREQ_SEL | XGE6_FC_TE_PAUSEREQ_SEL);

		rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_RX,
			   XGE_CFG1_PAUSE_TERM | XGE_CFG1_PAUSE_EXTRACT,
			   rx_pause ? (XGE_CFG1_PAUSE_TERM |
				       XGE_CFG1_PAUSE_EXTRACT) : 0U);

		rtl_ni_rmw(ni, RTL_NI_XGE_MAC_CFG_TX,
			   XGE_CFGTX_PAUSE_REQ_EN | XGE_CFGTX_PAUSE_AUTO_XON,
			   tx_pause ? (XGE_CFGTX_PAUSE_REQ_EN |
				       XGE_CFGTX_PAUSE_AUTO_XON) : 0U);
	}
}

void rtl_ni_eee_sync_link(struct rtl9617c_ni_port *port)
{
	struct net_device *ndev;
	struct rtl_ni_eee_state *es;
	bool active;

	if (!port || !rtl_ni_is_ge(port->id) || port->id >= NI_MAX_PORT_SLOTS)
		return;

	ndev = port->dev;

	mutex_lock(&qos_lock);
	es = &qos_st.eee[port->id];
	active = es->mac_lpi_active;
	if (ndev && ndev->phydev)
		active = ndev->phydev->enable_tx_lpi &&
			 es->eee_enabled && es->tx_lpi_enabled;

	rtl_ni_ge_apply_eee_hw(port->ni, port->id, active, es->tx_lpi_timer);
	mutex_unlock(&qos_lock);
}

int rtl_ni_eee_get(struct net_device *dev, struct ethtool_keee *eee)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	int ret;

	ret = phylink_ethtool_get_eee(port->phylink, eee);
	if (ret == -EOPNOTSUPP && dev->phydev)
		ret = phy_ethtool_get_eee(dev->phydev, eee);

	if (!ret && port->id < NI_MAX_PORT_SLOTS) {
		u32 cfg0;

		mutex_lock(&qos_lock);
		eee->tx_lpi_timer = qos_st.eee[port->id].tx_lpi_timer;
		if (rtl_ni_is_ge(port->id)) {
			cfg0 = rtl_ni_read(port->ni,
					   RTL_NI_PORT_EEE_LPI_CFG0(port->id));
			eee->tx_lpi_enabled = !!(cfg0 & EEE_LPI_TX_EN);
		}
		mutex_unlock(&qos_lock);
	}

	return ret;
}

int rtl_ni_eee_set(struct net_device *dev, struct ethtool_keee *eee)
{
	struct rtl9617c_ni_port *port = netdev_priv(dev);
	struct rtl_ni_eee_state *es;
	bool active;
	int ret;

	ret = phylink_ethtool_set_eee(port->phylink, eee);
	if (ret == -EOPNOTSUPP && dev->phydev)
		ret = phy_ethtool_set_eee(dev->phydev, eee);
	if (ret)
		return ret;

	if (port->id < NI_MAX_PORT_SLOTS) {
		mutex_lock(&qos_lock);
		es = &qos_st.eee[port->id];
		es->eee_enabled = eee->eee_enabled;
		es->tx_lpi_enabled = eee->tx_lpi_enabled;
		es->tx_lpi_timer = eee->tx_lpi_timer;
		active = eee->eee_enabled && eee->tx_lpi_enabled &&
			 (!dev->phydev || dev->phydev->enable_tx_lpi);
		es->mac_lpi_active = active;
		rtl_ni_ge_apply_eee_hw(port->ni, port->id, active,
				       eee->tx_lpi_timer);
		mutex_unlock(&qos_lock);
	}

	return 0;
}

int rtl_ni_mac_enable_tx_lpi(struct phylink_config *config, u32 timer,
			     bool tx_clk_stop)
{
	struct rtl9617c_ni_port *port;

	if (!config || config->type != PHYLINK_NETDEV || !config->dev)
		return 0;

	port = netdev_priv(to_net_dev(config->dev));
	mutex_lock(&qos_lock);
	if (port->id < NI_MAX_PORT_SLOTS) {
		qos_st.eee[port->id].mac_lpi_active = true;
		qos_st.eee[port->id].tx_lpi_timer = timer;
	}
	rtl_ni_ge_apply_eee_hw(port->ni, port->id, true, timer);
	mutex_unlock(&qos_lock);

	return 0;
}

void rtl_ni_mac_disable_tx_lpi(struct phylink_config *config)
{
	struct rtl9617c_ni_port *port;

	if (!config || config->type != PHYLINK_NETDEV || !config->dev)
		return;

	port = netdev_priv(to_net_dev(config->dev));
	mutex_lock(&qos_lock);
	if (port->id < NI_MAX_PORT_SLOTS)
		qos_st.eee[port->id].mac_lpi_active = false;
	rtl_ni_ge_apply_eee_hw(port->ni, port->id, false, 0);
	mutex_unlock(&qos_lock);
}

static int l2te_port_shaper_set(struct rtl9617c_ni *ni, unsigned int port_id,
				bool enable, u64 rate_kbps, u32 burst_bytes)
{
	u32 rate_m, rate_k, min_bs, bs;
	u32 data0, data1;
	int ret;

	if (port_id > 15)
		return -EINVAL;

	rate_m = min_t(u64, div_u64(rate_kbps, 500ULL), 0xffffU);
	rate_k = min_t(u64, rate_kbps - (u64)rate_m * 500ULL, 0x3ffU);
	min_bs = max_t(u32, 1U, (rate_m + 511U) >> 9);
	bs = clamp_t(u32, DIV_ROUND_UP(burst_bytes ?: 4096U, 256U),
		     min_bs, 0xfffU);

	mutex_lock(&qos_lock);
	ret = l2te_wait_clear(ni, RTL_L2TE_SHP_PORT_ACCESS);
	if (ret)
		goto out;

	data0 = (rate_k & 0x3ffU) | ((rate_m & 0xffffU) << 10) |
		((bs & 0x3fU) << 26);
	data1 = ((bs >> 6) & 0x3fU) | ((enable ? 0x3U : 0U) << 6);

	rtl_ni_write(ni, RTL_L2TE_SHP_PORT_DATA0, data0);
	rtl_ni_write(ni, RTL_L2TE_SHP_PORT_DATA1, data1);
	rtl_ni_write(ni, RTL_L2TE_SHP_PORT_ACCESS,
		     L2TE_ACC_BUSY | L2TE_ACC_RBW_WRITE | (port_id & 0xfU));
	ret = l2te_wait_clear(ni, RTL_L2TE_SHP_PORT_ACCESS);
out:
	mutex_unlock(&qos_lock);
	return ret;
}

static int l2te_port_policer_set(struct rtl9617c_ni *ni, unsigned int port_id,
				 bool enable, u64 cir_kbps, u64 pir_kbps,
				 u32 burst_bytes)
{
	u32 cir_m, cir_k, pir_m, pir_k, min_bs, cbs, pbs;
	u32 data0, data1, data2, type;
	int ret;

	if (port_id > 15)
		return -EINVAL;

	cir_m = min_t(u64, div_u64(cir_kbps, 500ULL), 0xffffU);
	cir_k = min_t(u64, cir_kbps - (u64)cir_m * 500ULL, 0x3ffU);
	pir_m = min_t(u64, div_u64(pir_kbps, 500ULL), 0xffffU);
	pir_k = min_t(u64, pir_kbps - (u64)pir_m * 500ULL, 0x3ffU);
	min_bs = max_t(u32, 1U, (max(cir_m, pir_m) + 511U) >> 9);
	cbs = clamp_t(u32, DIV_ROUND_UP(burst_bytes ?: 4096U, 256U),
		      min_bs, 0xfffU);
	pbs = cbs;

	mutex_lock(&qos_lock);
	ret = l2te_wait_clear(ni, RTL_L2TE_POL_PORT_ACCESS);
	if (ret)
		goto out;

	if (enable) {
		data0 = (cir_k & 0x3ffU) | ((cir_m & 0xffffU) << 10) |
			((cbs & 0x3fU) << 26);
		data1 = ((cbs >> 6) & 0x3fU) | ((pir_k & 0x3ffU) << 6) |
			((pir_m & 0xffffU) << 16);
		data2 = pbs & 0xfffU;
		rtl_ni_write(ni, RTL_L2TE_POL_PORT_DATA0, data0);
		rtl_ni_write(ni, RTL_L2TE_POL_PORT_DATA1, data1);
		rtl_ni_write(ni, RTL_L2TE_POL_PORT_DATA2, data2);
		rtl_ni_write(ni, RTL_L2TE_POL_PORT_ACCESS,
			     L2TE_ACC_BUSY | L2TE_ACC_RBW_WRITE |
			     (port_id & 0xfU));
		ret = l2te_wait_clear(ni, RTL_L2TE_POL_PORT_ACCESS);
		if (ret)
			goto out;
	}

	type = !enable ? 0U : ((pir_kbps > cir_kbps) ? 2U : 1U);
	rtl_ni_rmw(ni, RTL_L2TE_POL_PORT_CTRL0(port_id),
		   POL_CTRL0_TYPE_MASK | POL_CTRL0_IPG_MASK |
		   POL_CTRL0_UPDATE_MODE,
		   FIELD_PREP(POL_CTRL0_TYPE_MASK, type) |
		   FIELD_PREP(POL_CTRL0_IPG_MASK, 20));
out:
	mutex_unlock(&qos_lock);
	return ret;
}

#define NI_LAN_DQ_PORT(id)	(8 + (id))

static void l2tm_es_set_weights(struct rtl9617c_ni *ni, unsigned int port_id,
				const u8 weights[NI_VOQ_NUM])
{
	unsigned int idx = (port_id == 15) ? 14 : port_id;
	u32 w30 = 0, w74 = 0;
	unsigned int q;

	if (idx >= 15)
		return;

	for (q = 0; q < 4; q++) {
		w30 |= (u32)weights[q] << (q * 8);
		w74 |= (u32)weights[q + 4] << (q * 8);
	}

	rtl_ni_rmw(ni, RTL_L2TM_ES_SCH_CFG0(idx), 0xffU, 0xffU);
	rtl_ni_write(ni, RTL_L2TM_ES_VOQ3_0_W(idx), w30);
	rtl_ni_write(ni, RTL_L2TM_ES_VOQ7_4_W(idx), w74);

	/*
	 * Downstream pon0 -> LAN offloaded flows traverse L3QM DeepQ port
	 * (8 + port_id); sync its L3TM ES weights so strict/WRR applies across
	 * DeepQ as well as L2TM ES.
	 */
	if (ni->qm.lan_dq_bufs &&
	    (rtl_ni_is_ge(port_id) || port_id == RTL_NI_PORT_P6)) {
		unsigned int dq_port = NI_LAN_DQ_PORT(port_id);

		rtl_ni_rmw(ni, RTL_QM_SCH_CFG(dq_port),
			   RTL_QM_SCH_VOQ_EN, RTL_QM_SCH_VOQ_EN);
		rtl_ni_write(ni, RTL_QM_VOQ3_0_WEIGHT_RATIO_CFG(dq_port), w30);
		rtl_ni_write(ni, RTL_QM_VOQ3_0_WEIGHT_RATIO_CFG(dq_port) + 4,
			     w74);
	}
}

static int qos_setup_tbf(struct rtl9617c_ni_port *port,
			 struct tc_tbf_qopt_offload *tbf)
{
	u64 kbps;

	switch (tbf->command) {
	case TC_TBF_REPLACE:
		kbps = div_u64(tbf->replace_params.rate.rate_bytes_ps * 8ULL,
			       1000ULL);
		return l2te_port_shaper_set(port->ni, port->id, true, kbps,
					    tbf->replace_params.max_size);
	case TC_TBF_DESTROY:
		return l2te_port_shaper_set(port->ni, port->id, false, 0, 0);
	default:
		return -EOPNOTSUPP;
	}
}

static int qos_setup_mqprio(struct rtl9617c_ni_port *port,
			    struct tc_mqprio_qopt_offload *mq)
{
	u8 weights[NI_VOQ_NUM] = { };

	if (mq->qopt.num_tc > NI_VOQ_NUM)
		return -EINVAL;

	/* Weight 0 = Strict Priority across all 8 hardware VoQs */
	l2tm_es_set_weights(port->ni, port->id, weights);
	return 0;
}

int rtl_ni_qos_ets_to_weights(const struct tc_ets_qopt_offload *ets,
			      u8 weights[NI_VOQ_NUM])
{
	unsigned int voq, bands, max_q = 0;

	memset(weights, 0, NI_VOQ_NUM);
	if (!ets)
		return -EINVAL;
	if (ets->command == TC_ETS_DESTROY)
		return 0;
	if (ets->command != TC_ETS_REPLACE)
		return -EOPNOTSUPP;

	bands = ets->replace_params.bands;
	if (!bands || bands > NI_VOQ_NUM)
		return -EOPNOTSUPP;

	for (voq = 0; voq < bands; voq++)
		max_q = max(max_q, ets->replace_params.quanta[voq]);

	for (voq = 0; voq < NI_VOQ_NUM; voq++) {
		unsigned int band = ets->replace_params.priomap[voq];
		u32 q;

		if (band >= bands)
			band = voq;
		if (band >= bands)
			continue;
		q = ets->replace_params.quanta[band];
		if (q && max_q)
			weights[voq] = clamp_t(u32,
					       div_u64((u64)q * 255ULL + max_q / 2,
						       max_q),
					       1U, 255U);
	}
	return 0;
}

static int qos_setup_ets(struct rtl9617c_ni_port *port,
			 struct tc_ets_qopt_offload *ets)
{
	u8 weights[NI_VOQ_NUM];
	int ret;

	if (!ets || ets->parent != TC_H_ROOT)
		return -EOPNOTSUPP;

	ret = rtl_ni_qos_ets_to_weights(ets, weights);
	if (ret)
		return ret;

	l2tm_es_set_weights(port->ni, port->id, weights);
	return 0;
}

static int qos_setup_flower(struct rtl9617c_ni_port *port,
			    struct flow_cls_offload *f)
{
	const struct flow_action_entry *act;
	u64 cir_kbps, pir_kbps, burst_bytes;
	int i;

	if (port->id >= NI_MAX_PORT_SLOTS)
		return -EOPNOTSUPP;

	if (f->command == FLOW_CLS_DESTROY) {
		if (qos_st.police_active[port->id] &&
		    qos_st.police_cookie[port->id] == f->cookie) {
			qos_st.police_active[port->id] = false;
			qos_st.police_cookie[port->id] = 0;
			return l2te_port_policer_set(port->ni, port->id, false,
						     0, 0, 0);
		}
		return 0;
	}

	if (f->command != FLOW_CLS_REPLACE || !f->rule)
		return -EOPNOTSUPP;

	flow_action_for_each(i, act, &f->rule->action) {
		if (act->id != FLOW_ACTION_POLICE)
			continue;

		cir_kbps = div_u64(act->police.rate_bytes_ps * 8ULL, 1000ULL);
		pir_kbps = max(cir_kbps,
			       div_u64(act->police.peakrate_bytes_ps * 8ULL,
				       1000ULL));
		burst_bytes = div_u64(act->police.rate_bytes_ps *
				      PSCHED_NS2TICKS(act->police.burst),
				      PSCHED_TICKS_PER_SEC);
		qos_st.police_cookie[port->id] = f->cookie;
		qos_st.police_active[port->id] = true;
		return l2te_port_policer_set(port->ni, port->id, true,
					     cir_kbps, pir_kbps,
					     (u32)burst_bytes);
	}

	return -EOPNOTSUPP;
}

static LIST_HEAD(rtl_ni_qos_block_cb_list);

static int rtl_ni_qos_block_cb(enum tc_setup_type type, void *type_data,
				 void *cb_priv)
{
	struct rtl9617c_ni_port *port = cb_priv;
	struct flow_cls_offload *f = type_data;

	if (type != TC_SETUP_CLSFLOWER || !f || f->common.chain_index)
		return -EOPNOTSUPP;

	return qos_setup_flower(port, f);
}

static int qos_setup_block(struct rtl9617c_ni_port *port,
			   struct flow_block_offload *f)
{
	if (!f || f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	return flow_block_cb_setup_simple(f, &rtl_ni_qos_block_cb_list,
					  rtl_ni_qos_block_cb,
					  port, port, true);
}

int rtl_ni_qos_setup_tc(struct rtl9617c_ni_port *port, enum tc_setup_type type,
			void *type_data)
{
	switch (type) {
	case TC_SETUP_QDISC_TBF:
		return qos_setup_tbf(port, type_data);
	case TC_SETUP_QDISC_MQPRIO:
		return qos_setup_mqprio(port, type_data);
	case TC_SETUP_QDISC_ETS:
		return qos_setup_ets(port, type_data);
	case TC_SETUP_CLSFLOWER:
		return qos_setup_flower(port, type_data);
	case TC_SETUP_BLOCK:
		return qos_setup_block(port, type_data);
	default:
		return -EOPNOTSUPP;
	}
}
