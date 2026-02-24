#include <aosd/assert.h>
#include <aosd/cpu.h>
#include <aosd/errno.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/printk.h>
#include <aosd/riscv.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
#include <aosd/spinlock.h>
#include <aosd/trap.h>
#include <aosd/types.h>

static spinlock_define(wait_lock);

static void switch_pgtable(pgde_t *pgd)
{
	uint64_t paddr = virt_to_phys((uint64_t)pgd);
	uint64_t satp = make_satp_sv39(paddr);
	write_satp(satp);
	sfence_vma();
}

void scheduler(void)
{
	struct cpu *c = current_cpu();
	struct task *n = NULL;
	bool have_runnable = false;

	for (;;) {
		intr_on();
		intr_off();

		have_runnable = false;

		for (n = tasks; n < tasks + NR_TASKS; ++n) {
			spinlock_acquire(&n->lock);
			if (n->state == TASK_RUNNABLE) {
				c->current = n;
				n->cpu = c;
				n->state = TASK_RUNNING;
				n->time_slice = DEFAULT_TIME_SLICE;
				switch_pgtable(n->pgd);
				switch_context(&c->ctx, &n->ctx);
				c->current = NULL;
				n->cpu = NULL;
				have_runnable = true;
			}
			spinlock_release(&n->lock);
		}

		if (!have_runnable) {
			intr_on();
			asm volatile("wfi");
		}
	}
}

void sched(void)
{
	bool intena;
	struct task *t;

	t = current_task();

	assert(spinlock_holding(&t->lock));
	assert(current_cpu()->noff == 1);
	assert(t->state != TASK_RUNNING);
	assert(!intr_enabled());

	intena = current_cpu()->intena;
	switch_context(&t->ctx, &current_cpu()->ctx);
	current_cpu()->intena = intena;
}

void sched_yield(void)
{
	struct task *t = current_task();
	spinlock_acquire(&t->lock);
	t->state = TASK_RUNNABLE;
	sched();
	spinlock_release(&t->lock);
}

void sched_sleep(void *chan, spinlock_t *lock)
{
	struct task *t = current_task();
	spinlock_acquire(&t->lock);
	spinlock_release(lock);
	t->state = TASK_SLEEPING;
	t->chan = chan;
	sched();
	spinlock_release(&t->lock);
	spinlock_acquire(lock);
}

void sched_wake_up(void *chan)
{
	struct task *t;

	for (t = tasks; t < tasks + NR_TASKS; ++t) {
		spinlock_acquire(&t->lock);
		if (t->state == TASK_SLEEPING && t->chan == chan) {
			t->state = TASK_RUNNABLE;
			t->chan = NULL;
		}
		spinlock_release(&t->lock);
	}
}

void sched_exit(int status)
{
	struct task *k;
	struct task *t = current_task();

	if (t == init_task)
		panic("%s(): init task exit\n", __func__);

	spinlock_acquire(&wait_lock);

	for (k = tasks; k < tasks + NR_TASKS; ++k)
		if (k->parent == t)
			k->parent = init_task;

	sched_wake_up(t->parent);

	spinlock_acquire(&t->lock);

	t->state = TASK_ZOMBIE;
	t->exit_status = status;

	spinlock_release(&wait_lock);

	sched();

	panic("scheduling zombie task\n");
}

int sched_wait(int *status, pid_t *pid)
{
	bool have_kids;
	struct task *t;
	struct task *c = current_task();

	spinlock_acquire(&wait_lock);

again:
	have_kids = false;

	for (t = tasks; t < tasks + NR_TASKS; ++t) {
		if (t->parent == c) {
			spinlock_acquire(&t->lock);
			have_kids = true;
			if (t->state == TASK_ZOMBIE) {
				if (pid)
					*pid = t->pid;
				if (status)
					*status = t->exit_status;
				task_destroy(t);
				spinlock_release(&wait_lock);
				return 0;
			}
			spinlock_release(&t->lock);
		}
	}

	if (!have_kids) {
		spinlock_release(&wait_lock);
		return -ECHILD;
	}

	sched_sleep(c, &wait_lock);

	goto again;
}

struct task *current_task(void)
{
	push_off();
	struct cpu *cpu = current_cpu();
	struct task *task = cpu->current;
	pop_off();
	return task;
}

void fork_return(void)
{
	struct task *t = current_task();
	spinlock_release(&t->lock);
	if (t->thread_entry)
		t->thread_entry();
	prepare_to_return();
	user_trap_return(t);
}

void task_set_killed(struct task *t)
{
	spinlock_acquire(&t->lock);
	t->killed = true;
	spinlock_release(&t->lock);
}

bool task_is_killed(struct task *t)
{
	bool killed;
	spinlock_acquire(&t->lock);
	killed = t->killed;
	spinlock_release(&t->lock);
	return killed;
}
