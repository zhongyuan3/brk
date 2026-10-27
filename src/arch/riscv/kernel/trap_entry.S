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
	addi sp, sp, -RISCV_TRAP_FRAME_SIZE
	sd x5, 40(sp)
	addi x5, sp, RISCV_TRAP_FRAME_SIZE
	sd x5, 16(sp)
	j .Ltrap_save_regs

.Ltrap_from_user:
	# Trap from user mode. sp is the kernel stack top and sscratch holds
	# the user sp. tp still holds the user's thread pointer, so reload the
	# kernel per-CPU pointer saved by ret_from_trap.
	addi sp, sp, -RISCV_TRAP_FRAME_SIZE
	sd x5, 40(sp)
	csrr x5, sscratch
	sd x5, 16(sp)
	ld tp, RISCV_TRAP_FRAME_KERNEL_TP(sp)

.Ltrap_save_regs:
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
	call trap_handler

	j ret_from_trap

	.global ret_from_init
ret_from_init:
	call init_task_do
	j ret_from_trap

	# Body of a trap return. Newly forked processes are switched into here
	# with `sp` pointing at their copied frame.
	.global ret_from_fork
ret_from_fork:
	call task_schedlue_tail

ret_from_trap:
	# Remember the kernel per-CPU pointer while tp is still valid. It is
	# needed by trap_entry on the next user -> kernel transition, after the
	# user's tp has been restored below.
	sd tp, RISCV_TRAP_FRAME_KERNEL_TP(sp)

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
