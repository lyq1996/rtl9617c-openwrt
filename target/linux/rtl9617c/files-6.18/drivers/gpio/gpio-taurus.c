// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/gpio/driver.h>
#include <linux/pinctrl/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/soc/realtek/taurus-spare-reg.h>

#define TAURUS_GPIO_CFG		0x00
#define TAURUS_GPIO_OUT		0x04
#define TAURUS_GPIO_IN		0x08
#define TAURUS_GPIO_LVL		0x0c
#define TAURUS_GPIO_EDGE	0x10
#define TAURUS_GPIO_BOTHEDGE	0x14
#define TAURUS_GPIO_IE		0x18
#define TAURUS_GPIO_INT		0x1c
#define TAURUS_GPIO_STAT	0x20

#define TAURUS_GPIO_CFG_OUT	0
#define TAURUS_GPIO_CFG_IN	1

#define TAURUS_GPIO_MAX_BANK_NUM	5
#define TAURUS_GPIO_BANK_SIZE		32
#define TAURUS_GPIO_BANK_REG_OFFSET	0x24

struct taurus_gpio_bank {
	struct gpio_chip chip;
	struct irq_domain *domain;
	void __iomem *reg;
	int irq;
	char label[16];
	raw_spinlock_t lock;
};

struct taurus_gpio_bank_info {
	struct taurus_gpio *priv;
	struct device_node *node;
	unsigned int index;
	struct taurus_gpio_bank *bank;
};

struct taurus_gpio {
	struct device *dev;
	void __iomem *reg;
	int idx;
	struct taurus_gpio_bank_info *info[TAURUS_GPIO_MAX_BANK_NUM];
};

static void taurus_gpio_write(struct taurus_gpio_bank *bank,
			      unsigned int reg_offs, u32 data)
{
	iowrite32(data, bank->reg + reg_offs);
}

static u32 taurus_gpio_read(struct taurus_gpio_bank *bank, unsigned int reg_offs)
{
	return ioread32(bank->reg + reg_offs);
}

static void taurus_gpio_clear_bit(struct taurus_gpio_bank *bank,
				  unsigned int reg_offs, unsigned int offset)
{
	u32 value = taurus_gpio_read(bank, reg_offs);

	value &= ~BIT(offset);
	taurus_gpio_write(bank, reg_offs, value);
}

static void taurus_gpio_set_bit(struct taurus_gpio_bank *bank,
				unsigned int reg_offs, unsigned int offset)
{
	u32 value = taurus_gpio_read(bank, reg_offs);

	value |= BIT(offset);
	taurus_gpio_write(bank, reg_offs, value);
}

static void taurus_gpio_mod_bit(struct taurus_gpio_bank *bank,
				unsigned int reg_offs, unsigned int offset,
				bool val)
{
	u32 value = taurus_gpio_read(bank, reg_offs);

	value &= ~BIT(offset);
	if (val)
		value |= BIT(offset);
	taurus_gpio_write(bank, reg_offs, value);
}

static u32 taurus_gpio_get_bit(struct taurus_gpio_bank *bank,
			       unsigned int reg_offs, unsigned int offset)
{
	u32 value = taurus_gpio_read(bank, reg_offs) & BIT(offset);

	return value ? 1 : 0;
}

static int taurus_gpio_request(struct gpio_chip *chip, unsigned int offset)
{
	struct taurus_gpio_bank *bank = gpiochip_get_data(chip);
	unsigned long flags;
	int ret;

	ret = pinctrl_gpio_request(chip, offset);
	if (ret)
		return ret;

	raw_spin_lock_irqsave(&bank->lock, flags);

	taurus_gpio_set_bit(bank, TAURUS_GPIO_CFG, offset);

	raw_spin_unlock_irqrestore(&bank->lock, flags);

	if (!strcmp(bank->label, "taurus-gpio0") && offset < 16)
		taurus_spare_reg_mode_set(offset, 0);

	return 0;
}

static void taurus_gpio_free(struct gpio_chip *chip, unsigned int offset)
{
	struct taurus_gpio_bank *bank = gpiochip_get_data(chip);
	unsigned long flags;

	raw_spin_lock_irqsave(&bank->lock, flags);

	taurus_gpio_set_bit(bank, TAURUS_GPIO_CFG, offset);

	raw_spin_unlock_irqrestore(&bank->lock, flags);

	pinctrl_gpio_free(chip, offset);
}

static int taurus_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	struct taurus_gpio_bank *bank = gpiochip_get_data(chip);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&bank->lock, flags);

	val = taurus_gpio_get_bit(bank, TAURUS_GPIO_CFG, offset);

	raw_spin_unlock_irqrestore(&bank->lock, flags);

	return (val == TAURUS_GPIO_CFG_IN) ? GPIO_LINE_DIRECTION_IN : GPIO_LINE_DIRECTION_OUT;
}

static int taurus_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	struct taurus_gpio_bank *bank = gpiochip_get_data(chip);
	unsigned long flags;

	raw_spin_lock_irqsave(&bank->lock, flags);

	taurus_gpio_set_bit(bank, TAURUS_GPIO_CFG, offset);

	raw_spin_unlock_irqrestore(&bank->lock, flags);

	return 0;
}

static int taurus_gpio_direction_output(struct gpio_chip *chip,
					unsigned int offset, int output_value)
{
	struct taurus_gpio_bank *bank = gpiochip_get_data(chip);
	unsigned long flags;

	raw_spin_lock_irqsave(&bank->lock, flags);

	taurus_gpio_mod_bit(bank, TAURUS_GPIO_OUT, offset, output_value);
	taurus_gpio_clear_bit(bank, TAURUS_GPIO_CFG, offset);

	raw_spin_unlock_irqrestore(&bank->lock, flags);

	return 0;
}

static int taurus_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct taurus_gpio_bank *bank = gpiochip_get_data(chip);
	unsigned long flags;
	u32 value;
	int dir;

	dir = taurus_gpio_get_direction(chip, offset);

	raw_spin_lock_irqsave(&bank->lock, flags);

	if (dir == GPIO_LINE_DIRECTION_IN)
		value = taurus_gpio_get_bit(bank, TAURUS_GPIO_IN, offset);
	else
		value = taurus_gpio_get_bit(bank, TAURUS_GPIO_OUT, offset);

	raw_spin_unlock_irqrestore(&bank->lock, flags);

	return value;
}

static int taurus_gpio_set(struct gpio_chip *chip, unsigned int offset,
			   int output_value)
{
	struct taurus_gpio_bank *bank = gpiochip_get_data(chip);
	unsigned long flags;

	if (taurus_gpio_get_direction(chip, offset) == GPIO_LINE_DIRECTION_IN)
		return -EPERM;

	raw_spin_lock_irqsave(&bank->lock, flags);

	if (output_value)
		taurus_gpio_set_bit(bank, TAURUS_GPIO_OUT, offset);
	else
		taurus_gpio_clear_bit(bank, TAURUS_GPIO_OUT, offset);

	raw_spin_unlock_irqrestore(&bank->lock, flags);

	return 0;
}

static int taurus_gpio_to_irq(struct gpio_chip *chip, unsigned int offset)
{
	struct taurus_gpio_bank *bank = gpiochip_get_data(chip);

	if (!bank->domain)
		return -EINVAL;

	return irq_create_mapping(bank->domain, offset);
}

static struct taurus_gpio_bank *taurus_gpio_from_irqd(struct irq_data *data)
{
	return (struct taurus_gpio_bank *)data->domain->host_data;
}

static unsigned int taurus_gpio_startup_irq(struct irq_data *data)
{
	WARN(irqd_get_trigger_type(data) == IRQ_TYPE_NONE,
	     "irq type not set before enabling gpio irq %d", data->irq);

	irq_gc_ack_clr_bit(data);
	irq_gc_mask_set_bit(data);
	return 0;
}

static int taurus_gpio_set_irq_type(struct irq_data *data, unsigned int type)
{
	struct taurus_gpio_bank *bank = taurus_gpio_from_irqd(data);
	unsigned int level;
	unsigned int edge;
	unsigned int both;
	unsigned long flags;

	switch (type) {
	case IRQ_TYPE_LEVEL_LOW:
		level = 0;
		edge = 0;
		both = 0;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		level = 1;
		edge = 0;
		both = 0;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		level = 0;
		edge = 1;
		both = 0;
		break;
	case IRQ_TYPE_EDGE_RISING:
		level = 1;
		edge = 1;
		both = 0;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		level = 0;
		edge = 1;
		both = 1;
		break;
	default:
		return -EINVAL;
	}

	raw_spin_lock_irqsave(&bank->lock, flags);

	taurus_gpio_mod_bit(bank, TAURUS_GPIO_LVL, data->hwirq, level);
	taurus_gpio_mod_bit(bank, TAURUS_GPIO_EDGE, data->hwirq, edge);
	taurus_gpio_mod_bit(bank, TAURUS_GPIO_BOTHEDGE, data->hwirq, both);

	raw_spin_unlock_irqrestore(&bank->lock, flags);

	return irq_setup_alt_chip(data, type);
}

#ifdef CONFIG_SUSPEND
static int taurus_gpio_set_irq_wake(struct irq_data *data, unsigned int on)
{
	struct taurus_gpio_bank *bank = taurus_gpio_from_irqd(data);

	return irq_set_irq_wake(bank->irq, on);
}
#else
#define taurus_gpio_set_irq_wake NULL
#endif

static void taurus_gpio_irq_handler(struct irq_desc *desc)
{
	irq_hw_number_t hw;
	unsigned int irq_stat;
	struct taurus_gpio_bank *bank;
	struct irq_chip *chip = irq_desc_get_chip(desc);

	bank = (struct taurus_gpio_bank *)irq_desc_get_handler_data(desc);
	irq_stat = taurus_gpio_read(bank, TAURUS_GPIO_INT) &
		   taurus_gpio_read(bank, TAURUS_GPIO_IE);
	taurus_gpio_write(bank, TAURUS_GPIO_INT, irq_stat);

	chained_irq_enter(chip, desc);

	for (hw = 0; irq_stat; irq_stat >>= 1, ++hw) {
		if (!(irq_stat & 1))
			continue;

		generic_handle_domain_irq(bank->domain, hw);
	}

	chained_irq_exit(chip, desc);
}

static int taurus_gpio_bank_probe(struct taurus_gpio_bank_info *info)
{
	struct device_node *np = info->node;
	struct device *dev = info->priv->dev;
	struct taurus_gpio_bank *bank;
	struct irq_chip_generic *gc;
	unsigned int ngpios;
	int err;

	bank = devm_kzalloc(dev, sizeof(*bank), GFP_KERNEL);
	if (!bank)
		return -ENOMEM;

	raw_spin_lock_init(&bank->lock);

	bank->reg = info->priv->reg + info->index * TAURUS_GPIO_BANK_REG_OFFSET;

	snprintf(bank->label, sizeof(bank->label), "taurus-gpio%u", info->index);
	bank->chip.label		= bank->label;
	bank->chip.parent		= dev;
	bank->chip.request		= taurus_gpio_request;
	bank->chip.free			= taurus_gpio_free;
	bank->chip.get_direction	= taurus_gpio_get_direction;
	bank->chip.direction_input	= taurus_gpio_direction_input;
	bank->chip.direction_output	= taurus_gpio_direction_output;
	bank->chip.get			= taurus_gpio_get;
	bank->chip.set			= taurus_gpio_set;
	bank->chip.to_irq		= taurus_gpio_to_irq;
	bank->chip.fwnode		= of_fwnode_handle(np);

	bank->chip.base = info->index * TAURUS_GPIO_BANK_SIZE +
		info->priv->idx * TAURUS_GPIO_MAX_BANK_NUM * TAURUS_GPIO_BANK_SIZE;
	if (of_property_read_u32(np, "ngpios", &ngpios))
		ngpios = TAURUS_GPIO_BANK_SIZE;
	if (ngpios > TAURUS_GPIO_BANK_SIZE)
		ngpios = TAURUS_GPIO_BANK_SIZE;
	bank->chip.ngpio = ngpios;

	err = devm_gpiochip_add_data(dev, &bank->chip, bank);
	if (err < 0) {
		dev_err(dev, "couldn't add GPIO chip: %d\n", err);
		return err;
	}

	info->bank = bank;

	bank->irq = irq_of_parse_and_map(np, 0);

	if (!bank->irq) {
		dev_info(dev, "IRQ not provided for bank %u, IRQs disabled\n",
			 info->index);
		return 0;
	}

	bank->domain = irq_domain_add_linear(np, bank->chip.ngpio,
					     &irq_generic_chip_ops, bank);
	if (!bank->domain) {
		dev_info(dev, "IRQ domain alloc failed for bank %u, IRQs disabled\n",
			 info->index);
		return 0;
	}

	err = irq_alloc_domain_generic_chips(bank->domain, bank->chip.ngpio, 2,
					     bank->label, handle_bad_irq, 0, 0,
					     IRQ_GC_INIT_NESTED_LOCK);
	if (err) {
		dev_info(dev, "generic chip alloc failed for bank %u, IRQs disabled\n",
			 info->index);
		irq_domain_remove(bank->domain);
		bank->domain = NULL;
		return 0;
	}

	gc = irq_get_domain_generic_chip(bank->domain, 0);
	gc->reg_base = bank->reg;

	gc->chip_types[0].type			= IRQ_TYPE_LEVEL_MASK;
	gc->chip_types[0].handler		= handle_level_irq;
	gc->chip_types[0].regs.ack		= TAURUS_GPIO_INT;
	gc->chip_types[0].regs.mask		= TAURUS_GPIO_IE;
	gc->chip_types[0].chip.name		= bank->label;
	gc->chip_types[0].chip.irq_startup	= taurus_gpio_startup_irq;
	gc->chip_types[0].chip.irq_ack		= irq_gc_ack_clr_bit;
	gc->chip_types[0].chip.irq_mask		= irq_gc_mask_clr_bit;
	gc->chip_types[0].chip.irq_unmask	= irq_gc_mask_set_bit;
	gc->chip_types[0].chip.irq_set_type	= taurus_gpio_set_irq_type;
	gc->chip_types[0].chip.irq_set_wake	= taurus_gpio_set_irq_wake;
	gc->chip_types[0].chip.flags		= IRQCHIP_MASK_ON_SUSPEND;

	gc->chip_types[1].type			= IRQ_TYPE_EDGE_BOTH;
	gc->chip_types[1].handler		= handle_edge_irq;
	gc->chip_types[1].regs.ack		= TAURUS_GPIO_INT;
	gc->chip_types[1].regs.mask		= TAURUS_GPIO_IE;
	gc->chip_types[1].chip.name		= bank->label;
	gc->chip_types[1].chip.irq_startup	= taurus_gpio_startup_irq;
	gc->chip_types[1].chip.irq_ack		= irq_gc_ack_clr_bit;
	gc->chip_types[1].chip.irq_mask		= irq_gc_mask_clr_bit;
	gc->chip_types[1].chip.irq_unmask	= irq_gc_mask_set_bit;
	gc->chip_types[1].chip.irq_set_type	= taurus_gpio_set_irq_type;
	gc->chip_types[1].chip.irq_set_wake	= taurus_gpio_set_irq_wake;
	gc->chip_types[1].chip.flags		= IRQCHIP_MASK_ON_SUSPEND;

	irq_set_chained_handler_and_data(bank->irq, taurus_gpio_irq_handler, bank);

	return 0;
}

static void taurus_gpio_register_banks(struct taurus_gpio *priv)
{
	struct device_node *np = priv->dev->of_node;

	for_each_available_child_of_node_scoped(np, node) {
		struct taurus_gpio_bank_info *info;
		u32 addr;
		int ret;

		ret = of_property_read_u32(node, "reg", &addr);
		if (ret) {
			dev_err(priv->dev, "invalid reg on %pOF\n", node);
			continue;
		}
		if (addr >= TAURUS_GPIO_MAX_BANK_NUM) {
			dev_err(priv->dev, "index %u in %pOF out of range\n",
				addr, node);
			continue;
		}

		info = devm_kzalloc(priv->dev, sizeof(*info), GFP_KERNEL);
		if (!info)
			return;

		info->index = addr;
		info->node = of_node_get(node);
		info->priv = priv;
		info->bank = NULL;

		ret = taurus_gpio_bank_probe(info);
		if (ret) {
			dev_err(priv->dev, "failure registering %pOF\n", node);
			of_node_put(node);
			continue;
		}

		priv->info[info->index] = info;
	}
}

static int taurus_gpio_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct resource *res_regs;
	struct taurus_gpio *priv;

	if (!np)
		return -ENOENT;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);

	res_regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->reg = devm_ioremap_resource(dev, res_regs);
	if (IS_ERR(priv->reg))
		return PTR_ERR(priv->reg);

	if (of_property_read_u32(np, "id", &priv->idx))
		return -EINVAL;

	taurus_gpio_register_banks(priv);

	return 0;
}

static const struct of_device_id taurus_gpio_of_match[] = {
	{ .compatible = "realtek,taurus-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, taurus_gpio_of_match);

static struct platform_driver taurus_gpio_driver = {
	.driver = {
		.name		= "taurus-gpio",
		.of_match_table	= taurus_gpio_of_match,
	},
	.probe		= taurus_gpio_probe,
};

module_platform_driver(taurus_gpio_driver);

MODULE_DESCRIPTION("Realtek Taurus GPIO Driver");
MODULE_LICENSE("GPL");
