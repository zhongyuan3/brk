#include <brk/fs.h>
#include <brk/ktime.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/types.h>
#include <brk/writeback.h>
#include <uapi/errno.h>

#define WRITEBACK_INTERVAL_MS 5000

static void writeback_sleep_ms(unsigned long ms)
{
	struct timespec dur = {
		.tv_sec = (time_t)(ms / MS_PER_SEC),
		.tv_nsec = (long)((ms % MS_PER_SEC) * NS_PER_MS),
	};
	struct timespec rem;

	(void)ktime_nanosleep(&dur, &rem);
}

static void writeback_thread(void *arg)
{
	(void)arg;

	for (;;) {
		int err = sync_all_filesystems();

		if (err)
			pr_warn("writeback: sync failed: %d\n", err);

		writeback_sleep_ms(WRITEBACK_INTERVAL_MS);
	}
}

int writeback_start(void)
{
	struct task *task = kthread_create(writeback_thread, NULL);

	if (!task)
		return -ENOMEM;

	strlcpy(task->name, "writeback", sizeof(task->name));
	kthread_join(task);
	pr_info("writeback: dirty page flush thread started\n");

	return 0;
}
