#ifndef BRK_IRQ_H
#define BRK_IRQ_H

#include <brk/types.h>

typedef int irqreturn_t;

#define IRQ_NONE 0
#define IRQ_HANDLED 1

#define NR_IRQS 256

#define IRQF_SHARED 0x00000001
#define IRQF_NO_AUTOEN 0x00000002

#define IRQF_TRIGGER_MASK 0x000000f0
#define IRQF_TRIGGER_HIGH 0x00000010
#define IRQF_TRIGGER_LOW 0x00000020
#define IRQF_TRIGGER_RISING 0x00000040
#define IRQF_TRIGGER_FALLING 0x00000080

#define IRQ_TYPE_NONE 0
#define IRQ_TYPE_EDGE_RISING 1
#define IRQ_TYPE_EDGE_FALLING 2
#define IRQ_TYPE_EDGE_BOTH 3
#define IRQ_TYPE_LEVEL_HIGH 4
#define IRQ_TYPE_LEVEL_LOW 8

struct irq_desc;

typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);

int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
		const char *name, void *dev_id);
void free_irq(unsigned int irq, void *dev_id);

int enable_irq(unsigned int irq);
void disable_irq(unsigned int irq);

int irq_set_affinity(unsigned int irq, unsigned int hart);
int irq_set_type(unsigned int irq, unsigned int type);

int generic_handle_irq(unsigned int irq);
void handle_simple_irq(struct irq_desc *desc);
void handle_level_irq(struct irq_desc *desc);
void handle_edge_irq(struct irq_desc *desc);

void irq_init(void);
void irqchip_init(void);
void irq_init_hart(unsigned int hart);
int irq_handle_external(unsigned int hart);

/* Spurious interrupt accounting (claim 0 or unmapped hwirq). */
void irq_note_spurious(void);
u64 irq_spurious_count(void);

/* Render the IRQ descriptor table for /proc/interrupts. */
int irq_proc_show(char *buf, size_t size);

#endif
