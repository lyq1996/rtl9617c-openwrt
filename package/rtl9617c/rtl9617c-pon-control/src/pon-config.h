/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PON_CONFIG_H
#define PON_CONFIG_H
#include <errno.h>
#include <uci.h>

struct pon_config {
	struct uci_context *context;
	struct uci_section *user;
};

static int pon_config_load(struct pon_config *config, const char *directory)
{
	struct uci_package *package;

	config->context = uci_alloc_context();
	if (!config->context)
		return -ENOMEM;
	uci_set_confdir(config->context, directory);
	if (uci_load(config->context, "pon", &package) != UCI_OK)
		return -ENOENT;
	config->user = uci_lookup_section(config->context, package, "pon");
	return config->user ? 0 : -ENOENT;
}

static const char *pon_config_option(struct pon_config *config, const char *key)
{
	const char *value = uci_lookup_option_string(config->context, config->user, key);

	return value && value[0] ? value : NULL;
}
#endif
