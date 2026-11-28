// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/gpio/driver.h>
#include <linux/spinlock.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>

#define DRIVER_NAME	"pinctrl-taurus"

#define PIN_MUX_OFFSET		0x00
#define PIN_MUX_NEW_OFFSET	0x04
#define SD_IO_PULL_OFFSET	0x08
#define IO_DRV_OFFSET		0x0c
#define GPIO0_MUX_OFFSET	0x10
#define GPIO1_MUX_OFFSET	0x14
#define GPIO2_MUX_OFFSET	0x18
#define GPIO3_MUX_OFFSET	0x1c
#define GPIO4_MUX_OFFSET	0x20

#define GPIO_GROUP		5
#define PIN_COLLECTIONS		(GPIO_GROUP + 2)
#define PINS_PER_COLLECTION	32
#define PINS_COUNT		(PIN_COLLECTIONS * PINS_PER_COLLECTION)

struct taurus_pinmap {
	void __iomem		*regs;
	struct device		*dev;
	struct pinctrl_dev	*pctl;
	struct pinctrl_desc	*desc;
	const char		*pin_names[PINS_COUNT];
	spinlock_t		lock;
	bool			is_taurus;
};

static int taurus_get_groups_count(struct pinctrl_dev *pctldev)
{
	return PINS_COUNT;
}

static const char *taurus_get_group_name(struct pinctrl_dev *pctldev,
					 unsigned selector)
{
	struct taurus_pinmap *pmap = pinctrl_dev_get_drvdata(pctldev);

	return pmap->desc->pins[selector].name;
}

static int taurus_get_group_pins(struct pinctrl_dev *pctldev, unsigned selector,
				 const unsigned **pins, unsigned *num_pins)
{
	struct taurus_pinmap *pmap = pinctrl_dev_get_drvdata(pctldev);

	*pins = &pmap->desc->pins[selector].number;
	*num_pins = 1;

	return 0;
}

static const struct pinctrl_ops taurus_pinctrl_ops = {
	.get_groups_count	= taurus_get_groups_count,
	.get_group_name		= taurus_get_group_name,
	.get_group_pins		= taurus_get_group_pins,
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_pin,
	.dt_free_map		= pinconf_generic_dt_free_map,
};

static const char *const taurus_functions[] = {
	"gpio",
	"multi-fn",
};

static int taurus_get_functions_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(taurus_functions);
}

static const char *taurus_get_fname(struct pinctrl_dev *pctldev, unsigned selector)
{
	return taurus_functions[selector];
}

static int taurus_get_groups(struct pinctrl_dev *pctldev, unsigned selector,
			     const char * const **groups, unsigned * const num_groups)
{
	struct taurus_pinmap *pmap = pinctrl_dev_get_drvdata(pctldev);

	*groups = pmap->pin_names;
	*num_groups = PINS_COUNT;

	return 0;
}

static bool taurus_is_valid_gpio_pin(int pin_num)
{
	if ((pin_num >= 36 && pin_num <= 41) || (pin_num >= 43 && pin_num <= 46))
		return false;

	return true;
}

static void taurus_pin_sel(const struct taurus_pinmap *pmap, int pin_num,
			   int *reg, int *bit)
{
	*bit = (pin_num % PINS_PER_COLLECTION);

	if (pin_num >= (PINS_PER_COLLECTION * GPIO_GROUP)) {
		*reg = ((pin_num - PINS_PER_COLLECTION * GPIO_GROUP) /
			PINS_PER_COLLECTION) * 4;
	} else {
		if (taurus_is_valid_gpio_pin(pin_num))
			*reg = (pin_num / PINS_PER_COLLECTION) * 4 + GPIO0_MUX_OFFSET;
		else
			*reg = 0;
	}
}

static int taurus_is_pinmux(int reg_off)
{
	return (reg_off <= PIN_MUX_NEW_OFFSET);
}

static int taurus_set_mux(struct pinctrl_dev *pctldev, unsigned selector,
			  unsigned group)
{
	struct taurus_pinmap *pmap = pinctrl_dev_get_drvdata(pctldev);
	unsigned long flags;
	int bit_off, reg_off;
	u32 reg;

	taurus_pin_sel(pmap, group, &reg_off, &bit_off);

	spin_lock_irqsave(&pmap->lock, flags);
	reg = readl_relaxed(pmap->regs + reg_off);

	if (taurus_is_pinmux(reg_off))
		reg |= BIT(bit_off);
	else
		reg &= ~BIT(bit_off);

	writel_relaxed(reg, pmap->regs + reg_off);
	spin_unlock_irqrestore(&pmap->lock, flags);

	return 0;
}

static int taurus_pmx_request_gpio(struct pinctrl_dev *pcdev,
				   struct pinctrl_gpio_range *range,
				   unsigned offset)
{
	struct taurus_pinmap *pmap = pinctrl_dev_get_drvdata(pcdev);
	unsigned long flags;
	int bit_off, reg_off;
	u32 reg;

	taurus_pin_sel(pmap, offset, &reg_off, &bit_off);

	if (taurus_is_pinmux(reg_off))
		return -EBUSY;

	spin_lock_irqsave(&pmap->lock, flags);
	reg = readl_relaxed(pmap->regs + reg_off);
	reg |= BIT(bit_off);
	writel_relaxed(reg, pmap->regs + reg_off);
	spin_unlock_irqrestore(&pmap->lock, flags);

	return 0;
}

static const struct pinmux_ops taurus_pmxops = {
	.get_functions_count	= taurus_get_functions_count,
	.get_function_name	= taurus_get_fname,
	.get_function_groups	= taurus_get_groups,
	.set_mux		= taurus_set_mux,
	.gpio_request_enable	= taurus_pmx_request_gpio,
};

static int taurus_pinctrl_probe(struct platform_device *pdev)
{
	struct taurus_pinmap *pmap;
	struct resource *r;
	struct pinctrl_pin_desc *pins;
	struct pinctrl_desc *pctl_desc;
	char *pin_names;
	int name_len = strlen("GPIO_xxx") + 1;
	int i, j;

	pmap = devm_kzalloc(&pdev->dev, sizeof(*pmap), GFP_KERNEL);
	if (!pmap)
		return -ENOMEM;

	spin_lock_init(&pmap->lock);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pmap->regs = devm_ioremap_resource(&pdev->dev, r);
	if (IS_ERR(pmap->regs))
		return PTR_ERR(pmap->regs);

	pins = devm_kzalloc(&pdev->dev, sizeof(*pins) * PINS_COUNT, GFP_KERNEL);
	if (!pins)
		return -ENOMEM;
	pin_names = devm_kzalloc(&pdev->dev, name_len * PINS_COUNT, GFP_KERNEL);
	if (!pin_names)
		return -ENOMEM;

	for (i = 0; i < GPIO_GROUP; i++) {
		for (j = 0; j < PINS_PER_COLLECTION; j++) {
			int pin_id = i * PINS_PER_COLLECTION + j;
			char *name = &pin_names[pin_id * name_len];

			snprintf(name, name_len, "GPIO_%c%c%c",
				 '0' + i, '0' + (j / 10), '0' + (j % 10));
			pins[pin_id].number = pin_id;
			pins[pin_id].name = name;
			pmap->pin_names[pin_id] = name;
		}
	}

	for (i = GPIO_GROUP; i < PIN_COLLECTIONS; i++) {
		for (j = 0; j < PINS_PER_COLLECTION; j++) {
			int pin_id = i * PINS_PER_COLLECTION + j;
			char *name = &pin_names[pin_id * name_len];

			snprintf(name, name_len, "PMUX_%c%c%c",
				 '0' + (i - GPIO_GROUP), '0' + (j / 10),
				 '0' + (j % 10));
			pins[pin_id].number = pin_id;
			pins[pin_id].name = name;
			pmap->pin_names[pin_id] = name;
		}
	}

	pctl_desc = devm_kzalloc(&pdev->dev, sizeof(*pctl_desc), GFP_KERNEL);
	if (!pctl_desc)
		return -ENOMEM;

	pctl_desc->name = DRIVER_NAME;
	pctl_desc->owner = THIS_MODULE;
	pctl_desc->pctlops = &taurus_pinctrl_ops;
	pctl_desc->pmxops = &taurus_pmxops;
	pctl_desc->npins = PINS_COUNT;
	pctl_desc->pins = pins;
	pmap->desc = pctl_desc;

	pmap->dev = &pdev->dev;
	pmap->is_taurus = true;

	pmap->pctl = devm_pinctrl_register(&pdev->dev, pctl_desc, pmap);
	if (IS_ERR(pmap->pctl)) {
		dev_err(&pdev->dev, "pinctrl registration failed\n");
		return PTR_ERR(pmap->pctl);
	}

	return 0;
}

static const struct of_device_id taurus_pinctrl_ids[] = {
	{ .compatible = "realtek,taurus-pinctrl" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, taurus_pinctrl_ids);

static struct platform_driver taurus_pinctrl_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = taurus_pinctrl_ids,
	},
	.probe = taurus_pinctrl_probe,
};
module_platform_driver(taurus_pinctrl_driver);

MODULE_DESCRIPTION("Realtek Taurus Pin Control Driver");
MODULE_LICENSE("GPL");
