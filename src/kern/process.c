#include <arch/context.h>
#include <arch/csr.h>
#include <arch/pgtable.h>
#include <arch/trapframe.h>
#include <brk/asm/cpu.h>
#include <brk/assert.h>
#include <brk/console.h>
#include <brk/container_of.h>
#include <brk/cpu.h>
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/fsinfo.h>
#include <brk/genhd.h>
#include <brk/init.h>
#include <brk/klog.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/lengthof.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/netdevice.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/signal.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/thread.h>
#include <brk/tick.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <brk/vmalloc.h>
#include <brk/writeback.h>
#include <stddef.h>
#include <uapi/capability.h>
#include <uapi/errno.h>
#include <uapi/limits.h>
#include <uapi/signal.h>

struct process *init_reaper;

LIST_DEFINE(processes);
LIST_DEFINE(threads);
SPINLOCK_DEFINE(process_list_lock);
static struct slab_allocator process_cache;
static struct slab_allocator thread_cache;

static SPINLOCK_DEFINE(pid_lock);
static pid_t curr_pid = 1;

pid_t pid_alloc(void)
{
	pid_t pid;

	spin_lock(&pid_lock);
	pid = curr_pid++;
	spin_unlock(&pid_lock);
	return pid;
}

void pid_free(pid_t pid)
{
	(void)pid;
}

void process_cache_init(void)
{
	slab_init(&process_cache, sizeof(struct process),
		  alignof(struct process), "process_cache");
}

void thread_cache_init(void)
{
	slab_init(&thread_cache, sizeof(struct thread), alignof(struct thread),
		  "thread_cache");
}

static u64 kstack_alloc(void)
{
	struct page *pg = page_alloc(KSTACK_PAGE_ORDER);
	if (!pg)
		return 0;
	return page_to_virt(pg);
}

static void kstack_free(u64 stack)
{
	struct page *pg = virt_to_page((uintptr_t)stack);
	assert(pg);
	page_free(pg, KSTACK_PAGE_ORDER);
}

static struct process *process_alloc(void)
{
	return slab_alloc_zero(&process_cache);
}

static void process_free(struct process *process)
{
	slab_free(&process_cache, process);
}

static struct thread *thread_alloc(void)
{
	return slab_alloc_zero(&thread_cache);
}

static void thread_free(struct thread *thread)
{
	slab_free(&thread_cache, thread);
}

static void thread_sleep_timeout(struct timer_list *timer)
{
	struct thread *thread = container_of(timer, struct thread, sleep_timer);
	spinlock_t *lock =
		__atomic_load_n(&thread->sleep_lock, __ATOMIC_ACQUIRE);

	if (lock) {
		/*
		 * Taking the sleeper's condition lock orders this wakeup after
		 * thread_sleep() has published the thread on the sleep queue.
		 */
		irqstate_t flags = spin_lock_irqsave(lock);

		spin_unlock_irqrestore(lock, flags);
	}
	thread_wake(thread);
}

void thread_arm_sleep_timer(spinlock_t *lock, u64 expires_ns)
{
	struct thread *thread = current_thread();

	timer_setup(&thread->sleep_timer, thread_sleep_timeout);
	timer_set_expires(&thread->sleep_timer, expires_ns);
	__atomic_store_n(&thread->sleep_lock, lock, __ATOMIC_RELEASE);
	(void)timer_add(&thread->sleep_timer);
}

void thread_disarm_sleep_timer(void)
{
	struct thread *thread = current_thread();

	timer_del_sync(&thread->sleep_timer);
	__atomic_store_n(&thread->sleep_lock, NULL, __ATOMIC_RELEASE);
}

static void complete_thread_switch(void);
static void kthread_return(void);

static void default_new_thread_entry(void)
{
	complete_thread_switch();
	thread_exit_normal(0);
}

/*
 * Create a thread inside @process.  The main thread uses process->pid as its
 * tid; secondary threads get a fresh global pid.
 */
static struct thread *thread_create_in(struct process *process,
				       void (*fn)(void), bool main)
{
	struct thread *thread;
	unsigned long kernel_sp;
	struct context *ctx;

	thread = thread_alloc();
	if (!thread)
		return ERR_PTR(-ENOMEM);

	list_init(&thread->list);
	list_init(&thread->proc_threads);
	list_init(&thread->queue);
	spinlock_init(&thread->lock, "thread");
	thread->process = process;
	thread->state = THREAD_STATE_NEW;
	thread->oncpu = -1;
	thread->cpu = (int)this_cpuid();

	if (main) {
		thread->tid = process->pid;
	} else {
		thread->tid = pid_alloc();
		if (thread->tid < 0) {
			thread_free(thread);
			return ERR_PTR(-ENOMEM);
		}
	}

	thread->kstack_base = kstack_alloc();
	if (!thread->kstack_base) {
		thread_free(thread);
		return ERR_PTR(-ENOMEM);
	}
	thread->kstack_top = thread->kstack_base + KSTACK_SIZE;
	kernel_sp = thread->kstack_top;
	kernel_sp -= TRAP_FRAME_SIZE;
	assert(is_aligned(kernel_sp, 16));
	thread->frame = (struct trap_frame *)kernel_sp;
	memset(thread->frame, 0, TRAP_FRAME_SIZE);
	kernel_sp -= CONTEXT_SIZE;
	assert(is_aligned(kernel_sp, 16));
	ctx = (struct context *)kernel_sp;
	ctx->ra = (unsigned long)(fn ? fn : default_new_thread_entry);
	thread->kernel_sp = kernel_sp;

	irqstate_t flags = spin_lock_irqsave(&process->lock);
	if (!main && process->exited) {
		spin_unlock_irqrestore(&process->lock, flags);
		kstack_free(thread->kstack_base);
		thread_free(thread);
		return ERR_PTR(-EINVAL);
	}
	list_add_tail(&thread->proc_threads, &process->threads);
	process->nr_threads++;
	process->nr_live++;
	if (main)
		process->leader = thread;
	spin_unlock_irqrestore(&process->lock, flags);

	flags = spin_lock_irqsave(&process_list_lock);
	list_add_tail(&thread->list, &threads);
	spin_unlock_irqrestore(&process_list_lock, flags);

	return thread;
}

struct thread *thread_create(struct process *process, void (*fn)(void *),
			     void *data)
{
	struct thread *thread;
	bool dead;

	if (!process)
		return ERR_PTR(-EINVAL);

	irqstate_t flags = spin_lock_irqsave(&process->lock);
	dead = process->exited;
	spin_unlock_irqrestore(&process->lock, flags);
	if (dead)
		return ERR_PTR(-EINVAL);

	thread = thread_create_in(process, kthread_return, false);
	if (IS_ERR(thread))
		return thread;

	thread->kthrd_fn = fn;
	thread->kthrd_data = data;
	signal_thread_init(process, thread, 0);
	return thread;
}

static void process_unlink(struct process *process)
{
	irqstate_t flags = spin_lock_irqsave(&process_list_lock);

	list_del_init(&process->list);
	spin_unlock_irqrestore(&process_list_lock, flags);
}

static void process_link(struct process *process)
{
	irqstate_t flags = spin_lock_irqsave(&process_list_lock);

	list_add_tail(&process->list, &processes);
	spin_unlock_irqrestore(&process_list_lock, flags);
}

void process_destroy(struct process *process)
{
	struct thread *thread;

	if (process->flags & PROCESS_FLAG_STATIC)
		panic("attempt to destroy static process (pid=%d)\n",
		      process->pid);

	process_unlink(process);

	if (process->parent) {
		irqstate_t flags = spin_lock_irqsave(&wait_lock);

		list_del_init(&process->sibling);
		process->parent = NULL;
		spin_unlock_irqrestore(&wait_lock, flags);
	}

	/*
	 * The process is already unreachable: it has been removed from the
	 * global list and from its parent's children list.  Every thread must
	 * be off-CPU; normal reaping only gets here after the last thread has
	 * cleared oncpu in sched_resume(), while error paths destroy a thread
	 * that was never enqueued.
	 */
	while (!list_empty(&process->threads)) {
		thread = list_first_entry(&process->threads, struct thread,
					  proc_threads);
		assert(thread->oncpu == -1);

		list_del_init(&thread->proc_threads);

		timer_del_sync(&thread->sleep_timer);
		__atomic_store_n(&thread->sleep_lock, NULL, __ATOMIC_RELEASE);

		irqstate_t flags = spin_lock_irqsave(&process_list_lock);
		list_del_init(&thread->list);
		spin_unlock_irqrestore(&process_list_lock, flags);

		kstack_free(thread->kstack_base);
		thread_free(thread);
	}

	signal_deinit(process);
	if (process->fsinfo)
		fsinfo_put(process->fsinfo);
	if (process->fdtable)
		fdtable_put(process->fdtable);
	if (process->mm)
		uvm_space_put(process->mm);
	if (process->rsrc_usage)
		process_rusage_free(process->rsrc_usage);
	pid_free(process->pid);
	process_free(process);
}

static void thread_destroy(struct thread *thread)
{
	struct process *process = thread->process;

	timer_del_sync(&thread->sleep_timer);
	__atomic_store_n(&thread->sleep_lock, NULL, __ATOMIC_RELEASE);

	irqstate_t flags = spin_lock_irqsave(&process_list_lock);
	list_del_init(&thread->list);
	spin_unlock_irqrestore(&process_list_lock, flags);

	flags = spin_lock_irqsave(&process->lock);
	list_del_init(&thread->proc_threads);
	process->nr_threads--;
	spin_unlock_irqrestore(&process->lock, flags);

	kstack_free(thread->kstack_base);
	thread_free(thread);
}

struct process *process_create(struct process_create_args *args)
{
	struct process *process;
	struct thread *thread;
	int err = 0;

	process = process_alloc();
	if (!process)
		return ERR_PTR(-ENOMEM);

	list_init(&process->list);
	list_init(&process->children);
	list_init(&process->sibling);
	list_init(&process->threads);
	spinlock_init(&process->lock, "process");
	process->pid = pid_alloc();
	if (process->pid < 0) {
		err = -ENOMEM;
		goto pid_failed;
	}
	process->pgid = (args && args->pgid) ? args->pgid : process->pid;
	process->sid = (args && args->sid) ? args->sid : process->pid;
	process->parent = args ? args->parent : NULL;
	process->flags = args ? args->flags : 0;
	process->cred.umask = 022;
	process->cred.cap_effective = CAP_FULL_SET;
	process->cred.cap_permitted = CAP_FULL_SET;
	process->cred.cap_inheritable = 0;
	if (args && args->name)
		strlcpy(process->name, args->name, sizeof(process->name));

	if (args && args->mm) {
		process->mm = uvm_space_get(args->mm);
	} else {
		process->mm = uvm_space_create();
		if (IS_ERR(process->mm)) {
			err = PTR_ERR(process->mm);
			process->mm = NULL;
			goto mm_failed;
		}
	}

	if (args && args->fdtable) {
		process->fdtable = fdtable_get(args->fdtable);
	} else {
		process->fdtable = fdtable_alloc();
		if (!process->fdtable) {
			err = -ENOMEM;
			goto fdtable_failed;
		}
	}

	if (args && args->fsinfo) {
		process->fsinfo = fsinfo_get(args->fsinfo);
	} else {
		process->fsinfo = fsinfo_alloc();
		if (!process->fsinfo) {
			err = -ENOMEM;
			goto fsinfo_failed;
		}
	}

	if (args && args->rsrc_usage) {
		process->rsrc_usage = args->rsrc_usage;
	} else {
		process->rsrc_usage = process_rusage_alloc();
		if (!process->rsrc_usage) {
			err = -ENOMEM;
			goto rusage_failed;
		}
	}

	thread = thread_create_in(process, args ? args->fn : NULL, true);
	if (IS_ERR(thread)) {
		err = PTR_ERR(thread);
		goto thread_failed;
	}

	err = signal_init(process, thread, args ? args->sigactions : NULL,
			  args ? args->blocked : 0);
	if (err)
		goto signal_failed;

	process_link(process);

	if (process->parent) {
		irqstate_t flags = spin_lock_irqsave(&wait_lock);

		list_add_tail(&process->sibling, &process->parent->children);
		spin_unlock_irqrestore(&wait_lock, flags);
	}

	return process;

signal_failed:
	signal_deinit(process);
	thread_destroy(thread);
thread_failed:
	if (!(args && args->rsrc_usage))
		process_rusage_free(process->rsrc_usage);
rusage_failed:
	fsinfo_put(process->fsinfo);
fsinfo_failed:
	fdtable_put(process->fdtable);
fdtable_failed:
	uvm_space_put(process->mm);
mm_failed:
	pid_free(process->pid);
pid_failed:
	process_free(process);
	return ERR_PTR(err);
}

static void complete_thread_switch(void)
{
	sched_resume();

	struct thread *thread = current_thread();
	raw_spin_unlock(&thread->lock);

	/*
	 * The switch path always runs with interrupts disabled. A thread enters
	 * its kernel entry trampoline for the first time here, so this is the
	 * point where normal interrupt handling resumes.
	 */
	local_irq_enable();
}

static void idle_entry(void)
{
	complete_thread_switch();

	for (;;) {
		local_irq_enable();
		cpu_idle();
		local_irq_disable();
		thread_yield();
	}
}

struct process *process_create_idle(cpuid_t cpuid)
{
	struct process *process;
	struct thread *thread;

	if (cpuid >= MAX_CPUS)
		return NULL;

	process = process_alloc();
	if (!process)
		return NULL;

	list_init(&process->list);
	list_init(&process->children);
	list_init(&process->sibling);
	list_init(&process->threads);
	spinlock_init(&process->lock, "idle_process");
	process->pid = 0;
	process->pgid = 0;
	process->sid = 0;
	process->flags = PROCESS_FLAG_STATIC | PROCESS_FLAG_IDLE |
			 PROCESS_FLAG_KTHREAD;
	strlcpy(process->name, "idle", sizeof(process->name));

	thread = thread_create_in(process, idle_entry, true);
	if (IS_ERR(thread))
		goto fail;

	thread->state = THREAD_STATE_RUNNING;
	thread->is_init_thread = true;
	trap_frame_set_kernel_tp(thread->frame, (unsigned long)&cpus[cpuid]);

	return process;

fail:
	process_free(process);
	return NULL;
}

static struct process *process_create_user_init(void)
{
	struct process *init = process_create(&(struct process_create_args){
		.fn = ret_from_user_init,
		.name = "init",
	});
	if (IS_ERR(init))
		panic("failed to create user init process: %s\n",
		      strerror(PTR_ERR(init)));

	return init;
}

static void process_start_user_init(struct process *init)
{
	struct process *parent = current_process();

	fsinfo_copy(init->fsinfo, parent->fsinfo);
	fdtable_copy(init->fdtable, parent->fdtable);

	irqstate_t flags = spin_lock_irqsave(&wait_lock);
	init->parent = parent;
	list_add(&init->sibling, &parent->children);
	spin_unlock_irqrestore(&wait_lock, flags);

	flags = spin_lock_irqsave(&init->leader->lock);
	init->leader->state = THREAD_STATE_RUNNING;
	spin_unlock_irqrestore(&init->leader->lock, flags);

	sched_enqueue(init->leader);
}

void process_init_main(void)
{
	init_reaper = current_process();

	/*
	 * Allocate the user init process before fs_init() so it keeps PID 1.
	 * It stays NEW until the filesystem is ready and is only then made
	 * runnable.
	 */
	struct process *init = process_create_user_init();

	/* Block probing issues real I/O and must precede the rootfs mount. */
	gendisk_scan_partitions();

	int err = fs_init();
	if (err)
		panic("failed to initialize filesystem: %s\n", strerror(err));

	err = rootfs_init();
	if (err)
		panic("failed to initialize rootfs: %s\n", strerror(err));

	err = netif_rx_start();
	if (err)
		panic("failed to start network RX worker: %s\n", strerror(err));
	pr_info("file system initialized\n");

	process_start_user_init(init);

	for (;;) {
		if (process_wait(-1, NULL, 0, NULL, NULL) < 0)
			ktime_nanosleep((ktime_t)100 * NS_PER_MS, NULL);
	}
}

void user_init_entry(void)
{
	complete_thread_switch();

	char *argv[] = { "/sbin/init", 0 };
	char *envp[] = { 0 };
	int err = do_execve(argv[0], argv, envp);
	if (err < 0) {
		pr_err("execve %s failed: %s\n", argv[0], strerror(err));
		thread_exit_normal(1);
		panic("user init exit returned\n");
	}
	pr_debug("execve /bin/init ok\n");

	/*
	 * Start persistent kernel logging only after the initial exec
	 * succeeded.  Until klogd has opened /var/log/kernel.log the UART console
	 * stays active, so an exec failure is still visible.
	 */
	int console_err = console_log_start();
	if (console_err)
		pr_warn("console: failed to start log thread: %d\n",
			console_err);

	int klog_err = klog_start();
	if (klog_err)
		pr_warn("klog: failed to start log thread: %d\n", klog_err);

	int writeback_err = writeback_start();
	if (writeback_err)
		pr_warn("writeback: failed to start flush thread: %d\n",
			writeback_err);

	bring_up_secondaries();

	prepare_to_ret_to_user(current_thread()->frame);
}

/*
 * Find a process by pid. Caller must hold process_list_lock.
 */
static struct process *process_find_by_pid_locked(pid_t pid)
{
	struct process *process;

	list_for_each_entry(process, &processes, list) {
		if (process->pid == pid)
			return process;
	}
	return NULL;
}

int process_snapshot_pids(pid_t *out, int max)
{
	struct process *process;
	int n = 0;

	if (!out || max <= 0)
		return 0;

	irqstate_t flags = spin_lock_irqsave(&process_list_lock);
	list_for_each_entry(process, &processes, list) {
		if (n >= max)
			break;
		out[n++] = process->pid;
	}
	spin_unlock_irqrestore(&process_list_lock, flags);
	return n;
}

bool process_pid_exists(pid_t pid)
{
	bool found;
	irqstate_t flags;

	flags = spin_lock_irqsave(&process_list_lock);
	found = process_find_by_pid_locked(pid) != NULL;
	spin_unlock_irqrestore(&process_list_lock, flags);
	return found;
}

bool process_pgrp_exists(pid_t pgid, pid_t sid)
{
	struct process *process;
	bool found = false;
	irqstate_t flags;

	if (pgid <= 0)
		return false;

	flags = spin_lock_irqsave(&process_list_lock);
	list_for_each_entry(process, &processes, list) {
		if (process->pgid == pgid && process->sid == sid) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&process_list_lock, flags);
	return found;
}

pid_t process_get_pgid(pid_t pid)
{
	struct process *process;
	pid_t pgid = -ESRCH;
	irqstate_t flags;

	flags = spin_lock_irqsave(&process_list_lock);
	process = process_find_by_pid_locked(pid);
	if (process)
		pgid = process->pgid;
	spin_unlock_irqrestore(&process_list_lock, flags);
	return pgid;
}

pid_t process_get_sid(pid_t pid)
{
	struct process *process;
	pid_t sid = -ESRCH;
	irqstate_t flags;

	flags = spin_lock_irqsave(&process_list_lock);
	process = process_find_by_pid_locked(pid);
	if (process)
		sid = process->sid;
	spin_unlock_irqrestore(&process_list_lock, flags);
	return sid;
}

static enum thread_state process_state_locked(struct process *process)
{
	struct thread *thread;
	enum thread_state state = THREAD_STATE_SLEEPING;

	if (process->exited || process->nr_live == 0)
		return THREAD_STATE_ZOMBIE;

	list_for_each_entry(thread, &process->threads, proc_threads) {
		enum thread_state tstate =
			__atomic_load_n(&thread->state, __ATOMIC_RELAXED);

		if (tstate == THREAD_STATE_ZOMBIE)
			continue;
		if (tstate == THREAD_STATE_RUNNING)
			return THREAD_STATE_RUNNING;
		if (tstate == THREAD_STATE_NEW)
			state = THREAD_STATE_NEW;
	}
	return state;
}

bool process_get_info(pid_t pid, struct process_info *info)
{
	struct process *target;
	struct process *parent;
	enum thread_state state;
	int exit_status;
	unsigned int nr_threads;
	u64 utime;
	u64 ktime;
	u64 brk;
	char name[PROCESS_NAME_MAX];
	bool killed = false;

	if (!info)
		return false;

	irqstate_t lflags = spin_lock_irqsave(&process_list_lock);
	target = process_find_by_pid_locked(pid);
	if (!target) {
		spin_unlock_irqrestore(&process_list_lock, lflags);
		return false;
	}

	parent = target->parent;
	info->ppid = parent ? parent->pid : 0;

	spin_lock(&target->lock);
	info->pid = target->pid;
	exit_status = target->exit_status;
	nr_threads = target->nr_live;
	brk = target->mm ? target->mm->brk : 0;
	state = process_state_locked(target);
	for (size_t i = 0; i < PROCESS_NAME_MAX; ++i) {
		name[i] = target->name[i];
		if (name[i] == '\0')
			break;
	}
	name[PROCESS_NAME_MAX - 1] = '\0';
	spin_unlock(&target->lock);

	utime = process_get_user_time(target);
	ktime = process_get_system_time(target);
	if (target->leader)
		killed = signal_pending(target->leader);

	info->state = state;
	info->exit_status = exit_status;
	info->nr_threads = nr_threads;
	info->utime = utime;
	info->ktime = ktime;
	info->brk = brk;
	info->killed = killed;
	memcpy(info->name, name, sizeof(info->name));

	spin_unlock_irqrestore(&process_list_lock, lflags);
	return true;
}

int process_fork(void)
{
	int err;
	pid_t cpid;
	struct process *child;
	struct process *parent = current_process();

	child = process_create(&(struct process_create_args){
		.fn = ret_from_fork,
		.parent = parent,
		.pgid = parent->pgid,
		.sid = parent->sid,
		.name = parent->name,
	});
	if (IS_ERR(child))
		return PTR_ERR(child);

	err = uvm_space_copy(child->mm, parent->mm);
	if (err)
		goto failed;

	child->cred = parent->cred;

	fdtable_copy(child->fdtable, parent->fdtable);
	fsinfo_copy(child->fsinfo, parent->fsinfo);
	signal_copy(child, child->leader, parent, current_thread());

	memcpy(child->leader->frame, current_thread()->frame, TRAP_FRAME_SIZE);
	trap_frame_set_ret_val(child->leader->frame, 0);

	cpid = child->pid;
	child->leader->newly_forked = true;

	irqstate_t flags = spin_lock_irqsave(&child->leader->lock);
	child->leader->state = THREAD_STATE_RUNNING;
	spin_unlock_irqrestore(&child->leader->lock, flags);

	sched_enqueue(child->leader);

	return cpid;

failed:
	process_destroy(child);
	return err;
}

int process_get_times(struct process *process, struct tms *times)
{
	if (!process || !process->rsrc_usage)
		return -EINVAL;

	spin_lock(&process->rsrc_usage->lock);
	if (times)
		*times = process->rsrc_usage->times;
	spin_unlock(&process->rsrc_usage->lock);

	return 0;
}

void process_add_system_time(struct process *process, u64 time)
{
	if (!process || !process->rsrc_usage)
		return;

	spin_lock(&process->rsrc_usage->lock);
	process->rsrc_usage->times.tms_stime += time;
	spin_unlock(&process->rsrc_usage->lock);
}

void process_add_user_time(struct process *process, u64 time)
{
	if (!process || !process->rsrc_usage)
		return;

	spin_lock(&process->rsrc_usage->lock);
	process->rsrc_usage->times.tms_utime += time;
	spin_unlock(&process->rsrc_usage->lock);
}

void process_add_child_system_time(struct process *process, u64 time)
{
	if (!process || !process->rsrc_usage)
		return;

	spin_lock(&process->rsrc_usage->lock);
	process->rsrc_usage->times.tms_cstime += time;
	spin_unlock(&process->rsrc_usage->lock);
}

void process_add_child_user_time(struct process *process, u64 time)
{
	if (!process || !process->rsrc_usage)
		return;

	spin_lock(&process->rsrc_usage->lock);
	process->rsrc_usage->times.tms_cutime += time;
	spin_unlock(&process->rsrc_usage->lock);
}

u64 process_get_system_time(struct process *process)
{
	u64 time;

	if (!process || !process->rsrc_usage)
		return 0;

	spin_lock(&process->rsrc_usage->lock);
	time = process->rsrc_usage->times.tms_stime;
	spin_unlock(&process->rsrc_usage->lock);

	return time;
}

u64 process_get_user_time(struct process *process)
{
	u64 time;

	if (!process || !process->rsrc_usage)
		return 0;

	spin_lock(&process->rsrc_usage->lock);
	time = process->rsrc_usage->times.tms_utime;
	spin_unlock(&process->rsrc_usage->lock);

	return time;
}

u64 process_get_child_system_time(struct process *process)
{
	u64 time;

	if (!process || !process->rsrc_usage)
		return 0;

	spin_lock(&process->rsrc_usage->lock);
	time = process->rsrc_usage->times.tms_cstime;
	spin_unlock(&process->rsrc_usage->lock);

	return time;
}

u64 process_get_child_user_time(struct process *process)
{
	u64 time;

	if (!process || !process->rsrc_usage)
		return 0;

	spin_lock(&process->rsrc_usage->lock);
	time = process->rsrc_usage->times.tms_cutime;
	spin_unlock(&process->rsrc_usage->lock);

	return time;
}

struct process_resource_usage *process_rusage_alloc(void)
{
	struct process_resource_usage *rsrc_usage;

	rsrc_usage = kzalloc(sizeof(struct process_resource_usage));
	if (!rsrc_usage)
		return NULL;

	spinlock_init(&rsrc_usage->lock, "process_resource_usage");
	return rsrc_usage;
}

void process_rusage_free(struct process_resource_usage *rsrc_usage)
{
	if (!rsrc_usage)
		return;

	kfree(rsrc_usage);
}

static void kthread_return(void)
{
	complete_thread_switch();
	struct thread *thread = current_thread();

	if (thread->kthrd_fn)
		thread->kthrd_fn(thread->kthrd_data);
	thread_exit_normal(0);
}

struct thread *kthread_create(void (*fn)(void *), void *data)
{
	struct process *process;
	struct thread *thread;

	process = process_create(&(struct process_create_args){
		.fn = kthread_return,
		.parent = init_reaper,
		.name = "kthread",
		.flags = PROCESS_FLAG_KTHREAD,
	});
	if (IS_ERR(process))
		return NULL;

	thread = process->leader;
	thread->kthrd_fn = fn;
	thread->kthrd_data = data;
	return thread;
}

void thread_start(struct thread *thread)
{
	irqstate_t flags = spin_lock_irqsave(&thread->lock);

	thread->state = THREAD_STATE_RUNNING;
	spin_unlock_irqrestore(&thread->lock, flags);
	sched_enqueue(thread);
}

void thread_sched_tail(void)
{
	struct thread *thread = current_thread();
	assert(thread != NULL);
	assert(thread->newly_forked);
	thread->newly_forked = false;
	complete_thread_switch();
}
