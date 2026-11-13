/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTL9617C_FE_HW_H
#define RTL9617C_FE_HW_H

struct rtl_fe;
struct rtl_fe_cam_protocol;

void rtl_fe_bits_set(unsigned int *words, unsigned int bit,
	unsigned int width, unsigned int value);

void rtl_fe_action_set(unsigned int words[RTL_FE_ACTION_WORDS],
	enum rtl_fe_action_field field, unsigned int value);

void rtl_fe_ipv4_action(unsigned int words[RTL_FE_ACTION_WORDS],
	const struct rtl_fe_ipv4_action *action);

void rtl_fe_key_set(unsigned int words[RTL_FE_KEY_WORDS],
	enum rtl_fe_key_field field, unsigned int value);

void rtl_fe_vlan_key(unsigned int words[RTL_FE_KEY_WORDS],
	const struct rtl_fe_vlan vlan[2],unsigned int count);

void rtl_fe_ipv4_key(unsigned int words[RTL_FE_KEY_WORDS],
	const struct rtl_fe_ipv4_key *key);

void rtl_fe_ipv6_key(unsigned int words[RTL_FE_KEY_WORDS],
	const struct rtl_fe_ipv6_key *key);

unsigned int rtl_fe_reverse(unsigned int value, unsigned int bits);

void rtl_fe_key_crc(const unsigned int words[RTL_FE_KEY_WORDS],
	unsigned int *crc32, unsigned short *crc16);

void rtl_fe_ipv4_mask(unsigned int words[RTL_FE_MASK_WORDS]);

void rtl_fe_ipv6_mask(unsigned int words[RTL_FE_MASK_WORDS]);

void rtl_fe_bridge_mask(unsigned int words[RTL_FE_MASK_WORDS]);

void rtl_fe_bridge_key(unsigned int words[RTL_FE_KEY_WORDS],
	const unsigned char source[6],const unsigned char destination[6],
	unsigned int protocol,unsigned int ingress,unsigned int llid,unsigned int mask);

void rtl_fe_header_ipv4(unsigned int words[RTL_FE_HEADER_WORDS],
	const struct rtl_fe_ipv4_key *key);

void rtl_fe_header_ipv6(unsigned int words[RTL_FE_HEADER_WORDS],
	const struct rtl_fe_ipv6_key *key);

int rtl_fe_mask_write(struct rtl_fe *fe, unsigned int index,
	const unsigned int words[RTL_FE_MASK_WORDS]);

int rtl_fe_mask_read(struct rtl_fe *fe, unsigned int index,
	unsigned int words[RTL_FE_MASK_WORDS]);

int rtl_fe_crc_hardware(struct rtl_fe *fe,
	const unsigned int header[RTL_FE_HEADER_WORDS], unsigned int mask,
	unsigned int *crc32, unsigned short *crc16);

void rtl_fe_dma_tables(struct rtl_fe *fe,
	unsigned int address, unsigned int address_high);

void rtl_fe_cache_start(struct rtl_fe *fe);

unsigned int rtl_fe_hash_bucket(unsigned short crc16);

void rtl_fe_cpu_miss(unsigned int words[4]);

int rtl_fe_main_age(struct rtl_fe *fe,unsigned int index,unsigned int age);

int rtl_fe_main_age_read(struct rtl_fe *fe,
	unsigned int index,unsigned int *age);

int rtl_fe_cache_command(struct rtl_fe *fe,unsigned int index,
	unsigned short crc16,unsigned int command);

int rtl_fe_cache_read(struct rtl_fe *fe,unsigned int index,int fib,unsigned int words[7]);

int rtl_fe_cam_protocol_get(struct rtl_fe *fe,
	struct rtl_fe_cam_protocol *binding,unsigned int bank,unsigned int protocol);

int rtl_fe_cam_protocol_put(struct rtl_fe *fe,struct rtl_fe_cam_protocol *binding);

void rtl_fe_classify_hash(unsigned int words[7],unsigned int profile,unsigned int source);

void rtl_fe_ingress_lpb(unsigned int words[3],unsigned int source,unsigned int destination);

int rtl_fe_classifier_read(struct rtl_fe *fe,unsigned int index,unsigned int words[7]);

int rtl_fe_classifier_write(struct rtl_fe *fe,unsigned int index,const unsigned int words[7]);

int rtl_fe_source_mac_read(struct rtl_fe *fe,unsigned int index,unsigned int words[5]);

int rtl_fe_source_mac_write(struct rtl_fe *fe,unsigned int index,const unsigned int words[5]);

int rtl_fe_tx_counter_read(struct rtl_fe *fe,
	unsigned int index,unsigned int words[7]);

void rtl_fe_fdb_key(unsigned int words[4],const unsigned char mac[6]);

int rtl_fe_fdb_command(struct rtl_fe *fe,unsigned int command,unsigned int words[4],unsigned int *result);

#endif
