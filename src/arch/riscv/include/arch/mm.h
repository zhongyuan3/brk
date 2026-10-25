#ifndef ARCH_MM_H
#define ARCH_MM_H

#include <arch/asm/page.h>
#include <arch/asm/vmlayout.h>
#include <arch/boot_info.h>
#include <arch/pgtable_types.h>
#include <brk/asm/cpu.h>
#include <brk/kernel.h>
#include <brk/mm_types.h>
#include <brk/types.h>

extern unsigned long phys_offset;

extern char _skernel[], _ekernel[];
extern char _stext[], _etext[];
extern char _srodata[], _erodata[];
extern char _sdata[], _edata[];
extern char _sbss[], _ebss[];
extern char _secondary_start[];

extern unsigned long early_pgdir_rsvd_page_next;
extern char early_pgdir_rsvd_pages[];
extern char init_stack_rsvd_pages[];

static inline u64 symbol_phys(void *vaddr)
{
	return (uintptr_t)vaddr - boot_info.kernel_image_offset;
}

#define _SKERNEL ((unsigned long)_skernel)
#define _EKERNEL ((unsigned long)_ekernel)
#define _KERNEL_SIZE (_EKERNEL - _SKERNEL)
#define _SKERNEL_PHYS symbol_phys(_skernel)
#define _EKERNEL_PHYS symbol_phys(_ekernel)

#define _STEXT ((unsigned long)_stext)
#define _ETEXT ((unsigned long)_etext)
#define _TEXT_SIZE (_ETEXT - _STEXT)
#define _STEXT_PHYS symbol_phys(_stext)
#define _ETEXT_PHYS symbol_phys(_etext)
#define _ETEXT_ALIGNED_PHYS round_up(_ETEXT_PHYS, PAGE_SIZE)

#define _SRODATA ((unsigned long)_srodata)
#define _ERODATA ((unsigned long)_erodata)
#define _RODATA_SIZE (_ERODATA - _SRODATA)
#define _SRODATA_PHYS symbol_phys(_srodata)
#define _ERODATA_PHYS symbol_phys(_erodata)
#define _ERODATA_ALIGNED_PHYS round_up(_ERODATA_PHYS, PAGE_SIZE)

#define _SDATA ((unsigned long)_sdata)
#define _EDATA ((unsigned long)_edata)
#define _DATA_SIZE (_EDATA - _SDATA)
#define _SDATA_PHYS symbol_phys(_sdata)
#define _EDATA_PHYS symbol_phys(_edata)
#define _EDATA_ALIGNED_PHYS round_up(_EDATA_PHYS, PAGE_SIZE)

#define _SBSS ((unsigned long)_sbss)
#define _EBSS ((unsigned long)_ebss)
#define _BSS_SIZE (_EBSS - _SBSS)
#define _SBSS_PHYS symbol_phys(_sbss)
#define _EBSS_PHYS symbol_phys(_ebss)
#define _EBSS_ALIGNED_PHYS round_up(_EBSS_PHYS, PAGE_SIZE)

static inline uintptr_t phys_to_virt(u64 paddr)
{
	return (paddr - phys_offset) + PAGE_OFFSET;
}

static inline u64 virt_to_phys(uintptr_t vaddr)
{
	return (vaddr - PAGE_OFFSET) + phys_offset;
}

void paging_init(void);
void load_final_page_table(void);

#endif
