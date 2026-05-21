#include <brk/asm.h>
#include <brk/dcache.h>
#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/limits.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/pgtable.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/riscv.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <brk/vmalloc.h>

cpuid_t init_cpuid;
struct process *init_proc;
struct cpu cpus[NR_CPUS];

static LIST_DEFINE(procs);
static SPINLOCK_DEFINE(procs_lock);
static struct kmem_cache proc_cache;

void proc_cache_init(void)
{
	kmem_cache_init(&proc_cache, sizeof(struct process),
			alignof(struct process), "proc_cache");
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

struct process *proc_alloc(void)
{
	struct process *proc = kmem_cache_alloc(&proc_cache);
	if (!proc)
		return NULL;
	memset(proc, 0, sizeof(*proc));

	list_init(&proc->list);
	list_init(&proc->queue);
	list_init(&proc->children);
	list_init(&proc->child);
	spinlock_init(&proc->lock, "proc");
	proc->state = PROCESS_STATE_NEW;

	proc->pid = pid_alloc();
	if (proc->pid < 0)
		goto pid_alloc_failed;

	proc->kstack = kstack_alloc();
	if (!proc->kstack)
		goto kstack_alloc_failed;

	proc->mm = mm_alloc();
	if (!proc->mm)
		goto create_mm_failed;

	spinlock_acquire(&procs_lock);
	list_add_tail(&proc->list, &procs);
	spinlock_release(&procs_lock);

	return proc;

create_mm_failed:
	kstack_free(proc->kstack);
kstack_alloc_failed:
	pid_free(proc->pid);
pid_alloc_failed:
	kmem_cache_free(&proc_cache, proc);
	return NULL;
}

void proc_free(struct process *proc)
{
	spinlock_acquire(&procs_lock);
	list_del_init(&proc->list);
	spinlock_release(&procs_lock);
	mm_free(proc->mm);
	kstack_free(proc->kstack);
	pid_free(proc->pid);
	kmem_cache_free(&proc_cache, proc);
}

int proc_alloc_fd(struct process *proc, struct file *fp)
{
	for (int i = 0; i < OPEN_MAX; ++i) {
		if (!proc->ofiles[i]) {
			proc->ofiles[i] = fp;
			return i;
		}
	}

	return -1;
}

static void user_init_proc_return(void)
{
	proc_sched_resume();
	struct process *proc = current_process();
	spinlock_release(&proc->lock);

	int err = fs_init();
	if (err)
		panic("failed to initialize filesystem: %s\n", strerror(err));

	char *argv[] = { "/bin/init", 0 };
	char *envp[] = { 0 };
	err = do_execve(argv[0], argv, envp);
	if (err < 0)
		panic("execve %s failed: %s\n", argv[0], strerror(err));

	proc->tf.a0 = err;
	prepare_to_return();
	user_trap_return(&proc->tf);
}

void proc_init_user(void)
{
	init_proc = proc_alloc();
	if (!init_proc)
		panic("failed to create user init process\n");
	spinlock_acquire(&init_proc->lock);
	strlcpy(init_proc->name, "init", sizeof(init_proc->name));
	init_proc->ctx.ra = (u64)user_init_proc_return;
	init_proc->ctx.sp = init_proc->kstack + KSTACK_SIZE;
	init_proc->state = PROCESS_STATE_RUNNING;
	init_proc->irq_enabled = true;
	spinlock_release(&init_proc->lock);
	proc_join(init_proc);
}

void proc_set_killed(struct process *proc)
{
	spinlock_acquire(&proc->lock);
	proc->killed = true;
	spinlock_release(&proc->lock);
}

bool proc_is_killed(struct process *proc)
{
	bool killed;
	spinlock_acquire(&proc->lock);
	killed = proc->killed;
	spinlock_release(&proc->lock);
	return killed;
}

int proc_set_brk(u64 addr)
{
	struct process *proc = current_process();
	struct mm_struct *mm = proc->mm;
	struct vm_area *heap = mm->heap;
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

int proc_snapshot_pids(pid_t *out, int max)
{
	struct process *p;
	int n = 0;

	if (!out || max <= 0)
		return 0;

	spinlock_acquire(&procs_lock);
	list_for_each_entry(p, &procs, list) {
		if (n >= max)
			break;
		out[n++] = p->pid;
	}
	spinlock_release(&procs_lock);
	return n;
}

bool proc_pid_exists(pid_t pid)
{
	struct process *p;
	bool found = false;

	spinlock_acquire(&procs_lock);
	list_for_each_entry(p, &procs, list) {
		if (p->pid == pid) {
			found = true;
			break;
		}
	}
	spinlock_release(&procs_lock);
	return found;
}

bool proc_get_info(pid_t pid, struct process_info *info)
{
	struct process *p, *target = NULL;

	if (!info)
		return false;

	spinlock_acquire(&procs_lock);
	list_for_each_entry(p, &procs, list) {
		if (p->pid == pid) {
			target = p;
			break;
		}
	}
	if (!target) {
		spinlock_release(&procs_lock);
		return false;
	}

	/*
	 * procs_lock keeps both @target and its parent on the procs list,
	 * so neither can be freed underneath us. We take a snapshot of
	 * parent->pid without taking wait_lock: a concurrent reparent
	 * (proc_exit re-parenting orphans to init_proc) is a benign race
	 * here -- we may observe either the old or new parent pid.
	 */
	{
		struct process *par = target->parent;
		info->ppid = par ? par->pid : 0;
	}

	spinlock_acquire(&target->lock);
	info->pid = target->pid;
	info->state = target->state;
	info->exit_status = target->exit_status;
	info->killed = target->killed;
	info->utime = target->ptms.tms_utime;
	info->ktime = target->ptms.tms_stime;
	info->brk = target->mm ? target->mm->brk : 0;
	for (usize_t i = 0; i < PROCESS_NAME_MAX; ++i) {
		info->name[i] = target->name[i];
		if (target->name[i] == '\0')
			break;
	}
	info->name[PROCESS_NAME_MAX - 1] = '\0';
	spinlock_release(&target->lock);

	spinlock_release(&procs_lock);
	return true;
}

void proc_dump(void)
{
	static char *state_strs[] = {
		[PROCESS_STATE_NEW] = "new    ",
		[PROCESS_STATE_SLEEPING] = "sleep  ",
		[PROCESS_STATE_RUNNING] = "running",
		[PROCESS_STATE_ZOMBIE] = "zombie ",
	};
	struct process *proc;
	char *state;

	spinlock_acquire(&procs_lock);
	printk("\n");
	list_for_each_entry(proc, &procs, list) {
		if (proc->state >= 0 && proc->state < countof(state_strs) &&
		    state_strs[proc->state])
			state = state_strs[proc->state];
		else
			state = "???    ";
		printk("%ld %s %s\n", proc->pid, state, proc->name);
	}
	spinlock_release(&procs_lock);
}

static void proc_fork_return(void)
{
	proc_sched_resume();
	struct process *proc = current_process();
	spinlock_release(&proc->lock);
	prepare_to_return();
	user_trap_return(&proc->tf);
}

int proc_fork(void)
{
	int err;
	pid_t cpid;
	struct process *child;
	struct process *parent = current_process();

	child = proc_alloc();
	if (!child)
		return -ENOMEM;

	err = mm_copy(child->mm, parent->mm);
	if (err) {
		proc_free(child);
		return err;
	}

	memcpy(&child->tf, &parent->tf, sizeof(parent->tf));

	for (int i = 0; i < OPEN_MAX; ++i) {
		if (parent->ofiles[i])
			child->ofiles[i] = file_dup(parent->ofiles[i]);
	}
	path_dup(&parent->cwd);
	child->cwd = parent->cwd;
	path_dup(&parent->root);
	child->root = parent->root;

	child->tf.a0 = 0;

	spinlock_acquire(&wait_lock);
	child->parent = parent;
	list_add(&child->child, &parent->children);
	spinlock_release(&wait_lock);

	spinlock_acquire(&child->lock);
	cpid = child->pid;
	child->state = PROCESS_STATE_RUNNING;
	child->ctx.ra = (u64)proc_fork_return;
	child->ctx.sp = child->kstack + KSTACK_SIZE;
	spinlock_release(&child->lock);

	proc_join(child);

	return cpid;
}

struct process *current_process(void)
{
	push_off();
	struct cpu *cpu = current_cpu();
	struct process *proc = cpu->current;
	pop_off();
	return proc;
}

cpuid_t current_cpuid(void)
{
	return read_tp();
}

struct cpu *current_cpu(void)
{
	cpuid_t id = current_cpuid();
	return &cpus[id];
}

void push_off(void)
{
	int enabled = intr_off_get();
	struct cpu *cpu = current_cpu();
	if (cpu->irq_nest == 0)
		cpu->irq_enabled = enabled;
	cpu->irq_nest += 1;
}

void pop_off(void)
{
	struct cpu *cpu = current_cpu();
	if (intr_enabled())
		panic("%s(): interruptible\n", __func__);
	if (cpu->irq_nest < 1)
		panic("%s(): nesting level < 1\n", __func__);
	cpu->irq_nest -= 1;
	if (cpu->irq_nest == 0 && cpu->irq_enabled)
		intr_on();
}
