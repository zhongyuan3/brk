#ifndef BRK_IRQ_H
#define BRK_IRQ_H

#include <brk/types.h>

typedef void (*irq_handler_t)(void *ctx);

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
