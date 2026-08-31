#ifndef ASM_PAGE_TABLE_H
#define ASM_PAGE_TABLE_H

#define KERNEL_LOAD_ADDR 0x80200000

#if __riscv_xlen == 32
/*
 * The kernel link region must not overlap the direct map at PAGE_OFFSET
 * (0xc0000000); use the free VA gap between the top of user space
 * (0x7ffff000) and the direct map.
 */
#define KERNEL_LINK_ADDR 0x80000000
#else
#define KERNEL_LINK_ADDR 0xffffffff80000000
#endif

#endif
