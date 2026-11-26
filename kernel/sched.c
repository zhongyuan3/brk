#include <arch/mm.h>
#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/fsinfo.h>
#include <brk/futex.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/process.h>
#include <brk/refcnt.h>
#include <brk/signal.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/thread.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/capability.h>
#include <uapi/errno.h>
#include <uapi/futex.h>

SPINLOCK_DEFINE(wait_lock);
SPINLOCK_DEFINE(sleep_lock);

static LIST_DEFINE(sleep_queue);
static SPINLOCK_DEFINE(sleep_queue_lock);

static unsigned int next_cpu_hint;

static unsigned int cpu_load(struct cpu *cpu)
{
	irqstate_t flags = spin_lock_irqsave(&cpu->run_queue_lock);
	unsigned int load = cpu->nr_running;
	struct thread *cur = cpu->current;
	spin_unlock_irqrestore(&cpu->run_queue_lock, flags);

	/* nr_running counts queued threads; also account for the running one. */
	if (cur && cur != cpu->idle)
		load++;
	return load;
}

/*
 * Pick a CPU for a thread that is not currently running.  Only CPUs that have
 * entered their idle task (online) are eligible, so a newly started CPU
 * cannot receive an IPI before its scheduler context exists.
 *
 * The rotating hint makes equal-load choices round-robin instead of always
 * favouring CPU 0.
 */
static int thread_select_cpu(void)
{
	int best = -1;
	unsigned int best_load = ~0U;
	unsigned int start =
		__atomic_fetch_add(&next_cpu_hint, 1, __ATOMIC_RELAXED);

	for (int off = 0; off < MAX_CPUS; ++off) {
		int i = (int)((start + (unsigned int)off) % MAX_CPUS);
		unsigned int load;

		if (!__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE))
			continue;

		load = cpu_load(&cpus[i]);
		if (load < best_load) {
			best = i;
			best_load = load;
			if (load == 0)
				break;
		}
	}

	return best;
}

void sched_enqueue(struct thread *thread)
{
	struct cpu *cpu;
	irqstate_t flags;
	int target;

	/*
	 * The currently running thread must stay on this CPU until
	 * context_switch() has saved its context.  Everything else (new threads
	 * and threads woken from sleep) has a complete saved context and may be
	 * placed on any online CPU.
	 */
	if (thread->oncpu < 0) {
		target = thread_select_cpu();
		if (target < 0)
			target = thread->cpu;
	} else {
		kassert(thread->cpu == (int)this_cpuid());
		target = thread->cpu;
	}

	if (target < 0 || target >= MAX_CPUS)
		panic("sched_enqueue: tid=%d invalid cpu %d\n", thread->tid,
		      target);

	cpu = &cpus[target];
	flags = spin_lock_irqsave(&cpu->run_queue_lock);

	kassert(list_empty(&thread->queue));
	kassert(thread->state == THREAD_STATE_RUNNING);
	thread->cpu = target;
	list_add_tail(&thread->queue, &cpu->run_queue);
	cpu->nr_running++;
	spin_unlock_irqrestore(&cpu->run_queue_lock, flags);

	/*
	 * A remote CPU may be in WFI with interrupts enabled.  The timer tick
	 * would eventually notice the new queue entry, but an IPI keeps
	 * wakeup latency bounded and is required for prompt migration.
	 */
	if (target != (int)this_cpuid())
		send_ipi((cpuid_t)target);
}

/*
 * The caller runs with interrupts disabled and can therefore use the
 * non-irqsave form of the rq lock.
 */
static struct thread *thread_pick_next(struct cpu *cpu)
{
	struct thread *thread = NULL;

	spin_lock(&cpu->run_queue_lock);
	if (!list_empty(&cpu->run_queue)) {
		thread =
			list_first_entry(&cpu->run_queue, struct thread, queue);
		kassert(thread->cpu == (int)cpu->id);
		list_del_init(&thread->queue);
		kassert(cpu->nr_running > 0);
		cpu->nr_running--;
	}
	spin_unlock(&cpu->run_queue_lock);
	return thread;
}

/*
 * Static bootstrap process/thread for every CPU.  CPU0 runs the filesystem
 * setup in this thread and then keeps it as the orphan reaper; secondary CPUs
 * park their boot thread and hand the CPU over to a real idle thread.
 */
struct init_unit {
	struct process process;
	struct thread thread;
};

static struct init_unit init_tasks[MAX_CPUS];

static_assert(KSTACK_SIZE == (PAGE_SIZE << INIT_STACK_PAGE_ORDER),
	      "boot stack and kernel stack sizes differ");

static void init_unit_setup(struct init_unit *unit, cpuid_t cpuid)
{
	struct process *process = &unit->process;
	struct thread *thread = &unit->thread;

	if (thread->kstack_base)
		return;

	list_init(&process->list);
	list_init(&process->children);
	list_init(&process->sibling);
	list_init(&process->threads);
	spinlock_init(&process->lock, "init_process");
	spinlock_init(&process->cred_lock, "init_cred");
	fdtable_init(&process->fdtable);
	fsinfo_init(&process->fsinfo);
	spinlock_init(&process->rsrc_usage.lock, "init_resource_usage");
	process->pid = 0;
	process->pgid = 0;
	process->sid = 0;
	process->nr_threads = 1;
	process->nr_live = 1;
	process->leader = thread;
	process->flags = PROCESS_FLAG_STATIC | PROCESS_FLAG_KTHREAD;
	strlcpy(process->name, "init_task", sizeof(process->name));
	process->cred.umask = 022;
	process->cred.cap_effective = CAP_FULL_SET;
	process->cred.cap_permitted = CAP_FULL_SET;

	list_init(&thread->list);
	list_init(&thread->proc_threads);
	list_init(&thread->queue);
	spinlock_init(&thread->lock, "init_thread");
	thread->process = process;
	thread->tid = 0;
	thread->state = THREAD_STATE_RUNNING;
	thread->oncpu = -1;
	thread->cpu = (int)cpuid;
	thread->is_init_thread = true;
	list_add_tail(&thread->proc_threads, &process->threads);

	thread->kstack_top = arch_boot_stack_top(cpuid);
	thread->kstack_base = thread->kstack_top - KSTACK_SIZE;
	thread->frame =
		(struct trap_frame *)(thread->kstack_top - arch_frame_size());
	arch_frame_init(thread->frame);
	arch_frame_set_kernel_tp(thread->frame, (unsigned long)&cpus[cpuid]);

	signal_init(process, thread, 0);
}

/*
 * Bootstrap the CPU into its boot thread.  The current stack is the boot
 * stack and becomes init_thread's kernel stack.  A separate idle thread is
 * allocated so init_thread may sleep during filesystem setup and later while
 * reaping.
 */
void sched_start(void)
{
	struct cpu *cpu = this_cpu();
	struct init_unit *unit = &init_tasks[cpu->id];
	struct thread *init = &unit->thread;
	struct process *idle_process;

	local_irq_disable();

	init_unit_setup(unit, cpu->id);

	idle_process = process_create_idle(cpu->id);
	if (!idle_process)
		panic("failed to create idle thread for cpu %lu\n", cpu->id);

	cpu->idle = idle_process->leader;
	cpu->handoff = NULL;

	kassert(init->oncpu == -1);
	init->oncpu = cpu->id;
	set_current_thread(init);

	load_final_page_table();
	user_access_disable();

	init->time_slice = TIME_SLICE_MAX;
	init->last_user_ns = ktime_get_mono_ns();
	init->last_kernel_ns = init->last_user_ns;

	/*
	 * Publish this CPU as online only after current is the boot thread.  An
	 * IPI may then queue work immediately, but it stays pending until the
	 * thread enables interrupts or switches to idle.
	 */
	__atomic_store_n(&cpu->online, true, __ATOMIC_RELEASE);

	if (cpu->id == boot_info.boot_hart_id) {
		local_irq_enable();
		process_init_main();
		panic("init process returned\n");
	}

	/*
	 * Secondary CPUs have no boot work.  Park the static boot thread and
	 * hand over to the real idle thread through the normal scheduler path.
	 */
	init->state = THREAD_STATE_ZOMBIE;
	unit->process.nr_live = 0;
	unit->process.exited = true;
	unit->process.last_thread = init;
	thread_yield();
	panic("secondary init thread resumed\n");
}

static void sched_switch(void)
{
	struct thread *curr = current_thread();
	struct cpu *cpu = this_cpu();
	u64 now;

	kassert(!local_irq_enabled());
	kassert(curr->lock.locked);
	kassert(cpu->preempt_count == 0);
	kassert(curr->oncpu == (int)cpu->id);

	set_current_thread(NULL);

	switch (curr->state) {
	case THREAD_STATE_RUNNING:
		if (curr != cpu->idle)
			sched_enqueue(curr);
		break;
	case THREAD_STATE_ZOMBIE:
		break;
	case THREAD_STATE_SLEEPING:
		/* thread_sleep() already linked us into the sleep queue. */
		break;
	default:
		panic("unexpected state: tid=%d, state=%d\n", curr->tid,
		      curr->state);
	}

	struct thread *next = thread_pick_next(cpu);
	if (!next)
		next = cpu->idle;
	if (!next)
		panic("cpu %lu has no runnable thread and no idle thread\n",
		      cpu->id);

	if (next == curr) {
		curr->time_slice = TIME_SLICE_MAX;
		set_current_thread(curr);
		return;
	}

	now = ktime_get_mono_ns();
	if (now > curr->last_kernel_ns)
		thread_account_system(curr, now - curr->last_kernel_ns);
	curr->last_kernel_ns = now;
	curr->last_user_ns = now;
	cpu->handoff = curr;

	raw_spin_lock(&next->lock);
	kassert(next->oncpu == -1);
	next->oncpu = cpu->id;
	set_current_thread(next);
	arch_frame_set_kernel_tp(next->frame, (unsigned long)cpu);
	if (next->process->mm) {
		switch_pgtable(next->process->mm->pgd);
		user_access_enable();
	} else {
		load_final_page_table();
		user_access_disable();
	}
	next->time_slice = TIME_SLICE_MAX;
	next->last_kernel_ns = now;
	next->last_user_ns = now;
	/*
	 * curr->oncpu is cleared by sched_resume() in the next thread's
	 * context, i.e. only after this CPU has completely left curr.  This
	 * lets a parent on another CPU wait until a zombie is safe to reap.
	 */
	context_switch(curr, next);

	sched_resume();
}

void thread_yield(void)
{
	irqstate_t flags = local_irq_save();
	struct thread *thread = current_thread();

	kassert(preempt_count() == 0);

	raw_spin_lock(&thread->lock);
	sched_switch();
	raw_spin_unlock(&thread->lock);

	local_irq_restore(flags);
}

void sched_tick(void)
{
	struct thread *thread = current_thread();

	if (!thread)
		return;

	if (--thread->time_slice <= 0 && preempt_count() == 0)
		thread_yield();
}

void trap_return_to_user(void)
{
	struct thread *thread = current_thread();
	u64 now;

	if (!thread)
		return;

	now = ktime_get_mono_ns();
	if (now > thread->last_kernel_ns)
		thread_account_system(thread, now - thread->last_kernel_ns);
	thread->last_user_ns = now;
}

int thread_sleep_ns(ktime_t dur, ktime_t *rem)
{
	struct thread *task = current_thread();
	s64 start_ns;
	s64 deadline_ns;
	s64 now_ns;
	irqstate_t flags;

	if (dur <= 0) {
		if (rem)
			*rem = 0;
		return 0;
	}

	start_ns = (s64)ktime_get_mono_ns();
	deadline_ns = start_ns + dur;

	thread_disarm_sleep_timer();
	thread_arm_sleep_timer(&sleep_lock, (u64)deadline_ns);

	flags = spin_lock_irqsave(&sleep_lock);

	for (;;) {
		if (thread_abort_pending(task)) {
			s64 left_ns;

			spin_unlock_irqrestore(&sleep_lock, flags);
			thread_disarm_sleep_timer();
			now_ns = (s64)ktime_get_mono_ns();
			left_ns = deadline_ns - now_ns;
			if (left_ns < 0)
				left_ns = 0;
			if (rem)
				*rem = (ktime_t)left_ns;
			return -EINTR;
		}
		now_ns = (s64)ktime_get_mono_ns();
		if (now_ns >= deadline_ns)
			break;
		(void)thread_sleep_interruptible(task, &sleep_lock);
	}
	spin_unlock_irqrestore(&sleep_lock, flags);

	thread_disarm_sleep_timer();

	if (rem)
		*rem = 0;
	return 0;
}

/*
 * A pending fatal condition only aborts sleeps that opted in to being
 * interruptible.  Uninterruptible sleeps back kernel operations that must run
 * to completion (block I/O, page cache and inode serialization, filesystem
 * teardown); interrupting them mid-flight leaves persistent state (for example
 * an inode stuck in I_FREEING) because there is no way to unwind safely.
 *
 * The exit path of the current thread is excluded as well: once thread_exit()
 * starts dropping files and evicting inodes, it must be allowed to block on the
 * resulting writeback even though group_exiting is already set.
 */
bool thread_abort_pending(struct thread *thread)
{
	return !thread->in_exit && (signal_pending(thread) ||
				    process_group_exiting(thread->process));
}

/*
 * Return true if the thread actually slept, false if an interruptible sleep
 * was aborted by thread_abort_pending() before scheduling.
 */
static bool thread_sleep_common(void *chan, spinlock_t *lock,
				bool interruptible)
{
	struct thread *thread = current_thread();

	kassert(!local_irq_enabled());
	kassert(lock->locked);
	kassert(preempt_count() == 0);

	if (interruptible && thread_abort_pending(thread))
		return false;

	raw_spin_lock(&thread->lock);
	thread->state = THREAD_STATE_SLEEPING;
	thread->chan = chan;

	/*
	 * Publish the thread on the sleep queue while the caller's condition
	 * lock is still held. A wakeup that races with us must take that
	 * condition lock, so it is guaranteed to observe the queue entry.
	 */
	irqstate_t sflags = spin_lock_irqsave(&sleep_queue_lock);

	list_add_tail(&thread->queue, &sleep_queue);
	spin_unlock_irqrestore(&sleep_queue_lock, sflags);

	/*
	 * A signal or group exit that arrives between the caller's condition
	 * check and the queue publication above could not find us through
	 * thread_wake().  For interruptible callers, recheck after publication
	 * and abort before scheduling.  Such callers must return to user space
	 * promptly so the condition can be acted on.
	 */
	if (interruptible && thread_abort_pending(thread)) {
		sflags = spin_lock_irqsave(&sleep_queue_lock);
		thread->state = THREAD_STATE_RUNNING;
		thread->chan = NULL;
		list_del_init(&thread->queue);
		spin_unlock_irqrestore(&sleep_queue_lock, sflags);

		raw_spin_unlock(&thread->lock);
		raw_spin_unlock(lock);
		raw_spin_lock(lock);
		return false;
	}

	raw_spin_unlock(lock);
	sched_switch();
	thread->chan = NULL;
	raw_spin_unlock(&thread->lock);
	raw_spin_lock(lock);
	return true;
}

/*
 * Callers must hold @lock and run with interrupts disabled. The interrupt
 * state stays disabled across the switch and is restored by the caller's
 * irqrestore once it drops @lock.
 *
 * Uninterruptible sleeps always wait for their condition.  Use this for
 * kernel-internal operations that cannot be unwound safely.
 */
void thread_sleep(void *chan, spinlock_t *lock)
{
	(void)thread_sleep_common(chan, lock, false);
}

/*
 * Like thread_sleep(), but abort the sleep when a signal is pending or the
 * process group is exiting.  Returns false when the sleep was aborted; the
 * caller must unwind and return -EINTR (or a partial result) instead of
 * re-entering the sleep.
 */
bool thread_sleep_interruptible(void *chan, spinlock_t *lock)
{
	return thread_sleep_common(chan, lock, true);
}

void thread_wake_all(void *chan)
{
	while (1) {
		struct thread *p, *wakee = NULL;
		bool retry = false;
		irqstate_t flags;

		/*
		 * sched_switch() takes thread->lock before sleep_queue_lock when
		 * a thread goes to sleep, so never block on thread->lock while
		 * holding sleep_queue_lock. Try-lock instead; if the sleeper
		 * still owns it, drop sleep_queue_lock and retry.
		 */
		flags = spin_lock_irqsave(&sleep_queue_lock);
		list_for_each_entry(p, &sleep_queue, queue) {
			irqstate_t pflags;

			if (p->state != THREAD_STATE_SLEEPING ||
			    p->chan != chan)
				continue;

			if (!spin_trylock_irqsave(&p->lock, &pflags)) {
				retry = true;
				continue;
			}

			p->state = THREAD_STATE_RUNNING;
			p->chan = NULL;
			list_del_init(&p->queue);
			spin_unlock_irqrestore(&p->lock, pflags);
			wakee = p;
			break;
		}
		spin_unlock_irqrestore(&sleep_queue_lock, flags);

		if (wakee)
			sched_enqueue(wakee);
		else if (!retry)
			break;
	}
}

void thread_wake(struct thread *thread)
{
	while (1) {
		struct thread *p = NULL;
		bool found = false;
		irqstate_t flags;
		irqstate_t tflags;

		flags = spin_lock_irqsave(&sleep_queue_lock);
		list_for_each_entry(p, &sleep_queue, queue) {
			if (p == thread) {
				found = true;
				break;
			}
		}

		if (!found || p->state != THREAD_STATE_SLEEPING) {
			spin_unlock_irqrestore(&sleep_queue_lock, flags);
			return;
		}

		if (!spin_trylock_irqsave(&p->lock, &tflags)) {
			spin_unlock_irqrestore(&sleep_queue_lock, flags);
			continue;
		}

		p->state = THREAD_STATE_RUNNING;
		p->chan = NULL;
		list_del_init(&p->queue);
		spin_unlock_irqrestore(&p->lock, tflags);
		spin_unlock_irqrestore(&sleep_queue_lock, flags);
		sched_enqueue(p);
		return;
	}
}

void thread_exit_normal(int code)
{
	thread_exit((code & 0xff) << 8);
}

void thread_exit_signal(int sig)
{
	thread_exit(sig & 0x7f);
}

void thread_exit(int status)
{
	struct process *process = current_process();
	struct process *child;
	struct thread *thread = current_thread();
	irqstate_t flags;
	bool last;
	bool reparented = false;

	/*
	 * The teardown below can block on filesystem I/O.  Keep it out of
	 * thread_abort_pending() so neither a fatal signal nor a group exit
	 * can abort those waits halfway through.
	 */
	thread->in_exit = true;

	futex_cleanup_thread(thread);

	/*
	 * pthread_join-style wakeup: clear the user TID slot before this thread
	 * leaves the CPU, then wake futex waiters blocked on that address.
	 */
	if (thread->clear_tid) {
		int zero = 0;

		if (copy_to_user(thread->clear_tid, &zero, sizeof(zero)) == 0 &&
		    process->mm)
			(void)futex_wake_user(process->mm,
					      (u32 *)thread->clear_tid,
					      FUTEX_BITSET_MATCH_ANY,
					      0x7fffffff);
		thread->clear_tid = NULL;
	}

	/*
	 * Only the process-wide exit path releases filesystem state.  With a
	 * single thread per process this is always the last thread; the check
	 * keeps the code ready for real thread groups.
	 */
	flags = spin_lock_irqsave(&process->lock);
	process->nr_live--;
	last = process->nr_live == 0;
	if (last) {
		process->exited = true;
		process->exit_status = process->group_exiting ?
					       process->group_exit_status :
					       status;
		process->last_thread = thread;
	}
	spin_unlock_irqrestore(&process->lock, flags);

	if (last) {
		/*
		 * Drop the shared file references now so pipes and descriptors
		 * are released even if the parent never reaps us.
		 */
		fdtable_deinit(&process->fdtable);
		fsinfo_deinit(&process->fsinfo);

		flags = spin_lock_irqsave(&wait_lock);
		if (!list_empty(&process->children)) {
			struct process *reaper = init_reaper;

			if (!reaper)
				panic("thread_exit: no init reaper\n");

			list_for_each_entry(child, &process->children, sibling)
				child->parent = reaper;
			list_splice(&process->children, &reaper->children);
			reparented = true;
		}
		spin_unlock_irqrestore(&wait_lock, flags);

		if (reparented)
			thread_wake_all(init_reaper);
	}

	kassert(preempt_count() == 0);

	/* sched_switch() never returns for a zombie, so no irqrestore here. */
	local_irq_disable();
	raw_spin_lock(&thread->lock);

	thread->state = THREAD_STATE_ZOMBIE;
	thread->exit_status = status;

	sched_switch();

	panic("scheduling zombie\n");
}

pid_t process_wait(pid_t pid, int *status, int options,
		   struct __brk_rusage *rus, kuid_t *uid)
{
	struct process *child;
	struct process *parent = current_process();
	irqstate_t flags;
	bool found;
	bool nohang = options & WNOHANG;
	bool nowait = options & WNOWAIT;
	pid_t retpid;

	(void)rus;

	flags = spin_lock_irqsave(&wait_lock);

	if (list_empty(&parent->children)) {
		spin_unlock_irqrestore(&wait_lock, flags);
		return -ECHILD;
	}

again:
	found = false;

	list_for_each_entry(child, &parent->children, sibling) {
		irqstate_t cflags = spin_lock_irqsave(&child->lock);
		bool ready;

		if (pid > 0 && child->pid == pid)
			found = true;

		ready = child->exited && child->reap_ready;
		if (!ready || (pid > 0 && child->pid != pid)) {
			spin_unlock_irqrestore(&child->lock, cflags);
			continue;
		}

		if (status)
			*status = child->exit_status;
		if (uid)
			*uid = process_get_uid(child);
		retpid = child->pid;
		spin_unlock_irqrestore(&child->lock, cflags);

		if (!nowait) {
			process_add_child_system_time(
				parent,
				process_get_system_time(child) +
					process_get_child_system_time(child));
			process_add_child_user_time(
				parent,
				process_get_user_time(child) +
					process_get_child_user_time(child));
		}

		if (nowait) {
			spin_unlock_irqrestore(&wait_lock, flags);
			return retpid;
		}

		list_del_init(&child->sibling);
		spin_unlock_irqrestore(&wait_lock, flags);
		process_destroy(child);
		return retpid;
	}

	if (pid > 0 && !found) {
		spin_unlock_irqrestore(&wait_lock, flags);
		return -ECHILD;
	}

	if (nohang) {
		spin_unlock_irqrestore(&wait_lock, flags);
		return 0;
	}

	if (!thread_sleep_interruptible(parent, &wait_lock)) {
		spin_unlock_irqrestore(&wait_lock, flags);
		return -EINTR;
	}

	goto again;
}

void sched_resume(void)
{
	struct cpu *cpu = this_cpu();
	struct thread *prev = cpu->handoff;
	struct process *wake_parent = NULL;

	if (!prev)
		return;

	/*
	 * The previous thread's context is now fully saved; this CPU no longer
	 * touches its kernel stack.  Publish oncpu == -1 before releasing its
	 * lock so a parent on another CPU can safely reap a zombie.
	 */
	bool zombie = prev->state == THREAD_STATE_ZOMBIE;
	if (zombie) {
		struct process *process = prev->process;
		irqstate_t flags = spin_lock_irqsave(&process->lock);

		if (process->exited && process->last_thread == prev) {
			process->reap_ready = true;
			wake_parent = process->parent;
		}
		spin_unlock_irqrestore(&process->lock, flags);
	}

	prev->oncpu = -1;
	raw_spin_unlock(&prev->lock);
	cpu->handoff = NULL;

	/*
	 * The parent was not woken before the switch because it could have
	 * run on another CPU while @prev was still on-CPU.  Now that the
	 * zombie is really off-CPU and safe to reap, wake its waiters.
	 */
	if (wake_parent)
		thread_wake_all(wake_parent);
}
