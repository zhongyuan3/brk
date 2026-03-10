#include <aosd/assert.h>
#include <aosd/cpu.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/limits.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/printk.h>
#include <aosd/riscv.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
#include <aosd/string.h>
#include <aosd/timer.h>
#include <aosd/trap.h>
#include <aosd/types.h>

static SPINLOCK_DEFINE(wait_lock);

void scheduler(void)
{
	struct cpu *c = current_cpu();
	struct task *n = NULL;
	bool have_runnable = false;
	uint64_t jiffies = 0;

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
				switch_pgtable(n->mm->pgd);
				write_sstatus(read_sstatus() | SSTATUS_SUM);
				jiffies = jiffies_get();
				n->last_ktime = jiffies;
				switch_context(&c->ctx, &n->ctx);
				c->current = NULL;
				n->cpu = NULL;
				jiffies = jiffies_get();
				n->proc_tms.tms_stime +=
					jiffies - n->last_ktime;
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

	for (int i = 0; i <= OPEN_MAX; ++i) {
		if (t->ofiles[i]) {
			file_put(t->ofiles[i]);
			t->ofiles[i] = NULL;
		}
	}
	dentry_put(t->cwd);

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

pid_t do_wait4(pid_t child_pid, int *status, int options, struct rusage *rus)
{
	bool have_kids;
	struct task *child;
	struct task *parent = current_task();

	spinlock_acquire(&wait_lock);

again:
	have_kids = false;

	for (child = tasks; child < tasks + NR_TASKS; ++child) {
		if (child->parent == parent) {
			spinlock_acquire(&child->lock);
			if (child_pid >= 0 && child->pid != child_pid) {
				spinlock_release(&child->lock);
				continue;
			}
			have_kids = true;
			if (child->state == TASK_ZOMBIE) {
				if (status)
					*status = child->exit_status;
				child_pid = child->pid;
				parent->proc_tms.tms_cstime +=
					child->proc_tms.tms_stime;
				parent->proc_tms.tms_cutime +=
					child->proc_tms.tms_utime;
				task_free(child);
				spinlock_release(&wait_lock);
				return child_pid;
			}
			spinlock_release(&child->lock);
		}
	}

	if (!have_kids) {
		spinlock_release(&wait_lock);
		return -ECHILD;
	}

	sched_sleep(parent, &wait_lock);

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

	if (t == init_task)
		init_task_entry();

	static volatile bool first = true;
	if (first) {
		first = false;
		fs_init();
		char *argv[] = { "/bin/sh", 0 };
		char *envp[] = { 0 };
		int ret = do_execve(argv[0], argv, envp);
		if (ret < 0)
			panic("execve %s failed: %s\n", argv[0], strerror(ret));
		t->tf.a0 = ret;
	}

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

int task_fork(void)
{
	pid_t child_pid;
	size_t i = 0;
	struct task *parent = current_task();
	struct task *child = task_alloc();
	int err;

	if (!child)
		return -ENOMEM;

	err = mm_copy(child->mm, parent->mm);
	if (err) {
		task_free(child);
		return err;
	}

	memcpy(&child->tf, &parent->tf, sizeof(parent->tf));

	for (; i <= OPEN_MAX; ++i)
		if (parent->ofiles[i] != NULL)
			child->ofiles[i] = file_dup(parent->ofiles[i]);

	child->cwd = dentry_dup(parent->cwd);

	child->tf.a0 = 0;

	child_pid = child->pid;
	spinlock_release(&child->lock);

	spinlock_acquire(&wait_lock);
	child->parent = parent;
	spinlock_release(&wait_lock);

	spinlock_acquire(&child->lock);
	child->state = TASK_RUNNABLE;
	spinlock_release(&child->lock);

	return child_pid;
}
