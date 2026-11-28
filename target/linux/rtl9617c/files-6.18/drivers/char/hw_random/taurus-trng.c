// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/iopoll.h>

#define TAURUS_GLOBAL_CONFIG		0xf43200c0
#define TAURUS_GLOBAL_CONFIG_TRNG_PD	BIT(20)

#define TAURUS_RNG_CTRL		0x00
#define TAURUS_RNG_RETURN0	0x1c
#define TAURUS_RNG_RESULTR	0x28
#define TAURUS_RNG_ST_RETURN	0x40
#define TAURUS_RNG_INT		0x54
#define TAURUS_RNG_INT_EN	0x64

#define TAURUS_INT_PARITY	BIT(2)
#define TAURUS_INT_ST_ERR	BIT(3)

#define TAURUS_OUT_READY	BIT(0)
#define TAURUS_MAX_POLL_CNT	0xffff

#define TAURUS_RNG_SRST		BIT(0)
#define TAURUS_RNG_CALI_SRST	BIT(3)

union taurus_rng_ctrl_bits {
	struct {
		u32 rng_srst : 1;
		u32 clk_hf_sel : 1;
		u32 corrector_bypass : 1;
		u32 rng_cali_srst : 1;
		u32 rvd0 : 1;
		u32 corrector_imode : 1;
		u32 dbg0_sel : 5;
		u32 dbg1_sel : 5;
		u32 lfsr_bypass_1 : 1;
		u32 lfsr_mode : 1;
		u32 st_en : 1;
		u32 mode : 1;
		u32 rvd1 : 10;
		u32 opt1 : 1;
		u32 lfsr_mode2 : 1;
	};
	u32 wrd;
};

struct taurus_trng_ctrl {
	struct device *dev;
	void __iomem *reg;
	int irq;
	raw_spinlock_t lock;
	union taurus_rng_ctrl_bits ctrl_reg_v;
	unsigned int is_self_test_en:1;
	unsigned int is_lfsr_bypass:1;
	unsigned int is_corrector_bypass:1;
	atomic_t in_reset;
};

static int taurus_trng_wait_ready(struct hwrng *rng, bool wait)
{
	struct taurus_trng_ctrl *trng = (struct taurus_trng_ctrl *)rng->priv;
	int ready;

	ready = TAURUS_OUT_READY & readl(trng->reg + TAURUS_RNG_RETURN0);
	if (!ready && wait) {
		readl_poll_timeout_atomic(trng->reg + TAURUS_RNG_RETURN0, ready,
					  ready & TAURUS_OUT_READY, 3, 1500);
		ready &= TAURUS_OUT_READY;
	}

	return ready;
}

static int taurus_trng_read(struct hwrng *rng, void *buf, size_t max, bool wait)
{
	struct taurus_trng_ctrl *trng = (struct taurus_trng_ctrl *)rng->priv;
	int retval = 0;

	while (atomic_read(&trng->in_reset) != 0) {
		if (!wait)
			return 0;
		cpu_relax();
	}

	while (max >= sizeof(u32)) {
		if (!taurus_trng_wait_ready(rng, wait))
			break;

		*(u32 *)buf = readl(trng->reg + TAURUS_RNG_RESULTR);

		retval += sizeof(u32);
		buf += sizeof(u32);
		max -= sizeof(u32);
	}

	return retval || !wait ? retval : -EIO;
}

static struct hwrng taurus_trng_hwrng = {
	.name = "taurus-trng",
	.read = taurus_trng_read,
	.quality = 1000,
};

static void taurus_trng_reset_assert(struct taurus_trng_ctrl *t, int mode)
{
	unsigned long flags;

	if (!mode)
		return;

	atomic_inc(&t->in_reset);
	raw_spin_lock_irqsave(&t->lock, flags);
	t->ctrl_reg_v.wrd = readl(t->reg + TAURUS_RNG_CTRL);

	if (mode & TAURUS_RNG_SRST)
		t->ctrl_reg_v.rng_srst = 0x1;

	if (mode & TAURUS_RNG_CALI_SRST)
		t->ctrl_reg_v.rng_cali_srst = 0x1;

	writel(t->ctrl_reg_v.wrd, t->reg + TAURUS_RNG_CTRL);
	raw_spin_unlock_irqrestore(&t->lock, flags);
}

static void taurus_trng_reset_deassert(struct taurus_trng_ctrl *t, int mode)
{
	unsigned long flags;

	if (!mode)
		return;

	raw_spin_lock_irqsave(&t->lock, flags);
	t->ctrl_reg_v.wrd = readl(t->reg + TAURUS_RNG_CTRL);

	if (mode & TAURUS_RNG_SRST)
		t->ctrl_reg_v.rng_srst = 0x0;

	if (mode & TAURUS_RNG_CALI_SRST)
		t->ctrl_reg_v.rng_cali_srst = 0x0;

	writel(t->ctrl_reg_v.wrd, t->reg + TAURUS_RNG_CTRL);
	raw_spin_unlock_irqrestore(&t->lock, flags);
	atomic_dec(&t->in_reset);
}

static irqreturn_t taurus_trng_irq_handler(int irq, void *arg)
{
	struct taurus_trng_ctrl *t = arg;
	u32 val;

	val = readl(t->reg + TAURUS_RNG_INT);
	if (val & TAURUS_INT_PARITY) {
		taurus_trng_reset_assert(t, TAURUS_RNG_SRST);
		taurus_trng_reset_deassert(t, TAURUS_RNG_SRST);
	} else if (val & TAURUS_INT_ST_ERR) {
		dev_info_ratelimited(t->dev, "self test error found (0x%x)\n",
				     readl(t->reg + TAURUS_RNG_ST_RETURN) & 0xf);
		writel(0x0, t->reg + TAURUS_RNG_ST_RETURN);
	}

	writel(val, t->reg + TAURUS_RNG_INT);

	return IRQ_HANDLED;
}

static int taurus_trng_init(struct taurus_trng_ctrl *t)
{
	struct device *dev = t->dev;
	void __iomem *reg;
	u32 reg_v;

	reg = devm_ioremap(dev, TAURUS_GLOBAL_CONFIG, 4);
	if (!reg)
		return -ENOMEM;

	reg_v = readl(reg);
	reg_v &= ~TAURUS_GLOBAL_CONFIG_TRNG_PD;
	writel(reg_v, reg);
	devm_iounmap(dev, reg);

	taurus_trng_reset_assert(t, TAURUS_RNG_SRST | TAURUS_RNG_CALI_SRST);
	usleep_range(1000, 2000);
	taurus_trng_reset_deassert(t, TAURUS_RNG_SRST | TAURUS_RNG_CALI_SRST);

	return 0;
}

static void taurus_trng_setup(struct taurus_trng_ctrl *t)
{
	unsigned long flags;

	dev_info(t->dev, "Thor RNG setup with self test %s\n",
		 t->is_self_test_en ? "enabled" : "disabled");
	if (t->is_lfsr_bypass)
		dev_info(t->dev, "Thor RNG setup with LFSR bypass\n");
	if (t->is_corrector_bypass)
		dev_info(t->dev, "Thor RNG setup with corrector bypass\n");

	raw_spin_lock_irqsave(&t->lock, flags);
	t->ctrl_reg_v.wrd = readl(t->reg + TAURUS_RNG_CTRL);

	if (t->is_self_test_en) {
		t->ctrl_reg_v.st_en = 0x1;
		t->ctrl_reg_v.opt1 = 0x1;
	} else {
		t->ctrl_reg_v.st_en = 0x0;
		t->ctrl_reg_v.opt1 = 0x0;
	}

	t->ctrl_reg_v.lfsr_bypass_1 = t->is_lfsr_bypass ? 0x1 : 0x0;
	t->ctrl_reg_v.corrector_bypass = t->is_corrector_bypass ? 0x1 : 0x0;

	writel(t->ctrl_reg_v.wrd, t->reg + TAURUS_RNG_CTRL);
	writel(0x0000000f, t->reg + TAURUS_RNG_INT_EN);
	raw_spin_unlock_irqrestore(&t->lock, flags);
}

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct taurus_trng_ctrl *trng = dev_get_drvdata(dev);
	ssize_t ret = 0;

	ret += sprintf(buf + ret, "Thor RNG running with:\n");
	ret += sprintf(buf + ret, "Self-Test: %s\n",
		       trng->is_self_test_en ? "enabled" : "disabled");
	ret += sprintf(buf + ret, "LFSR Bypass: %s\n",
		       trng->is_lfsr_bypass ? "enabled" : "disabled");
	ret += sprintf(buf + ret, "Corrector Bypass: %s\n",
		       trng->is_corrector_bypass ? "enabled" : "disabled");

	return ret;
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct taurus_trng_ctrl *trng = dev_get_drvdata(dev);

	if (sysfs_streq(buf, "self-test") || sysfs_streq(buf, "self-test on") ||
	    sysfs_streq(buf, "1")) {
		trng->is_self_test_en = 1;
	} else if (sysfs_streq(buf, "self-test off") || sysfs_streq(buf, "0")) {
		trng->is_self_test_en = 0;
	} else {
		return -EINVAL;
	}

	taurus_trng_setup(trng);

	return count;
}
static DEVICE_ATTR_RW(mode);

static int taurus_trng_data_get(struct taurus_trng_ctrl *trng, u32 *data)
{
	int i = 0;

	while (i < TAURUS_MAX_POLL_CNT) {
		if (TAURUS_OUT_READY & readl(trng->reg + TAURUS_RNG_RETURN0))
			break;
		i++;
	}

	if (i >= TAURUS_MAX_POLL_CNT) {
		dev_err(trng->dev, "Thor RNG polling reaches limit (%d)",
			TAURUS_MAX_POLL_CNT);
		return -EBUSY;
	}

	*data = readl(trng->reg + TAURUS_RNG_RESULTR);

	if (!*data) {
		dev_err(trng->dev, "Thor RNG result is not available!");
		return -ENODATA;
	}

	return 0;
}

static ssize_t data_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct taurus_trng_ctrl *trng = dev_get_drvdata(dev);
	ssize_t cnt = 0;
	int ret = 0;
	u32 val = 0;

	ret = taurus_trng_data_get(trng, &val);
	if (ret < 0)
		cnt += sprintf(buf + cnt, "trng data is not ready\n");
	else
		cnt += sprintf(buf + cnt, "trng data: %x\n", val);

	return cnt;
}
static DEVICE_ATTR_RO(data);

static struct attribute *taurus_trng_attributes[] = {
	&dev_attr_mode.attr,
	&dev_attr_data.attr,
	NULL
};

static const struct attribute_group taurus_trng_attr_group = {
	.attrs = taurus_trng_attributes,
};

static int taurus_trng_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct resource *reg_res;
	struct taurus_trng_ctrl *trng;
	int ret;

	trng = devm_kzalloc(dev, sizeof(*trng), GFP_KERNEL);
	if (!trng)
		return -ENOMEM;

	trng->dev = dev;
	raw_spin_lock_init(&trng->lock);
	atomic_set(&trng->in_reset, 0);

	reg_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!reg_res)
		return -EINVAL;

	trng->reg = devm_ioremap_resource(dev, reg_res);
	if (IS_ERR(trng->reg))
		return PTR_ERR(trng->reg);

	trng->irq = platform_get_irq(pdev, 0);
	if (trng->irq < 0)
		return trng->irq;

	trng->is_self_test_en = of_property_read_bool(np, "self-test");
	trng->is_lfsr_bypass = of_property_read_bool(np, "lfsr-bypass");
	trng->is_corrector_bypass = of_property_read_bool(np,
							  "corrector_bypass");

	dev_set_drvdata(dev, trng);

	ret = taurus_trng_init(trng);
	if (ret) {
		dev_err(dev, "Failed to init Thor RNG\n");
		return ret;
	}

	writel(0, trng->reg + TAURUS_RNG_INT_EN);
	ret = devm_request_irq(dev, trng->irq, taurus_trng_irq_handler,
			       IRQF_NO_THREAD, "taurus-trng", trng);
	if (ret)
		return ret;

	taurus_trng_setup(trng);

	ret = sysfs_create_group(&dev->kobj, &taurus_trng_attr_group);
	if (ret) {
		dev_err(dev, "failed to register sysfs\n");
		goto err_irq;
	}

	taurus_trng_hwrng.priv = (unsigned long)trng;
	ret = hwrng_register(&taurus_trng_hwrng);
	if (ret) {
		dev_err(dev, "hwrng registration failed\n");
		sysfs_remove_group(&dev->kobj, &taurus_trng_attr_group);
		goto err_irq;
	}
	dev_info(dev, "hwrng registered\n");

	return 0;

err_irq:
	writel(0, trng->reg + TAURUS_RNG_INT_EN);
	devm_free_irq(dev, trng->irq, trng);
	return ret;
}

static void taurus_trng_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct taurus_trng_ctrl *trng = dev_get_drvdata(dev);
	void __iomem *reg;
	u32 reg_v;

	hwrng_unregister(&taurus_trng_hwrng);
	sysfs_remove_group(&dev->kobj, &taurus_trng_attr_group);
	writel(0, trng->reg + TAURUS_RNG_INT_EN);
	devm_free_irq(dev, trng->irq, trng);

	reg = devm_ioremap(dev, TAURUS_GLOBAL_CONFIG, 4);
	if (!reg)
		return;

	reg_v = readl(reg);
	reg_v |= TAURUS_GLOBAL_CONFIG_TRNG_PD;
	writel(reg_v, reg);
	devm_iounmap(dev, reg);

	dev_info(dev, "Power down Thor RNG\n");
}

static const struct of_device_id taurus_trng_of_match[] = {
	{ .compatible = "realtek,taurus-trng", },
	{ },
};
MODULE_DEVICE_TABLE(of, taurus_trng_of_match);

static struct platform_driver taurus_trng_driver = {
	.probe = taurus_trng_probe,
	.remove = taurus_trng_remove,
	.driver = {
		.name = "taurus-trng",
		.of_match_table = taurus_trng_of_match,
	},
};

module_platform_driver(taurus_trng_driver);

MODULE_DESCRIPTION("Realtek Thor RNG Driver for Taurus");
MODULE_LICENSE("GPL");
