#ifndef BRK_UART_H
#define BRK_UART_H

#include <brk/types.h>

struct uart_device {
	u64 phys_base;
	usize_t size;
	u32 irq;
	u32 clock_freq;
};

void uart_init(void);
void uart_init_hart(u32 hart_id);
void uart_putc(int c);
int uart_getc(void);

#endif
