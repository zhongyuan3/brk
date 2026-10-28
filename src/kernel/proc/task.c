#include <arch/context.h>
#include <arch/csr.h>
#include <arch/pgtable.h>
#include <arch/trapframe.h>
#include <brk/asm/cpu.h>
#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/fsinfo.h>
#include <brk/init.h>
#include <brk/initcall.h>
#include <brk/kernel.h>
#include <brk/klog.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/signal.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/stddef.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/task_types.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <brk/vmalloc.h>
#include <brk/writeback.h>
#include <uapi/errno.h>
#include <uapi/limits.h>
#include <uapi/signal.h>

struct task *initial_task;

LIST_DEFINE(tasks);
SPINLOCK_DEFINE(tasks_lock);
static struct slab_allocator task_cache;

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

void task_cache_init(void)
{
	slab_init(&task_cache, sizeof(struct task), alignof(struct task),
		  "task_cache");
}
core_initcall(task_cache_init);

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
	ASSERT(pg);
	page_free(pg, KSTACK_PAGE_ORDER);
}

static struct task *task_alloc(void)
{
	return slab_alloc_zero(&task_cache);
}

static void task_free(struct task *task)
{
	slab_free(&task_cache, task);
}

static void default_new_task_entry(void)
{
	task_exit_normal(0);
}

struct task *task_create(struct task_create_args *args)
{
	struct task *task;
	int err = 0;

	if (args && args->tgid < 0)
		return ERR_PTR(-EINVAL);

	task = task_alloc();
	if (!task)
		return ERR_PTR(-ENOMEM);

	list_init(&task->list);
	list_init(&task->queue);
	list_init(&task->children);
	list_init(&task->child);
	spinlock_init(&task->lock, "task");
	task->state = TASK_STATE_NEW;
	task->oncpu = -1;
	task->cpu = (int)this_cpuid();

	task->pid = pid_alloc();
	if (task->pid < 0) {
		err = -ENOMEM;
		goto pid_alloc_failed;
	}
	task->tgid = (!args || args->tgid == 0) ? task->pid : args->tgid;
	task->pgid = (args && args->pgid) ? args->pgid : task->pid;
	task->sid = (args && args->sid) ? args->sid : task->pid;

	task->kstack_base = kstack_alloc();
	if (!task->kstack_base) {
		err = -ENOMEM;
		goto kstack_alloc_failed;
	}
	task->kstack_top = task->kstack_base + KSTACK_SIZE;
	unsigned long kernel_sp = task->kstack_top;
	kernel_sp -= TRAP_FRAME_SIZE;
	ASSERT(is_aligned(kernel_sp, 16));
	task->frame = (struct riscv_trap_frame *)kernel_sp;
	memset(task->frame, 0, TRAP_FRAME_SIZE);
	kernel_sp -= CONTEXT_SIZE;
	ASSERT(is_aligned(kernel_sp, 16));
	struct riscv_context *ctx = (struct riscv_context *)kernel_sp;
	if (args && args->fn)
		ctx->ra = (unsigned long)args->fn;
	else
		ctx->ra = (unsigned long)default_new_task_entry;
	task->kernel_sp = kernel_sp;

	if (args && args->mm) {
		task->mm = uvm_space_get(args->mm);
	} else {
		task->mm = uvm_space_create();
		if (IS_ERR(task->mm)) {
			err = PTR_ERR(task->mm);
			goto create_mm_failed;
		}
	}

	if (args && args->fdtable) {
		task->fdtable = fdtable_get(args->fdtable);
	} else {
		task->fdtable = fdtable_alloc();
		if (!task->fdtable) {
			err = -ENOMEM;
			goto fdtable_alloc_failed;
		}
	}

	if (args && args->fsinfo) {
		task->fsinfo = fsinfo_get(args->fsinfo);
	} else {
		task->fsinfo = fsinfo_alloc();
		if (!task->fsinfo) {
			err = -ENOMEM;
			goto fsinfo_alloc_failed;
		}
	}

	if (args && args->rsrc_usage) {
		task->rsrc_usage = args->rsrc_usage;
	} else {
		task->rsrc_usage = task_rusage_alloc();
		if (!task->rsrc_usage) {
			err = -ENOMEM;
			goto rsrc_usage_alloc_failed;
		}
	}

	if (args)
		err = signal_init(task, args->sigactions, args->blocked);
	else
		err = signal_init(task, NULL, 0);
	if (err)
		goto signal_init_failed;

	irqstate_t flags = spin_lock_irqsave(&tasks_lock);
	list_add_tail(&task->list, &tasks);
	spin_unlock_irqrestore(&tasks_lock, flags);

	return task;

signal_init_failed:
	signal_deinit(task);
	if (!(args && args->rsrc_usage))
		task_rusage_free(task->rsrc_usage);
rsrc_usage_alloc_failed:
	fsinfo_put(task->fsinfo);
fsinfo_alloc_failed:
	fdtable_put(task->fdtable);
fdtable_alloc_failed:
	uvm_space_put(task->mm);
create_mm_failed:
	kstack_free(task->kstack_base);
kstack_alloc_failed:
	pid_free(task->pid);
pid_alloc_failed:
	task_free(task);
	return ERR_PTR(err);
}

static void idle_entry(void)
{
	for (;;) {
		local_irq_enable();
		cpu_idle();
		local_irq_disable();
		task_yield();
	}
}

struct task *task_create_idle(cpuid_t cpuid)
{
	struct task *task;
	unsigned long kernel_sp;
	struct riscv_context *ctx;

	if (cpuid >= MAX_CPUS)
		return NULL;

	task = task_alloc();
	if (!task)
		return NULL;

	list_init(&task->list);
	list_init(&task->queue);
	list_init(&task->children);
	list_init(&task->child);
	spinlock_init(&task->lock, "idle");
	task->state = TASK_STATE_RUNNING;
	task->oncpu = -1;
	task->cpu = (int)cpuid;
	task->pid = 0;
	task->tgid = 0;
	task->pgid = 0;
	task->sid = 0;

	task->kstack_base = kstack_alloc();
	if (!task->kstack_base)
		goto fail;

	task->kstack_top = task->kstack_base + KSTACK_SIZE;
	kernel_sp = task->kstack_top - TRAP_FRAME_SIZE;
	ASSERT(is_aligned(kernel_sp, 16));
	task->frame = (struct riscv_trap_frame *)kernel_sp;
	memset(task->frame, 0, TRAP_FRAME_SIZE);
	kernel_sp -= CONTEXT_SIZE;
	ASSERT(is_aligned(kernel_sp, 16));
	ctx = (struct riscv_context *)kernel_sp;
	ctx->ra = (unsigned long)idle_entry;
	task->kernel_sp = kernel_sp;

	task->frame->kernel_tp = (unsigned long)&cpus[cpuid];
	strlcpy(task->name, "idle", sizeof(task->name));

	return task;

fail:
	task_free(task);
	return NULL;
}

void task_destroy(struct task *task)
{
	irqstate_t flags = spin_lock_irqsave(&tasks_lock);
	list_del_init(&task->list);
	spin_unlock_irqrestore(&tasks_lock, flags);

	signal_deinit(task);
	if (task_is_leader(task))
		task_rusage_free(task->rsrc_usage);
	/* task_exit() may already have dropped these references. */
	if (task->fsinfo)
		fsinfo_put(task->fsinfo);
	if (task->fdtable)
		fdtable_put(task->fdtable);
	uvm_space_put(task->mm);
	kstack_free(task->kstack_base);
	pid_free(task->pid);
	task_free(task);
}

static void complete_task_switch(void)
{
	task_sched_resume();

	struct task *task = current_task();
	raw_spin_unlock(&task->lock);

	/*
	 * The switch path always runs with interrupts disabled. A task enters
	 * its kernel entry trampoline for the first time here, so this is the
	 * point where normal interrupt handling resumes.
	 */
	local_irq_enable();
}

void init_task_do(void)
{
	complete_task_switch();

	// while (1) {
	// }

	int err = fs_init();
	if (err)
		panic("failed to initialize filesystem: %s\n", strerror(err));
	else
		pr_info("file system initialized\n");

	char *argv[] = { "/bin/init", 0 };
	char *envp[] = { 0 };
	err = do_execve(argv[0], argv, envp);
	if (err < 0)
		panic("execve %s failed: %s\n", argv[0], strerror(err));
	else
	 	pr_debug("execve /bin/init ok\n");

	/*
	 * Start persistent kernel logging only after the initial exec
	 * succeeded.  Until klogd has opened /kernel.log the UART console
	 * stays active, so an exec failure is still visible.
	 */
	int klog_err = klog_start();
	if (klog_err)
		pr_warn("klog: failed to start log thread: %d\n", klog_err);

	int writeback_err = writeback_start();
	if (writeback_err)
		pr_warn("writeback: failed to start flush thread: %d\n",
			writeback_err);

	smp_boot_release_secondary_harts();

	unsigned long sstatus = read_sstatus();
	sstatus &= ~SSTATUS_SPP;
	sstatus |= SSTATUS_SPIE;
	sstatus &= ~SSTATUS_SIE;
	sstatus &= ~SSTATUS_SUM;
	current_task()->frame->sstatus = sstatus;
}

void task_init_user(void)
{
	initial_task = task_create(&(struct task_create_args){
		.fn = ret_from_init,
	});
	if (IS_ERR(initial_task))
		panic("failed to create initial user task: %s\n",
		      strerror(PTR_ERR(initial_task)));
	irqstate_t flags = spin_lock_irqsave(&initial_task->lock);

	strlcpy(initial_task->name, "init", sizeof(initial_task->name));
	initial_task->state = TASK_STATE_RUNNING;
	spin_unlock_irqrestore(&initial_task->lock, flags);
	task_join(initial_task);
}
device_initcall(task_init_user);

void task_set_killed(struct task *task)
{
	signal_send(task, SIGKILL);
}

bool task_is_killed(struct task *task)
{
	return signal_pending(task);
}

int task_set_brk(u64 addr)
{
	struct task *task = current_task();
	struct uvm_space *mm = task->mm;
	struct uvm_region *heap = mm->heap;
	u64 heap_start = heap->addr;
	u64 curr_heap_end = heap_start + heap->size;
	int err = 0;
	u64 new_heap_end;
	size_t incr;

	if (addr < heap_start)
		return 0;

	if (addr <= curr_heap_end) {
		mm->brk = addr;
		return 0;
	}

	new_heap_end = round_up(addr, PAGE_SIZE);
	incr = new_heap_end - curr_heap_end;
	size_t old_npgs = heap->nr_pages;
	size_t new_npgs = old_npgs + (incr >> PAGE_SHIFT);
	struct page **new_pgs = kcalloc(new_npgs, sizeof(struct page *));
	if (!new_pgs)
		return -ENOMEM;
	memcpy(new_pgs, heap->pages, old_npgs * sizeof(struct page *));

	addr = curr_heap_end;
	size_t i = old_npgs;
	while (i < new_npgs) {
		struct page *pg = page_alloc(0);
		if (!pg) {
			err = -ENOMEM;
			goto failed;
		}
		u64 pa = page_to_phys(pg);
		err = map_user_pages(mm->pgd, addr, PAGE_SIZE, pa,
				     PTE_R | PTE_W);
		if (err) {
			ASSERT(pg);
			page_free(pg, 0);
			goto failed;
		}
		new_pgs[i] = pg;
		++i;
		addr += PAGE_SIZE;
	}

	struct page **old_pgs = heap->pages;
	heap->pages = new_pgs;
	heap->nr_pages = new_npgs;
	kfree(old_pgs);

	heap->size = new_heap_end - heap_start;
	mm->brk = addr;

	return 0;

failed:
	for (u64 a = curr_heap_end; a < addr; a += PAGE_SIZE)
		unmap_user_pages(mm->pgd, a, PAGE_SIZE);
	for (size_t j = old_npgs; j < i; ++j) {
		ASSERT(new_pgs[j]);
		page_free(new_pgs[j], 0);
	}
	kfree(new_pgs);
	return err;
}

/*
 * Find a task by thread-group ID. Prefer the group leader (tid == tgid)
 * when multiple threads share the same tgid. Caller must hold tasks_lock.
 */
static struct task *task_find_by_tgid_locked(pid_t tgid)
{
	struct task *p, *match = NULL;

	list_for_each_entry(p, &tasks, list) {
		if (p->tgid != tgid)
			continue;
		if (p->pid == p->tgid)
			return p;
		if (!match)
			match = p;
	}
	return match;
}

int task_snapshot_pids(pid_t *out, int max)
{
	struct task *p;
	int n = 0;

	if (!out || max <= 0)
		return 0;

	irqstate_t flags = spin_lock_irqsave(&tasks_lock);
	list_for_each_entry(p, &tasks, list) {
		bool dup;
		int i;

		if (n >= max)
			break;
		dup = false;
		for (i = 0; i < n; i++) {
			if (out[i] == p->tgid) {
				dup = true;
				break;
			}
		}
		if (!dup)
			out[n++] = p->tgid;
	}
	spin_unlock_irqrestore(&tasks_lock, flags);
	return n;
}

bool task_pid_exists(pid_t tgid)
{
	bool found;
	irqstate_t flags;

	flags = spin_lock_irqsave(&tasks_lock);
	found = task_find_by_tgid_locked(tgid) != NULL;
	spin_unlock_irqrestore(&tasks_lock, flags);
	return found;
}

bool task_pgrp_exists(pid_t pgid, pid_t sid)
{
	struct task *p;
	bool found = false;
	irqstate_t flags;

	if (pgid <= 0)
		return false;

	flags = spin_lock_irqsave(&tasks_lock);
	list_for_each_entry(p, &tasks, list) {
		if (p->pgid == pgid && p->sid == sid) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&tasks_lock, flags);
	return found;
}

pid_t task_get_pgid(pid_t tgid)
{
	struct task *p = NULL;
	pid_t pgid = -ESRCH;
	irqstate_t flags;

	flags = spin_lock_irqsave(&tasks_lock);
	p = task_find_by_tgid_locked(tgid);
	if (p)
		pgid = p->pgid;
	spin_unlock_irqrestore(&tasks_lock, flags);
	return pgid;
}

pid_t task_get_sid(pid_t tgid)
{
	struct task *p = NULL;
	pid_t sid = -ESRCH;
	irqstate_t flags;

	flags = spin_lock_irqsave(&tasks_lock);
	p = task_find_by_tgid_locked(tgid);
	if (p)
		sid = p->sid;
	spin_unlock_irqrestore(&tasks_lock, flags);
	return sid;
}

bool task_get_info(pid_t tgid, struct task_info *info)
{
	struct task *target = NULL;
	struct task *par;

	if (!info)
		return false;

	irqstate_t lflags;

	lflags = spin_lock_irqsave(&tasks_lock);
	target = task_find_by_tgid_locked(tgid);
	if (!target) {
		spin_unlock_irqrestore(&tasks_lock, lflags);
		return false;
	}

	/*
	 * tasks_lock keeps both @target and its parent on the tasks list,
	 * so neither can be freed underneath us. We take a snapshot of
	 * parent->tgid without taking wait_lock: a concurrent reparent
	 * (task_exit re-parenting orphans to initial_task) is a benign race
	 * here -- we may observe either the old or new parent tgid.
	 */
	par = target->parent;
	info->ppid = par ? par->tgid : 0;

	spin_lock(&target->lock);

	info->pid = target->tgid;
	info->state = target->state;
	info->exit_status = target->exit_status;
	info->utime = task_get_user_time(target);
	info->ktime = task_get_system_time(target);
	info->brk = target->mm ? target->mm->brk : 0;
	for (size_t i = 0; i < TASK_NAME_MAX; ++i) {
		info->name[i] = target->name[i];
		if (target->name[i] == '\0')
			break;
	}
	info->name[TASK_NAME_MAX - 1] = '\0';
	spin_unlock(&target->lock);
	info->killed = signal_pending(target);

	spin_unlock_irqrestore(&tasks_lock, lflags);
	return true;
}

void task_dump(void)
{
	static char *state_strs[] = {
		[TASK_STATE_NEW] = "new    ",
		[TASK_STATE_SLEEPING] = "sleep  ",
		[TASK_STATE_RUNNING] = "running",
		[TASK_STATE_ZOMBIE] = "zombie ",
	};
	struct task *task;
	char *state;

	irqstate_t flags = spin_lock_irqsave(&tasks_lock);
	printk("\n");
	list_for_each_entry(task, &tasks, list) {
		if (task->state >= 0 && task->state < countof(state_strs) &&
		    state_strs[task->state])
			state = state_strs[task->state];
		else
			state = "???    ";
		printk("%ld %s %s\n", task->pid, state, task->name);
	}
	spin_unlock_irqrestore(&tasks_lock, flags);
}

int task_fork(void)
{
	int err;
	pid_t cpid;
	struct task *child;
	struct task *parent = current_task();

	child = task_create(&(struct task_create_args){
		.fn = ret_from_fork,
		.pgid = parent->pgid,
		.sid = parent->sid,
	});
	if (IS_ERR(child))
		return PTR_ERR(child);

	err = uvm_space_copy(child->mm, parent->mm);
	if (err) {
		task_destroy(child);
		return err;
	}

	memcpy(child->frame, parent->frame, TRAP_FRAME_SIZE);
	signal_copy(child, parent);

	fdtable_copy(child->fdtable, parent->fdtable);
	fsinfo_copy(child->fsinfo, parent->fsinfo);

	child->frame->a0 = 0;

	irqstate_t flags = spin_lock_irqsave(&wait_lock);

	child->parent = parent;
	list_add(&child->child, &parent->children);
	spin_unlock_irqrestore(&wait_lock, flags);

	flags = spin_lock_irqsave(&child->lock);
	cpid = child->tgid;
	child->state = TASK_STATE_RUNNING;
	spin_unlock_irqrestore(&child->lock, flags);

	child->newly_forked = true;

	task_join(child);

	return cpid;
}

int task_get_times(struct task *task, struct tms *times)
{
	if (!task || !task->rsrc_usage)
		return -EINVAL;

	spin_lock(&task->rsrc_usage->lock);
	if (times)
		*times = task->rsrc_usage->task_times;
	spin_unlock(&task->rsrc_usage->lock);

	return 0;
}

void task_add_system_time(struct task *task, u64 time)
{
	if (!task || !task->rsrc_usage)
		return;

	spin_lock(&task->rsrc_usage->lock);
	task->rsrc_usage->task_times.tms_stime += time;
	spin_unlock(&task->rsrc_usage->lock);
}

void task_add_user_time(struct task *task, u64 time)
{
	if (!task || !task->rsrc_usage)
		return;

	spin_lock(&task->rsrc_usage->lock);
	task->rsrc_usage->task_times.tms_utime += time;
	spin_unlock(&task->rsrc_usage->lock);
}

void task_add_child_system_time(struct task *task, u64 time)
{
	if (!task || !task->rsrc_usage)
		return;

	spin_lock(&task->rsrc_usage->lock);
	task->rsrc_usage->task_times.tms_cstime += time;
	spin_unlock(&task->rsrc_usage->lock);
}

void task_add_child_user_time(struct task *task, u64 time)
{
	if (!task || !task->rsrc_usage)
		return;

	spin_lock(&task->rsrc_usage->lock);
	task->rsrc_usage->task_times.tms_cutime += time;
	spin_unlock(&task->rsrc_usage->lock);
}

u64 task_get_system_time(struct task *task)
{
	u64 time;

	if (!task || !task->rsrc_usage)
		return 0;

	spin_lock(&task->rsrc_usage->lock);
	time = task->rsrc_usage->task_times.tms_stime;
	spin_unlock(&task->rsrc_usage->lock);

	return time;
}

u64 task_get_user_time(struct task *task)
{
	u64 time;

	if (!task || !task->rsrc_usage)
		return 0;

	spin_lock(&task->rsrc_usage->lock);
	time = task->rsrc_usage->task_times.tms_utime;
	spin_unlock(&task->rsrc_usage->lock);

	return time;
}

u64 task_get_child_system_time(struct task *task)
{
	u64 time;

	if (!task || !task->rsrc_usage)
		return 0;

	spin_lock(&task->rsrc_usage->lock);
	time = task->rsrc_usage->task_times.tms_cstime;
	spin_unlock(&task->rsrc_usage->lock);

	return time;
}

u64 task_get_child_user_time(struct task *task)
{
	u64 time;

	if (!task || !task->rsrc_usage)
		return 0;

	spin_lock(&task->rsrc_usage->lock);
	time = task->rsrc_usage->task_times.tms_cutime;
	spin_unlock(&task->rsrc_usage->lock);

	return time;
}

struct task_resource_usage *task_rusage_alloc(void)
{
	struct task_resource_usage *rsrc_usage;

	rsrc_usage = kzalloc(sizeof(struct task_resource_usage));
	if (!rsrc_usage)
		return NULL;

	spinlock_init(&rsrc_usage->lock, "task_resource_usage");
	return rsrc_usage;
}

void task_rusage_free(struct task_resource_usage *rsrc_usage)
{
	if (!rsrc_usage)
		return;

	kfree(rsrc_usage);
}

static void kthread_return(void)
{
	complete_task_switch();
	struct task *task = current_task();

	if (task->kthrd_fn) {
		task->kthrd_fn(task->kthrd_data);
	}
	task_exit_normal(0);
}

struct task *kthread_create(void (*fn)(void *), void *data)
{
	struct task *t = task_create(&(struct task_create_args){
		.fn = kthread_return,
	});
	if (IS_ERR(t))
		return NULL;
	t->kthrd_fn = fn;
	t->kthrd_data = data;
	return t;
}

void kthread_join(struct task *kthread)
{
	irqstate_t flags = spin_lock_irqsave(&kthread->lock);

	kthread->state = TASK_STATE_RUNNING;
	spin_unlock_irqrestore(&kthread->lock, flags);
	task_join(kthread);
}

void task_schedlue_tail(void)
{
	struct task *task = current_task();
	ASSERT(task != NULL);
	ASSERT(task->newly_forked);
	task->newly_forked = false;
	complete_task_switch();
}
