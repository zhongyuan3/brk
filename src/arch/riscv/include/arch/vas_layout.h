#ifndef ASM_VAS_LAYOUT_H
#define ASM_VAS_LAYOUT_H

#include <arch/page.h>

#if __riscv_xlen == 32
/*
 * RV32 4 GiB address space layout:
 *   0x00000000 - 0x7fffffff  user space (2 GiB)
 *   0xc0000000 - 0xdfffffff  direct map (512 MiB)
 *   0xe0000000 - 0xefffffff  vmalloc (256 MiB)
 *   0xf0000000 - 0xffffffff  vmemmap (256 MiB)
 */
#define USER_SPACE_SIZE_MAX ((1UL << 31) - PAGE_SIZE)

#define PAGE_OFFSET 0xc0000000
#define LINEAR_MAPPING_SIZE 0x20000000 /* 512M */

#define VMALLOC_START 0xe0000000
#define VMALLOC_SIZE 0x10000000 /* 256M */

#define VMEMMAP_START 0xf0000000
#define VMEMMAP_SIZE 0x10000000 /* 256M */
#else
#define USER_SPACE_SIZE_MAX ((1UL << 38) - PAGE_SIZE)

#define PAGE_OFFSET 0xffffffc000000000
#define LINEAR_MAPPING_SIZE 0x2000000000 /* 128G */

#define VMALLOC_START 0xffffffe000000000
#define VMALLOC_SIZE 0x1000000000 /* 64G */

#define VMEMMAP_START 0xfffffff000000000
#define VMEMMAP_SIZE 0x100000000 /* 4G */
#endif

#endif
