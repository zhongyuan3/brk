#ifndef BRK_IRQ_H
#define BRK_IRQ_H

#include <brk/types.h>

typedef void (*irq_handler_t)(void *ctx);

struct irq_chip {
	uint32_t (*get_ndev)(void);
	int (*init_hart)(uint32_t hart_id);
	int (*claim)(uint32_t hart_id, uint32_t *source);
	int (*complete)(uint32_t hart_id, uint32_t source);
	int (*set_priority)(uint32_t source, unsigned int priority);
	int (*enable)(uint32_t hart_id, uint32_t source);
	int (*disable)(uint32_t hart_id, uint32_t source);
};

void irq_register_chip(struct irq_chip *chip);
void irq_init(void);
void irq_init_hart(uint32_t hart_id);
int irq_register_handler(uint32_t source, irq_handler_t handler, void *ctx,
			 irq_handler_t *old_handler, void **old_ctx);
int irq_unregister_handler(uint32_t source, irq_handler_t *old_handler,
			   void **old_ctx);
int irq_handle_external(uint32_t hart_id);
int irq_set_priority(uint32_t source, unsigned int priority);
int irq_enable_source(uint32_t hart_id, uint32_t source);
int irq_disable_source(uint32_t hart_id, uint32_t source);

#endif