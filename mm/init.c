#include <brk/dtb.h>
#include <brk/kernel.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/pgtable.h>
#include <brk/riscv.h>
#include <brk/vmalloc.h>
#include <libfdt.h>

usize_t kernel_load_offset;
u64 ram_phys_offset;

static void map_mem(void)
{
	u64 idx;
	u64 start, end;
	struct memblock_region *regs, *reg;
	usize_t regs_cnt = 0;
	usize_t size;

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
	u64 vaddr;

	for (usize_t i = 0; i < regs_cnt; ++i) {
		vaddr = phys_to_virt(regs[i].base);
		kvmap_with_mode(vaddr, regs[i].size, regs[i].base, flags,
				VMAP_MODE_EARLY);
	}

	vaddr = phys_to_virt((u64)regs);
	kvmap_with_mode(vaddr, size, (u64)regs, flags, VMAP_MODE_EARLY);

	memblock_free((u64)regs, size);
}

static void map_kernel(void)
{
	u64 virt;

	kvmap_with_mode((u64)_stext, _TEXT_SIZE, _STEXT_PHYS, PTE_R | PTE_X,
			VMAP_MODE_EARLY);
	kvmap_with_mode((u64)_srodata, _RODATA_SIZE, _SRODATA_PHYS, PTE_R,
			VMAP_MODE_EARLY);
	kvmap_with_mode((u64)_sdata, _DATA_SIZE, _SDATA_PHYS, PTE_R | PTE_W,
			VMAP_MODE_EARLY);
	kvmap_with_mode((u64)_sbss, _BSS_SIZE, _SBSS_PHYS, PTE_R | PTE_W,
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
	usize_t dtb_size = fdt_totalsize(dtb_phys);
	kvmap_with_mode(dtb_virt, dtb_size, dtb_phys, PTE_R | PTE_W,
			VMAP_MODE_EARLY);
}

static u64 make_final_pgtable(void)
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
	u32 idx;
	u64 start, end;
	usize_t sz;
	u64 paddr;

	for_each_mem_pfn_range(idx, start, end) {
		sz = round_up((end - start) * sizeof(struct page), PAGE_SIZE);
		paddr = memblock_alloc(sz, 0, PAGE_SIZE);
		ASSERT(paddr);
		memset((void *)phys_to_virt(paddr), 0, sz);
		kvmap_with_mode((u64)pfn_to_page(start), sz, paddr,
				PTE_R | PTE_W, VMAP_MODE_INTERIM);
	}
}

void switch_pgtable(pgde_t *pgd)
{
	u64 paddr = virt_to_phys((u64)pgd);
	u64 satp = make_satp_sv39(paddr);
	write_satp(satp);
	sfence_vma();
}
