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
#include <brk/mchain.h>
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

static_assert(VMEMMAP_START % PAGE_SIZE == 0,
	      "VMEMMAP_START is not aligned to PAGE_SIZE");

phys_addr_t phys_offset;

virt_addr_t early_pgdir_rsvd_page_next;

static phys_addr_t alloc_early_pgdir_rsvd_page(void)
{
	phys_addr_t base = (phys_addr_t)(uintptr_t)early_pgdir_rsvd_pages;
	phys_addr_t end = base + NR_EARLY_PGDIR_RSVD_PAGES * PAGE_SIZE;

	if (early_pgdir_rsvd_page_next < end) {
		phys_addr_t addr = (phys_addr_t)early_pgdir_rsvd_page_next;
		early_pgdir_rsvd_page_next += PAGE_SIZE;
		return addr;
	}

	sbi_console_putstr("boot: reserved pages used up\n");
	while (1) {
	}
}

static phys_addr_t alloc_early_pgtable(void)
{
	return alloc_early_pgdir_rsvd_page();
}

static void *early_phys_to_virt(phys_addr_t paddr)
{
	return (void *)paddr;
}

static void map_early(virt_addr_t addr, phys_addr_t paddr, phys_size_t size,
		      unsigned int flags)
{
	struct map_pages_context ctx = {
		.root = (pgd_t *)early_pgdir_rsvd_pages,
		.addr = addr,
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

static void __setup_vm_early(phys_addr_t dtb_pa)
{
	virt_addr_t kern_end;
	phys_size_t size;
	unsigned int flags;

	early_pgdir_rsvd_page_next =
		(virt_addr_t)early_pgdir_rsvd_pages + PAGE_SIZE;

	kern_end = align_up(_EKERNEL, PAGE_SIZE_2M);
	size = kern_end - _SKERNEL;
	flags = PTE_R | PTE_W | PTE_X;

	/* Identity map so execution continues after satp is enabled. */
	map_early(_SKERNEL, _SKERNEL, size, flags);
	/* Map the kernel link address to the physical load address. */
	map_early(KERNEL_LINK_ADDR, _SKERNEL, size, flags);

	phys_addr_t dtb_end = dtb_pa + of_early_get_fdt_size(dtb_pa);
	if (dtb_pa >= _SKERNEL && dtb_end <= kern_end)
		return;

	if (dtb_pa < _SKERNEL)
		dtb_end = min(dtb_end, _SKERNEL);
	else
		dtb_pa = max(dtb_pa, kern_end);

	phys_addr_t map_start = align_down(dtb_pa, PAGE_SIZE_2M);
	phys_addr_t map_end = align_up(dtb_end, PAGE_SIZE_2M);
	size = map_end - map_start;
	flags = PTE_R | PTE_W;
	map_early(map_start, map_start, size, flags);
}

static void map_early_phys_range(phys_addr_t start, phys_addr_t end)
{
	phys_addr_t vstart = align_up(start, PAGE_SIZE);
	phys_addr_t vend = align_down(end, PAGE_SIZE);
	phys_size_t size;

	if (vstart >= vend)
		return;

	size = vend - vstart;
	map_early((virt_addr_t)vstart, vstart, size, PTE_R | PTE_W);
}

static void map_early_free_range(phys_addr_t start, phys_addr_t end)
{
	struct early_span {
		phys_addr_t start;
		phys_addr_t end;
	} spans[2] = {
		{ _SKERNEL_PHYS, align_up(_EKERNEL_PHYS, PAGE_SIZE_2M) },
		{ align_down(boot_info.dtb_pa, PAGE_SIZE_2M),
		  align_up(boot_info.dtb_pa +
				   of_early_get_fdt_size(boot_info.dtb_pa),
			   PAGE_SIZE_2M) },
	};
	struct early_span tmp;

	if (spans[0].start > spans[1].start) {
		tmp = spans[0];
		spans[0] = spans[1];
		spans[1] = tmp;
	}

	for (size_t i = 0; i < 2; ++i) {
		if (end <= spans[i].start)
			break;

		if (start < spans[i].start)
			map_early_phys_range(start, min(end, spans[i].start));

		if (end <= spans[i].end)
			return;

		if (start < spans[i].end)
			start = spans[i].end;
	}

	if (start < end)
		map_early_phys_range(start, end);
}

static void map_free_memory_early(void)
{
	struct mchain_iter it;
	phys_addr_t start, end;

	mchain_iter_init(&it, 0, MCHAIN_ANY_ADDR, MCHAIN_NID_ANY, 0);
	while (mchain_iter_next(&it, &start, &end)) {
		/*
		 * The final page table is built with mchain allocations.
		 * Until it is loaded, any allocated frame must be addressable
		 * through the early page table.  Identity-map every free
		 * physical range so those frames are always accessible.
		 *
		 * Skip ranges already covered by the early kernel/DTB
		 * mappings, otherwise map_pages() would see valid entries.
		 */
		map_early_free_range(start, end);
	}
}

void setup_vm_early(phys_addr_t dtb_pa, bool boot)
{
	if (boot) {
		__setup_vm_early(dtb_pa);
	}

	write_satp(make_satp((phys_addr_t)(uintptr_t)early_pgdir_rsvd_pages));
	sfence_vma();
	fence_i();
}

static int phys_mem_init(void)
{
	int err;

	err = of_early_scan_memory();
	if (err)
		return err;

	err = of_early_scan_reserved_memory();
	if (err)
		return err;

	/*
	 * Keep the firmware/bootloader area below the kernel image out of the
	 * allocator.  Older code used a min_addr on every allocation; reserving
	 * the range makes the mchain allocator safe.
	 */
	u64 dram_start = mchain_start_of_dram();
	if (dram_start < _SKERNEL_PHYS) {
		err = mchain_reserve(dram_start, _SKERNEL_PHYS - dram_start, 0,
				     MCHAIN_NID_ANY);
		if (err)
			return err;
	}

	err = mchain_reserve(_SKERNEL_PHYS, _KERNEL_SIZE, 0, MCHAIN_NID_ANY);
	if (err)
		return err;

	err = mchain_reserve(boot_info.dtb_pa,
			     of_early_get_fdt_size(boot_info.dtb_pa), 0,
			     MCHAIN_NID_ANY);
	if (err)
		return err;

	phys_offset = mchain_start_of_dram();

	min_pfn = phys_to_pfn(mchain_start_of_dram());
	max_pfn = phys_to_pfn(mchain_end_of_dram());
	vmemmap_start_pfn = min_pfn;

	return 0;
}

struct mapping_region {
	const char *name;
	virt_addr_t addr;
	phys_addr_t paddr;
	phys_size_t size;
	unsigned int flags;
	bool do_map;
};

static phys_addr_t alloc_frame_before_final(void)
{
	return mchain_alloc(PAGE_SIZE, PAGE_SIZE, 0, MCHAIN_ANY_ADDR,
			    MCHAIN_NID_ANY, 0);
}

static void *phys_to_virt_before_final(phys_addr_t paddr)
{
	return (void *)(uintptr_t)paddr;
}

static void final_pgtable_init(void)
{
	struct mchain_iter it;
	struct mapping_region *rngs;
	size_t rngs_cnt = 0;
	phys_size_t rngs_size;
	phys_addr_t start, end;

	mchain_iter_init(&it, 0, MCHAIN_ANY_ADDR, MCHAIN_NID_ANY, 0);
	while (mchain_iter_next(&it, &start, &end))
		rngs_cnt += 1;
	rngs_cnt += 7;

	rngs_size = sizeof(struct mapping_region) * rngs_cnt;
	rngs_size = align_up(rngs_size, PAGE_SIZE);
	rngs = (void *)mchain_alloc(rngs_size, PAGE_SIZE, 0, MCHAIN_ANY_ADDR,
				    MCHAIN_NID_ANY, 0);
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
	rngs[4].addr = phys_to_virt((phys_addr_t)(uintptr_t)rngs);
	rngs[4].paddr = (phys_addr_t)(uintptr_t)rngs;
	rngs[4].size = rngs_size;
	rngs[4].flags = PTE_R | PTE_W;
	rngs[4].do_map = true;

	rngs[5].name = "dtb";
	rngs[5].addr = phys_to_virt(boot_info.dtb_pa);
	rngs[5].paddr = boot_info.dtb_pa;
	rngs[5].size = of_early_get_fdt_size(boot_info.dtb_pa);
	rngs[5].flags = PTE_R | PTE_W;
	rngs[5].do_map = true;
	size_t i = 6;
	mchain_iter_init(&it, 0, MCHAIN_ANY_ADDR, MCHAIN_NID_ANY, 0);
	while (mchain_iter_next(&it, &start, &end)) {
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
		virt_addr_t aligned_addr = align_up(rngs[i].addr, PAGE_SIZE);
		phys_addr_t aligned_paddr = align_up(rngs[i].paddr, PAGE_SIZE);
		virt_addr_t aligned_end_addr =
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
			.addr = rngs[i].addr,
			.paddr = rngs[i].paddr,
			.size = rngs[i].size,
			.flags = rngs[i].flags,
			.alloc_frame = alloc_frame_before_final,
			.phys_to_virt = phys_to_virt_before_final,
		};
		map_pages(&ctx);
	}
}

static phys_addr_t alloc_frame_after_final(void)
{
	return mchain_alloc(PAGE_SIZE, PAGE_SIZE, 0, MCHAIN_ANY_ADDR,
			    MCHAIN_NID_ANY, 0);
}

static void *phys_to_virt_after_final(phys_addr_t paddr)
{
	return (void *)phys_to_virt(paddr);
}

static void vmemmap_map_range(pfn_t start_pfn, pfn_t end_pfn,
			      virt_addr_t *mapped_end)
{
	struct map_pages_context ctx;
	virt_addr_t va_start;
	virt_addr_t va_end;
	phys_addr_t paddr;
	phys_size_t size;

	if (start_pfn >= end_pfn)
		return;
	if (start_pfn < vmemmap_start_pfn || end_pfn > max_pfn)
		panic("mm: vmemmap: invalid pfn range [%lu,%lu)\n",
		      (unsigned long)start_pfn, (unsigned long)end_pfn);

	va_start = align_down((virt_addr_t)pfn_to_page(start_pfn), PAGE_SIZE);
	va_end = align_up((virt_addr_t)pfn_to_page(end_pfn), PAGE_SIZE);
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
	paddr = mchain_alloc(size, PAGE_SIZE, 0, MCHAIN_ANY_ADDR,
			     MCHAIN_NID_ANY, 0);
	assert(paddr);
	memset((void *)(uintptr_t)phys_to_virt(paddr), 0, size);

	pr_debug("vmemmap: va=%#lx,pa=%#lx,sz=%lu\n", va_start, paddr, size);

	ctx = (struct map_pages_context){
		.root = kernel_pgdir,
		.addr = va_start,
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
	phys_size_t nr_pfns;
	phys_size_t bytes;

	if (max_pfn < vmemmap_start_pfn)
		panic("mm: vmemmap: invalid pfn bounds [%lu,%lu)\n",
		      (unsigned long)vmemmap_start_pfn, (unsigned long)max_pfn);

	nr_pfns = max_pfn - vmemmap_start_pfn;
	if (nr_pfns > UINT64_MAX / sizeof(struct page))
		panic("mm: vmemmap: pfn range overflow\n");

	bytes = nr_pfns * sizeof(struct page);
	if (bytes > (u64)VMEMMAP_SIZE)
		panic("mm: vmemmap: need %lu bytes, available %lu\n",
		      (unsigned long)bytes, (unsigned long)VMEMMAP_SIZE);
}

static void vmemmap_init(void)
{
	struct mchain_entry *entry;
	virt_addr_t mapped_end = 0;
	phys_addr_t cur = 0;

	/*
	 * vmemmap_map_range() allocates page-table pages from mchain, which
	 * splits FREE entries while this walk is in progress.  Do not keep a
	 * saved next index: rescan from the head for the first entry whose end
	 * is above the physical cursor.  The union of all entries is stable
	 * even though allocations split individual free ranges.
	 */
	for (;;) {
		phys_addr_t entry_end;
		phys_addr_t rstart;
		phys_addr_t rend;
		int idx;

		entry = NULL;
		for (idx = mchain_pool_instance.head; idx != -1;
		     idx = mchain_pool_instance.entries[idx].next) {
			struct mchain_entry *candidate =
				&mchain_pool_instance.entries[idx];

			if (candidate->base + candidate->size > cur) {
				entry = candidate;
				break;
			}
		}

		if (!entry)
			break;

		entry_end = entry->base + entry->size;
		rstart = align_up(entry->base, PAGE_SIZE);
		rend = align_down(entry_end, PAGE_SIZE);

		if (rstart < rend)
			vmemmap_map_range(phys_to_pfn(rstart),
					  phys_to_pfn(rend), &mapped_end);

		cur = entry_end;
	}
}

void load_final_page_table(void)
{
	u64 satp = make_satp(symbol_phys(kernel_pgdir));
	write_satp(satp);
	sfence_vma();
}

int vm_init(void)
{
	int err = phys_mem_init();
	if (err)
		return err;

	map_free_memory_early();
	final_pgtable_init();
	load_final_page_table();
	mchain_allow_grow();
	of_early_set_fdt(phys_to_virt(boot_info.dtb_pa));
	vmemmap_check_capacity();
	vmemmap_init();

	return 0;
}

void switch_pgtable(pgd_t *pgd)
{
	phys_addr_t paddr = virt_to_phys((virt_addr_t)pgd);
	u64 satp = make_satp(paddr);
	write_satp(satp);
	sfence_vma();
}
