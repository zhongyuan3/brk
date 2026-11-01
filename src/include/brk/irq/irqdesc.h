#ifndef BRK_IRQ_IRQDESC_H
#define BRK_IRQ_IRQDESC_H

#include <brk/irq.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>

typedef unsigned int irq_hw_number_t;

struct irq_chip;
struct irq_domain;

struct irq_data {
	unsigned int irq;
	irq_hw_number_t hwirq;
	struct irq_chip *chip;
	struct irq_domain *domain;
	void *chip_data;
	unsigned int hart;
	unsigned int type;
};

struct irqaction {
	irq_handler_t handler;
	void *dev_id;
	unsigned long flags;
	const char *name;
	struct irqaction *next;
};

struct irq_desc {
	struct irq_data irq_data;
	struct irqaction *action;
	spinlock_t lock;
	void (*handle_irq)(struct irq_desc *desc);
	unsigned int depth;
	u64 count;
};

struct irq_desc *irq_to_desc(unsigned int irq);
unsigned int irq_alloc_desc(void);
void irq_free_desc(unsigned int irq);

void irq_set_chip_and_handler(unsigned int irq, struct irq_chip *chip,
			      void (*handle_irq)(struct irq_desc *desc));
void irq_set_chip_data(unsigned int irq, void *data);
struct irq_data *irq_get_irq_data(unsigned int irq);

#endif
