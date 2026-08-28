#include <arch/irqflags.h>
#include <arch/mm.h>
#include <brk/base/assert.h>
#include <brk/base/list.h>
#include <brk/base/types.h>
#include <brk/fs/fdtable.h>
#include <brk/fs/fs.h>
#include <brk/fs/fsinfo.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/mm.h>
#include <brk/printk/panic.h>
#include <brk/process/processor.h>
#include <brk/process/task.h>
#include <brk/process/task_types.h>
#include <brk/time/timekeeper.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/limits.h>

SPINLOCK_DEFINE(wait_lock);

static LIST_DEFINE(run_queue);
static SPINLOCK_DEFINE(run_queue_lock);
static LIST_DEFINE(sleep_queue);
static SPINLOCK_DEFINE(sleep_queue_lock);

void task_join(struct task_control_block *task)
{
	spinlock_acquire(&run_queue_lock);
	list_add_tail(&task->queue, &run_queue);
	spinlock_release(&run_queue_lock);
}

static struct task_control_block *task_pick_next(void)
{
	struct task_control_block *task = NULL;
	spinlock_acquire(&run_queue_lock);
	if (!list_empty(&run_queue)) {
		task = list_first_entry(&run_queue, struct task_control_block,
					queue);
		list_del_init(&task->queue);
	}
	spinlock_release(&run_queue_lock);
	return task;
}

void task_scheduler(void)
{
	struct cpu *cpu = current_cpu();
	struct task_control_block *next = NULL;

	cpu->handoff = NULL;
	for (;;) {
		task_sched_resume();

		intr_on();
		intr_off();

		next = task_pick_next();
		if (!next) {
			intr_on();
			arch_cpu_idle();
			continue;
		}

		spinlock_acquire(&next->lock);
		set_current_task(next);
		cpu->irq_enabled = next->irq_enabled;
		switch_pgtable(next->mm->pgd);
		user_access_enable();
		next->time_slice = TIME_SLICE_MAX;
		next->ktime = jiffies_get();
		switch_context(&cpu->ctx, &next->ctx);

		set_current_task(NULL);
	}
}

void task_sched(void)
{
	struct task_control_block *curr = current_task();
	struct cpu *cpu = current_cpu();

	ASSERT(!intr_enabled());
	ASSERT(spinlock_holding(&curr->lock));
	ASSERT(current_cpu()->irq_nest == 1);

	set_current_task(NULL);
	curr->irq_enabled = cpu->irq_enabled;

	switch (curr->state) {
	case TASK_STATE_RUNNING:
		task_join(curr);
		break;
	case TASK_STATE_ZOMBIE:
		break;
	case TASK_STATE_SLEEPING:
		spinlock_acquire(&sleep_queue_lock);
		list_add_tail(&curr->queue, &sleep_queue);
		spinlock_release(&sleep_queue_lock);
		break;
	default:
		panic("unexpected state: pid=%ld, state=%d\n", curr->pid,
		      curr->state);
	}

	struct task_control_block *next = task_pick_next();
	if (next == curr) {
		curr->time_slice = TIME_SLICE_MAX;
		set_current_task(curr);
		return;
	}

	task_add_system_time(curr, jiffies_get() - curr->ktime);
	cpu->handoff = curr;
	if (next) {
		spinlock_acquire(&next->lock);
		set_current_task(next);
		cpu->irq_enabled = next->irq_enabled;
		switch_pgtable(next->mm->pgd);
		next->time_slice = TIME_SLICE_MAX;
		next->ktime = jiffies_get();
		switch_context(&curr->ctx, &next->ctx);
	} else {
		set_current_task(NULL);
		switch_context(&curr->ctx, &cpu->ctx);
	}

	task_sched_resume();
}

void task_yield(void)
{
	struct task_control_block *task = current_task();
	spinlock_acquire(&task->lock);
	task_sched();
	spinlock_release(&task->lock);
}

void task_sleep(void *chan, spinlock_t *lock)
{
	struct task_control_block *task = current_task();
	spinlock_acquire(&task->lock);
	spinlock_release(lock);
	task->state = TASK_STATE_SLEEPING;
	task->chan = chan;
	task_sched();
	task->chan = NULL;
	spinlock_release(&task->lock);
	spinlock_acquire(lock);
}

void task_wake_all(void *chan)
{
	for (;;) {
		struct task_control_block *p, *wakee = NULL;

		spinlock_acquire(&sleep_queue_lock);
		list_for_each_entry(p, &sleep_queue, queue) {
			spinlock_acquire(&p->lock);
			if (p->state == TASK_STATE_SLEEPING &&
			    p->chan == chan) {
				p->state = TASK_STATE_RUNNING;
				p->chan = NULL;
				list_del_init(&p->queue);
				spinlock_release(&p->lock);
				wakee = p;
				break;
			}
			spinlock_release(&p->lock);
		}
		spinlock_release(&sleep_queue_lock);
		if (!wakee)
			return;
		task_join(wakee);
	}
}

void task_wake_spec(struct task_control_block *task)
{
	spinlock_acquire(&sleep_queue_lock);
	spinlock_acquire(&task->lock);
	if (task->state == TASK_STATE_SLEEPING) {
		task->state = TASK_STATE_RUNNING;
		task->chan = NULL;
		list_del_init(&task->queue);
		spinlock_release(&task->lock);
		spinlock_release(&sleep_queue_lock);
		task_join(task);
		return;
	}
	spinlock_release(&task->lock);
	spinlock_release(&sleep_queue_lock);
}

void task_exit_normal(int code)
{
	task_exit((code & 0xff) << 8);
}

void task_exit_signal(int sig)
{
	task_exit(sig & 0x7f);
}

void task_exit(int status)
{
	struct task_control_block *child;
	struct task_control_block *task = current_task();

	if (task == initial_task)
		panic("%s(): init exit\n", __func__);

	fdtable_close_all(task->fdtable);
	fsinfo_free_resources(task->fsinfo);

	spinlock_acquire(&wait_lock);
	list_for_each_entry(child, &task->children, child) {
		child->parent = initial_task;
	}
	list_splice(&task->children, &initial_task->children);
	task_wake_spec(task->parent);
	spinlock_release(&wait_lock);

	spinlock_acquire(&task->lock);

	task->state = TASK_STATE_ZOMBIE;
	task->exit_status = status;

	task_sched();

	panic("scheduling zombie\n");
}

pid_t task_wait(pid_t pid, int *status, int options, struct rusage *rus)
{
	struct task_control_block *child;
	struct task_control_block *parent = current_task();
	bool found;

	(void)options;
	(void)rus;

	spinlock_acquire(&wait_lock);

	if (list_empty(&parent->children)) {
		spinlock_release(&wait_lock);
		return -ECHILD;
	}

again:
	found = false;

	list_for_each_entry(child, &parent->children, child) {
		spinlock_acquire(&child->lock);
		if (pid > 0 && child->tgid == pid) {
			found = true;
		}

		if (child->state != TASK_STATE_ZOMBIE ||
		    (pid > 0 && child->tgid != pid)) {
			spinlock_release(&child->lock);
			continue;
		}

		if (status)
			*status = child->exit_status;
		pid = child->tgid;
		task_add_child_system_time(
			parent, task_get_system_time(child) +
					task_get_child_system_time(child));
		task_add_child_user_time(
			parent, task_get_user_time(child) +
					task_get_child_user_time(child));
		spinlock_release(&child->lock);
		list_del_init(&child->child);
		spinlock_release(&wait_lock);
		task_destroy(child);
		return pid;
	}

	if (pid > 0 && !found) {
		spinlock_release(&wait_lock);
		return -ECHILD;
	}

	task_sleep(parent, &wait_lock);

	goto again;
}

void task_sched_resume(void)
{
	struct cpu *cpu = current_cpu();
	if (cpu->handoff) {
		spinlock_release(&cpu->handoff->lock);
		cpu->handoff = NULL;
	}
}
