#include <aosd/asm.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/limits.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/macros.h>
#include <aosd/mm.h>
#include <aosd/mm_types.h>
#include <aosd/panic.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/process.h>
#include <aosd/riscv.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/timer.h>
#include <aosd/trap.h>
#include <aosd/types.h>
#include <aosd/vmalloc.h>

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

static uint64_t kstack_alloc(void)
{
	struct page *pg = page_alloc(KSTACK_PAGE_ORDER);
	if (!pg)
		return 0;
	return page_to_virt(pg);
}

static void kstack_free(uint64_t stack)
{
	struct page *pg = virt_to_page(stack);
	assert(pg);
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
	fs_init();
	char *argv[] = { "/bin/init", 0 };
	char *envp[] = { 0 };
	int ret = do_execve(argv[0], argv, envp);
	if (ret < 0)
		panic("execve %s failed: %s\n", argv[0], strerror(ret));
	proc->tf.a0 = ret;
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
	init_proc->ctx.ra = (uint64_t)user_init_proc_return;
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

int proc_set_brk(uint64_t addr)
{
	struct process *proc = current_process();
	struct mm_struct *mm = proc->mm;
	struct vm_area *heap = mm->heap;
	uint64_t heap_start = heap->addr;
	uint64_t curr_heap_end = heap_start + heap->size;
	int err = 0;
	uint64_t new_heap_end;
	size_t incr;

	if (addr < heap_start)
		return 0;

	if (addr <= curr_heap_end) {
		mm->brk = addr;
		return 0;
	}

	new_heap_end = align_up(addr, PAGE_SIZE);
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
		uint64_t pa = page_to_phys(pg);
		err = uvmap(mm->pgd, addr, PAGE_SIZE, pa, PTE_R | PTE_W);
		if (err) {
			assert(pg);
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
	for (uint64_t a = curr_heap_end; a < addr; a += PAGE_SIZE)
		uvunmap(mm->pgd, a, PAGE_SIZE);
	for (size_t j = old_npgs; j < i; ++j) {
		assert(new_pgs[j]);
		page_free(new_pgs[j], 0);
	}
	kfree(new_pgs);
	return err;
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
	child->cwd = dentry_dup(parent->cwd);

	child->tf.a0 = 0;

	spinlock_acquire(&wait_lock);
	child->parent = parent;
	list_add(&child->child, &parent->children);
	spinlock_release(&wait_lock);

	spinlock_acquire(&child->lock);
	cpid = child->pid;
	child->state = PROCESS_STATE_RUNNING;
	child->ctx.ra = (uint64_t)proc_fork_return;
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
