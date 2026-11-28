// SPDX-License-Identifier: GPL-2.0
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define TAURUS_WDT_DEFAULT_TIMEOUT		60
#define TAURUS_WDT_DEFAULT_HIGH_RESOLUTION	0
#define TAURUS_WDT_MIN_TIMEOUT			1

static int wdt_timeout = TAURUS_WDT_DEFAULT_TIMEOUT;
static int nowayout = WATCHDOG_NOWAYOUT;
static int high_resolution = TAURUS_WDT_DEFAULT_HIGH_RESOLUTION;

module_param(wdt_timeout, int, 0);
module_param(nowayout, int, 0);
module_param(high_resolution, int, 0);

struct taurus_wdt {
	void __iomem		*regs;
	void __iomem		*rst_reg;
	struct clk		*clk;
	unsigned long		clk_rate;
	int			irq;
	u32			prescaler;
	u16			ps_div;
	u32			delay_reset;
	bool			rst;
	bool			ctrl_clksel;
	u8			delay_type;
	spinlock_t		io_lock;
	struct watchdog_device	wdt_device;
};

#define TAURUS_WDT_CTRL		0x00
#define TAURUS_WDT_PS		0x04
#define TAURUS_WDT_DIV		0x08
#define TAURUS_WDT_LD		0x0c
#define TAURUS_WDT_LOADE	0x10
#define TAURUS_WDT_CNT		0x14
#define TAURUS_WDT_IE_0		0x18
#define TAURUS_WDT_STAT_0	0x20
#define TAURUS_WDT_INT_0	0x1c
#define TAURUS_WDT_INT_1	0x28
#define TAURUS_WDT_TST		0x30

#define TAURUS_WDT_CTRL_WDTEN		BIT(0)
#define TAURUS_WDT_CTRL_RSTEN		BIT(1)
#define TAURUS_WDT_CTRL_CLKSEL		BIT(2)
#define TAURUS_WDT_CTRL_DELAY_SHIFT	12
#define TAURUS_WDT_CTRL_MAX_DELAY	0x000fffff

#define TAURUS_WDT_LOADE_WDT		BIT(0)
#define TAURUS_WDT_LOADE_PRE		BIT(1)

#define TAURUS_WDT_IE_WDTE		BIT(0)
#define TAURUS_WDT_INT_WDTI		BIT(0)

#define TAURUS_WDT_TST_DELAY_EX_SHIFT	20
#define TAURUS_WDT_TST_DELAY_EX_MASK	0xfff
#define TAURUS_WDT_TST_DELAY_EX2_SHIFT	8
#define TAURUS_WDT_TST_DELAY_EX2_MASK	0xff

#define TAURUS_WD_RESET_SUBSYS_ENABLE	BIT(4)
#define TAURUS_WD_RESET_PCIE		BIT(5)
#define TAURUS_WD_RESET_ALL_BLOCKS	BIT(6)
#define TAURUS_WD_RESET_REMAP		BIT(7)
#define TAURUS_WD_RESET_EXT_RESET	BIT(8)

static inline u32 taurus_wdt_readreg(struct taurus_wdt *wdt, u32 offset)
{
	return readl(wdt->regs + offset);
}

static inline void taurus_wdt_writereg(struct taurus_wdt *wdt, u32 offset,
				       u32 val)
{
	writel_relaxed(val, wdt->regs + offset);
}

static int taurus_wdt_stop(struct watchdog_device *wdd)
{
	struct taurus_wdt *wdt = watchdog_get_drvdata(wdd);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&wdt->io_lock, flags);

	val = taurus_wdt_readreg(wdt, TAURUS_WDT_CTRL);
	val &= ~TAURUS_WDT_CTRL_WDTEN;
	taurus_wdt_writereg(wdt, TAURUS_WDT_CTRL, val);

	spin_unlock_irqrestore(&wdt->io_lock, flags);

	return 0;
}

static int taurus_wdt_reload(struct watchdog_device *wdd)
{
	struct taurus_wdt *wdt = watchdog_get_drvdata(wdd);
	unsigned long flags;

	spin_lock_irqsave(&wdt->io_lock, flags);

	taurus_wdt_writereg(wdt, TAURUS_WDT_LOADE,
			    TAURUS_WDT_LOADE_WDT | TAURUS_WDT_LOADE_PRE);

	spin_unlock_irqrestore(&wdt->io_lock, flags);

	return 0;
}

static int taurus_wdt_start(struct watchdog_device *wdd)
{
	struct taurus_wdt *wdt = watchdog_get_drvdata(wdd);
	unsigned long flags;
	unsigned int data = 0;
	unsigned short count;
	unsigned int clock_rate, delay_cycle;

	count = high_resolution ? wdd->timeout * 1000 : wdd->timeout;
	clock_rate = wdt->clk_rate;

	spin_lock_irqsave(&wdt->io_lock, flags);

	taurus_wdt_writereg(wdt, TAURUS_WDT_PS, wdt->prescaler);
	taurus_wdt_writereg(wdt, TAURUS_WDT_DIV, wdt->ps_div);
	taurus_wdt_writereg(wdt, TAURUS_WDT_LD, count);
	taurus_wdt_writereg(wdt, TAURUS_WDT_LOADE,
			    TAURUS_WDT_LOADE_WDT | TAURUS_WDT_LOADE_PRE);
	taurus_wdt_writereg(wdt, TAURUS_WDT_IE_0, TAURUS_WDT_IE_WDTE);

	if (wdt->rst)
		data |= TAURUS_WDT_CTRL_RSTEN;
	if (wdt->ctrl_clksel)
		data |= TAURUS_WDT_CTRL_CLKSEL;
	data |= TAURUS_WDT_CTRL_WDTEN;
	if (wdt->delay_reset) {
		unsigned int delay_ext2 = 0, tmp;

		if (wdt->delay_type == 1) {
			delay_cycle = wdt->delay_reset * (clock_rate / 1000);
			data |= (delay_cycle & TAURUS_WDT_CTRL_MAX_DELAY) <<
				TAURUS_WDT_CTRL_DELAY_SHIFT;
			if (delay_cycle >> TAURUS_WDT_TST_DELAY_EX_SHIFT)
				delay_cycle = delay_cycle >>
					      TAURUS_WDT_TST_DELAY_EX_SHIFT;
			else
				delay_cycle = 0;

			delay_ext2 = (taurus_wdt_readreg(wdt, TAURUS_WDT_TST) >>
				      TAURUS_WDT_TST_DELAY_EX2_SHIFT) &
				     TAURUS_WDT_TST_DELAY_EX2_MASK;
			if (delay_ext2 == 0)
				delay_ext2 = 0xff;

			tmp = (delay_cycle << TAURUS_WDT_TST_DELAY_EX_SHIFT) |
			      (delay_ext2 << TAURUS_WDT_TST_DELAY_EX2_SHIFT);
			taurus_wdt_writereg(wdt, TAURUS_WDT_TST, tmp);
		} else {
			delay_cycle = wdt->delay_reset * (clock_rate / 16000);
			if (delay_cycle >= TAURUS_WDT_CTRL_MAX_DELAY)
				delay_cycle = TAURUS_WDT_CTRL_MAX_DELAY;
			data |= delay_cycle << TAURUS_WDT_CTRL_DELAY_SHIFT;
		}
	}

	taurus_wdt_writereg(wdt, TAURUS_WDT_CTRL, data);

	spin_unlock_irqrestore(&wdt->io_lock, flags);

	return 0;
}

static int taurus_wdt_settimeout(struct watchdog_device *wdd, unsigned int new_time)
{
	wdd->timeout = new_time;

	return taurus_wdt_start(wdd);
}

static unsigned int taurus_wdt_gettimeleft(struct watchdog_device *wdd)
{
	struct taurus_wdt *wdt = watchdog_get_drvdata(wdd);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&wdt->io_lock, flags);

	val = taurus_wdt_readreg(wdt, TAURUS_WDT_CNT);

	spin_unlock_irqrestore(&wdt->io_lock, flags);

	return high_resolution ? val / 1000 : val;
}

static irqreturn_t taurus_wdt_irq_handler(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct taurus_wdt *wdt = platform_get_drvdata(pdev);
	unsigned long flags;

	spin_lock_irqsave(&wdt->io_lock, flags);

	taurus_wdt_writereg(wdt, TAURUS_WDT_INT_0, TAURUS_WDT_INT_WDTI);
	taurus_wdt_writereg(wdt, TAURUS_WDT_IE_0, 0);

	spin_unlock_irqrestore(&wdt->io_lock, flags);

	dev_info(&pdev->dev, "watchdog timed out, internal reset not enabled\n");

	return IRQ_HANDLED;
}

static const struct watchdog_info taurus_wdt_info = {
	.identity	= "taurus watchdog",
	.options	= WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops taurus_wdt_ops = {
	.owner = THIS_MODULE,
	.start = taurus_wdt_start,
	.stop = taurus_wdt_stop,
	.ping = taurus_wdt_reload,
	.set_timeout = taurus_wdt_settimeout,
	.get_timeleft = taurus_wdt_gettimeleft,
};

static int taurus_wdt_probe(struct platform_device *pdev)
{
	struct resource *res;
	int ret, irq;
	unsigned long clock_f;
	struct taurus_wdt *wdt;
	struct watchdog_device *wdt_device;

	wdt = devm_kzalloc(&pdev->dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt_device = &wdt->wdt_device;
	wdt_device->info = &taurus_wdt_info;
	wdt_device->ops = &taurus_wdt_ops;
	wdt_device->timeout = TAURUS_WDT_DEFAULT_TIMEOUT;
	wdt_device->min_timeout = TAURUS_WDT_MIN_TIMEOUT;
	if (high_resolution)
		wdt_device->max_timeout = U32_MAX / 1000;
	else
		wdt_device->max_timeout = U32_MAX;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	wdt->regs = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!wdt->regs)
		return -ENOMEM;

	ret = of_property_read_u32(pdev->dev.of_node, "delay-reset",
				   &wdt->delay_reset);
	if (ret != 0)
		wdt->delay_reset = 0;

	ret = of_property_read_u8(pdev->dev.of_node, "delay-type",
				  &wdt->delay_type);
	if (ret != 0)
		wdt->delay_type = 0;

	wdt->rst = of_property_read_bool(pdev->dev.of_node, "reset-on-timeout");

	if (wdt->rst) {
		u32 val;

		res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		if (!res)
			return -EINVAL;
		wdt->rst_reg = devm_ioremap(&pdev->dev, res->start,
					    resource_size(res));
		if (!wdt->rst_reg)
			return -ENOMEM;

		val = readl(wdt->rst_reg);
		val |= TAURUS_WD_RESET_SUBSYS_ENABLE | TAURUS_WD_RESET_ALL_BLOCKS |
		       TAURUS_WD_RESET_REMAP | TAURUS_WD_RESET_EXT_RESET |
		       TAURUS_WD_RESET_PCIE;
		writel_relaxed(val, wdt->rst_reg);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	if (res) {
		void __iomem *tmp_reg;

		tmp_reg = devm_ioremap(&pdev->dev, res->start, resource_size(res));
		if (!tmp_reg)
			return -ENOMEM;

		writel_relaxed(0x1, tmp_reg);
		devm_iounmap(&pdev->dev, tmp_reg);
	}

	wdt_device->parent = &pdev->dev;

	ret = watchdog_init_timeout(wdt_device, wdt_timeout, &pdev->dev);
	if (ret)
		return ret;

	watchdog_set_nowayout(wdt_device, nowayout);
	watchdog_set_drvdata(wdt_device, wdt);

	wdt->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(wdt->clk))
		return PTR_ERR(wdt->clk);

	clock_f = clk_get_rate(wdt->clk);
	wdt->clk_rate = clock_f;
	wdt->prescaler = clock_f / 1000 - 1;
	if (high_resolution) {
		wdt->ps_div = 0;
		wdt->ctrl_clksel = 1;
	} else {
		wdt->ps_div = 1000;
		wdt->ctrl_clksel = 0;
	}

	spin_lock_init(&wdt->io_lock);
	platform_set_drvdata(pdev, wdt);

	irq = platform_get_irq_optional(pdev, 0);
	wdt->irq = irq;
	if (irq >= 0) {
		ret = devm_request_irq(&pdev->dev, irq, taurus_wdt_irq_handler,
				       0, pdev->name, pdev);
		if (ret) {
			dev_err(&pdev->dev,
				"cannot register interrupt handler err=%d\n",
				ret);
			return ret;
		}
	}

	ret = watchdog_register_device(wdt_device);
	if (ret)
		return ret;

	return 0;
}

static void taurus_wdt_remove(struct platform_device *pdev)
{
	struct taurus_wdt *wdt = platform_get_drvdata(pdev);
	unsigned long flags;

	taurus_wdt_stop(&wdt->wdt_device);
	spin_lock_irqsave(&wdt->io_lock, flags);
	taurus_wdt_writereg(wdt, TAURUS_WDT_IE_0, 0);
	spin_unlock_irqrestore(&wdt->io_lock, flags);
	if (wdt->irq >= 0)
		devm_free_irq(&pdev->dev, wdt->irq, pdev);
	watchdog_unregister_device(&wdt->wdt_device);
}

static int __maybe_unused taurus_wdt_suspend(struct device *dev)
{
	struct taurus_wdt *wdt = dev_get_drvdata(dev);

	taurus_wdt_stop(&wdt->wdt_device);
	clk_disable_unprepare(wdt->clk);

	return 0;
}

static int __maybe_unused taurus_wdt_resume(struct device *dev)
{
	struct taurus_wdt *wdt = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(wdt->clk);
	if (ret)
		return ret;
	taurus_wdt_start(&wdt->wdt_device);

	return 0;
}

static SIMPLE_DEV_PM_OPS(taurus_wdt_pm_ops, taurus_wdt_suspend,
			 taurus_wdt_resume);

static const struct of_device_id taurus_wdt_of_match[] = {
	{ .compatible = "realtek,taurus-wdt", },
	{ }
};
MODULE_DEVICE_TABLE(of, taurus_wdt_of_match);

static struct platform_driver taurus_wdt_driver = {
	.probe = taurus_wdt_probe,
	.remove = taurus_wdt_remove,
	.driver = {
		.name = "taurus-wdt",
		.of_match_table = taurus_wdt_of_match,
		.pm = &taurus_wdt_pm_ops,
	},
};
module_platform_driver(taurus_wdt_driver);

MODULE_DESCRIPTION("Realtek Taurus peripheral watchdog driver");
MODULE_LICENSE("GPL");
