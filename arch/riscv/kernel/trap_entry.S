#include <arch/asm/trap.h>

	.global trap_entry
	.align 4
trap_entry:
	# sscratch holds the kernel stack pointer while running in user mode and
	# zero while in the kernel. Swap to the kernel stack when the trap came
	# from user mode; the swap with zero restores the kernel sp otherwise.
	csrrw sp, sscratch, sp
	bnez sp, .Ltrap_from_user

	# Trap from kernel mode. The swap above put the zero sscratch into sp
	# and the interrupted kernel sp into sscratch, so swap it back.
	csrrw sp, sscratch, sp
	addi sp, sp, -__RISCV_TRAP_FRAME_SIZE
	sd x5, 40(sp)
	addi x5, sp, __RISCV_TRAP_FRAME_SIZE
	sd x5, 16(sp)
	j .Ltrap_save_regs

.Ltrap_from_user:
	# Trap from user mode. sp is the kernel stack top and sscratch holds
	# the user sp. tp still holds the user's thread pointer: save it before
	# loading the kernel per-CPU pointer, otherwise the user TLS/TCB value
	# would be lost on every syscall.
	addi sp, sp, -__RISCV_TRAP_FRAME_SIZE
	sd x5, 40(sp)
	csrr x5, sscratch
	sd x5, 16(sp)
	sd x4, 32(sp)
	ld tp, __RISCV_TRAP_FRAME_KERNEL_TP(sp)

	# Save the user floating-point context. sstatus.FS is Initial or Dirty
	# here (set by prepare_to_ret_to_user before the first user entry and
	# promoted to Dirty by the user's own FP writes), so kernel FP
	# instructions execute legally. Saving the FP state here and restoring
	# it in ret_from_trap keeps FP registers intact across preemption.
	fsd f0,  __RISCV_TRAP_FRAME_FP+0(sp)
	fsd f1,  __RISCV_TRAP_FRAME_FP+8(sp)
	fsd f2,  __RISCV_TRAP_FRAME_FP+16(sp)
	fsd f3,  __RISCV_TRAP_FRAME_FP+24(sp)
	fsd f4,  __RISCV_TRAP_FRAME_FP+32(sp)
	fsd f5,  __RISCV_TRAP_FRAME_FP+40(sp)
	fsd f6,  __RISCV_TRAP_FRAME_FP+48(sp)
	fsd f7,  __RISCV_TRAP_FRAME_FP+56(sp)
	fsd f8,  __RISCV_TRAP_FRAME_FP+64(sp)
	fsd f9,  __RISCV_TRAP_FRAME_FP+72(sp)
	fsd f10, __RISCV_TRAP_FRAME_FP+80(sp)
	fsd f11, __RISCV_TRAP_FRAME_FP+88(sp)
	fsd f12, __RISCV_TRAP_FRAME_FP+96(sp)
	fsd f13, __RISCV_TRAP_FRAME_FP+104(sp)
	fsd f14, __RISCV_TRAP_FRAME_FP+112(sp)
	fsd f15, __RISCV_TRAP_FRAME_FP+120(sp)
	fsd f16, __RISCV_TRAP_FRAME_FP+128(sp)
	fsd f17, __RISCV_TRAP_FRAME_FP+136(sp)
	fsd f18, __RISCV_TRAP_FRAME_FP+144(sp)
	fsd f19, __RISCV_TRAP_FRAME_FP+152(sp)
	fsd f20, __RISCV_TRAP_FRAME_FP+160(sp)
	fsd f21, __RISCV_TRAP_FRAME_FP+168(sp)
	fsd f22, __RISCV_TRAP_FRAME_FP+176(sp)
	fsd f23, __RISCV_TRAP_FRAME_FP+184(sp)
	fsd f24, __RISCV_TRAP_FRAME_FP+192(sp)
	fsd f25, __RISCV_TRAP_FRAME_FP+200(sp)
	fsd f26, __RISCV_TRAP_FRAME_FP+208(sp)
	fsd f27, __RISCV_TRAP_FRAME_FP+216(sp)
	fsd f28, __RISCV_TRAP_FRAME_FP+224(sp)
	fsd f29, __RISCV_TRAP_FRAME_FP+232(sp)
	fsd f30, __RISCV_TRAP_FRAME_FP+240(sp)
	fsd f31, __RISCV_TRAP_FRAME_FP+248(sp)
	frcsr x5
	sd x5, __RISCV_TRAP_FRAME_FCSR(sp)
	j .Ltrap_save_regs_no_tp

.Ltrap_save_regs:
	# From here on the kernel runs with sscratch cleared, so a nested trap
	# taken in kernel mode is not mistaken for a user trap.
	csrw sscratch, zero

	sd x0, 0(sp)
	sd x1, 8(sp)
	sd x3, 24(sp)
	sd x4, 32(sp)
	j .Ltrap_save_regs_tail

.Ltrap_save_regs_no_tp:
	csrw sscratch, zero
	sd x0, 0(sp)
	sd x1, 8(sp)
	sd x3, 24(sp)

.Ltrap_save_regs_tail:
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
	call trap_handler

	j ret_from_trap

	.global ret_from_user_init
ret_from_user_init:
	call user_init_entry
	j ret_from_trap

	# Body of a trap return. Newly forked processes are switched into here
	# with `sp` pointing at their copied frame.
	.global ret_from_fork
ret_from_fork:
	call thread_sched_tail

ret_from_trap:
	# Remember the kernel per-CPU pointer while tp is still valid. It is
	# needed by trap_entry on the next user -> kernel transition, after the
	# user's tp has been restored below.
	sd tp, __RISCV_TRAP_FRAME_KERNEL_TP(sp)

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
	call trap_return_to_user
	addi x5, sp, __RISCV_TRAP_FRAME_SIZE
	csrw sscratch, x5

	# Return to user mode: restore the user's floating-point context.
	# Kernel-mode returns skip this; the kernel has no FP state of its own.
	fld f0,  __RISCV_TRAP_FRAME_FP+0(sp)
	fld f1,  __RISCV_TRAP_FRAME_FP+8(sp)
	fld f2,  __RISCV_TRAP_FRAME_FP+16(sp)
	fld f3,  __RISCV_TRAP_FRAME_FP+24(sp)
	fld f4,  __RISCV_TRAP_FRAME_FP+32(sp)
	fld f5,  __RISCV_TRAP_FRAME_FP+40(sp)
	fld f6,  __RISCV_TRAP_FRAME_FP+48(sp)
	fld f7,  __RISCV_TRAP_FRAME_FP+56(sp)
	fld f8,  __RISCV_TRAP_FRAME_FP+64(sp)
	fld f9,  __RISCV_TRAP_FRAME_FP+72(sp)
	fld f10, __RISCV_TRAP_FRAME_FP+80(sp)
	fld f11, __RISCV_TRAP_FRAME_FP+88(sp)
	fld f12, __RISCV_TRAP_FRAME_FP+96(sp)
	fld f13, __RISCV_TRAP_FRAME_FP+104(sp)
	fld f14, __RISCV_TRAP_FRAME_FP+112(sp)
	fld f15, __RISCV_TRAP_FRAME_FP+120(sp)
	fld f16, __RISCV_TRAP_FRAME_FP+128(sp)
	fld f17, __RISCV_TRAP_FRAME_FP+136(sp)
	fld f18, __RISCV_TRAP_FRAME_FP+144(sp)
	fld f19, __RISCV_TRAP_FRAME_FP+152(sp)
	fld f20, __RISCV_TRAP_FRAME_FP+160(sp)
	fld f21, __RISCV_TRAP_FRAME_FP+168(sp)
	fld f22, __RISCV_TRAP_FRAME_FP+176(sp)
	fld f23, __RISCV_TRAP_FRAME_FP+184(sp)
	fld f24, __RISCV_TRAP_FRAME_FP+192(sp)
	fld f25, __RISCV_TRAP_FRAME_FP+200(sp)
	fld f26, __RISCV_TRAP_FRAME_FP+208(sp)
	fld f27, __RISCV_TRAP_FRAME_FP+216(sp)
	fld f28, __RISCV_TRAP_FRAME_FP+224(sp)
	fld f29, __RISCV_TRAP_FRAME_FP+232(sp)
	fld f30, __RISCV_TRAP_FRAME_FP+240(sp)
	fld f31, __RISCV_TRAP_FRAME_FP+248(sp)
	ld x5, __RISCV_TRAP_FRAME_FCSR(sp)
	fscsr x5
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
