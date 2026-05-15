#include <brk/asm.h>
#include <brk/kernel.h>
#include <brk/mm.h>
#include <brk/pgtable.h>
#include <brk/riscv.h>
#include <brk/sbi.h>
#include <brk/types.h>
#include <libfdt.h>

static u64 alloc_pmd_early(u64) __attribute__((section(".text.head")));
static void vmap_early(u64 addr, usize_t size, u64 paddr, unsigned int flags)
	__attribute__((section(".text.head")));
void make_early_pgtable(u64 dtb) __attribute__((section(".text.head")));
void setup_early_pgtable(void) __attribute__((section(".text.head")));

static u64 alloc_pmd_early(u64 prev)
{
	u64 end;

	end = (u64)early_pgdir + NR_EARLY_PGDIR_PAGES * PAGE_SIZE;
	if (prev + PAGE_SIZE < end)
		return prev + PAGE_SIZE;

	sbi_console_putstr("OOM\n");
	while (1)
		;
}

static void vmap_early(u64 addr, usize_t size, u64 paddr, unsigned int flags)
{
	pgde_t *pgdep;
	pmde_t *pmdep;
	u64 pmd_phys;
	u64 end_addr = addr + size;
	u64 prev = (u64)early_pgdir;

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

void make_early_pgtable(u64 dtb)
{
	u64 start = (u64)_skernel;
	u64 end = round_up((u64)_ekernel, PAGE_SIZE_2M);
	usize_t size = end - start;
	unsigned int flags = PTE_R | PTE_W | PTE_X;

	vmap_early(start, size, start, flags);
	vmap_early(KERNEL_LINK_ADDR, size, start, flags);

	usize_t dtb_end = dtb + fdt_totalsize(dtb);
	if (dtb >= start && dtb_end <= end)
		return;

	if (dtb < start)
		dtb_end = min(dtb_end, start);
	else
		dtb = max(dtb, end);

	start = round_down(dtb, PAGE_SIZE_2M);
	end = round_up(dtb_end, PAGE_SIZE_2M);
	size = end - start;
	flags = PTE_R | PTE_W;
	vmap_early(start, size, start, flags);
}

void setup_early_pgtable(void)
{
	write_satp(make_satp_sv39((u64)early_pgdir));
	sfence_vma();
}
