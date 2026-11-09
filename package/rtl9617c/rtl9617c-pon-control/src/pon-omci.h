/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL9617C_OMCI_H
#define RTL9617C_OMCI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct omci_identity {
	uint8_t serial[8], equipment[20], hardware[14], software[14];
	uint8_t loid[24], password[12], operator_id[4];
	uint8_t psk[16], has_psk;
	uint8_t omcc_version, disable_enhanced_security;
};
struct omci_agent;
uint8_t rtl9617c_omci_led_auth(const struct omci_agent *agent);
struct omci_agent *rtl9617c_omci_new(const struct omci_identity *identity);
void rtl9617c_omci_free(struct omci_agent *agent);
int rtl9617c_omci_dispatch(struct omci_agent *agent, const uint8_t *data, size_t length, uint8_t *output, int backend_result);
int rtl9617c_omci_dispatch_apply(struct omci_agent *agent, const uint8_t *data, size_t length,
	uint8_t *output, int *backend_result,
	int (*apply)(void *context, const uint8_t *paths, size_t length), void *context);
int rtl9617c_omci_broadcast(const struct omci_agent *agent, const uint8_t *data, size_t length, uint8_t row[18]);
int rtl9617c_omci_paths(struct omci_agent *agent, uint8_t *output);
int rtl9617c_omci_session_key(struct omci_agent *agent, uint8_t *output);
void rtl9617c_omci_key_complete(struct omci_agent *agent, bool installed);
int rtl9617c_omci_notification(struct omci_agent *agent, uint8_t *output);
void rtl9617c_omci_epoch(struct omci_agent *agent);
#endif
