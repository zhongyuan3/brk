#include <brk/cpu.h>
#include <brk/error.h>
#include <brk/futex.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/mm_types.h>
#include <brk/process.h>
#include <brk/signal.h>
#include <brk/spinlock.h>
#include <brk/syscall.h>
#include <brk/thread.h>
#include <brk/uaccess.h>
#include <stdint.h>
#include <uapi/errno.h>
#include <uapi/futex.h>
#include <uapi/time.h>

#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE (1U << FUTEX_HASH_BITS)

struct futex_bucket {
	struct list_head waiters;
};

static struct futex_bucket futex_buckets[FUTEX_HASH_SIZE];
static SPINLOCK_DEFINE(futex_lock);

void futex_init(void)
{
	for (unsigned int i = 0; i < FUTEX_HASH_SIZE; ++i)
		list_init(&futex_buckets[i].waiters);
}

static unsigned int futex_hash(struct uvm_space *mm, const u32 *uaddr)
{
	uintptr_t key = ((uintptr_t)uaddr >> 2) ^ ((uintptr_t)mm >> 4);

	return (unsigned int)(key & (FUTEX_HASH_SIZE - 1));
}

static int futex_get_value(u32 *uaddr, u32 *value)
{
	if (!uaddr || ((uintptr_t)uaddr & (sizeof(u32) - 1)) ||
	    !user_range_ok((u64)(uintptr_t)uaddr, sizeof(u32)))
		return -EFAULT;

	*value = __atomic_load_n(uaddr, __ATOMIC_SEQ_CST);
	return 0;
}

static int futex_timeout_deadline(const struct __brk_timespec *utimeout,
				  bool absolute, bool realtime, u64 *deadline)
{
	struct __brk_timespec ts;
	ktime_t target;
	ktime_t now;

	*deadline = 0;
	if (!utimeout)
		return 0;

	if (copy_from_user(&ts, utimeout, sizeof(ts)))
		return -EFAULT;
	if (ts.tv_sec < 0 || ts.tv_nsec < 0 ||
	    ts.tv_nsec >= (__brk_long_t)NS_PER_SEC)
		return -EINVAL;
	if (ts.tv_sec > (__brk_time_t)(0x7fffffffffffffffLL / NS_PER_SEC))
		return -EINVAL;

	target = ktime_from_timespec(&ts);
	now = (ktime_t)ktime_get_mono_ns();

	if (absolute) {
		if (realtime) {
			ktime_t now_rt = ktime_realtime();

			if (target <= now_rt)
				return -ETIMEDOUT;
			*deadline = (u64)(now + (target - now_rt));
		} else {
			if (target <= now)
				return -ETIMEDOUT;
			*deadline = (u64)target;
		}
	} else {
		*deadline = (u64)(now + target);
	}

	return 0;
}

static int futex_wait_common(u32 *uaddr, u32 val,
			     const struct __brk_timespec *utimeout, u32 bitset,
			     bool absolute, bool realtime)
{
	struct thread *thread = current_thread();
	struct process *process = current_process();
	struct uvm_space *mm = process->mm;
	struct futex_waiter *waiter = &thread->futex;
	struct futex_bucket *bucket;
	u64 deadline;
	u64 now;
	u32 current;
	int err;

	if (!mm || !bitset)
		return -EINVAL;

	err = futex_get_value(uaddr, &current);
	if (err)
		return err;
	if (current != val)
		return -EAGAIN;

	err = futex_timeout_deadline(utimeout, absolute, realtime, &deadline);
	if (err)
		return err;

	bucket = &futex_buckets[futex_hash(mm, uaddr)];

	irqstate_t flags = spin_lock_irqsave(&futex_lock);

	err = futex_get_value(uaddr, &current);
	if (err) {
		spin_unlock_irqrestore(&futex_lock, flags);
		return err;
	}
	if (current != val) {
		spin_unlock_irqrestore(&futex_lock, flags);
		return -EAGAIN;
	}
	if (signal_pending(thread)) {
		spin_unlock_irqrestore(&futex_lock, flags);
		return -EINTR;
	}
	now = (u64)ktime_get_mono_ns();
	if (deadline && (s64)(now - deadline) >= 0) {
		spin_unlock_irqrestore(&futex_lock, flags);
		return -ETIMEDOUT;
	}

	waiter->bucket = bucket;
	waiter->mm = mm;
	waiter->thread = thread;
	waiter->uaddr = uaddr;
	waiter->bitset = bitset;
	waiter->queued = true;
	waiter->woken = false;
	list_add_tail(&waiter->node, &bucket->waiters);

	if (deadline)
		thread_arm_sleep_timer(&futex_lock, deadline);

	for (;;) {
		bool slept = thread_sleep_interruptible(bucket, &futex_lock);

		if (waiter->woken) {
			err = 0;
			break;
		}
		if (!slept || signal_pending(thread)) {
			err = -EINTR;
			break;
		}
		now = (u64)ktime_get_mono_ns();
		if (deadline && (s64)(now - deadline) >= 0) {
			err = -ETIMEDOUT;
			break;
		}
	}

	if (waiter->queued) {
		list_del_init(&waiter->node);
		waiter->queued = false;
	}
	spin_unlock_irqrestore(&futex_lock, flags);

	if (deadline)
		thread_disarm_sleep_timer();

	return err;
}

static int futex_wake_locked(struct uvm_space *mm, u32 *uaddr, u32 bitset,
			     int nr)
{
	struct futex_bucket *bucket;
	int count = 0;

	if (!mm || !bitset)
		return -EINVAL;
	if (nr <= 0)
		return 0;

	bucket = &futex_buckets[futex_hash(mm, uaddr)];

	while (count < nr) {
		struct futex_waiter *waiter;
		struct thread *thread = NULL;

		irqstate_t flags = spin_lock_irqsave(&futex_lock);
		list_for_each_entry(waiter, &bucket->waiters, node) {
			if (waiter->mm != mm || waiter->uaddr != uaddr)
				continue;
			if (!(waiter->bitset & bitset))
				continue;

			list_del_init(&waiter->node);
			waiter->queued = false;
			waiter->woken = true;
			thread = waiter->thread;
			++count;
			break;
		}
		spin_unlock_irqrestore(&futex_lock, flags);

		if (!thread)
			break;
		thread_wake(thread);
	}

	return count;
}

int futex_wake_user(struct uvm_space *mm, u32 *uaddr, u32 bitset, int nr)
{
	return futex_wake_locked(mm, uaddr, bitset, nr);
}

static int futex_requeue(struct uvm_space *mm, u32 *uaddr1, u32 *uaddr2,
			 int nr_wake, int nr_requeue, bool do_cmp, u32 cmpval)
{
	struct futex_bucket *bucket1;
	struct futex_bucket *bucket2;
	struct futex_waiter *waiter;
	struct futex_waiter *tmp;
	u32 value;
	int woken = 0;
	int requeued = 0;
	int err;

	if (!mm)
		return -EINVAL;

	bucket1 = &futex_buckets[futex_hash(mm, uaddr1)];
	bucket2 = &futex_buckets[futex_hash(mm, uaddr2)];

	irqstate_t flags = spin_lock_irqsave(&futex_lock);
	if (do_cmp) {
		err = futex_get_value(uaddr1, &value);
		if (err) {
			spin_unlock_irqrestore(&futex_lock, flags);
			return err;
		}
		if (value != cmpval) {
			spin_unlock_irqrestore(&futex_lock, flags);
			return -EAGAIN;
		}
	}
	spin_unlock_irqrestore(&futex_lock, flags);

	while (woken < nr_wake) {
		struct thread *thread = NULL;

		flags = spin_lock_irqsave(&futex_lock);
		list_for_each_entry(waiter, &bucket1->waiters, node) {
			if (waiter->mm != mm || waiter->uaddr != uaddr1)
				continue;
			list_del_init(&waiter->node);
			waiter->queued = false;
			waiter->woken = true;
			thread = waiter->thread;
			++woken;
			break;
		}
		spin_unlock_irqrestore(&futex_lock, flags);
		if (!thread)
			break;
		thread_wake(thread);
	}

	if (nr_requeue > 0) {
		LIST_DEFINE(moved);

		flags = spin_lock_irqsave(&futex_lock);
		list_for_each_entry_safe(waiter, tmp, &bucket1->waiters, node) {
			if (requeued >= nr_requeue)
				break;
			if (waiter->mm != mm || waiter->uaddr != uaddr1)
				continue;

			list_move_tail(&waiter->node, &moved);
			++requeued;
		}
		list_for_each_entry_safe(waiter, tmp, &moved, node) {
			list_del_init(&waiter->node);
			waiter->uaddr = uaddr2;
			waiter->bucket = bucket2;
			list_add_tail(&waiter->node, &bucket2->waiters);
		}
		spin_unlock_irqrestore(&futex_lock, flags);
	}

	return woken + requeued;
}

void futex_cleanup_thread(struct thread *thread)
{
	struct futex_waiter *waiter = &thread->futex;

	if (!waiter->queued)
		return;

	irqstate_t flags = spin_lock_irqsave(&futex_lock);

	if (waiter->queued) {
		list_del_init(&waiter->node);
		waiter->queued = false;
	}
	spin_unlock_irqrestore(&futex_lock, flags);
}

long sys_futex(const struct syscall_ctx *ctx)
{
	u32 *uaddr = syscall_arg_ptr(ctx, 0);
	int op = syscall_arg_int(ctx, 1);
	u32 val = (u32)syscall_arg_raw(ctx, 2);
	u64 arg_timeout = syscall_arg_raw(ctx, 3);
	u32 *uaddr2 = syscall_arg_ptr(ctx, 4);
	u32 val3 = (u32)syscall_arg_raw(ctx, 5);
	const struct __brk_timespec *timeout =
		(const struct __brk_timespec *)(uintptr_t)arg_timeout;
	int cmd = op & FUTEX_CMD_MASK;
	bool realtime = op & FUTEX_CLOCK_REALTIME;
	int ret;

	if (!uaddr || !user_range_ok((u64)(uintptr_t)uaddr, sizeof(u32)))
		return -EFAULT;
	if ((uintptr_t)uaddr & (sizeof(u32) - 1))
		return -EINVAL;

	switch (cmd) {
	case FUTEX_WAIT:
		if (realtime)
			return -EINVAL;
		ret = futex_wait_common(uaddr, val, timeout,
					FUTEX_BITSET_MATCH_ANY, false, false);
		break;
	case FUTEX_WAIT_BITSET:
		if (!val3)
			return -EINVAL;
		ret = futex_wait_common(uaddr, val, timeout, val3, true,
					realtime);
		break;
	case FUTEX_WAKE:
		ret = futex_wake_user(current_process()->mm, uaddr,
				      FUTEX_BITSET_MATCH_ANY, (int)val);
		break;
	case FUTEX_WAKE_BITSET:
		if (!val3)
			return -EINVAL;
		ret = futex_wake_user(current_process()->mm, uaddr, val3,
				      (int)val);
		break;
	case FUTEX_REQUEUE:
	case FUTEX_CMP_REQUEUE:
		if (!uaddr2 ||
		    !user_range_ok((u64)(uintptr_t)uaddr2, sizeof(u32)))
			return -EFAULT;
		if ((uintptr_t)uaddr2 & (sizeof(u32) - 1))
			return -EINVAL;
		ret = futex_requeue(current_process()->mm, uaddr, uaddr2,
				    (int)val, (int)arg_timeout,
				    cmd == FUTEX_CMP_REQUEUE, val3);
		break;
	default:
		ret = -ENOSYS;
		break;
	}

	return (u64)(long)ret;
}
