/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL9617C_ITU_H
#define RTL9617C_ITU_H

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* G.988 9.3.13 legacy EVTO rows. Tags are ordered from the Ethernet header. */

#define RTL_EVTO_ROWS 32
#define RTL_EVTO_STAGES 5
#define RTL_EVTO_TAGS (2 + 2 * RTL_EVTO_STAGES)
#define RTL_EVTO_NONE 65535U
#define RTL_EVTO_REF 65536U
#define RTL_EVTO_DSCP 131072U

struct rtl_evto_tag { unsigned short tpid, tci; };
struct rtl_evto_packet {
	unsigned int count, protocol, dscp, criteria, priority;
	unsigned int service_entry; /* A local VLAN service starts after UNI policy. */
	struct rtl_evto_tag tag[RTL_EVTO_TAGS];
};
struct rtl_evto_profile {
	unsigned short entity, input_tpid, output_tpid;
	unsigned char downstream, count, dscp[24], row[RTL_EVTO_ROWS][16];
};
struct rtl_evto_symbol { unsigned int field[4]; };

static inline unsigned int rtl_evto_word(const unsigned char *p)
{
	return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 | p[2] << 8 | p[3];
}
static inline unsigned int rtl_evto_field(const struct rtl_evto_tag *tag, unsigned int f)
{
	return f == 0 ? tag->tci >> 13 : f == 1 ? tag->tci & 4095 :
		f == 2 ? tag->tpid : (tag->tci >> 12) & 1;
}
static inline void rtl_evto_set_field(struct rtl_evto_tag *tag, unsigned int f, unsigned int value)
{
	if (f == 0) tag->tci = (tag->tci & 8191) | value << 13;
	else if (f == 1) tag->tci = (tag->tci & 61440) | value;
	else if (f == 2) tag->tpid = value;
	else tag->tci = (tag->tci & 61439) | value << 12;
}
static inline unsigned int rtl_itu_dscp_priority(const unsigned char map[24], unsigned int dscp)
{
	unsigned int bit = dscp * 3, n = bit / 8;
	unsigned int value = (unsigned int)map[n] << 8;
	if (n + 1 < 24) value |= map[n + 1];
	return (value >> (13 - bit % 8)) & 7;
}
static inline unsigned int rtl_evto_dscp(const struct rtl_evto_profile *p, unsigned int dscp)
{
	return rtl_itu_dscp_priority(p->dscp, dscp);
}
static inline unsigned int rtl_evto_input_count(const unsigned char row[16])
{
	return row[4] >> 4 == 15 ? 0 : row[0] >> 4 == 15 ? 1 : 2;
}
static inline int rtl_evto_default(const unsigned char row[16])
{
	unsigned int outer = rtl_evto_word(row), inner = rtl_evto_word(row + 4);
	return row[0] >> 4 == 14 || row[4] >> 4 == 14 ||
		(outer == 0xf8000000U && inner == 0xf8000000U);
}
static inline int rtl_evto_transparent(const unsigned char row[16])
{
	return !(row[8] >> 6) && (row[9] & 15) == 15 && (row[13] & 15) == 15;
}
static inline int rtl_evto_protocol(const struct rtl_evto_packet *packet, unsigned int word)
{
	static const unsigned short types[] = {0, 0x0800, 0x8863, 0x0806, 0x86dd, 0x888e};
	unsigned int type = word & 15, criteria = (word >> 4) & 255;
	if (type > 5 || criteria > 2) return 0;
	if (type && packet->protocol != types[type] && !(type == 2 && packet->protocol == 0x8864)) return 0;
	return !criteria || packet->criteria == criteria;
}

/* Each filtered input field is either constrained or free. */
static inline void rtl_evto_filter(const struct rtl_evto_profile *p, unsigned int word,
	unsigned int value[4], unsigned int known[4])
{
	unsigned int pri = word >> 28, vid = (word >> 15) & 8191, tpid = (word >> 12) & 7;
	value[0] = pri < 8 ? pri : 0; known[0] = pri < 8;
	value[1] = vid < 4095 ? vid : 0; known[1] = vid < 4095;
	value[2] = tpid == 4 || tpid == 0 ? 0x8100 : p->input_tpid; known[2] = tpid != 0;
	value[3] = tpid == 7; known[3] = tpid >= 6;
}

/* Expressions distinguish constants, copies of input tags, and the DSCP map. */
static inline int rtl_evto_compile(const struct rtl_evto_profile *p, const unsigned char row[16],
	unsigned int count, struct rtl_evto_symbol out[RTL_EVTO_TAGS], unsigned int *total)
{
	unsigned int input = rtl_evto_input_count(row), remove = row[8] >> 6, n = 0;
	if (remove == 3) return 0;
	if (count < input || (input < 2 && count != input) || remove > count) return -1;
	for (unsigned int i = 0; i < 2; i++) {
		unsigned int word = rtl_evto_word(row + 8 + i * 4);
		unsigned int pri = (word >> 16) & 15, vid = (word >> 3) & 8191, tpid = word & 7;
		unsigned int inner = input ? input - 1 : 0;
		if (pri == 15) continue;
		if (pri > 10 || vid == 4095 || vid > 4097 || tpid == 5 ||
		    ((!input) && (pri == 8 || vid == 4096 || tpid == 0 || tpid == 2)) ||
		    ((input < 2) && (pri == 9 || vid == 4097 || tpid == 1 || tpid == 3))) return -1;
		out[n].field[0] = pri < 8 ? pri : pri == 10 ? RTL_EVTO_DSCP : RTL_EVTO_REF | (pri == 8 ? inner : 0);
		out[n].field[1] = vid < 4095 ? vid : RTL_EVTO_REF | (vid == 4096 ? inner : 0);
		out[n].field[2] = tpid < 2 ? RTL_EVTO_REF | (tpid ? 0 : inner) : tpid == 4 ? 0x8100 : p->output_tpid;
		out[n].field[3] = tpid < 4 ? RTL_EVTO_REF | ((tpid & 1) ? 0 : inner) : tpid == 7;
		n++;
	}
	if (n + count - remove > RTL_EVTO_TAGS) return -1;
	for (unsigned int i = remove; i < count; i++, n++)
		for (unsigned int f = 0; f < 4; f++) out[n].field[f] = RTL_EVTO_REF | i;
	*total = n;
	return 1;
}

static inline int rtl_evto_valid(const struct rtl_evto_profile *p)
{
	struct rtl_evto_symbol expression[RTL_EVTO_TAGS];
	unsigned int total;
	if (p->downstream > 8 || p->count > RTL_EVTO_ROWS) return 0;
	for (unsigned int n = 0; n < p->count; n++) {
		const unsigned char *row = p->row[n];
		unsigned int count = rtl_evto_input_count(row), inner = rtl_evto_word(row + 4);
		if ((row[4] >> 4) == 15 && (row[0] >> 4) != 15) return 0;
		if ((row[9] & 15) != 15 && (row[13] & 15) == 15 && (row[8] >> 6) != 3) return 0;
		if ((inner & 15) > 5 || ((inner >> 4) & 255) > 2) return 0;
		for (unsigned int i = 0; i < count; i++) {
			unsigned int w = rtl_evto_word(row + (count == 1 ? 4 : i * 4));
			unsigned int pri = w >> 28, vid = (w >> 15) & 8191, tpid = (w >> 12) & 7;
			if ((pri > 8 && pri != 14) || vid == 4095 || vid > 4096 || (tpid && tpid < 4)) return 0;
		}
		if (rtl_evto_compile(p, row, count, expression, &total) < 0) return 0;
	}
	return 1;
}

static inline int rtl_evto_up_row(const struct rtl_evto_profile *p, const unsigned char row[16],
	struct rtl_evto_packet *packet)
{
	struct rtl_evto_symbol expression[RTL_EVTO_TAGS];
	struct rtl_evto_packet result = *packet;
	unsigned int input = rtl_evto_input_count(row), total;
	if (packet->count < input || (input < 2 && packet->count != input) ||
	    !rtl_evto_protocol(packet, rtl_evto_word(row + 4))) return 0;
	for (unsigned int i = 0; i < input; i++) {
		unsigned int value[4], known[4];
		rtl_evto_filter(p, rtl_evto_word(row + (input == 1 ? 4 : i * 4)), value, known);
		for (unsigned int f = 0; f < 4; f++)
			if (known[f] && value[f] != rtl_evto_field(&packet->tag[i], f)) return 0;
	}
	int ret = rtl_evto_compile(p, row, packet->count, expression, &total);
	if (ret <= 0) return -1;
	result.count = total;
	for (unsigned int i = 0; i < total; i++) {
		result.tag[i] = (struct rtl_evto_tag){0};
		for (unsigned int f = 0; f < 4; f++) {
			unsigned int value = expression[i].field[f];
			value = value == RTL_EVTO_DSCP ? rtl_evto_dscp(p, packet->dscp) :
				value & RTL_EVTO_REF ? rtl_evto_field(&packet->tag[value & 15], f) : value;
			rtl_evto_set_field(&result.tag[i], f, value);
		}
	}
	if (total) result.priority = result.tag[0].tci >> 13;
	*packet = result;
	return 1;
}

static inline int rtl_evto_down_row(const struct rtl_evto_profile *p, const unsigned char row[16],
	struct rtl_evto_packet *packet)
{
	struct rtl_evto_symbol expression[RTL_EVTO_TAGS];
	struct rtl_evto_packet result = *packet;
	unsigned int remove = row[8] >> 6, added = ((row[9] & 15) != 15) + ((row[13] & 15) != 15);
	unsigned int input = rtl_evto_input_count(row), total, count, values[RTL_EVTO_TAGS][4], known[RTL_EVTO_TAGS][4];
	unsigned int vid = p->downstream != 4 && p->downstream != 7;
	unsigned int pri = p->downstream != 3 && p->downstream != 6;
	if (remove == 3 || (rtl_evto_default(row) && rtl_evto_transparent(row)) ||
	    packet->count < added || !rtl_evto_protocol(packet, rtl_evto_word(row + 4))) return 0;
	count = packet->count - added + remove;
	if (count > RTL_EVTO_TAGS || rtl_evto_compile(p, row, count, expression, &total) <= 0) return 0;
	for (unsigned int i = 0; i < count; i++) {
		for (unsigned int f = 0; f < 4; f++) { values[i][f] = f == 2 ? 0x8100 : 0; known[i][f] = 0; }
		if (i < input) rtl_evto_filter(p, rtl_evto_word(row + (input == 1 ? 4 : i * 4)), values[i], known[i]);
	}
	for (unsigned int i = 0; i < total; i++)
		for (unsigned int f = 0; f < 4; f++) {
			unsigned int expr = expression[i].field[f], value = expr;
			if ((f == 0 && !pri) || (f == 1 && !vid) || (f >= 2 && p->downstream != 0)) continue;
			if (expr == RTL_EVTO_DSCP) {
				unsigned int mask = 0;
				for (unsigned int d = 0; d < 64; d++) mask |= 1U << rtl_evto_dscp(p, d);
				if (!(mask & (1U << rtl_evto_field(&packet->tag[i], f)))) return 0;
				continue;
			}
			if (expr & RTL_EVTO_REF) {
				if (!known[expr & 15][f]) {
					values[expr & 15][f] = rtl_evto_field(&packet->tag[i], f);
					known[expr & 15][f] = 2;
					continue;
				}
				value = values[expr & 15][f];
			}
			if (value != rtl_evto_field(&packet->tag[i], f)) return 0;
		}
	result.count = count;
	for (unsigned int i = 0; i < total; i++)
		for (unsigned int f = 0; f < 4; f++) {
			unsigned int expr = expression[i].field[f];
			if ((expr & RTL_EVTO_REF) && !known[expr & 15][f]) values[expr & 15][f] = rtl_evto_field(&packet->tag[i], f);
		}
	for (unsigned int i = 0; i < count; i++) {
		result.tag[i] = (struct rtl_evto_tag){0};
		for (unsigned int f = 0; f < 4; f++) {
			int source = (int)i + (int)packet->count - (int)count;
			unsigned int value = values[i][f];
			if (((f == 0 && !pri) || (f == 1 && !vid)) && source >= 0)
				value = rtl_evto_field(&packet->tag[source], f);
			rtl_evto_set_field(&result.tag[i], f, value);
		}
	}
	if (count) result.priority = result.tag[0].tci >> 13;
	*packet = result;
	return 1;
}

static inline int rtl_evto_apply(const struct rtl_evto_profile *p, struct rtl_evto_packet *packet, int downstream)
{
	if (downstream && p->downstream == 1) return 1;
	if (downstream && p->downstream == 8) return 0;
	for (unsigned int defaults = 0; defaults < (downstream ? 1U : 2U); defaults++)
		for (unsigned int n = 0; n < p->count; n++) {
			if (!downstream && !!rtl_evto_default(p->row[n]) != defaults) continue;
			int ret = downstream ? rtl_evto_down_row(p, p->row[n], packet) : rtl_evto_up_row(p, p->row[n], packet);
			if (ret) return ret > 0;
		}
	/* Mode0 uses inverse VID/PCP/TPID and forwards unmatched frames. */
	return downstream && p->downstream < 5;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */

#define RTL_ITU_PATH_MAX 248
#define RTL_ITU_PATH_WIRE_BYTES 50
#define RTL_ITU_SCHEDULE_WIRE_BYTES 12
#define RTL_ITU_CONFIG_WIRE_MAX (12 + RTL_ITU_PATH_MAX * RTL_ITU_PATH_WIRE_BYTES + \
	RTL_ITU_PATH_MAX * RTL_EVTO_STAGES * (32 + RTL_EVTO_ROWS * 16) + \
	31 * RTL_ITU_SCHEDULE_WIRE_BYTES)
#define RTL_ITU_VLAN_TAGGED 65533U
#define RTL_ITU_VLAN_UNTAGGED 65534U
#define RTL_ITU_VLAN_ANY 65535U

struct rtl_itu_path {
	unsigned short alloc_id, gem_id, vlan, tcont;
	unsigned char pbits, multicast, queue, encrypted, index, downstream_pbits;
	unsigned short vlan_profile[RTL_EVTO_STAGES];
	unsigned char vlan_exclude, unmarked_mode, default_priority, mapper_dscp[24];
};

struct rtl_itu_paths {
	unsigned int count, profile_count;
	unsigned long long generation;
	struct rtl_itu_path path[RTL_ITU_PATH_MAX];
	unsigned char weights[32][8];
	struct rtl_evto_profile profile[];
};

static inline unsigned long rtl_itu_paths_bytes(const struct rtl_itu_paths *paths)
{
	return sizeof(*paths) + paths->profile_count * sizeof(paths->profile[0]);
}

static inline int rtl_itu_path_vlan(const struct rtl_itu_paths *paths, const struct rtl_itu_path *path,
	struct rtl_evto_packet *packet, unsigned int begin, unsigned int end, int downstream)
{
	for (unsigned int n = begin; n < end; n++) {
		unsigned int i = downstream ? end - 1 - (n - begin) : n;
		unsigned int id = path->vlan_profile[i];
		if (id != RTL_EVTO_NONE && !rtl_evto_apply(&paths->profile[id], packet, downstream)) return 0;
	}
	return 1;
}

static inline int rtl_itu_path_matches(const struct rtl_itu_path *path,
		unsigned int vlan)
{
	if (path->vlan == RTL_ITU_VLAN_UNTAGGED)
		return vlan == RTL_ITU_VLAN_ANY;
	if (path->vlan == RTL_ITU_VLAN_TAGGED && vlan == RTL_ITU_VLAN_ANY)
		return 0;
	if (path->vlan < RTL_ITU_VLAN_TAGGED && path->vlan != vlan)
		return 0;
	return 1;
}

static inline const struct rtl_itu_path *rtl_itu_path_transmit_vlan(const struct rtl_itu_paths *paths,
	const struct rtl_evto_packet *input, struct rtl_evto_packet *output)
{
	const struct rtl_itu_path *selected = 0;
	unsigned int best = 0;
	int conflict = 0;
	for (unsigned int i = 0; i < paths->count; i++) {
		const struct rtl_itu_path *path = &paths->path[i];
		struct rtl_evto_packet packet = *input;
		unsigned int rank = path->vlan < RTL_ITU_VLAN_TAGGED ? 4 : path->vlan == RTL_ITU_VLAN_ANY ? 0 : 2;
		if (path->multicast || !rtl_itu_path_vlan(paths, path, &packet, input->service_entry ? 2 : 0, 2, 0) ||
		    (path->vlan_exclude & (1U << packet.priority)) ||
		    !rtl_itu_path_matches(path, packet.count ? packet.tag[0].tci & 4095 : RTL_ITU_VLAN_ANY)) continue;
		if (!rtl_itu_path_vlan(paths, path, &packet, 2, 3, 0)) continue;
		if (!packet.count && path->unmarked_mode)
			packet.priority = path->unmarked_mode == 1 ? rtl_itu_dscp_priority(path->mapper_dscp, packet.dscp) : path->default_priority;
		if (!(path->pbits & (1U << packet.priority))) continue;
		if (rank < best) continue;
		if (rank > best) { selected = 0; best = rank; conflict = 0; }
		/* Mapper/IWTP rules can reject a candidate or change its wire tags. */
		if (!rtl_itu_path_vlan(paths, path, &packet, 3, RTL_EVTO_STAGES, 0)) continue;
		if (!selected) { selected = path; *output = packet; }
		else if (selected->index != path->index || packet.count != output->count ||
			packet.priority != output->priority || memcmp(packet.tag, output->tag,
				packet.count * sizeof(packet.tag[0]))) conflict = 1;
	}
	return conflict ? 0 : selected;
}

static inline const struct rtl_itu_path *rtl_itu_path_receive_vlan(const struct rtl_itu_paths *paths,
	unsigned int index, const struct rtl_evto_packet *input, struct rtl_evto_packet *output)
{
	const struct rtl_itu_path *selected = 0;
	for (unsigned int i = 0; i < paths->count; i++) {
		const struct rtl_itu_path *path = &paths->path[i];
		struct rtl_evto_packet packet = *input;
		if ((index != RTL_EVTO_NONE && path->index != index) ||
		    !rtl_itu_path_vlan(paths, path, &packet, 2, RTL_EVTO_STAGES, 1) ||
		    !(path->downstream_pbits & (1U << packet.priority)) ||
		    !rtl_itu_path_matches(path, packet.count ? packet.tag[0].tci & 4095 : RTL_ITU_VLAN_ANY) ||
		    !rtl_itu_path_vlan(paths, path, &packet, input->service_entry ? 2 : 0, 2, 1)) continue;
		if (selected && (selected->index != path->index || packet.count != output->count)) return 0;
		if (selected) for (unsigned int n = 0; n < packet.count; n++)
			if (packet.tag[n].tci != output->tag[n].tci || packet.tag[n].tpid != output->tag[n].tpid) return 0;
		selected = path; *output = packet;
	}
	return selected;
}

static inline const struct rtl_itu_path *rtl_itu_path_select(const struct rtl_itu_paths *paths,
		unsigned int vlan, unsigned int priority)
{
	const struct rtl_itu_path *selected = 0;
	unsigned int i, best = 0;
	int conflict = 0;

	for (i = 0; i < paths->count; i++) {
		const struct rtl_itu_path *path = &paths->path[i];
		unsigned int rank = (path->vlan < RTL_ITU_VLAN_TAGGED ? 4 :
			path->vlan == RTL_ITU_VLAN_ANY ? 0 : 2);

		if (path->multicast || !(path->pbits & (1U << priority)) ||
		    (path->vlan_exclude & (1U << priority)) || !rtl_itu_path_matches(path, vlan))
			continue;
		if (!selected || rank > best) {
			selected = path;
			best = rank;
			conflict = 0;
		} else if (rank == best && selected->index != path->index) conflict = 1;
	}
	return conflict ? 0 : selected;
}

/* A Linux flowtable rule matches VID but carries no Ethernet PCP match. */
static inline const struct rtl_itu_path *rtl_itu_path_flow(const struct rtl_itu_paths *paths,
		unsigned int vlan)
{
	const struct rtl_itu_path *first = rtl_itu_path_select(paths, vlan, 0), *path;
	unsigned int priority;

	if (!first)
		return 0;
	for (priority = 1; priority < 8; priority++) {
		path = rtl_itu_path_select(paths, vlan, priority);
		if (!path || path->index != first->index || path->queue != first->queue)
			return 0;
	}
	return first;
}

/* Without receive GEM metadata, an offloaded ingress must resolve uniquely. */
static inline const struct rtl_itu_path *rtl_itu_path_downstream(const struct rtl_itu_paths *paths,
		unsigned int vlan, unsigned int pbits)
{
	const struct rtl_itu_path *first = 0;
	unsigned int i, covered = 0;

	for (i = 0; i < paths->count; i++) {
		const struct rtl_itu_path *path = &paths->path[i];

		if (path->multicast || !(path->downstream_pbits & pbits) || !rtl_itu_path_matches(path, vlan))
			continue;
		if (first && first->index != path->index)
			return 0;
		first = path;
		covered |= path->downstream_pbits;
	}
	return (covered & pbits) == pbits ? first : 0;
}

#endif
