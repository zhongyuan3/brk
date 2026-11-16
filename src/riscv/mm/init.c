#define pr_fmt(fmt) "mm: " fmt
#include <arch/asm/huge_page.h>
#include <arch/asm/mm.h>
#include <arch/asm/page.h>
#include <arch/asm/vmlayout.h>
#include <arch/boot_info.h>
#include <arch/csr.h>
#include <arch/mm.h>
#include <arch/pgtable.h>
#include <arch/pgtable_types.h>
#include <arch/sbi.h>
#include <brk/align.h>
#include <brk/assert.h>
#include <brk/memblock.h>
#include <brk/min_max.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/of.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/types.h>
#include <stddef.h>
#include <string.h>

static_assert(VMEMMAP_START % PAGE_SIZE == 0);

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

static unsigned long alloc_early_pgtable(void)
{
	return alloc_early_pgdir_rsvd_page();
}

static void *early_phys_to_virt(unsigned long paddr)
{
	return (void *)paddr;
}

static void map_early(unsigned long addr, unsigned long paddr,
		      unsigned long size, unsigned int flags)
{
	struct map_pages_context ctx = {
		.root = (pgd_t *)early_pgdir_rsvd_pages,
		.addr = (void *)addr,
		.paddr = paddr,
		.size = size,
		.flags = flags,
		.alloc_frame = alloc_early_pgtable,
		.phys_to_virt = early_phys_to_virt,
	};
	int err = map_pages(&ctx);

	if (err) {
		sbi_console_putstr("boot: map_early failed\n");
		while (1) {
		}
	}
}

static void __setup_vm_early(unsigned long dtb_pa)
{
	unsigned long kern_end;
	unsigned long size;
	unsigned int flags;

	early_pgdir_rsvd_page_next =
		(unsigned long)early_pgdir_rsvd_pages + PAGE_SIZE;

	kern_end = align_up(_EKERNEL, PAGE_SIZE_2M);
	size = kern_end - _SKERNEL;
	flags = PTE_R | PTE_W | PTE_X;

	/* Identity map so execution continues after satp is enabled. */
	map_early(_SKERNEL, _SKERNEL, size, flags);
	/* Map the kernel link address to the physical load address. */
	map_early(KERNEL_LINK_ADDR, _SKERNEL, size, flags);

	unsigned long dtb_end = dtb_pa + of_get_fdt_size(dtb_pa);
	if (dtb_pa >= _SKERNEL && dtb_end <= kern_end)
		return;

	if (dtb_pa < _SKERNEL)
		dtb_end = min(dtb_end, _SKERNEL);
	else
		dtb_pa = max(dtb_pa, kern_end);

	unsigned long map_start = align_down(dtb_pa, PAGE_SIZE_2M);
	unsigned long map_end = align_up(dtb_end, PAGE_SIZE_2M);
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

static int phys_mem_init(void)
{
	int err;

	err = of_scan_memory();
	if (err)
		return err;

	err = of_scan_reserved_memory();
	if (err)
		return err;

	/*
	 * Keep the firmware/bootloader area below the kernel image out of the
	 * allocator.  Older code used a min_addr on every allocation; reserving
	 * the range makes the Linux-style memblock_phys_alloc() safe.
	 */
	u64 dram_start = memblock_start_of_dram();
	if (dram_start < _SKERNEL_PHYS) {
		err = memblock_reserve(dram_start, _SKERNEL_PHYS - dram_start);
		if (err)
			return err;
	}

	err = memblock_reserve(_SKERNEL_PHYS, _KERNEL_SIZE);
	if (err)
		return err;

	err = memblock_reserve(boot_info.dtb_pa,
			       of_get_fdt_size(boot_info.dtb_pa));
	if (err)
		return err;

	phys_offset = memblock_start_of_dram();

	min_pfn = phys_to_pfn(memblock_start_of_dram());
	max_pfn = phys_to_pfn(memblock_end_of_dram());
	vmemmap_start_pfn = min_pfn;

	return 0;
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
	unsigned long addr = memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE);

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

	for_each_free_mem_range(idx, start, end)
	{
		rngs_cnt += 1;
	}
	rngs_cnt += 7;

	rngs_size = sizeof(struct mapping_region) * rngs_cnt;
	rngs_size = align_up(rngs_size, PAGE_SIZE);
	rngs = (void *)memblock_phys_alloc(rngs_size, PAGE_SIZE);
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
	rngs[4].name = "tmp_rngs_list";
	rngs[4].addr = phys_to_virt((unsigned long)rngs);
	rngs[4].paddr = (unsigned long)rngs;
	rngs[4].size = rngs_size;
	rngs[4].flags = PTE_R | PTE_W;
	rngs[4].do_map = true;

	rngs[5].name = "dtb";
	rngs[5].addr = phys_to_virt(boot_info.dtb_pa);
	rngs[5].paddr = boot_info.dtb_pa;
	rngs[5].size = of_get_fdt_size(boot_info.dtb_pa);
	rngs[5].flags = PTE_R | PTE_W;
	rngs[5].do_map = true;
	size_t i = 6;
	for_each_free_mem_range(idx, start, end)
	{
		pr_debug("start=%#lx,end=%#lx\n", start, end);
		rngs[i].name = "free_memory";
		rngs[i].addr = phys_to_virt(start);
		rngs[i].paddr = start;
		rngs[i].size = end - start;
		rngs[i].flags = PTE_R | PTE_W;
		rngs[i].do_map = true;
		i += 1;
	}
	for (i = 5; i < rngs_cnt; ++i) {
		unsigned long aligned_addr = align_up(rngs[i].addr, PAGE_SIZE);
		unsigned long aligned_paddr =
			align_up(rngs[i].paddr, PAGE_SIZE);
		unsigned long aligned_end_addr =
			align_up(rngs[i].addr + rngs[i].size, PAGE_SIZE);
		rngs[i].addr = aligned_addr;
		rngs[i].paddr = aligned_paddr;
		rngs[i].size = aligned_end_addr - aligned_addr;
	}

	for (i = 0; i < rngs_cnt; ++i) {
		if (!rngs[i].do_map)
			break;

		pr_debug("map %s: va=%#lx,pa=%#lx,sz=%lu\n", rngs[i].name,
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
	return memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE);
}

static void *phys_to_virt_after_final(unsigned long paddr)
{
	return (void *)phys_to_virt(paddr);
}

static void vmemmap_map_range(u64 start_pfn, u64 end_pfn,
			      unsigned long *mapped_end)
{
	struct map_pages_context ctx;
	unsigned long va_start;
	unsigned long va_end;
	unsigned long paddr;
	size_t size;

	if (start_pfn >= end_pfn)
		return;
	if (start_pfn < vmemmap_start_pfn || end_pfn > max_pfn)
		panic("mm: vmemmap: invalid pfn range [%lu,%lu)\n",
		      (unsigned long)start_pfn, (unsigned long)end_pfn);

	va_start = align_down((unsigned long)pfn_to_page(start_pfn), PAGE_SIZE);
	va_end = align_up((unsigned long)pfn_to_page(end_pfn), PAGE_SIZE);
	/*
	 * Adjacent physical ranges may share the last vmemmap page.  The
	 * previous mapping already provides zeroed struct pages for that
	 * overlap, so only map the range that is still unmapped.
	 */
	if (va_start < *mapped_end)
		va_start = *mapped_end;
	if (va_start >= va_end)
		return;

	size = va_end - va_start;
	paddr = memblock_phys_alloc(size, PAGE_SIZE);
	assert(paddr);
	memset((void *)phys_to_virt(paddr), 0, size);

	pr_debug("vmemmap: va=%#lx,pa=%#lx,sz=%lu\n", va_start, paddr, size);

	ctx = (struct map_pages_context){
		.root = kernel_pgdir,
		.addr = (void *)va_start,
		.paddr = paddr,
		.size = size,
		.flags = PTE_R | PTE_W,
		.alloc_frame = alloc_frame_after_final,
		.phys_to_virt = phys_to_virt_after_final,
	};
	if (map_pages(&ctx))
		panic("mm: vmemmap: map_pages() failed\n");

	*mapped_end = va_end;
}

static void vmemmap_check_capacity(void)
{
	u64 nr_pfns;
	u64 bytes;

	if (max_pfn < vmemmap_start_pfn)
		panic("mm: vmemmap: invalid pfn bounds [%lu,%lu)\n",
		      (unsigned long)vmemmap_start_pfn, (unsigned long)max_pfn);

	nr_pfns = (u64)(max_pfn - vmemmap_start_pfn);
	if (nr_pfns > UINT64_MAX / sizeof(struct page))
		panic("mm: vmemmap: pfn range overflow\n");

	bytes = nr_pfns * sizeof(struct page);
	if (bytes > (u64)VMEMMAP_SIZE)
		panic("mm: vmemmap: need %lu bytes, available %lu\n",
		      (unsigned long)bytes, (unsigned long)VMEMMAP_SIZE);
}

static void vmemmap_init(void)
{
	struct memblock_region *rgn;
	unsigned long mapped_end = 0;
	u64 start_pa = 0;
	u64 end_pa = 0;
	bool have_span = false;

	/*
	 * Coalesce adjacent memory regions before mapping.  Region flags may
	 * split one contiguous physical range (for example around NOMAP
	 * memory), but vmemmap still covers all physical memory.
	 */
	for_each_mem_region(rgn)
	{
		u64 rstart = align_up(rgn->base, PAGE_SIZE);
		u64 rend = align_down(rgn->base + rgn->size, PAGE_SIZE);

		if (rstart >= rend)
			continue;

		if (!have_span) {
			start_pa = rstart;
			end_pa = rend;
			have_span = true;
			continue;
		}

		if (rstart == end_pa) {
			end_pa = rend;
			continue;
		}

		vmemmap_map_range(phys_to_pfn(start_pa), phys_to_pfn(end_pa),
				  &mapped_end);
		start_pa = rstart;
		end_pa = rend;
	}

	if (have_span)
		vmemmap_map_range(phys_to_pfn(start_pa), phys_to_pfn(end_pa),
				  &mapped_end);
}

void load_final_page_table(void)
{
	u64 satp = make_satp(symbol_phys(kernel_pgdir));
	write_satp(satp);
	sfence_vma();
}

int paging_init(void)
{
	int err;

	err = phys_mem_init();
	if (err != 0)
		return err;

	final_pgtable_init();
	load_final_page_table();
	of_set_fdt(phys_to_virt(boot_info.dtb_pa));
	vmemmap_check_capacity();
	vmemmap_init();

	return 0;
}

void switch_pgtable(pgd_t *pgd)
{
	u64 paddr = virt_to_phys((uintptr_t)pgd);
	u64 satp = make_satp(paddr);
	write_satp(satp);
	sfence_vma();
}
