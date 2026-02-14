#ifndef AOSD_SCHED_TYPES_H
#define AOSD_SCHED_TYPES_H

#include <aosd/asm.h>
#include <aosd/mm_types.h>
#include <aosd/types.h>

#define KSTACK_PAGE_ORDER 1
#define KSTACK_SIZE (PAGE_SIZE * (1 << KSTACK_PAGE_ORDER))

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

struct task {
	uint64_t kstack_base;
	uint64_t kstack_top;
	struct context ctx;
	pid_t pid;
	struct list_head list;
	pgde_t *pgd;
};

#endif
