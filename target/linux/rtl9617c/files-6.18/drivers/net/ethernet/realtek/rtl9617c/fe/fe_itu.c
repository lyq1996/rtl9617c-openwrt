// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/in.h>

#include "fe_priv.h"
#include "rtl9617c_itu.h"

#define FE_ITU_DEP_DSCP			BIT(0)
#define FE_ITU_DEP_CRITERIA		BIT(1)
#define FE_ITU_TCI_VID			GENMASK(11, 0)
#define FE_ITU_TCI_DEI			BIT(12)
#define FE_ITU_TCI_PCP_SHIFT		13

struct rtl_fe_itu_result {
	const struct rtl_itu_path *path;
	struct rtl_evto_packet output;
	bool passthrough;
};

static bool rtl_fe_itu_equal(const struct rtl_evto_packet *a,
			     const struct rtl_evto_packet *b)
{
	return a->count == b->count &&
	       !memcmp(a->tag, b->tag, a->count * sizeof(a->tag[0]));
}

static bool rtl_fe_itu_shape(const struct rtl_evto_packet *a,
			     const struct rtl_evto_packet *b)
{
	unsigned int i;

	if (a->count != b->count)
		return false;
	for (i = 0; i < a->count; i++)
		if (a->tag[i].tpid != b->tag[i].tpid ||
		    (a->tag[i].tci & FE_ITU_TCI_VID) != (b->tag[i].tci & FE_ITU_TCI_VID))
			return false;
	return true;
}

static unsigned int rtl_fe_itu_dependencies(const struct rtl_itu_paths *paths)
{
	unsigned int flags = 0, i, n;

	for (i = 0; i < paths->count; i++)
		if (paths->path[i].unmarked_mode == 1)
			flags |= FE_ITU_DEP_DSCP;
	for (i = 0; i < paths->profile_count; i++) {
		for (n = 0; n < paths->profile[i].count; n++) {
			const u8 *row = paths->profile[i].row[n];

			if ((row[9] & 15) == 10 || (row[13] & 15) == 10)
				flags |= FE_ITU_DEP_DSCP;
			if ((rtl_evto_word(row + 4) >> 4) & 255)
				flags |= FE_ITU_DEP_CRITERIA;
		}
	}
	return flags;
}

static u16 rtl_fe_itu_tci(const struct rtl_fe_vlan *vlan)
{
	return vlan->vid | vlan->priority << FE_ITU_TCI_PCP_SHIFT |
	       (vlan->dei ? FE_ITU_TCI_DEI : 0);
}

static void rtl_fe_itu_packet(struct rtl_evto_packet *packet,
			      const struct rtl_fe_vlan *tags, unsigned int count,
			      unsigned int protocol)
{
	unsigned int i;

	*packet = (struct rtl_evto_packet){ .count = count, .protocol = protocol };
	for (i = 0; i < count; i++)
		packet->tag[i] = (struct rtl_evto_tag){
			tags[i].tpid == 1 ? ETH_P_8021Q : ETH_P_8021AD,
			rtl_fe_itu_tci(&tags[i]),
		};
	if (count)
		packet->priority = packet->tag[0].tci >> FE_ITU_TCI_PCP_SHIFT;
}

static void rtl_fe_itu_variant(struct rtl_evto_packet *packet, unsigned int v)
{
	unsigned int i;

	if (!packet->count) {
		packet->priority = v;
		return;
	}
	for (i = 0; i < packet->count; i++)
		packet->tag[i].tci = (packet->tag[i].tci & FE_ITU_TCI_VID) |
				     ((v >> (i * 4)) & 7) << FE_ITU_TCI_PCP_SHIFT |
				     ((v >> (i * 4 + 3)) & 1) << 12;
	packet->priority = packet->tag[0].tci >> FE_ITU_TCI_PCP_SHIFT;
}

static int rtl_fe_itu_output(struct rtl_fe_rule *data,
			     const struct rtl_evto_packet *packet, bool replace)
{
	unsigned int i;
	int type;

	if (packet->count > 2)
		return -EOPNOTSUPP;
	data->action.vlan_count = packet->count;
	data->action.vlan_set = replace;
	for (i = 0; i < packet->count; i++) {
		type = rtl_fe_vlan_tpid(htons(packet->tag[i].tpid));
		if (type < 0)
			return type;
		data->action.vlan[i] = (struct rtl_fe_vlan){
			.vid = packet->tag[i].tci & FE_ITU_TCI_VID,
			.tpid = type,
			.priority = packet->tag[i].tci >> FE_ITU_TCI_PCP_SHIFT,
			.dei = !!(packet->tag[i].tci & FE_ITU_TCI_DEI),
		};
	}
	data->ethernet_bytes = ETH_HLEN + VLAN_HLEN * packet->count +
			       (data->egress_pppoe ? 8 : 0);
	return 0;
}

static int rtl_fe_itu_invariant(const struct rtl_itu_paths *paths,
				const struct rtl_evto_packet *prototype,
				bool unknown_tci, bool bridge, bool downstream,
				unsigned int gem, struct rtl_fe_itu_result *result)
{
	unsigned int deps = rtl_fe_itu_dependencies(paths);
	unsigned int variants, dscps, criteria, v, d, c;
	bool first = true, constant = true, pass = true;

	if (prototype->count > 2)
		return -EOPNOTSUPP;

	if (unknown_tci)
		variants = prototype->count ? 1U << (prototype->count * 4) : 8;
	else
		variants = prototype->count ? 1 : 8;
	dscps = !downstream && (deps & FE_ITU_DEP_DSCP) &&
		(prototype->protocol == ETH_P_IP || prototype->protocol == ETH_P_IPV6 ||
		 prototype->protocol == ETH_P_PPP_SES) ? 64 : 1;
	criteria = bridge && (deps & FE_ITU_DEP_CRITERIA) ? 3 : 1;

	for (v = 0; v < variants; v++) {
		for (d = 0; d < dscps; d++) {
			for (c = 0; c < criteria; c++) {
				struct rtl_evto_packet input = *prototype, output;
				const struct rtl_itu_path *path;

				if (!input.count || unknown_tci)
					rtl_fe_itu_variant(&input, v);
				input.dscp = d;
				if (criteria > 1) {
					if (c == 1 && input.protocol == ETH_P_IPV6)
						continue;
					if (c == 2 && input.protocol == ETH_P_IP)
						continue;
					if (c && input.protocol != ETH_P_IP &&
					    input.protocol != ETH_P_IPV6 &&
					    input.protocol != ETH_P_PPP_SES)
						continue;
					input.criteria = c;
				}

				if (downstream)
					path = rtl_itu_path_receive_vlan(paths, gem,
									 &input, &output);
				else
					path = rtl_itu_path_transmit_vlan(paths, &input,
									  &output);
				if (!path || output.count > 2)
					return -EOPNOTSUPP;
				if (first) {
					result->path = path;
					result->output = output;
					first = false;
				} else if (path->index != result->path->index ||
					   path->queue != result->path->queue) {
					return -EOPNOTSUPP;
				}
				constant &= rtl_fe_itu_equal(&output, &result->output);
				pass &= rtl_fe_itu_equal(&input, &output);
				if (bridge && downstream) {
					constant &= output.priority == result->output.priority;
					pass &= output.priority == input.priority;
				}
			}
		}
	}

	if (first || (!constant && !pass))
		return -EOPNOTSUPP;
	result->passthrough = pass;
	return 0;
}

static int rtl_fe_itu_upstream(struct rtl_fe_rule *data,
			       struct rtl_evto_packet *logical, bool exact)
{
	bool replace = data->action.vlan_set;
	struct rtl_fe_itu_result result;
	int ret;

	ret = rtl_fe_itu_invariant(data->input->fe->itu_paths, logical,
				   !exact && !replace,
				   data->kind == RTL_FE_RULE_BRIDGE, false, 0, &result);
	if (ret)
		return ret;
	ret = rtl_fe_itu_output(data, &result.output, replace || !result.passthrough);
	if (ret)
		return ret;
	data->action.destination = FE_DEEPQ_DEST + result.path->tcont;
	data->action.deep_queue = 1;
	data->action.cos = result.path->queue;
	data->action.gem_set = 1;
	data->action.gem_index = result.path->index;
	return 0;
}

static unsigned int rtl_fe_itu_types(const struct rtl_itu_paths *paths,
				     const struct rtl_itu_path *path, u16 *types)
{
	unsigned int count = 3, stage, field, i;

	types[0] = ETH_P_8021Q;
	types[1] = ETH_P_8021AD;
	types[2] = 0x9100;
	for (stage = 0; stage < RTL_EVTO_STAGES; stage++) {
		unsigned int id = path->vlan_profile[stage];

		if (id == RTL_EVTO_NONE)
			continue;
		for (field = 0; field < 2; field++) {
			u16 type = field ? paths->profile[id].output_tpid :
					   paths->profile[id].input_tpid;

			for (i = 0; i < count && types[i] != type; i++)
				;
			if (i == count)
				types[count++] = type;
		}
	}
	return count;
}

struct rtl_fe_itu_search {
	const struct rtl_itu_paths *paths;
	const struct rtl_evto_packet *logical;
	const struct rtl_itu_path *selected;
	struct rtl_evto_packet physical;
};

static int rtl_fe_itu_try_wire(struct rtl_fe_itu_search *search,
			       const struct rtl_itu_path *path,
			       struct rtl_evto_packet *wire)
{
	const struct rtl_itu_path *matched;
	struct rtl_evto_packet roundtrip;

	matched = rtl_itu_path_receive_vlan(search->paths, path->index, wire,
					    &roundtrip);
	if (!matched || !rtl_fe_itu_shape(&roundtrip, search->logical))
		return 0;
	if (wire->count > 2 ||
	    (search->selected && (search->selected->index != matched->index ||
				  !rtl_fe_itu_shape(&search->physical, wire))))
		return -EOPNOTSUPP;
	search->physical = *wire;
	search->selected = matched;
	return 0;
}

static int rtl_fe_itu_try_types(struct rtl_fe_itu_search *search,
				const struct rtl_itu_path *path,
				const struct rtl_evto_packet *candidate,
				const u16 *types, unsigned int type_count)
{
	unsigned int a, b;
	int ret;

	for (a = 0; a < (candidate->count ? type_count : 1U); a++) {
		for (b = 0; b < (candidate->count == 2 ? type_count : 1U); b++) {
			struct rtl_evto_packet wire = *candidate;

			if (wire.count)
				wire.tag[0].tpid = types[a];
			if (wire.count == 2)
				wire.tag[1].tpid = types[b];
			ret = rtl_fe_itu_try_wire(search, path, &wire);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int rtl_fe_itu_try_path(struct rtl_fe_itu_search *search,
			       const struct rtl_itu_path *path)
{
	const struct rtl_evto_packet *logical = search->logical;
	unsigned int variants = logical->count ? 1U << (logical->count * 4) : 8;
	u16 types[3 + RTL_EVTO_STAGES * 2];
	unsigned int type_count, v, image;
	int ret;

	type_count = rtl_fe_itu_types(search->paths, path, types);
	for (v = 0; v < variants; v++) {
		for (image = 0; image < 2; image++) {
			struct rtl_evto_packet candidate = *logical;

			rtl_fe_itu_variant(&candidate, v);
			if (image &&
			    (!rtl_itu_path_vlan(search->paths, path, &candidate,
						logical->service_entry ? 2 : 0, 2, 0) ||
			     !rtl_itu_path_vlan(search->paths, path, &candidate, 2,
						RTL_EVTO_STAGES, 0)))
				continue;
			ret = rtl_fe_itu_try_types(search, path, &candidate, types,
						   type_count);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int rtl_fe_itu_downstream(struct rtl_fe_rule *data,
				 const struct rtl_evto_packet *logical)
{
	struct rtl_itu_paths *paths = data->input->fe->itu_paths;
	struct rtl_fe_itu_search search = {
		.paths = paths,
		.logical = logical,
	};
	const struct rtl_evto_packet *physical = &search.physical;
	struct rtl_fe_itu_result result;
	unsigned int n, i;
	int ret, type;

	if (rtl_fe_itu_dependencies(paths) & FE_ITU_DEP_DSCP)
		return -EOPNOTSUPP;

	for (n = 0; n < paths->count; n++) {
		const struct rtl_itu_path *path = &paths->path[n];

		if (path->multicast || !path->downstream_pbits)
			continue;
		ret = rtl_fe_itu_try_path(&search, path);
		if (ret)
			return ret;
	}

	if (!search.selected || physical->count > 2)
		return -EOPNOTSUPP;

	ret = rtl_fe_itu_invariant(paths, physical, true,
				   data->kind == RTL_FE_RULE_BRIDGE, true,
				   search.selected->index, &result);
	if (ret || !rtl_fe_itu_shape(&result.output, logical))
		return -EOPNOTSUPP;

	if (!data->action.vlan_set) {
		ret = rtl_fe_itu_output(data, &result.output, !result.passthrough);
		if (ret)
			return ret;
	}

	data->vlan_count = physical->count;
	for (i = 0; i < physical->count; i++) {
		type = rtl_fe_vlan_tpid(htons(physical->tag[i].tpid));
		if (type < 0)
			return type;
		data->vlan[i] = (struct rtl_fe_vlan){
			.vid = physical->tag[i].tci & FE_ITU_TCI_VID,
			.tpid = type,
		};
	}
	data->key.llid = search.selected->index;
	return 0;
}

static unsigned int rtl_fe_itu_criteria(const struct rtl_fe_rule *data,
					unsigned int a, unsigned int b)
{
	if (data->kind == RTL_FE_RULE_BRIDGE || data->key.protocol != IPPROTO_UDP)
		return 0;
	if (data->kind != RTL_FE_RULE_IPV6 && (a == 67 || a == 68) &&
	    (b == 67 || b == 68))
		return 1;
	if (data->kind == RTL_FE_RULE_IPV6 && (a == 546 || a == 547) &&
	    (b == 546 || b == 547))
		return 2;
	return 0;
}

static int rtl_fe_itu_mapped_rule(struct rtl_fe_rule *data)
{
	struct rtl_fe *fe = data->input->fe;
	struct rtl_evto_packet packet;
	unsigned int protocol, a, b;

	if (data->input->netdev == fe->itu_netdev) {
		if (data->kind == RTL_FE_RULE_BRIDGE)
			protocol = data->ethernet_protocol;
		else if (data->pppoe_input)
			protocol = ETH_P_PPP_SES;
		else
			protocol = data->kind == RTL_FE_RULE_IPV6 ? ETH_P_IPV6 : ETH_P_IP;

		rtl_fe_itu_packet(&packet, data->vlan, data->vlan_count, protocol);
		packet.service_entry = data->kind != RTL_FE_RULE_BRIDGE && packet.count;
		packet.criteria = rtl_fe_itu_criteria(data, data->key.source_port,
						      data->key.destination_port);

		if (!fe->itu_paths->profile_count) {
			const struct rtl_itu_path *path;

			path = rtl_itu_path_downstream(fe->itu_paths,
						       data->vlan_count ? data->vlan[0].vid :
						       RTL_ITU_VLAN_ANY, 255);
			if (!path)
				return -EOPNOTSUPP;
			data->key.llid = path->index;
		} else if (rtl_fe_itu_downstream(data, &packet)) {
			return -EOPNOTSUPP;
		}
	}

	if (data->output->netdev == fe->itu_netdev) {
		if (data->egress_pppoe)
			protocol = ETH_P_PPP_SES;
		else if (data->kind == RTL_FE_RULE_BRIDGE)
			protocol = data->ethernet_protocol;
		else
			protocol = data->kind == RTL_FE_RULE_IPV6 ? ETH_P_IPV6 : ETH_P_IP;

		rtl_fe_itu_packet(&packet, data->action.vlan, data->action.vlan_count,
				  protocol);
		packet.service_entry = data->kind != RTL_FE_RULE_BRIDGE && packet.count;
		a = data->key.source_port;
		b = data->key.destination_port;
		if (data->action.translate) {
			if (data->action.translate_destination)
				b = data->action.port;
			else
				a = data->action.port;
		}
		packet.criteria = rtl_fe_itu_criteria(data, a, b);
		if (rtl_fe_itu_upstream(data, &packet, data->vlan_exact))
			return -EOPNOTSUPP;
	}
	return 0;
}

int rtl_fe_itu_rule(struct rtl_fe_rule *data)
{
	struct rtl_fe *fe = data->input->fe;
	const struct rtl_itu_path *path;
	unsigned int i;

	if (fe->itu_paths) {
		bool mapped = fe->itu_paths->profile_count != 0;

		for (i = 0; !mapped && i < fe->itu_paths->count; i++)
			mapped = fe->itu_paths->path[i].unmarked_mode != 0;
		if (mapped)
			return rtl_fe_itu_mapped_rule(data);
	}

	if (data->input->netdev == fe->itu_netdev) {
		path = rtl_itu_path_downstream(fe->itu_paths,
					       data->vlan_count ? data->vlan[0].vid :
					       RTL_ITU_VLAN_ANY, 255);
		if (!path)
			return -EOPNOTSUPP;
		data->key.llid = path->index;
	}

	if (data->output->netdev == fe->itu_netdev) {
		path = rtl_itu_path_flow(fe->itu_paths, data->action.vlan_count ?
					 data->action.vlan[0].vid : RTL_ITU_VLAN_ANY);
		if (!path)
			return -EOPNOTSUPP;
		data->action.destination = FE_DEEPQ_DEST + path->tcont;
		data->action.deep_queue = 1;
		data->action.cos = path->queue;
		data->action.gem_set = 1;
		data->action.gem_index = path->index;
	}
	return 0;
}

int rtl_fe_itu_learn(struct rtl_fe_rule *data, unsigned int gem, u64 generation)
{
	struct rtl_fe *fe = data->input->fe;
	struct rtl_evto_packet packet;
	struct rtl_fe_itu_result inverse;
	unsigned int i;
	int ret;

	if (data->input->netdev == fe->itu_netdev) {
		if (!gem || generation != fe->itu_paths->generation)
			return -ENOENT;
		rtl_fe_itu_packet(&packet, data->vlan, data->vlan_count,
				  data->ethernet_protocol);
		for (i = 0; i < packet.count; i++)
			packet.tag[i].tci = data->ingress_tci[i];
		if (packet.count)
			packet.priority = packet.tag[0].tci >> FE_ITU_TCI_PCP_SHIFT;
		ret = rtl_fe_itu_invariant(fe->itu_paths, &packet, false, true, true,
					   gem, &inverse);
		if (ret)
			return ret;
		data->key.llid = gem;
	}

	if (data->output->netdev == fe->itu_netdev) {
		rtl_fe_itu_packet(&packet, data->action.vlan, data->action.vlan_count,
				  data->ethernet_protocol);
		ret = rtl_fe_itu_upstream(data, &packet, true);
		if (ret)
			return ret;
	}
	return 0;
}
