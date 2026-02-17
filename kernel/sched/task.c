#include <aosd/errno.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
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

static uint64_t kstack_alloc(void)
{
	struct page *page = page_alloc(KSTACK_PAGE_ORDER);
	if (!page)
		return 0;
	return page_to_virt(page);
}

static void kstack_free(uint64_t stack)
{
	page_free(virt_to_page(stack), KSTACK_PAGE_ORDER);
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
	struct task *task;

	task = task_alloc();
	if (!task)
		return NULL;

	task->pid = pid_alloc();
	if (task->pid < 0)
		goto pid_alloc_failed;

	task->stack = kstack_alloc();
	if (!task->stack)
		goto kstack_alloc_failed;

	task->pgd = create_user_pgtable();
	if (!task->pgd)
		goto create_pgtable_failed;

	return task;

create_pgtable_failed:
	kstack_free(task->stack);
kstack_alloc_failed:
	pid_free(task->pid);
pid_alloc_failed:
	task_free(task);
	return NULL;
}

void task_destroy(struct task *task)
{
	destroy_user_pgtable(task->pgd);
	kstack_free(task->stack);
	pid_free(task->pid);
	task_free(task);
}
