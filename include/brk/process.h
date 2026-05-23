#ifndef BRK_PROCESS_H
#define BRK_PROCESS_H

#include <brk/asm.h>
#include <brk/fs_types.h>
#include <brk/lock.h>
#include <brk/mm_types.h>
#include <brk/path.h>
#include <brk/types.h>
#include <uapi/brk/limits.h>
#include <uapi/resource.h>
#include <uapi/times.h>

#define KSTACK_PAGE_ORDER 1
#define KSTACK_SIZE (PAGE_SIZE * (1 << KSTACK_PAGE_ORDER))
#define USTACK_PAGE_ORDER 4
#define USTACK_SIZE (PAGE_SIZE * (1 << USTACK_PAGE_ORDER))

#define TIME_SLICE_MAX 5
#define PROCESS_NAME_MAX 32

struct trap_frame {
	/* 0   */ u64 kernel_sp;
	/* 8   */ u64 ra;
	/* 16  */ u64 sp;
	/* 24  */ u64 gp;
	/* 32  */ u64 tp;
	/* 40  */ u64 t0;
	/* 48  */ u64 t1;
	/* 56  */ u64 t2;
	/* 64  */ u64 s0;
	/* 72  */ u64 s1;
	/* 80  */ u64 a0;
	/* 88  */ u64 a1;
	/* 96  */ u64 a2;
	/* 104 */ u64 a3;
	/* 112 */ u64 a4;
	/* 120 */ u64 a5;
	/* 128 */ u64 a6;
	/* 136 */ u64 a7;
	/* 144 */ u64 s2;
	/* 152 */ u64 s3;
	/* 160 */ u64 s4;
	/* 168 */ u64 s5;
	/* 176 */ u64 s6;
	/* 184 */ u64 s7;
	/* 192 */ u64 s8;
	/* 200 */ u64 s9;
	/* 208 */ u64 s10;
	/* 216 */ u64 s11;
	/* 224 */ u64 t3;
	/* 232 */ u64 t4;
	/* 240 */ u64 t5;
	/* 248 */ u64 t6;
	/* 256 */ u64 epc;
	/* 264 */ u64 cpuid;
};

struct switch_frame {
	u64 ra;
	u64 sp;
	u64 s0;
	u64 s1;
	u64 s2;
	u64 s3;
	u64 s4;
	u64 s5;
	u64 s6;
	u64 s7;
	u64 s8;
	u64 s9;
	u64 s10;
	u64 s11;
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
	struct switch_frame ctx;
	pid_t pid;
	enum process_state state;
	int exit_status;
	int pending_sig;

	struct uvm_space *mm;
	struct file *ofiles[OPEN_MAX];
	struct path cwd;
	struct path root;
	struct tms ptms;
	struct trap_frame tf;
	u64 kstack;
	u64 ktime;
	u64 utime;
	int time_slice;
	char name[PROCESS_NAME_MAX];
	bool irq_enabled;
};

struct cpu {
	struct process *handoff;
	struct switch_frame ctx;
	int irq_nest;
	bool irq_enabled;
};

void proc_init_user(void);

void proc_cache_init(void);
struct process *proc_alloc(void);
void proc_free(struct process *proc);
void proc_set_killed(struct process *proc);
bool proc_is_killed(struct process *proc);
void proc_exit(int status);
void proc_exit_normal(int code);
void proc_exit_signal(int sig);
int proc_fork(void);
int proc_set_brk(u64 addr);
int proc_alloc_fd(struct process *proc, struct file *fp);
void proc_dump(void);

void proc_yield(void);
void proc_sleep(void *chan, spinlock_t *lock);
void proc_wake_up(void *chan);
void proc_wake_all(void *chan);
pid_t proc_wait(pid_t cpid, int *status, int options, struct rusage *rus);
void proc_wake_process(struct process *proc);
void proc_sched(void);
void proc_sched_resume(void);
void proc_scheduler(void);
void proc_join(struct process *proc);

pid_t pid_alloc(void);
void pid_free(pid_t pid);

/*
 * Read-only snapshot of the parts of a process that are safe to look at
 * from outside the scheduler / proc_lock.
 */
struct process_info {
	pid_t pid;
	pid_t ppid;
	enum process_state state;
	int exit_status;
	bool killed;
	u64 utime;
	u64 ktime;
	u64 brk;
	char name[PROCESS_NAME_MAX];
};

/*
 * Copy every live pid into @out (up to @max entries). Returns the number
 * of pids written. The order is the global creation order maintained by
 * proc_alloc().
 */
int proc_snapshot_pids(pid_t *out, int max);

/*
 * Snapshot a single process's fields into @info. Returns true on
 * success, false if no such pid exists.
 */
bool proc_get_info(pid_t pid, struct process_info *info);

/* Lightweight existence check. */
bool proc_pid_exists(pid_t pid);

void push_off(void);
void pop_off(void);

struct process *current_process(void);
struct cpu *current_cpu(void);
cpuid_t current_cpuid(void);
void set_current_process(struct process *proc);
void set_current_cpuid(cpuid_t cpuid);

void switch_context(struct switch_frame *prev, struct switch_frame *next);

extern cpuid_t init_cpuid;
extern struct process *init_proc;
extern struct list_head procs;
extern spinlock_t procs_lock;
extern struct cpu cpus[NR_CPUS];
extern spinlock_t wait_lock;

#endif
