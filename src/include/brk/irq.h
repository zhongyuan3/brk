#ifndef BRK_IRQ_H
#define BRK_IRQ_H

#include <brk/types.h>

typedef void (*irq_handler_t)(void *ctx);

struct irq_chip {
	u32 (*get_ndev)(void *data);
	int (*init_hart)(void *data, u32 hart_id);
	int (*claim)(void *data, u32 hart_id, u32 *source);
	int (*complete)(void *data, u32 hart_id, u32 source);
	int (*set_priority)(void *data, u32 source, unsigned int priority);
	int (*enable)(void *data, u32 hart_id, u32 source);
	int (*disable)(void *data, u32 hart_id, u32 source);
};

void irq_register_chip(struct irq_chip *chip, void *data);
void irq_init(void);
void irq_init_hart(u32 hart_id);
int irq_register_handler(u32 source, irq_handler_t handler, void *ctx,
			 irq_handler_t *old_handler, void **old_ctx);
int irq_unregister_handler(u32 source, irq_handler_t *old_handler,
			   void **old_ctx);
int irq_handle_external(u32 hart_id);
int irq_set_priority(u32 source, unsigned int priority);
int irq_enable_source(u32 hart_id, u32 source);
int irq_disable_source(u32 hart_id, u32 source);

/* architecture-specific */

int irqchip_init(void);

#endif