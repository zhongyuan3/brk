#include <aosd/align.h>
#include <aosd/asm.h>
#include <aosd/libfdt.h>
#include <aosd/macros.h>
#include <aosd/mm.h>
#include <aosd/pgtable.h>
#include <aosd/riscv.h>
#include <aosd/sbi.h>
#include <aosd/types.h>

static uint64_t alloc_pmd_early(void)
{
	static size_t current = 1;
	uint64_t addr;

	if (current < EARLY_PGDIR_PAGES) {
		addr = (uint64_t)early_pgdir + current * PAGE_SIZE;
		++current;
		return addr;
	}

	sbi_console_putstr("OOM\n");
	while (1)
		;
}

static void vmap_early(uint64_t addr, size_t size, uint64_t paddr,
		       unsigned int flags)
{
	pgde_t *pgdep;
	pmde_t *pmdep;
	uint64_t pmd_phys;
	uint64_t end_addr = addr + size;
	while (addr < end_addr) {
		pgdep = (pgde_t *)early_pgdir + pgde_index(addr);
		if (pgde_present(*pgdep)) {
			pmd_phys = pgde_get_pmd(*pgdep);
		} else {
			pmd_phys = alloc_pmd_early();
			pgde_set_pmd(pgdep, pmd_phys);
		}
		pmdep = (pmde_t *)pmd_phys + pmde_index(addr);
		pmde_set_large(pmdep, paddr, flags);
		addr += PAGE_SIZE_2M;
		paddr += PAGE_SIZE_2M;
	}
}

void setup_early_pgtable(uint64_t dtb)
{
	uint64_t start = (uint64_t)_skernel;
	uint64_t end = align_up((uint64_t)_ekernel, PAGE_SIZE_2M);
	size_t size = end - start;
	unsigned int flags = PTE_R | PTE_W | PTE_X;

	vmap_early(start, size, start, flags);
	vmap_early(KERNEL_LINK_ADDR, size, start, flags);

	size_t dtb_end = dtb + fdt_totalsize(dtb);
	if (dtb >= start && dtb_end <= end)
		return;

	if (dtb < start)
		dtb_end = min(dtb_end, start);
	else
		dtb = max(dtb, end);

	start = align_down(dtb, PAGE_SIZE_2M);
	end = align_up(dtb_end, PAGE_SIZE_2M);
	size = end - start;
	flags = PTE_R | PTE_W;
	vmap_early(start, size, start, flags);

	write_satp(make_satp_sv39((uint64_t)early_pgdir));
	sfence_vma();
}
