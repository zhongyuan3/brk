#include <brk/assert.h>
#include <brk/atomic.h>
#include <brk/cpu.h>
#include <brk/panic.h>
#include <brk/process.h>
#include <brk/rwsem.h>
#include <brk/spinlock.h>
#include <brk/thread.h>
#include <brk/wait.h>
#include <uapi/errno.h>

void rwsem_init(rwsem_t *sem, const char *name)
{
	sem->readers = 0;
	sem->writer = NULL;
	sem->waiting_writers = 0;
	spinlock_init(&sem->wait_lock, "rwsem.wait_lock");
	wait_queue_init(&sem->wait, &sem->wait_lock, name);
	sem->name = name;
}

void down_read(rwsem_t *sem)
{
	irqstate_t flags;

	might_sleep();

	flags = spin_lock_irqsave(&sem->wait_lock);
	if (sem->writer == current_thread())
		panic("down_read: recursive name=%s\n", sem->name);
	while (sem->writer || sem->waiting_writers)
		thread_sleep_on(&sem->wait, &sem->wait_lock);
	sem->readers++;
	spin_unlock_irqrestore(&sem->wait_lock, flags);
}

int down_read_interruptible(rwsem_t *sem)
{
	irqstate_t flags;
	int err;

	might_sleep();

	flags = spin_lock_irqsave(&sem->wait_lock);
	if (sem->writer == current_thread())
		panic("down_read_interruptible: recursive name=%s\n",
		      sem->name);

	while (sem->writer || sem->waiting_writers) {
		err = thread_sleep_on_interruptible(&sem->wait,
						    &sem->wait_lock);
		if (err < 0) {
			/*
			 * The lock may have become available just as the
			 * abort was requested; take it instead of failing.
			 */
			if (!sem->writer && !sem->waiting_writers) {
				sem->readers++;
				err = 0;
			}
			spin_unlock_irqrestore(&sem->wait_lock, flags);
			return err;
		}
	}
	sem->readers++;
	spin_unlock_irqrestore(&sem->wait_lock, flags);
	return 0;
}

bool down_read_trylock(rwsem_t *sem)
{
	irqstate_t flags;
	bool acquired = false;

	flags = spin_lock_irqsave(&sem->wait_lock);
	if (!sem->writer && !sem->waiting_writers) {
		sem->readers++;
		acquired = true;
	}
	spin_unlock_irqrestore(&sem->wait_lock, flags);
	return acquired;
}

void up_read(rwsem_t *sem)
{
	irqstate_t flags;

	flags = spin_lock_irqsave(&sem->wait_lock);
	if (!sem->readers)
		panic("up_read: name=%s readers=0\n", sem->name);
	sem->readers--;
	if (sem->readers == 0)
		thread_wake_all(&sem->wait);
	spin_unlock_irqrestore(&sem->wait_lock, flags);
}

void down_write(rwsem_t *sem)
{
	struct thread *self = current_thread();
	irqstate_t flags;

	might_sleep();

	flags = spin_lock_irqsave(&sem->wait_lock);
	if (sem->writer == self)
		panic("down_write: recursive name=%s\n", sem->name);

	sem->waiting_writers++;
	while (sem->writer || sem->readers)
		thread_sleep_on(&sem->wait, &sem->wait_lock);
	sem->waiting_writers--;
	sem->writer = self;
	spin_unlock_irqrestore(&sem->wait_lock, flags);
}

int down_write_interruptible(rwsem_t *sem)
{
	struct thread *self = current_thread();
	irqstate_t flags;
	int err;

	might_sleep();

	flags = spin_lock_irqsave(&sem->wait_lock);
	if (sem->writer == self)
		panic("down_write_interruptible: recursive name=%s\n",
		      sem->name);

	sem->waiting_writers++;
	while (sem->writer || sem->readers) {
		err = thread_sleep_on_interruptible(&sem->wait,
						    &sem->wait_lock);
		if (err < 0) {
			/*
			 * The lock may have become available just as the
			 * abort was requested; take it instead of failing.
			 */
			if (!sem->writer && !sem->readers) {
				sem->waiting_writers--;
				sem->writer = self;
				spin_unlock_irqrestore(&sem->wait_lock, flags);
				return 0;
			}

			sem->waiting_writers--;
			if (!sem->waiting_writers)
				thread_wake_all(&sem->wait);
			spin_unlock_irqrestore(&sem->wait_lock, flags);
			return err;
		}
	}
	sem->waiting_writers--;
	sem->writer = self;
	spin_unlock_irqrestore(&sem->wait_lock, flags);
	return 0;
}

bool down_write_trylock(rwsem_t *sem)
{
	irqstate_t flags;
	bool acquired = false;

	flags = spin_lock_irqsave(&sem->wait_lock);
	if (!sem->writer && !sem->readers && !sem->waiting_writers) {
		sem->writer = current_thread();
		acquired = true;
	}
	spin_unlock_irqrestore(&sem->wait_lock, flags);
	return acquired;
}

void up_write(rwsem_t *sem)
{
	struct thread *self = current_thread();
	irqstate_t flags;

	flags = spin_lock_irqsave(&sem->wait_lock);
	if (sem->writer != self)
		panic("up_write: name=%s writer=%p self=%p\n", sem->name,
		      sem->writer, self);
	sem->writer = NULL;
	thread_wake_all(&sem->wait);
	spin_unlock_irqrestore(&sem->wait_lock, flags);
}

bool rwsem_is_locked(const rwsem_t *sem)
{
	bool locked;

	/* The caller is only expected to use this for debugging. */
	locked = atomic_load_n(&sem->readers, ATOMIC_ACQUIRE) != 0 ||
		 atomic_load_n(&sem->writer, ATOMIC_ACQUIRE) != NULL;
	return locked;
}

bool rwsem_is_write_locked_by_me(const rwsem_t *sem)
{
	return atomic_load_n(&sem->writer, ATOMIC_ACQUIRE) == current_thread();
}
