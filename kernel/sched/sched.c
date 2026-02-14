#include <aosd/cpu.h>
#include <aosd/list.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/riscv.h>
#include <aosd/sched/sched.h>

static LIST_HEAD(ready_queue);

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
			sched_join(next);
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
