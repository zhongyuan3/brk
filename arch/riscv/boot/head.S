#include <arch/boot.h>
#include <arch/page.h>
#include <arch/smp.h>

	.macro compute_load_offset
	la t0, _skernel
	li t1, KERNEL_LINK_ADDR
	sub s4, t0, t1
	.endm

	.macro setup_per_hart_stack hart_reg
	li t0, 1
	slli t0, t0, PAGE_SHIFT
	li t1, NR_INIT_STACK_PAGES
	mul t0, t0, t1
	addi t1, \hart_reg, 1
	mul t0, t1, t0
	la t2, init_stack
	add s5, t2, t0
	mv sp, s5
	.endm

	.macro jump_to_kernel_virt target
	mv sp, s5
	sub sp, sp, s4
	la t0, \target
	sub t0, t0, s4
	jr t0
	.endm

	.section ".text.head"
	.global _start
_start:
	mv s2, a0
	mv s3, a1

	compute_load_offset

	la t0, _sbss
	la t1, _ebss
.Lclear_bss:
	bgeu t0, t1, .Lclear_bss_done
	sd zero, 0(t0)
	addi t0, t0, 8
	j .Lclear_bss
.Lclear_bss_done:

	setup_per_hart_stack a0

	mv a0, s3
	mv a1, s4
	call early_pgtable_map

	mv a0, s4
	call early_pgtable_enable

	mv a0, s2
	mv a1, s3
	mv a2, s4
	jump_to_kernel_virt start_kernel

#if ENABLE_SMP
	.global hart_entry
hart_entry:
	mv s2, a0

	compute_load_offset
	setup_per_hart_stack a0

	mv a0, s4
	call early_pgtable_enable

	mv a0, s2
	mv a2, s4
	jump_to_kernel_virt start_hart
#endif
