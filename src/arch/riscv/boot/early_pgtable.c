#include <arch/boot.h>
#include <arch/csr.h>
#include <arch/large_page.h>
#include <arch/page.h>
#include <arch/page_table.h>
#include <arch/pgtable.h>
#include <arch/sbi.h>
#include <brk/base/kernel.h>
#include <brk/base/types.h>
#include <brk/mm/mm.h>
#include <libfdt.h>

static void early_pgtable_hang(const char *msg)
	__attribute__((section(".text.head"), noreturn));
static uint64_t early_pgtable_alloc_table(void)
	__attribute__((section(".text.head")));
static void early_pgtable_map_range(uint64_t addr, size_t size, uint64_t paddr,
				    unsigned int flags)
	__attribute__((section(".text.head")));
void early_pgtable_map(uintptr_t dtb, size_t load_offset)
	__attribute__((section(".text.head")));
void early_pgtable_enable(size_t load_offset)
	__attribute__((section(".text.head")));

static void early_pgtable_hang(const char *msg)
{
	sbi_console_putstr(msg);
	for (;;)
		asm volatile("wfi");
}

/*
 * All page tables below the top-level PGD are carved out of the early_pgdir
 * region. The frontier must be global: the identity, link and DTB mappings
 * are built by separate map_range() calls that must not reuse each other's
 * tables.
 */
static uint64_t early_pgtable_next_table;

static uint64_t early_pgtable_alloc_table(void)
{
	uint64_t end =
		(uintptr_t)early_pgdir + NR_EARLY_PGDIR_PAGES * PAGE_SIZE;

	if (early_pgtable_next_table + PAGE_SIZE < end) {
		early_pgtable_next_table += PAGE_SIZE;
		return early_pgtable_next_table;
	}

	early_pgtable_hang("early pgtable OOM\n");
}

static void early_pgtable_map_range(uint64_t addr, size_t size, uint64_t paddr,
				    unsigned int flags)
{
#if __riscv_xlen == 32
	/*
	 * Sv32: the kernel is loaded at 0x80200000, which is not 4MiB
	 * aligned, so 4MiB superpages cannot encode the link mapping
	 * correctly. Walk PGD -> PTE and map 4KiB pages instead.
	 */
	pgd_t *pgdep;
	pte_t *ptep;
	uint64_t pt_phys;
	uint64_t end_addr = addr + size;

	while (addr < end_addr) {
		pgdep = (pgd_t *)early_pgdir + pgd_index(addr);
		if (pgd_present(*pgdep)) {
			pt_phys = pgd_get_pmd(*pgdep);
		} else {
			pt_phys = early_pgtable_alloc_table();
			pgd_set_pmd(pgdep, pt_phys);
		}
		ptep = (pte_t *)(uintptr_t)pt_phys + pte_index(addr);
		pte_set(ptep, paddr, flags);
		addr += PAGE_SIZE;
		paddr += PAGE_SIZE;
	}
#else
	pgd_t *pgdep;
	pmd_t *pmdep;
	uint64_t pmd_phys;
	uint64_t end_addr = addr + size;

	while (addr < end_addr) {
		pgdep = (pgd_t *)early_pgdir + pgd_index(addr);
		if (pgd_present(*pgdep)) {
			pmd_phys = pgd_get_pmd(*pgdep);
		} else {
			pmd_phys = early_pgtable_alloc_table();
			pgd_set_pmd(pgdep, pmd_phys);
		}
		pmdep = (pmd_t *)pmd_phys + pmd_index(addr);
		pmd_set_large(pmdep, paddr, flags);
		addr += PAGE_SIZE_2M;
		paddr += PAGE_SIZE_2M;
	}
#endif
}

void early_pgtable_map(uintptr_t dtb, size_t load_offset)
{
	uint64_t kern_end;
	size_t size;
	unsigned int flags;

	/*
	 * Before paging is enabled, linker symbols resolve to physical
	 * addresses via PC-relative access. load_offset is passed from
	 * head.S and matches (uintptr_t)_skernel - KERNEL_LINK_ADDR.
	 */
	(void)load_offset;

	early_pgtable_next_table = (uintptr_t)early_pgdir;

#if __riscv_xlen == 32
	kern_end = round_up((uintptr_t)_ekernel, PAGE_SIZE_4M);
#else
	kern_end = round_up((uintptr_t)_ekernel, PAGE_SIZE_2M);
#endif
	size = kern_end - (uintptr_t)_skernel;
	flags = PTE_R | PTE_W | PTE_X;

	/* Identity map so execution continues after satp is enabled. */
	early_pgtable_map_range((uintptr_t)_skernel, size, (uintptr_t)_skernel,
				flags);
	/* Map the kernel link address to the physical load address. */
	early_pgtable_map_range((uintptr_t)KERNEL_LINK_ADDR, size,
				(uintptr_t)_skernel, flags);

	size_t dtb_end = dtb + fdt_totalsize((uintptr_t)dtb);
	if (dtb >= (uintptr_t)_skernel && dtb_end <= kern_end)
		return;

	if (dtb < (uintptr_t)_skernel)
		dtb_end = min(dtb_end, (uintptr_t)_skernel);
	else
		dtb = max(dtb, kern_end);

#if __riscv_xlen == 32
	uint64_t map_start = round_down(dtb, PAGE_SIZE_4M);
	uint64_t map_end = round_up(dtb_end, PAGE_SIZE_4M);
#else
	uint64_t map_start = round_down(dtb, PAGE_SIZE_2M);
	uint64_t map_end = round_up(dtb_end, PAGE_SIZE_2M);
#endif
	size = map_end - map_start;
	flags = PTE_R | PTE_W;
	early_pgtable_map_range(map_start, size, map_start, flags);
}

void early_pgtable_enable(size_t load_offset)
{
	/*
	 * early_pgdir resolves to a physical address here via PC-relative
	 * access; load_offset is unused but kept for a uniform boot ABI.
	 */
	(void)load_offset;
	write_satp(make_satp((uintptr_t)early_pgdir));
	sfence_vma();
	fence_i();
}
