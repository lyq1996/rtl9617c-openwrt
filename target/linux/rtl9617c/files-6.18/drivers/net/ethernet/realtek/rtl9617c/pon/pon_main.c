// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Native Taurus PON platform binding: probe/remove, the sysfs attribute table
 * and the control_lock wrappers the submodules call into.
 * Register definitions: GPL Cortina-Access 2015 aal_pon.c/aal_mpcp.c,
 * Realtek Taurus register header, and the stock DTB NI resources.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <linux/unaligned.h>
#include "pon/pon.h"
#include "pon/pon_priv.h"
#include "pon/pon_debug.h"
#include "rtl9617c_bosa.h"

/* Every dev_attr_* and bin_attr_* below is defined by the file that owns the
 * corresponding protocol block; the group only references them. */
extern struct device_attribute dev_attr_state, dev_attr_led_state;
extern struct device_attribute dev_attr_bosa, dev_attr_transceiver;
extern struct device_attribute dev_attr_serdes, dev_attr_serdes_registers;
extern struct device_attribute dev_attr_optical, dev_attr_mac, dev_attr_activation;
extern struct device_attribute dev_attr_fec, dev_attr_onu_mac;
extern struct device_attribute dev_attr_factory_onu_mac, dev_attr_llid_count;
extern struct device_attribute dev_attr_mpcp, dev_attr_llid_table;
extern struct device_attribute dev_attr_pdc, dev_attr_puc, dev_attr_traffic;
extern struct device_attribute dev_attr_fe, dev_attr_data_path;
extern struct device_attribute dev_attr_active_serial_number;
extern struct device_attribute dev_attr_factory_serial_number;
extern struct device_attribute dev_attr_omci_msk;
extern struct bin_attribute bin_attr_ploam, bin_attr_ploam_default;
extern struct bin_attribute bin_attr_gpon_identity, bin_attr_xgpon_identity;
extern struct bin_attribute bin_attr_itu_paths, bin_attr_omci_broadcast;
extern struct bin_attribute bin_attr_epon_key, bin_attr_epon_tx_control;
extern struct bin_attribute bin_attr_epon_holdover, bin_attr_epon_sla;
extern struct bin_attribute bin_attr_pm_counters;

int rtl_pon_serdes_configure(struct rtl9617c_pon *pon, enum rtl_pon_mode mode)
{
	int ret;

	mutex_lock(&pon->control_lock);
	ret = rtl_pon_serdes_configure_locked(pon, mode);
	mutex_unlock(&pon->control_lock);
	return ret;
}

int rtl_pon_epon_prepare(struct rtl9617c_pon *pon)
{
	int ret;

	mutex_lock(&pon->control_lock);
	ret = rtl_pon_epon_prepare_locked(pon);
	mutex_unlock(&pon->control_lock);
	return ret;
}

int rtl_pon_epon_traffic(struct rtl9617c_pon *pon, bool enable)
{
	int ret;

	mutex_lock(&pon->control_lock);
	ret = rtl_pon_mode_is_epon(pon->mode) ?
		rtl_pon_epon_traffic_locked(pon, enable) : -EOPNOTSUPP;
	mutex_unlock(&pon->control_lock);
	return ret;
}

int rtl_pon_mpcp_stop(struct rtl9617c_pon *pon)
{
	int ret = 0;

	mutex_lock(&pon->control_lock);
	if (rtl_pon_mode_is_epon(pon->mode)) {
		int stopped;

		stopped = rtl_pon_mpcp_stop_locked(pon);
		if (!ret)
			ret = stopped;
	} else
		ret = -EOPNOTSUPP;
	mutex_unlock(&pon->control_lock);
	pon_event(pon, PON_EV_MPCP_STOP, ret, pon->llid_count, pon->registered);
	return ret;
}

int rtl_pon_mpcp_configure(struct rtl9617c_pon *pon)
{
	int ret;

	mutex_lock(&pon->control_lock);
	ret = rtl_pon_mpcp_configure_locked(pon);
	mutex_unlock(&pon->control_lock);
	pon_event(pon, PON_EV_MPCP_CONFIG, ret, pon->llid_count, pon->registered);
	return ret;
}

int rtl_pon_pdc_configure(struct rtl9617c_pon *pon)
{
	int ret;

	mutex_lock(&pon->control_lock);
	ret = pon->gpon_drain.phase || pon->xgpon_recovery_phase ? -EBUSY :
		rtl_pon_pdc_configure_locked(pon);
	mutex_unlock(&pon->control_lock);
	pon_event(pon, PON_EV_PDC_CONFIG, ret, pon->pdc_error, pon->pdc_configured);
	return ret;
}

int rtl_pon_puc_configure(struct rtl9617c_pon *pon)
{
	int ret;

	mutex_lock(&pon->control_lock);
	ret = pon->gpon_drain.phase || pon->xgpon_recovery_phase ? -EBUSY :
		rtl_pon_puc_configure_locked(pon);
	mutex_unlock(&pon->control_lock);
	pon_event(pon, PON_EV_PUC_CONFIG, ret, pon->puc_error,
		  pon->puc_configured);
	return ret;
}


static ssize_t fe_show(struct device *device, struct device_attribute *attribute, char *buffer)
{
	struct rtl9617c_pon *pon = dev_get_drvdata(device);

	return rtl9617c_ni_fe_state(pon->ni_dev, buffer);
}
DEVICE_ATTR_RO(fe);

static ssize_t active_serial_number_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rtl9617c_pon *pon = dev_get_drvdata(dev);
	const u8 *serial;
	ssize_t length;

	mutex_lock(&pon->lock);
	if (rtl_pon_mode_is_epon(pon->mode)) {
		mutex_unlock(&pon->lock);
		return -EOPNOTSUPP;
	}
	serial = pon->mode == RTL_PON_GPON ? pon->gpon_serial : pon->xgpon_serial;
	length = sysfs_emit(buf, "%*phN\n", 8, serial);
	mutex_unlock(&pon->lock);
	return length;
}
DEVICE_ATTR_RO(active_serial_number);

static ssize_t factory_serial_number_show(struct device *dev,
					  struct device_attribute *attr, char *buf)
{
	struct rtl9617c_pon *pon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", pon->factory_serial_number);
}
DEVICE_ATTR_RO(factory_serial_number);

static const struct bin_attribute *const rtl9617c_pon_bin_attrs[] = {
	&bin_attr_ploam,
	&bin_attr_ploam_default,
	&bin_attr_gpon_identity,
	&bin_attr_xgpon_identity,
	&bin_attr_itu_paths,
	&bin_attr_omci_broadcast,
	&bin_attr_epon_key,
	&bin_attr_epon_tx_control,
	&bin_attr_epon_holdover,
	&bin_attr_epon_sla,
	&bin_attr_pm_counters,
	NULL,
};

static struct attribute *rtl9617c_pon_attrs[] = {
	&dev_attr_fe.attr,
	&dev_attr_data_path.attr,
	&dev_attr_active_serial_number.attr,
	&dev_attr_factory_serial_number.attr,
	&dev_attr_omci_msk.attr,
	&dev_attr_state.attr,
	&dev_attr_led_state.attr,
	&dev_attr_bosa.attr,
	&dev_attr_transceiver.attr,
	&dev_attr_serdes.attr,
	&dev_attr_serdes_registers.attr,
	&dev_attr_optical.attr,
	&dev_attr_mac.attr,
	&dev_attr_activation.attr,
	&dev_attr_fec.attr,
	&dev_attr_onu_mac.attr,
	&dev_attr_factory_onu_mac.attr,
	&dev_attr_llid_count.attr,
	&dev_attr_mpcp.attr,
	&dev_attr_llid_table.attr,
	&dev_attr_pdc.attr,
	&dev_attr_puc.attr,
	&dev_attr_traffic.attr,
	NULL,
};
static const struct attribute_group rtl9617c_pon_group = {
	.attrs = rtl9617c_pon_attrs,
	.bin_attrs = rtl9617c_pon_bin_attrs,
};
__ATTRIBUTE_GROUPS(rtl9617c_pon);

static int rtl_pon_factory_serial(struct rtl9617c_pon *pon)
{
	struct nvmem_cell *cell;
	size_t length, index;
	u8 *buffer;

	cell = nvmem_cell_get(pon->dev, "pon-serial");
	if (IS_ERR(cell))
		return PTR_ERR(cell);
	buffer = nvmem_cell_read(cell, &length);
	nvmem_cell_put(cell);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	for (index = 0; index < length && index < sizeof(pon->factory_serial_number) - 1; index++) {
		if (buffer[index] < 0x20 || buffer[index] > 0x7e)
			break;
		pon->factory_serial_number[index] = buffer[index];
	}
	pon->factory_serial_number[index] = '\0';
	kfree(buffer);
	return 0;
}

static int rtl_pon_base_mac(struct rtl9617c_pon *pon)
{
	struct nvmem_cell *cell;
	size_t length = 0;
	u8 *buffer;
	int ret = 0;

	cell = nvmem_cell_get(pon->dev, "base-mac");
	if (IS_ERR(cell))
		return PTR_ERR(cell);
	buffer = nvmem_cell_read(cell, &length);
	nvmem_cell_put(cell);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	if (length >= ETH_ALEN && is_valid_ether_addr(buffer))
		ether_addr_copy(pon->onu_mac, buffer);
	else
		ret = -ENODATA;
	kfree(buffer);
	return ret;
}

static int rtl9617c_pon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl9617c_pon *pon;
	int ret;

	pon = devm_kzalloc(dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;
	pon->dev = dev;
	for (unsigned int i = 0; i < ARRAY_SIZE(pon->uni_ports); i++)
		pon->uni_ports[i] = ARRAY_SIZE(pon->uni_ports) - 1 - i;
	if (device_property_count_u8(dev, "realtek,uni-ports") == ARRAY_SIZE(pon->uni_ports))
		device_property_read_u8_array(dev, "realtek,uni-ports", pon->uni_ports,
					      ARRAY_SIZE(pon->uni_ports));
	pon->tx_disable = devm_gpiod_get(dev, "tx-disable", GPIOD_OUT_HIGH);
	if (IS_ERR(pon->tx_disable))
		return dev_err_probe(dev, PTR_ERR(pon->tx_disable), "PON TX-disable GPIO\n");
	{
		struct device_node *node = of_parse_phandle(dev->of_node, "realtek,optical-front-end", 0);
		struct i2c_client *client;

		if (!node)
			return dev_err_probe(dev, -ENODEV, "missing realtek,optical-front-end\n");
		client = of_find_i2c_device_by_node(node);
		of_node_put(node);
		if (!client)
			return -EPROBE_DEFER;
		if (!i2c_get_clientdata(client)) {
			put_device(&client->dev);
			return -EPROBE_DEFER;
		}
		pon->bosa_dev = &client->dev;
		if (!device_link_add(dev, pon->bosa_dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
			put_device(&client->dev);
			return -ENOMEM;
		}
		put_device(&client->dev);
	}
	{
		struct device_node *node = of_parse_phandle(dev->of_node, "realtek,network-engine", 0);
		struct platform_device *engine;

		if (!node)
			return dev_err_probe(dev, -ENODEV, "missing realtek,network-engine\n");
		engine = of_find_device_by_node(node);
		of_node_put(node);
		if (!engine)
			return -EPROBE_DEFER;
		if (!platform_get_drvdata(engine)) {
			put_device(&engine->dev);
			return -EPROBE_DEFER;
		}
		pon->ni_dev = &engine->dev;
		if (!device_link_add(dev, pon->ni_dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
			put_device(&engine->dev);
			return -ENOMEM;
		}
		put_device(&engine->dev);
	}
	ret = of_get_mac_address(dev->of_node, pon->onu_mac);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret || !is_valid_ether_addr(pon->onu_mac))
		ret = rtl_pon_base_mac(pon);
	if (ret)
		return ret;
	ether_addr_copy(pon->factory_mac, pon->onu_mac);
	ret = rtl_pon_factory_serial(pon);
	if (ret)
		dev_warn(dev, "factory serial unavailable: %d\n", ret);
	pon->mode = RTL_PON_EPON_D10G;
	mutex_init(&pon->lock);
	mutex_init(&pon->control_lock);
	rtl_pon_serdes_init_work(pon);
	pon->control = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(pon->control))
		return PTR_ERR(pon->control);
	pon->mac = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pon->mac))
		return PTR_ERR(pon->mac);
	pon->epon_control = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(pon->epon_control))
		return PTR_ERR(pon->epon_control);
	pon->gpon_control = devm_platform_ioremap_resource(pdev, 7);
	if (IS_ERR(pon->gpon_control))
		return PTR_ERR(pon->gpon_control);
	pon->serdes_init = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(pon->serdes_init))
		return PTR_ERR(pon->serdes_init);
	of_property_read_u32(dev->of_node, "realtek,burst-polarity", &pon->burst_polarity);
	if (pon->burst_polarity > 1)
		return -EINVAL;
	pon->optical_control = devm_platform_ioremap_resource(pdev, 4);
	if (IS_ERR(pon->optical_control))
		return PTR_ERR(pon->optical_control);
	pon->optical_status = devm_platform_ioremap_resource(pdev, 5);
	if (IS_ERR(pon->optical_status))
		return PTR_ERR(pon->optical_status);
	{
		struct resource *optical = platform_get_resource(pdev, IORESOURCE_MEM, 4);

		pon->optical_irq = devm_ioremap(dev, optical->start + 8, 8);
		if (!pon->optical_irq)
			return -ENOMEM;
	}
	of_property_read_u32(dev->of_node, "realtek,los-invert", &pon->los_invert);
	if (pon->los_invert > 1)
		return -EINVAL;
	pon->top_irq = devm_platform_ioremap_resource(pdev, 6);
	if (IS_ERR(pon->top_irq))
		return PTR_ERR(pon->top_irq);
	{
		int irq = platform_get_irq(pdev, 0);
		int ret;

		if (irq < 0)
			return irq;
		ret = rtl_pon_mpcp_init(pon, irq);
		if (ret)
			return ret;
	}
	platform_set_drvdata(pdev, pon);
	dev_info(dev, "native PON MAC binding: global control=%08x\n", readl(pon->control));
	ret = rtl_pon_oam_init(pon);
	if (ret)
		goto err_mpcp;
	ret = rtl_pon_omci_init(pon);
	if (ret) {
		rtl_pon_oam_remove(pon);
		goto err_mpcp;
	}
	ret = rtl_pon_dying_gasp_init(pon, pdev);
	if (ret) {
		rtl_pon_oam_remove(pon);
		rtl_pon_omci_remove(pon);
		goto err_mpcp;
	}
	pon_debug_init(pon);
	return 0;
err_mpcp:
	writel(0, pon->top_irq + 4);
	writel(0, pon->optical_irq + 4);
	synchronize_irq(pon->mpcp_irq);
	return ret;
}

static void rtl9617c_pon_shutdown(struct platform_device *pdev)
{
	struct rtl9617c_pon *pon = platform_get_drvdata(pdev);
	int ret;

	mutex_lock(&pon->control_lock);
	ret = rtl_pon_stop_locked(pon);
	if (ret)
		dev_err(pon->dev, "PON forwarding cleanup failed during shutdown: %d\n", ret);
	writel(0, pon->top_irq + 4);
	writel(0, pon->optical_irq + 4);
	synchronize_irq(pon->mpcp_irq);
	mutex_unlock(&pon->control_lock);
	rtl_pon_serdes_stop_work(pon);
}

static void rtl9617c_pon_remove(struct platform_device *pdev)
{
	struct rtl9617c_pon *pon = platform_get_drvdata(pdev);

	rtl9617c_pon_shutdown(pdev);
	pon_debug_remove(pon);
	rtl_pon_oam_remove(pon);
	rtl_pon_omci_remove(pon);
}

static const struct of_device_id rtl9617c_pon_of_match[] = {
	{ .compatible = "realtek,rtl9617c-pon" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9617c_pon_of_match);

static struct platform_driver rtl9617c_pon_driver = {
	.probe = rtl9617c_pon_probe,
	.remove = rtl9617c_pon_remove,
	.shutdown = rtl9617c_pon_shutdown,
	.driver = {
		.name = "rtl9617c-pon",
		.of_match_table = rtl9617c_pon_of_match,
		.dev_groups = rtl9617c_pon_groups,
	},
};
module_platform_driver(rtl9617c_pon_driver);
MODULE_DESCRIPTION("Native RTL9617C PON MAC platform binding");
MODULE_LICENSE("GPL");
