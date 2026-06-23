#include <arch/pgtable.h>
#include <brk/dtb.h>
#include <brk/ioremap.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/plic.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>

static struct plic_device plic;
static u64 mem_base;
static u64 priority_base;
static u64 pending_base;
static u64 enable_base;
static u64 threshold_base;
static u64 claim_complete_base;

static bool plic_source_is_valid(u32 source)
{
	return source < plic.ndev;
}

void plic_init(void)
{
	dtb_parse_plic(&plic);
	mem_base = (u64)ioremap(plic.phys_base, plic.size, PTE_R | PTE_W);
	priority_base = mem_base + PLIC_PRIORITY_OFFSET;
	pending_base = mem_base + PLIC_PENDING_OFFSET;
	enable_base = mem_base + PLIC_ENABLE_OFFSET;
	threshold_base = mem_base + PLIC_THRESHOLD_OFFSET;
	claim_complete_base = mem_base + PLIC_CLAIM_COMPLETE_OFFSET;
}

static volatile u32 *plic_senable(u32 hart_id)
{
	size_t ctx = 2 * hart_id + 1;
	return (volatile u32 *)(enable_base + ctx * 0x80);
}

int plic_enable(u32 hart_id, u32 source)
{
	volatile u32 *senable;

	if (!plic_source_is_valid(source))
		return -EINVAL;

	senable = plic_senable(hart_id);
	if (!senable)
		return -EINVAL;

	senable[source / 32] |= (1 << (source % 32));
	return 0;
}

int plic_disable(u32 hart_id, u32 source)
{
	volatile u32 *senable;

	if (!plic_source_is_valid(source))
		return -EINVAL;

	senable = plic_senable(hart_id);
	if (!senable)
		return -EINVAL;

	senable[source / 32] &= ~(1 << (source % 32));
	return 0;
}

int plic_set_priority(u32 source, unsigned int priority)
{
	if (!plic_source_is_valid(source))
		return -EINVAL;

	((volatile u32 *)(priority_base))[source] = priority;

	return 0;
}

static volatile u32 *plic_sthreshold(u32 hart_id)
{
	size_t ctx = 2 * hart_id + 1;
	return (volatile u32 *)(threshold_base + ctx * 0x1000);
}

int plic_set_threshold(u32 hart_id, unsigned int threshold)
{
	volatile u32 *sthreshold;

	sthreshold = plic_sthreshold(hart_id);
	if (!sthreshold)
		return -EINVAL;

	*sthreshold = threshold;
	return 0;
}

static volatile u32 *plic_sclaim_complete(u32 hart_id)
{
	size_t ctx = 2 * hart_id + 1;
	return (volatile u32 *)(claim_complete_base + ctx * 0x1000);
}

int plic_claim(u32 hart_id, u32 *source)
{
	volatile u32 *sclaim;

	sclaim = plic_sclaim_complete(hart_id);
	if (!sclaim)
		return -EINVAL;

	*source = *sclaim;
	return 0;
}

int plic_complete(u32 hart_id, u32 source)
{
	volatile u32 *scomplete;

	if (!plic_source_is_valid(source))
		return -EINVAL;

	scomplete = plic_sclaim_complete(hart_id);
	if (!scomplete)
		return -EINVAL;

	*scomplete = source;
	return 0;
}

u32 plic_get_ndev(void)
{
	return plic.ndev;
}
