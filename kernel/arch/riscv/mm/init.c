#include <arch/csr.h>
#include <arch/mm.h>
#include <arch/pgtable.h>
#include <brk/assert.h>
#include <brk/dtb.h>
#include <brk/kernel.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/types.h>
#include <brk/vmalloc.h>
#include <libfdt.h>

size_t load_offset;
uint64_t phys_ram_base;

static void map_mem(void)
{
	uint64_t idx;
	uint64_t start, end;
	struct memblock_region *regs, *reg;
	size_t regs_cnt = 0;
	size_t size;

	for_each_mem_range(idx, start, end)
		++regs_cnt;

	size = round_up(sizeof(struct memblock_region) * regs_cnt, PAGE_SIZE);
	regs = (struct memblock_region *)memblock_alloc(size, _EKERNEL_PHYS,
							PAGE_SIZE);

	reg = regs;
	for_each_mem_range(idx, start, end) {
		reg->base = start;
		reg->size = end - start;
		++reg;
	}

	unsigned int flags = PTE_R | PTE_W;
	uint64_t vaddr;

	for (size_t i = 0; i < regs_cnt; ++i) {
		vaddr = phys_to_virt(regs[i].base);
		kvmap_with_mode(vaddr, regs[i].size, regs[i].base, flags,
				VMAP_MODE_EARLY);
	}

	vaddr = phys_to_virt((uint64_t)regs);
	kvmap_with_mode(vaddr, size, (uint64_t)regs, flags, VMAP_MODE_EARLY);

	memblock_free((uint64_t)regs, size);
}

static void map_kernel(void)
{
	uint64_t virt;

	kvmap_with_mode((uint64_t)_stext, _TEXT_SIZE, _STEXT_PHYS,
			PTE_R | PTE_X, VMAP_MODE_EARLY);
	kvmap_with_mode((uint64_t)_srodata, _RODATA_SIZE, _SRODATA_PHYS, PTE_R,
			VMAP_MODE_EARLY);
	kvmap_with_mode((uint64_t)_sdata, _DATA_SIZE, _SDATA_PHYS,
			PTE_R | PTE_W, VMAP_MODE_EARLY);
	kvmap_with_mode((uint64_t)_sbss, _BSS_SIZE, _SBSS_PHYS, PTE_R | PTE_W,
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
	uint64_t dtb_virt = phys_to_virt(dtb_phys);
	size_t dtb_size = fdt_totalsize(dtb_phys);
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
	uint32_t idx;
	uint64_t start, end;
	size_t sz;
	uint64_t paddr;

	for_each_mem_pfn_range(idx, start, end) {
		sz = round_up((end - start) * sizeof(struct page), PAGE_SIZE);
		paddr = memblock_alloc(sz, 0, PAGE_SIZE);
		ASSERT(paddr);
		memset((void *)phys_to_virt(paddr), 0, sz);
		kvmap_with_mode((uint64_t)pfn_to_page(start), sz, paddr,
				PTE_R | PTE_W, VMAP_MODE_INTERIM);
	}
}

void final_pgtable_enable(void)
{
	uint64_t satp = make_satp_sv39(symbol_phys(kernel_pgdir));
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
	uint64_t paddr = virt_to_phys((uint64_t)pgd);
	uint64_t satp = make_satp_sv39(paddr);
	write_satp(satp);
	sfence_vma();
}
