#ifndef AOSD_PROCESS_TYPES_H
#define AOSD_PROCESS_TYPES_H

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
#define PROCESS_NAME_MAX 64

struct cpu;
struct file;
struct dentry;
struct process;

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

enum process_state {
	PROCESS_UNUSED,
	PROCESS_USED,
	PROCESS_RUNNABLE,
	PROCESS_RUNNING,
	PROCESS_SLEEPING,
	PROCESS_ZOMBIE,
};

struct process {
	/* Protected by wait_lock */
	struct process *parent; /* Parent process */

	spinlock_t lock;
	/* The following fields are protected by a lock: */
	void *chan; /* If non-NULL, sleeping on chan */
	pid_t pid;
	enum process_state state;
	int exit_status;
	bool killed;

	/* The following fields are private fields: */
	struct mem_mgmt *mm; /* Memory management structure */
	struct cpu *cpu;
	struct file *ofiles[OPEN_MAX];
	struct dentry *cwd;
	struct tms proc_tms;
	struct trapframe tf;
	struct context ctx;
	uint64_t kstack;
	uint64_t last_ktime;
	uint64_t last_utime;
	int time_slice;
	char name[PROCESS_NAME_MAX];
};

#endif
