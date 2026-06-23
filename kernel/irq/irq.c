#include <brk/irq.h>
#include <brk/kmalloc.h>
#include <brk/panic.h>
#include <brk/plic.h>
#include <brk/spinlock.h>
#include <uapi/brk/errno.h>

static irq_handler_t *irq_handlers;
static void **irq_handlers_ctx;
static uint32_t irq_handlers_num;
static SPINLOCK_DEFINE(irq_handlers_lock);

void irq_init(void)
{
	plic_init();
	uint32_t ndev = plic_get_ndev();
	irq_handlers = kcalloc(ndev, sizeof(irq_handler_t));
	if (!irq_handlers)
		panic("%s(): kcalloc() failed\n", __func__);
	irq_handlers_ctx = kcalloc(ndev, sizeof(void *));
	if (!irq_handlers_ctx)
		panic("%s(): kcalloc() failed\n", __func__);
	irq_handlers_num = ndev;
}

void irq_init_hart(uint32_t hart_id)
{
	plic_set_threshold(hart_id, 0);
}

int irq_register_handler(uint32_t source, irq_handler_t handler, void *ctx,
			 irq_handler_t *old_handler, void **old_ctx)
{
	if (source >= irq_handlers_num)
		return -EINVAL;

	spinlock_acquire(&irq_handlers_lock);
	if (old_handler)
		*old_handler = irq_handlers[source];
	if (old_ctx)
		*old_ctx = irq_handlers_ctx[source];

	irq_handlers[source] = handler;
	irq_handlers_ctx[source] = ctx;
	spinlock_release(&irq_handlers_lock);
	return 0;
}

int irq_unregister_handler(uint32_t source, irq_handler_t *old_handler,
			   void **old_ctx)
{
	if (source >= irq_handlers_num)
		return -EINVAL;

	spinlock_acquire(&irq_handlers_lock);
	if (old_handler)
		*old_handler = irq_handlers[source];
	if (old_ctx)
		*old_ctx = irq_handlers_ctx[source];

	irq_handlers[source] = NULL;
	irq_handlers_ctx[source] = NULL;
	spinlock_release(&irq_handlers_lock);
	return 0;
}

int irq_handle_external(uint32_t hart_id)
{
	uint32_t source = 0;
	irq_handler_t handler;
	void *ctx;

	int err = plic_claim(hart_id, &source);
	if (err)
		return err;

	if (source == 0)
		return 0;

	if (!irq_handlers || source >= irq_handlers_num) {
		plic_complete(hart_id, source);
		return -EINVAL;
	}

	spinlock_acquire(&irq_handlers_lock);
	handler = irq_handlers[source];
	ctx = irq_handlers_ctx[source];
	spinlock_release(&irq_handlers_lock);

	if (handler)
		handler(ctx);

	return plic_complete(hart_id, source);
}

int irq_set_priority(uint32_t source, unsigned int priority)
{
	return plic_set_priority(source, priority);
}

int irq_enable_source(uint32_t hart_id, uint32_t source)
{
	return plic_enable(hart_id, source);
}

int irq_disable_source(uint32_t hart_id, uint32_t source)
{
	return plic_disable(hart_id, source);
}
