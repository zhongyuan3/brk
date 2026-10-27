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
#include <brk/stdnoreturn.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/types.h>

static const char *const exception_names[] = {
	[EXCEPTION_INSTRUCTION_MISALIGNED] = "Instruction address misaligned",
	[EXCEPTION_INSTRUCTION_ACCESS_FAULT] = "Instruction access fault",
	[EXCEPTION_ILLEGAL_INSTRUCTION] = "Illegal instruction",
	[EXCEPTION_BREAKPOINT] = "Breakpoint",
	[EXCEPTION_LOAD_MISALIGNED] = "Load address misaligned",
	[EXCEPTION_LOAD_ACCESS_FAULT] = "Load access fault",
	[EXCEPTION_STORE_MISALIGNED] = "Store/AMO address misaligned",
	[EXCEPTION_STORE_ACCESS_FAULT] = "Store/AMO access fault",
	[EXCEPTION_ECALL_FROM_USER] = "Environment call from U-mode",
	[EXCEPTION_ECALL_FROM_SUPERVISOR] = "Environment call from S-mode",
	[EXCEPTION_INSTRUCTION_PAGE_FAULT] = "Instruction page fault",
	[EXCEPTION_LOAD_PAGE_FAULT] = "Load page fault",
	[EXCEPTION_STORE_PAGE_FAULT] = "Store/AMO page fault",
};

static const char *const interrupt_names[] = {
	[INTERRUPT_SUPERVISOR_SOFTWARE] = "Supervisor software interrupt",
	[INTERRUPT_SUPERVISOR_TIMER] = "Supervisor timer interrupt",
	[INTERRUPT_SUPERVISOR_EXTERNAL] = "Supervisor external interrupt",
};

static const char *const reg_names[] = {
	"zero", "ra", "sp", "gp", "tp",	 "t0",	"t1", "t2", "s0", "s1", "a0",
	"a1",	"a2", "a3", "a4", "a5",	 "a6",	"a7", "s2", "s3", "s4", "s5",
	"s6",	"s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static const char *exception_name(unsigned long code)
{
	if (code > countof(exception_names) || !exception_names[code])
		return "Unexpected exception";
	else
		return exception_names[code];
}

static const char *interrupt_name(unsigned long code)
{
	if (code > countof(interrupt_names) || !interrupt_names[code])
		return "Unexpected interrupt";
	else
		return interrupt_names[code];
}

static void trap_enable_timer(void)
{
	write_sie(read_sie() | SIE_STIE);
}

static void trap_enable_external(void)
{
	write_sie(read_sie() | SIE_SEIE);
}

static void trap_enable_software(void)
{
	write_sie(read_sie() | SIE_SSIE);
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

static bool in_early = true;
static bool in_early_trap = false;

static void noreturn early_trap_handler(struct riscv_trap_frame *frame,
					bool is_interrupt, unsigned long code)
{
	if (in_early_trap) {
		sbi_console_putstr("EARLY NESTED TRAP");
		while (1) {
		}
	} else {
		in_early_trap = true;
	}

	if (is_interrupt) {
		sbi_console_putstr("EARLY INTERRUPT: ");
		sbi_console_putstr(interrupt_name(code));
	} else {
		sbi_console_putstr("EARLY EXCEPTION: ");
		sbi_console_putstr(exception_name(code));
	}
	sbi_console_putchar('\n');

	sbi_console_putstr("sstatus:");
	print_hex(frame->sstatus);
	sbi_console_putchar('\n');

	sbi_console_putstr("scause :");
	print_hex(frame->scause);
	sbi_console_putchar('\n');

	sbi_console_putstr("sepc   :");
	print_hex(frame->sepc);
	sbi_console_putchar('\n');

	sbi_console_putstr("stval  :");
	print_hex(frame->stval);
	sbi_console_putchar('\n');

	while (1) {
	}
}

static void noreturn trap_panic(const char *name, unsigned long code,
				struct riscv_trap_frame *frame)
{
	pr_emerg("trap: unhandled %s (code %lu)\n", name, code);
	pr_emerg("trap: sepc=%#018lx stval=%#018lx\n", frame->sepc,
		 frame->stval);
	pr_emerg("trap: scause=%#018lx sstatus=%#018lx\n", frame->scause,
		 frame->sstatus);
	for (int i = 0; i < 32; ++i) {
		pr_emerg("trap: %-4s %#018lx\n", reg_names[i], frame->x[i]);
	}
	panic("trap: unhandled %s (code %lu)\n", name, code);
}

static void handle_interrupt(unsigned long code, struct riscv_trap_frame *frame,
			     bool from_user, struct task *task)
{
	switch (code) {
	case INTERRUPT_SUPERVISOR_SOFTWARE:
		clear_ssip_csr();
		break;
	case INTERRUPT_SUPERVISOR_TIMER:
		handle_tick();
		if (from_user)
			signal_deliver_pending(task);
		if (task && --task->time_slice <= 0)
			task_yield();
		break;
	case INTERRUPT_SUPERVISOR_EXTERNAL:
		irq_handle_external(this_cpuid());
		break;
	default:
		if (from_user) {
			task_set_killed(task);
			pr_warn("%s: pid=%ld,scause=%#" PRIx64 ",sepc=%#" PRIx64
				",stval=%#" PRIx64 "\n",
				interrupt_name(code), task->pid, frame->scause,
				frame->sepc, frame->stval);
		} else {
			trap_panic(interrupt_name(code), code, frame);
		}
	}
}

static void handle_exception(unsigned long code, struct riscv_trap_frame *frame,
			     bool from_user, struct task *task)
{
	switch (code) {
	case EXCEPTION_ECALL_FROM_USER:
		if (from_user) {
			signal_deliver_pending(task);
			frame->sepc += 4;
			intr_on();
			syscall();
		}
		break;
	default:
		if (from_user) {
			pr_warn("%s: pid=%ld,scause=%#" PRIx64 ",sepc=%#" PRIx64
				",stval=%#" PRIx64 "\n",
				interrupt_name(code), task->pid, frame->scause,
				frame->sepc, frame->stval);
			task_set_killed(task);
		} else {
			trap_panic(exception_name(code), code, frame);
		}
	}
}

void trap_handler(struct riscv_trap_frame *frame)
{
	bool is_interrupt = (frame->scause & BIT(63)) != 0;
	unsigned long code = frame->scause & ~BIT(63);

	if (in_early)
		early_trap_handler(frame, is_interrupt, code);

	bool from_user = (frame->sstatus & SSTATUS_SPP) == 0;
	struct task *task = current_task();
	u64 jiffies = 0;

	if (from_user) {
		write_sstatus(read_sstatus() | SSTATUS_SUM);
		jiffies = jiffies_get();
		task_add_user_time(task, jiffies - task->utime);
		task->ktime = jiffies;
	}

	if (is_interrupt)
		handle_interrupt(code, frame, from_user, task);
	else
		handle_exception(code, frame, from_user, task);

	if (from_user) {
		unsigned long sstatus = read_sstatus();
		sstatus &= ~SSTATUS_SPP;
		sstatus |= SSTATUS_SPIE;
		sstatus &= ~SSTATUS_SUM;
		frame->sstatus = sstatus;
		signal_deliver_pending(task);
		jiffies = jiffies_get();
		task_add_system_time(task, jiffies - task->ktime);
		task->utime = jiffies;
	}
}

extern char trap_entry[];

void setup_trap_entry_early(void)
{
	write_stvec((unsigned long)trap_entry);
}

void setup_trap_entry(void)
{
	write_stvec((unsigned long)trap_entry);
	in_early = false;
}

void trap_init(void)
{
	trap_enable_timer();
	trap_enable_external();
	trap_enable_software();
}
