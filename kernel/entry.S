	.section .text
	.global kernel_trap_vector
	.align 4
kernel_trap_vector:
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

	sret

	.global user_trap_vector
	.align 4
user_trap_vector:
	/*
	 * The struct trapframe pointer of the current process is stored in the
	 * sscratch register. We temporarily swap the values of sscratch and a0
	 * to save the registers.
	 */
	csrrw a0, sscratch, a0

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

	/* Write the struct trapframe pointer back to sscratch */
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

	csrrw a0, sscratch, a0

	sret
