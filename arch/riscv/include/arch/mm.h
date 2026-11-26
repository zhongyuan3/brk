#ifndef ARCH_MM_H
#define ARCH_MM_H

#include <arch/asm/page.h>
#include <arch/asm/vmlayout.h>
#include <brk/align.h>
#include <brk/types.h>
#include <riscv/boot_info.h>

extern phys_addr_t phys_offset;

extern char _skernel[], _ekernel[];
extern char _stext[], _etext[];
extern char _srodata[], _erodata[];
extern char _sdata[], _edata[];
extern char _sbss[], _ebss[];
extern char _secondary_start[];

extern virt_addr_t early_pgdir_rsvd_page_next;
extern char early_pgdir_rsvd_pages[];
extern char init_stack_rsvd_pages[];

/* Top of the static boot stack reserved for @cpuid. */
virt_addr_t arch_boot_stack_top(cpuid_t cpuid);

static inline phys_addr_t symbol_phys(const void *vaddr)
{
	return (phys_addr_t)((virt_addr_t)vaddr -
			     boot_info.kernel_image_offset);
}

#define _SKERNEL ((virt_addr_t)_skernel)
#define _EKERNEL ((virt_addr_t)_ekernel)
#define _KERNEL_SIZE ((phys_size_t)(_EKERNEL - _SKERNEL))
#define _SKERNEL_PHYS symbol_phys(_skernel)
#define _EKERNEL_PHYS symbol_phys(_ekernel)

#define _STEXT ((virt_addr_t)_stext)
#define _ETEXT ((virt_addr_t)_etext)
#define _TEXT_SIZE ((phys_size_t)(_ETEXT - _STEXT))
#define _STEXT_PHYS symbol_phys(_stext)
#define _ETEXT_PHYS symbol_phys(_etext)
#define _ETEXT_ALIGNED_PHYS align_up(_ETEXT_PHYS, PAGE_SIZE)

#define _SRODATA ((virt_addr_t)_srodata)
#define _ERODATA ((virt_addr_t)_erodata)
#define _RODATA_SIZE ((phys_size_t)(_ERODATA - _SRODATA))
#define _SRODATA_PHYS symbol_phys(_srodata)
#define _ERODATA_PHYS symbol_phys(_erodata)
#define _ERODATA_ALIGNED_PHYS align_up(_ERODATA_PHYS, PAGE_SIZE)

#define _SDATA ((virt_addr_t)_sdata)
#define _EDATA ((virt_addr_t)_edata)
#define _DATA_SIZE ((phys_size_t)(_EDATA - _SDATA))
#define _SDATA_PHYS symbol_phys(_sdata)
#define _EDATA_PHYS symbol_phys(_edata)
#define _EDATA_ALIGNED_PHYS align_up(_EDATA_PHYS, PAGE_SIZE)

#define _SBSS ((virt_addr_t)_sbss)
#define _EBSS ((virt_addr_t)_ebss)
#define _BSS_SIZE ((phys_size_t)(_EBSS - _SBSS))
#define _SBSS_PHYS symbol_phys(_sbss)
#define _EBSS_PHYS symbol_phys(_ebss)
#define _EBSS_ALIGNED_PHYS align_up(_EBSS_PHYS, PAGE_SIZE)

static inline virt_addr_t phys_to_virt(phys_addr_t paddr)
{
	return (virt_addr_t)((paddr - phys_offset) + PAGE_OFFSET);
}

static inline phys_addr_t virt_to_phys(virt_addr_t vaddr)
{
	return (phys_addr_t)((vaddr - PAGE_OFFSET) + phys_offset);
}

int vm_init(void);
void load_final_page_table(void);

#endif
