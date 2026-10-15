#include <arch/boot.h>
#include <arch/csr.h>
#include <arch/large_page.h>
#include <arch/mm.h>
#include <arch/page.h>
#include <arch/page_table.h>
#include <arch/pgtable.h>
#include <arch/sbi.h>
#include <brk/assert.h>
#include <brk/kernel.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/of.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/types.h>
#include <brk/vmalloc.h>
#include <libfdt.h>

size_t load_offset;
u64 phys_ram_base;

/*
 * All page tables below the top-level PGD are carved out of the early_pgdir
 * region. The frontier must be global: the identity, link and DTB mappings
 * are built by separate map_range() calls that must not reuse each other's
 * tables.
 */
static u64 rsv_page_next;

static u64 alloc_early_rsv_page(void)
{
	u64 end = (uintptr_t)early_pgdir + NR_EARLY_PGDIR_PAGES * PAGE_SIZE;

	if (rsv_page_next + PAGE_SIZE < end) {
		rsv_page_next += PAGE_SIZE;
		return rsv_page_next;
	}

	sbi_console_putstr("early pgtable OOM\n");
	intr_off();
	for (;;) {
	}
}

static void map_early(u64 addr, size_t size, u64 paddr, unsigned int flags)
{
#if __riscv_xlen == 32
	/*
	 * Sv32: the kernel is loaded at 0x80200000, which is not 4MiB
	 * aligned, so 4MiB superpages cannot encode the link mapping
	 * correctly. Walk PGD -> PTE and map 4KiB pages instead.
	 */
	pgd_t *pgdep;
	pte_t *ptep;
	u64 pt_phys;
	u64 end_addr = addr + size;

	while (addr < end_addr) {
		pgdep = (pgd_t *)early_pgdir + pgd_index(addr);
		if (pgd_present(*pgdep)) {
			pt_phys = pgd_get_pmd(*pgdep);
		} else {
			pt_phys = alloc_early_rsv_page();
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
	u64 pmd_phys;
	u64 end_addr = addr + size;

	while (addr < end_addr) {
		pgdep = (pgd_t *)early_pgdir + pgd_index(addr);
		if (pgd_present(*pgdep)) {
			pmd_phys = pgd_get_pmd(*pgdep);
		} else {
			pmd_phys = alloc_early_rsv_page();
			pgd_set_pmd(pgdep, pmd_phys);
		}
		pmdep = (pmd_t *)pmd_phys + pmd_index(addr);
		pmd_set_large(pmdep, paddr, flags);
		addr += PAGE_SIZE_2M;
		paddr += PAGE_SIZE_2M;
	}
#endif
}

static void __setup_vm_early(uintptr_t dtb, size_t load_offset)
{
	u64 kern_end;
	size_t size;
	unsigned int flags;

	/*
	 * Before paging is enabled, linker symbols resolve to physical
	 * addresses via PC-relative access. load_offset is passed from
	 * head.S and matches (uintptr_t)_skernel - KERNEL_LINK_ADDR.
	 */
	(void)load_offset;

	rsv_page_next = (uintptr_t)early_pgdir;

#if __riscv_xlen == 32
	kern_end = round_up((uintptr_t)_ekernel, PAGE_SIZE_4M);
#else
	kern_end = round_up((uintptr_t)_ekernel, PAGE_SIZE_2M);
#endif
	size = kern_end - (uintptr_t)_skernel;
	flags = PTE_R | PTE_W | PTE_X;

	/* Identity map so execution continues after satp is enabled. */
	map_early((uintptr_t)_skernel, size, (uintptr_t)_skernel, flags);
	/* Map the kernel link address to the physical load address. */
	map_early((uintptr_t)KERNEL_LINK_ADDR, size, (uintptr_t)_skernel,
		  flags);

	size_t dtb_end = dtb + fdt_totalsize((uintptr_t)dtb);
	if (dtb >= (uintptr_t)_skernel && dtb_end <= kern_end)
		return;

	if (dtb < (uintptr_t)_skernel)
		dtb_end = min(dtb_end, (uintptr_t)_skernel);
	else
		dtb = max(dtb, kern_end);

#if __riscv_xlen == 32
	u64 map_start = round_down(dtb, PAGE_SIZE_4M);
	u64 map_end = round_up(dtb_end, PAGE_SIZE_4M);
#else
	u64 map_start = round_down(dtb, PAGE_SIZE_2M);
	u64 map_end = round_up(dtb_end, PAGE_SIZE_2M);
#endif
	size = map_end - map_start;
	flags = PTE_R | PTE_W;
	map_early(map_start, size, map_start, flags);
}

void setup_vm_early(uintptr_t dtb, size_t load_offset, int boot)
{
	if (boot) {
		__setup_vm_early(dtb, load_offset);
	}

	write_satp(make_satp((uintptr_t)early_pgdir));
	sfence_vma();
	fence_i();
}

static void map_mem(void)
{
	u64 idx;
	u64 start, end;
	struct memblock_region *regs, *reg;
	size_t regs_cnt = 0;
	size_t size;

	for_each_mem_range(idx, start, end)
		++regs_cnt;

	size = round_up(sizeof(struct memblock_region) * regs_cnt, PAGE_SIZE);
	regs = (struct memblock_region *)(uintptr_t)memblock_alloc(
		size, _EKERNEL_PHYS, PAGE_SIZE);

	reg = regs;
	for_each_mem_range(idx, start, end) {
		reg->base = start;
		reg->size = end - start;
		++reg;
	}

	unsigned int flags = PTE_R | PTE_W;
	u64 vaddr;

	for (size_t i = 0; i < regs_cnt; ++i) {
		vaddr = phys_to_virt(regs[i].base);
		kvmap_with_mode(vaddr, regs[i].size, regs[i].base, flags,
				VMAP_MODE_EARLY);
	}

	vaddr = phys_to_virt((uintptr_t)regs);
	kvmap_with_mode(vaddr, size, (uintptr_t)regs, flags, VMAP_MODE_EARLY);

	memblock_free((uintptr_t)regs, size);
}

static void map_kernel(void)
{
	u64 virt;

	kvmap_with_mode((uintptr_t)_stext, _TEXT_SIZE, _STEXT_PHYS,
			PTE_R | PTE_X, VMAP_MODE_EARLY);
	kvmap_with_mode((uintptr_t)_srodata, _RODATA_SIZE, _SRODATA_PHYS, PTE_R,
			VMAP_MODE_EARLY);
	kvmap_with_mode((uintptr_t)_sdata, _DATA_SIZE, _SDATA_PHYS,
			PTE_R | PTE_W, VMAP_MODE_EARLY);
	kvmap_with_mode((uintptr_t)_sbss, _BSS_SIZE, _SBSS_PHYS, PTE_R | PTE_W,
			VMAP_MODE_EARLY);

	virt = phys_to_virt(_SRODATA_PHYS);
	kvmap_with_mode(virt, _RODATA_SIZE, _SRODATA_PHYS, PTE_R,
			VMAP_MODE_EARLY);
	virt = phys_to_virt(_SDATA_PHYS);
	kvmap_with_mode(virt, _DATA_SIZE, _SDATA_PHYS, PTE_R | PTE_W,
			VMAP_MODE_EARLY);
	virt = phys_to_virt(_SBSS_PHYS);
	kvmap_with_mode(virt, _BSS_SIZE, _SBSS_PHYS, PTE_R | PTE_W,
			VMAP_MODE_EARLY);
}

static void map_dtb(void)
{
	u64 dtb_virt = phys_to_virt(dtb_phys);
	size_t dtb_size = fdt_totalsize((uintptr_t)dtb_phys);
	kvmap_with_mode(dtb_virt, dtb_size, dtb_phys, PTE_R | PTE_W,
			VMAP_MODE_EARLY);
}

static void final_pgtable_init(void)
{
	map_mem();
	map_kernel();
	map_dtb();
}

static void vmemmap_init(void)
{
	u32 idx;
	u64 start, end;
	size_t sz;
	u64 paddr;

	for_each_mem_pfn_range(idx, start, end) {
		sz = round_up((end - start) * sizeof(struct page), PAGE_SIZE);
		paddr = memblock_alloc(sz, 0, PAGE_SIZE);
		ASSERT(paddr);
		memset((void *)phys_to_virt(paddr), 0, sz);
		kvmap_with_mode((uintptr_t)pfn_to_page(start), sz, paddr,
				PTE_R | PTE_W, VMAP_MODE_INTERIM);
	}
}

void final_pgtable_enable(void)
{
	u64 satp = make_satp(symbol_phys(kernel_pgdir));
	write_satp(satp);
	sfence_vma();
}

void paging_init(void)
{
	final_pgtable_init();
	final_pgtable_enable();
	vmemmap_init();
}

void switch_pgtable(pgd_t *pgd)
{
	u64 paddr = virt_to_phys((uintptr_t)pgd);
	u64 satp = make_satp(paddr);
	write_satp(satp);
	sfence_vma();
}
