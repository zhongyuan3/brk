#include <aosd/asm.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/limits.h>
#include <aosd/lock.h>
#include <aosd/macros.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/process.h>
#include <aosd/riscv.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/trap.h>
#include <aosd/types.h>
#include <aosd/vmalloc.h>

struct task_struct tasks[NR_TASKS];
struct processor processors[NR_PROCESSORS];
processor_id_t init_proc_id;

static void new_task_ret(void)
{
	struct task_struct *task = current_task();
	spinlock_release(&task->lock);
	prepare_to_return();
	user_trap_return(&task->tf);
}

static void init_task_ret(void)
{
	struct task_struct *task = current_task();
	spinlock_release(&task->lock);
	fs_init();
	char *argv[] = { "/bin/init", 0 };
	char *envp[] = { 0 };
	int ret = do_execve(argv[0], argv, envp);
	if (ret < 0)
		panic("execve %s failed: %s\n", argv[0], strerror(ret));
	task->tf.a0 = ret;
	prepare_to_return();
	user_trap_return(&task->tf);
}

int task_fork(void)
{
	int err;
	pid_t cpid;
	struct task_struct *child;
	struct task_struct *parent = current_task();

	child = task_alloc();
	if (!child)
		return -ENOMEM;

	err = mm_copy(child->mm, parent->mm);
	if (err) {
		task_free(child);
		return err;
	}

	memcpy(&child->tf, &parent->tf, sizeof(parent->tf));

	for (int i = 0; i < OPEN_MAX; ++i) {
		if (parent->ofiles[i])
			child->ofiles[i] = file_dup(parent->ofiles[i]);
	}
	child->cwd = dentry_dup(parent->cwd);

	child->tf.a0 = 0;

	cpid = child->pid;
	spinlock_release(&child->lock);

	spinlock_acquire(&wait_lock);
	child->parent = parent;
	spinlock_release(&wait_lock);

	spinlock_acquire(&child->lock);
	child->state = TASK_STATE_RUNNABLE;
	spinlock_release(&child->lock);

	return cpid;
}

void task_set_killed(struct task_struct *task)
{
	spinlock_acquire(&task->lock);
	task->killed = true;
	spinlock_release(&task->lock);
}

bool task_is_killed(struct task_struct *task)
{
	bool killed;
	spinlock_acquire(&task->lock);
	killed = task->killed;
	spinlock_release(&task->lock);
	return killed;
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

static struct task_struct *__task_alloc(void)
{
	struct task_struct *task;

	for (task = tasks; task < tasks + NR_TASKS; ++task) {
		spinlock_acquire(&task->lock);
		if (task->state == TASK_STATE_UNUSED) {
			task->state = TASK_STATE_USED;
			return task;
		}
		spinlock_release(&task->lock);
	}

	return NULL;
}

static void __task_free(struct task_struct *task)
{
	task->state = TASK_STATE_UNUSED;
	spinlock_release(&task->lock);
}

struct task_struct *task_alloc(void)
{
	struct task_struct *task = __task_alloc();
	if (!task)
		return NULL;

	task->pid = pid_alloc();
	if (task->pid < 0)
		goto pid_alloc_failed;

	task->kstack = kstack_alloc();
	if (!task->kstack)
		goto kstack_alloc_failed;

	task->mm = mm_alloc();
	if (!task->mm)
		goto create_mm_failed;

	task->time_slice = TASK_TIME_SLICE;
	task->ctx.ra = (uint64_t)new_task_ret;
	task->ctx.sp = task->kstack + KSTACK_SIZE;

	return task;

create_mm_failed:
	kstack_free(task->kstack);
	task->kstack = 0;
kstack_alloc_failed:
	pid_free(task->pid);
	task->pid = 0;
pid_alloc_failed:
	__task_free(task);
	return NULL;
}

void task_free(struct task_struct *task)
{
	mm_free(task->mm);
	task->mm = NULL;
	kstack_free(task->kstack);
	task->kstack = 0;
	pid_free(task->pid);
	task->pid = 0;
	__task_free(task);
}

void task_init(void)
{
	struct task_struct *task;

	for (task = tasks; task < tasks + NR_TASKS; ++task)
		spinlock_init(&task->lock, "task");

	init_task = task_alloc();
	strlcpy(init_task->name, "init", sizeof(init_task->name));
	init_task->ctx.ra = (uint64_t)init_task_ret;
	init_task->state = TASK_STATE_RUNNABLE;
	spinlock_release(&init_task->lock);
}

void task_dump(void)
{
	static char *states[] = {
		[TASK_STATE_UNUSED] = "unused",
		[TASK_STATE_USED] = "  used",
		[TASK_STATE_SLEEPING] = " sleep",
		[TASK_STATE_RUNNABLE] = "runble",
		[TASK_STATE_RUNNING] = "   run",
		[TASK_STATE_ZOMBIE] = "zombie",
	};
	struct task_struct *task;
	char *state;

	printk("\n");
	for (task = tasks; task < tasks + NR_TASKS; ++task) {
		if (task->state == TASK_STATE_UNUSED)
			continue;
		if (task->state >= 0 && task->state < countof(states) &&
		    states[task->state])
			state = states[task->state];
		else
			state = "???";
		printk("%ld %s %s", task->pid, state, task->name);
		printk("\n");
	}
}

int task_set_brk(uint64_t addr)
{
	struct task_struct *task = current_task();
	struct mem_mgmt *mm = task->mm;
	struct vmem_area *heap = mm->heap;
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

int task_alloc_fd(struct task_struct *task, struct file *fp)
{
	for (int i = 0; i < OPEN_MAX; ++i) {
		if (!task->ofiles[i]) {
			task->ofiles[i] = fp;
			return i;
		}
	}

	return -1;
}

void push_off(void)
{
	bool irq_enabled = intr_off_get();
	struct processor *proc = current_processor();
	if (proc->irq_nest == 0)
		proc->irq_enabled = irq_enabled;
	proc->irq_nest += 1;
}

void pop_off(void)
{
	struct processor *proc;
	if (intr_enabled())
		panic("%s(): interruptible\n", __func__);
	proc = current_processor();
	if (proc->irq_nest < 1)
		panic("%s(): nesting level 0\n", __func__);
	proc->irq_nest -= 1;
	if (proc->irq_nest == 0 && proc->irq_enabled)
		intr_on();
}

struct processor *current_processor(void)
{
	return &processors[current_processor_id()];
}

processor_id_t current_processor_id(void)
{
	return read_tp();
}
