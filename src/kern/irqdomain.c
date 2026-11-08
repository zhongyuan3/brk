#include <brk/cpu.h>
#include <brk/dt.h>
#include <brk/irqdesc.h>
#include <brk/irqdomain.h>
#include <brk/kmalloc.h>
#include <brk/lengthof.h>
#include <brk/of.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <libfdt.h>
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

static struct irq_domain *irq_find_host_by_offset(int fdt_node)
{
	struct irq_domain *d, *found = NULL;
	irqstate_t flags;

	flags = spin_lock_irqsave(&irq_domain_lock);
	list_for_each_entry(d, &irq_domains, list) {
		if (d->of_node && d->of_node->fdt_offset == fdt_node) {
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
	return irq_find_host_by_offset(node->fdt_offset);
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

static int irq_parent_cells(const void *fdt, int parent)
{
	const u32 *prop;
	int len;
	int cells;

	prop = fdt_getprop(fdt, parent, "#interrupt-cells", &len);
	if (!prop || len != sizeof(u32))
		return -EINVAL;

	cells = fdt32_to_cpu(*prop);
	if (cells <= 0 || cells > 4)
		return -EINVAL;
	return cells;
}

static int irq_find_parent(const void *fdt, int fdt_node)
{
	const u32 *prop;
	int len;

	/* interrupt-parent is inherited from the closest ancestor. */
	for (int n = fdt_node; n >= 0; n = fdt_parent_offset(fdt, n)) {
		prop = fdt_getprop(fdt, n, "interrupt-parent", &len);
		if (prop && len == sizeof(u32))
			return fdt_node_offset_by_phandle(fdt,
							  fdt32_to_cpu(*prop));
	}
	return -1;
}

static unsigned int irq_map_spec(const struct device_node *node, int parent,
				 const u32 *intspec, int cells)
{
	struct irq_domain *d;
	irq_hw_number_t hwirq = 0;
	unsigned int type = 0;
	u32 spec[4];

	d = irq_find_host_by_offset(parent);
	if (!d)
		return 0;

	if (cells <= 0 || cells > (int)lengthof(spec))
		return 0;

	for (int i = 0; i < cells; i++)
		spec[i] = fdt32_to_cpu(intspec[i]);

	if (d->ops->xlate && d->ops->xlate(d, node, spec, cells, &hwirq, &type))
		return 0;

	return irq_create_mapping(d, hwirq, type);
}

static unsigned int
irq_parse_extended(const void *fdt, const struct device_node *node, int index)
{
	const u32 *ext;
	const u32 *p;
	int len;
	int entry = 0;
	int remaining;

	ext = fdt_getprop(fdt, node->fdt_offset, "interrupts-extended", &len);
	if (!ext || len < (int)sizeof(u32) || len % sizeof(u32))
		return 0;

	p = ext;
	remaining = len / sizeof(u32);

	/*
	 * Each entry is <&interrupt-parent spec...>, and the number of spec
	 * cells depends on that parent, so walk entries sequentially.
	 */
	while (remaining > 0) {
		u32 phandle = fdt32_to_cpu(*p++);
		int parent;
		int cells;

		remaining--;
		if (!phandle)
			return 0;

		parent = fdt_node_offset_by_phandle(fdt, phandle);
		if (parent < 0)
			return 0;

		cells = irq_parent_cells(fdt, parent);
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
	void *fdt = dt_get_fdt();
	const u32 *ints, *ext;
	int fdt_node, parent, cells, len;

	if (!fdt || !node || index < 0)
		return 0;

	fdt_node = node->fdt_offset;

	ext = fdt_getprop(fdt, fdt_node, "interrupts-extended", &len);
	if (ext && len > 0)
		return irq_parse_extended(fdt, node, index);

	parent = irq_find_parent(fdt, fdt_node);
	if (parent < 0)
		return 0;

	cells = irq_parent_cells(fdt, parent);
	if (cells < 0)
		return 0;

	ints = fdt_getprop(fdt, fdt_node, "interrupts", &len);
	if (!ints || len < (index + 1) * cells * (int)sizeof(u32))
		return 0;

	return irq_map_spec(node, parent, ints + index * cells, cells);
}
