#include <brk/cpu.h>
#include <brk/dt.h>
#include <brk/irq/irqdesc.h>
#include <brk/irq/irqdomain.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <libfdt.h>
#include <uapi/errno.h>

static LIST_DEFINE(irq_domains);
static SPINLOCK_DEFINE(irq_domain_lock);

struct irq_domain *irq_domain_add_linear(int fdt_node, unsigned int size,
					 const struct irq_domain_ops *ops,
					 void *host_data)
{
	struct irq_domain *d;
	irqstate_t flags;

	if (!ops || !size)
		return NULL;

	d = kzalloc(sizeof(*d));
	if (!d)
		return NULL;

	d->revmap = kcalloc(size, sizeof(d->revmap[0]));
	if (!d->revmap) {
		kfree(d);
		return NULL;
	}

	d->name = "linear";
	d->fdt_node = fdt_node;
	d->size = size;
	d->ops = ops;
	d->host_data = host_data;
	list_init(&d->list);

	flags = spin_lock_irqsave(&irq_domain_lock);
	list_add_tail(&d->list, &irq_domains);
	spin_unlock_irqrestore(&irq_domain_lock, flags);
	return d;
}

void irq_domain_remove(struct irq_domain *d)
{
	irqstate_t flags;

	if (!d)
		return;

	flags = spin_lock_irqsave(&irq_domain_lock);
	if (!list_empty(&d->list))
		list_del_init(&d->list);
	spin_unlock_irqrestore(&irq_domain_lock, flags);

	kfree(d->revmap);
	kfree(d);
}

struct irq_domain *irq_find_host(int fdt_node)
{
	struct irq_domain *d, *found = NULL;
	irqstate_t flags;

	flags = spin_lock_irqsave(&irq_domain_lock);
	list_for_each_entry(d, &irq_domains, list) {
		if (d->fdt_node == fdt_node) {
			found = d;
			break;
		}
	}
	spin_unlock_irqrestore(&irq_domain_lock, flags);
	return found;
}

unsigned int irq_create_mapping(struct irq_domain *d, irq_hw_number_t hwirq,
				unsigned int type)
{
	struct irq_desc *desc;
	unsigned int virq;
	int err;

	if (!d || hwirq >= d->size)
		return 0;

	if (d->revmap[hwirq])
		return d->revmap[hwirq];

	virq = irq_alloc_desc();
	if (!virq)
		return 0;

	desc = irq_to_desc(virq);
	desc->irq_data.hwirq = hwirq;
	desc->irq_data.domain = d;
	desc->irq_data.type = type;
	desc->irq_data.hart = this_cpuid();

	if (d->ops->map)
		err = d->ops->map(d, virq, hwirq, type);
	else
		err = 0;

	if (err) {
		irq_free_desc(virq);
		return 0;
	}

	d->revmap[hwirq] = virq;
	return virq;
}

unsigned int irq_find_mapping(struct irq_domain *d, irq_hw_number_t hwirq)
{
	if (!d || hwirq >= d->size)
		return 0;
	return d->revmap[hwirq];
}

unsigned int irq_of_parse_and_map(int fdt_node, int index)
{
	void *fdt = dt_get_fdt();
	struct irq_domain *d;
	const u32 *ints, *prop;
	irq_hw_number_t hwirq;
	unsigned int type, virq;
	int parent = -1, cells, len;
	u32 spec[4];

	if (!fdt || index < 0)
		return 0;

	for (int n = fdt_node; n >= 0; n = fdt_parent_offset(fdt, n)) {
		prop = fdt_getprop(fdt, n, "interrupt-parent", &len);
		if (prop && len == sizeof(u32)) {
			parent = fdt_node_offset_by_phandle(
				fdt, fdt32_to_cpu(*prop));
			break;
		}
	}
	if (parent < 0)
		return 0;

	d = irq_find_host(parent);
	if (!d)
		return 0;

	prop = fdt_getprop(fdt, parent, "#interrupt-cells", &len);
	if (!prop || len != sizeof(u32))
		return 0;
	cells = fdt32_to_cpu(*prop);
	if (cells <= 0 || cells > (int)countof(spec))
		return 0;

	ints = fdt_getprop(fdt, fdt_node, "interrupts", &len);
	if (!ints || len < (index + 1) * cells * (int)sizeof(u32))
		return 0;

	for (int i = 0; i < cells; i++)
		spec[i] = fdt32_to_cpu(ints[index * cells + i]);

	if (d->ops->xlate &&
	    d->ops->xlate(d, fdt_node, spec, cells, &hwirq, &type))
		return 0;

	virq = irq_create_mapping(d, hwirq, type);
	return virq;
}
