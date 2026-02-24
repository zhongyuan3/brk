#include <aosd/assert.h>
#include <aosd/cpu.h>
#include <aosd/errno.h>
#include <aosd/initcode.h>
#include <aosd/list.h>
#include <aosd/macros.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/riscv.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
#include <aosd/slab.h>
#include <aosd/spinlock.h>
#include <aosd/string.h>
#include <aosd/trap.h>
#include <aosd/vmalloc.h>

struct task *init_task;
struct task tasks[NR_TASKS];

static pid_t pid_alloc(void)
{
	static spinlock_define(pid_lock);
	static pid_t pid = 0;

	pid_t ret;

	spinlock_acquire(&pid_lock);
	ret = pid++;
	spinlock_release(&pid_lock);
	return ret;
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

struct task *task_create(void)
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

	task->pgd = create_user_pgtable();
	if (!task->pgd)
		goto create_pgtable_failed;

	task->chan = NULL;
	task->parent = NULL;
	task->exit_status = 0;
	task->cpu = NULL;
	task->time_slice = DEFAULT_TIME_SLICE;
	task->thread_entry = NULL;
	task->killed = false;
	task->ctx.ra = (uint64_t)fork_return;
	task->ctx.sp = task->stack + KSTACK_SIZE;

	return task;

create_pgtable_failed:
	kstack_free(task->stack);
kstack_alloc_failed:
	pid_free(task->pid);
pid_alloc_failed:
	task->state = TASK_UNUSED;
	spinlock_release(&task->lock);
	return NULL;
}

void task_destroy(struct task *task)
{
	destroy_user_pgtable(task->pgd);
	kstack_free(task->stack);
	pid_free(task->pid);
	task->state = TASK_UNUSED;
	spinlock_release(&task->lock);
}

static void init_task_entry(void)
{
	int status;
	pid_t pid;
	int err;

	while (1) {
		status = 0;
		pid = -1;
		err = sched_wait(&status, &pid);
		if (!err)
			printk("init: task %ld exit with status %d\n", pid,
			       status);
	}
}

void sched_init(void)
{
	init_task = task_create();
	init_task->thread_entry = init_task_entry;
	init_task->state = TASK_RUNNABLE;
	spinlock_release(&init_task->lock);

	struct task *uinit = task_create();
	void *mem = kzalloc(PAGE_SIZE);
	memcpy(mem, user_initcode, user_initcode_len);
	uint64_t paddr = virt_to_phys((uint64_t)mem);
	uvmap(uinit->pgd, 0, PAGE_SIZE, paddr, PTE_R | PTE_W | PTE_X);
	uinit->tf.epc = 0;
	uinit->tf.sp = PAGE_SIZE;
	uinit->state = TASK_RUNNABLE;
	printk("user_init_task: pid=%ld\n", uinit->pid);
	spinlock_release(&uinit->lock);
}

void show_all_tasks(void)
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
