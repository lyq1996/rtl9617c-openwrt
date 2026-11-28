// SPDX-License-Identifier: GPL-2.0
#include <linux/io.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include <linux/soc/realtek/taurus-leds.h>

#define TAURUS_LED_CONTROL		0x00

#define TAURUS_LED_MAX_HW_BLINK		127
#define TAURUS_LED_MAX_COUNT		16
#define TAURUS_LED_MAX_PORT		8

#define TAURUS_LED_BLINK_RATE1_OFFSET	0
#define TAURUS_LED_BLINK_RATE1_MASK	0xff
#define TAURUS_LED_BLINK_RATE2_OFFSET	8
#define TAURUS_LED_BLINK_RATE2_MASK	0xff
#define TAURUS_LED_CLK_TEST		BIT(16)
#define TAURUS_LED_CLK_POLARITY		BIT(17)
#define TAURUS_LED_CLK_TEST_MODE	BIT(16)
#define TAURUS_LED_CLK_TEST_RX_TEST	BIT(30)
#define TAURUS_LED_CLK_TEST_TX_TEST	BIT(31)

#define TAURUS_LED_EVENT_ON_OFFSET	0
#define TAURUS_LED_EVENT_ON_MASK	0x7
#define TAURUS_LED_EVENT_BLINK_OFFSET	3
#define TAURUS_LED_EVENT_BLINK_MASK	0x7
#define TAURUS_LED_EVENT_OFF_OFFSET	6
#define TAURUS_LED_EVENT_OFF_MASK	0x7
#define TAURUS_LED_OFF_ON_OFFSET	9
#define TAURUS_LED_OFF_ON_MASK		0x3
#define TAURUS_LED_PORT_OFFSET		11
#define TAURUS_LED_PORT_MASK		0x7
#define TAURUS_LED_OFF_VAL		BIT(14)
#define TAURUS_LED_SW_EVENT		BIT(15)
#define TAURUS_LED_BLINK_SEL		BIT(16)

struct taurus_led_cfg {
	struct led_classdev cdev;
	void __iomem *mem;
	spinlock_t *lock;
	unsigned long data;
	int idx;
	bool active_low;

	int off_event;
	int blink_event;
	int on_event;
	int port;
	int blink;
	int enable;
	struct device *dev;
};

struct taurus_led_ctrl {
	void __iomem *mem;
	spinlock_t *lock;

	int test_mode;
	int clk_low;
	u16 blink_rate1;
	u16 blink_rate2;

	struct device *dev;

	struct taurus_led_cfg *led_cfg[TAURUS_LED_MAX_COUNT];
};

static const char * const taurus_trigger_str[] = {
	[TAURUS_LED_TRIGGER_HW_RX] = "rx",
	[TAURUS_LED_TRIGGER_HW_TX] = "tx",
	[TAURUS_LED_TRIGGER_SW] = "sw",
};

static const char * const taurus_rate_str[] = {
	[TAURUS_LED_BLINK_RATE1] = "blink_rate1",
	[TAURUS_LED_BLINK_RATE2] = "blink_rate2",
};

static struct taurus_led_ctrl *glb_led_ctrl;
static int led_control = 2;

void taurus_led_set_all(int control)
{
	unsigned long flags;
	int idx;
	struct taurus_led_cfg *led_cfg;

	if (!glb_led_ctrl)
		return;

	spin_lock_irqsave(glb_led_ctrl->lock, flags);
	if (led_control == control)
		goto out;

	if ((control > 2) || (control < 0))
		goto out;

	led_control = control;
	for (idx = 0; idx < TAURUS_LED_MAX_COUNT; idx++) {
		unsigned long val;

		led_cfg = glb_led_ctrl->led_cfg[idx];
		if (!led_cfg)
			continue;
		val = led_cfg->data &
		      ~(TAURUS_LED_OFF_ON_MASK << TAURUS_LED_OFF_ON_OFFSET) &
		      ~(TAURUS_LED_EVENT_BLINK_MASK <<
			TAURUS_LED_EVENT_BLINK_OFFSET);
		switch (control) {
		case 0:
			iowrite32(val & ~TAURUS_LED_SW_EVENT, led_cfg->mem);
			break;
		case 1:
			iowrite32(val | TAURUS_LED_SW_EVENT, led_cfg->mem);
			break;
		case 2:
			iowrite32(led_cfg->data, led_cfg->mem);
			break;
		}
	}
out:
	spin_unlock_irqrestore(glb_led_ctrl->lock, flags);
}
EXPORT_SYMBOL(taurus_led_set_all);

int taurus_led_get_all(void)
{
	return led_control;
}
EXPORT_SYMBOL(taurus_led_get_all);

static void taurus_led_write(void __iomem *reg, unsigned long data,
			     struct taurus_led_cfg *cfg)
{
	if (cfg == NULL) {
		iowrite32(data, reg);
	} else {
		cfg->data = data;
		if (led_control == 2)
			iowrite32(data, reg);
	}
}

static unsigned long taurus_led_read(void __iomem *reg,
				     struct taurus_led_cfg *cfg)
{
	if (cfg == NULL)
		return ioread32(reg);
	return cfg->data;
}

int taurus_led_sw_on(int idx, int on)
{
	struct taurus_led_cfg *led_cfg;
	unsigned long flags;
	u32 val;

	if (!glb_led_ctrl)
		return -ENODEV;
	if (idx < 0 || idx >= TAURUS_LED_MAX_COUNT)
		return -EINVAL;

	led_cfg = glb_led_ctrl->led_cfg[idx];
	if (!led_cfg)
		return -ENODEV;

	spin_lock_irqsave(led_cfg->lock, flags);
	val = taurus_led_read(led_cfg->mem, led_cfg);
	if (on)
		val |= TAURUS_LED_SW_EVENT;
	else
		val &= ~TAURUS_LED_SW_EVENT;

	taurus_led_write(led_cfg->mem, val, led_cfg);
	spin_unlock_irqrestore(led_cfg->lock, flags);

	return 0;
}
EXPORT_SYMBOL(taurus_led_sw_on);

int taurus_led_enable(int idx, int enable)
{
	struct taurus_led_cfg *led_cfg;
	unsigned long flags;
	u32 val;

	if (!glb_led_ctrl)
		return -ENODEV;
	if (idx < 0 || idx >= TAURUS_LED_MAX_COUNT)
		return -EINVAL;

	led_cfg = glb_led_ctrl->led_cfg[idx];
	if (!led_cfg)
		return -ENODEV;

	spin_lock_irqsave(led_cfg->lock, flags);

	val = taurus_led_read(led_cfg->mem, led_cfg);

	if (enable) {
		led_cfg->enable = 1;
		val &= ~(TAURUS_LED_OFF_ON_MASK << TAURUS_LED_OFF_ON_OFFSET);
	} else {
		led_cfg->enable = 0;
		val &= ~(TAURUS_LED_OFF_ON_MASK << TAURUS_LED_OFF_ON_OFFSET);
		val |= 0x3 << TAURUS_LED_OFF_ON_OFFSET;
	}

	taurus_led_write(led_cfg->mem, val, led_cfg);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return 0;
}
EXPORT_SYMBOL(taurus_led_enable);

int taurus_led_config(int idx, int active_low, int off_event, int blink_event,
		      int on_event, int port, int blink)
{
	struct taurus_led_cfg *led_cfg;
	unsigned long flags;
	u32 val;

	if (!glb_led_ctrl)
		return -ENODEV;
	if (idx < 0 || idx >= TAURUS_LED_MAX_COUNT)
		return -EINVAL;
	if (off_event < 0 || off_event > TAURUS_LED_TRIGGER_NONE)
		return -EINVAL;
	if (blink_event < 0 || blink_event > TAURUS_LED_TRIGGER_NONE)
		return -EINVAL;
	if (on_event < 0 || on_event > TAURUS_LED_TRIGGER_NONE)
		return -EINVAL;
	if (port < 0 || port >= TAURUS_LED_MAX_PORT)
		return -EINVAL;
	if (blink < 0 || blink > TAURUS_LED_BLINK_RATE2)
		return -EINVAL;

	led_cfg = glb_led_ctrl->led_cfg[idx];
	if (!led_cfg)
		return -ENODEV;

	spin_lock_irqsave(led_cfg->lock, flags);

	val = taurus_led_read(led_cfg->mem, led_cfg);

	if (active_low) {
		led_cfg->active_low = true;
		val |= TAURUS_LED_OFF_VAL;
	} else {
		led_cfg->active_low = false;
		val &= ~TAURUS_LED_OFF_VAL;
	}

	led_cfg->off_event = off_event;
	led_cfg->blink_event = blink_event;
	led_cfg->on_event = on_event;
	led_cfg->blink = blink;

	if (blink == TAURUS_LED_BLINK_RATE1)
		val &= ~TAURUS_LED_BLINK_SEL;
	else if (blink == TAURUS_LED_BLINK_RATE2)
		val |= TAURUS_LED_BLINK_SEL;

	val &= ~(TAURUS_LED_EVENT_OFF_MASK << TAURUS_LED_EVENT_OFF_OFFSET);
	if (off_event != TAURUS_LED_TRIGGER_NONE)
		val |= BIT(off_event) << TAURUS_LED_EVENT_OFF_OFFSET;

	val &= ~(TAURUS_LED_EVENT_BLINK_MASK << TAURUS_LED_EVENT_BLINK_OFFSET);
	if (blink_event != TAURUS_LED_TRIGGER_NONE)
		val |= BIT(blink_event) << TAURUS_LED_EVENT_BLINK_OFFSET;

	val &= ~(TAURUS_LED_EVENT_ON_MASK << TAURUS_LED_EVENT_ON_OFFSET);
	if (on_event != TAURUS_LED_TRIGGER_NONE)
		val |= BIT(on_event) << TAURUS_LED_EVENT_ON_OFFSET;

	led_cfg->port = port;
	val &= ~(TAURUS_LED_PORT_MASK << TAURUS_LED_PORT_OFFSET);
	val |= port << TAURUS_LED_PORT_OFFSET;

	taurus_led_write(led_cfg->mem, val, led_cfg);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return 0;
}
EXPORT_SYMBOL(taurus_led_config);

int taurus_led_test_mode(int enable)
{
	struct taurus_led_cfg *led_cfg;
	struct led_classdev *led_cdev;
	unsigned long flags;
	u32 val;
	int i;

	if (!glb_led_ctrl)
		return -ENODEV;

	if (enable) {
		for (i = 0; i < TAURUS_LED_MAX_COUNT; i++) {
			led_cfg = glb_led_ctrl->led_cfg[i];
			if (!led_cfg)
				continue;
			led_cdev = &led_cfg->cdev;
			mutex_lock(&led_cdev->led_access);
			led_sysfs_disable(led_cdev);
			mutex_unlock(&led_cdev->led_access);
		}

		spin_lock_irqsave(glb_led_ctrl->lock, flags);

		glb_led_ctrl->test_mode = 1;

		for (i = 0; i < TAURUS_LED_MAX_COUNT; i++) {
			if (!glb_led_ctrl->led_cfg[i])
				continue;

			led_cfg = glb_led_ctrl->led_cfg[i];
			val = taurus_led_read(led_cfg->mem, led_cfg);
			val &= ~(TAURUS_LED_EVENT_ON_MASK <<
				 TAURUS_LED_EVENT_ON_OFFSET);
			val &= ~(TAURUS_LED_EVENT_BLINK_MASK <<
				 TAURUS_LED_EVENT_BLINK_OFFSET);
			val |= BIT(0) << TAURUS_LED_EVENT_BLINK_OFFSET;
			val |= TAURUS_LED_BLINK_SEL;
			taurus_led_write(led_cfg->mem, val, led_cfg);
		}

		val = taurus_led_read(glb_led_ctrl->mem, NULL);
		val |= TAURUS_LED_CLK_TEST_MODE |
		       TAURUS_LED_CLK_TEST_RX_TEST |
		       TAURUS_LED_CLK_TEST_TX_TEST;
		taurus_led_write(glb_led_ctrl->mem, val, NULL);

		spin_unlock_irqrestore(glb_led_ctrl->lock, flags);
	} else {
		spin_lock_irqsave(glb_led_ctrl->lock, flags);

		glb_led_ctrl->test_mode = 0;

		val = taurus_led_read(glb_led_ctrl->mem, NULL);
		val &= ~(TAURUS_LED_CLK_TEST_MODE |
			 TAURUS_LED_CLK_TEST_RX_TEST |
			 TAURUS_LED_CLK_TEST_TX_TEST);
		taurus_led_write(glb_led_ctrl->mem, val, NULL);

		spin_unlock_irqrestore(glb_led_ctrl->lock, flags);

		for (i = 0; i < TAURUS_LED_MAX_COUNT; i++) {
			if (!glb_led_ctrl->led_cfg[i])
				continue;

			led_cfg = glb_led_ctrl->led_cfg[i];

			taurus_led_config(i, led_cfg->active_low,
					  led_cfg->off_event,
					  led_cfg->blink_event,
					  led_cfg->on_event, led_cfg->port,
					  led_cfg->blink);
			if (led_cfg->on_event == TAURUS_LED_TRIGGER_SW) {
				led_cdev = &led_cfg->cdev;
				mutex_lock(&led_cdev->led_access);
				led_sysfs_enable(led_cdev);
				mutex_unlock(&led_cdev->led_access);
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(taurus_led_test_mode);

static void taurus_led_set(struct led_classdev *led_cdev,
			   enum led_brightness value)
{
	struct taurus_led_cfg *led_cfg =
		container_of(led_cdev, struct taurus_led_cfg, cdev);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(led_cfg->lock, flags);
	val = taurus_led_read(led_cfg->mem, led_cfg);
	if (value == LED_OFF)
		val &= ~TAURUS_LED_SW_EVENT;
	else
		val |= TAURUS_LED_SW_EVENT;
	taurus_led_write(led_cfg->mem, val, led_cfg);
	spin_unlock_irqrestore(led_cfg->lock, flags);
}

static int taurus_led_blink_set(struct led_classdev *led_cdev,
				unsigned long *delay_on, unsigned long *delay_off)
{
	struct taurus_led_cfg *led_cfg =
		container_of(led_cdev, struct taurus_led_cfg, cdev);
	struct taurus_led_ctrl *glb_led;
	unsigned long delay, flags;
	u32 val;

	if (led_cfg->blink_event == TAURUS_LED_TRIGGER_SW)
		return -EINVAL;

	glb_led = dev_get_drvdata(led_cfg->dev);

	if (*delay_on != *delay_off)
		return -EINVAL;

	if (*delay_on == ((glb_led->blink_rate1 + 1) * 16)) {
		delay = 0;
	} else if (*delay_on == ((glb_led->blink_rate2 + 1) * 16)) {
		delay = 1;
	} else {
		return -EINVAL;
	}

	spin_lock_irqsave(led_cfg->lock, flags);
	val = taurus_led_read(led_cfg->mem, led_cfg);
	if (delay)
		val |= TAURUS_LED_BLINK_SEL;
	else
		val &= ~TAURUS_LED_BLINK_SEL;
	taurus_led_write(led_cfg->mem, val, led_cfg);
	spin_unlock_irqrestore(led_cfg->lock, flags);

	return 0;
}

static ssize_t taurus_led_show_hw_event(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct taurus_led_cfg *led_cfg =
		container_of(led_cdev, struct taurus_led_cfg, cdev);
	unsigned long flags;
	ssize_t len = 0;
	int i;

	spin_lock_irqsave(led_cfg->lock, flags);

	for (i = 0; i <= TAURUS_LED_TRIGGER_SW; i++) {
		if (i == led_cfg->blink_event)
			len += sprintf(buf + len, "[%s] ", taurus_trigger_str[i]);
		else
			len += sprintf(buf + len, "%s ", taurus_trigger_str[i]);
	}

	spin_unlock_irqrestore(led_cfg->lock, flags);

	len += sprintf(buf + len, "\n");
	return len;
}

static ssize_t taurus_led_store_hw_event(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct taurus_led_cfg *led_cfg =
		container_of(led_cdev, struct taurus_led_cfg, cdev);
	unsigned long flags;
	u32 val;
	int i;

	for (i = 0; i <= TAURUS_LED_TRIGGER_SW; i++) {
		if (!strncmp(buf, taurus_trigger_str[i],
			     strlen(taurus_trigger_str[i])))
			break;
	}
	if (i > TAURUS_LED_TRIGGER_SW)
		return size;

	spin_lock_irqsave(led_cfg->lock, flags);

	led_cfg->blink_event = i;

	val = taurus_led_read(led_cfg->mem, led_cfg);
	val &= ~(TAURUS_LED_EVENT_ON_MASK << TAURUS_LED_EVENT_ON_OFFSET);
	val &= ~(TAURUS_LED_EVENT_BLINK_MASK << TAURUS_LED_EVENT_BLINK_OFFSET);
	if ((led_cfg->blink_event == TAURUS_LED_TRIGGER_HW_RX) ||
	    (led_cfg->blink_event == TAURUS_LED_TRIGGER_HW_TX)) {
		if (led_cfg->blink_event)
			val |= BIT(led_cfg->blink_event) <<
			       TAURUS_LED_EVENT_BLINK_OFFSET;
		else
			val |= BIT(led_cfg->blink_event) <<
			       TAURUS_LED_EVENT_ON_OFFSET;
	} else if (led_cfg->blink_event == TAURUS_LED_TRIGGER_SW) {
		val |= BIT(led_cfg->blink_event) << TAURUS_LED_EVENT_ON_OFFSET;
	}
	taurus_led_write(led_cfg->mem, val, led_cfg);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	mutex_lock(&led_cfg->cdev.led_access);
	if (led_cfg->blink_event == TAURUS_LED_TRIGGER_SW)
		led_sysfs_enable(&led_cfg->cdev);
	else
		led_sysfs_disable(&led_cfg->cdev);
	mutex_unlock(&led_cfg->cdev.led_access);

	return size;
}
static DEVICE_ATTR(hw_event, 0644, taurus_led_show_hw_event,
		   taurus_led_store_hw_event);

static ssize_t taurus_led_show_hw_port(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct taurus_led_cfg *led_cfg =
		container_of(led_cdev, struct taurus_led_cfg, cdev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_cfg->lock, flags);

	ret += sprintf(buf, "%d\n", led_cfg->port);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return ret;
}

static ssize_t taurus_led_store_hw_port(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct taurus_led_cfg *led_cfg =
		container_of(led_cdev, struct taurus_led_cfg, cdev);
	unsigned long flags, param;
	u32 val;

	if (kstrtoul(buf, 10, &param))
		return size;

	spin_lock_irqsave(led_cfg->lock, flags);

	led_cfg->port = param & TAURUS_LED_PORT_MASK;

	val = taurus_led_read(led_cfg->mem, led_cfg);
	val &= ~(TAURUS_LED_PORT_MASK << TAURUS_LED_PORT_OFFSET);
	val |= led_cfg->port << TAURUS_LED_PORT_OFFSET;
	taurus_led_write(led_cfg->mem, val, led_cfg);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return size;
}
static DEVICE_ATTR(hw_port, 0644, taurus_led_show_hw_port,
		   taurus_led_store_hw_port);

static ssize_t taurus_led_show_hw_blink(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct taurus_led_cfg *led_cfg =
		container_of(led_cdev, struct taurus_led_cfg, cdev);
	unsigned long flags;
	ssize_t len = 0;
	int i;

	spin_lock_irqsave(led_cfg->lock, flags);

	for (i = 0; i <= TAURUS_LED_BLINK_RATE2; i++) {
		if (i == led_cfg->blink)
			len += sprintf(buf + len, "[%s] ", taurus_rate_str[i]);
		else
			len += sprintf(buf + len, "%s ", taurus_rate_str[i]);
	}

	spin_unlock_irqrestore(led_cfg->lock, flags);

	len += sprintf(buf + len, "\n");
	return len;
}

static ssize_t taurus_led_store_hw_blink(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct taurus_led_cfg *led_cfg =
		container_of(led_cdev, struct taurus_led_cfg, cdev);
	unsigned long flags;
	u32 val;
	int i;

	for (i = 0; i <= TAURUS_LED_BLINK_RATE2; i++) {
		if (!strncmp(buf, taurus_rate_str[i], strlen(taurus_rate_str[i])))
			break;
	}
	if (i > TAURUS_LED_BLINK_RATE2)
		return size;

	spin_lock_irqsave(led_cfg->lock, flags);

	led_cfg->blink = i;

	val = taurus_led_read(led_cfg->mem, led_cfg);
	if (led_cfg->blink == TAURUS_LED_BLINK_RATE1)
		val &= ~TAURUS_LED_BLINK_SEL;
	else if (led_cfg->blink == TAURUS_LED_BLINK_RATE2)
		val |= TAURUS_LED_BLINK_SEL;
	taurus_led_write(led_cfg->mem, val, led_cfg);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return size;
}
static DEVICE_ATTR(hw_blink, 0644, taurus_led_show_hw_blink,
		   taurus_led_store_hw_blink);

static ssize_t taurus_led_show_hw_enable(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct taurus_led_cfg *led_cfg =
		container_of(led_cdev, struct taurus_led_cfg, cdev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_cfg->lock, flags);

	ret += sprintf(buf, "%d\n", led_cfg->enable);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return ret;
}

static ssize_t taurus_led_store_hw_enable(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct taurus_led_cfg *led_cfg =
		container_of(led_cdev, struct taurus_led_cfg, cdev);
	unsigned long param;

	if (kstrtoul(buf, 10, &param))
		return size;

	taurus_led_enable(led_cfg->idx, param);

	return size;
}
static DEVICE_ATTR(hw_enable, 0644, taurus_led_show_hw_enable,
		   taurus_led_store_hw_enable);

static ssize_t taurus_led_show_hw_id(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct taurus_led_cfg *led_cfg =
		container_of(led_cdev, struct taurus_led_cfg, cdev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_cfg->lock, flags);

	ret += sprintf(buf, "%d\n", led_cfg->idx);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return ret;
}
static DEVICE_ATTR(hw_id, 0644, taurus_led_show_hw_id, NULL);

static struct attribute *taurus_led_attrs[] = {
	&dev_attr_hw_id.attr,
	&dev_attr_hw_event.attr,
	&dev_attr_hw_port.attr,
	&dev_attr_hw_blink.attr,
	&dev_attr_hw_enable.attr,
	NULL
};
ATTRIBUTE_GROUPS(taurus_led);

static int taurus_led_probe_one(struct taurus_led_ctrl *led_ctrl,
				struct device_node *nc, u32 reg,
				void __iomem *mem, spinlock_t *lock)
{
	struct taurus_led_cfg *led_cfg;
	unsigned long flags;
	const char *state;
	u32 val;
	int rc, active_low, off_event, blink_event, on_event, port, blink;

	led_cfg = devm_kzalloc(led_ctrl->dev, sizeof(*led_cfg), GFP_KERNEL);
	if (!led_cfg)
		return -ENOMEM;
	led_ctrl->led_cfg[reg] = led_cfg;

	led_cfg->idx = reg;
	led_cfg->mem = mem;
	led_cfg->lock = lock;
	led_cfg->dev = led_ctrl->dev;
	led_cfg->data = ioread32(mem);

	led_cfg->cdev.name = of_get_property(nc, "label", NULL) ? : nc->name;
	led_cfg->cdev.default_trigger = of_get_property(nc,
							"linux,default-trigger",
							NULL);

	if (of_property_read_bool(nc, "active-low"))
		active_low = 1;
	else
		active_low = 0;

	if (of_property_read_bool(nc, "off,hw-rx"))
		off_event = TAURUS_LED_TRIGGER_HW_RX;
	else if (of_property_read_bool(nc, "off,hw-tx"))
		off_event = TAURUS_LED_TRIGGER_HW_TX;
	else if (of_property_read_bool(nc, "off,sw"))
		off_event = TAURUS_LED_TRIGGER_SW;
	else
		off_event = TAURUS_LED_TRIGGER_NONE;

	if (of_property_read_bool(nc, "blinking,hw-rx"))
		blink_event = TAURUS_LED_TRIGGER_HW_RX;
	else if (of_property_read_bool(nc, "blinking,hw-tx"))
		blink_event = TAURUS_LED_TRIGGER_HW_TX;
	else if (of_property_read_bool(nc, "blinking,sw"))
		blink_event = TAURUS_LED_TRIGGER_SW;
	else
		blink_event = TAURUS_LED_TRIGGER_NONE;

	if (of_property_read_bool(nc, "on,hw-rx"))
		on_event = TAURUS_LED_TRIGGER_HW_RX;
	else if (of_property_read_bool(nc, "on,hw-tx"))
		on_event = TAURUS_LED_TRIGGER_HW_TX;
	else if (of_property_read_bool(nc, "on,sw"))
		on_event = TAURUS_LED_TRIGGER_SW;
	else
		on_event = TAURUS_LED_TRIGGER_NONE;

	if (!of_property_read_u32(nc, "port", &val))
		port = val;
	else
		port = 0;

	if (of_property_read_bool(nc, "blink-rate1"))
		blink = TAURUS_LED_BLINK_RATE1;
	else
		blink = TAURUS_LED_BLINK_RATE2;

	taurus_led_config(led_cfg->idx, active_low, off_event, blink_event,
			  on_event, port, blink);

	spin_lock_irqsave(lock, flags);

	if (!of_property_read_string(nc, "default-state", &state)) {
		if (!strcmp(state, "disabled")) {
			led_cfg->enable = 0;
			led_cfg->cdev.brightness = LED_OFF;
		} else {
			led_cfg->enable = 1;

			if (!strcmp(state, "on"))
				led_cfg->cdev.brightness = LED_FULL;
			else if (!strcmp(state, "keep"))
				led_cfg->cdev.brightness = LED_OFF;
		}
	} else {
		led_cfg->enable = 1;
		led_cfg->cdev.brightness = LED_OFF;
	}

	spin_unlock_irqrestore(lock, flags);

	if (led_cfg->enable)
		taurus_led_set(&led_cfg->cdev, led_cfg->cdev.brightness);

	taurus_led_enable(led_cfg->idx, led_cfg->enable);

	led_cfg->cdev.brightness_set = taurus_led_set;
	led_cfg->cdev.blink_set = taurus_led_blink_set;
	led_cfg->cdev.groups = taurus_led_groups;

	rc = devm_led_classdev_register(led_ctrl->dev, &led_cfg->cdev);
	if (rc < 0)
		return rc;

	dev_set_drvdata(led_cfg->cdev.dev, led_cfg);

	mutex_lock(&led_cfg->cdev.led_access);
	if (led_cfg->on_event == TAURUS_LED_TRIGGER_SW) {
		led_sysfs_enable(&led_cfg->cdev);
	} else {
		led_sysfs_disable(&led_cfg->cdev);
		led_cfg->cdev.default_trigger = "none";
	}
	mutex_unlock(&led_cfg->cdev.led_access);

	return 0;
}

static ssize_t taurus_leds_test_mode_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct taurus_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_ctrl->lock, flags);

	ret += sprintf(buf, "%d\n", led_ctrl->test_mode);

	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return ret;
}

static ssize_t taurus_leds_test_mode_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	unsigned long param;

	if (kstrtoul(buf, 10, &param))
		return count;

	taurus_led_test_mode(param);

	return count;
}
static DEVICE_ATTR(test_mode, 0664, taurus_leds_test_mode_show,
		   taurus_leds_test_mode_store);

static ssize_t taurus_leds_clk_low_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct taurus_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_ctrl->lock, flags);

	ret += sprintf(buf, "%d\n", led_ctrl->clk_low);

	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return ret;
}

static ssize_t taurus_leds_clk_low_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct taurus_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags, param;
	u32 val;

	if (kstrtoul(buf, 10, &param))
		return count;

	spin_lock_irqsave(led_ctrl->lock, flags);
	val = taurus_led_read(led_ctrl->mem, NULL);
	if (param) {
		led_ctrl->clk_low = 1;
		val |= TAURUS_LED_CLK_POLARITY;
	} else {
		led_ctrl->clk_low = 0;
		val &= ~TAURUS_LED_CLK_POLARITY;
	}
	taurus_led_write(led_ctrl->mem, val, NULL);
	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return count;
}
static DEVICE_ATTR(clk_low, 0664, taurus_leds_clk_low_show,
		   taurus_leds_clk_low_store);

static ssize_t taurus_leds_blink_rate1_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct taurus_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_ctrl->lock, flags);

	ret += sprintf(buf, "%d\n", (led_ctrl->blink_rate1 + 1) * 16);

	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return ret;
}

static ssize_t taurus_leds_blink_rate1_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct taurus_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags, param;
	u32 val;

	if (kstrtoul(buf, 10, &param))
		return count;

	if (param >= 16) {
		param = param / 16 - 1;
		if (param > TAURUS_LED_MAX_HW_BLINK)
			param = TAURUS_LED_MAX_HW_BLINK;
	} else {
		param = 0;
	}

	spin_lock_irqsave(led_ctrl->lock, flags);
	led_ctrl->blink_rate1 = param;
	val = taurus_led_read(led_ctrl->mem, NULL);
	val &= ~(TAURUS_LED_BLINK_RATE1_MASK << TAURUS_LED_BLINK_RATE1_OFFSET);
	val |= led_ctrl->blink_rate1 << TAURUS_LED_BLINK_RATE1_OFFSET;
	taurus_led_write(led_ctrl->mem, val, NULL);
	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return count;
}
static DEVICE_ATTR(blink_rate1, 0664, taurus_leds_blink_rate1_show,
		   taurus_leds_blink_rate1_store);

static ssize_t taurus_leds_blink_rate2_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct taurus_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_ctrl->lock, flags);

	ret += sprintf(buf, "%d\n", (led_ctrl->blink_rate2 + 1) * 16);

	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return ret;
}

static ssize_t taurus_leds_blink_rate2_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct taurus_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags, param;
	u32 val;

	if (kstrtoul(buf, 10, &param))
		return count;

	if (param >= 16) {
		param = param / 16 - 1;
		if (param > TAURUS_LED_MAX_HW_BLINK)
			param = TAURUS_LED_MAX_HW_BLINK;
	} else {
		param = 0;
	}

	spin_lock_irqsave(led_ctrl->lock, flags);
	led_ctrl->blink_rate2 = param;
	val = taurus_led_read(led_ctrl->mem, NULL);
	val &= ~(TAURUS_LED_BLINK_RATE2_MASK << TAURUS_LED_BLINK_RATE2_OFFSET);
	val |= led_ctrl->blink_rate2 << TAURUS_LED_BLINK_RATE2_OFFSET;
	taurus_led_write(led_ctrl->mem, val, NULL);
	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return count;
}
static DEVICE_ATTR(blink_rate2, 0664, taurus_leds_blink_rate2_show,
		   taurus_leds_blink_rate2_store);

static struct attribute *taurus_leds_attributes[] = {
	&dev_attr_test_mode.attr,
	&dev_attr_clk_low.attr,
	&dev_attr_blink_rate1.attr,
	&dev_attr_blink_rate2.attr,
	NULL
};

static const struct attribute_group taurus_leds_attr_group = {
	.attrs = taurus_leds_attributes,
};

static int taurus_leds_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct resource *mem_r;
	void __iomem *mem;
	spinlock_t *lock;
	u32 val, reg;
	unsigned long flags;

	glb_led_ctrl = devm_kzalloc(dev, sizeof(*glb_led_ctrl), GFP_KERNEL);
	if (!glb_led_ctrl)
		return -ENOMEM;
	glb_led_ctrl->dev = dev;

	mem_r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem_r)
		return -EINVAL;

	mem = devm_ioremap_resource(dev, mem_r);
	if (IS_ERR(mem))
		return PTR_ERR(mem);
	glb_led_ctrl->mem = mem;

	lock = devm_kzalloc(dev, sizeof(*lock), GFP_KERNEL);
	if (!lock)
		return -ENOMEM;

	spin_lock_init(lock);
	glb_led_ctrl->lock = lock;

	reg = 0;
	if (!of_property_read_u32(np, "clk-low", &val)) {
		glb_led_ctrl->clk_low = 1;
		reg |= TAURUS_LED_CLK_POLARITY;
	} else {
		reg &= ~TAURUS_LED_CLK_POLARITY;
	}

	if (!of_property_read_u32(np, "hw-blink-rate1", &val)) {
		val = val >= 16 ? val / 16 - 1 : 0;
		glb_led_ctrl->blink_rate1 = val > TAURUS_LED_MAX_HW_BLINK ?
					    TAURUS_LED_MAX_HW_BLINK : val;
	}
	reg |= (glb_led_ctrl->blink_rate1 & TAURUS_LED_BLINK_RATE1_MASK) <<
	       TAURUS_LED_BLINK_RATE1_OFFSET;

	if (!of_property_read_u32(np, "hw-blink-rate2", &val)) {
		val = val >= 16 ? val / 16 - 1 : 0;
		glb_led_ctrl->blink_rate2 = val > TAURUS_LED_MAX_HW_BLINK ?
					    TAURUS_LED_MAX_HW_BLINK : val;
	}
	reg |= (glb_led_ctrl->blink_rate2 & TAURUS_LED_BLINK_RATE2_MASK) <<
	       TAURUS_LED_BLINK_RATE2_OFFSET;

	spin_lock_irqsave(lock, flags);
	taurus_led_write(glb_led_ctrl->mem, reg, NULL);
	spin_unlock_irqrestore(lock, flags);

	dev_set_drvdata(dev, glb_led_ctrl);

	mem += 4;
	for_each_available_child_of_node_scoped(np, child) {
		int rc;
		u32 reg;

		if (of_property_read_u32(child, "reg", &reg))
			continue;

		if (reg >= TAURUS_LED_MAX_COUNT) {
			dev_err(dev, "invalid LED (%u >= %d)\n", reg,
				TAURUS_LED_MAX_COUNT);
			continue;
		}

		rc = taurus_led_probe_one(glb_led_ctrl, child, reg,
					  mem + reg * 4, lock);
		if (rc < 0) {
			glb_led_ctrl = NULL;
			return rc;
		}
	}

	return devm_device_add_group(dev, &taurus_leds_attr_group);
}

static const struct of_device_id taurus_leds_of_match[] = {
	{ .compatible = "realtek,taurus-leds", },
	{ },
};
MODULE_DEVICE_TABLE(of, taurus_leds_of_match);

static struct platform_driver taurus_leds_driver = {
	.probe = taurus_leds_probe,
	.driver = {
		.name = "taurus-leds",
		.of_match_table = taurus_leds_of_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(taurus_leds_driver);

MODULE_DESCRIPTION("LED driver for Realtek Taurus controllers");
MODULE_LICENSE("GPL");
