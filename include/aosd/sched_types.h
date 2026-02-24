#ifndef AOSD_SCHED_TYPES_H
#define AOSD_SCHED_TYPES_H

#include <aosd/asm.h>
#include <aosd/lock.h>
#include <aosd/mm_types.h>
#include <aosd/types.h>

#define KSTACK_PAGE_ORDER 1
#define KSTACK_SIZE (PAGE_SIZE * (1 << KSTACK_PAGE_ORDER))

struct cpu;

struct trapframe {
	uint64_t kernel_sp;
	uint64_t ra;
	uint64_t sp;
	uint64_t gp;
	uint64_t tp;
	uint64_t t0;
	uint64_t t1;
	uint64_t t2;
	uint64_t s0;
	uint64_t s1;
	uint64_t a0;
	uint64_t a1;
	uint64_t a2;
	uint64_t a3;
	uint64_t a4;
	uint64_t a5;
	uint64_t a6;
	uint64_t a7;
	uint64_t s2;
	uint64_t s3;
	uint64_t s4;
	uint64_t s5;
	uint64_t s6;
	uint64_t s7;
	uint64_t s8;
	uint64_t s9;
	uint64_t s10;
	uint64_t s11;
	uint64_t t3;
	uint64_t t4;
	uint64_t t5;
	uint64_t t6;
	uint64_t epc;
};

struct context {
	uint64_t ra;
	uint64_t sp;
	uint64_t s0;
	uint64_t s1;
	uint64_t s2;
	uint64_t s3;
	uint64_t s4;
	uint64_t s5;
	uint64_t s6;
	uint64_t s7;
	uint64_t s8;
	uint64_t s9;
	uint64_t s10;
	uint64_t s11;
};

typedef long pid_t;

typedef enum {
	TASK_UNUSED,
	TASK_USED,
	TASK_RUNNABLE,
	TASK_RUNNING,
	TASK_SLEEPING,
	TASK_ZOMBIE,
} task_state_t;

struct task {
	struct trapframe tf;
	uint64_t stack;
	struct context ctx;
	pid_t pid;
	pgde_t *pgd;
	task_state_t state;
	void *chan;
	struct task *parent;
	int exit_status;
	struct cpu *cpu;
	int time_slice;
	void (*thread_entry)(void);
	spinlock_t lock;
	bool killed;
};

#endif
