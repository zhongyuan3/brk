#ifndef BRK_MM_H
#define BRK_MM_H

#include <brk/align.h>
#include <brk/asm.h>
#include <brk/assert.h>
#include <brk/mm_types.h>
#include <brk/printk.h>
#include <brk/types.h>
#include <brk/vmalloc.h>

/* Offset between kernel mapping virtual address and kernel load address */
extern size_t kernel_load_offset;
/* Offset between the physical address of the RAM starting address and 0 */
extern uint64_t ram_phys_offset;

extern char _skernel[], _ekernel[];
extern char _stext[], _etext[];
extern char _srodata[], _erodata[];
extern char _sdata[], _edata[];
extern char _sbss[], _ebss[];
extern char init_stack[], init_stack_top[];
extern char hart_entry[];

static inline uint64_t symbol_phys(void *vaddr)
{
	return (uint64_t)vaddr + kernel_load_offset;
}

#define _SKERNEL_PHYS symbol_phys(_skernel)
#define _EKERNEL_PHYS symbol_phys(_ekernel)
#define _KERNEL_SIZE ((uint64_t)_ekernel - (uint64_t)_skernel)

#define _STEXT_PHYS symbol_phys(_stext)
#define _ETEXT_PHYS symbol_phys(_etext)
#define _ETEXT_ALIGNED_PHYS align_up(_ETEXT_PHYS, PAGE_SIZE)
#define _TEXT_SIZE ((uint64_t)_etext - (uint64_t)_stext)

#define _SRODATA_PHYS symbol_phys(_srodata)
#define _ERODATA_PHYS symbol_phys(_erodata)
#define _ERODATA_ALIGNED_PHYS align_up(_ERODATA_PHYS, PAGE_SIZE)
#define _RODATA_SIZE ((uint64_t)_erodata - (uint64_t)_srodata)

#define _SDATA_PHYS symbol_phys(_sdata)
#define _EDATA_PHYS symbol_phys(_edata)
#define _EDATA_ALIGNED_PHYS align_up(_EDATA_PHYS, PAGE_SIZE)
#define _DATA_SIZE ((uint64_t)_edata - (uint64_t)_sdata)

#define _SBSS_PHYS symbol_phys(_sbss)
#define _EBSS_PHYS symbol_phys(_ebss)
#define _EBSS_ALIGNED_PHYS align_up(_EBSS_PHYS, PAGE_SIZE)
#define _BSS_SIZE ((uint64_t)_ebss - (uint64_t)_sbss)

static inline uint64_t phys_to_virt(uint64_t paddr)
{
	return (paddr - ram_phys_offset) + PAGE_OFFSET;
}

static inline uint64_t virt_to_phys(uint64_t vaddr)
{
	return (vaddr - PAGE_OFFSET) + ram_phys_offset;
}

void vmemmap_init(void);
void setup_final_pgtable(void);
void switch_pgtable(pgde_t *pgd);

void mm_cache_init(void);
struct mm_struct *mm_alloc(void);
void mm_free(struct mm_struct *mm);
int mm_copy(struct mm_struct *dst, struct mm_struct *src);

#endif
