#include <arch/pgtable.h>
#include <brk/dt.h>
#include <brk/ioremap.h>
#include <brk/irq.h>
#include <brk/kmalloc.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/plic.h>
#include <brk/sifive_plic.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

#define PLIC_PRIORITY_OFFSET 0x000000
#define PLIC_PENDING_OFFSET 0x001000
#define PLIC_ENABLE_OFFSET 0x002000
#define PLIC_THRESHOLD_OFFSET 0x200000
#define PLIC_CLAIM_COMPLETE_OFFSET 0x200004

struct sifive_plic {
	u64 mem_base;
	u64 priority_base;
	u64 pending_base;
	u64 enable_base;
	u64 threshold_base;
	u64 claim_complete_base;
};

static bool plic_source_is_valid(struct plic_device *plic, u32 source)
{
	return source < plic->ndev;
}

static volatile u32 *plic_senable(struct sifive_plic *plic_data, u32 hart_id)
{
	size_t ctx = 2 * hart_id + 1;
	return (volatile u32 *)(uintptr_t)(plic_data->enable_base + ctx * 0x80);
}

static int plic_enable(void *data, u32 hart_id, u32 source)
{
	volatile u32 *senable;
	struct plic_device *plic = data;
	struct sifive_plic *plic_data = plic->client_data;

	if (!plic_source_is_valid(plic, source))
		return -EINVAL;

	senable = plic_senable(plic_data, hart_id);
	if (!senable)
		return -EINVAL;

	senable[source / 32] |= (1 << (source % 32));
	return 0;
}

static int plic_disable(void *data, u32 hart_id, u32 source)
{
	volatile u32 *senable;
	struct plic_device *plic = data;
	struct sifive_plic *plic_data = plic->client_data;

	if (!plic_source_is_valid(plic, source))
		return -EINVAL;

	senable = plic_senable(plic_data, hart_id);
	if (!senable)
		return -EINVAL;

	senable[source / 32] &= ~(1 << (source % 32));
	return 0;
}

static int plic_set_priority(void *data, u32 source, unsigned int priority)
{
	struct plic_device *plic = data;
	struct sifive_plic *plic_data = plic->client_data;
	if (!plic_source_is_valid(plic, source))
		return -EINVAL;

	((volatile u32 *)(uintptr_t)(plic_data->priority_base))[source] =
		priority;

	return 0;
}

static volatile u32 *plic_sthreshold(struct sifive_plic *plic_data, u32 hart_id)
{
	size_t ctx = 2 * hart_id + 1;
	return (volatile u32 *)(uintptr_t)(plic_data->threshold_base +
					   ctx * 0x1000);
}

static int plic_set_threshold(struct sifive_plic *plic_data, u32 hart_id,
			      unsigned int threshold)
{
	volatile u32 *sthreshold;

	sthreshold = plic_sthreshold(plic_data, hart_id);
	if (!sthreshold)
		return -EINVAL;

	*sthreshold = threshold;
	return 0;
}

static volatile u32 *plic_sclaim_complete(struct sifive_plic *plic_data,
					  u32 hart_id)
{
	size_t ctx = 2 * hart_id + 1;
	return (volatile u32 *)(uintptr_t)(plic_data->claim_complete_base +
					   ctx * 0x1000);
}

static int plic_claim(void *data, u32 hart_id, u32 *source)
{
	volatile u32 *sclaim;
	struct plic_device *plic = data;
	struct sifive_plic *plic_data = plic->client_data;

	sclaim = plic_sclaim_complete(plic_data, hart_id);
	if (!sclaim)
		return -EINVAL;

	*source = *sclaim;
	return 0;
}

static int plic_complete(void *data, u32 hart_id, u32 source)
{
	volatile u32 *scomplete;
	struct plic_device *plic = data;
	struct sifive_plic *plic_data = plic->client_data;

	if (!plic_source_is_valid(plic, source))
		return -EINVAL;

	scomplete = plic_sclaim_complete(plic_data, hart_id);
	if (!scomplete)
		return -EINVAL;

	*scomplete = source;
	return 0;
}

static u32 plic_get_ndev(void *data)
{
	struct plic_device *plic = data;
	return plic->ndev;
}

static int plic_init_hart(void *data, u32 hart_id)
{
	struct plic_device *plic = data;
	struct sifive_plic *plic_data = plic->client_data;
	return plic_set_threshold(plic_data, hart_id, 0);
}

static struct irq_chip sifive_plic_irq_chip = {
	.get_ndev = plic_get_ndev,
	.init_hart = plic_init_hart,
	.claim = plic_claim,
	.complete = plic_complete,
	.set_priority = plic_set_priority,
	.enable = plic_enable,
	.disable = plic_disable,
};

int sifive_plic_init(struct plic_device *plic)
{
	struct sifive_plic *plic_data = kzalloc(sizeof(struct sifive_plic));
	plic_data->mem_base =
		(uintptr_t)ioremap(plic->phys_base, plic->size, PTE_R | PTE_W);
	plic_data->priority_base = plic_data->mem_base + PLIC_PRIORITY_OFFSET;
	plic_data->pending_base = plic_data->mem_base + PLIC_PENDING_OFFSET;
	plic_data->enable_base = plic_data->mem_base + PLIC_ENABLE_OFFSET;
	plic_data->threshold_base = plic_data->mem_base + PLIC_THRESHOLD_OFFSET;
	plic_data->claim_complete_base =
		plic_data->mem_base + PLIC_CLAIM_COMPLETE_OFFSET;
	plic->client_data = plic_data;
	plic->chip = &sifive_plic_irq_chip;

	return 0;
}
