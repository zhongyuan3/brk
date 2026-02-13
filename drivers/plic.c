#include <aosd/align.h>
#include <aosd/dtb.h>
#include <aosd/errno.h>
#include <aosd/ioremap.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/pgtable.h>
#include <aosd/plic.h>
#include <aosd/string.h>
#include <aosd/types.h>

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

void plic_init(void)
{
	dtb_parse_plic(&plic);
	mem_base = (uint64_t)ioremap(plic.phys_base, plic.size, PTE_R | PTE_W);
	priority_base = mem_base + PLIC_PRIORITY_OFFSET;
	pending_base = mem_base + PLIC_PENDING_OFFSET;
	enable_base = mem_base + PLIC_ENABLE_OFFSET;
	threshold_base = mem_base + PLIC_THRESHOLD_OFFSET;
	claim_complete_base = mem_base + PLIC_CLAIM_COMPLETE_OFFSET;
}

static volatile uint32_t *plic_senable(uint32_t hart_id)
{
	size_t ctx = 2 * hart_id + 1;
	return (volatile uint32_t *)(enable_base + ctx * 0x80);
}

int plic_enable(struct cpu *cpu, uint32_t source)
{
	volatile uint32_t *senable;

	if (!plic_source_is_valid(source))
		return -EINVAL;

	senable = plic_senable(cpu->hart_id);
	if (!senable)
		return -EINVAL;

	senable[source / 32] |= (1 << (source % 32));
	return 0;
}

int plic_set_priority(uint32_t source, unsigned int priority)
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

int plic_set_threshold(struct cpu *cpu, unsigned int threshold)
{
	volatile uint32_t *sthreshold;

	sthreshold = plic_sthreshold(cpu->hart_id);
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

int plic_claim(struct cpu *cpu, uint32_t *source)
{
	volatile uint32_t *sclaim;

	sclaim = plic_sclaim_complete(cpu->hart_id);
	if (!sclaim)
		return -EINVAL;

	*source = *sclaim;
	return 0;
}

int plic_complete(struct cpu *cpu, uint32_t source)
{
	volatile uint32_t *scomplete;

	if (!plic_source_is_valid(source))
		return -EINVAL;

	scomplete = plic_sclaim_complete(cpu->hart_id);
	if (!scomplete)
		return -EINVAL;

	*scomplete = source;
	return 0;
}

uint32_t plic_get_ndev(void)
{
	return plic.ndev;
}
