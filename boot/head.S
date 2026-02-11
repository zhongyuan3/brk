#include <aosd/asm.h>

	.section ".text.head"
	.global _start
_start:
	la t0, early_trap_vector
	csrw stvec, t0

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

	la sp, init_stack_top

	mv a0, a1

	call setup_early_pgtable

	mv a0, s2
	mv a1, s3
	mv a2, s4

	la sp, init_stack_top
	sub sp, sp, a2
	la t0, start_kernel
	sub t0, t0, a2
	jr t0

.Lspin:
	wfi
	j .Lspin
