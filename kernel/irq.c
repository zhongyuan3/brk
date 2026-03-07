#include <aosd/errno.h>
#include <aosd/irq.h>
#include <aosd/lock.h>
#include <aosd/panic.h>
#include <aosd/plic.h>
#include <aosd/slab.h>

static irq_handler_t *irq_handlers;
static uint32_t irq_handlers_num;
static spinlock_define(irq_handlers_lock);

void irq_init(void)
{
	plic_init();
	uint32_t ndev = plic_get_ndev();
	irq_handlers = kcalloc(ndev, sizeof(irq_handler_t));
	if (!irq_handlers)
		panic("%s(): kcalloc() failed\n", __func__);
	irq_handlers_num = ndev;
}

void irq_init_hart(uint32_t hart_id)
{
	plic_set_threshold(hart_id, 0);
}

int irq_register_handler(uint32_t source, irq_handler_t handler,
			 irq_handler_t *old_handler)
{
	if (source >= irq_handlers_num)
		return -EINVAL;

	spinlock_acquire(&irq_handlers_lock);
	if (old_handler)
		*old_handler = irq_handlers[source];

	irq_handlers[source] = handler;
	spinlock_release(&irq_handlers_lock);
	return 0;
}

int irq_unregister_handler(uint32_t source, irq_handler_t *old_handler)
{
	if (source >= irq_handlers_num)
		return -EINVAL;

	spinlock_acquire(&irq_handlers_lock);
	if (old_handler)
		*old_handler = irq_handlers[source];

	irq_handlers[source] = NULL;
	spinlock_release(&irq_handlers_lock);
	return 0;
}

int irq_handle_external(uint32_t hart_id)
{
	uint32_t source = 0;
	irq_handler_t handler;

	int err = plic_claim(hart_id, &source);
	if (err)
		return err;

	if (source == 0)
		return 0;

	spinlock_acquire(&irq_handlers_lock);
	handler = irq_handlers[source];
	spinlock_release(&irq_handlers_lock);

	if (handler)
		handler();

	return plic_complete(hart_id, source);
}
