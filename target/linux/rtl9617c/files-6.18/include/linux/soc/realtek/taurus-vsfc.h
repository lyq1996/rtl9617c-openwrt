/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SOC_REALTEK_TAURUS_VSFC_H
#define _LINUX_SOC_REALTEK_TAURUS_VSFC_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_REALTEK_TAURUS_VSFC)
bool taurus_vsfc_quiesce(void);
void taurus_vsfc_resume(bool was_enabled);
#else
static inline bool taurus_vsfc_quiesce(void)
{
	return false;
}

static inline void taurus_vsfc_resume(bool was_enabled)
{
}
#endif

#endif /* _LINUX_SOC_REALTEK_TAURUS_VSFC_H */
