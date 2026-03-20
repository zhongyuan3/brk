#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/limits.h>
#include <aosd/lock.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/process.h>
#include <aosd/riscv.h>
#include <aosd/timer.h>
#include <aosd/types.h>

extern void switch_context(struct context *prev, struct context *next);

SPINLOCK_DEFINE(wait_lock);
struct task_struct *init_task;

static bool __scheduler(struct processor *proc, processor_id_t proc_id)
{
	struct task_struct *next;
	bool have_runnable = false;

	for (next = tasks; next < tasks + NR_TASKS; ++next) {
		spinlock_acquire(&next->lock);
		if (next->state == TASK_STATE_RUNNABLE) {
			proc->task = next;
			next->on_proc = proc_id;
			next->state = TASK_STATE_RUNNING;
			next->time_slice = TASK_TIME_SLICE;
			switch_pgtable(next->mm->pgd);
			write_sstatus(read_sstatus() | SSTATUS_SUM);
			next->ktime = jiffies_get();
			switch_context(&proc->ctx, &next->ctx);
			proc->task = NULL;
			next->on_proc = -1;
			next->ptms.tms_stime += jiffies_get() - next->ktime;
			have_runnable = true;
		}
		spinlock_release(&next->lock);
	}

	return have_runnable;
}

void scheduler(void)
{
	struct processor *proc = current_processor();
	processor_id_t proc_id = current_processor_id();

	for (;;) {
		intr_on();
		intr_off();

		if (!__scheduler(proc, proc_id)) {
			intr_on();
			asm volatile("wfi");
		}
	}
}

static void __sched(void)
{
	bool enabled;
	struct task_struct *task = current_task();

	assert(spinlock_holding(&task->lock));
	assert(current_processor()->irq_nest == 1);
	assert(task->state != TASK_STATE_RUNNING);
	assert(!intr_enabled());

	enabled = current_processor()->irq_enabled;
	switch_context(&task->ctx, &current_processor()->ctx);
	current_processor()->irq_enabled = enabled;
}

void task_yield(void)
{
	struct task_struct *task = current_task();
	spinlock_acquire(&task->lock);
	task->state = TASK_STATE_RUNNABLE;
	__sched();
	spinlock_release(&task->lock);
}

void task_sleep(void *chan, spinlock_t *lock)
{
	struct task_struct *task = current_task();
	spinlock_acquire(&task->lock);
	spinlock_release(lock);
	task->state = TASK_STATE_SLEEPING;
	task->chan = chan;
	__sched();
	spinlock_release(&task->lock);
	spinlock_acquire(lock);
}

void task_wake_up(void *chan)
{
	struct task_struct *task;

	for (task = tasks; task < tasks + NR_TASKS; ++task) {
		spinlock_acquire(&task->lock);
		if (task->state == TASK_STATE_SLEEPING && task->chan == chan) {
			task->state = TASK_STATE_RUNNABLE;
			task->chan = NULL;
		}
		spinlock_release(&task->lock);
	}
}

void task_exit(int status)
{
	struct task_struct *child;
	struct task_struct *parent = current_task();

	if (parent == init_task)
		panic("%s(): init task exit\n", __func__);

	for (int i = 0; i < OPEN_MAX; ++i) {
		if (parent->ofiles[i]) {
			file_put(parent->ofiles[i]);
			parent->ofiles[i] = NULL;
		}
	}
	dentry_put(parent->cwd);

	spinlock_acquire(&wait_lock);
	for (child = tasks; child < tasks + NR_TASKS; ++child) {
		if (child->parent == parent)
			child->parent = init_task;
	}
	task_wake_up(parent->parent);
	spinlock_release(&wait_lock);

	spinlock_acquire(&parent->lock);

	parent->state = TASK_STATE_ZOMBIE;
	parent->exit_status = status;

	__sched();

	panic("scheduling zombie task\n");
}

pid_t task_wait(pid_t pid, int *status, int options, struct rusage *rus)
{
	bool found;
	struct task_struct *child;
	struct task_struct *parent = current_task();

	spinlock_acquire(&wait_lock);

again:
	found = false;

	for (child = tasks; child < tasks + NR_TASKS; ++child) {
		if (child->parent == parent) {
			spinlock_acquire(&child->lock);
			if (pid > 0 && child->pid != pid) {
				spinlock_release(&child->lock);
				continue;
			}
			found = true;
			if (child->state == TASK_STATE_ZOMBIE) {
				if (status)
					*status = child->exit_status;
				pid = child->pid;
				parent->ptms.tms_cstime +=
					child->ptms.tms_stime;
				parent->ptms.tms_cutime +=
					child->ptms.tms_utime;
				task_free(child);
				spinlock_release(&wait_lock);
				return pid;
			}
			spinlock_release(&child->lock);
		}
	}

	if (!found) {
		spinlock_release(&wait_lock);
		return -ECHILD;
	}

	task_sleep(parent, &wait_lock);

	goto again;
}

struct task_struct *current_task(void)
{
	struct task_struct *task;
	push_off();
	task = current_processor()->task;
	pop_off();
	return task;
}
