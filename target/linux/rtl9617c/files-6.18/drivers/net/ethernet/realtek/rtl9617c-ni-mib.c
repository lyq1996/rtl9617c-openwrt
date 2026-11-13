/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/workqueue.h>

#include "rtl9617c-mib.h"
#include "rtl9617c-ni-priv.h"
#include "rtl9617c-ni.h"

static unsigned int rtl_mib_read(void *context, unsigned int reg)
{
	struct rtl9617c_ni *priv = context;

	return rtl_ni_read(priv->regs, reg);
}

static void rtl_mib_write(void *context, unsigned int reg, unsigned int value)
{
	struct rtl9617c_ni *priv = context;

	rtl_ni_write(priv->regs, reg, value);
}

static int rtl_mib_indirect(void *context, unsigned int reg, unsigned int command)
{
	struct rtl9617c_ni *priv = context;

	return rtl9617c_ni_indirect(priv->regs, reg, command, RTL_NI_INDIRECT_BUSY);
}

int rtl9617c_ni_pm_read(struct device *dev, unsigned int port, unsigned long long counters[37])
{
	struct rtl9617c_ni *priv = dev_get_drvdata(dev);
	const struct rtl_dq_io io = {priv, rtl_mib_read, rtl_mib_write, rtl_mib_indirect};
	unsigned long long raw[RTL_NI_MIB_COUNTERS];
	int ret;

	if (port >= ARRAY_SIZE(priv->mib_total))
		return -EINVAL;
	mutex_lock(&priv->mib_lock);
	ret = rtl_ni_mib_read(&io, port, raw);
	if (!ret)
		rtl_ni_mib_pm(priv->mib_total[port], raw, counters);
	mutex_unlock(&priv->mib_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rtl9617c_ni_pm_read);

void rtl9617c_ni_mib_work(struct work_struct *work)
{
	struct rtl9617c_ni *priv = container_of(to_delayed_work(work), struct rtl9617c_ni, mib_work);
	unsigned long long counters[37];
	unsigned int port;

	for (port = 0; port < ARRAY_SIZE(priv->ports); port++) {
		int ret;

		if (!(READ_ONCE(priv->registered_ports) & BIT(port)) || port >= 5)
			continue;
		ret = rtl9617c_ni_pm_read(priv->dev, port, counters);
		if (ret)
			dev_err_ratelimited(priv->dev, "NI port%u MIB read failed: %d\n", port, ret);
	}
	schedule_delayed_work(&priv->mib_work, HZ);
}
