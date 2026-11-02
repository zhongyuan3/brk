#include <brk/console.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/fsinfo.h>
#include <brk/klog.h>
#include <brk/ktime.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/printk_log.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/types.h>
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
	ktime_t rem;

	(void)ktime_nanosleep((ktime_t)ms * (ktime_t)NS_PER_MS, &rem);
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
}

static void klog_sync(struct fs_file *file)
{
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
	char rec[PRINTK_MAX_PAYLOAD + 1];
	char out[PRINTK_MAX_PAYLOAD + 256];
	struct fs_file *file = NULL;
	struct printk_log_reader reader;
	bool reader_ready = false;
	u8 level;
	u64 ts;
	u8 flags;

	(void)arg;

	for (;;) {
		if (!file) {
			struct fs_file *f =
				do_openat(AT_FDCWD, KLOG_PATH,
					  O_WRONLY | O_CREAT | O_APPEND, 0644);

			if (IS_ERR(f)) {
				pr_warn("klogd: open %s failed: %ld, retrying\n",
					KLOG_PATH, (long)PTR_ERR(f));
				klog_sleep_ms(1000);
				continue;
			}

			file = f;
			if (!reader_ready) {
				printk_log_reader_init(&reader,
						       PRINTK_LOG_START_OLDEST);
				reader_ready = true;
			}
			/*
			 * The file is ready; keep boot consoles available for
			 * emergency panic output, but stop normal kernel log
			 * traffic to them.  User I/O through /dev/tty0 is
			 * unaffected.
			 */
			console_set_boot_consoles_level(KERN_EMERG);
			pr_info("klogd: writing kernel log to %s\n", KLOG_PATH);
		}

		bool wrote = false;
		bool empty = false;
		bool error = false;
		int budget = KLOG_BATCH;

		while (budget-- > 0) {
			int n = printk_log_read(&reader, rec, sizeof(rec),
						&level, &ts, &flags);

			if (reader.pending_lost) {
				char lostbuf[96];
				u64 lost = reader.pending_lost;
				int m;

				reader.pending_lost = 0;
				m = snprintf(lostbuf, sizeof(lostbuf),
					     "<%u>[            ] ** %" PRIu64
					     " bytes lost **\n",
					     KERN_WARN, lost);
				if (m > 0) {
					size_t len = (size_t)m;

					if (len >= sizeof(lostbuf))
						len = sizeof(lostbuf) - 1;
					klog_write(file, lostbuf, len);
					wrote = true;
				}
			}

			if (n == 0) {
				empty = true;
				break;
			}
			if (n == -ENOSPC)
				continue;
			if (n < 0) {
				error = true;
				break;
			}
			if (level > KLOG_LEVEL)
				continue;

			int m = snprintf(
				out, sizeof(out),
				"<%u>[%5" PRIu64 ".%06" PRIu64 "] %s%s%s",
				level, (u64)(ts / NS_PER_SEC),
				(u64)((ts % NS_PER_SEC) / NS_PER_US), rec,
				(flags & PRINTK_FLAGS_TRUNCATED) ? "..." : "",
				(rec[n - 1] == '\n') ? "" : "\n");
			if (m < 0)
				continue;
			if ((size_t)m >= sizeof(out))
				m = sizeof(out) - 1;

			klog_write(file, out, (size_t)m);
			wrote = true;
		}

		if (!empty && !error)
			continue;

		if (wrote)
			klog_sync(file);
		klog_sleep_ms(error ? 10 : (wrote ? 10 : KLOG_POLL_MS));
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
