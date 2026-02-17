#include <aosd/cpu.h>
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
	default:
		panic("%s(): unknown task state %d\n", __func__, task->state);
	}
}

void schedule(void)
{
	struct task *prev, *next;
	struct cpu *cpu;

	enable_int();

	cpu = my_cpu();
	prev = cpu->current_task;
	add_to_queue(prev);

	next = pick_next_task();
	cpu->current_task = next;
	switch_pgdir(next->pgd);

	switch_context(&prev->ctx, &next->ctx);

	cpu = my_cpu();
	cpu->current_task = prev;
	switch_pgdir(prev->pgd);
}

void sched_yield(void)
{
	schedule();
}

void sched_sleep(void *chan)
{
	struct task *current = my_cpu()->current_task;
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

void start_scheduling(void)
{
	struct task *next = NULL;
	struct cpu *cpu = my_cpu();
	while (1) {
		next = pick_next_task();
		if (next)
			break;
		enable_int();
		asm volatile("wfi");
	}
	cpu->current_task = next;
	switch_pgdir(next->pgd);
	switch_to(&next->ctx);
}
