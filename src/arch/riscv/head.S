#include <arch/asm/vmlayout.h>
#include <arch/asm/page.h>
#include <brk/asm/cpu.h>

#define HART_ID_SAVED s2
#define DTB_PA_SAVED s3
#define KERNEL_VA_PA_OFFSET_SAVED s4
#define INIT_STACK_SAVED s5

	.macro compute_kernel_va_pa_offset
	la t0, _skernel
	li t1, KERNEL_LINK_ADDR
	sub KERNEL_VA_PA_OFFSET_SAVED, t1, t0
	.endm

	.macro setup_per_hart_stack hart_id
	li t0, INIT_STACK_SIZE
	addi t1, \hart_id, 1
	mul t0, t1, t0
	la t2, init_stack
	add INIT_STACK_SAVED, t2, t0
	mv sp, INIT_STACK_SAVED
	.endm

	.macro jump_to_kernel_virt target
	mv sp, INIT_STACK_SAVED
	add sp, sp, KERNEL_VA_PA_OFFSET_SAVED
	la t0, \target
	add t0, t0, KERNEL_VA_PA_OFFSET_SAVED
	jr t0
	.endm

	.section ".text.head"
	.global _start
_start:
	mv HART_ID_SAVED, a0
	mv DTB_PA_SAVED, a1

	compute_kernel_va_pa_offset

	la t0, _sbss
	la t1, _ebss
.Lclear_bss:
	bgeu t0, t1, .Lclear_bss_done
	sd zero, 0(t0)
	addi t0, t0, 8
	j .Lclear_bss
.Lclear_bss_done:

	setup_per_hart_stack HART_ID_SAVED

	mv a0, DTB_PA_SAVED
	li a1, 1
	call setup_vm_early

	mv a0, HART_ID_SAVED
	mv a1, DTB_PA_SAVED
	mv a2, KERNEL_VA_PA_OFFSET_SAVED
	jump_to_kernel_virt primary_entry

	.global _secondary_start
_secondary_start:
	mv HART_ID_SAVED, a0

	compute_kernel_va_pa_offset
	setup_per_hart_stack HART_ID_SAVED

	/* Non-boot hart ignore the first and second arguments. */
	li a0, 0
	li a1, 0
	call setup_vm_early

	mv a0, HART_ID_SAVED
	jump_to_kernel_virt secondary_entry
