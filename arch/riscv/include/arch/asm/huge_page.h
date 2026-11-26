#ifndef _ARCH_ASM_HUGE_PAGE_H
#define _ARCH_ASM_HUGE_PAGE_H

/* RV64 supports 2MiB and 1GiB superpages. */
#define PAGE_SHIFT_2M 21
#define PAGE_SHIFT_1G 30

#define PAGE_SIZE_2M 0x200000
#define PAGE_SIZE_1G 0x40000000

#endif
