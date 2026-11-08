/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_RTL9617C_LED_H
#define _LINUX_RTL9617C_LED_H

#include <linux/leds.h>
#include <linux/mutex.h>

struct rtl9617c_led {
	struct led_classdev cdev;
	struct mutex lock;
	int (*output)(struct rtl9617c_led *led, unsigned int value);
	unsigned int requested;
	void *drvdata;
	unsigned int channel;
};

int rtl9617c_led_register(struct device *dev, struct rtl9617c_led *led,
			  struct led_init_data *init);
void rtl9617c_led_unregister(struct rtl9617c_led *led);

#endif
