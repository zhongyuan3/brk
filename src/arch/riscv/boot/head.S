#include <arch/boot.h>
#include <arch/page_table.h>
#include <arch/page.h>
#include <arch/smp.h>

#define HART_ID_SAVED_REG s2
#define DTB_PHYS_SAVED_REG s3
#define LOAD_OFFSET_SAVED_REG s4
#define STACK_SAVED_REG s5

	.macro compute_load_offset
	la t0, _skernel
	li t1, KERNEL_LINK_ADDR
	sub LOAD_OFFSET_SAVED_REG, t1, t0
	.endm

	.macro setup_per_hart_stack hart_reg
	li t0, 1
	slli t0, t0, PAGE_SHIFT
	li t1, NR_INIT_STACK_PAGES
	mul t0, t0, t1
	addi t1, \hart_reg, 1
	mul t0, t1, t0
	la t2, init_stack
	add STACK_SAVED_REG, t2, t0
	mv sp, STACK_SAVED_REG
	.endm

	.macro jump_to_kernel_virt target
	mv sp, STACK_SAVED_REG
	add sp, sp, LOAD_OFFSET_SAVED_REG
	la t0, \target
	add t0, t0, LOAD_OFFSET_SAVED_REG
	jr t0
	.endm

	.section ".text.head"
	.global _start
_start:
	mv HART_ID_SAVED_REG, a0
	mv DTB_PHYS_SAVED_REG, a1

	csrw sscratch, a0
	li tp, 0

	compute_load_offset

	la t0, _sbss
	la t1, _ebss
.Lclear_bss:
	bgeu t0, t1, .Lclear_bss_done
#if __riscv_xlen == 32
	sw zero, 0(t0)
	addi t0, t0, 4
#else
	sd zero, 0(t0)
	addi t0, t0, 8
#endif
	j .Lclear_bss
.Lclear_bss_done:

	setup_per_hart_stack HART_ID_SAVED_REG

	mv a0, DTB_PHYS_SAVED_REG
	mv a1, LOAD_OFFSET_SAVED_REG
	call early_pgtable_map

	mv a0, LOAD_OFFSET_SAVED_REG
	call early_pgtable_enable

	la a0, boot_cpuid
#if __riscv_xlen == 32
	sw HART_ID_SAVED_REG, 0(a0)
#else
	sd HART_ID_SAVED_REG, 0(a0)
#endif
	la a0, dtb_phys
#if __riscv_xlen == 32
	/* dtb_phys is uint64_t: store the low word; the high word is
	 * already zeroed by the BSS clear above. */
	sw DTB_PHYS_SAVED_REG, 0(a0)
#else
	sd DTB_PHYS_SAVED_REG, 0(a0)
#endif
	la a0, load_offset
#if __riscv_xlen == 32
	sw LOAD_OFFSET_SAVED_REG, 0(a0)
#else
	sd LOAD_OFFSET_SAVED_REG, 0(a0)
#endif

	jump_to_kernel_virt start_kernel

#if ENABLE_SMP
	.global hart_entry
hart_entry:
	mv HART_ID_SAVED_REG, a0

	csrw sscratch, a0
	li tp, 0

	compute_load_offset
	setup_per_hart_stack HART_ID_SAVED_REG

	mv a0, LOAD_OFFSET_SAVED_REG
	call early_pgtable_enable

	jump_to_kernel_virt start_hart
#endif
