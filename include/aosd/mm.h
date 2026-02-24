#ifndef AOSD_MM_H
#define AOSD_MM_H

#include <aosd/align.h>
#include <aosd/asm.h>
#include <aosd/mm_types.h>
#include <aosd/types.h>

struct kernel_mapping {
	/* Offset between kernel mapping virtual address and kernel load address */
	size_t load_offset;
	/* Offset between the physical address of the RAM starting address and 0 */
	uint64_t phys_offset;
};

extern struct kernel_mapping kernel_map;

extern char _skernel[], _ekernel[];
extern char _stext[], _etext[];
extern char _stexthead[], _etexthead[];
extern char _srodata[], _erodata[];
extern char _sdata[], _edata[];
extern char _sbss[], _ebss[];
extern char init_stack[], init_stack_top[];
extern char hart_entry[];

static inline uint64_t symbol_phys(void *vaddr)
{
	return (uint64_t)vaddr + kernel_map.load_offset;
}

#define _SKERNEL_PHYS symbol_phys(_skernel)
#define _EKERNEL_PHYS symbol_phys(_ekernel)
#define _KERNEL_SIZE ((uint64_t)_ekernel - (uint64_t)_skernel)

#define _STEXT_PHYS symbol_phys(_stext)
#define _ETEXT_PHYS symbol_phys(_etext)
#define _ETEXT_ALIGNED_PHYS align_up(_ETEXT_PHYS, PAGE_SIZE)
#define _TEXT_SIZE ((uint64_t)_etext - (uint64_t)_stext)

#define _STEXTHEAD_PHYS symbol_phys(_stexthead)
#define _ETEXTHEAD_PHYS symbol_phys(_etexthead)
#define _ETEXTHEAD_ALIGNED_PHYS align_up(_ETEXTHEAD_PHYS, PAGE_SIZE)
#define _TEXTHEAD_SIZE ((uint64_t)_etexthead - (uint64_t)_stexthead)

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

#define INIT_STACK_PHYS symbol_phys(init_stack)
#define INIT_STACK_TOP_PHYS symbol_phys(init_stack_top)
#define INIT_STACK_TOP_ALIGNED_PHYS align_up(INIT_STACK_TOP_PHYS, PAGE_SIZE)
#define INIT_STACK_SIZE ((uint64_t)init_stack_top - (uint64_t)init_stack)

static inline uint64_t phys_to_virt(uint64_t paddr)
{
	return (paddr - kernel_map.phys_offset) + PAGE_OFFSET;
}

static inline uint64_t virt_to_phys(uint64_t vaddr)
{
	return (vaddr - PAGE_OFFSET) + kernel_map.phys_offset;
}

void mm_init(void);

#endif
