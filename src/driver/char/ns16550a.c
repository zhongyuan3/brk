#include <arch/pgtable.h>
#include <brk/chrdev.h>
#include <brk/console.h>
#include <brk/dt.h>
#include <brk/initcall.h>
#include <brk/ioremap.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/mmio.h>
#include <brk/platform.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/tty.h>
#include <brk/types.h>
#include <brk/uart.h>
#include <uapi/errno.h>
#include <uapi/types.h>

#define RHR 0 /* Receiver Holding Register */
#define THR 0 /* Transmitter Holding Register */
#define DLL 0 /* Divisor Latch Register */
#define IER 1 /* Interrupt Enable Register */
#define IER_RX_ENABLE (1 << 0)
#define IER_TX_ENABLE (1 << 1)
#define DLM 1 /* Divisor Latch Register */
#define FCR 2 /* FIFO Control Register */
#define FCR_FIFO_ENABLE (1 << 0)
#define FCR_FIFO_CLEAR (3 << 1)
#define ISR 2 /* Interrupt Status Register */
#define LCR 3 /* Line Control Register */
#define LCR_EIGHT_BITS (3 << 0)
#define LCR_BAUD_LATCH (1 << 7)
#define MCR 4 /* Modem Control Register */
#define LSR 5 /* Line Status Register */
#define LSR_RX_READY (1 << 0)
#define LSR_TX_IDLE (1 << 5)
#define MSR 6 /* Modem Status Register */
#define SPR 7 /* ScratchPad Register */

struct ns16550a_device *ns16550a_device_alloc(void)
{
	struct ns16550a_device *dev = kzalloc(sizeof(*dev));
	if (!dev)
		return NULL;
	spinlock_init(&dev->lock, "ns16550a_device");
	return dev;
}

void ns16550a_device_free(struct ns16550a_device *dev)
{
	kfree(dev);
}

static volatile u8 *ns16550a_device_reg(struct ns16550a_device *dev,
					unsigned int reg)
{
	return dev->mem_base + reg;
}

static void ns16550a_device_write_reg(struct ns16550a_device *dev,
				      unsigned int reg, u8 val)
{
	writeb(val, ns16550a_device_reg(dev, reg));
}

static u8 ns16550a_device_read_reg(struct ns16550a_device *dev,
				   unsigned int reg)
{
	return readb(ns16550a_device_reg(dev, reg));
}

static int ns16550a_device_getc(struct ns16550a_device *dev)
{
	int ret = 0;
	irqstate_t flags = spin_lock_irqsave(&dev->lock);

	if (ns16550a_device_read_reg(dev, LSR) & LSR_RX_READY)
		ret = ns16550a_device_read_reg(dev, RHR);
	else
		ret = -ENODATA;
	spin_unlock_irqrestore(&dev->lock, flags);
	return ret;
}

static irqreturn_t ns16550a_device_handle_irq(int irq, void *dev_id)
{
	struct ns16550a_device *dev = dev_id;
	struct tty_port *port = dev->port;
	int c;

	(void)irq;

	c = ns16550a_device_getc(dev);
	if (c < 0)
		return IRQ_NONE;

	if (port && port->tty)
		tty_receive(port->tty, c);

	return IRQ_HANDLED;
}

int ns16550a_device_init(struct ns16550a_device *dev)
{
	int err;

	dev->mem_base = ioremap(dev->phys_base, dev->size, PTE_R | PTE_W);
	if (!dev->mem_base)
		return -ENOMEM;

	/* Disable interrupts. */
	ns16550a_device_write_reg(dev, IER, 0x00);

	/* Special mode to set baud rate. */
	ns16550a_device_write_reg(dev, LCR, LCR_BAUD_LATCH);

	/* LSB for baud rate of 38.4K */
	ns16550a_device_write_reg(dev, DLL, 0x03);

	/* MSB for baud rate of 48.4K */
	ns16550a_device_write_reg(dev, DLM, 0x00);

	/*
	 * Leave set-baud mode, and set word length to 8 bits, no parity
	 */
	ns16550a_device_write_reg(dev, LCR, LCR_EIGHT_BITS);

	/* Reset and enable FIFOs */
	ns16550a_device_write_reg(dev, FCR, FCR_FIFO_CLEAR | FCR_FIFO_ENABLE);

	/* Enable receive interrupts */
	ns16550a_device_write_reg(dev, IER, IER_RX_ENABLE);

	err = request_irq(dev->irq, ns16550a_device_handle_irq, 0,
			  "ns16550a", dev);
	if (err) {
		iounmap((void *)dev->mem_base, dev->size);
		return err;
	}

	return 0;
}

void ns16550a_device_finalize(struct ns16550a_device *dev)
{
	free_irq(dev->irq, dev);
	iounmap((void *)dev->mem_base, dev->size);
}

static struct tty_driver *ns16550a_driver;

static int ns16550a_probe(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct ns16550a_platform_data *pdata = pdev->pdata;
	struct ns16550a_device *uart;
	int err;

	uart = ns16550a_device_alloc();
	if (!uart)
		return -ENOMEM;

	uart->phys_base = pdev->phys_base;
	uart->size = pdev->size;
	uart->irq = pdev->irq;
	uart->clock_freq = pdata ? pdata->clock_freq : 0;

	err = ns16550a_device_init(uart);
	if (err) {
		ns16550a_device_free(uart);
		return err;
	}

	err = ns16550a_add_device(uart);
	if (err) {
		ns16550a_device_finalize(uart);
		ns16550a_device_free(uart);
		return err;
	}

	dev->driver_data = uart;
	return 0;
}

static void ns16550a_remove(struct device *dev)
{
	struct ns16550a_device *uart = dev->driver_data;
	struct ns16550a_driver_data *data;

	if (!uart)
		return;

	dev->driver_data = NULL;
	data = ns16550a_driver ? ns16550a_driver->driver_data : NULL;

	if (uart->port)
		tty_driver_remove_port(ns16550a_driver, uart->port);

	if (data) {
		spin_lock(&ns16550a_driver->lock);
		hlist_del_init(&uart->node);
		spin_unlock(&ns16550a_driver->lock);
	}

	ns16550a_device_finalize(uart);
	ns16550a_device_free(uart);
}

static const struct device_id ns16550a_ids[] = {
	{ .compatible = "ns16550a" },
	{ },
};

static struct platform_driver ns16550a_platform_driver = {
	.driver = {
		.name = "ns16550a",
		.id_table = ns16550a_ids,
		.probe = ns16550a_probe,
		.remove = ns16550a_remove,
	},
};

static int ns16550a_device_putc(struct ns16550a_device *dev, int c)
{
	int ret = 0;
	irqstate_t flags = spin_lock_irqsave(&dev->lock);

	if (ns16550a_device_read_reg(dev, LSR) & LSR_TX_IDLE)
		ns16550a_device_write_reg(dev, THR, c);
	else
		ret = -EBUSY;
	spin_unlock_irqrestore(&dev->lock, flags);
	return ret;
}

static int ns16550a_console_put_char(struct console *console, int c)
{
	struct ns16550a_device *dev = console->client_data;

	if (!dev)
		return -EIO;
	return ns16550a_device_putc(dev, c);
}

static struct console ns16550a_console = {
	.put_char = ns16550a_console_put_char,
	.active = true,
};

void ns16550a_console_disable(void)
{
	console_deactivate(&ns16550a_console);
}

static int ns16550a_tty_put_char(struct tty *tty, int c)
{
	struct ns16550a_device *dev = tty->port->client_data;
	if (!dev)
		return -EIO;

	if (c == TTY_VIS_BACKSPACE) {
		ns16550a_device_putc(dev, '\b');
		ns16550a_device_putc(dev, ' ');
		ns16550a_device_putc(dev, '\b');
	} else {
		ns16550a_device_putc(dev, c);
	}
	return 0;
}

static const struct tty_ops ns16550a_ops = {
	.put_char = ns16550a_tty_put_char,
};


void ns16550a_driver_init(void)
{
	struct tty_driver *driver;
	int err;
	struct ns16550a_driver_data *data;
	dev_t dev = 0;

	data = kzalloc(sizeof(*data));
	if (!data)
		panic("%s(): kzalloc failed\n", __func__);

	driver = tty_alloc_driver(NS16550A_NUM_PORTS);
	if (!driver)
		panic("%s(): tty_alloc_driver failed\n", __func__);

	err = chrdev_alloc_region(NS16550A_MAJOR, 0, NS16550A_NUM_PORTS, &dev);
	if (err)
		panic("%s(): chrdev_alloc_region failed\n", __func__);
	driver->major = MAJOR(dev);
	driver->minor_start = MINOR(dev);
	driver->name = "ns16550a";
	driver->ops = &ns16550a_ops;
	driver->driver_data = data;

	err = tty_register_driver(driver);
	if (err)
		panic("%s(): tty_register_driver failed\n", __func__);

	ns16550a_driver = driver;

	err = platform_driver_register(&ns16550a_platform_driver);
	if (err)
		panic("%s(): platform_driver_register failed\n", __func__);
}
subsys_initcall(ns16550a_driver_init);

int ns16550a_add_device(struct ns16550a_device *dev)
{
	struct tty_port *port;
	int err;
	struct ns16550a_driver_data *data;

	if (!ns16550a_driver)
		return -ENXIO;

	port = tty_port_alloc();
	if (!port)
		return -ENOMEM;

	port->driver = ns16550a_driver;
	port->client_data = dev;
	dev->port = port;

	err = tty_driver_add_port(ns16550a_driver, port);
	if (err) {
		tty_port_free(port);
		return err;
	}

	data = ns16550a_driver->driver_data;

	spin_lock(&ns16550a_driver->lock);
	bool first = hlist_empty(&data->devices);
	hlist_add_head(&dev->node, &data->devices);
	spin_unlock(&ns16550a_driver->lock);

	if (first) {
		ns16550a_console.client_data = dev;
		console_register(&ns16550a_console);
	}

	return 0;
}
