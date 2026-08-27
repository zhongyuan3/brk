#include <arch/pgtable.h>
#include <brk/base/types.h>
#include <brk/drivers/plic.h>
#include <brk/init/dtb.h>
#include <brk/irq/irq.h>
#include <brk/lib/string.h>
#include <brk/mm/ioremap.h>
#include <brk/mm/mm.h>
#include <brk/printk/panic.h>
#include <uapi/brk/errno.h>

#define PLIC_PRIORITY_OFFSET 0x000000
#define PLIC_PENDING_OFFSET 0x001000
#define PLIC_ENABLE_OFFSET 0x002000
#define PLIC_THRESHOLD_OFFSET 0x200000
#define PLIC_CLAIM_COMPLETE_OFFSET 0x200004

static struct plic_device plic;
static uint64_t mem_base;
static uint64_t priority_base;
static uint64_t pending_base;
static uint64_t enable_base;
static uint64_t threshold_base;
static uint64_t claim_complete_base;

static bool plic_source_is_valid(uint32_t source)
{
	return source < plic.ndev;
}

static volatile uint32_t *plic_senable(uint32_t hart_id)
{
	size_t ctx = 2 * hart_id + 1;
	return (volatile uint32_t *)(enable_base + ctx * 0x80);
}

static int plic_enable(uint32_t hart_id, uint32_t source)
{
	volatile uint32_t *senable;

	if (!plic_source_is_valid(source))
		return -EINVAL;

	senable = plic_senable(hart_id);
	if (!senable)
		return -EINVAL;

	senable[source / 32] |= (1 << (source % 32));
	return 0;
}

static int plic_disable(uint32_t hart_id, uint32_t source)
{
	volatile uint32_t *senable;

	if (!plic_source_is_valid(source))
		return -EINVAL;

	senable = plic_senable(hart_id);
	if (!senable)
		return -EINVAL;

	senable[source / 32] &= ~(1 << (source % 32));
	return 0;
}

static int plic_set_priority(uint32_t source, unsigned int priority)
{
	if (!plic_source_is_valid(source))
		return -EINVAL;

	((volatile uint32_t *)(priority_base))[source] = priority;

	return 0;
}

static volatile uint32_t *plic_sthreshold(uint32_t hart_id)
{
	size_t ctx = 2 * hart_id + 1;
	return (volatile uint32_t *)(threshold_base + ctx * 0x1000);
}

static int plic_set_threshold(uint32_t hart_id, unsigned int threshold)
{
	volatile uint32_t *sthreshold;

	sthreshold = plic_sthreshold(hart_id);
	if (!sthreshold)
		return -EINVAL;

	*sthreshold = threshold;
	return 0;
}

static volatile uint32_t *plic_sclaim_complete(uint32_t hart_id)
{
	size_t ctx = 2 * hart_id + 1;
	return (volatile uint32_t *)(claim_complete_base + ctx * 0x1000);
}

static int plic_claim(uint32_t hart_id, uint32_t *source)
{
	volatile uint32_t *sclaim;

	sclaim = plic_sclaim_complete(hart_id);
	if (!sclaim)
		return -EINVAL;

	*source = *sclaim;
	return 0;
}

static int plic_complete(uint32_t hart_id, uint32_t source)
{
	volatile uint32_t *scomplete;

	if (!plic_source_is_valid(source))
		return -EINVAL;

	scomplete = plic_sclaim_complete(hart_id);
	if (!scomplete)
		return -EINVAL;

	*scomplete = source;
	return 0;
}

static uint32_t plic_get_ndev(void)
{
	return plic.ndev;
}

static int plic_init_hart(uint32_t hart_id)
{
	return plic_set_threshold(hart_id, 0);
}

static struct irq_chip plic_irq_chip = {
	.get_ndev = plic_get_ndev,
	.init_hart = plic_init_hart,
	.claim = plic_claim,
	.complete = plic_complete,
	.set_priority = plic_set_priority,
	.enable = plic_enable,
	.disable = plic_disable,
};

void plic_init(void)
{
	dtb_parse_plic(&plic);
	mem_base = (uint64_t)ioremap(plic.phys_base, plic.size, PTE_R | PTE_W);
	priority_base = mem_base + PLIC_PRIORITY_OFFSET;
	pending_base = mem_base + PLIC_PENDING_OFFSET;
	enable_base = mem_base + PLIC_ENABLE_OFFSET;
	threshold_base = mem_base + PLIC_THRESHOLD_OFFSET;
	claim_complete_base = mem_base + PLIC_CLAIM_COMPLETE_OFFSET;
	irq_register_chip(&plic_irq_chip);
}