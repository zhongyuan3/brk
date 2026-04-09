#ifndef BRK_UART_H
#define BRK_UART_H

#include <brk/types.h>

struct uart_device {
	uint64_t phys_base;
	size_t size;
	uint32_t irq;
	uint32_t clock_freq;
};

void uart_init(void);
void uart_init_hart(uint32_t hart_id);
void uart_putc(int c);
int uart_getc(void);

#endif
