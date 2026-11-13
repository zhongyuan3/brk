#include <brk/cpu.h>
#include <brk/irqdesc.h>
#include <brk/irqdomain.h>
#include <brk/kmalloc.h>
#include <brk/lengthof.h>
#include <brk/of.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

static LIST_DEFINE(irq_domains);
static SPINLOCK_DEFINE(irq_domain_lock);

struct irq_domain *irq_domain_add_linear(const struct device_node *node,
					 unsigned int size,
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
	d->of_node = node;
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

static struct irq_domain *irq_find_host_by_node(const struct device_node *node)
{
	struct irq_domain *d, *found = NULL;
	irqstate_t flags;

	flags = spin_lock_irqsave(&irq_domain_lock);
	list_for_each_entry(d, &irq_domains, list) {
		if (d->of_node == node) {
			found = d;
			break;
		}
	}
	spin_unlock_irqrestore(&irq_domain_lock, flags);
	return found;
}

struct irq_domain *irq_find_host(const struct device_node *node)
{
	if (!node)
		return NULL;
	return irq_find_host_by_node(node);
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

static int irq_parent_cells(const struct device_node *parent)
{
	const struct property *prop;
	u32 cells;

	prop = of_find_property(parent, "#interrupt-cells", NULL);
	if (!prop || prop->length != (int)sizeof(u32))
		return -EINVAL;

	cells = of_read_be32(prop->value);
	if (cells == 0 || cells > 4)
		return -EINVAL;
	return (int)cells;
}

static struct device_node *irq_find_parent(const struct device_node *node)
{
	const struct device_node *cur;
	const struct property *prop;
	int len;

	/* interrupt-parent is inherited from the closest ancestor. */
	for (cur = node; cur; cur = cur->parent) {
		prop = of_find_property(cur, "interrupt-parent", &len);
		if (prop && len == (int)sizeof(u32))
			return of_find_node_by_phandle(
				of_read_be32(prop->value));
	}
	return NULL;
}

static unsigned int irq_map_spec(const struct device_node *node,
				 const struct device_node *parent,
				 const u32 *intspec, int cells)
{
	struct irq_domain *d;
	irq_hw_number_t hwirq = 0;
	unsigned int type = 0;
	u32 spec[4];

	d = irq_find_host_by_node(parent);
	if (!d)
		return 0;

	if (cells <= 0 || cells > (int)lengthof(spec))
		return 0;

	for (int i = 0; i < cells; i++)
		spec[i] = of_read_be32(&intspec[i]);

	if (d->ops->xlate && d->ops->xlate(d, node, spec, cells, &hwirq, &type))
		return 0;

	return irq_create_mapping(d, hwirq, type);
}

static unsigned int irq_parse_extended(const struct device_node *node,
				       int index)
{
	const u32 *ext;
	const u32 *p;
	int len;
	int entry = 0;
	int remaining;

	ext = of_get_property(node, "interrupts-extended", &len);
	if (!ext || len < (int)sizeof(u32) || len % sizeof(u32))
		return 0;

	p = ext;
	remaining = len / sizeof(u32);

	/*
	 * Each entry is <&interrupt-parent spec...>, and the number of spec
	 * cells depends on that parent, so walk entries sequentially.
	 */
	while (remaining > 0) {
		struct device_node *parent;
		u32 phandle = of_read_be32(p++);
		int cells;

		remaining--;
		if (!phandle)
			return 0;

		parent = of_find_node_by_phandle(phandle);
		if (!parent)
			return 0;

		cells = irq_parent_cells(parent);
		if (cells < 0 || remaining < cells)
			return 0;

		if (entry == index)
			return irq_map_spec(node, parent, p, cells);

		p += cells;
		remaining -= cells;
		entry++;
	}

	return 0;
}

unsigned int irq_of_parse_and_map(const struct device_node *node, int index)
{
	const u32 *ints, *ext;
	struct device_node *parent;
	int cells, len;

	if (!node || index < 0)
		return 0;

	ext = of_get_property(node, "interrupts-extended", &len);
	if (ext && len > 0)
		return irq_parse_extended(node, index);

	parent = irq_find_parent(node);
	if (!parent)
		return 0;

	cells = irq_parent_cells(parent);
	if (cells < 0)
		return 0;

	ints = of_get_property(node, "interrupts", &len);
	if (!ints || len < (index + 1) * cells * (int)sizeof(u32))
		return 0;

	return irq_map_spec(node, parent, ints + index * cells, cells);
}
