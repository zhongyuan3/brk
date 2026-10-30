#include <brk/cpu.h>
#include <brk/dt.h>
#include <brk/initcall.h>
#include <brk/irq.h>
#include <brk/irq/irqchip.h>
#include <brk/irq/irqdomain.h>
#include <brk/list.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/types.h>
#include <libfdt.h>
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

static struct irqchip_driver *irqchip_match(int fdt_node)
{
	void *fdt = dt_get_fdt();
	struct irqchip_driver *drv;

	list_for_each_entry(drv, &irqchip_drivers, list) {
		for (const char *const *c = drv->compatibles; *c; c++) {
			if (fdt_node_check_compatible(fdt, fdt_node, *c) == 0)
				return drv;
		}
	}
	return NULL;
}

void irqchip_init(void)
{
	void *fdt = dt_get_fdt();
	int node = -1;
	int depth = 0;

	if (!fdt)
		return;

	while ((node = fdt_next_node(fdt, node, &depth)) >= 0) {
		struct irqchip_driver *drv;

		if (!fdt_getprop(fdt, node, "interrupt-controller", NULL))
			continue;

		drv = irqchip_match(node);
		if (drv) {
			int err = drv->probe(node);

			if (err)
				pr_warn("irqchip: %s probe failed: %d\n",
					drv->name, err);
		}
	}

	irq_init_hart_all(this_cpuid());
}
postcore_initcall(irqchip_init);
