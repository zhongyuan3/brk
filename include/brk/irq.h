#ifndef BRK_IRQ_H
#define BRK_IRQ_H

#include <brk/types.h>

typedef void (*irq_handler_t)(void);

void irq_init(void);
void irq_init_hart(u32 hart_id);
int irq_register_handler(u32 source, irq_handler_t handler,
			 irq_handler_t *old_handler);
int irq_unregister_handler(u32 source, irq_handler_t *old_handler);
int irq_handle_external(u32 hart_id);

#endif
