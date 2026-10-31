#include <brk/error.h>
#include <brk/fs.h>
#include <brk/fsinfo.h>
#include <brk/klog.h>
#include <brk/ktime.h>
#include <brk/printk.h>
#include <brk/printf.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/types.h>
#include <brk/uart.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>

#define KLOG_PATH "/var/log/kernel.log"

/*
 * KERN_INFO keeps the on-disk log useful without flooding the disk with the
 * very chatty KERN_DEBUG dentry/page-cache traces.  Change this to
 * KERN_DEBUG if a verbose trace is needed.
 */
#define KLOG_LEVEL KERN_INFO

#define KLOG_POLL_MS 100
#define KLOG_BATCH 64

static void klog_sleep_ms(unsigned long ms)
{
	struct timespec dur = {
		.tv_sec = (time_t)(ms / MS_PER_SEC),
		.tv_nsec = (long)((ms % MS_PER_SEC) * NS_PER_MS),
	};
	struct timespec rem;

	(void)ktime_nanosleep(&dur, &rem);
}

static void klog_write(struct fs_file *file, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = fs_file_write(file, buf, len);

		if (n < 0) {
			/* The filesystem may be temporarily busy; retry. */
			klog_sleep_ms(100);
			continue;
		}
		if (n == 0)
			break;
		buf += n;
		len -= (size_t)n;
	}

	/*
	 * The log file is the only persistent copy of kernel messages.
	 * Flush it after each batch so a crash does not leave everything in
	 * the page cache.
	 */
	if (file->ops && file->ops->fsync)
		(void)file->ops->fsync(file, 0, -1, 0);
}

static void klog_entry(void *arg)
{
	char rec[1024];
	char out[1536];
	struct fs_file *file = NULL;
	u8 level;
	u64 ts;
	int n = 0;

	(void)arg;

	for (;;) {
		if (!file) {
			struct fs_file *f = do_openat(AT_FDCWD, KLOG_PATH,
						      O_WRONLY | O_CREAT |
							      O_APPEND,
						      0644);

			if (IS_ERR(f)) {
				pr_warn("klogd: open %s failed: %ld, retrying\n",
					KLOG_PATH, (long)PTR_ERR(f));
				klog_sleep_ms(1000);
				continue;
			}

			file = f;
			/*
			 * The file is ready; switch kernel output from the
			 * UART to the disk log.  User I/O through /dev/tty0 is
			 * unaffected.
			 */
			ns16550a_console_disable();
			pr_info("klogd: writing kernel log to %s\n",
				KLOG_PATH);
		}

		bool wrote = false;
		int budget = KLOG_BATCH;

		while (budget-- > 0 &&
		       (n = printk_read_record(rec, sizeof(rec), &level, &ts)) >
			       0) {
			int m;

			if (level > KLOG_LEVEL)
				continue;

			m = snprintf(out, sizeof(out),
				     "<%u>[%5" PRIu64 ".%06" PRIu64 "] %s%s",
				     level, (u64)(ts / NS_PER_SEC),
				     (u64)((ts % NS_PER_SEC) / NS_PER_US), rec,
				     (n > 0 && rec[n - 1] == '\n') ? "" : "\n");
			if (m < 0)
				continue;
			if ((size_t)m >= sizeof(out))
				m = sizeof(out) - 1;

			klog_write(file, out, (size_t)m);
			wrote = true;
		}

		if (n < 0) {
			/* Oversized record was consumed; nothing to write. */
			continue;
		}

		klog_sleep_ms(wrote ? 10 : KLOG_POLL_MS);
	}
}

int klog_start(void)
{
	struct task *task = kthread_create(klog_entry, NULL);

	if (!task)
		return -ENOMEM;

	/*
	 * kthread_create() gives the new task an empty fsinfo.  Inherit the
	 * current root/cwd so absolute paths such as /var/log/kernel.log resolve
	 * correctly before the thread opens its file.
	 */
	fsinfo_copy(task->fsinfo, current_task()->fsinfo);
	strlcpy(task->name, "klogd", sizeof(task->name));
	kthread_join(task);

	return 0;
}
