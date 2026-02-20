#include <aosd/assert.h>
#include <aosd/cpu.h>
#include <aosd/errno.h>
#include <aosd/initcode.h>
#include <aosd/list.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/riscv.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/trap.h>
#include <aosd/vmalloc.h>

static struct kmem_cache task_cache;
struct task *init_task;

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

static void user_init_task_entry(void)
{
	prepare_to_return();
	struct cpu *cpu = current_cpu();
	user_trap_return(cpu->current);
}

static void setup_user_init_task(void)
{
	struct task *task;
	uint64_t paddr;
	void *mem;

	task = task_create();
	mem = kzalloc(PAGE_SIZE);
	memcpy(mem, user_initcode, user_initcode_len);

	paddr = virt_to_phys((uint64_t)mem);
	uvmap(task->pgd, 0, PAGE_SIZE, paddr, PTE_R | PTE_W | PTE_X);
	task->ctx.sp = task->stack + KSTACK_SIZE;
	task->ctx.ra = (uint64_t)user_init_task_entry;
	task->tf.epc = 0;
	task->tf.sp = PAGE_SIZE;

	sched_join(task);
}

static void init_task_entry(void)
{
	volatile size_t i = 0;
	while (1) {
		if (i % 50000000 == 0)
			printk("init: %zu\n", i);
		++i;
	}
}

static void setup_init_task(void)
{
	init_task = task_create();
	init_task->ctx.sp = init_task->stack + KSTACK_SIZE;
	init_task->ctx.ra = (uint64_t)init_task_entry;
	sched_join(init_task);
}

void sched_init(void)
{
	kmem_cache_init(&task_cache, sizeof(struct task), alignof(struct task),
			"task");
	setup_init_task();
	setup_user_init_task();
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

	task->state = TASK_RUNNING;
	task->chan = NULL;
	task->parent = NULL;
	list_init_head(&task->list);
	list_init_head(&task->children);
	list_init_head(&task->child_list);
	task->exit_status = 0;
	task->cpu = NULL;
	task->time_slice = 5;

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
