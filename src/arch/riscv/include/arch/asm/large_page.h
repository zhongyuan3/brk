#ifndef ARCH_ASM_LARGE_PAGE_H
#define ARCH_ASM_LARGE_PAGE_H

/* RV64/SV39 supports 2MiB and 1GiB superpages. */
#define PAGE_SHIFT_2M 21
#define PAGE_SHIFT_1G 30

#define PAGE_SIZE_2M 0x200000
#define PAGE_SIZE_1G 0x40000000

#endif
