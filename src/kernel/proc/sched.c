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

static LIST_DEFINE(sleep_queue);
static SPINLOCK_DEFINE(sleep_queue_lock);

void task_join(struct task *task)
{
	struct cpu *cpu;
	irqstate_t flags;

	if (task->cpu < 0 || task->cpu >= MAX_CPUS)
		panic("task_join: pid=%ld invalid cpu %d\n", task->pid,
		      task->cpu);

	cpu = &cpus[task->cpu];
	flags = spin_lock_irqsave(&cpu->run_queue_lock);

	ASSERT(list_empty(&task->queue));
	ASSERT(task->state == TASK_STATE_RUNNING);
	list_add_tail(&task->queue, &cpu->run_queue);
	spin_unlock_irqrestore(&cpu->run_queue_lock, flags);
}

/*
 * The caller runs with interrupts disabled and can therefore use the
 * non-irqsave form of the rq lock.
 */
static struct task *task_pick_next(struct cpu *cpu)
{
	struct task *task = NULL;

	spin_lock(&cpu->run_queue_lock);
	if (!list_empty(&cpu->run_queue)) {
		task = list_first_entry(&cpu->run_queue, struct task, queue);
		ASSERT(task->cpu == (int)cpu->id);
		list_del_init(&task->queue);
	}
	spin_unlock(&cpu->run_queue_lock);
	return task;
}

/*
 * Bootstrap the CPU into its idle task. From this point on the scheduler is
 * entered from task context only; cpu->kernel_sp is just the stack this
 * startup path is abandoned on.
 */
void task_scheduler(void)
{
	struct cpu *cpu = this_cpu();
	struct task *idle = cpu->idle;

	if (!idle) {
		idle = task_create_idle(cpu->id);
		if (!idle)
			panic("failed to create idle task for cpu %lu\n",
			      cpu->id);
		cpu->idle = idle;
	}

	cpu->handoff = NULL;
	local_irq_disable();

	ASSERT(idle->oncpu == -1);
	idle->oncpu = cpu->id;
	idle->frame->kernel_tp = (unsigned long)cpu;
	set_current_task(idle);

	load_final_page_table();
	user_access_disable();

	idle->time_slice = TIME_SLICE_MAX;
	idle->ktime = jiffies_get();
	__switch_to(&cpu->kernel_sp, &idle->kernel_sp);

	panic("scheduler returned to bootstrap context\n");
}

static void task_sched(void)
{
	struct task *curr = current_task();
	struct cpu *cpu = this_cpu();

	ASSERT(!intr_enabled());
	ASSERT(curr->lock.locked);
	ASSERT(cpu->preempt_count == 0);
	ASSERT(curr->oncpu == (int)cpu->id);

	set_current_task(NULL);

	switch (curr->state) {
	case TASK_STATE_RUNNING:
		if (curr != cpu->idle)
			task_join(curr);
		break;
	case TASK_STATE_ZOMBIE:
		break;
	case TASK_STATE_SLEEPING:
		/* task_sleep() already linked us into the sleep queue. */
		break;
	default:
		panic("unexpected state: pid=%ld, state=%d\n", curr->pid,
		      curr->state);
	}

	struct task *next = task_pick_next(cpu);
	if (!next)
		next = cpu->idle;
	if (!next)
		panic("cpu %lu has no runnable task and no idle task\n",
		      cpu->id);

	if (next == curr) {
		curr->time_slice = TIME_SLICE_MAX;
		set_current_task(curr);
		return;
	}

	task_add_system_time(curr, jiffies_get() - curr->ktime);
	cpu->handoff = curr;

	raw_spin_lock(&next->lock);
	ASSERT(next->oncpu == -1);
	next->oncpu = cpu->id;
	set_current_task(next);
	next->frame->kernel_tp = (unsigned long)cpu;
	if (next->mm) {
		switch_pgtable(next->mm->pgd);
		user_access_enable();
	} else {
		load_final_page_table();
		user_access_disable();
	}
	next->time_slice = TIME_SLICE_MAX;
	next->ktime = jiffies_get();
	curr->oncpu = -1;
	__switch_to(&curr->kernel_sp, &next->kernel_sp);

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
	ASSERT(lock->locked);
	ASSERT(preempt_count() == 0);

	raw_spin_lock(&task->lock);
	task->state = TASK_STATE_SLEEPING;
	task->chan = chan;

	/*
	 * Publish the task on the sleep queue while the caller's condition
	 * lock is still held. A wakeup that races with us must take that
	 * condition lock, so it is guaranteed to observe the queue entry.
	 */
	irqstate_t sflags = spin_lock_irqsave(&sleep_queue_lock);

	list_add_tail(&task->queue, &sleep_queue);
	spin_unlock_irqrestore(&sleep_queue_lock, sflags);

	raw_spin_unlock(lock);
	task_sched();
	task->chan = NULL;
	raw_spin_unlock(&task->lock);
	raw_spin_lock(lock);
}

void task_wake_all(void *chan)
{
	while (1) {
		struct task *p, *wakee = NULL;
		bool retry = false;
		irqstate_t flags;

		/*
		 * task_sched() takes task->lock before sleep_queue_lock when
		 * a task goes to sleep, so never block on task->lock while
		 * holding sleep_queue_lock. Try-lock instead; if the sleeper
		 * still owns it, drop sleep_queue_lock and retry.
		 */
		flags = spin_lock_irqsave(&sleep_queue_lock);
		list_for_each_entry(p, &sleep_queue, queue) {
			irqstate_t pflags;

			if (p->state != TASK_STATE_SLEEPING ||
			    p->chan != chan)
				continue;

			if (!spin_trylock_irqsave(&p->lock, &pflags)) {
				retry = true;
				continue;
			}

			p->state = TASK_STATE_RUNNING;
			p->chan = NULL;
			list_del_init(&p->queue);
			spin_unlock_irqrestore(&p->lock, pflags);
			wakee = p;
			break;
		}
		spin_unlock_irqrestore(&sleep_queue_lock, flags);

		if (wakee)
			task_join(wakee);
		else if (!retry)
			break;
	}
}

void task_wake_spec(struct task *task)
{
	while (1) {
		struct task *p = NULL;
		bool found = false;
		irqstate_t flags;
		irqstate_t tflags;

		flags = spin_lock_irqsave(&sleep_queue_lock);
		list_for_each_entry(p, &sleep_queue, queue) {
			if (p == task) {
				found = true;
				break;
			}
		}

		if (!found || p->state != TASK_STATE_SLEEPING) {
			spin_unlock_irqrestore(&sleep_queue_lock, flags);
			return;
		}

		if (!spin_trylock_irqsave(&p->lock, &tflags)) {
			spin_unlock_irqrestore(&sleep_queue_lock, flags);
			continue;
		}

		p->state = TASK_STATE_RUNNING;
		p->chan = NULL;
		list_del_init(&p->queue);
		spin_unlock_irqrestore(&p->lock, tflags);
		spin_unlock_irqrestore(&sleep_queue_lock, flags);
		task_join(p);
		return;
	}
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
