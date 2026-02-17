#include <aosd/console.h>
#include <aosd/cpu.h>
#include <aosd/dtb.h>
#include <aosd/irq.h>
#include <aosd/mm.h>
#include <aosd/plic.h>
#include <aosd/printk.h>
#include <aosd/riscv.h>
#include <aosd/sbi.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
#include <aosd/slab.h>
#include <aosd/timer.h>
#include <aosd/trap.h>
#include <aosd/uart.h>
#include <aosd/virtio.h>
#include <aosd/virtio_blk.h>

static int sbi_console_write(struct console *con, char const *buf, size_t n,
			     size_t *written)
{
	for (size_t i = 0; i < n; ++i)
		sbi_console_putchar(buf[i]);

	if (written)
		*written = n;

	return 0;
}

static struct console sbi_console = {
	.write = sbi_console_write,
};

static int uart_write(struct console *con, char const *buf, size_t n,
		      size_t *written)
{
	for (size_t i = 0; i < n; ++i)
		uart_putc(buf[i]);

	if (written)
		*written = n;

	return 0;
}

static struct console uart_console = {
	.write = uart_write,
};

#define INTERVAL 40000000

static void thread0(void)
{
	volatile size_t i = 0;
	while (1) {
		if (i % INTERVAL == 0)
			printk("%s: A\n", __func__);
		++i;
	}
}

static void thread0_entry(void)
{
	enable_int();
	thread0();
}

static void thread1(void)
{
	volatile size_t i = 0;
	char *buf = kmalloc(SECTOR_SIZE);
	virtio_blk_read(2, buf, 1);
	size_t j = 0;
	while (j < SECTOR_SIZE) {
		if (i % INTERVAL == 0) {
			printk("%s: %d\n", __func__, buf[j]);
			++j;
		}
		++i;
	}
	kfree(buf);
	while (1) {
		if (i % INTERVAL == 0)
			printk("%s: B\n", __func__);

		++i;
	}
}

static void thread1_entry(void)
{
	enable_int();
	thread1();
}

static void thread2(void)
{
	volatile size_t i = 0;
	while (1) {
		if (i % INTERVAL == 0)
			printk("%s: C\n", __func__);
		++i;
	}
}

static void thread2_entry(void)
{
	enable_int();
	thread2();
}

static void create_thread(void (*entry)(void))
{
	struct task *task = task_create();
	if (!task) {
		log_warn("create thread failed\n");
		return;
	}

	task->ctx.ra = (uint64_t)entry;
	task->ctx.sp = task->stack + KSTACK_SIZE;
	task->state = TASK_RUNNING;
	sched_join(task);
}

void start_kernel(size_t hart_id, uint64_t dtb, size_t load_offset)
{
	write_stvec((uint64_t)early_trap_vector);

	console_register(&sbi_console);

	kernel_map.load_offset = load_offset;
	dtb_phys = dtb;
	dtb_virt = (void *)dtb;
	mm_init();
	dtb_virt = (void *)phys_to_virt(dtb);

	dtb_init_scan_cpu();
	save_cpu(hart_id);
	timer_init();

	plic_init();
	irq_init();

	uart_init(hart_id);
	console_unregister(&sbi_console);
	console_register(&uart_console);

	trap_init(hart_id);

	dtb_init_scan_virtio_dev();
	struct virtio_device *dev = virtio_dev_get(VIRTIO_DEVICE_ID_BLK);
	virtio_blk_init(hart_id, dev, 16);

	sched_init();

	create_thread(thread0_entry);
	create_thread(thread1_entry);
	create_thread(thread2_entry);

	start_scheduling();

	while (1) {
	}
}
