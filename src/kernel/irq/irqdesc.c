#include <brk/initcall.h>
#include <brk/irq.h>
#include <brk/irq/irqchip.h>
#include <brk/irq/irqdesc.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/printf.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

static struct irq_desc irq_desc_table[NR_IRQS];
static bool irq_allocated[NR_IRQS];
static SPINLOCK_DEFINE(irq_alloc_lock);
static SPINLOCK_DEFINE(irq_desc_init_lock);

void irq_init(void)
{
	spin_lock(&irq_desc_init_lock);
	for (unsigned int i = 0; i < NR_IRQS; i++) {
		memset(&irq_desc_table[i], 0, sizeof(irq_desc_table[i]));
		spinlock_init(&irq_desc_table[i].lock, "irq_desc");
		irq_desc_table[i].irq_data.irq = i;
		irq_desc_table[i].handle_irq = handle_simple_irq;
	}
	spin_unlock(&irq_desc_init_lock);
}
core_initcall(irq_init);

struct irq_desc *irq_to_desc(unsigned int irq)
{
	if (irq == 0 || irq >= NR_IRQS || !irq_allocated[irq])
		return NULL;
	return &irq_desc_table[irq];
}

unsigned int irq_alloc_desc(void)
{
	unsigned int irq;
	irqstate_t flags = spin_lock_irqsave(&irq_alloc_lock);

	for (irq = 1; irq < NR_IRQS; irq++) {
		if (!irq_allocated[irq])
			break;
	}

	if (irq == NR_IRQS) {
		spin_unlock_irqrestore(&irq_alloc_lock, flags);
		return 0;
	}

	irq_allocated[irq] = true;
	spin_unlock_irqrestore(&irq_alloc_lock, flags);

	struct irq_desc *desc = &irq_desc_table[irq];

	memset(desc, 0, sizeof(*desc));
	spinlock_init(&desc->lock, "irq_desc");
	desc->irq_data.irq = irq;
	desc->handle_irq = handle_simple_irq;
	desc->depth = 1; /* masked until request_irq() enables it */
	return irq;
}

void irq_free_desc(unsigned int irq)
{
	irqstate_t flags;

	if (irq == 0 || irq >= NR_IRQS)
		return;

	flags = spin_lock_irqsave(&irq_alloc_lock);
	irq_allocated[irq] = false;
	spin_unlock_irqrestore(&irq_alloc_lock, flags);
}

void irq_set_chip_and_handler(unsigned int irq, struct irq_chip *chip,
			      void (*handle_irq)(struct irq_desc *desc))
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc)
		return;

	desc->irq_data.chip = chip;
	desc->handle_irq = handle_irq ? handle_irq : handle_simple_irq;
}

void irq_set_chip_data(unsigned int irq, void *data)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc)
		desc->irq_data.chip_data = data;
}

struct irq_data *irq_get_irq_data(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	return desc ? &desc->irq_data : NULL;
}

int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
		const char *name, void *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction *action, *tmp;
	irqstate_t sflags;
	int err = 0;

	if (!desc || !handler || !desc->irq_data.chip)
		return -EINVAL;

	action = kzalloc(sizeof(*action));
	if (!action)
		return -ENOMEM;

	action->handler = handler;
	action->dev_id = dev_id;
	action->flags = flags;
	action->name = name;

	sflags = spin_lock_irqsave(&desc->lock);

	if (desc->action && !(flags & IRQF_SHARED)) {
		err = -EBUSY;
		goto out;
	}

	for (tmp = desc->action; tmp; tmp = tmp->next) {
		if (!(tmp->flags & IRQF_SHARED)) {
			err = -EBUSY;
			goto out;
		}
		if (tmp->dev_id == dev_id) {
			err = -EINVAL;
			goto out;
		}
	}

	action->next = desc->action;
	desc->action = action;
	action = NULL;

out:
	spin_unlock_irqrestore(&desc->lock, sflags);
	kfree(action);
	if (err)
		return err;

	if (!(flags & IRQF_NO_AUTOEN))
		enable_irq(irq);
	return 0;
}

void free_irq(unsigned int irq, void *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction **pp, *action;
	irqstate_t flags;
	bool empty;

	if (!desc)
		return;

	flags = spin_lock_irqsave(&desc->lock);

	for (pp = &desc->action; (action = *pp); pp = &action->next) {
		if (action->dev_id == dev_id) {
			*pp = action->next;
			break;
		}
	}
	if (!action) {
		spin_unlock_irqrestore(&desc->lock, flags);
		return;
	}
	empty = !desc->action;
	spin_unlock_irqrestore(&desc->lock, flags);

	kfree(action);
	if (empty)
		disable_irq(irq);
}

int enable_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	irqstate_t flags;

	if (!desc)
		return -EINVAL;

	flags = spin_lock_irqsave(&desc->lock);
	if (!desc->depth) {
		spin_unlock_irqrestore(&desc->lock, flags);
		return 0;
	}
	if (--desc->depth == 0 && desc->irq_data.chip &&
	    desc->irq_data.chip->irq_unmask)
		desc->irq_data.chip->irq_unmask(&desc->irq_data);
	spin_unlock_irqrestore(&desc->lock, flags);
	return 0;
}

void disable_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	irqstate_t flags;

	if (!desc)
		return;

	flags = spin_lock_irqsave(&desc->lock);
	if (desc->depth++ == 0 && desc->irq_data.chip &&
	    desc->irq_data.chip->irq_mask)
		desc->irq_data.chip->irq_mask(&desc->irq_data);
	spin_unlock_irqrestore(&desc->lock, flags);
}

int irq_set_affinity(unsigned int irq, unsigned int hart)
{
	struct irq_desc *desc = irq_to_desc(irq);
	irqstate_t flags;
	int err = 0;

	if (!desc)
		return -EINVAL;

	flags = spin_lock_irqsave(&desc->lock);
	if (desc->irq_data.chip && desc->irq_data.chip->irq_set_affinity)
		err = desc->irq_data.chip->irq_set_affinity(&desc->irq_data,
							    hart);
	else
		desc->irq_data.hart = hart;
	spin_unlock_irqrestore(&desc->lock, flags);
	return err;
}

int irq_set_type(unsigned int irq, unsigned int type)
{
	struct irq_desc *desc = irq_to_desc(irq);
	irqstate_t flags;
	int err = 0;

	if (!desc)
		return -EINVAL;

	flags = spin_lock_irqsave(&desc->lock);
	desc->irq_data.type = type;
	if (desc->irq_data.chip && desc->irq_data.chip->irq_set_type)
		err = desc->irq_data.chip->irq_set_type(&desc->irq_data, type);
	spin_unlock_irqrestore(&desc->lock, flags);
	return err;
}

int generic_handle_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || !desc->handle_irq)
		return -EINVAL;

	desc->handle_irq(desc);
	return 0;
}

static size_t irq_proc_append(char *buf, size_t size, size_t off,
			      const char *fmt, ...)
{
	va_list ap;
	int n;

	if (off >= size)
		return size;

	va_start(ap, fmt);
	n = vsnprintf(buf + off, size - off, fmt, ap);
	va_end(ap);

	if (n < 0)
		return off;
	if ((size_t)n >= size - off)
		return size;
	return off + (size_t)n;
}

int irq_proc_show(char *buf, size_t size)
{
	size_t off;

	if (!buf || !size)
		return 0;

	off = irq_proc_append(buf, size, 0, "virq hwirq count chip action\n");

	for (unsigned int irq = 1; irq < NR_IRQS && off < size; irq++) {
		struct irq_desc *desc = irq_to_desc(irq);
		struct irqaction *action;
		struct irq_chip *chip;
		irqstate_t flags;

		if (!desc)
			continue;

		flags = spin_lock_irqsave(&desc->lock);
		chip = desc->irq_data.chip;

		off = irq_proc_append(buf, size, off,
				      "%3u %5u %10" PRIu64 " %-12s", irq,
				      desc->irq_data.hwirq, desc->count,
				      chip && chip->name ? chip->name : "none");

		for (action = desc->action; action && off < size;
		     action = action->next)
			off = irq_proc_append(buf, size, off, " %s",
					      action->name ? action->name :
							     "?");

		if (!desc->action)
			off = irq_proc_append(buf, size, off, " <none>");

		spin_unlock_irqrestore(&desc->lock, flags);

		off = irq_proc_append(buf, size, off, "\n");
	}

	off = irq_proc_append(buf, size, off, "spurious: %" PRIu64 "\n",
			      irq_spurious_count());

	return (int)off;
}
