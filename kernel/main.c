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

void start_kernel(size_t hart_id, uint64_t dtb, size_t load_offset)
{
	struct virtio_device *dev;

	console_register(&sbi_console);

	kernel_map.load_offset = load_offset;
	dtb_phys = dtb;
	dtb_virt = (void *)dtb;
	mm_init();
	dtb_virt = (void *)phys_to_virt(dtb);

	dtb_init_scan_cpu();
	cpu_init(hart_id);
	timer_init();

	plic_init();
	irq_init();

	uart_init(hart_id);
	console_unregister(&sbi_console);
	console_register(&uart_console);

	trap_init(hart_id);

	dtb_init_scan_virtio_dev();
	dev = virtio_dev_get(VIRTIO_DEVICE_ID_BLK);
	virtio_blk_init(hart_id, dev, 16);

	sched_init();
	start_scheduling();
}
