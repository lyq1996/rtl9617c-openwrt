// SPDX-License-Identifier: GPL-2.0
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include <linux/soc/realtek/taurus-spare-reg.h>

#define TAURUS_SPARE_MAX_COUNT	16

#define TAURUS_SPARE_GPIO	0
#define TAURUS_SPARE_PARALLEL_LED	1

struct taurus_spare_reg_ctrl {
	void __iomem *mem;
	struct device *dev;
};

static DEFINE_RAW_SPINLOCK(taurus_spare_lock);
static struct taurus_spare_reg_ctrl *glb_spare_ctrl;

int taurus_spare_reg_mode_set(unsigned int bit, int mode)
{
	unsigned long flags;
	u32 val;

	if (mode != TAURUS_SPARE_GPIO && mode != TAURUS_SPARE_PARALLEL_LED)
		return -EINVAL;

	raw_spin_lock_irqsave(&taurus_spare_lock, flags);
	if (!glb_spare_ctrl) {
		raw_spin_unlock_irqrestore(&taurus_spare_lock, flags);
		WARN(1, "taurus-spare-reg: calling before probed\n");
		return -ENODEV;
	}

	if (bit >= TAURUS_SPARE_MAX_COUNT) {
		struct device *dev = glb_spare_ctrl->dev;

		raw_spin_unlock_irqrestore(&taurus_spare_lock, flags);
		dev_err(dev, "invalid bit(%u) for PER_SPARE\n", bit);
		return -EINVAL;
	}

	val = ioread32(glb_spare_ctrl->mem);
	if (mode == TAURUS_SPARE_GPIO)
		val &= ~BIT(bit);
	else
		val |= BIT(bit);
	iowrite32(val, glb_spare_ctrl->mem);
	raw_spin_unlock_irqrestore(&taurus_spare_lock, flags);

	return 0;
}
EXPORT_SYMBOL(taurus_spare_reg_mode_set);

static int taurus_spare_reg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct taurus_spare_reg_ctrl *ctrl;
	struct resource *reg_res;
	unsigned long flags;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->dev = dev;

	reg_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!reg_res)
		return -EINVAL;

	ctrl->mem = devm_ioremap_resource(dev, reg_res);
	if (IS_ERR(ctrl->mem))
		return PTR_ERR(ctrl->mem);

	dev_set_drvdata(dev, ctrl);

	raw_spin_lock_irqsave(&taurus_spare_lock, flags);
	glb_spare_ctrl = ctrl;
	raw_spin_unlock_irqrestore(&taurus_spare_lock, flags);

	return 0;
}

static void taurus_spare_reg_remove(struct platform_device *pdev)
{
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&taurus_spare_lock, flags);
	if (glb_spare_ctrl) {
		val = ioread32(glb_spare_ctrl->mem);
		val &= ~GENMASK(TAURUS_SPARE_MAX_COUNT - 1, 0);
		iowrite32(val, glb_spare_ctrl->mem);
		glb_spare_ctrl = NULL;
	}
	raw_spin_unlock_irqrestore(&taurus_spare_lock, flags);
}

static const struct of_device_id taurus_spare_reg_of_match[] = {
	{ .compatible = "realtek,taurus-spare-reg", },
	{ },
};
MODULE_DEVICE_TABLE(of, taurus_spare_reg_of_match);

static struct platform_driver taurus_spare_reg_driver = {
	.probe = taurus_spare_reg_probe,
	.remove = taurus_spare_reg_remove,
	.driver = {
		.name = "taurus-spare-reg",
		.of_match_table = taurus_spare_reg_of_match,
	},
};

static int __init taurus_spare_reg_init(void)
{
	return platform_driver_register(&taurus_spare_reg_driver);
}
arch_initcall(taurus_spare_reg_init);

MODULE_DESCRIPTION("Realtek Taurus PER_SPARE register driver");
MODULE_LICENSE("GPL");
