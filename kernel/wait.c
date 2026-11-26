#include <brk/assert.h>
#include <brk/list.h>
#include <brk/wait.h>

void wait_queue_init(wait_queue_head_t *wq, spinlock_t *lock, const char *name)
{
	kassert(wq != NULL);
	kassert(lock != NULL);

	wq->lock = lock;
	wq->name = name;
	list_init(&wq->waiters);
}

int wake_up(wait_queue_head_t *wq)
{
	kassert(wq != NULL);

	return thread_wake_one(wq);
}

int wake_up_all(wait_queue_head_t *wq)
{
	kassert(wq != NULL);

	return thread_wake_all(wq);
}
