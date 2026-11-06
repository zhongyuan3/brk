#include <brk/irq.h>
#include <brk/irq/irqchip.h>
#include <brk/irq/irqdesc.h>
#include <brk/spinlock.h>
#include <brk/types.h>

void handle_simple_irq(struct irq_desc *desc)
{
	struct irqaction *action;
	irqstate_t flags = spin_lock_irqsave(&desc->lock);

	desc->count++;
	for (action = desc->action; action; action = action->next)
		action->handler(desc->irq_data.irq, action->dev_id);

	spin_unlock_irqrestore(&desc->lock, flags);
}

void handle_level_irq(struct irq_desc *desc)
{
	struct irq_data *d = &desc->irq_data;
	struct irqaction *action;
	irqstate_t flags = spin_lock_irqsave(&desc->lock);

	if (d->chip && d->chip->irq_mask)
		d->chip->irq_mask(d);
	if (d->chip && d->chip->irq_ack)
		d->chip->irq_ack(d);

	desc->count++;
	for (action = desc->action; action; action = action->next)
		action->handler(desc->irq_data.irq, action->dev_id);

	if (d->chip && d->chip->irq_unmask)
		d->chip->irq_unmask(d);

	spin_unlock_irqrestore(&desc->lock, flags);
}

void handle_edge_irq(struct irq_desc *desc)
{
	struct irq_data *d = &desc->irq_data;
	struct irqaction *action;
	irqstate_t flags = spin_lock_irqsave(&desc->lock);

	if (d->chip && d->chip->irq_ack)
		d->chip->irq_ack(d);

	desc->count++;
	for (action = desc->action; action; action = action->next)
		action->handler(desc->irq_data.irq, action->dev_id);

	if (d->chip && d->chip->irq_eoi)
		d->chip->irq_eoi(d);

	spin_unlock_irqrestore(&desc->lock, flags);
}
