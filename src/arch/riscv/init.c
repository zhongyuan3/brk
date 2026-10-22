#include <arch/asm/page.h>
#include <arch/asm/vmlayout.h>
#include <arch/csr.h>
#include <arch/large_page.h>
#include <arch/mm.h>
#include <arch/pgtable.h>
#include <arch/sbi.h>
#include <brk/asm/cpu.h>
#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/init.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/of.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/task.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <brk/vmalloc.h>
#include <libfdt.h>

void primary_entry(unsigned long hart_id, unsigned long dtb_pa,
		   unsigned long kernel_va_pa_offset)
{
	boot_cpuid = hart_id;
	dtb_phys = dtb_pa;
	kernel_map.kernel_va_pa_offset = kernel_va_pa_offset;
	cpu_init(hart_id);

	start_kernel();
}

void secondary_entry(unsigned long hart_id)
{
	cpu_init(hart_id);
	final_pgtable_enable();

	irq_init_hart(hart_id);
	/* Device IRQs are handled on the boot hart only (no IPI support yet). */
	trap_init_hart(hart_id);

	intr_on();

	klog_info("hart %lu ready\n", hart_id);

	task_scheduler();
}

void arch_init(void)
{
	memblock_reserve(_SKERNEL_PHYS, _KERNEL_SIZE);
	size_t dtb_size =
		round_up(fdt_totalsize((uintptr_t)dtb_phys), PAGE_SIZE);
	memblock_reserve(dtb_phys, dtb_size);
	dtb_early_init_scan_mem();
	dtb_early_init_scan_reserved_mem();
	phys_ram_base = memblock_get_ram_base();
	paging_init();
}

static void smp_wake_secondary_harts(u64 init_hart_id)
{
	u64 start_addr = symbol_phys(_secondary_start);

	for (u64 id = 0; id < MAX_CPUS; ++id) {
		if (id == init_hart_id)
			continue;
		sbi_hart_start(id, start_addr, 0);
	}
}

void smp_boot_release_secondary_harts(void)
{
	static bool released;

	if (released)
		return;
	released = true;
	smp_wake_secondary_harts(boot_cpuid);
}

struct kernel_mapping kernel_map;
u64 phys_ram_base;

u64 rsv_page_next;
u8 rsv_pages[PAGE_SIZE * NR_RSV_PAGES] __attribute__((aligned(PAGE_SIZE)));

static u64 alloc_early_rsv_page(void)
{
	unsigned long base = (unsigned long)rsv_pages;
	unsigned long end = base + NR_EARLY_PGDIR_PAGES * PAGE_SIZE;

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
	pgd_t *pgdep;
	pmd_t *pmdep;
	u64 pmd_phys;
	u64 end_addr = addr + size;

	while (addr < end_addr) {
		pgdep = (pgd_t *)rsv_pages + pgd_index(addr);
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
}

static void __setup_vm_early(uintptr_t dtb)
{
	u64 kern_end;
	size_t size;
	unsigned int flags;

	rsv_page_next = (uintptr_t)rsv_pages;

	kern_end = round_up((uintptr_t)_ekernel, PAGE_SIZE_2M);
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

	u64 map_start = round_down(dtb, PAGE_SIZE_2M);
	u64 map_end = round_up(dtb_end, PAGE_SIZE_2M);
	size = map_end - map_start;
	flags = PTE_R | PTE_W;
	map_early(map_start, size, map_start, flags);
}

void setup_vm_early(uintptr_t dtb, bool boot)
{
	if (boot) {
		__setup_vm_early(dtb);
	}

	write_satp(make_satp((uintptr_t)rsv_pages));
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
