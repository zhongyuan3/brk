#include <arch/asm/mm.h>
#include <arch/asm/page.h>
#include <arch/asm/vmlayout.h>
#include <arch/large_page.h>
#include <arch/mm.h>
#include <arch/pgtable.h>
#include <arch/pgtable_types.h>
#include <brk/asm/cpu.h>
#include <brk/assert.h>
#include <brk/kernel.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/of.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/stddef.h>
#include <brk/types.h>
#include <libfdt.h>
#include <riscv/boot_info.h>
#include <riscv/csr.h>
#include <riscv/sbi.h>

unsigned long phys_offset;

unsigned long early_pgdir_rsvd_page_next;

static unsigned long alloc_early_pgdir_rsvd_page(void)
{
	unsigned long base = (unsigned long)early_pgdir_rsvd_pages;
	unsigned long end = base + NR_EARLY_PGDIR_RSVD_PAGES * PAGE_SIZE;

	if (early_pgdir_rsvd_page_next < end) {
		unsigned long addr = early_pgdir_rsvd_page_next;
		early_pgdir_rsvd_page_next += PAGE_SIZE;
		return addr;
	}

	sbi_console_putstr("boot: reserved pages used up\n");
	while (1) {
	}
}

static void map_early(unsigned long addr, unsigned long paddr,
		      unsigned long size, unsigned int flags)
{
	pgd_t *pgdep;
	pmd_t *pmdep;
	unsigned long pmd_phys;
	unsigned long end_addr = addr + size;

	while (addr < end_addr) {
		pgdep = (pgd_t *)early_pgdir_rsvd_pages + pgd_index(addr);
		if (pgd_is_valid(*pgdep)) {
			pmd_phys = pgd_next(*pgdep);
		} else {
			pmd_phys = alloc_early_pgdir_rsvd_page();
			pgd_set_next(pgdep, pmd_phys);
		}
		pmdep = (pmd_t *)pmd_phys + pmd_index(addr);
		pmd_set_leaf(pmdep, paddr, flags);
		addr += PAGE_SIZE_2M;
		paddr += PAGE_SIZE_2M;
	}
}

static void __setup_vm_early(unsigned long dtb_pa)
{
	unsigned long kern_end;
	unsigned long size;
	unsigned int flags;

	early_pgdir_rsvd_page_next =
		(unsigned long)early_pgdir_rsvd_pages + PAGE_SIZE;

	kern_end = round_up(_EKERNEL, PAGE_SIZE_2M);
	size = kern_end - _SKERNEL;
	flags = PTE_R | PTE_W | PTE_X;

	/* Identity map so execution continues after satp is enabled. */
	map_early(_SKERNEL, _SKERNEL, size, flags);
	/* Map the kernel link address to the physical load address. */
	map_early(KERNEL_LINK_ADDR, _SKERNEL, size, flags);

	unsigned long dtb_end = dtb_pa + fdt_totalsize(dtb_pa);
	if (dtb_pa >= _SKERNEL && dtb_end <= kern_end)
		return;

	if (dtb_pa < _SKERNEL)
		dtb_end = min(dtb_end, _SKERNEL);
	else
		dtb_pa = max(dtb_pa, kern_end);

	unsigned long map_start = round_down(dtb_pa, PAGE_SIZE_2M);
	unsigned long map_end = round_up(dtb_end, PAGE_SIZE_2M);
	size = map_end - map_start;
	flags = PTE_R | PTE_W;
	map_early(map_start, map_start, size, flags);
}

void setup_vm_early(unsigned long dtb_pa, bool boot)
{
	if (boot) {
		__setup_vm_early(dtb_pa);
	}

	write_satp(make_satp((unsigned long)early_pgdir_rsvd_pages));
	sfence_vma();
	fence_i();
}

static void phys_mem_init(void)
{
	memblock_reserve(_SKERNEL_PHYS, _KERNEL_SIZE);
	memblock_reserve(boot_info.dtb_pa, fdt_totalsize(boot_info.dtb_pa));
	dtb_early_init_scan_mem(boot_info.dtb_pa);
	dtb_early_init_scan_reserved_mem(boot_info.dtb_pa);
	phys_offset = memblock_start_of_dram();
}

struct mapping_region {
	const char *name;
	unsigned long addr;
	unsigned long paddr;
	unsigned long size;
	unsigned int flags;
	bool do_map;
};

static unsigned long alloc_frame_before_final(void)
{
	unsigned long addr =
		memblock_alloc(PAGE_SIZE, _EKERNEL_PHYS, PAGE_SIZE);
	return addr;
}

static void *phys_to_virt_before_final(unsigned long paddr)
{
	return (void *)paddr;
}

static void final_pgtable_init(void)
{
	u64 idx = 0;
	struct mapping_region *rngs;
	size_t rngs_cnt = 0;
	size_t rngs_size;
	unsigned long start, end;

	for_each_mem_range(idx, start, end) {
		rngs_cnt += 1;
	}
	rngs_cnt += 7;
	klog_debug("rngs_cnt = %lu\n", rngs_cnt);

	rngs_size = sizeof(struct mapping_region) * rngs_cnt;
	rngs_size = round_up(rngs_size, PAGE_SIZE);
	rngs = (void *)memblock_alloc(rngs_size, _EKERNEL_PHYS, PAGE_SIZE);
	if (rngs == NULL) {
		sbi_console_putstr("rngs == NULL!\n");
		while (1) {
		}
	}
	memset(rngs, 0, rngs_size);

	rngs[0].name = "text";
	rngs[0].addr = _STEXT;
	rngs[0].paddr = _STEXT_PHYS;
	rngs[0].size = _TEXT_SIZE;
	rngs[0].flags = PTE_R | PTE_X;
	rngs[0].do_map = true;
	rngs[1].name = "rodata";
	rngs[1].addr = _SRODATA;
	rngs[1].paddr = _SRODATA_PHYS;
	rngs[1].size = _RODATA_SIZE;
	rngs[1].flags = PTE_R;
	rngs[1].do_map = true;
	rngs[2].name = "data";
	rngs[2].addr = _SDATA;
	rngs[2].paddr = _SDATA_PHYS;
	rngs[2].size = _DATA_SIZE;
	rngs[2].flags = PTE_R | PTE_W;
	rngs[2].do_map = true;
	rngs[3].name = "bss";
	rngs[3].addr = _SBSS;
	rngs[3].paddr = _SBSS_PHYS;
	rngs[3].size = _BSS_SIZE;
	rngs[3].flags = PTE_R | PTE_W;
	rngs[3].do_map = true;
	rngs[4].name = "temp_rngs_array";
	rngs[4].addr = phys_to_virt((unsigned long)rngs);
	rngs[4].paddr = (unsigned long)rngs;
	rngs[4].size = rngs_size;
	rngs[4].flags = PTE_R | PTE_W;
	rngs[4].do_map = true;

	rngs[5].name = "dtb";
	rngs[5].addr = phys_to_virt(boot_info.dtb_pa);
	rngs[5].paddr = boot_info.dtb_pa;
	rngs[5].size = fdt_totalsize(boot_info.dtb_pa);
	rngs[5].flags = PTE_R | PTE_W;
	rngs[5].do_map = true;
	size_t i = 6;
	for_each_mem_range(idx, start, end) {
		rngs[i].name = "free_memory";
		rngs[i].addr = phys_to_virt(start);
		rngs[i].paddr = start;
		rngs[i].size = end - start;
		rngs[i].flags = PTE_R | PTE_W;
		rngs[i].do_map = true;
		i += 1;
	}
	for (i = 5; i < rngs_cnt; ++i) {
		unsigned long aligned_addr = round_up(rngs[i].addr, PAGE_SIZE);
		unsigned long aligned_paddr =
			round_up(rngs[i].paddr, PAGE_SIZE);
		unsigned long aligned_end_addr =
			round_up(rngs[i].addr + rngs[i].size, PAGE_SIZE);
		rngs[i].addr = aligned_addr;
		rngs[i].paddr = aligned_paddr;
		rngs[i].size = aligned_end_addr - aligned_addr;
	}

	for (i = 0; i < rngs_cnt; ++i) {
		if (!rngs[i].do_map)
			break;

		klog_debug("mm: map %s: va=%#lx,pa=%#lx,sz=%lu\n", rngs[i].name,
			   rngs[i].addr, rngs[i].paddr, rngs[i].size);

		struct map_pages_context ctx = {
			.root = kernel_pgdir,
			.addr = (void *)rngs[i].addr,
			.paddr = rngs[i].paddr,
			.size = rngs[i].size,
			.flags = rngs[i].flags,
			.alloc_frame = alloc_frame_before_final,
			.phys_to_virt = phys_to_virt_before_final,
		};
		map_pages(&ctx);
	}
}

static unsigned long alloc_frame_after_final(void)
{
	return memblock_alloc(PAGE_SIZE, _EKERNEL_PHYS, PAGE_SIZE);
}

static void *phys_to_virt_after_final(unsigned long paddr)
{
	return (void *)phys_to_virt(paddr);
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
		struct map_pages_context ctx = {
			.root = kernel_pgdir,
			.addr = pfn_to_page(start),
			.paddr = paddr,
			.size = sz,
			.flags = PTE_R | PTE_W,
			.alloc_frame = alloc_frame_after_final,
			.phys_to_virt = phys_to_virt_after_final,
		};
		map_pages(&ctx);
	}
}

void load_final_page_table(void)
{
	u64 satp = make_satp(symbol_phys(kernel_pgdir));
	write_satp(satp);
	sfence_vma();
}

void paging_init(void)
{
	phys_mem_init();
	final_pgtable_init();
	load_final_page_table();
	vmemmap_init();
}

void switch_pgtable(pgd_t *pgd)
{
	u64 paddr = virt_to_phys((uintptr_t)pgd);
	u64 satp = make_satp(paddr);
	write_satp(satp);
	sfence_vma();
}
