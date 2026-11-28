// SPDX-License-Identifier: GPL-2.0
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#define TAURUS_PERI_IRQ_STATUS	0x00
#define TAURUS_PERI_IRQ_ENABLE	0x04

static void taurus_peri_ictl_chain_handler(struct irq_desc *desc)
{
	struct irq_domain *d = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	int n;

	chained_irq_enter(chip, desc);

	for (n = 0; n < d->revmap_size; n += 32) {
		struct irq_chip_generic *gc = irq_get_domain_generic_chip(d, n);
		u32 stat = readl_relaxed(gc->reg_base + TAURUS_PERI_IRQ_STATUS);
		u32 enable = readl_relaxed(gc->reg_base + TAURUS_PERI_IRQ_ENABLE);

		stat &= enable;

		while (stat) {
			u32 hwirq = ffs(stat) - 1;
			u32 virq = irq_find_mapping(d, gc->irq_base + hwirq);

			if (virq)
				generic_handle_irq(virq);
			stat &= ~BIT(hwirq);
		}
	}

	chained_irq_exit(chip, desc);
}

static irqreturn_t taurus_peri_ictl_handler(int irq, void *arg)
{
	struct irq_domain *d = arg;
	int n;

	for (n = 0; n < d->revmap_size; n += 32) {
		struct irq_chip_generic *gc = irq_get_domain_generic_chip(d, n);
		u32 stat = readl_relaxed(gc->reg_base + TAURUS_PERI_IRQ_STATUS);
		u32 enable = readl_relaxed(gc->reg_base + TAURUS_PERI_IRQ_ENABLE);

		stat &= enable;

		while (stat) {
			u32 hwirq = ffs(stat) - 1;
			u32 virq = irq_find_mapping(d, gc->irq_base + hwirq);

			if (virq)
				generic_handle_irq(virq);
			stat &= ~BIT(hwirq);
		}
	}
	return IRQ_HANDLED;
}

static int __init taurus_peri_ictl_init(struct device_node *np,
					struct device_node *parent)
{
	unsigned int clr = IRQ_NOREQUEST | IRQ_NOPROBE | IRQ_NOAUTOEN;
	struct resource r;
	struct irq_domain *domain;
	struct irq_chip_generic *gc;
	void __iomem *iobase;
	int ret, nrirqs, irq;

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0)
		return -EINVAL;

	ret = of_address_to_resource(np, 0, &r);
	if (ret)
		return ret;

	if (!request_mem_region(r.start, resource_size(&r), np->full_name))
		return -ENOMEM;

	iobase = ioremap(r.start, resource_size(&r));
	if (!iobase) {
		ret = -ENOMEM;
		goto err_release;
	}

	writel_relaxed(0, iobase + TAURUS_PERI_IRQ_ENABLE);

	nrirqs = 32;

	domain = irq_domain_add_linear(np, nrirqs, &irq_generic_chip_ops, NULL);
	if (!domain) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	ret = irq_alloc_domain_generic_chips(domain, 32, (nrirqs > 32) ? 2 : 1,
					     np->name, handle_level_irq, clr, 0,
					     IRQ_GC_INIT_MASK_CACHE);
	if (ret)
		goto err_unmap;

	gc = irq_get_domain_generic_chip(domain, 0);
	gc->private = domain;
	gc->reg_base = iobase;
	gc->wake_enabled = IRQ_MSK(32);

	gc->chip_types[0].regs.ack = TAURUS_PERI_IRQ_STATUS;
	gc->chip_types[0].regs.mask = TAURUS_PERI_IRQ_ENABLE;

	gc->chip_types[0].chip.irq_ack = irq_gc_ack_set_bit;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_clr_bit;
	gc->chip_types[0].chip.irq_unmask = irq_gc_mask_set_bit;
	gc->chip_types[0].chip.irq_set_wake = irq_gc_set_wake;

	if (of_property_read_bool(np, "export-irq"))
		ret = request_irq(irq, taurus_peri_ictl_handler, IRQF_NO_THREAD,
				  np->name, domain);
	else
		irq_set_chained_handler_and_data(irq, taurus_peri_ictl_chain_handler,
						 domain);

	return 0;

err_unmap:
	iounmap(iobase);
err_release:
	release_mem_region(r.start, resource_size(&r));
	return ret;
}
IRQCHIP_DECLARE(taurus_peri_ictl, "realtek,taurus-peri-ictl", taurus_peri_ictl_init);
