#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/panic.h>
#include <brk/plic.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/riscv.h>
#include <brk/sbi.h>
#include <brk/syscall.h>
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

void trap_init(void)
{
	timer_init();
}

void trap_init_hart(u32 hart_id)
{
	(void)hart_id;
	write_stvec((u64)kernel_trap_vector);
	write_sie(read_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);
	timer_set_next();
	intr_on();
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
	struct process *proc = current_process();
	u64 code = TRAP_CAUSE_CODE(scause);

	if (TRAP_IS_INTERRUPT(scause)) {
		if (code == 5) {
			timer_handle_int();
			if (proc && --proc->time_slice <= 0)
				proc_yield();
		} else if (code == 9) {
			irq_handle_external(current_cpuid());
		} else {
			panic("%s: scause=%#lx,sepc=%#lx,stval=%#lx\n",
			      cause_to_str(scause), scause, sepc, stval);
		}
	} else {
		panic("%s: scause=%#lx,sepc=%#lx,stval=%#lx\n",
		      cause_to_str(scause), scause, sepc, stval);
	}

	write_sepc(sepc);
	write_sstatus(sstatus);
}

struct trapframe *user_trap_handler(void)
{
	struct trapframe *tf;
	struct process *proc;
	u64 jiffies;
	u64 scause = read_scause();
	u64 sepc = read_sepc();
	u64 stval = read_stval();
	u64 code = TRAP_CAUSE_CODE(scause);

	write_stvec((u64)kernel_trap_vector);

	tf = (struct trapframe *)read_sscratch();
	proc = container_of(tf, struct process, tf);
	write_tp(tf->cpuid);
	write_sstatus(read_sstatus() | SSTATUS_SUM);

	jiffies = jiffies_get();
	proc->ptms.tms_utime += jiffies - proc->utime;
	proc->ktime = jiffies;

	proc->tf.epc = sepc;

	if (TRAP_IS_INTERRUPT(scause)) {
		if (code == 5) {
			timer_handle_int();
			if (proc_is_killed(proc))
				proc_exit(1);
			if (--proc->time_slice <= 0)
				proc_yield();
		} else if (code == 9) {
			irq_handle_external(current_cpuid());
		} else {
			klog_warn("%s: scause=%#lx,sepc=%#lx,stval=%#lx\n",
				  cause_to_str(scause), scause, sepc, stval);
			proc_set_killed(proc);
		}
	} else {
		if (code == 8) {
			if (proc_is_killed(proc))
				proc_exit(1);
			proc->tf.epc += 4; /* skip ecall */
			intr_on();
			syscall();
		} else {
			klog_warn("%s: scause=%#lx,sepc=%#lx,stval=%#lx\n",
				  cause_to_str(scause), scause, sepc, stval);
			proc_set_killed(proc);
		}
	}

	if (proc_is_killed(proc))
		proc_exit(1);

	prepare_to_return();
	jiffies = jiffies_get();
	proc->ptms.tms_stime += jiffies - proc->ktime;
	proc->utime = jiffies;
	return &proc->tf;
}

void prepare_to_return(void)
{
	u64 sstatus;
	struct process *proc;

	intr_off();

	write_stvec((u64)user_trap_vector);

	proc = current_process();

	sstatus = read_sstatus();
	sstatus &= ~SSTATUS_SPP;
	sstatus |= SSTATUS_SPIE;
	write_sstatus(sstatus);

	proc->tf.kernel_sp = proc->kstack + KSTACK_SIZE;
	proc->tf.cpuid = current_cpuid();

	write_sepc(proc->tf.epc);

	write_sstatus(read_sstatus() & ~SSTATUS_SUM);
}
