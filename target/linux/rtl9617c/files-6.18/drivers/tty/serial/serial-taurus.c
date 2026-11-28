// SPDX-License-Identifier: GPL-2.0
#include <linux/clk.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/sysrq.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/kfifo.h>

#define TAURUS_UART_NR		2

#define TAURUS_UCFG		0x00
#define TAURUS_UFC		0x04
#define TAURUS_URX_SAMPLE	0x08
#define TAURUS_URT_TUNE		0x0c
#define TAURUS_UTX_DATA		0x10
#define TAURUS_URX_DATA		0x14
#define TAURUS_UINFO		0x18
#define TAURUS_UINT_EN		0x1c
#define TAURUS_UINT_CLR		0x24
#define TAURUS_UINT_STAT	0x2c

#define TAURUS_CFG_STOP_2BIT	BIT(2)
#define TAURUS_CFG_PARITY_EVEN	BIT(3)
#define TAURUS_CFG_PARITY_EN	BIT(4)
#define TAURUS_CFG_TX_EN	BIT(5)
#define TAURUS_CFG_RX_EN	BIT(6)
#define TAURUS_CFG_UART_EN	BIT(7)
#define TAURUS_CFG_BAUD_SHIFT	8

#define TAURUS_INFO_TX_EMPTY	BIT(3)
#define TAURUS_INFO_TX_FULL	BIT(2)
#define TAURUS_INFO_RX_EMPTY	BIT(1)
#define TAURUS_INFO_RX_FULL	BIT(0)

#define TAURUS_INT_RX_BREAK	BIT(7)
#define TAURUS_INT_RX_NON_EMPTY	BIT(6)
#define TAURUS_INT_TX_EMPTY	BIT(5)
#define TAURUS_INT_RX_UNDERRUN	BIT(4)
#define TAURUS_INT_RX_OVERRUN	BIT(3)
#define TAURUS_INT_RX_PARITY_ERR	BIT(2)
#define TAURUS_INT_RX_STOP_ERR	BIT(1)
#define TAURUS_INT_TX_OVERRUN	BIT(0)

#define PORT_TAURUS	199

struct taurus_uart_port {
	struct uart_port uart;
	char name[16];
	char break_indicator;
	unsigned int may_wakeup;
	struct gpio_desc *en_gpio;
	unsigned int en_iodelay;
	unsigned int en_txdelay;
	atomic_t en_state;
};

static struct taurus_uart_port *taurus_uart_ports[TAURUS_UART_NR];
static bool sysrq = true;

static irqreturn_t taurus_uart_interrupt(int irq, void *dev_id);

static unsigned int taurus_uart_tx_empty(struct uart_port *port)
{
	return (readl(port->membase + TAURUS_UINFO) & TAURUS_INFO_TX_EMPTY) ?
	       TIOCSER_TEMT : 0;
}

static void taurus_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int taurus_uart_get_mctrl(struct uart_port *port)
{
	return TIOCM_CTS | TIOCM_DSR | TIOCM_CD;
}

static void taurus_uart_stop_tx(struct uart_port *port)
{
	unsigned int temp;
	struct taurus_uart_port *cp = container_of(port, struct taurus_uart_port,
						   uart);

	temp = readl(port->membase + TAURUS_UINT_EN);
	writel(temp & ~TAURUS_CFG_TX_EN, port->membase + TAURUS_UINT_EN);

	if (cp->en_gpio) {
		while (!(readl(port->membase + TAURUS_UINFO) & TAURUS_INFO_TX_EMPTY))
			;
		udelay(cp->en_iodelay);

		gpiod_set_value(cp->en_gpio, 0);
		atomic_set(&cp->en_state, 0);
	}
}

static void taurus_uart_transmit_buffer(struct uart_port *port)
{
	struct tty_port *tport = &port->state->port;
	struct taurus_uart_port *cp = container_of(port, struct taurus_uart_port,
						   uart);
	unsigned char ch;

	if (kfifo_is_empty(&tport->xmit_fifo) || uart_tx_stopped(port)) {
		taurus_uart_stop_tx(port);
		return;
	}

	if (cp->en_gpio) {
		if (!atomic_read(&cp->en_state))
			udelay(cp->en_txdelay);

		gpiod_set_value(cp->en_gpio, 1);
		atomic_set(&cp->en_state, 1);
	}

	while (!(readl(port->membase + TAURUS_UINFO) & TAURUS_INFO_TX_FULL)) {
		if (!uart_fifo_get(port, &ch))
			break;
		writel(ch, port->membase + TAURUS_UTX_DATA);
	}

	if (kfifo_is_empty(&tport->xmit_fifo))
		taurus_uart_stop_tx(port);
}

static void taurus_uart_start_tx(struct uart_port *port)
{
	unsigned long temp;

	temp = readl(port->membase + TAURUS_UINT_EN);
	writel(temp | TAURUS_CFG_TX_EN, port->membase + TAURUS_UINT_EN);

	if (readl(port->membase + TAURUS_UINFO) & TAURUS_INFO_TX_EMPTY)
		taurus_uart_transmit_buffer(port);
}

static void taurus_uart_stop_rx(struct uart_port *port)
{
	unsigned long temp;

	temp = readl(port->membase + TAURUS_UINT_EN);
	writel(temp & ~TAURUS_CFG_RX_EN, port->membase + TAURUS_UINT_EN);
}

static void taurus_uart_enable_ms(struct uart_port *port)
{
}

static void taurus_uart_break_ctl(struct uart_port *port, int ctl)
{
}

static u8 taurus_uart_irq_cpu = 0xff;
static int __init taurus_uart_irq_cpu_setup(char *val)
{
	return kstrtou8(val, 10, &taurus_uart_irq_cpu);
}
__setup("taurus_uart_cpu=", taurus_uart_irq_cpu_setup);

static int taurus_uart_startup(struct uart_port *port)
{
	unsigned long temp;
	int retval;
	unsigned long flags;

	temp = readl(port->membase + TAURUS_UINT_EN);
	writel(temp & 0, port->membase + TAURUS_UINT_EN);

	retval = request_irq(port->irq, taurus_uart_interrupt, 0, "taurus_uart",
			     port);
	if (retval)
		return retval;

	if ((taurus_uart_irq_cpu < num_possible_cpus()) &&
	    cpu_online(taurus_uart_irq_cpu)) {
		if (irq_set_affinity(port->irq,
				     cpumask_of(taurus_uart_irq_cpu)) == 0)
			pr_info("taurus-uart: IRQ%d bound to CPU%d\n", port->irq,
				taurus_uart_irq_cpu);
	}

	uart_port_lock_irqsave(port, &flags);

	temp = readl(port->membase + TAURUS_UCFG);
	temp |= (TAURUS_CFG_UART_EN | TAURUS_CFG_TX_EN | TAURUS_CFG_RX_EN | 0x3);
	writel(temp, port->membase + TAURUS_UCFG);
	temp = readl(port->membase + TAURUS_UINT_EN);
	writel(temp | TAURUS_CFG_TX_EN | TAURUS_CFG_RX_EN,
	       port->membase + TAURUS_UINT_EN);

	uart_port_unlock_irqrestore(port, flags);
	return 0;
}

static void taurus_uart_shutdown(struct uart_port *port)
{
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	taurus_uart_stop_tx(port);
	taurus_uart_stop_rx(port);
	uart_port_unlock_irqrestore(port, flags);

	free_irq(port->irq, port);
}

static void taurus_uart_set_termios(struct uart_port *port,
				    struct ktermios *termios,
				    const struct ktermios *old)
{
	unsigned long flags, temp;
	int baud;
	unsigned int sample_fre = 0;
	struct taurus_uart_port *cp = container_of(port, struct taurus_uart_port,
						   uart);

	baud = uart_get_baud_rate(port, termios, old, 0, 115200);
	if (!baud)
		baud = 9600;
	temp = readl(port->membase + TAURUS_UCFG);
	temp &= 0xff;
	switch (baud) {
	case 9600:
		temp |= (port->uartclk / 9600) << TAURUS_CFG_BAUD_SHIFT;
		break;
	case 19200:
		temp |= (port->uartclk / 19200) << TAURUS_CFG_BAUD_SHIFT;
		break;
	case 38400:
		temp |= (port->uartclk / 38400) << TAURUS_CFG_BAUD_SHIFT;
		break;
	case 57600:
		temp |= (port->uartclk / 57600) << TAURUS_CFG_BAUD_SHIFT;
		break;
	case 115200:
		temp |= (port->uartclk / 115200) << TAURUS_CFG_BAUD_SHIFT;
		break;
	default:
		temp |= (port->uartclk / 38400) << TAURUS_CFG_BAUD_SHIFT;
		break;
	}
	cp->en_iodelay = (10000000UL / baud) + 1;

	sample_fre = (temp >> TAURUS_CFG_BAUD_SHIFT) / 2;
	temp &= 0xfffffffc;
	switch (termios->c_cflag & CSIZE) {
	case CS5:
		temp |= 0x0;
		break;
	case CS6:
		temp |= 0x1;
		break;
	case CS7:
		temp |= 0x2;
		break;
	case CS8:
	default:
		temp |= 0x3;
		break;
	}

	temp &= ~TAURUS_CFG_STOP_2BIT;
	if (termios->c_cflag & CSTOPB)
		temp |= TAURUS_CFG_STOP_2BIT;

	temp &= ~TAURUS_CFG_PARITY_EN;
	temp |= TAURUS_CFG_PARITY_EVEN;
	if (termios->c_cflag & PARENB) {
		temp |= TAURUS_CFG_PARITY_EN;
		if (termios->c_cflag & PARODD)
			temp &= ~TAURUS_CFG_PARITY_EVEN;
	}

	uart_port_lock_irqsave(port, &flags);
	writel(temp, port->membase + TAURUS_UCFG);
	writel(sample_fre, port->membase + TAURUS_URX_SAMPLE);
	uart_port_unlock_irqrestore(port, flags);
}

static const char *taurus_uart_type(struct uart_port *port)
{
	if (port->type != PORT_TAURUS)
		return NULL;

	return container_of(port, struct taurus_uart_port, uart)->name;
}

static void taurus_uart_release_port(struct uart_port *port)
{
}

static int taurus_uart_request_port(struct uart_port *port)
{
	return 0;
}

static void taurus_uart_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_TAURUS;
}

static int taurus_uart_verify_port(struct uart_port *port,
				   struct serial_struct *ser)
{
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_TAURUS)
		return -EINVAL;
	return 0;
}

#if defined(CONFIG_KGDB_SERIAL_CONSOLE) || \
	defined(CONFIG_KGDB_SERIAL_CONSOLE_MODULE)
static int taurus_uart_poll_get_char(struct uart_port *port)
{
	unsigned int rx;

	while (readl(port->membase + TAURUS_UINFO) & TAURUS_INFO_RX_EMPTY)
		;
	rx = readl(port->membase + TAURUS_URX_DATA);

	return rx;
}

static void taurus_uart_poll_put_char(struct uart_port *port, unsigned char c)
{
	unsigned int ie = readl(port->membase + TAURUS_UINT_EN);

	writel(0, port->membase + TAURUS_UINT_EN);
	while (!(readl(port->membase + TAURUS_UINFO) & TAURUS_INFO_TX_EMPTY))
		;

	writel(c, port->membase + TAURUS_UTX_DATA);
	while (!(readl(port->membase + TAURUS_UINFO) & TAURUS_INFO_TX_EMPTY))
		;

	writel(ie, port->membase + TAURUS_UINT_EN);
}
#endif

static const struct uart_ops taurus_uart_ops = {
	.tx_empty = taurus_uart_tx_empty,
	.set_mctrl = taurus_uart_set_mctrl,
	.get_mctrl = taurus_uart_get_mctrl,
	.stop_tx = taurus_uart_stop_tx,
	.start_tx = taurus_uart_start_tx,
	.stop_rx = taurus_uart_stop_rx,
	.enable_ms = taurus_uart_enable_ms,
	.break_ctl = taurus_uart_break_ctl,
	.startup = taurus_uart_startup,
	.shutdown = taurus_uart_shutdown,
	.set_termios = taurus_uart_set_termios,
	.type = taurus_uart_type,
	.release_port = taurus_uart_release_port,
	.request_port = taurus_uart_request_port,
	.config_port = taurus_uart_config_port,
	.verify_port = taurus_uart_verify_port,
#if defined(CONFIG_KGDB_SERIAL_CONSOLE) || \
	defined(CONFIG_KGDB_SERIAL_CONSOLE_MODULE)
	.poll_get_char = taurus_uart_poll_get_char,
	.poll_put_char = taurus_uart_poll_put_char,
#endif
};

static void taurus_uart_interrupt_rx_chars(struct uart_port *port,
					   unsigned long status)
{
	struct taurus_uart_port *cp = container_of(port, struct taurus_uart_port,
						   uart);
	struct tty_port *ttyport = &port->state->port;
	unsigned int ch;
	unsigned int rx, flg;

	rx = readl(port->membase + TAURUS_UINFO);
	if (TAURUS_INFO_RX_EMPTY & rx)
		return;

	do {
		flg = TTY_NORMAL;
		port->icount.rx++;
		ch = readl(port->membase + TAURUS_URX_DATA);

		if (cp->break_indicator) {
			if (sysrq && (status & TAURUS_INT_RX_BREAK)) {
				if (uart_handle_break(port))
					goto ignore;
			}
		} else {
			if (sysrq && (status & TAURUS_INT_RX_STOP_ERR)) {
				if (uart_handle_break(port))
					goto ignore;
			}
		}
		if (!(ch & 0x100))
			goto ignore;
		if (uart_prepare_sysrq_char(port, (unsigned char)ch))
			goto ignore;
		tty_insert_flip_char(ttyport, ch, flg);
ignore:
		rx = readl(port->membase + TAURUS_UINFO);
	} while (!(TAURUS_INFO_RX_EMPTY & rx));

	uart_port_unlock(port);
	tty_flip_buffer_push(ttyport);
	uart_port_lock(port);
}

static void taurus_uart_interrupt_tx_chars(struct uart_port *port)
{
	struct tty_port *tport = &port->state->port;

	if (port->x_char) {
		writel(port->x_char, port->membase + TAURUS_UTX_DATA);
		port->x_char = 0;
		return;
	}

	if (kfifo_is_empty(&tport->xmit_fifo) || uart_tx_stopped(port)) {
		taurus_uart_stop_tx(port);
		return;
	}

	taurus_uart_transmit_buffer(port);

	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	if (kfifo_is_empty(&tport->xmit_fifo))
		taurus_uart_stop_tx(port);
}

static irqreturn_t taurus_uart_interrupt(int irq, void *dev_id)
{
	struct uart_port *port = (struct uart_port *)dev_id;
	unsigned long temp;

	uart_port_lock(port);

	temp = readl(port->membase + TAURUS_UINT_CLR);
	writel(temp, port->membase + TAURUS_UINT_CLR);

	taurus_uart_interrupt_rx_chars(port, temp);
	taurus_uart_interrupt_tx_chars(port);

	uart_unlock_and_check_sysrq(port);

	return IRQ_HANDLED;
}

#ifdef CONFIG_SERIAL_REALTEK_TAURUS_CONSOLE

static void taurus_console_write(struct console *co, const char *s,
				 unsigned int count)
{
	struct uart_port *port = &taurus_uart_ports[co->index]->uart;
	unsigned long previous;
	int i;
	unsigned long flags;
	int locked;

	local_irq_save(flags);
	if (oops_in_progress) {
		locked = uart_port_trylock(port);
	} else {
		uart_port_lock(port);
		locked = 1;
	}

	previous = readl(port->membase + TAURUS_UINT_EN);
	taurus_uart_stop_tx(port);

	for (i = 0; i < count; i++) {
		while (!(readl(port->membase + TAURUS_UINFO) & TAURUS_INFO_TX_EMPTY))
			udelay(1);

		writel(*s, port->membase + TAURUS_UTX_DATA);

		if (*s++ == '\n') {
			while (!(readl(port->membase + TAURUS_UINFO) & TAURUS_INFO_TX_EMPTY))
				udelay(1);
			writel('\r', port->membase + TAURUS_UTX_DATA);
		}
	}

	writel(previous, port->membase + TAURUS_UINT_EN);

	if (locked)
		uart_port_unlock(port);
	local_irq_restore(flags);
}

static int __init taurus_console_setup(struct console *co, char *options)
{
	struct uart_port *port;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	if (co->index < 0 || co->index >= TAURUS_UART_NR)
		return -ENODEV;

	if (!taurus_uart_ports[co->index])
		return -ENODEV;

	port = &taurus_uart_ports[co->index]->uart;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(port, co, baud, parity, bits, flow);
}

static struct uart_driver taurus_uart_driver;

static struct console taurus_console = {
	.name = "ttyS",
	.write = taurus_console_write,
	.device = uart_console_device,
	.setup = taurus_console_setup,
	.flags = CON_PRINTBUFFER,
	.index = -1,
	.data = &taurus_uart_driver,
};

static void taurus_uart_putc(struct uart_port *port, unsigned char c)
{
	while (!(readl(port->membase + TAURUS_UINFO) & TAURUS_INFO_TX_EMPTY))
		udelay(10);

	writel(c, port->membase + TAURUS_UTX_DATA);
}

static void taurus_early_write(struct console *con, const char *s, unsigned n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, taurus_uart_putc);
}

static int __init taurus_early_console_setup(struct earlycon_device *device,
					     const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = taurus_early_write;
	return 0;
}
OF_EARLYCON_DECLARE(taurus_uart, "realtek,taurus-uart",
		    taurus_early_console_setup);

#endif

static struct uart_driver taurus_uart_driver = {
	.owner = THIS_MODULE,
	.driver_name = "taurus_uart",
	.dev_name = "ttyS",
	.minor = 64,
	.nr = TAURUS_UART_NR,
	.cons = &taurus_console,
};

static int taurus_uart_probe(struct platform_device *pdev)
{
	int ret;
	void __iomem *base;
	struct taurus_uart_port *port;
	struct device_node *np = pdev->dev.of_node;
	struct resource mem_resource;
	u32 of_clock_frequency;
	struct clk *pclk_info;
	int uart_idx;

	for (uart_idx = 0; uart_idx < TAURUS_UART_NR; ++uart_idx)
		if (taurus_uart_ports[uart_idx] == NULL)
			break;

	if (uart_idx >= TAURUS_UART_NR)
		return -EINVAL;

	port = devm_kzalloc(&pdev->dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	snprintf(port->name, sizeof(port->name), "Taurus UART%d", uart_idx);

	ret = of_address_to_resource(np, 0, &mem_resource);
	if (ret)
		return ret;

	base = devm_ioremap(&pdev->dev, mem_resource.start,
			    resource_size(&mem_resource));
	if (!base)
		return -ENOMEM;

	port->uart.irq = irq_of_parse_and_map(np, 0);
	port->uart.membase = base;
	port->uart.mapbase = mem_resource.start;
	port->uart.ops = &taurus_uart_ops;
	port->uart.dev = &pdev->dev;
	port->uart.line = uart_idx;
	port->uart.has_sysrq = IS_ENABLED(CONFIG_SERIAL_REALTEK_TAURUS_CONSOLE);

	if (of_property_read_u32(np, "clock-frequency", &of_clock_frequency)) {
		pclk_info = devm_clk_get_enabled(&pdev->dev, NULL);
		if (IS_ERR(pclk_info))
			return PTR_ERR(pclk_info);

		of_clock_frequency = clk_get_rate(pclk_info);
	}
	port->uart.uartclk = of_clock_frequency;

	if (of_property_read_bool(np, "wakeup-source"))
		port->may_wakeup = true;
	if (of_property_read_bool(np, "break-indicator"))
		port->break_indicator = true;

	port->en_gpio = devm_gpiod_get_optional(&pdev->dev, "en-io",
						GPIOD_OUT_LOW);
	if (IS_ERR(port->en_gpio))
		return PTR_ERR(port->en_gpio);
	if (of_property_read_u32(np, "en-tx-delay", &port->en_txdelay))
		port->en_txdelay = 0;
	if (port->en_gpio)
		dev_info(&pdev->dev, "GPIO enabled, tx-delay = %u\n",
			 port->en_txdelay);

	port->uart.type = PORT_TAURUS;
	taurus_uart_ports[port->uart.line] = port;

	if (port->may_wakeup)
		device_init_wakeup(&pdev->dev, true);

	ret = uart_add_one_port(&taurus_uart_driver, &port->uart);
	if (ret) {
		taurus_uart_ports[port->uart.line] = NULL;
		return ret;
	}

	platform_set_drvdata(pdev, port);

	return 0;
}

static void taurus_uart_remove(struct platform_device *pdev)
{
	struct taurus_uart_port *port = platform_get_drvdata(pdev);

	if (port) {
		uart_remove_one_port(&taurus_uart_driver, &port->uart);
		taurus_uart_ports[port->uart.line] = NULL;
	}

	platform_set_drvdata(pdev, NULL);
}

#ifdef CONFIG_PM_SLEEP
static int taurus_uart_suspend(struct device *dev)
{
	struct taurus_uart_port *port = dev_get_drvdata(dev);

	if (port && port->may_wakeup) {
		device_set_wakeup_enable(dev, 1);
		enable_irq_wake(port->uart.irq);
	}
	return 0;
}

static int taurus_uart_resume(struct device *dev)
{
	struct taurus_uart_port *port = dev_get_drvdata(dev);

	if (port && port->may_wakeup) {
		disable_irq_wake(port->uart.irq);
		device_set_wakeup_enable(dev, 0);
	}

	return 0;
}
#endif

static const struct dev_pm_ops taurus_uart_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(taurus_uart_suspend, taurus_uart_resume)
};

static const struct of_device_id taurus_uart_of_match[] = {
	{ .compatible = "realtek,taurus-uart", },
	{ }
};
MODULE_DEVICE_TABLE(of, taurus_uart_of_match);

static struct platform_driver taurus_uart_platform_driver = {
	.probe = taurus_uart_probe,
	.remove = taurus_uart_remove,
	.driver = {
		.name = "taurus_uart",
		.of_match_table = taurus_uart_of_match,
		.pm = &taurus_uart_pm_ops,
	},
};

static int __init taurus_uart_init(void)
{
	int ret;

	ret = uart_register_driver(&taurus_uart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&taurus_uart_platform_driver);
	if (ret)
		uart_unregister_driver(&taurus_uart_driver);

	return ret;
}

static void __exit taurus_uart_exit(void)
{
	platform_driver_unregister(&taurus_uart_platform_driver);
	uart_unregister_driver(&taurus_uart_driver);
}

module_init(taurus_uart_init);
module_exit(taurus_uart_exit);

module_param(sysrq, bool, 0644);

MODULE_DESCRIPTION("Realtek Taurus UART driver");
MODULE_LICENSE("GPL");
