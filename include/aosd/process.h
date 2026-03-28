#ifndef AOSD_PROCESS_H
#define AOSD_PROCESS_H

#include <aosd/asm.h>
#include <aosd/limits.h>
#include <aosd/lock.h>
#include <aosd/mm_types.h>
#include <aosd/types.h>
#include <uapi/aosd/resource.h>
#include <uapi/aosd/time.h>

#define KSTACK_PAGE_ORDER 1
#define KSTACK_SIZE (PAGE_SIZE * (1 << KSTACK_PAGE_ORDER))
#define USTACK_PAGE_ORDER 4
#define USTACK_SIZE (PAGE_SIZE * (1 << USTACK_PAGE_ORDER))

#define TIME_SLICE_MAX 5
#define PROCESS_NAME_MAX 32

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
	/* 264 */ uint64_t cpuid;
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
	PROCESS_STATE_NEW,
	PROCESS_STATE_RUNNING,
	PROCESS_STATE_SLEEPING,
	PROCESS_STATE_ZOMBIE,
};

struct process {
	struct list_head list;

	struct list_head queue;

	struct process *parent;
	struct list_head children;
	struct list_head child;

	spinlock_t lock;
	void *chan;
	struct context ctx;
	pid_t pid;
	enum process_state state;
	int exit_status;
	bool killed;

	struct mm_struct *mm;
	struct file *ofiles[OPEN_MAX];
	struct dentry *cwd;
	struct tms ptms;
	struct trapframe tf;
	uint64_t kstack;
	uint64_t ktime;
	uint64_t utime;
	int time_slice;
	char name[PROCESS_NAME_MAX];
	bool irq_enabled;
};

struct cpu {
	struct process *current;
	struct process *handoff;
	struct context ctx;
	int irq_nest;
	bool irq_enabled;
};

void proc_init_user(void);

void proc_cache_init(void);
struct process *proc_alloc(void);
void proc_free(struct process *proc);
void proc_set_killed(struct process *proc);
bool proc_is_killed(struct process *proc);
int proc_fork(void);
int proc_set_brk(uint64_t addr);
int proc_alloc_fd(struct process *proc, struct file *fp);
void proc_dump(void);

void proc_yield(void);
void proc_sleep(void *chan, spinlock_t *lock);
void proc_wake_up(void *chan);
void proc_exit(int status);
pid_t proc_wait(pid_t cpid, int *status, int options, struct rusage *rus);
void proc_sched(void);
void proc_sched_resume(void);
void proc_scheduler(void);
void proc_join(struct process *proc);

pid_t pid_alloc(void);
void pid_free(pid_t pid);

void push_off(void);
void pop_off(void);

struct process *current_process(void);
struct cpu *current_cpu(void);
cpuid_t current_cpuid(void);

void switch_context(struct context *prev, struct context *next);

extern cpuid_t init_cpuid;
extern struct process *init_proc;
extern struct cpu cpus[NR_CPUS];
extern spinlock_t wait_lock;

#endif
