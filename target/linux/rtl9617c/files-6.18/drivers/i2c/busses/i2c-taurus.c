// SPDX-License-Identifier: GPL-2.0
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define TAURUS_BIW_CFG		0x00
#define TAURUS_BIW_CTRL		0x04
#define TAURUS_BIW_TXR		0x08
#define TAURUS_BIW_RXR		0x0c
#define TAURUS_BIW_ACK		0x10
#define TAURUS_BIW_IE0		0x14
#define TAURUS_BIW_INT0		0x18
#define TAURUS_BIW_IE1		0x1c
#define TAURUS_BIW_INT1		0x20
#define TAURUS_BIW_STAT		0x24

#define TAURUS_BIW_CFG_CORE_EN		BIT(0)
#define TAURUS_BIW_CFG_SOFT_RESET	BIT(1)
#define TAURUS_BIW_CFG_PRER_SHIFT	16
#define TAURUS_BIW_CFG_PRER_MASK	0xffff0000

#define TAURUS_BIW_CTRL_DONE		BIT(0)
#define TAURUS_BIW_CTRL_ACK_IN		BIT(3)
#define TAURUS_BIW_CTRL_WRITE		BIT(4)
#define TAURUS_BIW_CTRL_READ		BIT(5)
#define TAURUS_BIW_CTRL_STOP		BIT(6)
#define TAURUS_BIW_CTRL_START		BIT(7)

#define TAURUS_BIW_TXR_MASK		0x000000ff
#define TAURUS_BIW_RXR_MASK		0x000000ff

#define TAURUS_BIW_ACK_AL		BIT(0)
#define TAURUS_BIW_ACK_BUSY		BIT(1)
#define TAURUS_BIW_ACK_ACK_OUT		BIT(2)

#define TAURUS_BIW_IE_BIT		BIT(0)
#define TAURUS_BIW_INT_BIT		BIT(0)
#define TAURUS_BIW_STAT_BIT		BIT(0)

enum {
	TAURUS_I2C_STATE_INVALID,
	TAURUS_I2C_STATE_IDLE,
	TAURUS_I2C_STATE_START,
	TAURUS_I2C_STATE_WAITING_FOR_ADDR_1_ACK,
	TAURUS_I2C_STATE_WAITING_FOR_ADDR_2_ACK,
	TAURUS_I2C_STATE_WAITING_FOR_WRITE_ACK,
	TAURUS_I2C_STATE_WAITING_FOR_READ_ACK,
};

struct taurus_i2c_data {
	int			irq;
	u32			state;
	u32			aborting;
	void __iomem		*reg_base;
	u32			addr1;
	u32			addr2;
	u32			bytes_left;
	u32			byte_posn;
	u32			block;
	int			rc;
	u32			ref_freq;
	u32			bus_freq;
	u32			ack;
	struct clk		*clk;
	wait_queue_head_t	waitq;
	spinlock_t		lock;
	struct i2c_msg		*msg;
	struct i2c_adapter	adapter;
	struct pinctrl			*pinctrl;
	struct pinctrl_state		*pins_default;
	struct pinctrl_state		*pins_gpio;
	struct i2c_bus_recovery_info	recovery;
};

#define TAURUS_I2C_CTLR_NAME	"taurus-i2c"

#define TAURUS_I2C_DEFAULT_BUS_FREQ	100000
#define TAURUS_I2C_FM_BUS_FREQ		400000
#define TAURUS_I2C_HSM_BUS_FREQ		3400000
#define TAURUS_I2C_DEFAULT_TIMEOUT	1000
#define TAURUS_I2C_DEFAULT_RETRIES	3

#define TAURUS_I2C_M_NOSTOP	0x1000

#define TAURUS_I2C_RECOVERY_US		5
#define TAURUS_I2C_RECOVERY_CLOCKS	9

static inline u32 taurus_rd_reg(struct taurus_i2c_data *drv_data, unsigned reg)
{
	return readl(drv_data->reg_base + reg);
}

static inline void taurus_wr_reg(struct taurus_i2c_data *drv_data, unsigned reg,
				 u32 val)
{
	writel(val, drv_data->reg_base + reg);
}

static void taurus_dump_regs(struct taurus_i2c_data *drv_data)
{
	struct device *dev = &drv_data->adapter.dev;

	dev_info(dev, "0x%02x: BIW_CFG = 0x%x\n", TAURUS_BIW_CFG,
		 taurus_rd_reg(drv_data, TAURUS_BIW_CFG));
	dev_info(dev, "0x%02x: BIW_CTRL = 0x%x\n", TAURUS_BIW_CTRL,
		 taurus_rd_reg(drv_data, TAURUS_BIW_CTRL));
	dev_info(dev, "0x%02x: BIW_TXR = 0x%x\n", TAURUS_BIW_TXR,
		 taurus_rd_reg(drv_data, TAURUS_BIW_TXR));
	dev_info(dev, "0x%02x: BIW_RXR = 0x%x\n", TAURUS_BIW_RXR,
		 taurus_rd_reg(drv_data, TAURUS_BIW_RXR));
	dev_info(dev, "0x%02x: BIW_ACK = 0x%x\n", TAURUS_BIW_ACK,
		 taurus_rd_reg(drv_data, TAURUS_BIW_ACK));
	dev_info(dev, "0x%02x: BIW_IE0 = 0x%x\n", TAURUS_BIW_IE0,
		 taurus_rd_reg(drv_data, TAURUS_BIW_IE0));
	dev_info(dev, "0x%02x: BIW_INT0 = 0x%x\n", TAURUS_BIW_INT0,
		 taurus_rd_reg(drv_data, TAURUS_BIW_INT0));
	dev_info(dev, "0x%02x: BIW_IE1 = 0x%x\n", TAURUS_BIW_IE1,
		 taurus_rd_reg(drv_data, TAURUS_BIW_IE1));
	dev_info(dev, "0x%02x: BIW_INT1 = 0x%x\n", TAURUS_BIW_INT1,
		 taurus_rd_reg(drv_data, TAURUS_BIW_INT1));
	dev_info(dev, "0x%02x: BIW_STAT = 0x%x\n", TAURUS_BIW_STAT,
		 taurus_rd_reg(drv_data, TAURUS_BIW_STAT));
}

static void taurus_i2c_hw_fini(struct taurus_i2c_data *drv_data)
{
	taurus_wr_reg(drv_data, TAURUS_BIW_IE0, 0);
	taurus_wr_reg(drv_data, TAURUS_BIW_CFG, 0);
	drv_data->state = TAURUS_I2C_STATE_INVALID;
}

static void taurus_i2c_hw_init(struct taurus_i2c_data *drv_data)
{
	u32 reg_biw_cfg;

	reg_biw_cfg = taurus_rd_reg(drv_data, TAURUS_BIW_CFG);

	if (reg_biw_cfg & TAURUS_BIW_CFG_CORE_EN) {
		reg_biw_cfg |= TAURUS_BIW_CFG_SOFT_RESET;
		taurus_wr_reg(drv_data, TAURUS_BIW_CFG, reg_biw_cfg);

		mdelay(50);

		reg_biw_cfg &= ~TAURUS_BIW_CFG_SOFT_RESET;
		taurus_wr_reg(drv_data, TAURUS_BIW_CFG, reg_biw_cfg);
	}

	reg_biw_cfg &= ~TAURUS_BIW_CFG_PRER_MASK;
	reg_biw_cfg |= (drv_data->ref_freq / (5 * drv_data->bus_freq) - 1) <<
		       TAURUS_BIW_CFG_PRER_SHIFT;

	reg_biw_cfg |= TAURUS_BIW_CFG_CORE_EN;
	taurus_wr_reg(drv_data, TAURUS_BIW_CFG, reg_biw_cfg);

	mdelay(50);

	taurus_wr_reg(drv_data, TAURUS_BIW_IE0, 1);

	drv_data->state = TAURUS_I2C_STATE_IDLE;
}

static void taurus_i2c_fsm(struct taurus_i2c_data *drv_data)
{
	struct i2c_msg *msg = drv_data->msg;

	if (!msg || drv_data->state == TAURUS_I2C_STATE_IDLE)
		return;

	if ((drv_data->bytes_left != 0) &&
	    ((drv_data->ack & TAURUS_BIW_ACK_ACK_OUT) ||
	     (drv_data->ack & TAURUS_BIW_ACK_AL))) {
		if (drv_data->ack & TAURUS_BIW_ACK_AL)
			drv_data->rc = -EAGAIN;
		else
			drv_data->rc = -ENXIO;
		drv_data->state = TAURUS_I2C_STATE_INVALID;
	}

	switch (drv_data->state) {
	case TAURUS_I2C_STATE_START:
		drv_data->state = TAURUS_I2C_STATE_WAITING_FOR_ADDR_1_ACK;

		taurus_wr_reg(drv_data, TAURUS_BIW_TXR, drv_data->addr1);
		taurus_wr_reg(drv_data, TAURUS_BIW_CTRL,
			      TAURUS_BIW_CTRL_START | TAURUS_BIW_CTRL_WRITE);
		break;

	case TAURUS_I2C_STATE_WAITING_FOR_ADDR_1_ACK:
		if ((msg->flags & I2C_M_TEN) && !(msg->flags & I2C_M_NOSTART)) {
			drv_data->state = TAURUS_I2C_STATE_WAITING_FOR_ADDR_2_ACK;

			taurus_wr_reg(drv_data, TAURUS_BIW_TXR, drv_data->addr2);
			taurus_wr_reg(drv_data, TAURUS_BIW_CTRL,
				      TAURUS_BIW_CTRL_WRITE);
			break;
		}
		fallthrough;
	case TAURUS_I2C_STATE_WAITING_FOR_ADDR_2_ACK:
		if (msg->flags & I2C_M_RD) {
			drv_data->state = TAURUS_I2C_STATE_WAITING_FOR_READ_ACK;

			drv_data->bytes_left--;

			if (drv_data->bytes_left ||
			    (msg->flags & TAURUS_I2C_M_NOSTOP))
				taurus_wr_reg(drv_data, TAURUS_BIW_CTRL,
					      TAURUS_BIW_CTRL_READ);
			else
				taurus_wr_reg(drv_data, TAURUS_BIW_CTRL,
					      TAURUS_BIW_CTRL_READ |
					      TAURUS_BIW_CTRL_ACK_IN |
					      TAURUS_BIW_CTRL_STOP);
		} else {
			drv_data->state = TAURUS_I2C_STATE_WAITING_FOR_WRITE_ACK;

			taurus_wr_reg(drv_data, TAURUS_BIW_TXR,
				      msg->buf[drv_data->byte_posn++]);
			drv_data->bytes_left--;

			if (drv_data->bytes_left ||
			    (msg->flags & TAURUS_I2C_M_NOSTOP))
				taurus_wr_reg(drv_data, TAURUS_BIW_CTRL,
					      TAURUS_BIW_CTRL_WRITE);
			else
				taurus_wr_reg(drv_data, TAURUS_BIW_CTRL,
					      TAURUS_BIW_CTRL_WRITE |
					      TAURUS_BIW_CTRL_STOP);
		}
		break;

	case TAURUS_I2C_STATE_WAITING_FOR_WRITE_ACK:
		if (drv_data->bytes_left) {
			taurus_wr_reg(drv_data, TAURUS_BIW_TXR,
				      msg->buf[drv_data->byte_posn++]);
			drv_data->bytes_left--;

			if (drv_data->bytes_left ||
			    (msg->flags & TAURUS_I2C_M_NOSTOP))
				taurus_wr_reg(drv_data, TAURUS_BIW_CTRL,
					      TAURUS_BIW_CTRL_WRITE);
			else
				taurus_wr_reg(drv_data, TAURUS_BIW_CTRL,
					      TAURUS_BIW_CTRL_WRITE |
					      TAURUS_BIW_CTRL_STOP);
		} else {
			drv_data->msg = NULL;
			drv_data->state = TAURUS_I2C_STATE_IDLE;
			drv_data->block = 0;
			wake_up(&drv_data->waitq);
		}
		break;

	case TAURUS_I2C_STATE_WAITING_FOR_READ_ACK:
		msg->buf[drv_data->byte_posn++] =
			taurus_rd_reg(drv_data, TAURUS_BIW_RXR);

		if (drv_data->bytes_left) {
			drv_data->bytes_left--;

			if (drv_data->bytes_left)
				taurus_wr_reg(drv_data, TAURUS_BIW_CTRL,
					      TAURUS_BIW_CTRL_READ);
			else
				taurus_wr_reg(drv_data, TAURUS_BIW_CTRL,
					      TAURUS_BIW_CTRL_READ |
					      TAURUS_BIW_CTRL_ACK_IN |
					      TAURUS_BIW_CTRL_STOP);
		} else {
			drv_data->msg = NULL;
			drv_data->state = TAURUS_I2C_STATE_IDLE;
			drv_data->block = 0;
			wake_up(&drv_data->waitq);
		}
		break;

	case TAURUS_I2C_STATE_INVALID:
	default:
		if (!drv_data->rc)
			drv_data->rc = -EIO;

		drv_data->msg = NULL;
		drv_data->state = TAURUS_I2C_STATE_IDLE;

		drv_data->block = 0;
		wake_up(&drv_data->waitq);
	}
}

static irqreturn_t taurus_i2c_intr(int irq, void *param)
{
	struct taurus_i2c_data *drv_data = param;
	unsigned long flags;
	u32 reg_biw_ctrl;
	u32 reg_biw_int0;

	spin_lock_irqsave(&drv_data->lock, flags);

	taurus_wr_reg(drv_data, TAURUS_BIW_IE0, 0);

	reg_biw_int0 = taurus_rd_reg(drv_data, TAURUS_BIW_INT0);
	reg_biw_int0 |= TAURUS_BIW_INT_BIT;
	taurus_wr_reg(drv_data, TAURUS_BIW_INT0, reg_biw_int0);

	reg_biw_ctrl = taurus_rd_reg(drv_data, TAURUS_BIW_CTRL);
	if (!(reg_biw_ctrl & TAURUS_BIW_CTRL_DONE))
		dev_err_ratelimited(&drv_data->adapter.dev,
				    "taurus-i2c: no done signal\n");
	taurus_wr_reg(drv_data, TAURUS_BIW_CTRL, reg_biw_ctrl);

	drv_data->ack = taurus_rd_reg(drv_data, TAURUS_BIW_ACK);

	taurus_i2c_fsm(drv_data);

	taurus_wr_reg(drv_data, TAURUS_BIW_IE0, 1);

	spin_unlock_irqrestore(&drv_data->lock, flags);

	return IRQ_HANDLED;
}

static void taurus_i2c_prepare_for_io(struct taurus_i2c_data *drv_data,
				      struct i2c_msg *msg)
{
	u32 dir = 0;

	drv_data->msg = msg;
	drv_data->byte_posn = 0;
	drv_data->bytes_left = msg->len;
	drv_data->aborting = 0;
	drv_data->rc = 0;
	drv_data->ack = 0;

	if (msg->flags & I2C_M_RD)
		dir = 1;

	if (msg->flags & I2C_M_TEN) {
		drv_data->addr1 = 0xf0 | (((u32)msg->addr & 0x300) >> 7) | dir;
		drv_data->addr2 = (u32)msg->addr & 0xff;
	} else {
		drv_data->addr1 = ((u32)msg->addr & 0x7f) << 1 | dir;
		drv_data->addr2 = 0;
	}
}

static void taurus_i2c_wait_for_completion(struct taurus_i2c_data *drv_data)
{
	unsigned long time_left, flags;

	time_left = wait_event_timeout(drv_data->waitq, !drv_data->block,
				       drv_data->adapter.timeout);

	if (!time_left) {
		spin_lock_irqsave(&drv_data->lock, flags);
		taurus_wr_reg(drv_data, TAURUS_BIW_IE0, 0);
		taurus_dump_regs(drv_data);
		drv_data->rc = -ETIMEDOUT;
		drv_data->msg = NULL;
		drv_data->state = TAURUS_I2C_STATE_IDLE;
		dev_err(&drv_data->adapter.dev,
			"taurus-i2c: I2C bus locked, block: %d, time_left: %d\n",
			drv_data->block, (int)time_left);
		drv_data->block = 0;
		spin_unlock_irqrestore(&drv_data->lock, flags);

		synchronize_irq(drv_data->irq);

		if (drv_data->adapter.bus_recovery_info)
			i2c_recover_bus(&drv_data->adapter);

		taurus_i2c_hw_init(drv_data);
	}
}

static int taurus_i2c_execute_msg(struct taurus_i2c_data *drv_data,
				  struct i2c_msg *msg)
{
	unsigned long flags;
	int rc;

	if (msg->len == 0)
		return -EINVAL;

	spin_lock_irqsave(&drv_data->lock, flags);

	taurus_i2c_prepare_for_io(drv_data, msg);

	if (unlikely(msg->flags & I2C_M_NOSTART))
		drv_data->state = TAURUS_I2C_STATE_WAITING_FOR_ADDR_2_ACK;
	else
		drv_data->state = TAURUS_I2C_STATE_START;

	drv_data->block = 1;
	taurus_i2c_fsm(drv_data);

	spin_unlock_irqrestore(&drv_data->lock, flags);

	taurus_i2c_wait_for_completion(drv_data);

	spin_lock_irqsave(&drv_data->lock, flags);
	drv_data->msg = NULL;
	rc = drv_data->rc;
	spin_unlock_irqrestore(&drv_data->lock, flags);

	return rc;
}

static u32 taurus_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_10BIT_ADDR | I2C_FUNC_SMBUS_EMUL;
}

static int taurus_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[],
			   int num)
{
	struct taurus_i2c_data *drv_data = i2c_get_adapdata(adap);
	int i, rc;

	for (i = 0; i < num; i++) {
		if (i < (num - 1))
			msgs[i].flags |= TAURUS_I2C_M_NOSTOP;

		rc = taurus_i2c_execute_msg(drv_data, &msgs[i]);
		if (rc)
			return rc;
	}

	return num;
}

static void taurus_i2c_scl_set(struct gpio_desc *scl, int val)
{
	if (val)
		gpiod_direction_input(scl);
	else
		gpiod_direction_output(scl, 0);
}

static int taurus_i2c_recover_bus(struct i2c_adapter *adap)
{
	struct taurus_i2c_data *drv_data = i2c_get_adapdata(adap);
	struct device *dev = drv_data->adapter.dev.parent;
	struct gpio_desc *scl, *sda = NULL;
	int i, ret, restore_ret;

	ret = pinctrl_select_state(drv_data->pinctrl, drv_data->pins_gpio);
	if (ret)
		return ret;

	scl = gpiod_get_optional(dev, "scl", GPIOD_IN);
	if (IS_ERR(scl)) {
		ret = PTR_ERR(scl);
		goto restore;
	}
	if (!scl) {
		ret = -EOPNOTSUPP;
		goto restore;
	}

	sda = gpiod_get_optional(dev, "sda", GPIOD_IN);
	if (IS_ERR(sda))
		sda = NULL;

	for (i = 0; i < TAURUS_I2C_RECOVERY_CLOCKS; i++) {
		taurus_i2c_scl_set(scl, 0);
		udelay(TAURUS_I2C_RECOVERY_US);
		taurus_i2c_scl_set(scl, 1);
		udelay(TAURUS_I2C_RECOVERY_US);
	}

	if (!gpiod_get_value_cansleep(scl)) {
		ret = -EBUSY;
		goto put_sda;
	}

	if (sda) {
		taurus_i2c_scl_set(scl, 0);
		gpiod_direction_output(sda, 0);
		udelay(TAURUS_I2C_RECOVERY_US);
		taurus_i2c_scl_set(scl, 1);
		udelay(TAURUS_I2C_RECOVERY_US);
		gpiod_direction_input(sda);
		udelay(TAURUS_I2C_RECOVERY_US);
	}
	ret = 0;

put_sda:
	if (sda)
		gpiod_put(sda);
	gpiod_put(scl);
restore:
	restore_ret = pinctrl_select_state(drv_data->pinctrl,
					   drv_data->pins_default);
	if (!ret)
		ret = restore_ret;
	return ret;
}

static int taurus_i2c_recovery_init(struct taurus_i2c_data *drv_data,
				    struct device *dev)
{
	int ret;

	if (!device_property_present(dev, "scl-gpios"))
		return 0;

	drv_data->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(drv_data->pinctrl)) {
		ret = PTR_ERR(drv_data->pinctrl);
		drv_data->pinctrl = NULL;
		return ret == -ENODEV ? 0 : ret;
	}

	drv_data->pins_default = pinctrl_lookup_state(drv_data->pinctrl,
						      "default");
	if (IS_ERR(drv_data->pins_default)) {
		drv_data->pins_default = NULL;
		goto no_recovery;
	}

	drv_data->pins_gpio = pinctrl_lookup_state(drv_data->pinctrl, "gpio");
	if (IS_ERR(drv_data->pins_gpio)) {
		drv_data->pins_gpio = NULL;
		goto no_recovery;
	}

	drv_data->recovery.recover_bus = taurus_i2c_recover_bus;
	drv_data->adapter.bus_recovery_info = &drv_data->recovery;

	return 0;

no_recovery:
	drv_data->pinctrl = NULL;
	dev_warn(dev, "bus recovery disabled\n");
	return 0;
}

static const struct i2c_algorithm taurus_i2c_algo = {
	.master_xfer = taurus_i2c_xfer,
	.functionality = taurus_i2c_functionality,
};

static const struct of_device_id taurus_i2c_of_match_table[] = {
	{ .compatible = "realtek,taurus-i2c", },
	{ },
};
MODULE_DEVICE_TABLE(of, taurus_i2c_of_match_table);

static int taurus_i2c_of_config(struct taurus_i2c_data *drv_data,
				struct device *dev)
{
	struct device_node *np = dev->of_node;
	u32 bus_freq, timeout, retries;
	int rc = 0;

	if (IS_ERR(drv_data->clk)) {
		rc = -ENODEV;
		goto out;
	}
	drv_data->ref_freq = clk_get_rate(drv_data->clk);

	if (of_property_read_u32(np, "clock-frequency", &bus_freq))
		drv_data->bus_freq = TAURUS_I2C_DEFAULT_BUS_FREQ;
	else
		drv_data->bus_freq = bus_freq;
	if ((drv_data->bus_freq != TAURUS_I2C_DEFAULT_BUS_FREQ) &&
	    (drv_data->bus_freq != TAURUS_I2C_FM_BUS_FREQ) &&
	    (drv_data->bus_freq != TAURUS_I2C_HSM_BUS_FREQ))
		dev_warn(dev, "I2C SCL set to %dKHz\n",
			 drv_data->bus_freq / 1000);

	if (of_property_read_u32(np, "timeout", &timeout))
		drv_data->adapter.timeout = msecs_to_jiffies(TAURUS_I2C_DEFAULT_TIMEOUT);
	else
		drv_data->adapter.timeout = msecs_to_jiffies(timeout);

	if (of_property_read_u32(np, "retries", &retries))
		drv_data->adapter.retries = TAURUS_I2C_DEFAULT_RETRIES;
	else
		drv_data->adapter.retries = retries;

	drv_data->irq = irq_of_parse_and_map(np, 0);

out:
	return rc;
}

static int taurus_i2c_probe(struct platform_device *pd)
{
	struct taurus_i2c_data *drv_data;
	struct resource *res;
	int rc;

	if (!pd->dev.of_node)
		return -ENODEV;

	drv_data = devm_kzalloc(&pd->dev, sizeof(*drv_data), GFP_KERNEL);
	if (!drv_data)
		return -ENOMEM;

	res = platform_get_resource(pd, IORESOURCE_MEM, 0);
	drv_data->reg_base = devm_ioremap_resource(&pd->dev, res);
	if (IS_ERR(drv_data->reg_base))
		return PTR_ERR(drv_data->reg_base);

	strscpy(drv_data->adapter.name, TAURUS_I2C_CTLR_NAME " adapter",
		sizeof(drv_data->adapter.name));

	init_waitqueue_head(&drv_data->waitq);
	spin_lock_init(&drv_data->lock);

	drv_data->state = TAURUS_I2C_STATE_INVALID;

	drv_data->clk = devm_clk_get(&pd->dev, NULL);
	if (!IS_ERR(drv_data->clk))
		clk_prepare_enable(drv_data->clk);

	rc = taurus_i2c_of_config(drv_data, &pd->dev);
	if (rc)
		goto exit_clk;

	drv_data->irq = platform_get_irq(pd, 0);
	if (drv_data->irq < 0) {
		rc = drv_data->irq;
		goto exit_clk;
	}

	rc = taurus_i2c_recovery_init(drv_data, &pd->dev);
	if (rc)
		goto exit_clk;

	drv_data->adapter.dev.parent = &pd->dev;
	drv_data->adapter.algo = &taurus_i2c_algo;
	drv_data->adapter.owner = THIS_MODULE;
	drv_data->adapter.class = I2C_CLASS_DEPRECATED;
	drv_data->adapter.nr = pd->id;
	drv_data->adapter.dev.of_node = pd->dev.of_node;
	platform_set_drvdata(pd, drv_data);
	i2c_set_adapdata(&drv_data->adapter, drv_data);

	taurus_i2c_hw_init(drv_data);

	dev_info(&pd->dev, "irq %d\n", drv_data->irq);
	rc = devm_request_irq(&pd->dev, drv_data->irq, taurus_i2c_intr, 0,
			      TAURUS_I2C_CTLR_NAME, drv_data);
	if (rc) {
		dev_err(&pd->dev,
			"taurus-i2c: can't register intr handler irq%d: %d\n",
			drv_data->irq, rc);
		goto exit_hw;
	}

	rc = i2c_add_numbered_adapter(&drv_data->adapter);
	if (rc != 0) {
		dev_err(&pd->dev,
			"taurus-i2c: can't add i2c adapter, rc: %d\n", -rc);
		devm_free_irq(&pd->dev, drv_data->irq, drv_data);
		goto exit_hw;
	}

	return 0;

exit_hw:
	taurus_i2c_hw_fini(drv_data);
exit_clk:
	if (!IS_ERR(drv_data->clk))
		clk_disable_unprepare(drv_data->clk);

	return rc;
}

static void taurus_i2c_remove(struct platform_device *dev)
{
	struct taurus_i2c_data *drv_data = platform_get_drvdata(dev);

	i2c_del_adapter(&drv_data->adapter);
	taurus_wr_reg(drv_data, TAURUS_BIW_IE0, 0);
	devm_free_irq(&dev->dev, drv_data->irq, drv_data);
	taurus_i2c_hw_fini(drv_data);
	if (!IS_ERR(drv_data->clk))
		clk_disable_unprepare(drv_data->clk);
}

static struct platform_driver taurus_i2c_driver = {
	.probe	= taurus_i2c_probe,
	.remove	= taurus_i2c_remove,
	.driver	= {
		.name	= TAURUS_I2C_CTLR_NAME,
		.of_match_table = taurus_i2c_of_match_table,
	},
};
module_platform_driver(taurus_i2c_driver);

MODULE_DESCRIPTION("Realtek Taurus I2C controller driver");
MODULE_LICENSE("GPL");
