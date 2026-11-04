/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_TYFZA1_LED_H
#define _LINUX_TYFZA1_LED_H

#include <linux/leds.h>
#include <linux/list.h>

struct tyfza1_led {
	struct led_classdev cdev;
	struct list_head node;
	int (*output)(struct tyfza1_led *led, unsigned int value);
	unsigned int requested;
	/* Back-pointer for the output callback. A board can expose several
	 * channels of one controller, so the callback cannot use
	 * container_of() on a single embedded member; it reads drvdata and
	 * channel instead. */
	void *drvdata;
	unsigned int channel;
};

int tyfza1_led_register(struct device *dev, struct tyfza1_led *led,
		       struct led_init_data *init);
void tyfza1_led_unregister(struct tyfza1_led *led);

#endif
