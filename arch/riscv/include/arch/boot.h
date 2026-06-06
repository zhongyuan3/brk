#ifndef ARCH_BOOT_H
#define ARCH_BOOT_H

#include <arch/page.h>

#define KERNEL_LOAD_ADDR 0x80200000
#define KERNEL_LINK_ADDR 0xffffffff80000000

#define NR_INIT_STACK_PAGES 2
#define NR_EARLY_PGDIR_PAGES 4

#endif
