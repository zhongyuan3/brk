#include <aosd/cpu.h>
#include <aosd/errno.h>
#include <aosd/list.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/printk.h>
#include <aosd/riscv.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
#include <aosd/types.h>

static LIST_HEAD(running_queue);
static LIST_HEAD(sleep_queue);

static struct task *pick_next_task(void)
{
	struct task *next = NULL;

	if (!list_empty(&running_queue)) {
		next = list_first_entry(&running_queue, struct task, list);
		list_del(&next->list);
	}

	return next;
}

void sched_join(struct task *task)
{
	list_add_tail(&task->list, &running_queue);
}

static void switch_pgdir(pgde_t *pgd)
{
	uint64_t paddr = virt_to_phys((uint64_t)pgd);
	uint64_t satp = make_satp_sv39(paddr);
	sfence_vma();
	write_satp(satp);
	sfence_vma();
}

static void add_to_queue(struct task *task)
{
	switch (task->state) {
	case TASK_RUNNING:
		list_add_tail(&task->list, &running_queue);
		break;
	case TASK_SLEEPING:
		list_add_tail(&task->list, &sleep_queue);
		break;
	case TASK_ZOMBIE:
		/* Do nothing, just wait for parent to wait */
		break;
	default:
		panic("%s(): unknown task state %d\n", __func__, task->state);
	}
}

void start_scheduling(void)
{
	struct task *next = NULL;
	struct cpu *cpu = current_cpu();

	while (1) {
		next = pick_next_task();
		if (next)
			break;
		enable_int();
		asm volatile("wfi");
	}

	cpu->current = next;
	next->cpu = cpu;
	switch_pgdir(next->pgd);
	switch_to(&next->ctx);
}

void schedule(void)
{
	struct task *prev, *next;
	struct cpu *cpu;

	enable_int();
	disable_int();

	cpu = current_cpu();
	prev = cpu->current;
	cpu->current = NULL;
	add_to_queue(prev);

	next = pick_next_task();
	cpu->current = next;
	next->cpu = cpu;
	switch_pgdir(next->pgd);
	switch_context(&prev->ctx, &next->ctx);

	prev->time_slice = 5;
	cpu = current_cpu();
	cpu->current = prev;
	prev->cpu = cpu;
	switch_pgdir(prev->pgd);
}

void sched_yield(void)
{
	schedule();
}

void sched_sleep(void *chan)
{
	struct task *current = current_cpu()->current;
	current->state = TASK_SLEEPING;
	current->chan = chan;
	schedule();
}

void sched_wake_up(void *chan)
{
	struct task *curr, *next;

	list_for_each_entry_safe(curr, next, &sleep_queue, list) {
		if (curr->chan == chan) {
			curr->state = TASK_RUNNING;
			list_del(&curr->list);
			list_add_tail(&curr->list, &running_queue);
		}
	}
}

static void move_children_to_init_task(struct task *parent)
{
	struct task *curr, *next;

	list_for_each_entry_safe(curr, next, &parent->children, child_list) {
		list_del(&curr->child_list);
		curr->parent = init_task;
		list_add_tail(&curr->list, &init_task->children);
	}
}

void sched_exit(int status)
{
	struct task *current = current_cpu()->current;
	if (current == init_task)
		panic("%s(): init task exit\n", __func__);

	move_children_to_init_task(current);

	current->state = TASK_ZOMBIE;
	current->exit_status = status;

	sched_wake_up(current->parent);

	schedule();

	panic("scheduling zombie task\n");
}

pid_t sched_wait(int *status)
{
	struct task *curr, *next;
	struct task *parent = current_cpu()->current;

	if (list_empty(&parent->children))
		return -ECHILD;

again:
	list_for_each_entry_safe(curr, next, &parent->children, child_list) {
		if (curr->state == TASK_ZOMBIE) {
			list_del(&curr->child_list);
			if (status)
				*status = curr->exit_status;
			task_destroy(curr);
			return curr->pid;
		}
	}

	sched_sleep(parent);

	goto again;
}
