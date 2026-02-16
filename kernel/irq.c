#include <aosd/errno.h>
#include <aosd/irq.h>
#include <aosd/panic.h>
#include <aosd/plic.h>
#include <aosd/slab.h>

static irq_handler_t *irq_handlers;
static uint32_t irq_handlers_num;

void irq_init(void)
{
	uint32_t ndev = plic_get_ndev();
	irq_handlers = kcalloc(ndev, sizeof(irq_handler_t));
	if (!irq_handlers)
		panic("%s(): kcalloc() failed\n", __func__);
	irq_handlers_num = ndev;
}

int irq_register_handler(uint32_t source, irq_handler_t handler,
			 irq_handler_t *old_handler)
{
	if (source >= irq_handlers_num)
		return -EINVAL;

	if (old_handler)
		*old_handler = irq_handlers[source];

	irq_handlers[source] = handler;
	return 0;
}

int irq_unregister_handler(uint32_t source, irq_handler_t *old_handler)
{
	if (source >= irq_handlers_num)
		return -EINVAL;

	if (old_handler)
		*old_handler = irq_handlers[source];

	irq_handlers[source] = NULL;
	return 0;
}

int irq_handle_external(uint32_t hart_id)
{
	uint32_t source = 0;

	int err = plic_claim(hart_id, &source);
	if (err)
		return err;

	if (source == 0)
		return 0;

	if (irq_handlers[source])
		irq_handlers[source]();

	return plic_complete(hart_id, source);
}
