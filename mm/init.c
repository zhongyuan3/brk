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

static uint64_t alloc_pgtable_before_final(void)
{
	uint64_t addr = memblock_alloc(PAGE_SIZE, _EKERNEL_PHYS, PAGE_SIZE);
	if (!addr)
		panic("%s(): memblock_alloc() failed\n", __func__);
	memset((void *)addr, 0, PAGE_SIZE);
	return addr;
}

static uint64_t alloc_pgd_before_final(void)
{
	return alloc_pgtable_before_final();
}

static uint64_t alloc_pmd_before_final(void)
{
	return alloc_pgtable_before_final();
}

static uint64_t alloc_pt_before_final(void)
{
	return alloc_pgtable_before_final();
}

static pgde_t *get_pgd_virt_before_final(uint64_t pgd_phys)
{
	return (pgde_t *)pgd_phys;
}

static pmde_t *get_pmd_virt_before_final(uint64_t pmd_phys)
{
	return (pmde_t *)pmd_phys;
}

static pte_t *get_pt_virt_before_final(uint64_t pt_phys)
{
	return (pte_t *)pt_phys;
}

static void vmap_before_final(uint64_t addr, size_t size, uint64_t paddr,
			      unsigned int flags)
{
	struct vmap_ops ops = {
		.alloc_pgd = alloc_pgd_before_final,
		.alloc_pmd = alloc_pmd_before_final,
		.alloc_pt = alloc_pt_before_final,
		.get_pgd_virt = get_pgd_virt_before_final,
		.get_pmd_virt = get_pmd_virt_before_final,
		.get_pt_virt = get_pt_virt_before_final,
	};
	size = align_up(size, PAGE_SIZE);
	spinlock_acquire(&kernel_pgdir_lock);
	vmap(kernel_pgdir, addr, size, paddr, flags, &ops);
	spinlock_release(&kernel_pgdir_lock);
}

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
		vmap_before_final(vaddr, regs[i].size, regs[i].base, flags);
	}

	vaddr = phys_to_virt((uint64_t)regs);
	vmap_before_final(vaddr, size, (uint64_t)regs, flags);

	memblock_free((uint64_t)regs, size);
}

static void map_kernel(void)
{
	vmap_before_final((uint64_t)_stext, _TEXT_SIZE, _STEXT_PHYS,
			  PTE_R | PTE_X);
	vmap_before_final((uint64_t)_srodata, _RODATA_SIZE, _SRODATA_PHYS,
			  PTE_R);
	vmap_before_final((uint64_t)_sdata, _DATA_SIZE, _SDATA_PHYS,
			  PTE_R | PTE_W);
	vmap_before_final((uint64_t)_sbss, _BSS_SIZE, _SBSS_PHYS,
			  PTE_R | PTE_W);
	uint64_t virt = phys_to_virt(_SRODATA_PHYS);
	vmap_before_final(virt, _RODATA_SIZE, _SRODATA_PHYS, PTE_R);
	virt = phys_to_virt(_SDATA_PHYS);
	vmap_before_final(virt, _DATA_SIZE, _SDATA_PHYS, PTE_R | PTE_W);
	virt = phys_to_virt(_SBSS_PHYS);
	vmap_before_final(virt, _BSS_SIZE, _SBSS_PHYS, PTE_R | PTE_W);
}

static void map_dtb(void)
{
	uint64_t dtb_virt = phys_to_virt(dtb_phys);
	size_t dtb_size = fdt_totalsize(dtb_phys);
	vmap_before_final(dtb_virt, dtb_size, dtb_phys, PTE_R | PTE_W);
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

static uint64_t alloc_pgtable_before_buddy(void)
{
	uint64_t addr = memblock_alloc(PAGE_SIZE, _EKERNEL_PHYS, PAGE_SIZE);
	if (!addr)
		panic("%s(): memblock_alloc() failed\n", __func__);
	memset((void *)phys_to_virt(addr), 0, PAGE_SIZE);
	return addr;
}

static uint64_t alloc_pgd_before_buddy(void)
{
	return alloc_pgtable_before_buddy();
}

static uint64_t alloc_pmd_before_buddy(void)
{
	return alloc_pgtable_before_buddy();
}

static uint64_t alloc_pt_before_buddy(void)
{
	return alloc_pgtable_before_buddy();
}

static pgde_t *get_pgd_virt_before_buddy(uint64_t pgd_phys)
{
	return (pgde_t *)phys_to_virt(pgd_phys);
}

static pmde_t *get_pmd_virt_before_buddy(uint64_t pmd_phys)
{
	return (pmde_t *)phys_to_virt(pmd_phys);
}

static pte_t *get_pt_virt_before_buddy(uint64_t pt_phys)
{
	return (pte_t *)phys_to_virt(pt_phys);
}

static void vmap_before_buddy(uint64_t addr, size_t size, uint64_t paddr,
			      unsigned int flags)
{
	struct vmap_ops ops = {
		.alloc_pgd = alloc_pgd_before_buddy,
		.alloc_pmd = alloc_pmd_before_buddy,
		.alloc_pt = alloc_pt_before_buddy,
		.get_pgd_virt = get_pgd_virt_before_buddy,
		.get_pmd_virt = get_pmd_virt_before_buddy,
		.get_pt_virt = get_pt_virt_before_buddy,
	};
	size = align_up(size, PAGE_SIZE);
	spinlock_acquire(&kernel_pgdir_lock);
	vmap(kernel_pgdir, addr, size, paddr, flags, &ops);
	spinlock_release(&kernel_pgdir_lock);
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
		vmap_before_buddy((uint64_t)pfn_to_page(start), sz, paddr,
				  PTE_R | PTE_W);
	}
}

void switch_pgtable(pgde_t *pgd)
{
	uint64_t paddr = virt_to_phys((uint64_t)pgd);
	uint64_t satp = make_satp_sv39(paddr);
	write_satp(satp);
	sfence_vma();
}
