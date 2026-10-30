#include <brk/irq.h>
#include <brk/ktime.h>
#include <brk/net/irqtest.h>
#include <brk/net/net.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <brk/types.h>
#include <brk/virtio_net.h>
#include <uapi/errno.h>

static u64 shared_net_calls;
static unsigned int shared_net_irq;
static char result[160] = "commands: shared-net | spurious | status\n";

static irqreturn_t shared_net_handler(int irq, void *dev_id)
{
	(void)irq;
	(void)dev_id;
	__atomic_add_fetch(&shared_net_calls, 1, __ATOMIC_RELAXED);
	return IRQ_HANDLED;
}

static int run_shared_net(void)
{
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 1000000000 };
	struct timespec rem;
	unsigned int irq = virtio_net_first_irq();
	int err;

	if (!irq) {
		snprintf(result, sizeof(result),
			 "shared-net: no virtio-net IRQ\n");
		pr_err("irqtest: no virtio-net IRQ\n");
		return -ENODEV;
	}

	shared_net_irq = irq;
	__atomic_store_n(&shared_net_calls, 0, __ATOMIC_RELAXED);

	err = request_irq(irq, shared_net_handler, IRQF_SHARED, "irqtest-net",
			  NULL);
	if (err) {
		snprintf(result, sizeof(result),
			 "shared-net: request_irq failed: %d\n", err);
		pr_err("irqtest: request_irq(%u) failed: %d\n", irq, err);
		return err;
	}

	/* Generate real traffic: ARP + ICMP through eth0. */
	err = net_ping(NET_DEFAULT_GW);
	if (err && err != -EAGAIN) {
		free_irq(irq, NULL);
		snprintf(result, sizeof(result),
			 "shared-net: net_ping failed: %d\n", err);
		pr_err("irqtest: net_ping failed: %d\n", err);
		return err;
	}

	ktime_nanosleep(&delay, &rem);
	free_irq(irq, NULL);

	u64 calls = __atomic_load_n(&shared_net_calls, __ATOMIC_RELAXED);

	if (calls) {
		snprintf(result, sizeof(result),
			 "shared-net: passed (%" PRIu64 " calls, virq=%u)\n",
			 calls, irq);
		pr_info("irqtest: shared virtio-net IRQ passed (%" PRIu64
			" calls, virq=%u)\n",
			calls, irq);
		return 0;
	}

	snprintf(result, sizeof(result),
		 "shared-net: failed (no interrupts, virq=%u)\n", irq);
	pr_err("irqtest: shared virtio-net IRQ saw no interrupts (virq=%u)\n",
	       irq);
	return -EIO;
}

static int run_spurious(void)
{
	irq_note_spurious();
	u64 count = irq_spurious_count();

	snprintf(result, sizeof(result), "spurious: %" PRIu64 "\n", count);
	pr_info("irqtest: spurious counter=%" PRIu64 "\n", count);
	return 0;
}

static int run_status(void)
{
	u64 calls = __atomic_load_n(&shared_net_calls, __ATOMIC_RELAXED);

	snprintf(result, sizeof(result),
		 "spurious=%" PRIu64 " shared_net_calls=%" PRIu64 " virq=%u\n",
		 irq_spurious_count(), calls, shared_net_irq);
	pr_info("irqtest: %s", result);
	return 0;
}

int irq_exttest_run(const char *buf, size_t size)
{
	size_t len = size;

	while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
		       buf[len - 1] == ' ' || buf[len - 1] == '\t'))
		len--;

	if (len == 10 && !memcmp(buf, "shared-net", 10))
		return run_shared_net();
	if (len == 8 && !memcmp(buf, "spurious", 8))
		return run_spurious();
	if (len == 6 && !memcmp(buf, "status", 6))
		return run_status();

	snprintf(result, sizeof(result), "unknown command\n");
	return -EINVAL;
}

int irq_exttest_show(char *buf, size_t size)
{
	return snprintf(buf, size, "%s", result);
}
