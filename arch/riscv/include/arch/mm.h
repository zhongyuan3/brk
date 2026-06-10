#ifndef ARCH_MM_H
#define ARCH_MM_H

#include <arch/pgtable_types.h>
#include <arch/vmlayout.h>
#include <asm/page.h>
#include <brk/kernel.h>
#include <brk/mm_types.h>
#include <brk/types.h>

extern usize_t kernel_load_offset;
extern u64 ram_phys_offset;

extern char _skernel[], _ekernel[];
extern char _stext[], _etext[];
extern char _srodata[], _erodata[];
extern char _sdata[], _edata[];
extern char _sbss[], _ebss[];
extern char init_stack[];
extern char hart_entry[];

static inline u64 symbol_phys(void *vaddr)
{
	return (u64)vaddr + kernel_load_offset;
}

#define _SKERNEL_PHYS symbol_phys(_skernel)
#define _EKERNEL_PHYS symbol_phys(_ekernel)
#define _KERNEL_SIZE ((u64)_ekernel - (u64)_skernel)

#define _STEXT_PHYS symbol_phys(_stext)
#define _ETEXT_PHYS symbol_phys(_etext)
#define _ETEXT_ALIGNED_PHYS round_up(_ETEXT_PHYS, PAGE_SIZE)
#define _TEXT_SIZE ((u64)_etext - (u64)_stext)

#define _SRODATA_PHYS symbol_phys(_srodata)
#define _ERODATA_PHYS symbol_phys(_erodata)
#define _ERODATA_ALIGNED_PHYS round_up(_ERODATA_PHYS, PAGE_SIZE)
#define _RODATA_SIZE ((u64)_erodata - (u64)_srodata)

#define _SDATA_PHYS symbol_phys(_sdata)
#define _EDATA_PHYS symbol_phys(_edata)
#define _EDATA_ALIGNED_PHYS round_up(_EDATA_PHYS, PAGE_SIZE)
#define _DATA_SIZE ((u64)_edata - (u64)_sdata)

#define _SBSS_PHYS symbol_phys(_sbss)
#define _EBSS_PHYS symbol_phys(_ebss)
#define _EBSS_ALIGNED_PHYS round_up(_EBSS_PHYS, PAGE_SIZE)
#define _BSS_SIZE ((u64)_ebss - (u64)_sbss)

static inline u64 phys_to_virt(u64 paddr)
{
	return (paddr - ram_phys_offset) + PAGE_OFFSET;
}

static inline u64 virt_to_phys(u64 vaddr)
{
	return (vaddr - PAGE_OFFSET) + ram_phys_offset;
}

void vmemmap_init(void);
void setup_final_pgtable(void);
void switch_pgtable(pgde_t *pgd);
void arch_mm_activate(pgde_t *pgd);
void arch_mm_switch(pgde_t *pgd);

#endif
