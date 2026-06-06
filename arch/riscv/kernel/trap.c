#include <arch/csr.h>
#include <arch/irqflags.h>
#include <arch/sbi.h>
#include <arch/trap.h>
#include <arch/trapframe.h>
#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/panic.h>
#include <brk/plic.h>
#include <brk/printk.h>
#include <brk/signal.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/types.h>

static const char *const exception_strs[] = {
	[0] = "Instruction address misaligned",
	[1] = "Instruction access fault",
	[2] = "Illegal instruction",
	[3] = "Breakpoint",
	[4] = "Load address misaligned",
	[5] = "Load access fault",
	[6] = "Store/AMO address misaligned",
	[7] = "Store/AMO access fault",
	[8] = "Environment call from U-mode",
	[9] = "Environment call from S-mode",
	[11] = "Environment call from M-mode",
	[12] = "Instruction page fault",
	[13] = "Load page fault",
	[15] = "Store/AMO page fault",
	[16] = "Double trap",
	[18] = "Software check",
	[19] = "Hardware error",
};

static const char *const interrupt_strs[] = {
	[1] = "Supervisor software interrupt",
	[3] = "Machine software interrupt",
	[5] = "Supervisor timer interrupt",
	[7] = "Supervisor timer interrupt",
	[9] = "Supervisor external interrupt",
	[11] = "Machine external interrupt",
	[13] = "Counter-overflow interrupt",
};

void trap_init_hart(u32 hart_id)
{
	(void)hart_id;
	write_stvec((u64)kernel_trap_vector);
	write_sie(read_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);
	timer_set_next();
}

static const char *cause_to_str(u64 cause)
{
	u64 code = TRAP_CAUSE_CODE(cause);
	if (TRAP_IS_INTERRUPT(cause)) {
		if (code > countof(interrupt_strs) || !interrupt_strs[code])
			return "Unexpected interrupt";
		else
			return interrupt_strs[code];
	} else {
		if (code > countof(exception_strs) || !exception_strs[code])
			return "Unexpected exception";
		else
			return exception_strs[code];
	}
}

void kernel_trap_handler(void)
{
	u64 sstatus = read_sstatus();
	u64 scause = read_scause();
	u64 sepc = read_sepc();
	u64 stval = read_stval();
	struct task_control_block *task = current_task();
	u64 code = TRAP_CAUSE_CODE(scause);

	if (TRAP_IS_INTERRUPT(scause)) {
		if (code == 1) {
			clear_ssip_csr();
		} else if (code == 5) {
			timer_handle_int();
			if (task && --task->time_slice <= 0)
				task_yield();
		} else if (code == 9) {
			irq_handle_external(current_cpuid());
		} else {
			panic("%s: cpu=%d scause=%#lx,sepc=%#lx,stval=%#lx\n",
			      cause_to_str(scause), current_cpuid(), scause,
			      sepc, stval);
		}
	} else {
		panic("%s: cpu=%d scause=%#lx,sepc=%#lx,stval=%#lx\n",
		      cause_to_str(scause), current_cpuid(), scause, sepc,
		      stval);
	}

	write_sepc(sepc);
	write_sstatus(sstatus);
}

struct trap_frame *user_trap_handler(void)
{
	struct trap_frame *tf;
	struct task_control_block *task;
	u64 jiffies;
	u64 scause = read_scause();
	u64 sepc = read_sepc();
	u64 stval = read_stval();
	u64 code = TRAP_CAUSE_CODE(scause);

	write_stvec((u64)kernel_trap_vector);

	tf = (struct trap_frame *)read_sscratch();
	task = ((struct extended_trap_frame *)tf)->task;
	set_current_task(task);
	set_current_cpuid(arch_tf_get_cpuid(tf));
	write_sstatus(read_sstatus() | SSTATUS_SUM);

	jiffies = jiffies_get();
	task_add_user_time(task, jiffies - task->utime);
	task->ktime = jiffies;

	arch_tf_set_pc(task->tf, sepc);

	if (TRAP_IS_INTERRUPT(scause)) {
		if (code == 1) {
			clear_ssip_csr();
		} else if (code == 5) {
			timer_handle_int();
			signal_deliver_pending(task);
			if (--task->time_slice <= 0)
				task_yield();
		} else if (code == 9) {
			irq_handle_external(current_cpuid());
		} else {
			klog_warn(
				"%s: pid=%ld,scause=%#lx,sepc=%#lx,stval=%#lx\n",
				cause_to_str(scause), task->pid, scause, sepc,
				stval);
			task_set_killed(task);
		}
	} else {
		if (code == 8) {
			signal_deliver_pending(task);
			arch_tf_skip_syscall(task->tf);
			intr_on();
			syscall();
		} else {
			klog_warn(
				"%s: pid=%ld,scause=%#lx,sepc=%#lx,stval=%#lx\n",
				cause_to_str(scause), task->pid, scause, sepc,
				stval);
			task_set_killed(task);
		}
	}

	signal_deliver_pending(task);

	prepare_to_return();
	jiffies = jiffies_get();
	task_add_system_time(task, jiffies - task->ktime);
	task->utime = jiffies;
	return task->tf;
}

void prepare_to_return(void)
{
	u64 sstatus;
	struct task_control_block *task;

	intr_off();

	write_stvec((u64)user_trap_vector);

	task = current_task();

	sstatus = read_sstatus();
	sstatus &= ~SSTATUS_SPP;
	sstatus |= SSTATUS_SPIE;
	write_sstatus(sstatus);

	arch_tf_set_kernel_sp(task->tf, task->kstack_top);
	arch_tf_set_cpuid(task->tf, current_cpuid());

	write_sepc(arch_tf_get_pc(task->tf));

	write_sstatus(read_sstatus() & ~SSTATUS_SUM);
}
