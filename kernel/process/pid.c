#include <aosd/process.h>

static SPINLOCK_DEFINE(pid_lock);
static pid_t curr_pid = 1;

pid_t pid_alloc(void)
{
	pid_t pid;

	spinlock_acquire(&pid_lock);
	pid = curr_pid++;
	spinlock_release(&pid_lock);
	return pid;
}

void pid_free(pid_t pid)
{
}
