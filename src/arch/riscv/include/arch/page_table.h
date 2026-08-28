#ifndef ASM_PAGE_TABLE_H
#define ASM_PAGE_TABLE_H

#define KERNEL_LOAD_ADDR 0x80200000

#if __riscv_xlen == 32
#define KERNEL_LINK_ADDR 0xc0000000
#else
#define KERNEL_LINK_ADDR 0xffffffff80000000
#endif

#endif
