#ifndef BRK_IRQDOMAIN_H
#define BRK_IRQDOMAIN_H

#include <brk/irqdesc.h>
#include <brk/list.h>
#include <brk/types.h>

struct device_node;
struct irq_domain;

struct irq_domain_ops {
	int (*xlate)(struct irq_domain *d, const struct device_node *node,
		     const u32 *intspec, unsigned int intsize,
		     irq_hw_number_t *out_hwirq, unsigned int *out_type);
	int (*map)(struct irq_domain *d, unsigned int virq,
		   irq_hw_number_t hwirq, unsigned int type);
	void (*unmap)(struct irq_domain *d, unsigned int virq);
};

struct irq_domain {
	const char *name;
	const struct device_node *of_node;
	unsigned int size;
	unsigned int *revmap;
	const struct irq_domain_ops *ops;
	void *host_data;
	struct list_head list;
};

struct irq_domain *irq_domain_add_linear(const struct device_node *node,
					 unsigned int size,
					 const struct irq_domain_ops *ops,
					 void *host_data);
void irq_domain_remove(struct irq_domain *d);
struct irq_domain *irq_find_host(const struct device_node *node);

unsigned int irq_create_mapping(struct irq_domain *d, irq_hw_number_t hwirq,
				unsigned int type);
unsigned int irq_find_mapping(struct irq_domain *d, irq_hw_number_t hwirq);

/*
 * Parse the index'th "interrupts" or "interrupts-extended" entry and return
 * the mapped virq (0 on failure).
 */
unsigned int irq_of_parse_and_map(const struct device_node *node, int index);

#endif
