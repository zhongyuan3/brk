#ifndef RISCV_BOOT_INFO_H
#define RISCV_BOOT_INFO_H

#include <brk/types.h>

extern struct boot_info {
	virt_addr_t kernel_image_offset;
	unsigned long boot_hart_id;
	phys_addr_t dtb_pa;
} boot_info;

#endif
