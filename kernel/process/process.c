#include <aosd/asm.h>
#include <aosd/cpu.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/limits.h>
#include <aosd/lock.h>
#include <aosd/macros.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/process.h>
#include <aosd/process_types.h>
#include <aosd/riscv.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/timer.h>
#include <aosd/trap.h>
#include <aosd/types.h>
#include <aosd/vmalloc.h>

static SPINLOCK_DEFINE(wait_lock);
static struct process *init;
static struct process processes[NR_PROCESSES];

extern void switch_context(struct context *prev, struct context *next);

void proc_scheduler(void)
{
	struct cpu *c = current_cpu();
	struct process *next = NULL;
	bool have_runnable = false;
	uint64_t jiffies = 0;

	for (;;) {
		intr_on();
		intr_off();

		have_runnable = false;

		for (next = processes; next < processes + NR_PROCESSES;
		     ++next) {
			spinlock_acquire(&next->lock);
			if (next->state == PROCESS_RUNNABLE) {
				c->current = next;
				next->cpu = c;
				next->state = PROCESS_RUNNING;
				next->time_slice = PROCESS_TIME_SLICE;
				switch_pgtable(next->mm->pgd);
				write_sstatus(read_sstatus() | SSTATUS_SUM);
				jiffies = jiffies_get();
				next->last_ktime = jiffies;
				switch_context(&c->ctx, &next->ctx);
				c->current = NULL;
				next->cpu = NULL;
				jiffies = jiffies_get();
				next->proc_tms.tms_stime +=
					jiffies - next->last_ktime;
				have_runnable = true;
			}
			spinlock_release(&next->lock);
		}

		if (!have_runnable) {
			intr_on();
			asm volatile("wfi");
		}
	}
}

static void proc_sched(void)
{
	bool intena;
	struct process *proc;

	proc = proc_get_current();

	assert(spinlock_holding(&proc->lock));
	assert(current_cpu()->noff == 1);
	assert(proc->state != PROCESS_RUNNING);
	assert(!intr_enabled());

	intena = current_cpu()->intena;
	switch_context(&proc->ctx, &current_cpu()->ctx);
	current_cpu()->intena = intena;
}

void proc_yield(void)
{
	struct process *proc = proc_get_current();
	spinlock_acquire(&proc->lock);
	proc->state = PROCESS_RUNNABLE;
	proc_sched();
	spinlock_release(&proc->lock);
}

void proc_sleep(void *chan, spinlock_t *lock)
{
	struct process *proc = proc_get_current();
	spinlock_acquire(&proc->lock);
	spinlock_release(lock);
	proc->state = PROCESS_SLEEPING;
	proc->chan = chan;
	proc_sched();
	spinlock_release(&proc->lock);
	spinlock_acquire(lock);
}

void proc_wake_up(void *chan)
{
	struct process *proc;

	for (proc = processes; proc < processes + NR_PROCESSES; ++proc) {
		spinlock_acquire(&proc->lock);
		if (proc->state == PROCESS_SLEEPING && proc->chan == chan) {
			proc->state = PROCESS_RUNNABLE;
			proc->chan = NULL;
		}
		spinlock_release(&proc->lock);
	}
}

void proc_exit(int status)
{
	struct process *child;
	struct process *curr = proc_get_current();

	if (curr == init)
		panic("%s(): init task exit\n", __func__);

	for (int i = 0; i < OPEN_MAX; ++i) {
		if (curr->ofiles[i]) {
			file_put(curr->ofiles[i]);
			curr->ofiles[i] = NULL;
		}
	}
	dentry_put(curr->cwd);

	spinlock_acquire(&wait_lock);

	for (child = processes; child < processes + NR_PROCESSES; ++child)
		if (child->parent == curr)
			child->parent = init;

	proc_wake_up(curr->parent);

	spinlock_acquire(&curr->lock);

	curr->state = PROCESS_ZOMBIE;
	curr->exit_status = status;

	spinlock_release(&wait_lock);

	proc_sched();

	panic("scheduling zombie task\n");
}

pid_t proc_wait(pid_t child_pid, int *status, int options, struct rusage *rus)
{
	bool have_kids;
	struct process *child;
	struct process *parent = proc_get_current();

	spinlock_acquire(&wait_lock);

again:
	have_kids = false;

	for (child = processes; child < processes + NR_PROCESSES; ++child) {
		if (child->parent == parent) {
			spinlock_acquire(&child->lock);
			if (child_pid >= 0 && child->pid != child_pid) {
				spinlock_release(&child->lock);
				continue;
			}
			have_kids = true;
			if (child->state == PROCESS_ZOMBIE) {
				if (status)
					*status = child->exit_status;
				child_pid = child->pid;
				parent->proc_tms.tms_cstime +=
					child->proc_tms.tms_stime;
				parent->proc_tms.tms_cutime +=
					child->proc_tms.tms_utime;
				proc_free(child);
				spinlock_release(&wait_lock);
				return child_pid;
			}
			spinlock_release(&child->lock);
		}
	}

	if (!have_kids) {
		spinlock_release(&wait_lock);
		return -ECHILD;
	}

	proc_sleep(parent, &wait_lock);

	goto again;
}

struct process *proc_get_current(void)
{
	push_off();
	struct cpu *cpu = current_cpu();
	struct process *proc = cpu->current;
	pop_off();
	return proc;
}

void proc_fork_return(void)
{
	struct process *proc = proc_get_current();
	spinlock_release(&proc->lock);
	prepare_to_return();
	user_trap_return(&proc->tf);
}

void proc_set_killed(struct process *proc)
{
	spinlock_acquire(&proc->lock);
	proc->killed = true;
	spinlock_release(&proc->lock);
}

bool proc_is_killed(struct process *proc)
{
	bool killed;
	spinlock_acquire(&proc->lock);
	killed = proc->killed;
	spinlock_release(&proc->lock);
	return killed;
}

int proc_fork(void)
{
	pid_t child_pid;
	struct process *parent = proc_get_current();
	struct process *child = proc_alloc();
	int err;

	if (!child)
		return -ENOMEM;

	err = mm_copy(child->mm, parent->mm);
	if (err) {
		proc_free(child);
		return err;
	}

	memcpy(&child->tf, &parent->tf, sizeof(parent->tf));

	for (int i = 0; i < OPEN_MAX; ++i)
		if (parent->ofiles[i])
			child->ofiles[i] = file_dup(parent->ofiles[i]);

	child->cwd = dentry_dup(parent->cwd);

	child->tf.a0 = 0;

	child_pid = child->pid;
	spinlock_release(&child->lock);

	spinlock_acquire(&wait_lock);
	child->parent = parent;
	spinlock_release(&wait_lock);

	spinlock_acquire(&child->lock);
	child->state = PROCESS_RUNNABLE;
	spinlock_release(&child->lock);

	return child_pid;
}

static uint64_t proc_alloc_kstack(void)
{
	struct page *pg = page_alloc(KSTACK_PAGE_ORDER);
	if (!pg)
		return 0;
	return page_to_virt(pg);
}

static void proc_free_kstack(uint64_t stack)
{
	struct page *pg = virt_to_page(stack);
	assert(pg);
	page_free(pg, KSTACK_PAGE_ORDER);
}

struct process *proc_alloc(void)
{
	struct process *proc;

	for (proc = processes; proc < processes + NR_PROCESSES; ++proc) {
		spinlock_acquire(&proc->lock);
		if (proc->state == PROCESS_UNUSED) {
			proc->state = PROCESS_USED;
			goto found;
		}
		spinlock_release(&proc->lock);
	}

	return NULL;

found:
	proc->pid = pid_alloc();
	if (proc->pid < 0)
		goto pid_alloc_failed;

	proc->kstack = proc_alloc_kstack();
	if (!proc->kstack)
		goto kstack_alloc_failed;

	proc->mm = mm_alloc();
	if (!proc->mm)
		goto create_mm_failed;

	proc->time_slice = PROCESS_TIME_SLICE;
	proc->ctx.ra = (uint64_t)proc_fork_return;
	proc->ctx.sp = proc->kstack + KSTACK_SIZE;

	return proc;

create_mm_failed:
	proc_free_kstack(proc->kstack);
	proc->kstack = 0;
kstack_alloc_failed:
	pid_free(proc->pid);
	proc->pid = 0;
pid_alloc_failed:
	proc->state = PROCESS_UNUSED;
	spinlock_release(&proc->lock);
	return NULL;
}

void proc_free(struct process *proc)
{
	mm_free(proc->mm);
	proc->mm = NULL;
	proc_free_kstack(proc->kstack);
	proc->kstack = 0;
	pid_free(proc->pid);
	proc->pid = 0;

	proc->parent = NULL;
	proc->chan = NULL;
	proc->exit_status = 0;
	proc->killed = false;
	proc->cpu = NULL;
	memset(proc->ofiles, 0, sizeof(proc->ofiles));
	proc->cwd = NULL;
	memset(&proc->proc_tms, 0, sizeof(proc->proc_tms));
	memset(&proc->tf, 0, sizeof(proc->tf));
	memset(&proc->ctx, 0, sizeof(proc->ctx));
	proc->last_ktime = 0;
	proc->last_utime = 0;
	proc->time_slice = 0;
	memset(proc->name, 0, sizeof(proc->name));

	proc->state = PROCESS_UNUSED;
	spinlock_release(&proc->lock);
}

static void proc_init_return(void)
{
	struct process *proc = proc_get_current();
	spinlock_release(&proc->lock);
	fs_init();
	char *argv[] = { "/bin/init", 0 };
	char *envp[] = { 0 };
	int ret = do_execve(argv[0], argv, envp);
	if (ret < 0)
		panic("execve %s failed: %s\n", argv[0], strerror(ret));
	proc->tf.a0 = ret;
	prepare_to_return();
	user_trap_return(&proc->tf);
}

void proc_init(void)
{
	struct process *proc;

	for (proc = processes; proc < processes + NR_PROCESSES; ++proc)
		spinlock_init(&proc->lock, "process");

	init = proc_alloc();
	strlcpy(init->name, "init", sizeof(init->name));
	init->ctx.ra = (uint64_t)proc_init_return;
	init->state = PROCESS_RUNNABLE;
	spinlock_release(&init->lock);
}

void proc_dump(void)
{
	static char *states[] = {
		[PROCESS_UNUSED] = "unused",   [PROCESS_USED] = "  used",
		[PROCESS_SLEEPING] = " sleep", [PROCESS_RUNNABLE] = "runble",
		[PROCESS_RUNNING] = "   run",  [PROCESS_ZOMBIE] = "zombie"
	};
	struct process *proc;
	char *state;

	printk("\n");
	for (proc = processes; proc < processes + NR_PROCESSES; ++proc) {
		if (proc->state == PROCESS_UNUSED)
			continue;
		if (proc->state >= 0 && proc->state < countof(states) &&
		    states[proc->state])
			state = states[proc->state];
		else
			state = "???";
		printk("%ld %s %s", proc->pid, state, proc->name);
		printk("\n");
	}
}

int proc_set_brk(uint64_t addr)
{
	struct process *proc = proc_get_current();
	struct mem_mgmt *mm = proc->mm;
	struct vmem_area *heap = mm->heap;
	uint64_t heap_start = heap->addr;
	uint64_t curr_heap_end = heap_start + heap->size;
	int err = 0;
	uint64_t new_heap_end;
	size_t incr;

	if (addr < heap_start)
		return 0;

	if (addr <= curr_heap_end) {
		mm->brk = addr;
		return 0;
	}

	new_heap_end = align_up(addr, PAGE_SIZE);
	incr = new_heap_end - curr_heap_end;
	size_t old_npgs = heap->nr_pages;
	size_t new_npgs = old_npgs + (incr >> PAGE_SHIFT);
	struct page **new_pgs = kcalloc(new_npgs, sizeof(struct page *));
	if (!new_pgs)
		return -ENOMEM;
	memcpy(new_pgs, heap->pages, old_npgs * sizeof(struct page *));

	addr = curr_heap_end;
	size_t i = old_npgs;
	while (i < new_npgs) {
		struct page *pg = page_alloc(0);
		if (!pg) {
			err = -ENOMEM;
			goto failed;
		}
		uint64_t pa = page_to_phys(pg);
		err = uvmap(mm->pgd, addr, PAGE_SIZE, pa, PTE_R | PTE_W);
		if (err) {
			assert(pg);
			page_free(pg, 0);
			goto failed;
		}
		new_pgs[i] = pg;
		++i;
		addr += PAGE_SIZE;
	}

	struct page **old_pgs = heap->pages;
	heap->pages = new_pgs;
	heap->nr_pages = new_npgs;
	kfree(old_pgs);

	heap->size = new_heap_end - heap_start;
	mm->brk = addr;

	return 0;

failed:
	for (uint64_t a = curr_heap_end; a < addr; a += PAGE_SIZE)
		uvunmap(mm->pgd, a, PAGE_SIZE);
	for (size_t j = old_npgs; j < i; ++j) {
		assert(new_pgs[j]);
		page_free(new_pgs[j], 0);
	}
	kfree(new_pgs);
	return err;
}

int proc_alloc_fd(struct process *proc, struct file *fp)
{
	for (int i = 0; i < OPEN_MAX; ++i) {
		if (!proc->ofiles[i]) {
			proc->ofiles[i] = fp;
			return i;
		}
	}

	return -1;
}
