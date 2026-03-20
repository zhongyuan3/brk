#ifndef AOSD_PROCESS_H
#define AOSD_PROCESS_H

#include <aosd/asm.h>
#include <aosd/limits.h>
#include <aosd/lock.h>
#include <aosd/mm_types.h>
#include <aosd/types.h>
#include <uapi/aosd/resource.h>
#include <uapi/aosd/time.h>

#define TASK_TIME_SLICE 5
#define NR_TASKS 128

#define KSTACK_PAGE_ORDER 1
#define KSTACK_SIZE (PAGE_SIZE * (1 << KSTACK_PAGE_ORDER))
#define USTACK_PAGE_ORDER 4
#define USTACK_SIZE (PAGE_SIZE * (1 << USTACK_PAGE_ORDER))
#define TASK_NAME_MAX 64

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
	TASK_STATE_UNUSED,
	TASK_STATE_USED,
	TASK_STATE_RUNNABLE,
	TASK_STATE_RUNNING,
	TASK_STATE_SLEEPING,
	TASK_STATE_ZOMBIE,
};

struct task_struct {
	/* The following fields are protected by wait_lock: */
	struct task_struct *parent; /* Parent process */

	spinlock_t lock;
	/* The following fields are protected by task_struct::lock: */
	void *chan; /* If non-NULL, sleeping on chan */
	pid_t pid;
	enum task_state state;
	int exit_status;
	bool killed;

	/* The following fields are private fields: */
	struct mem_mgmt *mm; /* Memory management structure */
	struct file *ofiles[OPEN_MAX];
	struct dentry *cwd;
	struct tms ptms;
	struct trapframe tf;
	struct context ctx;
	uint64_t kstack;
	uint64_t ktime;
	uint64_t utime;
	processor_id_t on_proc;
	int time_slice;
	char name[TASK_NAME_MAX];
};

struct processor {
	struct task_struct *task;
	struct context ctx;
	int irq_nest;
	bool irq_enabled;
};

void task_init(void);
struct task_struct *task_alloc(void);
void task_free(struct task_struct *task);

void task_set_killed(struct task_struct *task);
bool task_is_killed(struct task_struct *task);
int task_fork(void);
int task_set_brk(uint64_t addr);
int task_alloc_fd(struct task_struct *task, struct file *fp);
void task_dump(void);

void task_yield(void);
void task_sleep(void *chan, spinlock_t *lock);
void task_wake_up(void *chan);
void task_exit(int status);
pid_t task_wait(pid_t cpid, int *status, int options, struct rusage *rus);
void scheduler(void);

struct task_struct *current_task(void);

pid_t pid_alloc(void);
void pid_free(pid_t pid);

void push_off(void);
void pop_off(void);

struct processor *current_processor(void);
processor_id_t current_processor_id(void);

extern struct task_struct tasks[NR_TASKS];
extern struct processor processors[NR_PROCESSORS];
extern struct task_struct *init_task;
extern spinlock_t wait_lock;
extern processor_id_t init_proc_id;

#endif
