#include <aosd/errno.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/sched/sched.h>
#include <aosd/slab.h>

static struct kmem_cache task_cache;

static pid_t pid_alloc(void)
{
	static pid_t pid = 0;
	return pid++;
}

static void pid_free(pid_t pid)
{
}

static int kstack_alloc(uint64_t *base, uint64_t *top)
{
	struct page *page = page_alloc(KSTACK_PAGE_ORDER);
	if (!page)
		return -ENOMEM;
	uint64_t kstack_base = page_to_virt(page);
	uint64_t kstack_top = kstack_base + KSTACK_SIZE;
	if (!is_aligned(kstack_top, 16)) {
		kstack_top = align_down(kstack_top, 16);
		log_debug("kstack_top not aligned, align down to %#lx\n",
			  kstack_top);
	}
	*base = kstack_base;
	*top = kstack_top;
	return 0;
}

static void kstack_free(uint64_t base, uint64_t top)
{
	page_free(virt_to_page(base), KSTACK_PAGE_ORDER);
}

void sched_init(void)
{
	kmem_cache_init(&task_cache, sizeof(struct task), alignof(struct task),
			"task");
}

static struct task *task_alloc(void)
{
	return kmem_cache_alloc(&task_cache);
}

static void task_free(struct task *task)
{
	kmem_cache_free(&task_cache, task);
}

struct task *task_create(void)
{
	int err = 0;
	struct task *task = NULL;

	task = task_alloc();
	if (!task)
		return NULL;

	task->pid = pid_alloc();
	if (task->pid < 0)
		goto pid_alloc_failed;

	err = kstack_alloc(&task->kstack_base, &task->kstack_top);
	if (err)
		goto kstack_alloc_failed;

	log_debug("task %p created, kstack_base=%#lx, kstack_top=%#lx\n", task,
		  task->kstack_base, task->kstack_top);

	task->pgd = create_user_pgtable();
	if (!task->pgd)
		goto create_pgtable_failed;

	return task;

create_pgtable_failed:
	kstack_free(task->kstack_base, task->kstack_top);
kstack_alloc_failed:
	pid_free(task->pid);
pid_alloc_failed:
	task_free(task);
	return NULL;
}

void task_destroy(struct task *task)
{
	destroy_user_pgtable(task->pgd);
	kstack_free(task->kstack_base, task->kstack_top);
	pid_free(task->pid);
	task_free(task);
}
