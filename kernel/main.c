#include <aosd/asm.h>
#include <aosd/console.h>
#include <aosd/cpu.h>
#include <aosd/dtb.h>
#include <aosd/irq.h>
#include <aosd/mm.h>
#include <aosd/pgtable.h>
#include <aosd/plic.h>
#include <aosd/printk.h>
#include <aosd/riscv.h>
#include <aosd/sbi.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
#include <aosd/slab.h>
#include <aosd/spinlock.h>
#include <aosd/timer.h>
#include <aosd/trap.h>
#include <aosd/uart.h>
#include <aosd/virtio.h>
#include <aosd/virtio_blk.h>
#include <aosd/vmalloc.h>

void start_other_harts(uint64_t init_hart_id)
{
	uint64_t start_addr;

	start_addr = symbol_phys(hart_entry);
	for (uint64_t id = 0; id < NR_CPUS; ++id) {
		if (id == init_hart_id)
			continue;

		sbi_hart_start(id, start_addr, 0);
	}
}

void thread_create(void (*entry)(void))
{
	struct task *t = task_create();
	if (!t) {
		printk("create task failed\n");
		return;
	}
	t->thread_entry = entry;
	t->state = TASK_RUNNABLE;
	t->parent = init_task;
	spinlock_release(&t->lock);
}

void thread0(void)
{
	intr_on();
	printk("%s: hello\n", __func__);
	while (1) {
	}
}

void thread1(void)
{
	intr_on();
	printk("%s: hello\n", __func__);
	while (1) {
	}
}

void thread2(void)
{
	intr_on();
	printk("%s: hello\n", __func__);
	while (1) {
	}
}

void thread3(void)
{
	intr_on();
	printk("%s: hello\n", __func__);
	char *buf = kmalloc(SECTOR_SIZE);
	virtio_blk_read(2, buf, 1);
	printk("%s: read disk succeed\n", __func__);
	kfree(buf);
	intr_on();
	sched_exit(0);
}

void thread4(void)
{
	intr_on();
	printk("%s: hello\n", __func__);
	while (1) {
	}
}

void thread5(void)
{
	intr_on();
	printk("%s: hello\n", __func__);
	char *buf = kmalloc(SECTOR_SIZE);
	virtio_blk_read(2, buf, 1);
	printk("%s: read disk succeed\n", __func__);
	kfree(buf);
	intr_on();
	sched_exit(0);
}

void start_kernel(size_t hart_id, uint64_t dtb, size_t load_offset)
{
	cpu_init_hart(hart_id);

	kernel_map.load_offset = load_offset;
	dtb_phys = dtb;
	dtb_virt = (void *)dtb;
	mm_init();
	dtb_virt = (void *)phys_to_virt(dtb);

	dtb_init_scan_cpu();

	irq_init();
	irq_init_hart(hart_id);

	uart_init();
	uart_init_hart(hart_id);

	trap_init();
	trap_init_hart(hart_id);

	dtb_init_scan_virtio_dev();

	virtio_blk_init(virtio_dev_get(VIRTIO_DEVICE_ID_BLK), 16);
	virtio_blk_init_hart(hart_id);

	sched_init();

	thread_create(thread0);
	thread_create(thread1);
	thread_create(thread2);
	thread_create(thread3);
	thread_create(thread4);
	thread_create(thread5);

	start_other_harts(hart_id);

	scheduler();
}

void start_hart(uint64_t hart_id)
{
	cpu_init_hart(hart_id);

	write_satp(make_satp_sv39(symbol_phys(kernel_pgdir)));
	sfence_vma();

	printk("hart %lu starting\n", hart_id);

	irq_init_hart(hart_id);
	uart_init_hart(hart_id);
	virtio_blk_init_hart(hart_id);
	trap_init_hart(hart_id);

	scheduler();
}
