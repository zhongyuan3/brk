#ifndef AOSD_IRQ_H
#define AOSD_IRQ_H

#include <aosd/types.h>

typedef void (*irq_handler_t)(void);

void irq_init(void);
void irq_init_hart(uint32_t hart_id);
int irq_register_handler(uint32_t source, irq_handler_t handler,
			 irq_handler_t *old_handler);
int irq_unregister_handler(uint32_t source, irq_handler_t *old_handler);
int irq_handle_external(uint32_t hart_id);

#endif
