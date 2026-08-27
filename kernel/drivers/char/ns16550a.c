#include <arch/pgtable.h>
#include <brk/chrdev.h>
#include <brk/dtb.h>
#include <brk/ioremap.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/mmio.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/tty.h>
#include <brk/types.h>
#include <brk/uart.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/types.h>

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

static volatile uint8_t *ns16550a_device_reg(struct ns16550a_device *dev,
					     unsigned int reg)
{
	return dev->mem_base + reg;
}

static void ns16550a_device_write_reg(struct ns16550a_device *dev,
				      unsigned int reg, uint8_t val)
{
	writeb(val, ns16550a_device_reg(dev, reg));
}

static uint8_t ns16550a_device_read_reg(struct ns16550a_device *dev,
					unsigned int reg)
{
	return readb(ns16550a_device_reg(dev, reg));
}

static int ns16550a_device_getc(struct ns16550a_device *dev)
{
	int ret = 0;
	spinlock_acquire(&dev->lock);
	if (ns16550a_device_read_reg(dev, LSR) & LSR_RX_READY)
		ret = ns16550a_device_read_reg(dev, RHR);
	else
		ret = -ENODATA;
	spinlock_release(&dev->lock);
	return ret;
}

static void ns16550a_device_handle_irq(void *ctx)
{
	struct ns16550a_device *dev = ctx;
	int c;
	struct tty_port *port = dev->port;

	c = ns16550a_device_getc(dev);
	if (c < 0)
		return;

	if (!port)
		return;

	if (port->tty)
		tty_receive(port->tty, c);
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

	err = irq_register_handler(dev->irq, ns16550a_device_handle_irq, dev,
				   NULL, NULL);
	if (err) {
		iounmap((void *)dev->mem_base, dev->size);
		return err;
	}

	return 0;
}

void ns16550a_device_finalize(struct ns16550a_device *dev)
{
	irq_unregister_handler(dev->irq, NULL, NULL);
	iounmap((void *)dev->mem_base, dev->size);
}

static int ns16550a_device_enable_irq(struct ns16550a_device *dev,
				      uint32_t hart_id)
{
	int err;
	err = irq_enable_source(hart_id, dev->irq);
	if (err)
		return err;
	err = irq_set_priority(dev->irq, 1);
	if (err) {
		irq_disable_source(hart_id, dev->irq);
		return err;
	}
	return 0;
}

static int ns16550a_device_putc(struct ns16550a_device *dev, int c)
{
	int ret = 0;
	spinlock_acquire(&dev->lock);
	if (ns16550a_device_read_reg(dev, LSR) & LSR_TX_IDLE)
		ns16550a_device_write_reg(dev, THR, c);
	else
		ret = -EBUSY;
	spinlock_release(&dev->lock);
	return ret;
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

static struct tty_driver *ns16550a_driver;

int ns16550a_driver_init(void)
{
	struct tty_driver *driver;
	int err;
	struct ns16550a_driver_data *data;
	dev_t dev = 0;

	data = kzalloc(sizeof(*data));
	if (!data)
		return -ENOMEM;

	driver = tty_alloc_driver(NS16550A_NUM_PORTS);
	if (!driver) {
		kfree(data);
		return -ENOMEM;
	}

	err = chrdev_alloc_region(NS16550A_MAJOR, 0, NS16550A_NUM_PORTS, &dev);
	if (err) {
		tty_free_driver(driver);
		kfree(data);
		return err;
	}
	driver->major = MAJOR(dev);
	driver->minor_start = MINOR(dev);
	driver->name = "ns16550a";
	driver->ops = &ns16550a_ops;
	driver->driver_data = data;

	err = tty_register_driver(driver);
	if (err) {
		kfree(data);
		tty_free_driver(driver);
		return err;
	}

	ns16550a_driver = driver;

	return 0;
}

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

	spinlock_acquire(&ns16550a_driver->lock);
	hlist_add_head(&dev->node, &data->devices);
	spinlock_release(&ns16550a_driver->lock);

	return 0;
}

int ns16550a_enable_irq(uint32_t hart_id)
{
	struct ns16550a_device *dev;

	if (!ns16550a_driver)
		return -ENXIO;

	struct ns16550a_driver_data *data = ns16550a_driver->driver_data;
	int err = 0;

	spinlock_acquire(&ns16550a_driver->lock);
	hlist_for_each_entry(dev, &data->devices, node) {
		err = ns16550a_device_enable_irq(dev, hart_id);
		if (err)
			break;
	}
	spinlock_release(&ns16550a_driver->lock);
	return err;
}
