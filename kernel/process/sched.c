#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/limits.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/pgtable.h>
#include <brk/process.h>
#include <brk/riscv.h>
#include <brk/timer.h>
#include <brk/types.h>

SPINLOCK_DEFINE(wait_lock);

static LIST_DEFINE(run_queue);
static SPINLOCK_DEFINE(run_queue_lock);
static LIST_DEFINE(sleep_queue);
static SPINLOCK_DEFINE(sleep_queue_lock);

void proc_join(struct process *proc)
{
	spinlock_acquire(&run_queue_lock);
	list_add_tail(&proc->queue, &run_queue);
	spinlock_release(&run_queue_lock);
}

static struct process *proc_get_next(void)
{
	struct process *proc = NULL;
	spinlock_acquire(&run_queue_lock);
	if (!list_empty(&run_queue)) {
		proc = list_first_entry(&run_queue, struct process, queue);
		list_del_init(&proc->queue);
	}
	spinlock_release(&run_queue_lock);
	return proc;
}

void proc_scheduler(void)
{
	struct cpu *cpu = current_cpu();
	struct process *next = NULL;

	cpu->handoff = NULL;
	cpu->current = NULL;
	for (;;) {
		proc_sched_resume();

		intr_on();
		intr_off();

		next = proc_get_next();
		if (!next) {
			intr_on();
			asm volatile("wfi");
			continue;
		}

		spinlock_acquire(&next->lock);
		cpu->current = next;
		cpu->irq_enabled = next->irq_enabled;
		switch_pgtable(next->mm->pgd);
		write_sstatus(read_sstatus() | SSTATUS_SUM);
		next->time_slice = TIME_SLICE_MAX;
		next->ktime = jiffies_get();
		switch_context(&cpu->ctx, &next->ctx);

		cpu->current = NULL;
	}
}

void proc_sched(void)
{
	struct process *curr = current_process();
	struct cpu *cpu = current_cpu();

	assert(!intr_enabled());
	assert(spinlock_holding(&curr->lock));
	assert(current_cpu()->irq_nest == 1);

	curr->irq_enabled = cpu->irq_enabled;

	switch (curr->state) {
	case PROCESS_STATE_RUNNING:
		proc_join(curr);
		break;
	case PROCESS_STATE_ZOMBIE:
		break;
	case PROCESS_STATE_SLEEPING:
		spinlock_acquire(&sleep_queue_lock);
		list_add_tail(&curr->queue, &sleep_queue);
		spinlock_release(&sleep_queue_lock);
		break;
	default:
		panic("unexpected state: %d\n", curr->state);
	}

	struct process *next = proc_get_next();
	if (next == curr) {
		curr->time_slice = TIME_SLICE_MAX;
		cpu->current = curr;
		cpu->irq_enabled = curr->irq_enabled;
		return;
	}

	curr->ptms.tms_stime += jiffies_get() - curr->ktime;
	cpu->handoff = curr;
	if (next) {
		spinlock_acquire(&next->lock);
		cpu->current = next;
		cpu->irq_enabled = next->irq_enabled;
		switch_pgtable(next->mm->pgd);
		next->time_slice = TIME_SLICE_MAX;
		next->ktime = jiffies_get();
		switch_context(&curr->ctx, &next->ctx);
	} else {
		cpu->current = NULL;
		switch_context(&curr->ctx, &cpu->ctx);
	}

	proc_sched_resume();
}

void proc_yield(void)
{
	struct process *proc = current_process();
	spinlock_acquire(&proc->lock);
	proc_sched();
	spinlock_release(&proc->lock);
}

void proc_sleep(void *chan, spinlock_t *lock)
{
	struct process *proc = current_process();
	spinlock_acquire(&proc->lock);
	spinlock_release(lock);
	proc->state = PROCESS_STATE_SLEEPING;
	proc->chan = chan;
	proc_sched();
	proc->chan = NULL;
	spinlock_release(&proc->lock);
	spinlock_acquire(lock);
}

void proc_wake_up(void *chan)
{
	struct process *proc;

	spinlock_acquire(&sleep_queue_lock);
	list_for_each_entry(proc, &sleep_queue, queue) {
		spinlock_acquire(&proc->lock);
		if (proc->state == PROCESS_STATE_SLEEPING &&
		    proc->chan == chan) {
			proc->state = PROCESS_STATE_RUNNING;
			proc->chan = NULL;
			list_del_init(&proc->queue);
			spinlock_release(&proc->lock);
			spinlock_release(&sleep_queue_lock);
			proc_join(proc);
			return;
		}
		spinlock_release(&proc->lock);
	}
	spinlock_release(&sleep_queue_lock);
}

void proc_exit(int status)
{
	struct process *child;
	struct process *parent = current_process();

	if (parent == init_proc)
		panic("%s(): init exit\n", __func__);

	for (int i = 0; i < OPEN_MAX; ++i) {
		if (parent->ofiles[i]) {
			file_put(parent->ofiles[i]);
			parent->ofiles[i] = NULL;
		}
	}
	path_put(&parent->cwd);
	path_put(&parent->root);

	spinlock_acquire(&wait_lock);
	list_for_each_entry(child, &parent->children, child) {
		child->parent = init_proc;
	}
	list_splice(&parent->children, &init_proc->children);
	proc_wake_up(parent->parent);
	spinlock_release(&wait_lock);

	spinlock_acquire(&parent->lock);

	parent->state = PROCESS_STATE_ZOMBIE;
	parent->exit_status = status;

	proc_sched();

	panic("scheduling zombie\n");
}

pid_t proc_wait(pid_t pid, int *status, int options, struct rusage *rus)
{
	struct process *child;
	struct process *parent = current_process();
	bool found;

	spinlock_acquire(&wait_lock);

	if (list_empty(&parent->children)) {
		spinlock_release(&wait_lock);
		return -ECHILD;
	}

again:
	found = false;

	list_for_each_entry(child, &parent->children, child) {
		spinlock_acquire(&child->lock);
		if (pid > 0 && child->pid == pid) {
			found = true;
		}

		if (child->state != PROCESS_STATE_ZOMBIE ||
		    (pid > 0 && child->pid != pid)) {
			spinlock_release(&child->lock);
			continue;
		}

		if (status)
			*status = child->exit_status;
		pid = child->pid;
		parent->ptms.tms_cstime += child->ptms.tms_stime;
		parent->ptms.tms_cutime += child->ptms.tms_utime;
		spinlock_release(&child->lock);
		list_del_init(&child->child);
		spinlock_release(&wait_lock);
		proc_free(child);
		return pid;
	}

	if (pid > 0 && !found) {
		spinlock_release(&wait_lock);
		return -ECHILD;
	}

	proc_sleep(parent, &wait_lock);

	goto again;
}

void proc_sched_resume(void)
{
	struct cpu *cpu = current_cpu();
	if (cpu->handoff) {
		spinlock_release(&cpu->handoff->lock);
		cpu->handoff = NULL;
	}
}
