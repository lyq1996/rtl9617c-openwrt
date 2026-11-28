// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include "rtl9617c_bosa.h"

enum rtl_bosa_model {
	RTL_BOSA_A2H = 100,
	RTL_BOSA_GN25L95 = 201,
	RTL_BOSA_GN25L98,
	RTL_BOSA_GN28L95,
	RTL_BOSA_GN28L96,
	RTL_BOSA_GN28L97,
};

enum rtl_bosa_opcode {
	RTL_BOSA_WRITE,
	RTL_BOSA_CAL_BYTE,
	RTL_BOSA_CAL_RANGE,
	RTL_BOSA_MODIFY,
	RTL_BOSA_PULSE,
	RTL_BOSA_READ,
	RTL_BOSA_WAIT,
	RTL_BOSA_KEY_REGISTER,
};

enum rtl_bosa_calibration {
	RTL_BOSA_CAL_NONE,
	RTL_BOSA_CAL_NVMEM,
	RTL_BOSA_CAL_EEPROM,
};

struct rtl_bosa_op {
	u8 opcode;
	u8 reg;
	u8 value;
	u8 mask;
	u16 offset;
	u16 first;
	u16 last;
};

struct rtl_bosa_profile {
	u8 model;
	const char *name;
	const struct rtl_bosa_op *ops;
	unsigned int ops_count;
	u8 key_register;
};

struct rtl_bosa_io {
	void *context;
	int (*write)(void *context, unsigned int reg, unsigned char value);
	int (*read)(void *context, unsigned int reg, unsigned char *value);
	int (*wait)(void *context, unsigned int reg, unsigned char mask, unsigned char value);
};

static const struct rtl_bosa_op rtl_bosa_gn28l96_ops[] = {
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x81 },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x89, .value = 0x00 },
	{ .opcode = RTL_BOSA_WAIT, .reg = 0x89, .mask = 0x02, .value = 0x02 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x100, .first = 0x00, .last = 0x3f },
	{ .opcode = RTL_BOSA_CAL_BYTE, .reg = 0x6f, .offset = 0x16f },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x170, .first = 0x70, .last = 0x77 },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x00 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x1f8, .first = 0xf8, .last = 0xfd },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x82 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x300, .first = 0x80, .last = 0xff },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x83 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x380, .first = 0x80, .last = 0xff },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x80 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x200, .first = 0x80, .last = 0xff },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x81 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x280, .first = 0x80, .last = 0x87 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x289, .first = 0x89, .last = 0x89 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x28b, .first = 0x8b, .last = 0x8c },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x28e, .first = 0x8e, .last = 0xff },
	{ .opcode = RTL_BOSA_CAL_BYTE, .reg = 0x6e, .offset = 0x16e },
	{ .opcode = RTL_BOSA_READ, .reg = 0x6e },
	{ .opcode = RTL_BOSA_CAL_BYTE, .reg = 0x88, .offset = 0x288 },
};

static const struct rtl_bosa_op rtl_bosa_gn25l_ops[] = {
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x04 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x280, .first = 0x80, .last = 0xff },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x05 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x300, .first = 0x80, .last = 0xff },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x06 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x380, .first = 0x80, .last = 0xff },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x02 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x200, .first = 0x80, .last = 0xff },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x252, .first = 0xd2, .last = 0xe7 },
	{ .opcode = RTL_BOSA_CAL_RANGE, .offset = 0x100, .first = 0x00, .last = 0x4f },
	{ .opcode = RTL_BOSA_CAL_BYTE, .reg = 0x6f, .offset = 0x16f },
	{ .opcode = RTL_BOSA_CAL_BYTE, .reg = 0x72, .offset = 0x172 },
	{ .opcode = RTL_BOSA_CAL_BYTE, .reg = 0x78, .offset = 0x178 },
	{ .opcode = RTL_BOSA_CAL_BYTE, .reg = 0x79, .offset = 0x179 },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x01 },
	{ .opcode = RTL_BOSA_CAL_BYTE, .reg = 0xf8, .offset = 0x1f8 },
	{ .opcode = RTL_BOSA_MODIFY, .reg = 0xf9, .mask = 0xc0, .value = 0xc0 },
	{ .opcode = RTL_BOSA_CAL_BYTE, .reg = 0xfc, .offset = 0x1fc },
	{ .opcode = RTL_BOSA_MODIFY, .reg = 0xfd, .mask = 0xc0, .value = 0xc0 },
	{ .opcode = RTL_BOSA_PULSE, .reg = 0x6e, .mask = 0x40 },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7f, .value = 0x02 },
	{ .opcode = RTL_BOSA_KEY_REGISTER, .value = 0x6a },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7b, .value = 0xff },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7c, .value = 0xff },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7d, .value = 0xff },
	{ .opcode = RTL_BOSA_WRITE, .reg = 0x7e, .value = 0xff },
	{ .opcode = RTL_BOSA_MODIFY, .reg = 0xbb, .mask = 0x01, .value = 0x00 },
	{ .opcode = RTL_BOSA_READ, .reg = 0xbb },
	{ .opcode = RTL_BOSA_READ, .reg = 0xb1 },
	{ .opcode = RTL_BOSA_READ, .reg = 0xb7 },
};

static const struct rtl_bosa_profile rtl_bosa_profiles[] = {
	{ .model = RTL_BOSA_A2H, .name = "A2H" },
	{ .model = RTL_BOSA_GN25L95, .name = "GN25L95", .ops = rtl_bosa_gn25l_ops,
	  .ops_count = ARRAY_SIZE(rtl_bosa_gn25l_ops), .key_register = 0xa0 },
	{ .model = RTL_BOSA_GN25L98, .name = "GN25L98", .ops = rtl_bosa_gn25l_ops,
	  .ops_count = ARRAY_SIZE(rtl_bosa_gn25l_ops), .key_register = 0x98 },
	{ .model = RTL_BOSA_GN28L95, .name = "GN28L95", .ops = rtl_bosa_gn25l_ops,
	  .ops_count = ARRAY_SIZE(rtl_bosa_gn25l_ops), .key_register = 0x88 },
	{ .model = RTL_BOSA_GN28L96, .name = "GN28L96", .ops = rtl_bosa_gn28l96_ops,
	  .ops_count = ARRAY_SIZE(rtl_bosa_gn28l96_ops) },
	{ .model = RTL_BOSA_GN28L97, .name = "GN28L97", .ops = rtl_bosa_gn28l96_ops,
	  .ops_count = ARRAY_SIZE(rtl_bosa_gn28l96_ops) },
};

struct rtl_bosa {
	struct i2c_client *client;
	struct mutex lock;
	const struct rtl_bosa_profile *profile;
	enum rtl_bosa_model model;
	enum rtl_bosa_calibration calibration;
	bool configured;
	bool eeprom;
	unsigned int writes;
	int error;
	u8 identity[3];
};

static const struct rtl_bosa_profile *rtl_bosa_profile(unsigned int model)
{
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(rtl_bosa_profiles); index++)
		if (rtl_bosa_profiles[index].model == model)
			return &rtl_bosa_profiles[index];
	return NULL;
}

static int rtl_bosa_write(void *context, unsigned int reg, unsigned char value)
{
	struct rtl_bosa *bosa = context;
	int ret;

	ret = i2c_smbus_write_byte_data(bosa->client, reg, value);
	if (!ret)
		bosa->writes++;
	return ret;
}

static int rtl_bosa_read(void *context, unsigned int reg, unsigned char *value)
{
	struct rtl_bosa *bosa = context;
	int ret;

	ret = i2c_smbus_read_byte_data(bosa->client, reg);
	if (ret < 0)
		return ret;
	*value = ret;
	return 0;
}

static int rtl_bosa_wait(void *context, unsigned int reg, unsigned char mask, unsigned char value)
{
	struct rtl_bosa *bosa = context;
	int status, ret;

	ret = read_poll_timeout(i2c_smbus_read_byte_data, status,
		status < 0 || (status & mask) == value, 1000, 1000000, false, bosa->client, reg);
	return status < 0 ? status : ret;
}

static int rtl_bosa_execute(const struct rtl_bosa_io *io,
			    const struct rtl_bosa_profile *profile,
			    const u8 *calibration)
{
	unsigned int index;
	int ret;

	for (index = 0; index < profile->ops_count; index++) {
		const struct rtl_bosa_op *op = &profile->ops[index];
		unsigned int reg;
		u8 value;

		switch (op->opcode) {
		case RTL_BOSA_WRITE:
			ret = io->write(io->context, op->reg, op->value);
			break;
		case RTL_BOSA_CAL_BYTE:
			ret = io->write(io->context, op->reg, calibration[op->offset]);
			break;
		case RTL_BOSA_CAL_RANGE:
			ret = 0;
			for (reg = op->first; reg <= op->last; reg++) {
				ret = io->write(io->context, reg,
						calibration[op->offset + reg - op->first]);
				if (ret)
					break;
			}
			break;
		case RTL_BOSA_MODIFY:
			ret = io->read(io->context, op->reg, &value);
			if (!ret)
				ret = io->write(io->context, op->reg,
						(value & ~op->mask) | op->value);
			break;
		case RTL_BOSA_PULSE:
			ret = io->read(io->context, op->reg, &value);
			if (!ret)
				ret = io->write(io->context, op->reg, value | op->mask);
			if (!ret)
				ret = io->write(io->context, op->reg, value & ~op->mask);
			break;
		case RTL_BOSA_READ:
			ret = io->read(io->context, op->reg, &value);
			break;
		case RTL_BOSA_WAIT:
			ret = io->wait(io->context, op->reg, op->mask, op->value);
			break;
		case RTL_BOSA_KEY_REGISTER:
			ret = io->write(io->context, profile->key_register, op->value);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		if (ret)
			return ret;
	}
	return 0;
}

static int rtl_bosa_calibration_required(const struct rtl_bosa_profile *profile,
					 size_t *required)
{
	unsigned int index;

	*required = 0;
	for (index = 0; index < profile->ops_count; index++) {
		const struct rtl_bosa_op *op = &profile->ops[index];
		size_t end;

		switch (op->opcode) {
		case RTL_BOSA_CAL_BYTE:
			end = (size_t)op->offset + 1;
			break;
		case RTL_BOSA_CAL_RANGE:
			end = (size_t)op->offset + op->last - op->first + 1;
			break;
		default:
			continue;
		}
		if (end > *required)
			*required = end;
	}
	return *required ? 0 : -EOPNOTSUPP;
}

static bool rtl_bosa_calibration_blank(const u8 *data, size_t length)
{
	size_t index;

	for (index = 0; index < length; index++)
		if (data[index] != 0xff)
			return false;
	return true;
}

static int rtl_bosa_calibrate(const struct rtl_bosa_io *io,
			      const struct rtl_bosa_profile *profile,
			      const u8 *data, size_t length)
{
	size_t required;
	int ret;

	if (!profile->ops)
		return -EOPNOTSUPP;
	ret = rtl_bosa_calibration_required(profile, &required);
	if (ret)
		return ret;
	if (length < required)
		return -EINVAL;
	if (rtl_bosa_calibration_blank(data, required))
		return -ENODATA;
	return rtl_bosa_execute(io, profile, data);
}

static int rtl_bosa_apply_calibration(struct rtl_bosa *bosa, const struct rtl_bosa_io *io)
{
	struct nvmem_cell *cell;
	u8 *data;
	size_t length;
	int ret;

	cell = nvmem_cell_get(&bosa->client->dev, "calibration");
	if (IS_ERR(cell)) {
		ret = PTR_ERR(cell);
		return ret == -ENOENT ? -ENODATA : ret;
	}
	data = nvmem_cell_read(cell, &length);
	nvmem_cell_put(cell);
	if (IS_ERR(data))
		return PTR_ERR(data);
	ret = rtl_bosa_calibrate(io, bosa->profile, data, length);
	kfree(data);
	return ret;
}

static int rtl_bosa_identify(struct rtl_bosa *bosa)
{
	static const u8 registers[] = { 0x80, 0x85, 0x86 };
	unsigned int index;
	u8 id, digital;
	int ret;

	ret = rtl_bosa_write(bosa, 0x7f, 0xff);
	if (ret)
		return ret;
	for (index = 0; index < ARRAY_SIZE(registers); index++) {
		ret = rtl_bosa_read(bosa, registers[index], &bosa->identity[index]);
		if (ret)
			return ret;
	}
	if (!memcmp(bosa->identity, "G96", 3))
		bosa->model = RTL_BOSA_GN28L96;
	else if (!memcmp(bosa->identity, "G97", 3))
		bosa->model = RTL_BOSA_GN28L97;
	else if (!memcmp(bosa->identity, "\xa1\0\0", 3))
		bosa->model = RTL_BOSA_GN28L95;
	else {
		u8 t_hi = 0xff, t_lo = 0xff;

		ret = rtl_bosa_write(bosa, 0x7f, 2);
		if (ret)
			return ret;
		ret = rtl_bosa_read(bosa, 0xd1, &id);
		if (ret)
			return ret;
		if ((id & 0xf0) == 0xa0)
			bosa->model = RTL_BOSA_GN25L95;
		else if ((id & 0xf0) == 0xc0)
			bosa->model = RTL_BOSA_GN25L98;
		else {
			ret = rtl_bosa_write(bosa, 0x7f, 0);
			if (ret)
				return ret;
			ret = rtl_bosa_read(bosa, 0x60, &t_hi);
			if (!ret)
				ret = rtl_bosa_read(bosa, 0x61, &t_lo);
			if (ret)
				return ret;
			if ((t_hi == 0xff && t_lo == 0xff) ||
			    (t_hi == 0x00 && t_lo == 0x00))
				return -ENODEV;
			bosa->identity[0] = 'A';
			bosa->identity[1] = '2';
			bosa->identity[2] = 'H';
			bosa->model = RTL_BOSA_A2H;
			bosa->eeprom = true;
			return 0;
		}
	}
	ret = rtl_bosa_write(bosa, 0x7f, bosa->model >= RTL_BOSA_GN28L96 ? 0x81 : 2);
	if (ret)
		return ret;
	ret = rtl_bosa_read(bosa, bosa->model >= RTL_BOSA_GN28L96 ? 0x89 :
		bosa->model == RTL_BOSA_GN28L95 ? 0xc7 : 0xc0, &digital);
	if (ret)
		return ret;
	bosa->eeprom = bosa->model >= RTL_BOSA_GN28L96 ? !(digital & 1) : digital & 1;
	return 0;
}

static const char *rtl_bosa_calibration_name(enum rtl_bosa_calibration calibration)
{
	switch (calibration) {
	case RTL_BOSA_CAL_NVMEM:
		return "nvmem";
	case RTL_BOSA_CAL_EEPROM:
		return "module-eeprom";
	default:
		return "none";
	}
}

static int rtl_bosa_configure(struct rtl_bosa *bosa)
{
	struct rtl_bosa_io io = {
		.context = bosa,
		.write = rtl_bosa_write,
		.read = rtl_bosa_read,
		.wait = rtl_bosa_wait,
	};
	u8 page, tx;
	int ret, restore;

	mutex_lock(&bosa->lock);
	bosa->configured = false;
	bosa->profile = NULL;
	bosa->eeprom = false;
	bosa->calibration = RTL_BOSA_CAL_NONE;
	bosa->writes = 0;
	ret = rtl_bosa_read(bosa, 0x7f, &page);
	if (ret)
		goto unlock;
	ret = rtl_bosa_identify(bosa);
	if (ret)
		goto restore;
	bosa->profile = rtl_bosa_profile(bosa->model);
	if (!bosa->profile) {
		ret = -ENODEV;
		goto restore;
	}
	ret = rtl_bosa_apply_calibration(bosa, &io);
	switch (ret) {
	case 0:
		bosa->calibration = RTL_BOSA_CAL_NVMEM;
		break;
	case -ENODATA:
	case -EOPNOTSUPP:
		if (!bosa->eeprom) {
			dev_warn(&bosa->client->dev,
				 "BOSA has no EEPROM and no calibration\n");
			goto restore;
		}
		if (ret == -EOPNOTSUPP)
			dev_warn(&bosa->client->dev,
				 "BOSA %s has no calibration sequence, using module EEPROM\n",
				 bosa->profile->name);
		bosa->calibration = RTL_BOSA_CAL_EEPROM;
		ret = 0;
		break;
	default:
		dev_warn(&bosa->client->dev, "BOSA calibration failed: %d\n", ret);
		goto restore;
	}
	ret = rtl_bosa_read(bosa, 0x6e, &tx);
	if (!ret && tx & BIT(6))
		ret = -EIO;
	if (!ret) {
		bosa->configured = true;
		dev_info(&bosa->client->dev,
			 "BOSA model=%s EEPROM=%u calibration=%s, %u writes\n",
			 bosa->profile->name, bosa->eeprom,
			 rtl_bosa_calibration_name(bosa->calibration), bosa->writes);
	}
restore:
	restore = rtl_bosa_write(bosa, 0x7f, page);
	if (!ret)
		ret = restore;
unlock:
	if (ret)
		bosa->configured = false;
	bosa->error = ret;
	mutex_unlock(&bosa->lock);
	return ret;
}

int rtl9617c_bosa_prepare(struct device *device)
{
	return rtl_bosa_configure(dev_get_drvdata(device));
}
EXPORT_SYMBOL_GPL(rtl9617c_bosa_prepare);

ssize_t rtl9617c_bosa_state(struct device *dev, char *buf)
{
	struct rtl_bosa *bosa = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&bosa->lock);
	ret = sysfs_emit(buf, "configured=%u model=%s eeprom=%u calibration=%s writes=%u error=%d identity=%02x/%02x/%02x\n",
		bosa->configured, bosa->profile ? bosa->profile->name : "unknown",
		bosa->eeprom, rtl_bosa_calibration_name(bosa->calibration),
		bosa->writes, bosa->error, bosa->identity[0], bosa->identity[1],
		bosa->identity[2]);
	mutex_unlock(&bosa->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rtl9617c_bosa_state);

static ssize_t configure_show(struct device *dev, struct device_attribute *attribute, char *buf)
{
	return rtl9617c_bosa_state(dev, buf);
}

DEVICE_ATTR_RO(configure);

ssize_t rtl9617c_bosa_diagnostics(struct device *dev, char *buf)
{
	struct rtl_bosa *bosa = dev_get_drvdata(dev);
	u8 data[14];
	unsigned int index;
	int ret;

	mutex_lock(&bosa->lock);
	for (index = 0; index < ARRAY_SIZE(data); index++) {
		ret = rtl_bosa_read(bosa, 0x60 + index, &data[index]);
		if (ret)
			goto unlock;
	}
	ret = sysfs_emit(buf, "temperature_raw=%d voltage_raw=%u bias_raw=%u tx_power_raw=%u rx_power_raw=%u control=%02x/%02x/%02x/%02x\n",
		(s16)get_unaligned_be16(data), get_unaligned_be16(data + 2),
		get_unaligned_be16(data + 4), get_unaligned_be16(data + 6),
		get_unaligned_be16(data + 8), data[10], data[11], data[12], data[13]);
unlock:
	mutex_unlock(&bosa->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rtl9617c_bosa_diagnostics);

static ssize_t diagnostics_show(struct device *dev, struct device_attribute *attribute, char *buf)
{
	return rtl9617c_bosa_diagnostics(dev, buf);
}
DEVICE_ATTR_RO(diagnostics);

static struct attribute *rtl_bosa_attrs[] = {
	&dev_attr_configure.attr,
	&dev_attr_diagnostics.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rtl_bosa);

static int rtl_bosa_probe(struct i2c_client *client)
{
	struct rtl_bosa *bosa;

	bosa = devm_kzalloc(&client->dev, sizeof(*bosa), GFP_KERNEL);
	if (!bosa)
		return -ENOMEM;
	bosa->client = client;
	mutex_init(&bosa->lock);
	i2c_set_clientdata(client, bosa);
	return 0;
}

static const struct of_device_id rtl_bosa_match[] = {
	{ .compatible = "realtek,rtl9617c-bosa" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl_bosa_match);

static struct i2c_driver rtl_bosa_driver = {
	.probe = rtl_bosa_probe,
	.driver = {
		.name = "rtl9617c-bosa",
		.of_match_table = rtl_bosa_match,
		.dev_groups = rtl_bosa_groups,
	},
};
module_i2c_driver(rtl_bosa_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RTL9617C native Semtech BOSA calibration client");
