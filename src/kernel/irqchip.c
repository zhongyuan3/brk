#include <brk/cpu.h>
#include <brk/irq.h>
#include <brk/irqchip.h>
#include <brk/irqdomain.h>
#include <brk/list.h>
#include <brk/of.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/types.h>
#include <uapi/errno.h>

static LIST_DEFINE(irqchip_drivers);
static LIST_DEFINE(irq_controllers);
static SPINLOCK_DEFINE(irqchip_lock);
static u64 irq_spurious;

void irq_note_spurious(void)
{
	__atomic_add_fetch(&irq_spurious, 1, __ATOMIC_RELAXED);
}

u64 irq_spurious_count(void)
{
	return __atomic_load_n(&irq_spurious, __ATOMIC_RELAXED);
}

int irqchip_register_driver(struct irqchip_driver *drv)
{
	irqstate_t flags;

	if (!drv || !drv->name || !drv->compatibles || !drv->probe)
		return -EINVAL;

	flags = spin_lock_irqsave(&irqchip_lock);
	list_add_tail(&drv->list, &irqchip_drivers);
	spin_unlock_irqrestore(&irqchip_lock, flags);
	return 0;
}

void irq_register_controller(struct irq_controller *ctrl)
{
	irqstate_t flags;

	if (!ctrl)
		return;

	flags = spin_lock_irqsave(&irqchip_lock);
	list_add_tail(&ctrl->list, &irq_controllers);
	spin_unlock_irqrestore(&irqchip_lock, flags);
}

void irq_init_hart_all(unsigned int hart)
{
	struct irq_controller *ctrl;

	list_for_each_entry(ctrl, &irq_controllers, list) {
		if (ctrl->init_hart)
			ctrl->init_hart(ctrl, hart);
	}
}

void irq_init_hart(unsigned int hart)
{
	irq_init_hart_all(hart);
}

int irq_dispatch_external(unsigned int hart)
{
	struct irq_controller *ctrl;
	int handled = 0;

	list_for_each_entry(ctrl, &irq_controllers, list) {
		if (ctrl->handle_irq)
			handled += ctrl->handle_irq(ctrl, hart);
	}

	return handled ? 0 : -EINVAL;
}

int irq_handle_external(unsigned int hart)
{
	return irq_dispatch_external(hart);
}

static struct irqchip_driver *irqchip_match(const struct device_node *node)
{
	struct irqchip_driver *drv;

	list_for_each_entry(drv, &irqchip_drivers, list) {
		for (const char *const *c = drv->compatibles; *c; c++) {
			if (of_device_is_compatible(node, *c))
				return drv;
		}
	}
	return NULL;
}

static void irqchip_probe_node(const struct device_node *node)
{
	struct device_node *child;
	struct irqchip_driver *drv;

	if (of_get_property(node, "interrupt-controller", NULL)) {
		drv = irqchip_match(node);
		if (drv) {
			int err = drv->probe(node);

			if (err)
				pr_warn("irqchip: %s probe failed: %d\n",
					drv->name, err);
		}
	}

	of_for_each_child(child, node) irqchip_probe_node(child);
}

/*
 * Early irqchip exception.
 *
 * Interrupt controllers are discovered from the OF tree and probed during
 * early boot, before ordinary platform drivers register.  Platform
 * devices are populated in the same early phase, but consumers resolve their
 * IRQs later, from probe context, through platform_get_irq()/of_irq_get(),
 * so every domain they need is already registered.
 *
 * Fully modelling an interrupt controller as a platform device would require
 * a bootstrap IRQ for the controller itself and deferral of every consumer
 * until the controller has probed.  Keep this direct early phase as an
 * explicit exception until that dependency chain is implemented.
 */
void irqchip_early_init(void)
{
	struct device_node *root = of_root();

	if (!root)
		return;

	irqchip_probe_node(root);
	irq_init_hart_all(this_cpuid());
}
