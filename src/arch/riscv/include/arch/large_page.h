#ifndef ASM_LARGE_PAGE_H
#define ASM_LARGE_PAGE_H

/*
 * RV64/SV39 supports 2MiB and 1GiB superpages.
 * RV32/Sv32 supports only 4MiB superpages, encoded in the top-level
 * (PGD) entries.
 */
#if __riscv_xlen == 32

#define PAGE_SHIFT_4M 22
#define PAGE_SIZE_4M 0x400000

#else

#define PAGE_SHIFT_2M 21
#define PAGE_SHIFT_1G 30

#define PAGE_SIZE_2M 0x200000
#define PAGE_SIZE_1G 0x40000000

#endif

#endif
