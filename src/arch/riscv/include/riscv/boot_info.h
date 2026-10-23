#ifndef RISCV_BOOT_INFO_H
#define RISCV_BOOT_INFO_H

extern struct boot_info {
	unsigned long kernel_image_offset;
	unsigned long boot_hart_id;
	unsigned long dtb_pa;
} boot_info;

#endif
