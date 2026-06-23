#include <arch/csr.h>
#include <arch/pgtable.h>
#include <arch/sbi.h>
#include <asm/boot.h>
#include <asm/large_page.h>
#include <asm/page.h>
#include <asm/page_table.h>
#include <brk/kernel.h>
#include <brk/mm.h>
#include <brk/types.h>
#include <libfdt.h>

static void early_pgtable_hang(const char *msg)
	__attribute__((section(".text.head"), noreturn));
static uint64_t early_pgtable_alloc_pmd(uint64_t)
	__attribute__((section(".text.head")));
static void early_pgtable_map_range(uint64_t addr, size_t size, uint64_t paddr,
				    unsigned int flags)
	__attribute__((section(".text.head")));
void early_pgtable_map(uint64_t dtb, size_t load_offset)
	__attribute__((section(".text.head")));
void early_pgtable_enable(size_t load_offset)
	__attribute__((section(".text.head")));

static void early_pgtable_hang(const char *msg)
{
	sbi_console_putstr(msg);
	for (;;)
		asm volatile("wfi");
}

static uint64_t early_pgtable_alloc_pmd(uint64_t prev)
{
	uint64_t end;

	end = (uint64_t)early_pgdir + NR_EARLY_PGDIR_PAGES * PAGE_SIZE;
	if (prev + PAGE_SIZE < end)
		return prev + PAGE_SIZE;

	early_pgtable_hang("early pgtable OOM\n");
}

static void early_pgtable_map_range(uint64_t addr, size_t size, uint64_t paddr,
				    unsigned int flags)
{
	pgd_t *pgdep;
	pmd_t *pmdep;
	uint64_t pmd_phys;
	uint64_t end_addr = addr + size;
	uint64_t prev = (uint64_t)early_pgdir;

	while (addr < end_addr) {
		pgdep = (pgd_t *)early_pgdir + pgd_index(addr);
		if (pgd_present(*pgdep)) {
			pmd_phys = pgd_get_pmd(*pgdep);
		} else {
			pmd_phys = early_pgtable_alloc_pmd(prev);
			prev = pmd_phys;
			pgd_set_pmd(pgdep, pmd_phys);
		}
		pmdep = (pmd_t *)pmd_phys + pmd_index(addr);
		pmd_set_large(pmdep, paddr, flags);
		addr += PAGE_SIZE_2M;
		paddr += PAGE_SIZE_2M;
	}
}

void early_pgtable_map(uint64_t dtb, size_t load_offset)
{
	uint64_t kern_end;
	size_t size;
	unsigned int flags;

	/*
	 * Before paging is enabled, linker symbols resolve to physical
	 * addresses via PC-relative access. load_offset is passed from
	 * head.S and matches (uint64_t)_skernel - KERNEL_LINK_ADDR.
	 */
	(void)load_offset;

	kern_end = round_up((uint64_t)_ekernel, PAGE_SIZE_2M);
	size = kern_end - (uint64_t)_skernel;
	flags = PTE_R | PTE_W | PTE_X;

	/* Identity map so execution continues after satp is enabled. */
	early_pgtable_map_range((uint64_t)_skernel, size, (uint64_t)_skernel,
				flags);
	/* Map the kernel link address to the physical load address. */
	early_pgtable_map_range((uint64_t)KERNEL_LINK_ADDR, size,
				(uint64_t)_skernel, flags);

	size_t dtb_end = dtb + fdt_totalsize(dtb);
	if (dtb >= (uint64_t)_skernel && dtb_end <= kern_end)
		return;

	if (dtb < (uint64_t)_skernel)
		dtb_end = min(dtb_end, (uint64_t)_skernel);
	else
		dtb = max(dtb, kern_end);

	uint64_t map_start = round_down(dtb, PAGE_SIZE_2M);
	uint64_t map_end = round_up(dtb_end, PAGE_SIZE_2M);
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
	write_satp(make_satp_sv39((uint64_t)early_pgdir));
	sfence_vma();
	fence_i();
}
