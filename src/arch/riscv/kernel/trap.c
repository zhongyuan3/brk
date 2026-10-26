#include <arch/csr.h>
#include <arch/irqflags.h>
#include <arch/sbi.h>
#include <arch/timer.h>
#include <arch/trap.h>
#include <arch/trapframe.h>
#include <brk/assert.h>
#include <brk/bits.h>
#include <brk/cpu.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/panic.h>
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

void trap_enable_timer(void)
{
	write_sie(read_sie() | SIE_STIE);
}

void trap_enable_external(void)
{
	write_sie(read_sie() | SIE_SEIE);
}

void trap_enable_software(void)
{
	write_sie(read_sie() | SIE_SSIE);
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

static const char *exception_name(unsigned long code)
{
	if (code > countof(exception_strs) || !exception_strs[code])
		return "Unexpected exception";
	else
		return exception_strs[code];
}

static const char *interrupt_name(unsigned long code)
{
	if (code > countof(interrupt_strs) || !interrupt_strs[code])
		return "Unexpected interrupt";
	else
		return interrupt_strs[code];
}

void kernel_trap_handler(void)
{
	u64 sstatus = read_sstatus();
	u64 scause = read_scause();
	u64 sepc = read_sepc();
	u64 stval = read_stval();
	struct task *task = current_task();
	u64 code = TRAP_CAUSE_CODE(scause);

	if (TRAP_IS_INTERRUPT(scause)) {
		if (code == 1) {
			clear_ssip_csr();
		} else if (code == 5) {
			handle_tick();
			if (task && --task->time_slice <= 0)
				task_yield();
		} else if (code == 9) {
			irq_handle_external(this_cpuid());
		} else {
			panic("%s: cpu=%lu scause=%#" PRIx64 ",sepc=%#" PRIx64
			      ",stval=%#" PRIx64 "\n",
			      cause_to_str(scause), this_cpuid(), scause, sepc,
			      stval);
		}
	} else {
		panic("%s: cpu=%lu scause=%#" PRIx64 ",sepc=%#" PRIx64
		      ",stval=%#" PRIx64 "\n",
		      cause_to_str(scause), this_cpuid(), scause, sepc, stval);
	}

	write_sepc(sepc);
	write_sstatus(sstatus);
}

struct trap_frame *user_trap_handler(void)
{
	struct trap_frame *tf;
	struct task *task;
	u64 jiffies;
	u64 scause = read_scause();
	u64 sepc = read_sepc();
	u64 stval = read_stval();
	u64 code = TRAP_CAUSE_CODE(scause);

	write_stvec((uintptr_t)kernel_trap_vector);

	tf = (struct trap_frame *)(uintptr_t)read_sscratch();
	task = ((struct extended_trap_frame *)tf)->task;
	set_current_task(task);
	set_this_cpuid(arch_tf_get_cpuid(tf));
	write_sstatus(read_sstatus() | SSTATUS_SUM);

	jiffies = jiffies_get();
	task_add_user_time(task, jiffies - task->utime);
	task->ktime = jiffies;

	arch_tf_set_pc(task->tf, sepc);

	if (TRAP_IS_INTERRUPT(scause)) {
		if (code == 1) {
			clear_ssip_csr();
		} else if (code == 5) {
			handle_tick();
			signal_deliver_pending(task);
			if (--task->time_slice <= 0)
				task_yield();
		} else if (code == 9) {
			irq_handle_external(this_cpuid());
		} else {
			pr_warn("%s: pid=%ld,scause=%#" PRIx64 ",sepc=%#" PRIx64
				",stval=%#" PRIx64 "\n",
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
			pr_warn("%s: pid=%ld,scause=%#" PRIx64 ",sepc=%#" PRIx64
				",stval=%#" PRIx64 "\n",
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
	struct task *task;

	intr_off();

	write_stvec((uintptr_t)user_trap_vector);

	task = current_task();

	sstatus = read_sstatus();
	sstatus &= ~SSTATUS_SPP;
	sstatus |= SSTATUS_SPIE;
	write_sstatus(sstatus);

	arch_tf_set_kernel_sp(task->tf, task->kstack_top);
	arch_tf_set_cpuid(task->tf, this_cpuid());

	write_sepc(arch_tf_get_pc(task->tf));

	write_sstatus(read_sstatus() & ~SSTATUS_SUM);
}

static void print_hex(unsigned long num)
{
	char hex[] = "0123456789abcdef";
	char buf[32];
	int i = 0;

	if (num == 0) {
		sbi_console_putchar('0');
		return;
	}

	while (num > 0) {
		buf[i++] = hex[num & 0xF];
		num >>= 4;
	}

	while (i > 0) {
		sbi_console_putchar(buf[--i]);
	}
}

static bool early = true;

static void __early_trap_handler(struct riscv_trap_frame *frame)
{
	sbi_console_putstr(cause_to_str(frame->scause));
	sbi_console_putchar('\n');
	sbi_console_putstr("sepc :");
	print_hex(frame->sepc);
	sbi_console_putchar('\n');
	sbi_console_putstr("stval:");
	print_hex(frame->stval);
	sbi_console_putchar('\n');
	while (1) {
	}
}

static const char *reg_names[] = {
	"zero", "ra", "sp", "gp", "tp",	 "t0",	"t1", "t2", "s0", "s1", "a0",
	"a1",	"a2", "a3", "a4", "a5",	 "a6",	"a7", "s2", "s3", "s4", "s5",
	"s6",	"s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static void trap_panic(const char *name, unsigned long code,
		       struct riscv_trap_frame *frame)
{
	pr_error("trap: unhandled %s (code %lu)\n", name, code);
	pr_error("trap: sepc=%#018lx stval=%#018lx\n", frame->sepc,
		 frame->stval);
	pr_error("trap: scause=%#018lx sstatus=%#018lx\n", frame->scause,
		 frame->sstatus);
	for (int i = 0; i < 32; ++i) {
		pr_error("trap: %-4s %#018lx\n", reg_names[i], frame->x[i]);
	}
	panic("trap: unhandled %s (code %lu)\n", name, code);
}

#define EXCEPTION_INSTRUCTION_MISALIGNED 0
#define EXCEPTION_INSTRUCTION_ACCESS_FAULT 1
#define EXCEPTION_ILLEGAL_INSTRUCTION 2
#define EXCEPTION_BREAKPOINT 3
#define EXCEPTION_LOAD_MISALIGNED 4
#define EXCEPTION_LOAD_ACCESS_FAULT 5
#define EXCEPTION_STORE_MISALIGNED 6
#define EXCEPTION_STORE_ACCESS_FAULT 7
#define EXCEPTION_ECALL_FROM_USER 8
#define EXCEPTION_ECALL_FROM_SUPERVISOR 9
#define EXCEPTION_INSTRUCTION_PAGE_FAULT 12
#define EXCEPTION_LOAD_PAGE_FAULT 13
#define EXCEPTION_STORE_PAGE_FAULT 15

#define INTERRUPT_SUPERVISOR_SOFTWARE 1
#define INTERRUPT_SUPERVISOR_TIMER 5
#define INTERRUPT_SUPERVISOR_EXTERNAL 9

static void handle_interrupt(unsigned long code, struct riscv_trap_frame *frame)
{
	struct task *task = current_task();

	switch (code) {
	case INTERRUPT_SUPERVISOR_TIMER:
		handle_tick();
		if (task && --task->time_slice <= 0)
			task_yield();
		break;
	case INTERRUPT_SUPERVISOR_EXTERNAL:
		irq_handle_external(this_cpuid());
		break;
	default:
		trap_panic(interrupt_name(code), code, frame);
	}
}

static void handle_exception(unsigned long code, struct riscv_trap_frame *frame)
{
	trap_panic(exception_name(code), code, frame);
}

void __trap_handler(struct riscv_trap_frame *frame)
{
	if (early) {
		__early_trap_handler(frame);
	} else {
		bool is_interrupt = (frame->scause & BIT(63)) != 0;
		unsigned long code = frame->scause & ~BIT(63);
		if (is_interrupt)
			handle_interrupt(code, frame);
		else
			handle_exception(code, frame);
	}
}

extern char __trap_entry[];

void setup_trap_entry_early(void)
{
	write_stvec((unsigned long)__trap_entry);
}

void setup_trap_entry(void)
{
	write_stvec((unsigned long)__trap_entry);
	early = false;
}

void trap_init(void)
{
	trap_enable_timer();
	trap_enable_external();
	trap_enable_software();
}
