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
	struct task *task;

	assert(sstatus & SSTATUS_SPP);
	assert(!intr_enabled());

	if (TRAP_IS_INTERRUPT(scause)) {
		switch (TRAP_CAUSE_CODE(scause)) {
		case 1:
			panic("Supervisor software interrupt: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 3:
			panic("Machine software interrupt: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 5:
			timer_handle_int();
			task = current_task();
			if (task) {
				if (--task->time_slice <= 0)
					sched_yield();
			}
			break;
		case 7:
			panic("Supervisor timer interrupt: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 9:
			irq_handle_external(current_cpu()->hart_id);
			break;
		case 11:
			panic("Machine external interrupt: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 13:
			panic("Counter-overflow interrupt: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		default:
			panic("Unexpected interrupt: scause=%#lx,sepc=%#lx,stval=%#lx\n",
			      scause, sepc, stval);
			break;
		}
	} else {
		switch (TRAP_CAUSE_CODE(scause)) {
		case 0:
			panic("Instruction address misaligned: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 1:
			panic("Instruction access fault: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 2:
			panic("Illegal instruction: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 3:
			panic("Breakpoint: sepc=%#lx,stval=%#lx\n", sepc,
			      stval);
			break;
		case 4:
			panic("Load address misaligned: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 5:
			panic("Load access fault: sepc=%#lx,stval=%#lx\n", sepc,
			      stval);
			break;
		case 6:
			panic("Store/AMO address misaligned: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 7:
			panic("Store/AMO access fault: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 8:
			panic("Environment call from U-mode: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 9:
			panic("Environment call from S-mode: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 11:
			panic("Environment call from M-mode: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 12:
			panic("Instruction page fault: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 13:
			panic("Load page fault: sepc=%#lx,stval=%#lx\n", sepc,
			      stval);
			break;
		case 15:
			panic("Store/AMO page fault: sepc=%#lx,stval=%#lx\n",
			      sepc, stval);
			break;
		case 16:
			panic("Double trap: sepc=%#lx,stval=%#lx\n", sepc,
			      stval);
			break;
		case 18:
			panic("Software check: sepc=%#lx,stval=%#lx\n", sepc,
			      stval);
			break;
		case 19:
			panic("Hardware error: sepc=%#lx,stval=%#lx\n", sepc,
			      stval);
			break;
		default:
			panic("Unexpected exception: scause=%#lx,sepc=%#lx,stval=%#lx\n",
			      scause, sepc, stval);
			break;
		}
	}

	write_sepc(sepc);
	write_sstatus(sstatus);
}

struct task *user_trap_handler(void)
{
	struct task *t;
	uint64_t scause;
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
	t->tf.epc = sepc;

	if (TRAP_IS_INTERRUPT(scause)) {
		switch (TRAP_CAUSE_CODE(scause)) {
		case 1:
			printk("Supervisor software interrupt: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 3:
			printk("Machine software interrupt: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 5:
			timer_handle_int();
			if (t) {
				if (task_is_killed(t))
					sched_exit(1);
				if (--t->time_slice <= 0)
					sched_yield();
			}
			break;
		case 7:
			printk("Supervisor timer interrupt: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 9:
			irq_handle_external(c->hart_id);
			break;
		case 11:
			printk("Machine external interrupt: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 13:
			printk("Counter-overflow interrupt: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		default:
			printk("Unexpected interrupt: scause=%#lx,sepc=%#lx,stval=%#lx\n",
			       scause, sepc, stval);
			task_set_killed(t);
			break;
		}
	} else {
		switch (TRAP_CAUSE_CODE(scause)) {
		case 0:
			printk("Instruction address misaligned: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 1:
			printk("Instruction access fault: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 2:
			printk("Illegal instruction: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 3:
			printk("Breakpoint: sepc=%#lx,stval=%#lx\n", sepc,
			       stval);
			task_set_killed(t);
			break;
		case 4:
			printk("Load address misaligned: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 5:
			printk("Load access fault: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 6:
			printk("Store/AMO address misaligned: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 7:
			printk("Store/AMO access fault: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 8:
			if (task_is_killed(t))
				sched_exit(1);
			t->tf.epc += 4; /* skip ecall */
			intr_on();
			syscall();
			break;
		case 9:
			printk("Environment call from S-mode: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 11:
			printk("Environment call from M-mode: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 12:
			printk("Instruction page fault: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 13:
			printk("Load page fault: sepc=%#lx,stval=%#lx\n", sepc,
			       stval);
			task_set_killed(t);
			break;
		case 15:
			printk("Store/AMO page fault: sepc=%#lx,stval=%#lx\n",
			       sepc, stval);
			task_set_killed(t);
			break;
		case 16:
			printk("Double trap: sepc=%#lx,stval=%#lx\n", sepc,
			       stval);
			task_set_killed(t);
			break;
		case 18:
			printk("Software check: sepc=%#lx,stval=%#lx\n", sepc,
			       stval);
			task_set_killed(t);
			break;
		case 19:
			printk("Hardware error: sepc=%#lx,stval=%#lx\n", sepc,
			       stval);
			task_set_killed(t);
			break;
		default:
			printk("Unexpected exception: scause=%#lx,sepc=%#lx,stval=%#lx\n",
			       scause, sepc, stval);
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
