#include <aosd/sched.h>

pid_t pid_alloc(void)
{
	static spinlock_define(pid_lock);
	static pid_t pid = 1;

	pid_t ret;

	spinlock_acquire(&pid_lock);
	ret = pid++;
	spinlock_release(&pid_lock);
	return ret;
}

void pid_free(pid_t pid)
{
}
