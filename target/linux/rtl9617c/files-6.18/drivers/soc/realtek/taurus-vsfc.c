// SPDX-License-Identifier: GPL-2.0
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/io.h>
#include <linux/kstrtox.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include <linux/soc/realtek/taurus-vsfc.h>

#define TAURUS_VSFC_REG_GAINN		0x00
#define TAURUS_VSFC_REG_GAINP		0x04
#define TAURUS_VSFC_REG_CONTROL		0x08
#define TAURUS_VSFC_REG_TRIG_CONTROL	0x0c
#define TAURUS_VSFC_REG_RECV_CTRL0	0x10
#define TAURUS_VSFC_REG_RECV_CTRL1	0x14
#define TAURUS_VSFC_REG_RECV_CTRL2	0x18
#define TAURUS_VSFC_REG_RECV_CTRL3	0x1c
#define TAURUS_VSFC_REG_RECV_CTRL4	0x20
#define TAURUS_VSFC_REG_STATUS		0x2c
#define TAURUS_VSFC_REG_PORF_ST0	0x30

#define TAURUS_VSFC_CTRL_GLB_EN		BIT(0)
#define TAURUS_VSFC_CTRL_DIV_SEL	BIT(4)
#define TAURUS_VSFC_CTRL_TRIG_SEL	GENMASK(10, 8)

#define TAURUS_VSFC_TRIG_UVLO_MIN	GENMASK(2, 0)
#define TAURUS_VSFC_TRIG_FSS_MIN	GENMASK(6, 4)
#define TAURUS_VSFC_TRIG_CORE_MIN	GENMASK(9, 8)
#define TAURUS_VSFC_TRIG_CORE_IDLE_DIV_EN	BIT(16)

#define TAURUS_VSFC_RECV0_DET_RECV_SEL	GENMASK(1, 0)

#define TAURUS_VSFC_GAINP_POW		GENMASK(29, 25)

#define TAURUS_VSFC_STATUS_IDLE_ST	BIT(0)

#define TAURUS_VSFC_PORF_ST0_TRIG_CNT	GENMASK(11, 0)
#define TAURUS_VSFC_PORF_ST0_OVFL	BIT(31)

#define TAURUS_VSFC_TRIG_SEL_CPU_INTERNAL	BIT(0)
#define TAURUS_VSFC_TRIG_SEL_UVLO		BIT(1)
#define TAURUS_VSFC_TRIG_SEL_SPEED_SENESOR	BIT(2)

struct realtek_vsfc {
	struct platform_device *pdev;
	void __iomem *regs;
	u32 gainn;
	u32 gainp;
	u32 ctrl;
	u32 trig_ctrl;
	u32 recv_ctrl0;
	u32 recv_ctrl1;
	u32 recv_ctrl2;
	u32 recv_ctrl3;
	u32 recv_ctrl4;
};

static DEFINE_MUTEX(taurus_vsfc_lock);
static struct realtek_vsfc *glb_vsfc;

static void taurus_vsfc_writel_2t(struct realtek_vsfc *vsfc, u32 off, u32 val)
{
	writel(val, vsfc->regs + off);
	writel(val, vsfc->regs + off);
}

static void taurus_vsfc_disable(struct realtek_vsfc *vsfc)
{
	ktime_t timeout;
	u32 ctrl, status;

	ctrl = readl(vsfc->regs + TAURUS_VSFC_REG_CONTROL);
	ctrl &= ~TAURUS_VSFC_CTRL_GLB_EN;
	taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_CONTROL, ctrl);

	timeout = ktime_add_ms(ktime_get(), 100);
	do {
		status = readl(vsfc->regs + TAURUS_VSFC_REG_STATUS);
		/* Ensure status read completes before polling idle bit */
		rmb();
	} while (!(status & TAURUS_VSFC_STATUS_IDLE_ST) &&
		 ktime_before(ktime_get(), timeout));
}

static int taurus_vsfc_init(struct realtek_vsfc *vsfc)
{
	u32 ctrl, trig_ctrl;

	taurus_vsfc_disable(vsfc);
	ctrl = readl(vsfc->regs + TAURUS_VSFC_REG_CONTROL);

	switch (FIELD_GET(TAURUS_VSFC_CTRL_TRIG_SEL, vsfc->ctrl)) {
	case TAURUS_VSFC_TRIG_SEL_CPU_INTERNAL:
		ctrl &= ~TAURUS_VSFC_CTRL_TRIG_SEL;
		ctrl |= vsfc->ctrl & TAURUS_VSFC_CTRL_TRIG_SEL;
		ctrl &= ~TAURUS_VSFC_CTRL_DIV_SEL;
		ctrl |= vsfc->ctrl & TAURUS_VSFC_CTRL_DIV_SEL;

		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_CONTROL, ctrl);

		trig_ctrl = readl(vsfc->regs + TAURUS_VSFC_REG_TRIG_CONTROL);
		trig_ctrl &= ~(TAURUS_VSFC_TRIG_CORE_MIN |
			       TAURUS_VSFC_TRIG_CORE_IDLE_DIV_EN);
		trig_ctrl |= vsfc->trig_ctrl & (TAURUS_VSFC_TRIG_CORE_MIN |
						TAURUS_VSFC_TRIG_CORE_IDLE_DIV_EN);
		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_TRIG_CONTROL,
				      trig_ctrl);

		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_RECV_CTRL0,
				      vsfc->recv_ctrl0);
		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_RECV_CTRL1,
				      vsfc->recv_ctrl1);
		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_RECV_CTRL2,
				      vsfc->recv_ctrl2);
		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_RECV_CTRL3,
				      vsfc->recv_ctrl3);
		/* Commit recovery parameters before setting global enable */
		wmb();

		ctrl |= TAURUS_VSFC_CTRL_GLB_EN;
		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_CONTROL, ctrl);
		break;
	case TAURUS_VSFC_TRIG_SEL_UVLO:
	case TAURUS_VSFC_TRIG_SEL_SPEED_SENESOR:
		if (vsfc->gainn & GENMASK(24, 0))
			taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_GAINN,
					      vsfc->gainn);
		if (vsfc->gainp & GENMASK(24, 0)) {
			vsfc->gainp &= ~TAURUS_VSFC_GAINP_POW;
			vsfc->gainp |= 0x1f << 25;
			taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_GAINP,
					      vsfc->gainp);
		}
		ctrl &= ~TAURUS_VSFC_CTRL_TRIG_SEL;
		ctrl |= vsfc->ctrl & TAURUS_VSFC_CTRL_TRIG_SEL;
		ctrl &= ~TAURUS_VSFC_CTRL_DIV_SEL;
		ctrl |= vsfc->ctrl & TAURUS_VSFC_CTRL_DIV_SEL;

		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_CONTROL, ctrl);
		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_TRIG_CONTROL,
				      vsfc->trig_ctrl);
		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_RECV_CTRL0,
				      vsfc->recv_ctrl0);
		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_RECV_CTRL1,
				      vsfc->recv_ctrl1);
		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_RECV_CTRL2,
				      vsfc->recv_ctrl2);
		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_RECV_CTRL3,
				      vsfc->recv_ctrl3);
		/* Commit recovery parameters before setting global enable */
		wmb();
		ctrl |= TAURUS_VSFC_CTRL_GLB_EN;
		taurus_vsfc_writel_2t(vsfc, TAURUS_VSFC_REG_CONTROL, ctrl);
		break;
	default:
		break;
	}
	return 0;
}

static ssize_t enabled_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct realtek_vsfc *vsfc = dev_get_drvdata(dev);
	u32 ctrl = readl(vsfc->regs + TAURUS_VSFC_REG_CONTROL);

	return sysfs_emit(buf, "%u\n", !!(ctrl & TAURUS_VSFC_CTRL_GLB_EN));
}

static ssize_t enabled_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct realtek_vsfc *vsfc = dev_get_drvdata(dev);
	bool en;
	int ret;

	ret = kstrtobool(buf, &en);
	if (ret)
		return ret;

	mutex_lock(&taurus_vsfc_lock);
	if (en)
		taurus_vsfc_init(vsfc);
	else
		taurus_vsfc_disable(vsfc);
	mutex_unlock(&taurus_vsfc_lock);

	return count;
}
static DEVICE_ATTR_RW(enabled);

static ssize_t trig_cnt_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct realtek_vsfc *vsfc = dev_get_drvdata(dev);
	u32 st0 = readl(vsfc->regs + TAURUS_VSFC_REG_PORF_ST0);

	return sysfs_emit(buf, "%lu\n",
			  FIELD_GET(TAURUS_VSFC_PORF_ST0_TRIG_CNT, st0));
}
static DEVICE_ATTR_RO(trig_cnt);

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct realtek_vsfc *vsfc = dev_get_drvdata(dev);
	u32 ctrl = readl(vsfc->regs + TAURUS_VSFC_REG_CONTROL);
	u32 trig_ctrl = readl(vsfc->regs + TAURUS_VSFC_REG_TRIG_CONTROL);
	u32 status = readl(vsfc->regs + TAURUS_VSFC_REG_STATUS);
	u32 st0 = readl(vsfc->regs + TAURUS_VSFC_REG_PORF_ST0);

	return sysfs_emit(buf,
			  "enabled=%u ctrl=0x%08x trig_ctrl=0x%08x status=0x%08x idle=%u trig_cnt=%lu ovfl=%u\n",
			  !!(ctrl & TAURUS_VSFC_CTRL_GLB_EN), ctrl, trig_ctrl,
			  status, !!(status & TAURUS_VSFC_STATUS_IDLE_ST),
			  FIELD_GET(TAURUS_VSFC_PORF_ST0_TRIG_CNT, st0),
			  !!(st0 & TAURUS_VSFC_PORF_ST0_OVFL));
}
static DEVICE_ATTR_RO(status);

static struct attribute *taurus_vsfc_attrs[] = {
	&dev_attr_enabled.attr,
	&dev_attr_trig_cnt.attr,
	&dev_attr_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(taurus_vsfc);

static int taurus_vsfc_probe(struct platform_device *pdev)
{
	struct realtek_vsfc *vsfc;
	int ret;
	u32 value;

	vsfc = devm_kzalloc(&pdev->dev, sizeof(*vsfc), GFP_KERNEL);
	if (!vsfc)
		return -ENOMEM;

	vsfc->pdev = pdev;
	platform_set_drvdata(pdev, vsfc);

	vsfc->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vsfc->regs))
		return PTR_ERR(vsfc->regs);

	if (of_property_read_u32(pdev->dev.of_node, "trig_sel", &value))
		vsfc->ctrl = 0;
	else
		vsfc->ctrl = (value << 8) & TAURUS_VSFC_CTRL_TRIG_SEL;

	if (of_property_read_u32(pdev->dev.of_node, "div_sel", &value))
		vsfc->ctrl &= ~TAURUS_VSFC_CTRL_DIV_SEL;
	else if (value)
		vsfc->ctrl |= TAURUS_VSFC_CTRL_DIV_SEL;

	if (of_property_read_u32(pdev->dev.of_node, "core_idle_div_en", &value))
		vsfc->trig_ctrl = 0;
	else if (value)
		vsfc->trig_ctrl |= TAURUS_VSFC_TRIG_CORE_IDLE_DIV_EN;

	if (of_property_read_u32(pdev->dev.of_node, "core_min", &value) ||
	    value == 0)
		vsfc->trig_ctrl &= ~TAURUS_VSFC_TRIG_CORE_MIN;
	else
		vsfc->trig_ctrl |= FIELD_PREP(TAURUS_VSFC_TRIG_CORE_MIN,
					      value - 1);

	if (of_property_read_u32(pdev->dev.of_node, "fss_min", &value) ||
	    value == 0)
		vsfc->trig_ctrl &= ~TAURUS_VSFC_TRIG_FSS_MIN;
	else
		vsfc->trig_ctrl |= FIELD_PREP(TAURUS_VSFC_TRIG_FSS_MIN,
					      value - 1);

	if (of_property_read_u32(pdev->dev.of_node, "uvlo_min", &value) ||
	    value == 0)
		vsfc->trig_ctrl &= ~TAURUS_VSFC_TRIG_UVLO_MIN;
	else
		vsfc->trig_ctrl |= FIELD_PREP(TAURUS_VSFC_TRIG_UVLO_MIN,
					      value - 1);

	if (of_property_read_u32(pdev->dev.of_node, "det_recv_sel", &value))
		vsfc->recv_ctrl0 = 1 << 0;
	else if (value >= 1 && value <= 2)
		vsfc->recv_ctrl0 = value & TAURUS_VSFC_RECV0_DET_RECV_SEL;

	if (of_property_read_u32(pdev->dev.of_node, "recv_ctrl1", &value))
		vsfc->recv_ctrl1 = 0x00000010;
	else
		vsfc->recv_ctrl1 = value;
	if (of_property_read_u32(pdev->dev.of_node, "recv_ctrl2", &value))
		vsfc->recv_ctrl2 = 0x00000010;
	else
		vsfc->recv_ctrl2 = value;
	if (of_property_read_u32(pdev->dev.of_node, "recv_ctrl3", &value))
		vsfc->recv_ctrl3 = 0x00000010;
	else
		vsfc->recv_ctrl3 = value;
	if (of_property_read_u32(pdev->dev.of_node, "recv_ctrl4", &value))
		vsfc->recv_ctrl4 = 0;
	else
		vsfc->recv_ctrl4 = value;

	if (of_property_read_u32(pdev->dev.of_node, "control_gainn", &value))
		vsfc->gainn = 0;
	else
		vsfc->gainn = value;

	if (of_property_read_u32(pdev->dev.of_node, "control_gainp", &value))
		vsfc->gainp = 0;
	else
		vsfc->gainp = value;

	mutex_lock(&taurus_vsfc_lock);
	ret = taurus_vsfc_init(vsfc);
	glb_vsfc = vsfc;
	mutex_unlock(&taurus_vsfc_lock);

	dev_info(&pdev->dev, "RTK VSFC init (ctrl=0x%08x trig_ctrl=0x%08x)\n",
		 readl(vsfc->regs + TAURUS_VSFC_REG_CONTROL),
		 readl(vsfc->regs + TAURUS_VSFC_REG_TRIG_CONTROL));

	return ret;
}

bool taurus_vsfc_quiesce(void)
{
	u32 ctrl;

	mutex_lock(&taurus_vsfc_lock);
	if (!glb_vsfc)
		return false;

	ctrl = readl(glb_vsfc->regs + TAURUS_VSFC_REG_CONTROL);
	if (ctrl & TAURUS_VSFC_CTRL_GLB_EN) {
		taurus_vsfc_disable(glb_vsfc);
		return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(taurus_vsfc_quiesce);

void taurus_vsfc_resume(bool was_enabled)
{
	if (was_enabled && glb_vsfc)
		taurus_vsfc_init(glb_vsfc);
	mutex_unlock(&taurus_vsfc_lock);
}
EXPORT_SYMBOL_GPL(taurus_vsfc_resume);

static const struct of_device_id taurus_vsfc_of_match[] = {
	{ .compatible = "realtek,taurus-vsfc", },
	{ }
};
MODULE_DEVICE_TABLE(of, taurus_vsfc_of_match);

static struct platform_driver taurus_vsfc_driver = {
	.probe = taurus_vsfc_probe,
	.driver = {
		.name = "taurus-vsfc",
		.of_match_table = taurus_vsfc_of_match,
		.dev_groups = taurus_vsfc_groups,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(taurus_vsfc_driver);

MODULE_DESCRIPTION("Realtek Taurus VSFC driver");
MODULE_LICENSE("GPL");
