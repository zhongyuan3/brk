#include <brk/align.h>
#include <brk/asm.h>
#include <brk/macros.h>
#include <brk/mm.h>
#include <brk/pgtable.h>
#include <brk/riscv.h>
#include <brk/sbi.h>
#include <brk/types.h>
#include <libfdt.h>

static uint64_t alloc_pmd_early(uint64_t)
	__attribute__((section(".text.head")));
static void vmap_early(uint64_t addr, size_t size, uint64_t paddr,
		       unsigned int flags)
	__attribute__((section(".text.head")));
void make_early_pgtable(uint64_t dtb) __attribute__((section(".text.head")));
void setup_early_pgtable(void) __attribute__((section(".text.head")));

static uint64_t alloc_pmd_early(uint64_t prev)
{
	uint64_t end;

	end = (uint64_t)early_pgdir + NR_EARLY_PGDIR_PAGES * PAGE_SIZE;
	if (prev + PAGE_SIZE < end)
		return prev + PAGE_SIZE;

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
	uint64_t prev = (uint64_t)early_pgdir;

	while (addr < end_addr) {
		pgdep = (pgde_t *)early_pgdir + pgde_index(addr);
		if (pgde_present(*pgdep)) {
			pmd_phys = pgde_get_pmd(*pgdep);
		} else {
			pmd_phys = alloc_pmd_early(prev);
			prev = pmd_phys;
			pgde_set_pmd(pgdep, pmd_phys);
		}
		pmdep = (pmde_t *)pmd_phys + pmde_index(addr);
		pmde_set_large(pmdep, paddr, flags);
		addr += PAGE_SIZE_2M;
		paddr += PAGE_SIZE_2M;
	}
}

void make_early_pgtable(uint64_t dtb)
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
}

void setup_early_pgtable(void)
{
	write_satp(make_satp_sv39((uint64_t)early_pgdir));
	sfence_vma();
}
