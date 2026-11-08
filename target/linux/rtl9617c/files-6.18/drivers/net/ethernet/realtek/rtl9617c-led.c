// SPDX-License-Identifier: GPL-2.0-only
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include "rtl9617c-led.h"

static int rtl9617c_led_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct rtl9617c_led *led = container_of(cdev, struct rtl9617c_led, cdev);
	int ret;

	mutex_lock(&led->lock);
	led->requested = value;
	ret = led->output(led, value);
	mutex_unlock(&led->lock);
	return ret;
}

int rtl9617c_led_register(struct device *dev, struct rtl9617c_led *led,
			  struct led_init_data *init)
{
	int ret;

	mutex_init(&led->lock);
	led->cdev.brightness = led->requested;
	led->cdev.brightness_set_blocking = rtl9617c_led_set;
	ret = led_classdev_register_ext(dev, &led->cdev, init);
	if (ret)
		return ret;
	ret = led->output(led, led->requested);
	if (ret)
		led_classdev_unregister(&led->cdev);
	return ret;
}

void rtl9617c_led_unregister(struct rtl9617c_led *led)
{
	int ret;

	led_classdev_unregister(&led->cdev);
	mutex_lock(&led->lock);
	ret = led->output(led, 0);
	mutex_unlock(&led->lock);
	if (ret)
		pr_err("rtl9617c LED %s: shutdown failed: %d\n", led->cdev.name, ret);
}
