// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_bridge.h>
#include <linux/ppp_defs.h>
#include <linux/rtl9617c-bridge.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <net/net_namespace.h>
#include <net/pkt_cls.h>
#include <net/switchdev.h>

#include "fe_priv.h"
#include "rtl9617c_itu.h"

#define FE_BRIDGE_PENDING	256
#define FE_PPPOE_VER_TYPE	0x11

static unsigned int learned_max = 16384;
module_param(learned_max, uint, 0644);
MODULE_PARM_DESC(learned_max, "Maximum number of learned bridge flows");

struct rtl_fe_bridge_packet {
	struct net *net;
	struct rtl9617c_bridge_frame input;
	struct rtl9617c_bridge_frame output;
	unsigned int bridge;
	unsigned int ingress;
	unsigned int egress;
	u16 vid;
};

struct rtl_fe_bridge_hook {
	struct list_head list;
	struct nf_hook_ops ops;
};

struct rtl_fe_bridge {
	struct rtl_fe *fe;
	struct delayed_work work;
	struct notifier_block netdev_nb;
	struct notifier_block switchdev_nb;
	struct notifier_block switchdev_blocking_nb;
	struct nf_hook_ops hooks[2];
	struct list_head ingress_hooks;
	spinlock_t pending_lock;
	unsigned int read;
	unsigned int write;
	atomic_t epoch;
	atomic_t flush;
	unsigned int learned;
	u64 drops;
	bool stopping;
	struct rtl_fe_bridge_packet pending[FE_BRIDGE_PENDING];
};

static struct net_device *rtl_fe_bridge_lower(struct net_device *dev,
					      struct rtl9617c_bridge_frame *frame,
					      u32 priority)
{
	while (is_vlan_dev(dev)) {
		if (frame->tags == 2)
			return NULL;
		frame->tag[1] = frame->tag[0];
		frame->tag[0] = (struct rtl9617c_bridge_tag){
			.tci = vlan_dev_vlan_id(dev) |
			       vlan_dev_get_egress_qos_mask(dev, priority),
			.protocol = ntohs(vlan_dev_vlan_proto(dev)),
		};
		frame->tags++;
		dev = vlan_dev_real_dev(dev);
	}
	return dev;
}

static int rtl_fe_bridge_frame(struct sk_buff *skb,
			       struct rtl9617c_bridge_frame *frame)
{
	__be16 protocol = eth_hdr(skb)->h_proto;
	struct vlan_hdr storage, *vlan;
	unsigned int offset = 0;

	ether_addr_copy(frame->source, eth_hdr(skb)->h_source);
	ether_addr_copy(frame->destination, eth_hdr(skb)->h_dest);
	if (!is_valid_ether_addr(frame->source) ||
	    !is_valid_ether_addr(frame->destination))
		return -EINVAL;

	if (skb_vlan_tag_present(skb)) {
		frame->tag[0] = (struct rtl9617c_bridge_tag){
			.tci = skb_vlan_tag_get(skb),
			.protocol = ntohs(skb->vlan_proto),
		};
		frame->tags = 1;
	}
	while (eth_type_vlan(protocol)) {
		if (frame->tags == 2)
			return -E2BIG;
		vlan = skb_header_pointer(skb, offset, sizeof(storage), &storage);
		if (!vlan)
			return -EINVAL;
		frame->tag[frame->tags++] = (struct rtl9617c_bridge_tag){
			.tci = ntohs(vlan->h_vlan_TCI),
			.protocol = ntohs(protocol),
		};
		protocol = vlan->h_vlan_encapsulated_proto;
		offset += sizeof(storage);
	}

	frame->protocol = ntohs(protocol);
	if (protocol == htons(ETH_P_PPP_SES)) {
		u8 hdr[8];
		const u8 *pppoe;
		u16 ppp;

		pppoe = skb_header_pointer(skb, offset, sizeof(hdr), hdr);
		if (!pppoe || pppoe[0] != FE_PPPOE_VER_TYPE || pppoe[1])
			return -EINVAL;
		ppp = get_unaligned_be16(pppoe + 6);
		if (ppp != PPP_IP && ppp != PPP_IPV6)
			return -EOPNOTSUPP;
		frame->session = get_unaligned_be16(pppoe + 2);
		frame->ppp_protocol = ppp;
	}
	return 0;
}

static unsigned int rtl_fe_bridge_ingress(void *priv, struct sk_buff *skb,
					  const struct nf_hook_state *state)
{
	struct rtl9617c_bridge_frame frame = { }, *saved;
	const struct rtl9617c_bridge_frame *received;
	struct rtl_fe_bridge *bridge = priv;

	if (!rtl_fe_is_enabled(bridge->fe) || !rtl_fe_has_bindings(bridge->fe) ||
	    !tc_can_offload(state->in) || skb_is_gso(skb) ||
	    rtl_fe_bridge_frame(skb, &frame))
		return NF_ACCEPT;

	frame.ingress = state->in->ifindex;
	frame.epoch = atomic_read(&bridge->epoch);
	received = skb_ext_find(skb, SKB_EXT_RTL9617C_BRIDGE);
	if (received && received->pon_gem) {
		frame.pon_gem = received->pon_gem;
		frame.pon_generation = received->pon_generation;
		frame.tags = received->tags;
		memcpy(frame.tag, received->tag, sizeof(frame.tag));
	}
	saved = skb_ext_add(skb, SKB_EXT_RTL9617C_BRIDGE);
	if (saved)
		*saved = frame;
	return NF_ACCEPT;
}

static bool rtl_fe_bridge_net_hooked(struct rtl_fe_bridge *bridge, struct net *net)
{
	struct rtl_fe_bridge_hook *hook;

	list_for_each_entry(hook, &bridge->ingress_hooks, list)
		if (dev_net(hook->ops.dev) == net)
			return true;
	return false;
}

static void rtl_fe_bridge_unhook(struct rtl_fe_bridge *bridge,
				 struct rtl_fe_bridge_hook *hook)
{
	struct net *net = dev_net(hook->ops.dev);

	nf_unregister_net_hook(net, &hook->ops);
	list_del(&hook->list);
	kfree(hook);
	if (!rtl_fe_bridge_net_hooked(bridge, net))
		nf_unregister_net_hooks(net, bridge->hooks, ARRAY_SIZE(bridge->hooks));
}

static void rtl_fe_bridge_hook_dev(struct rtl_fe_bridge *bridge,
				   struct net_device *dev, bool add)
{
	struct rtl_fe_bridge_hook *hook;
	bool first, known;
	int ret;

	list_for_each_entry(hook, &bridge->ingress_hooks, list) {
		if (hook->ops.dev != dev)
			continue;
		if (!add)
			rtl_fe_bridge_unhook(bridge, hook);
		return;
	}
	if (!add)
		return;

	mutex_lock(&bridge->fe->lock);
	known = rtl_fe_port_find(bridge->fe, dev);
	mutex_unlock(&bridge->fe->lock);
	if (!known)
		return;

	hook = kzalloc(sizeof(*hook), GFP_KERNEL);
	if (!hook)
		return;
	hook->ops = (struct nf_hook_ops){
		.hook = rtl_fe_bridge_ingress,
		.priv = bridge,
		.dev = dev,
		.pf = NFPROTO_NETDEV,
		.hooknum = NF_NETDEV_INGRESS,
		.priority = INT_MIN + 1,
	};
	ret = nf_register_net_hook(dev_net(dev), &hook->ops);
	if (ret) {
		kfree(hook);
		return;
	}

	first = !rtl_fe_bridge_net_hooked(bridge, dev_net(dev));
	if (first)
		nf_register_net_hooks(dev_net(dev), bridge->hooks,
				      ARRAY_SIZE(bridge->hooks));
	list_add_tail(&hook->list, &bridge->ingress_hooks);
}

static unsigned int rtl_fe_bridge_capture(void *priv, struct sk_buff *skb,
					  const struct nf_hook_state *state)
{
	struct rtl9617c_bridge_frame frame = { }, *saved;
	const struct rtl9617c_bridge_frame *wire;
	struct rtl_fe_bridge *bridge = priv;
	struct net_device *brdev;
	u16 protocol;

	if (!rtl_fe_is_enabled(bridge->fe) || !rtl_fe_has_bindings(bridge->fe) ||
	    skb_is_gso(skb) || !state->in ||
	    rtl_fe_bridge_frame(skb, &frame))
		return NF_ACCEPT;

	frame.port = state->in->ifindex;
	brdev = netdev_master_upper_dev_get_rcu(state->in);
	if (!brdev || !netif_is_bridge_master(brdev))
		return NF_ACCEPT;
	if (br_vlan_enabled(brdev)) {
		br_vlan_get_proto(brdev, &protocol);
		if (frame.tags && frame.tag[0].protocol == protocol)
			frame.vid = frame.tag[0].tci & VLAN_VID_MASK;
		if (!frame.vid && br_vlan_get_pvid_rcu(state->in, &frame.vid))
			return NF_ACCEPT;
	}

	wire = skb_ext_find(skb, SKB_EXT_RTL9617C_BRIDGE);
	if (!wire || wire->epoch != (u32)atomic_read(&bridge->epoch))
		return NF_ACCEPT;
	frame.ingress = wire->ingress;
	frame.epoch = wire->epoch;
	frame.pon_gem = wire->pon_gem;
	frame.pon_generation = wire->pon_generation;
	frame.tags = wire->tags;
	memcpy(frame.tag, wire->tag, sizeof(frame.tag));
	saved = skb_ext_add(skb, SKB_EXT_RTL9617C_BRIDGE);
	if (saved)
		*saved = frame;
	return NF_ACCEPT;
}

static unsigned int rtl_fe_bridge_forward(void *priv, struct sk_buff *skb,
					  const struct nf_hook_state *state)
{
	const struct rtl9617c_bridge_frame *saved;
	struct rtl_fe_bridge_packet packet = { .net = state->net };
	struct rtl_fe_bridge *bridge = priv;
	struct net_device *brdev, *lower;
	unsigned int next;

	saved = skb_ext_find(skb, SKB_EXT_RTL9617C_BRIDGE);
	if (!saved || !saved->port || !state->out ||
	    saved->epoch != (u32)atomic_read(&bridge->epoch) ||
	    rtl_fe_bridge_frame(skb, &packet.output))
		return NF_ACCEPT;

	lower = rtl_fe_bridge_lower(state->out, &packet.output, skb->priority);
	brdev = netdev_master_upper_dev_get_rcu(state->out);
	if (!lower || !brdev || !netif_is_bridge_master(brdev))
		return NF_ACCEPT;

	packet.input = *saved;
	packet.output.port = state->out->ifindex;
	packet.egress = lower->ifindex;
	packet.ingress = saved->port;
	packet.bridge = brdev->ifindex;
	packet.vid = saved->vid;
	if (packet.input.protocol != packet.output.protocol ||
	    packet.input.session != packet.output.session ||
	    packet.input.ppp_protocol != packet.output.ppp_protocol ||
	    !ether_addr_equal(packet.input.source, packet.output.source) ||
	    !ether_addr_equal(packet.input.destination, packet.output.destination))
		return NF_ACCEPT;

	spin_lock_bh(&bridge->pending_lock);
	next = (bridge->write + 1) % FE_BRIDGE_PENDING;
	if (next != bridge->read) {
		get_net(packet.net);
		bridge->pending[bridge->write] = packet;
		bridge->write = next;
	}
	spin_unlock_bh(&bridge->pending_lock);
	mod_delayed_work(system_wq, &bridge->work, 0);
	return NF_ACCEPT;
}

static bool rtl_fe_bridge_ports(struct net *net, unsigned int index,
				unsigned int ingress, unsigned int egress,
				const u8 *source, const u8 *destination, u16 vid)
{
	struct net_device *brdev = __dev_get_by_index(net, index);
	struct net_device *in = __dev_get_by_index(net, ingress);
	struct net_device *out = __dev_get_by_index(net, egress);

	if (!brdev || !in || !out || !netif_running(in) || !netif_running(out) ||
	    !netif_carrier_ok(in) || !netif_carrier_ok(out) ||
	    netdev_master_upper_dev_get(in) != brdev ||
	    netdev_master_upper_dev_get(out) != brdev ||
	    br_port_get_stp_state(in) != BR_STATE_FORWARDING ||
	    br_port_get_stp_state(out) != BR_STATE_FORWARDING)
		return false;
	return br_fdb_find_port(brdev, source, vid) == in &&
	       br_fdb_find_port(brdev, destination, vid) == out;
}

static bool rtl_fe_bridge_known(struct rtl_fe *fe, const struct rtl_fe_rule *data,
				const struct rtl_fe_bridge_packet *packet)
{
	struct rtl_fe_flow *flow;

	list_for_each_entry(flow, &fe->flows, list) {
		if (flow->learned && flow->input == data->input &&
		    flow->output == data->output &&
		    flow->ethernet_protocol == packet->input.protocol &&
		    flow->ingress_session == packet->input.session &&
		    flow->bridge_ppp_protocol == packet->input.ppp_protocol &&
		    flow->bridge_vid == packet->vid &&
		    ether_addr_equal(flow->ethernet_source, packet->input.source) &&
		    ether_addr_equal(flow->destination, packet->input.destination) &&
		    flow->bridge_tags == packet->input.tags &&
		    !memcmp(flow->bridge_tag, packet->input.tag,
			    sizeof(packet->input.tag)))
			return true;
	}
	return false;
}

static int rtl_fe_bridge_learn(struct rtl_fe_bridge *bridge,
			       const struct rtl_fe_bridge_packet *packet)
{
	const struct rtl9617c_bridge_frame *in = &packet->input;
	const struct rtl9617c_bridge_frame *out = &packet->output;
	struct rtl_fe_rule data = {
		.kind = RTL_FE_RULE_BRIDGE,
		.vlan_exact = true,
	};
	struct rtl_fe *fe = bridge->fe;
	struct rtl_fe_flow *flow;
	unsigned int i;
	int ret;

	if (in->epoch != (u32)atomic_read(&bridge->epoch) ||
	    !rtl_fe_bridge_ports(packet->net, packet->bridge, packet->ingress,
				 out->port, in->source, in->destination, packet->vid))
		return -ENOENT;

	data.input = rtl_fe_port_find(fe, __dev_get_by_index(packet->net, in->ingress));
	data.output = rtl_fe_port_find(fe, __dev_get_by_index(packet->net,
							      packet->egress));
	if (!data.input || !data.output || data.input == data.output || in->tags > 2)
		return -EOPNOTSUPP;
	if (rtl_fe_bridge_known(fe, &data, packet))
		return 0;

	data.key = (struct rtl_fe_key){
		.ingress = data.input->source,
		.llid = data.input->llid,
		.mask = RTL_FE_MASK_LEARN,
	};
	data.action = (struct rtl_fe_action){
		.bridge = 1,
		.destination = data.output->destination,
		.deep_queue = data.output->destination >= FE_DEEPQ_DEST,
	};
	data.ethernet_protocol = in->protocol;
	data.pppoe_input = in->protocol == ETH_P_PPP_SES;
	data.pppoe_session = in->session;
	if (data.pppoe_input) {
		data.key.mask = RTL_FE_MASK_LEARN_PPPOE;
		data.ppp_protocol_slot = fe->ppp_protocol_ids[in->ppp_protocol == PPP_IPV6];
	}
	data.action.check_mask = data.key.mask;
	ether_addr_copy(data.source, in->source);
	ether_addr_copy(data.destination, in->destination);

	data.vlan_count = in->tags;
	for (i = 0; i < in->tags; i++) {
		ret = rtl_fe_vlan_tpid(htons(in->tag[i].protocol));
		if (ret < 0)
			return ret;
		data.vlan[i] = (struct rtl_fe_vlan){
			.vid = in->tag[i].tci & VLAN_VID_MASK,
			.tpid = ret,
		};
		data.ingress_tci[i] = in->tag[i].tci;
	}

	data.action.vlan_count = out->tags;
	data.action.vlan_set = in->tags != out->tags ||
			       memcmp(in->tag, out->tag, sizeof(in->tag));
	for (i = 0; i < out->tags; i++) {
		ret = rtl_fe_vlan_tpid(htons(out->tag[i].protocol));
		if (ret < 0)
			return ret;
		data.action.vlan[i] = (struct rtl_fe_vlan){
			.vid = out->tag[i].tci & VLAN_VID_MASK,
			.tpid = ret,
			.priority = out->tag[i].tci >> VLAN_PRIO_SHIFT,
			.dei = !!(out->tag[i].tci & VLAN_CFI_MASK),
		};
	}

	ret = rtl_fe_itu_learn(&data, in->pon_gem, in->pon_generation);
	if (ret)
		return ret;

	data.ethernet_bytes = ETH_HLEN + VLAN_HLEN * data.action.vlan_count +
			      (data.pppoe_input ? 8 : 0);
	if (bridge->learned >= learned_max) {
		bridge->drops++;
		return -ENOSPC;
	}

	ret = rtl_fe_install(data.input, &data, 0, &flow);
	if (ret)
		return ret;

	bridge->learned++;
	flow->learned = true;
	flow->bridge_index = packet->bridge;
	flow->bridge_vid = packet->vid;
	flow->bridge_ingress = packet->ingress;
	flow->bridge_egress = out->port;
	flow->bridge_tags = in->tags;
	flow->bridge_ppp_protocol = in->ppp_protocol;
	memcpy(flow->bridge_tag, in->tag, sizeof(flow->bridge_tag));
	return 0;
}

static void rtl_fe_bridge_age(struct rtl_fe *fe, struct rtl_fe_flow *flow,
			      bool flush)
{
	struct net *net = dev_net(flow->input->netdev);
	struct net_device *brdev;
	unsigned long lifetime;
	unsigned int age;

	if (flush || !rtl_fe_bridge_ports(net, flow->bridge_index,
					  flow->bridge_ingress, flow->bridge_egress,
					  flow->ethernet_source, flow->destination,
					  flow->bridge_vid)) {
		rtl_fe_delete(fe, flow);
		return;
	}

	brdev = __dev_get_by_index(net, flow->bridge_index);
	lifetime = clock_t_to_jiffies(br_get_ageing_time(brdev));
	if (rtl_fe_age_read(fe, flow->hash_index, &age))
		return;
	if (age > 1) {
		rtl_fe_age_write(fe, flow->hash_index, 1);
		flow->lastused = jiffies;
		br_fdb_refresh_offload(__dev_get_by_index(net, flow->bridge_ingress),
				       flow->ethernet_source, flow->bridge_vid, true);
		br_fdb_refresh_offload(__dev_get_by_index(net, flow->bridge_egress),
				       flow->destination, flow->bridge_vid, false);
	} else if (lifetime && time_after(jiffies, flow->lastused + lifetime)) {
		rtl_fe_delete(fe, flow);
	}
}

static void rtl_fe_bridge_work(struct work_struct *work)
{
	struct rtl_fe_bridge *bridge = container_of(to_delayed_work(work),
						    struct rtl_fe_bridge, work);
	bool flush = atomic_xchg(&bridge->flush, 0);
	unsigned int budget = FE_BRIDGE_PENDING;
	struct rtl_fe *fe = bridge->fe;
	struct rtl_fe_flow *flow, *next;
	bool active = false;

	rtnl_lock();
	mutex_lock(&fe->lock);
	list_for_each_entry_safe(flow, next, &fe->flows, list) {
		if (!flow->learned)
			continue;
		if (flow->deleting || flow->input->retired || flow->output->retired)
			rtl_fe_delete(fe, flow);
		else
			rtl_fe_bridge_age(fe, flow, flush);
	}

	while (budget--) {
		struct rtl_fe_bridge_packet packet;

		spin_lock_bh(&bridge->pending_lock);
		if (bridge->read == bridge->write) {
			spin_unlock_bh(&bridge->pending_lock);
			break;
		}
		packet = bridge->pending[bridge->read];
		bridge->read = (bridge->read + 1) % FE_BRIDGE_PENDING;
		spin_unlock_bh(&bridge->pending_lock);

		rtl_fe_bridge_learn(bridge, &packet);
		put_net(packet.net);
	}

	list_for_each_entry(flow, &fe->flows, list) {
		if (flow->learned && flow->published) {
			active = true;
			break;
		}
	}
	mutex_unlock(&fe->lock);
	rtnl_unlock();

	if (active && !READ_ONCE(bridge->stopping))
		queue_delayed_work(system_wq, &bridge->work, HZ);
}

static void rtl_fe_bridge_invalidate(struct rtl_fe_bridge *bridge, bool flush)
{
	atomic_inc(&bridge->epoch);
	if (flush)
		atomic_set(&bridge->flush, 1);
	mod_delayed_work(system_wq, &bridge->work, 0);
}

static int rtl_fe_bridge_netdev_event(struct notifier_block *nb,
				      unsigned long event, void *ptr)
{
	struct rtl_fe_bridge *bridge = container_of(nb, struct rtl_fe_bridge,
						    netdev_nb);
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	switch (event) {
	case NETDEV_REGISTER:
	case NETDEV_UP:
		rtl_fe_bridge_hook_dev(bridge, dev, true);
		break;
	case NETDEV_UNREGISTER:
		rtl_fe_bridge_hook_dev(bridge, dev, false);
		break;
	case NETDEV_DOWN:
	case NETDEV_CHANGE:
	case NETDEV_CHANGEUPPER:
	case NETDEV_PRECHANGEUPPER:
	case NETDEV_CHANGEINFODATA:
	case NETDEV_CHANGEADDR:
		rtl_fe_bridge_invalidate(bridge, true);
		break;
	}
	return NOTIFY_DONE;
}

static int rtl_fe_bridge_switchdev(struct rtl_fe_bridge *bridge,
				   unsigned long event)
{
	switch (event) {
	case SWITCHDEV_FDB_ADD_TO_DEVICE:
	case SWITCHDEV_FDB_DEL_TO_DEVICE:
		rtl_fe_bridge_invalidate(bridge, false);
		break;
	case SWITCHDEV_PORT_OBJ_ADD:
	case SWITCHDEV_PORT_OBJ_DEL:
	case SWITCHDEV_PORT_ATTR_SET:
		rtl_fe_bridge_invalidate(bridge, true);
		break;
	}
	return NOTIFY_DONE;
}

static int rtl_fe_bridge_switchdev_event(struct notifier_block *nb,
					 unsigned long event, void *ptr)
{
	return rtl_fe_bridge_switchdev(container_of(nb, struct rtl_fe_bridge,
						    switchdev_nb), event);
}

static int rtl_fe_bridge_switchdev_blocking_event(struct notifier_block *nb,
						  unsigned long event, void *ptr)
{
	return rtl_fe_bridge_switchdev(container_of(nb, struct rtl_fe_bridge,
						    switchdev_blocking_nb), event);
}

void rtl_fe_bridge_flow_removed(struct rtl_fe *fe)
{
	if (fe->bridge && fe->bridge->learned)
		fe->bridge->learned--;
}

static void rtl_fe_bridge_drain(struct rtl_fe_bridge *bridge)
{
	while (bridge->read != bridge->write) {
		put_net(bridge->pending[bridge->read].net);
		bridge->read = (bridge->read + 1) % FE_BRIDGE_PENDING;
	}
}

struct rtl_fe_bridge *rtl_fe_bridge_start(struct rtl_fe *fe)
{
	struct rtl_fe_bridge *bridge;
	int ret;

	bridge = kvzalloc(sizeof(*bridge), GFP_KERNEL);
	if (!bridge)
		return ERR_PTR(-ENOMEM);

	bridge->fe = fe;
	INIT_LIST_HEAD(&bridge->ingress_hooks);
	spin_lock_init(&bridge->pending_lock);
	INIT_DELAYED_WORK(&bridge->work, rtl_fe_bridge_work);
	bridge->netdev_nb.notifier_call = rtl_fe_bridge_netdev_event;
	bridge->switchdev_nb.notifier_call = rtl_fe_bridge_switchdev_event;
	bridge->switchdev_blocking_nb.notifier_call =
		rtl_fe_bridge_switchdev_blocking_event;
	bridge->hooks[0] = (struct nf_hook_ops){
		.hook = rtl_fe_bridge_capture,
		.priv = bridge,
		.pf = NFPROTO_BRIDGE,
		.hooknum = NF_BR_PRE_ROUTING,
		.priority = INT_MIN,
	};
	bridge->hooks[1] = (struct nf_hook_ops){
		.hook = rtl_fe_bridge_forward,
		.priv = bridge,
		.pf = NFPROTO_BRIDGE,
		.hooknum = NF_BR_POST_ROUTING,
		.priority = INT_MAX,
	};

	ret = register_switchdev_notifier(&bridge->switchdev_nb);
	if (ret)
		goto free;
	ret = register_switchdev_blocking_notifier(&bridge->switchdev_blocking_nb);
	if (ret)
		goto unregister_switchdev;
	ret = register_netdevice_notifier(&bridge->netdev_nb);
	if (ret)
		goto unregister_blocking;
	return bridge;

unregister_blocking:
	unregister_switchdev_blocking_notifier(&bridge->switchdev_blocking_nb);
unregister_switchdev:
	unregister_switchdev_notifier(&bridge->switchdev_nb);
free:
	WRITE_ONCE(bridge->stopping, true);
	cancel_delayed_work_sync(&bridge->work);
	rtl_fe_bridge_drain(bridge);
	kvfree(bridge);
	return ERR_PTR(ret);
}

void rtl_fe_bridge_stop(struct rtl_fe_bridge *bridge)
{
	struct rtl_fe_bridge_hook *hook, *next;

	WRITE_ONCE(bridge->stopping, true);
	unregister_netdevice_notifier(&bridge->netdev_nb);
	rtnl_lock();
	list_for_each_entry_safe(hook, next, &bridge->ingress_hooks, list)
		rtl_fe_bridge_unhook(bridge, hook);
	rtnl_unlock();
	unregister_switchdev_blocking_notifier(&bridge->switchdev_blocking_nb);
	unregister_switchdev_notifier(&bridge->switchdev_nb);
	cancel_delayed_work_sync(&bridge->work);
	rtl_fe_bridge_drain(bridge);
	bridge->fe->bridge = NULL;
	kvfree(bridge);
}
