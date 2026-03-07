#ifndef AOSD_SCHED_TYPES_H
#define AOSD_SCHED_TYPES_H

#include <aosd/asm.h>
#include <aosd/limits.h>
#include <aosd/lock.h>
#include <aosd/mm_types.h>
#include <aosd/types.h>
#include <uapi/aosd/time.h>

#define KSTACK_PAGE_ORDER 1
#define KSTACK_SIZE (PAGE_SIZE * (1 << KSTACK_PAGE_ORDER))
#define USTACK_PAGE_ORDER 4
#define USTACK_SIZE (PAGE_SIZE * (1 << USTACK_PAGE_ORDER))

struct cpu;
struct file;
struct dentry;

struct trapframe {
	/* 0   */ uint64_t kernel_sp;
	/* 8   */ uint64_t ra;
	/* 16  */ uint64_t sp;
	/* 24  */ uint64_t gp;
	/* 32  */ uint64_t tp;
	/* 40  */ uint64_t t0;
	/* 48  */ uint64_t t1;
	/* 56  */ uint64_t t2;
	/* 64  */ uint64_t s0;
	/* 72  */ uint64_t s1;
	/* 80  */ uint64_t a0;
	/* 88  */ uint64_t a1;
	/* 96  */ uint64_t a2;
	/* 104 */ uint64_t a3;
	/* 112 */ uint64_t a4;
	/* 120 */ uint64_t a5;
	/* 128 */ uint64_t a6;
	/* 136 */ uint64_t a7;
	/* 144 */ uint64_t s2;
	/* 152 */ uint64_t s3;
	/* 160 */ uint64_t s4;
	/* 168 */ uint64_t s5;
	/* 176 */ uint64_t s6;
	/* 184 */ uint64_t s7;
	/* 192 */ uint64_t s8;
	/* 200 */ uint64_t s9;
	/* 208 */ uint64_t s10;
	/* 216 */ uint64_t s11;
	/* 224 */ uint64_t t3;
	/* 232 */ uint64_t t4;
	/* 240 */ uint64_t t5;
	/* 248 */ uint64_t t6;
	/* 256 */ uint64_t epc;
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

enum task_state {
	TASK_UNUSED,
	TASK_USED,
	TASK_RUNNABLE,
	TASK_RUNNING,
	TASK_SLEEPING,
	TASK_ZOMBIE,
};

struct task {
	struct trapframe tf;
	uint64_t stack;
	struct context ctx;
	pid_t pid;
	enum task_state state;
	void *chan;
	struct task *parent;
	int exit_status;
	struct cpu *cpu;
	int time_slice;
	spinlock_t lock;
	bool killed;
	struct file *ofiles[OPEN_MAX + 1];
	struct dentry *cwd;
	struct mem_mgmt *mm;
	struct tms proc_tms;
	uint64_t last_ktime;
	uint64_t last_utime;
};

#endif
