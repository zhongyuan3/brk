#include <brk/cpu.h>
#include <brk/dtb.h>
#include <brk/ioremap.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/lock.h>
#include <brk/mm.h>
#include <brk/mmio.h>
#include <brk/panic.h>
#include <brk/pgtable.h>
#include <brk/plic.h>
#include <brk/printk.h>
#include <brk/tty.h>
#include <brk/uart.h>

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

static struct uart_device uart;
static uint8_t volatile *mem_base;
static SPINLOCK_DEFINE(uart_lock);

static volatile uint8_t *uart_reg(unsigned int reg)
{
	return mem_base + reg;
}

static void uart_write_reg(unsigned int reg, uint8_t val)
{
	writeb(val, uart_reg(reg));
}

static uint8_t uart_read_reg(unsigned int reg)
{
	return readb(uart_reg(reg));
}

static void uart_handle_irq(void)
{
	int c = uart_getc();
	if (c < 0)
		return;
	tty_receive(tty_boot(), c);
}

void uart_init(void)
{
	dtb_parse_uart(&uart);

	mem_base = ioremap(uart.phys_base, uart.size, PTE_R | PTE_W);

	/* Disable interrupts. */
	uart_write_reg(IER, 0x00);

	/* Special mode to set baud rate. */
	uart_write_reg(LCR, LCR_BAUD_LATCH);

	/* LSB for baud rate of 38.4K */
	uart_write_reg(DLL, 0x03);

	/* MSB for baud rate of 48.4K */
	uart_write_reg(DLM, 0x00);

	/*
	 * Leave set-baud mode, and set word length to 8 bits, no parity
	 */
	uart_write_reg(LCR, LCR_EIGHT_BITS);

	/* Reset and enable FIFOs */
	uart_write_reg(FCR, FCR_FIFO_CLEAR | FCR_FIFO_ENABLE);

	/* Enable transmit and receive interrupts */
	uart_write_reg(IER, LSR_RX_READY | LSR_TX_IDLE);

	irq_register_handler(uart.irq, uart_handle_irq, NULL);
	plic_set_priority(uart.irq, 1);
}

void uart_init_hart(uint32_t hart_id)
{
	plic_enable(hart_id, uart.irq);
}

void uart_putc(int c)
{
	spinlock_acquire(&uart_lock);

	if (panicked)
		for (;;)
			;

	while (!(uart_read_reg(LSR) & LSR_TX_IDLE))
		;
	uart_write_reg(THR, c);

	spinlock_release(&uart_lock);
}

int uart_getc(void)
{
	int c;

	spinlock_acquire(&uart_lock);

	if (uart_read_reg(LSR) & LSR_RX_READY)
		c = uart_read_reg(RHR);
	else
		c = -1;

	spinlock_release(&uart_lock);

	return c;
}
