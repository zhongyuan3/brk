#include <arch/pgtable.h>
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/fsinfo.h>
#include <brk/init.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/processor.h>
#include <brk/signal.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <brk/vmalloc.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/limits.h>
#include <uapi/signal.h>

cpuid_t boot_cpuid;
struct task_control_block *initial_task;
struct cpu cpus[NR_CPUS];

LIST_DEFINE(tasks);
SPINLOCK_DEFINE(tasks_lock);
static struct slab_allocator task_cache;

void task_cache_init(void)
{
	slab_init(&task_cache, sizeof(struct task_control_block),
		  alignof(struct task_control_block), "task_cache");
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
	struct page *pg = virt_to_page(stack);
	ASSERT(pg);
	page_free(pg, KSTACK_PAGE_ORDER);
}

static struct task_control_block *task_alloc(void)
{
	return slab_alloc_zero(&task_cache);
}

static void task_free(struct task_control_block *task)
{
	slab_free(&task_cache, task);
}

static void default_new_task_entry(void)
{
	task_exit_normal(0);
}

struct task_control_block *task_create(struct task_create_args *args)
{
	u64 kstack_top;
	struct extended_trap_frame *ext_tf;
	struct task_control_block *task;
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
	spinlock_init(&task->lock, "task_control_block");
	task->state = TASK_STATE_NEW;

	task->pid = pid_alloc();
	if (task->pid < 0) {
		err = -ENOMEM;
		goto pid_alloc_failed;
	}
	task->tgid = (!args || args->tgid == 0) ? task->pid : args->tgid;

	task->kstack_base = kstack_alloc();
	if (!task->kstack_base) {
		err = -ENOMEM;
		goto kstack_alloc_failed;
	}

	kstack_top = task->kstack_base + KSTACK_SIZE;
	kstack_top -= TRAP_FRAME_ON_STACK_SIZE;
	ext_tf = (struct extended_trap_frame *)kstack_top;
	ext_tf->task = task;
	task->tf = &ext_tf->tf;
	arch_tf_init(task->tf);
	task->kstack_top = kstack_top;

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

	if (args && args->fn)
		task->ctx.ra = (u64)args->fn;
	else
		task->ctx.ra = (u64)default_new_task_entry;
	task->ctx.sp = task->kstack_top;

	spinlock_acquire(&tasks_lock);
	list_add_tail(&task->list, &tasks);
	spinlock_release(&tasks_lock);

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

void task_destroy(struct task_control_block *task)
{
	spinlock_acquire(&tasks_lock);
	list_del_init(&task->list);
	spinlock_release(&tasks_lock);

	signal_deinit(task);
	if (task_is_leader(task))
		task_rusage_free(task->rsrc_usage);
	fsinfo_put(task->fsinfo);
	fdtable_put(task->fdtable);
	uvm_space_put(task->mm);
	kstack_free(task->kstack_base);
	pid_free(task->pid);
	task_free(task);
}

static void initial_task_return(void)
{
	task_sched_resume();
	struct task_control_block *task = current_task();
	spinlock_release(&task->lock);

	int err = fs_init();
	if (err)
		panic("failed to initialize filesystem: %s\n", strerror(err));

	char *argv[] = { "/bin/init", 0 };
	char *envp[] = { 0 };
	err = do_execve(argv[0], argv, envp);
	if (err < 0)
		panic("execve %s failed: %s\n", argv[0], strerror(err));

#if ENABLE_SMP
	smp_boot_release_secondary_harts();
#endif

	prepare_to_return();
	user_trap_return(task->tf);
}

void task_init_user(void)
{
	initial_task = task_create(NULL);
	if (IS_ERR(initial_task))
		panic("failed to create initial user task: %s\n",
		      strerror(PTR_ERR(initial_task)));
	spinlock_acquire(&initial_task->lock);
	strlcpy(initial_task->name, "init", sizeof(initial_task->name));
	initial_task->ctx.ra = (u64)initial_task_return;
	initial_task->ctx.sp = initial_task->kstack_top;
	initial_task->state = TASK_STATE_RUNNING;
	initial_task->irq_enabled = true;
	spinlock_release(&initial_task->lock);
	task_join(initial_task);
}

void task_set_killed(struct task_control_block *task)
{
	signal_send(task, SIGKILL);
}

bool task_is_killed(struct task_control_block *task)
{
	return signal_pending(task);
}

int task_set_brk(u64 addr)
{
	struct task_control_block *task = current_task();
	struct uvm_space *mm = task->mm;
	struct uvm_region *heap = mm->heap;
	u64 heap_start = heap->addr;
	u64 curr_heap_end = heap_start + heap->size;
	int err = 0;
	u64 new_heap_end;
	usize_t incr;

	if (addr < heap_start)
		return 0;

	if (addr <= curr_heap_end) {
		mm->brk = addr;
		return 0;
	}

	new_heap_end = round_up(addr, PAGE_SIZE);
	incr = new_heap_end - curr_heap_end;
	usize_t old_npgs = heap->nr_pages;
	usize_t new_npgs = old_npgs + (incr >> PAGE_SHIFT);
	struct page **new_pgs = kcalloc(new_npgs, sizeof(struct page *));
	if (!new_pgs)
		return -ENOMEM;
	memcpy(new_pgs, heap->pages, old_npgs * sizeof(struct page *));

	addr = curr_heap_end;
	usize_t i = old_npgs;
	while (i < new_npgs) {
		struct page *pg = page_alloc(0);
		if (!pg) {
			err = -ENOMEM;
			goto failed;
		}
		u64 pa = page_to_phys(pg);
		err = uvmap(mm->pgd, addr, PAGE_SIZE, pa, PTE_R | PTE_W);
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
		uvunmap(mm->pgd, a, PAGE_SIZE);
	for (usize_t j = old_npgs; j < i; ++j) {
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
static struct task_control_block *task_find_by_tgid_locked(pid_t tgid)
{
	struct task_control_block *p, *match = NULL;

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
	struct task_control_block *p;
	int n = 0;

	if (!out || max <= 0)
		return 0;

	spinlock_acquire(&tasks_lock);
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
	spinlock_release(&tasks_lock);
	return n;
}

bool task_pid_exists(pid_t tgid)
{
	bool found;

	spinlock_acquire(&tasks_lock);
	found = task_find_by_tgid_locked(tgid) != NULL;
	spinlock_release(&tasks_lock);
	return found;
}

bool task_get_info(pid_t tgid, struct task_info *info)
{
	struct task_control_block *target = NULL;
	struct task_control_block *par;

	if (!info)
		return false;

	spinlock_acquire(&tasks_lock);
	target = task_find_by_tgid_locked(tgid);
	if (!target) {
		spinlock_release(&tasks_lock);
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

	spinlock_acquire(&target->lock);
	info->pid = target->tgid;
	info->state = target->state;
	info->exit_status = target->exit_status;
	info->utime = task_get_user_time(target);
	info->ktime = task_get_system_time(target);
	info->brk = target->mm ? target->mm->brk : 0;
	for (usize_t i = 0; i < TASK_NAME_MAX; ++i) {
		info->name[i] = target->name[i];
		if (target->name[i] == '\0')
			break;
	}
	info->name[TASK_NAME_MAX - 1] = '\0';
	spinlock_release(&target->lock);
	info->killed = signal_pending(target);

	spinlock_release(&tasks_lock);
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
	struct task_control_block *task;
	char *state;

	spinlock_acquire(&tasks_lock);
	printk("\n");
	list_for_each_entry(task, &tasks, list) {
		if (task->state >= 0 && task->state < countof(state_strs) &&
		    state_strs[task->state])
			state = state_strs[task->state];
		else
			state = "???    ";
		printk("%ld %s %s\n", task->pid, state, task->name);
	}
	spinlock_release(&tasks_lock);
}

static void task_fork_return(void)
{
	task_sched_resume();
	struct task_control_block *task = current_task();
	spinlock_release(&task->lock);
	prepare_to_return();
	user_trap_return(task->tf);
}

int task_fork(void)
{
	int err;
	pid_t cpid;
	struct task_control_block *child;
	struct task_control_block *parent = current_task();

	child = task_create(NULL);
	if (IS_ERR(child))
		return PTR_ERR(child);

	err = uvm_space_copy(child->mm, parent->mm);
	if (err) {
		task_destroy(child);
		return err;
	}

	arch_tf_copy(child->tf, parent->tf);
	signal_copy(child, parent);

	fdtable_copy(child->fdtable, parent->fdtable);
	fsinfo_copy(child->fsinfo, parent->fsinfo);

	arch_tf_set_a0(child->tf, 0);

	spinlock_acquire(&wait_lock);
	child->parent = parent;
	list_add(&child->child, &parent->children);
	spinlock_release(&wait_lock);

	spinlock_acquire(&child->lock);
	cpid = child->tgid;
	child->state = TASK_STATE_RUNNING;
	child->ctx.ra = (u64)task_fork_return;
	child->ctx.sp = child->kstack_top;
	spinlock_release(&child->lock);

	task_join(child);

	return cpid;
}

int task_get_times(struct task_control_block *task, struct tms *times)
{
	if (!task || !task->rsrc_usage)
		return -EINVAL;

	spinlock_acquire(&task->rsrc_usage->lock);
	if (times)
		*times = task->rsrc_usage->task_times;
	spinlock_release(&task->rsrc_usage->lock);

	return 0;
}

void task_add_system_time(struct task_control_block *task, u64 time)
{
	if (!task || !task->rsrc_usage)
		return;

	spinlock_acquire(&task->rsrc_usage->lock);
	task->rsrc_usage->task_times.tms_stime += time;
	spinlock_release(&task->rsrc_usage->lock);
}

void task_add_user_time(struct task_control_block *task, u64 time)
{
	if (!task || !task->rsrc_usage)
		return;

	spinlock_acquire(&task->rsrc_usage->lock);
	task->rsrc_usage->task_times.tms_utime += time;
	spinlock_release(&task->rsrc_usage->lock);
}

void task_add_child_system_time(struct task_control_block *task, u64 time)
{
	if (!task || !task->rsrc_usage)
		return;

	spinlock_acquire(&task->rsrc_usage->lock);
	task->rsrc_usage->task_times.tms_cstime += time;
	spinlock_release(&task->rsrc_usage->lock);
}

void task_add_child_user_time(struct task_control_block *task, u64 time)
{
	if (!task || !task->rsrc_usage)
		return;

	spinlock_acquire(&task->rsrc_usage->lock);
	task->rsrc_usage->task_times.tms_cutime += time;
	spinlock_release(&task->rsrc_usage->lock);
}

u64 task_get_system_time(struct task_control_block *task)
{
	u64 time;

	if (!task || !task->rsrc_usage)
		return 0;

	spinlock_acquire(&task->rsrc_usage->lock);
	time = task->rsrc_usage->task_times.tms_stime;
	spinlock_release(&task->rsrc_usage->lock);

	return time;
}

u64 task_get_user_time(struct task_control_block *task)
{
	u64 time;

	if (!task || !task->rsrc_usage)
		return 0;

	spinlock_acquire(&task->rsrc_usage->lock);
	time = task->rsrc_usage->task_times.tms_utime;
	spinlock_release(&task->rsrc_usage->lock);

	return time;
}

u64 task_get_child_system_time(struct task_control_block *task)
{
	u64 time;

	if (!task || !task->rsrc_usage)
		return 0;

	spinlock_acquire(&task->rsrc_usage->lock);
	time = task->rsrc_usage->task_times.tms_cstime;
	spinlock_release(&task->rsrc_usage->lock);

	return time;
}

u64 task_get_child_user_time(struct task_control_block *task)
{
	u64 time;

	if (!task || !task->rsrc_usage)
		return 0;

	spinlock_acquire(&task->rsrc_usage->lock);
	time = task->rsrc_usage->task_times.tms_cutime;
	spinlock_release(&task->rsrc_usage->lock);

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
