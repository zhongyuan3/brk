#include <aosd/asm.h>
#include <aosd/assert.h>
#include <aosd/cpu.h>
#include <aosd/errno.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/macros.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/riscv.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/timer.h>
#include <aosd/trap.h>
#include <aosd/vmalloc.h>

struct task *init_task;
struct task tasks[NR_TASKS];

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

struct task *task_alloc(void)
{
	struct task *task;

	for (task = tasks; task < tasks + NR_TASKS; ++task) {
		spinlock_acquire(&task->lock);
		if (task->state == TASK_UNUSED) {
			task->state = TASK_USED;
			goto found;
		}
		spinlock_release(&task->lock);
	}

	return NULL;

found:
	task->pid = pid_alloc();
	if (task->pid < 0)
		goto pid_alloc_failed;

	task->stack = kstack_alloc();
	if (!task->stack)
		goto kstack_alloc_failed;

	task->mm = mm_alloc();
	if (!task->mm)
		goto create_mm_failed;

	task->chan = NULL;
	task->parent = NULL;
	task->exit_status = 0;
	task->cpu = NULL;
	task->time_slice = DEFAULT_TIME_SLICE;
	task->killed = false;
	task->ctx.ra = (uint64_t)fork_return;
	task->ctx.sp = task->stack + KSTACK_SIZE;
	memset(&task->proc_tms, 0, sizeof(task->proc_tms));
	task->last_ktime = 0;
	task->last_utime = 0;

	return task;

create_mm_failed:
	kstack_free(task->stack);
kstack_alloc_failed:
	pid_free(task->pid);
pid_alloc_failed:
	task->state = TASK_UNUSED;
	spinlock_release(&task->lock);
	return NULL;
}

void task_free(struct task *task)
{
	mm_free(task->mm);
	kstack_free(task->stack);
	pid_free(task->pid);
	memset(&task->proc_tms, 0, sizeof(task->proc_tms));
	task->last_ktime = 0;
	task->last_utime = 0;
	task->state = TASK_UNUSED;
	spinlock_release(&task->lock);
}

void init_task_entry(void)
{
	intr_on();
	while (1) {
		int status = 0;
		pid_t pid = do_wait4(-1, &status, 0, 0);
		if (pid >= 0)
			printk("init: task %ld exit with status %d\n", pid,
			       status);
	}
}

void sched_init(void)
{
	init_task = task_alloc();
	init_task->state = TASK_RUNNABLE;
	spinlock_release(&init_task->lock);

	struct task *uinit = task_alloc();
	uinit->parent = init_task;
	uinit->state = TASK_RUNNABLE;
	spinlock_release(&uinit->lock);
}

void task_dump(void)
{
	static char *states[] = {
		[TASK_UNUSED] = "unused",  [TASK_USED] = "used",
		[TASK_SLEEPING] = "sleep", [TASK_RUNNABLE] = "runble",
		[TASK_RUNNING] = "run",	   [TASK_ZOMBIE] = "zombie"
	};
	struct task *t;
	char *state;

	printk("\n");
	for (t = tasks; t < tasks + NR_TASKS; ++t) {
		if (t->state == TASK_UNUSED)
			continue;
		if (t->state >= 0 && t->state < countof(states) &&
		    states[t->state])
			state = states[t->state];
		else
			state = "???";
		printk("%ld %s", t->pid, state);
		printk("\n");
	}
}

uint64_t make_user_stack(void)
{
	uint64_t ustack_top = TASK_SIZE_MAX;
	timer_srand();
	ustack_top -= (timer_rand() % 32 + 1) * PAGE_SIZE;
	ustack_top -= USTACK_SIZE;
	return ustack_top;
}

int task_set_brk(uint64_t addr)
{
	struct task *t = current_task();
	struct mem_mgmt *mm = t->mm;
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

int task_alloc_fd(struct task *t, struct file *f)
{
	int fd = 0;

	for (; fd <= OPEN_MAX; ++fd) {
		if (!t->ofiles[fd]) {
			t->ofiles[fd] = f;
			return fd;
		}
	}

	return -1;
}
