#ifndef BRK_UART_H
#define BRK_UART_H

#include <brk/base/types.h>
#include <brk/drivers/device.h>
#include <brk/lock/spinlock_types.h>

#define NS16550A_NUM_PORTS NS16550A_MINOR_COUNT

struct tty_port;

struct ns16550a_device {
	uint64_t phys_base;
	size_t size;
	uint32_t irq;
	uint32_t clock_freq;
	struct hlist_node node;
	volatile uint8_t *mem_base;
	spinlock_t lock;
	struct tty_port *port;
};

struct ns16550a_device *ns16550a_device_alloc(void);
void ns16550a_device_free(struct ns16550a_device *dev);
int ns16550a_device_init(struct ns16550a_device *dev);
void ns16550a_device_finalize(struct ns16550a_device *dev);

struct ns16550a_driver_data {
	struct hlist_head devices;
};

void ns16550a_driver_init(void);
int ns16550a_add_device(struct ns16550a_device *dev);

int ns16550a_enable_irq(uint32_t hart_id);

#endif
