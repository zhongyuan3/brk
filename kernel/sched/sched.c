#include <aosd/cpu.h>
#include <aosd/list.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/riscv.h>
#include <aosd/sched.h>

static LIST_HEAD(ready_queue);
static LIST_HEAD(sleep_queue);

static struct task *pick_next_task(void)
{
	struct task *next = NULL;

	if (!list_empty(&ready_queue)) {
		next = list_first_entry(&ready_queue, struct task, list);
		list_del(&next->list);
	}

	return next;
}

void sched_join(struct task *task)
{
	list_add_tail(&task->list, &ready_queue);
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
		list_add_tail(&task->list, &ready_queue);
		break;
	case TASK_SLEEPING:
		list_add_tail(&task->list, &sleep_queue);
		break;
	default:
		panic("%s(): unknown task state %d\n", __func__, task->state);
	}
}

void start_scheduler(void)
{
	struct task *next = NULL;
	struct cpu *cpu = my_cpu();

	while (1) {
		enable_int();

		next = pick_next_task();
		if (next) {
			switch_pgdir(next->pgd);
			cpu->current_task = next;
			switch_context(&cpu->ctx, &next->ctx);
			cpu->current_task = NULL;
			add_to_queue(next);
		} else {
			enable_int();
			asm volatile("wfi");
		}
	}
}

void sched_yield(void)
{
	struct cpu *cpu = my_cpu();
	struct task *current = cpu->current_task;
	if (!current)
		panic("%s(): no current task\n", __func__);
	switch_context(&current->ctx, &cpu->ctx);
}

void sched_sleep(void *chan)
{
	struct task *current = my_cpu()->current_task;
	current->state = TASK_SLEEPING;
	current->chan = chan;
	sched_yield();
}

void sched_wake_up(void *chan)
{
	struct task *curr, *next;

	list_for_each_entry_safe(curr, next, &sleep_queue, list) {
		if (curr->chan == chan) {
			curr->state = TASK_RUNNING;
			list_del(&curr->list);
			list_add_tail(&curr->list, &ready_queue);
		}
	}
}
