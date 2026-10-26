#include <arch/asm/trap.h>

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
	 * The struct trap_frame pointer of the current process is stored in the
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

	csrrw a0, sscratch, a0

	sret

	.global __trap_entry
	.align 4
__trap_entry:
	# sscratch holds the kernel stack pointer while running in user mode and
	# zero while in the kernel. Swap to the kernel stack when the trap came
	# from user mode; the swap with zero restores the kernel sp otherwise.
	csrrw sp, sscratch, sp
	bnez sp, .Ltrap_have_stack
	csrrw sp, sscratch, sp
.Ltrap_have_stack:
	addi sp, sp, -RISCV_TRAP_FRAME_SIZE

	# Save t0 (x5) first, then reuse it to record the trap-time stack pointer:
	# the user stack is in sscratch, the kernel one is sp + frame_size.
	sd x5, 40(sp)
	csrr x5, sscratch
	bnez x5, .Ltrap_save_sp
	addi x5, sp, RISCV_TRAP_FRAME_SIZE
.Ltrap_save_sp:
	sd x5, 16(sp)

	# From here on the kernel runs with sscratch cleared, so a nested trap
	# taken in kernel mode is not mistaken for a user trap.
	csrw sscratch, zero

	sd x0, 0(sp)
	sd x1, 8(sp)
	sd x3, 24(sp)
	sd x4, 32(sp)
	sd x6, 48(sp)
	sd x7, 56(sp)
	sd x8, 64(sp)
	sd x9, 72(sp)
	sd x10, 80(sp)
	sd x11, 88(sp)
	sd x12, 96(sp)
	sd x13, 104(sp)
	sd x14, 112(sp)
	sd x15, 120(sp)
	sd x16, 128(sp)
	sd x17, 136(sp)
	sd x18, 144(sp)
	sd x19, 152(sp)
	sd x20, 160(sp)
	sd x21, 168(sp)
	sd x22, 176(sp)
	sd x23, 184(sp)
	sd x24, 192(sp)
	sd x25, 200(sp)
	sd x26, 208(sp)
	sd x27, 216(sp)
	sd x28, 224(sp)
	sd x29, 232(sp)
	sd x30, 240(sp)
	sd x31, 248(sp)

	csrr x5, sstatus
	sd x5, 256(sp)
	csrr x5, sepc
	sd x5, 264(sp)
	csrr x5, scause
	sd x5, 272(sp)
	csrr x5, stval
	sd x5, 280(sp)

	mv a0, sp
	call __trap_handler

	# Body of a trap return. Newly forked processes are switched into here
	# with `sp` pointing at their copied frame.
	.global ret_from_trap
ret_from_trap:
	call __prepare_ret_from_trap

	# Restore the CSRs first; `sret` takes the return PC from sepc and the
	# interrupt enable from sstatus.SPIE.
	ld x5, 256(sp)
	csrw sstatus, x5
	ld x5, 264(sp)
	csrw sepc, x5

	# Keep sscratch consistent with the mode we return to: the kernel stack
	# pointer for user mode, zero for the kernel.
	csrr x5, sstatus
	andi x5, x5, 0x100
	bnez x5, .Ltrap_ret_kernel
	addi x5, sp, RISCV_TRAP_FRAME_SIZE
	csrw sscratch, x5
	j .Ltrap_ret_done
.Ltrap_ret_kernel:
	csrw sscratch, zero
.Ltrap_ret_done:

	ld x1, 8(sp)
	ld x3, 24(sp)
	ld x4, 32(sp)
	ld x5, 40(sp)
	ld x6, 48(sp)
	ld x7, 56(sp)
	ld x8, 64(sp)
	ld x9, 72(sp)
	ld x10, 80(sp)
	ld x11, 88(sp)
	ld x12, 96(sp)
	ld x13, 104(sp)
	ld x14, 112(sp)
	ld x15, 120(sp)
	ld x16, 128(sp)
	ld x17, 136(sp)
	ld x18, 144(sp)
	ld x19, 152(sp)
	ld x20, 160(sp)
	ld x21, 168(sp)
	ld x22, 176(sp)
	ld x23, 184(sp)
	ld x24, 192(sp)
	ld x25, 200(sp)
	ld x26, 208(sp)
	ld x27, 216(sp)
	ld x28, 224(sp)
	ld x29, 232(sp)
	ld x30, 240(sp)
	ld x31, 248(sp)

	# The stack pointer is restored last, once the frame is no longer needed.
	ld x2, 16(sp)
	sret
