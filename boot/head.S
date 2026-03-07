#include <aosd/asm.h>

	.section ".text.head"
	.global _start
_start:
	mv s2, a0
	mv s3, a1

	la t0, _skernel
	li t1, KERNEL_LINK_ADDR
	sub s4, t0, t1

	la t0, _sbss
	la t1, _ebss
.L1:
	bgeu t0, t1, .L2
	sd zero, 0(t0)
	addi t0, t0, 8
	j .L1
.L2:

	li t0, 1
	slli t0, t0, PAGE_SHIFT
	li t1, NR_INIT_STACK_PAGES
	mul t0, t0, t1
	addi t1, a0, 1
	mul t0, t1, t0
	la t2, init_stack
	add s5, t2, t0
	mv sp, s5

	mv a0, a1

	call make_early_pgtable
	call setup_early_pgtable

	mv a0, s2
	mv a1, s3
	mv a2, s4

	mv sp, s5
	sub sp, sp, a2
	la t0, start_kernel
	sub t0, t0, a2
	jr t0

.Lspin:
	wfi
	j .Lspin

	.global hart_entry
hart_entry:
	mv s2, a0

	la t0, _skernel
	li t1, KERNEL_LINK_ADDR
	sub s4, t0, t1

	li t0, 1
	slli t0, t0, PAGE_SHIFT
	li t1, NR_INIT_STACK_PAGES
	mul t0, t0, t1
	addi t1, a0, 1
	mul t0, t1, t0
	la t2, init_stack
	add s5, t2, t0
	mv sp, s5

	mv a0, sp

	call setup_early_pgtable

	mv a0, s2
	mv a2, s4

	mv sp, s5
	sub sp, sp, a2
	la t0, start_hart
	sub t0, t0, a2
	jr t0

.Lhart_spin:
	wfi
	j .Lhart_spin
