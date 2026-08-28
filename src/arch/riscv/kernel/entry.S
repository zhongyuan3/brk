.section .text
	.global kernel_trap_vector
	.align 4
kernel_trap_vector:
#if __riscv_xlen == 32
	addi sp, sp, -128

	sw ra, 0(sp)
	sw sp, 4(sp)
	sw gp, 8(sp)
	sw tp, 12(sp)
	sw t0, 16(sp)
	sw t1, 20(sp)
	sw t2, 24(sp)
	sw a0, 36(sp)
	sw a1, 40(sp)
	sw a2, 44(sp)
	sw a3, 48(sp)
	sw a4, 52(sp)
	sw a5, 56(sp)
	sw a6, 60(sp)
	sw a7, 64(sp)
	sw t3, 108(sp)
	sw t4, 112(sp)
	sw t5, 116(sp)
	sw t6, 120(sp)

	call kernel_trap_handler

	lw ra, 0(sp)
	lw sp, 4(sp)
	lw gp, 8(sp)
	lw t0, 16(sp)
	lw t1, 20(sp)
	lw t2, 24(sp)
	lw a0, 36(sp)
	lw a1, 40(sp)
	lw a2, 44(sp)
	lw a3, 48(sp)
	lw a4, 52(sp)
	lw a5, 56(sp)
	lw a6, 60(sp)
	lw a7, 64(sp)
	lw t3, 108(sp)
	lw t4, 112(sp)
	lw t5, 116(sp)
	lw t6, 120(sp)

	addi sp, sp, 128
#else
	addi sp, sp, -256

	sd ra, 0(sp)
	sd sp, 8(sp)
	sd gp, 16(sp)
	sd tp, 24(sp)
	sd t0, 32(sp)
	sd t1, 40(sp)
	sd t2, 48(sp)
	sd a0, 72(sp)
	sd a1, 80(sp)
	sd a2, 88(sp)
	sd a3, 96(sp)
	sd a4, 104(sp)
	sd a5, 112(sp)
	sd a6, 120(sp)
	sd a7, 128(sp)
	sd t3, 216(sp)
	sd t4, 224(sp)
	sd t5, 232(sp)
	sd t6, 240(sp)

	call kernel_trap_handler

	ld ra, 0(sp)
	ld sp, 8(sp)
	ld gp, 16(sp)
	ld t0, 32(sp)
	ld t1, 40(sp)
	ld t2, 48(sp)
	ld a0, 72(sp)
	ld a1, 80(sp)
	ld a2, 88(sp)
	ld a3, 96(sp)
	ld a4, 104(sp)
	ld a5, 112(sp)
	ld a6, 120(sp)
	ld a7, 128(sp)
	ld t3, 216(sp)
	ld t4, 224(sp)
	ld t5, 232(sp)
	ld t6, 240(sp)

	addi sp, sp, 256
#endif

	sret

	.global user_trap_vector
	.align 4
user_trap_vector:
	/*
	 * The struct trap_frame pointer of the current process is stored in the
	 * sscratch register. We temporarily swap the values of sscratch and a0
	 * to save the registers.
	 */
	csrrw a0, sscratch, a0

#if __riscv_xlen == 32
	sw ra, 4(a0)
	sw sp, 8(a0)
	sw gp, 12(a0)
	sw tp, 16(a0)
	sw t0, 20(a0)
	sw t1, 24(a0)
	sw t2, 28(a0)
	sw s0, 32(a0)
	sw s1, 36(a0)
	sw a1, 44(a0)
	sw a2, 48(a0)
	sw a3, 52(a0)
	sw a4, 56(a0)
	sw a5, 60(a0)
	sw a6, 64(a0)
	sw a7, 68(a0)
	sw s2, 72(a0)
	sw s3, 76(a0)
	sw s4, 80(a0)
	sw s5, 84(a0)
	sw s6, 88(a0)
	sw s7, 92(a0)
	sw s8, 96(a0)
	sw s9, 100(a0)
	sw s10, 104(a0)
	sw s11, 108(a0)
	sw t3, 112(a0)
	sw t4, 116(a0)
	sw t5, 120(a0)
	sw t6, 124(a0)

	/* Now save the a0 register */
	csrr t0, sscratch
	sw t0, 40(a0)

	/* Write the struct trap_frame pointer back to sscratch */
	csrw sscratch, a0

	/* Set kernel stack */
	lw sp, 0(a0)

	call user_trap_handler

	.global user_trap_return
user_trap_return:
	lw t0, 40(a0)
	csrw sscratch, t0

	lw ra, 4(a0)
	lw sp, 8(a0)
	lw gp, 12(a0)
	lw tp, 16(a0)
	lw t0, 20(a0)
	lw t1, 24(a0)
	lw t2, 28(a0)
	lw s0, 32(a0)
	lw s1, 36(a0)
	lw a1, 44(a0)
	lw a2, 48(a0)
	lw a3, 52(a0)
	lw a4, 56(a0)
	lw a5, 60(a0)
	lw a6, 64(a0)
	lw a7, 68(a0)
	lw s2, 72(a0)
	lw s3, 76(a0)
	lw s4, 80(a0)
	lw s5, 84(a0)
	lw s6, 88(a0)
	lw s7, 92(a0)
	lw s8, 96(a0)
	lw s9, 100(a0)
	lw s10, 104(a0)
	lw s11, 108(a0)
	lw t3, 112(a0)
	lw t4, 116(a0)
	lw t5, 120(a0)
	lw t6, 124(a0)
#else
	sd ra, 8(a0)
	sd sp, 16(a0)
	sd gp, 24(a0)
	sd tp, 32(a0)
	sd t0, 40(a0)
	sd t1, 48(a0)
	sd t2, 56(a0)
	sd s0, 64(a0)
	sd s1, 72(a0)
	sd a1, 88(a0)
	sd a2, 96(a0)
	sd a3, 104(a0)
	sd a4, 112(a0)
	sd a5, 120(a0)
	sd a6, 128(a0)
	sd a7, 136(a0)
	sd s2, 144(a0)
	sd s3, 152(a0)
	sd s4, 160(a0)
	sd s5, 168(a0)
	sd s6, 176(a0)
	sd s7, 184(a0)
	sd s8, 192(a0)
	sd s9, 200(a0)
	sd s10, 208(a0)
	sd s11, 216(a0)
	sd t3, 224(a0)
	sd t4, 232(a0)
	sd t5, 240(a0)
	sd t6, 248(a0)

	/* Now save the a0 register */
	csrr t0, sscratch
	sd t0, 80(a0)

	/* Write the struct trap_frame pointer back to sscratch */
	csrw sscratch, a0

	/* Set kernel stack */
	ld sp, 0(a0)

	call user_trap_handler

	.global user_trap_return
user_trap_return:
	ld t0, 80(a0)
	csrw sscratch, t0

	ld ra, 8(a0)
	ld sp, 16(a0)
	ld gp, 24(a0)
	ld tp, 32(a0)
	ld t0, 40(a0)
	ld t1, 48(a0)
	ld t2, 56(a0)
	ld s0, 64(a0)
	ld s1, 72(a0)
	ld a1, 88(a0)
	ld a2, 96(a0)
	ld a3, 104(a0)
	ld a4, 112(a0)
	ld a5, 120(a0)
	ld a6, 128(a0)
	ld a7, 136(a0)
	ld s2, 144(a0)
	ld s3, 152(a0)
	ld s4, 160(a0)
	ld s5, 168(a0)
	ld s6, 176(a0)
	ld s7, 184(a0)
	ld s8, 192(a0)
	ld s9, 200(a0)
	ld s10, 208(a0)
	ld s11, 216(a0)
	ld t3, 224(a0)
	ld t4, 232(a0)
	ld t5, 240(a0)
	ld t6, 248(a0)
#endif

	csrrw a0, sscratch, a0

	sret