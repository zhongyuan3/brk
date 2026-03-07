#include <aosd/assert.h>
#include <aosd/cpu.h>
#include <aosd/irq.h>
#include <aosd/macros.h>
#include <aosd/panic.h>
#include <aosd/plic.h>
#include <aosd/printk.h>
#include <aosd/riscv.h>
#include <aosd/sbi.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
#include <aosd/syscall.h>
#include <aosd/timer.h>
#include <aosd/trap.h>
#include <aosd/types.h>

static char const *interrupt_str(unsigned int intno)
{
	static char const *intstrs[] = {
		"U-mode software interrupt",
		"S-mode software interrupt",
		"reserved-1",
		"M-mode software interrupt",
		"U-mode timer interrupt",
		"S-mode timer interrupt",
		"reserved-2",
		"M-mode timer interrupt",
		"U-mode external interrupt",
		"S-mode external interrupt",
		"reserved-3",
		"M-mode external interrupt",
		"reserved-4",
		"reserved-5",
		"reserved-6",
		"reserved-7",
	};

	if (intno >= countof(intstrs))
		return "unknown interrupt";
	else
		return intstrs[intno];
}

static char const *exception_str(unsigned int excno)
{
	static char const *excstrs[] = {
		"Instruction address misaligned",
		"Instruction access fault",
		"Illegal instruction",
		"Breakpoint",
		"Load address misaligned",
		"Load access fault",
		"Store/AMO address misaligned",
		"Store/AMO access fault",
		"Environment call from U-mode",
		"Environment call from S-mode",
		"reserved-1",
		"Environment call from M-mode",
		"Instruction page fault",
		"Load page fault",
		"reserved-2",
		"Store/AMO page fault",
	};

	if (excno >= countof(excstrs))
		return "unknown exception";
	else
		return excstrs[excno];
}

void trap_init(void)
{
	timer_init();
}

void trap_init_hart(uint32_t hart_id)
{
	write_stvec((uint64_t)kernel_trap_vector);
	write_sie(read_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);
	timer_set_next();
	intr_on();
}

void kernel_trap_handler(void)
{
	uint64_t sstatus = read_sstatus();
	uint64_t scause = read_scause();
	uint64_t sepc = read_sepc();
	uint64_t stval = read_stval();
	uint32_t code = scause & 0x3FF;
	struct task *task;

	assert(sstatus & SSTATUS_SPP);
	assert(!intr_enabled());

	if (scause & (1ULL << 63)) {
		switch (code) {
		case 5:
			timer_handle_int();
			task = current_task();
			if (task) {
				if (--task->time_slice <= 0)
					sched_yield();
			}
			break;
		case 9:
			irq_handle_external(current_cpu()->hart_id);
			break;
		default:
			panic("interrupt: %s, scause=%#lx, sepc=%#lx, stval=%#lx\n",
			      interrupt_str(code), scause, sepc, stval);
		}
	} else {
		panic("exception: %s, scause=%#lx, sepc=%#lx, stval=%#lx\n",
		      exception_str(code), scause, sepc, stval);
	}

	write_sepc(sepc);
	write_sstatus(sstatus);
}

struct task *user_trap_handler(void)
{
	struct task *t;
	uint64_t scause;
	uint32_t code;
	struct cpu *c;
	uint64_t sepc;
	uint64_t stval;

	write_stvec((uint64_t)kernel_trap_vector);

	t = (struct task *)read_sscratch();
	c = t->cpu;
	write_tp((uint64_t)c);
	write_sstatus(read_sstatus() | SSTATUS_SUM);

	assert(!(read_sstatus() & SSTATUS_SPP));
	assert(!intr_enabled());

	uint64_t jiffies = jiffies_get();
	t->proc_tms.tms_utime += jiffies - t->last_utime;
	t->last_ktime = jiffies;

	scause = read_scause();
	sepc = read_sepc();
	stval = read_stval();
	code = scause & 0x3FF;
	t->tf.epc = sepc;
	if (scause & (1ULL << 63)) {
		switch (code) {
		case 5:
			timer_handle_int();
			if (t) {
				if (task_is_killed(t))
					sched_exit(1);
				if (--t->time_slice <= 0)
					sched_yield();
			}
			break;
		case 9:
			irq_handle_external(c->hart_id);
			break;
		default:
			printk("USER INTERRUPT: %s: pid=%ld,scause=%#lx,sepc=%#lx,stval=%#lx\n",
			       interrupt_str(code), t->pid, scause, sepc,
			       stval);
			sched_exit(1);
			break;
		}
	} else {
		switch (code) {
		case 8:
			if (task_is_killed(t))
				sched_exit(1);
			t->tf.epc += 4;
			intr_on();
			syscall();
			break;
		default:
			printk("USER EXCEPTION: %s: pid=%ld,scause=%#lx,sepc=%#lx,stval=%#lx\n",
			       exception_str(code), t->pid, scause, sepc,
			       stval);
			task_set_killed(t);
			break;
		}
	}

	if (task_is_killed(t))
		sched_exit(1);

	prepare_to_return();
	jiffies = jiffies_get();
	t->proc_tms.tms_stime += jiffies - t->last_ktime;
	t->last_utime = jiffies;
	return t;
}

void prepare_to_return(void)
{
	uint64_t sstatus;
	struct task *task;

	intr_off();

	write_stvec((uint64_t)user_trap_vector);

	task = current_cpu()->current;

	sstatus = read_sstatus();
	sstatus &= ~SSTATUS_SPP;
	sstatus |= SSTATUS_SPIE;
	write_sstatus(sstatus);

	task->tf.kernel_sp = task->stack + KSTACK_SIZE;

	write_sepc(task->tf.epc);

	write_sstatus(read_sstatus() & ~SSTATUS_SUM);
}
