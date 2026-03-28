#include <aosd/align.h>
#include <aosd/dtb.h>
#include <aosd/memblock.h>
#include <aosd/mm.h>
#include <aosd/mm_types.h>
#include <aosd/panic.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/riscv.h>
#include <aosd/vmalloc.h>
#include <libfdt.h>

size_t kernel_load_offset;
uint64_t ram_phys_offset;

static void map_mem(void)
{
	uint64_t idx;
	uint64_t start, end;
	struct memblock_region *regs, *reg;
	size_t regs_cnt = 0;
	size_t size;

	for_each_mem_range(idx, start, end)
		++regs_cnt;

	size = align_up(sizeof(struct memblock_region) * regs_cnt, PAGE_SIZE);
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

static uint64_t make_final_pgtable(void)
{
	map_mem();
	map_kernel();
	map_dtb();
	return make_satp_sv39(symbol_phys(kernel_pgdir));
}

void setup_final_pgtable(void)
{
	write_satp(make_final_pgtable());
	sfence_vma();
}

void vmemmap_init(void)
{
	uint32_t idx;
	uint64_t start, end;
	size_t sz;
	uint64_t paddr;

	for_each_mem_pfn_range(idx, start, end) {
		sz = align_up((end - start) * sizeof(struct page), PAGE_SIZE);
		paddr = memblock_alloc(sz, 0, PAGE_SIZE);
		assert(paddr);
		memset((void *)phys_to_virt(paddr), 0, sz);
		kvmap_with_mode((uint64_t)pfn_to_page(start), sz, paddr,
				PTE_R | PTE_W, VMAP_MODE_INTERIM);
	}
}

void switch_pgtable(pgde_t *pgd)
{
	uint64_t paddr = virt_to_phys((uint64_t)pgd);
	uint64_t satp = make_satp_sv39(paddr);
	write_satp(satp);
	sfence_vma();
}
