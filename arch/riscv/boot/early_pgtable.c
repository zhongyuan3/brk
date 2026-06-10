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
static u64 early_pgtable_alloc_pmd(u64) __attribute__((section(".text.head")));
static void early_pgtable_map_range(u64 addr, usize_t size, u64 paddr,
				    unsigned int flags)
	__attribute__((section(".text.head")));
void early_pgtable_map(u64 dtb, usize_t load_offset)
	__attribute__((section(".text.head")));
void early_pgtable_enable(usize_t load_offset)
	__attribute__((section(".text.head")));

static void early_pgtable_hang(const char *msg)
{
	sbi_console_putstr(msg);
	for (;;)
		asm volatile("wfi");
}

static u64 early_pgtable_alloc_pmd(u64 prev)
{
	u64 end;

	end = (u64)early_pgdir + NR_EARLY_PGDIR_PAGES * PAGE_SIZE;
	if (prev + PAGE_SIZE < end)
		return prev + PAGE_SIZE;

	early_pgtable_hang("early pgtable OOM\n");
}

static void early_pgtable_map_range(u64 addr, usize_t size, u64 paddr,
				    unsigned int flags)
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
			pmd_phys = early_pgtable_alloc_pmd(prev);
			prev = pmd_phys;
			pgde_set_pmd(pgdep, pmd_phys);
		}
		pmdep = (pmde_t *)pmd_phys + pmde_index(addr);
		pmde_set_large(pmdep, paddr, flags);
		addr += PAGE_SIZE_2M;
		paddr += PAGE_SIZE_2M;
	}
}

void early_pgtable_map(u64 dtb, usize_t load_offset)
{
	u64 kern_end;
	usize_t size;
	unsigned int flags;

	/*
	 * Before paging is enabled, linker symbols resolve to physical
	 * addresses via PC-relative access. load_offset is passed from
	 * head.S and matches (u64)_skernel - KERNEL_LINK_ADDR.
	 */
	(void)load_offset;

	kern_end = round_up((u64)_ekernel, PAGE_SIZE_2M);
	size = kern_end - (u64)_skernel;
	flags = PTE_R | PTE_W | PTE_X;

	/* Identity map so execution continues after satp is enabled. */
	early_pgtable_map_range((u64)_skernel, size, (u64)_skernel, flags);
	/* Map the kernel link address to the physical load address. */
	early_pgtable_map_range((u64)KERNEL_LINK_ADDR, size, (u64)_skernel,
				flags);

	usize_t dtb_end = dtb + fdt_totalsize(dtb);
	if (dtb >= (u64)_skernel && dtb_end <= kern_end)
		return;

	if (dtb < (u64)_skernel)
		dtb_end = min(dtb_end, (u64)_skernel);
	else
		dtb = max(dtb, kern_end);

	u64 map_start = round_down(dtb, PAGE_SIZE_2M);
	u64 map_end = round_up(dtb_end, PAGE_SIZE_2M);
	size = map_end - map_start;
	flags = PTE_R | PTE_W;
	early_pgtable_map_range(map_start, size, map_start, flags);
}

void early_pgtable_enable(usize_t load_offset)
{
	/*
	 * early_pgdir resolves to a physical address here via PC-relative
	 * access; load_offset is unused but kept for a uniform boot ABI.
	 */
	(void)load_offset;
	write_satp(make_satp_sv39((u64)early_pgdir));
	sfence_vma();
	fence_i();
}
