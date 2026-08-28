#ifndef ASM_BOOT_H
#define ASM_BOOT_H

#define NR_INIT_STACK_PAGES 1

#if __riscv_xlen == 32
/* Sv32 early boot maps 4KiB pages: 1 page for the PGD + PTE tables. */
#define NR_EARLY_PGDIR_PAGES 8
#else
#define NR_EARLY_PGDIR_PAGES 4
#endif

#endif
