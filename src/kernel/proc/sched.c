#include <arch/irqflags.h>
#include <arch/mm.h>
#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/fsinfo.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/spinlock.h>
#include <brk/task.h>
#include <brk/task_types.h>
#include <brk/timekeeper.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/limits.h>

SPINLOCK_DEFINE(wait_lock);

static LIST_DEFINE(run_queue);
static SPINLOCK_DEFINE(run_queue_lock);
static LIST_DEFINE(sleep_queue);
static SPINLOCK_DEFINE(sleep_queue_lock);

void task_join(struct task *task)
{
	irqstate_t flags = spin_lock_irqsave(&run_queue_lock);

	list_add_tail(&task->queue, &run_queue);
	spin_unlock_irqrestore(&run_queue_lock, flags);
}

static struct task *task_pick_next(void)
{
	struct task *task = NULL;
	spin_lock(&run_queue_lock);
	if (!list_empty(&run_queue)) {
		task = list_first_entry(&run_queue, struct task, queue);
		list_del_init(&task->queue);
	}
	spin_unlock(&run_queue_lock);
	return task;
}

void task_scheduler(void)
{
	struct cpu *cpu = this_cpu();
	struct task *next = NULL;

	cpu->handoff = NULL;
	while (1) {
		task_sched_resume();

		local_irq_enable();
		local_irq_disable();

		next = task_pick_next();
		if (!next) {
			local_irq_enable();
			cpu_idle();
			continue;
		}

		raw_spin_lock(&next->lock);
		set_current_task(next);
		switch_pgtable(next->mm->pgd);
		user_access_enable();
		next->time_slice = TIME_SLICE_MAX;
		next->ktime = jiffies_get();
		__switch_to(&cpu->kernel_sp, &next->kernel_sp);

		set_current_task(NULL);
	}
}

static void task_sched(void)
{
	struct task *curr = current_task();
	struct cpu *cpu = this_cpu();

	ASSERT(!intr_enabled());
	ASSERT(spin_is_locked_by_me(&curr->lock));
	ASSERT(cpu->preempt_count == 0);

	set_current_task(NULL);

	switch (curr->state) {
	case TASK_STATE_RUNNING:
		task_join(curr);
		break;
	case TASK_STATE_ZOMBIE:
		break;
	case TASK_STATE_SLEEPING:
		spin_lock(&sleep_queue_lock);
		list_add_tail(&curr->queue, &sleep_queue);
		spin_unlock(&sleep_queue_lock);
		break;
	default:
		panic("unexpected state: pid=%ld, state=%d\n", curr->pid,
		      curr->state);
	}

	struct task *next = task_pick_next();
	if (next == curr) {
		curr->time_slice = TIME_SLICE_MAX;
		set_current_task(curr);
		return;
	}

	task_add_system_time(curr, jiffies_get() - curr->ktime);
	cpu->handoff = curr;
	if (next) {
		raw_spin_lock(&next->lock);
		set_current_task(next);
		switch_pgtable(next->mm->pgd);
		next->time_slice = TIME_SLICE_MAX;
		next->ktime = jiffies_get();
		__switch_to(&curr->kernel_sp, &next->kernel_sp);
	} else {
		set_current_task(NULL);
		__switch_to(&curr->kernel_sp, &cpu->kernel_sp);
	}

	task_sched_resume();
}

void task_yield(void)
{
	irqstate_t flags = local_irq_save();
	struct task *task = current_task();

	ASSERT(preempt_count() == 0);

	raw_spin_lock(&task->lock);
	task_sched();
	raw_spin_unlock(&task->lock);

	local_irq_restore(flags);
}

/*
 * Callers must hold @lock and run with interrupts disabled. The interrupt
 * state stays disabled across the switch and is restored by the caller's
 * irqrestore once it drops @lock.
 */
void task_sleep(void *chan, spinlock_t *lock)
{
	struct task *task = current_task();

	ASSERT(!intr_enabled());
	ASSERT(spin_is_locked_by_me(lock));
	ASSERT(preempt_count() == 0);

	raw_spin_lock(&task->lock);
	raw_spin_unlock(lock);
	task->state = TASK_STATE_SLEEPING;
	task->chan = chan;
	task_sched();
	task->chan = NULL;
	raw_spin_unlock(&task->lock);
	raw_spin_lock(lock);
}

void task_wake_all(void *chan)
{
	while (1) {
		struct task *p, *wakee = NULL;
		irqstate_t flags;

		flags = spin_lock_irqsave(&sleep_queue_lock);
		list_for_each_entry(p, &sleep_queue, queue) {
			irqstate_t pflags = spin_lock_irqsave(&p->lock);

			if (p->state == TASK_STATE_SLEEPING &&
			    p->chan == chan) {
				p->state = TASK_STATE_RUNNING;
				p->chan = NULL;
				list_del_init(&p->queue);
				spin_unlock_irqrestore(&p->lock, pflags);
				wakee = p;
				break;
			}
			spin_unlock_irqrestore(&p->lock, pflags);
		}
		spin_unlock_irqrestore(&sleep_queue_lock, flags);
		if (!wakee)
			break;
		task_join(wakee);
	}
}

void task_wake_spec(struct task *task)
{
	irqstate_t flags = spin_lock_irqsave(&sleep_queue_lock);
	irqstate_t tflags = spin_lock_irqsave(&task->lock);

	if (task->state == TASK_STATE_SLEEPING) {
		task->state = TASK_STATE_RUNNING;
		task->chan = NULL;
		list_del_init(&task->queue);
		spin_unlock_irqrestore(&task->lock, tflags);
		spin_unlock_irqrestore(&sleep_queue_lock, flags);
		task_join(task);
		return;
	}
	spin_unlock_irqrestore(&task->lock, tflags);
	spin_unlock_irqrestore(&sleep_queue_lock, flags);
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
	struct task *child;
	struct task *task = current_task();
	irqstate_t flags;

	if (task == initial_task)
		panic("%s(): init exit\n", __func__);

	fdtable_close_all(task->fdtable);
	fsinfo_free_resources(task->fsinfo);

	flags = spin_lock_irqsave(&wait_lock);
	list_for_each_entry(child, &task->children, child) {
		child->parent = initial_task;
	}
	list_splice(&task->children, &initial_task->children);
	task_wake_spec(task->parent);
	spin_unlock_irqrestore(&wait_lock, flags);

	ASSERT(preempt_count() == 0);

	/* task_sched() never returns for a zombie, so no irqrestore here. */
	local_irq_disable();
	raw_spin_lock(&task->lock);

	task->state = TASK_STATE_ZOMBIE;
	task->exit_status = status;

	task_sched();

	panic("scheduling zombie\n");
}

pid_t task_wait(pid_t pid, int *status, int options, struct rusage *rus)
{
	struct task *child;
	struct task *parent = current_task();
	irqstate_t flags;
	bool found;

	(void)options;
	(void)rus;

	flags = spin_lock_irqsave(&wait_lock);

	if (list_empty(&parent->children)) {
		spin_unlock_irqrestore(&wait_lock, flags);
		return -ECHILD;
	}

again:
	found = false;

	list_for_each_entry(child, &parent->children, child) {
		irqstate_t cflags = spin_lock_irqsave(&child->lock);

		if (pid > 0 && child->tgid == pid) {
			found = true;
		}

		if (child->state != TASK_STATE_ZOMBIE ||
		    (pid > 0 && child->tgid != pid)) {
			spin_unlock_irqrestore(&child->lock, cflags);
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
		spin_unlock_irqrestore(&child->lock, cflags);
		list_del_init(&child->child);
		spin_unlock_irqrestore(&wait_lock, flags);
		task_destroy(child);
		return pid;
	}

	if (pid > 0 && !found) {
		spin_unlock_irqrestore(&wait_lock, flags);
		return -ECHILD;
	}

	task_sleep(parent, &wait_lock);

	goto again;
}

void task_sched_resume(void)
{
	struct cpu *cpu = this_cpu();
	if (cpu->handoff) {
		raw_spin_unlock(&cpu->handoff->lock);
		cpu->handoff = NULL;
	}
}
