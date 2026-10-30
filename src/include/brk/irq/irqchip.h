#ifndef BRK_IRQ_IRQCHIP_H
#define BRK_IRQ_IRQCHIP_H

#include <brk/list.h>
#include <brk/types.h>

struct irq_data;
struct irq_domain;

struct irq_chip {
	const char *name;
	void (*irq_mask)(struct irq_data *d);
	void (*irq_unmask)(struct irq_data *d);
	void (*irq_ack)(struct irq_data *d);
	void (*irq_eoi)(struct irq_data *d);
	int (*irq_set_type)(struct irq_data *d, unsigned int type);
	int (*irq_set_affinity)(struct irq_data *d, unsigned int hart);
};

struct irq_controller {
	const char *name;
	int (*init_hart)(struct irq_controller *ctrl, unsigned int hart);
	int (*handle_irq)(struct irq_controller *ctrl, unsigned int hart);
	struct irq_domain *domain;
	void *priv;
	struct list_head list;
};

struct irqchip_driver {
	const char *name;
	const char *const *compatibles;
	int (*probe)(int fdt_node);
	struct list_head list;
};

int irqchip_register_driver(struct irqchip_driver *drv);
void irq_register_controller(struct irq_controller *ctrl);
void irq_init_hart_all(unsigned int hart);
int irq_dispatch_external(unsigned int hart);

#endif
