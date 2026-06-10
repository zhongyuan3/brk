#include <arch/irqflags.h>
#include <brk/drivers/blkdev.h>
#include <brk/drivers/chrdev.h>
#include <brk/drivers/device.h>
#include <brk/drivers/rtc.h>
#include <brk/drivers/uart.h>
#include <brk/drivers/virtio.h>
#include <brk/drivers/virtio_blk.h>
#include <brk/fs/dcache.h>
#include <brk/fs/fs.h>
#include <brk/kernel/console.h>
#include <brk/kernel/cpu.h>
#include <brk/kernel/dtb.h>
#include <brk/kernel/init.h>
#include <brk/kernel/irq.h>
#include <brk/kernel/task.h>
#include <brk/kernel/timekeeper.h>
#include <brk/kernel/timer.h>
#include <brk/kernel/trap.h>
#include <brk/lib/kernel.h>
#include <brk/mm/kmalloc.h>
#include <brk/mm/memblock.h>
#include <brk/mm/mm.h>
#include <brk/mm/pagecache.h>
#include <brk/mm/pgalloc.h>
#include <brk/mm/vmalloc.h>
#include <libfdt.h>

void boot_run_primary(usize_t hart_id, u64 dtb, usize_t load_offset)
{
	set_current_cpuid(hart_id);
	set_current_task(NULL);
	kernel_load_offset = load_offset;
	dtb_phys = dtb;
	boot_cpuid = hart_id;

	memblock_init();
	memblock_reserve(_SKERNEL_PHYS, _KERNEL_SIZE);
	usize_t dtb_size = round_up(fdt_totalsize(dtb_phys), PAGE_SIZE);
	memblock_reserve(dtb_phys, dtb_size);
	dtb_early_init_scan_mem();
	dtb_early_init_scan_reserved_mem();

	ram_phys_offset = memblock_get_ram_base();

	setup_final_pgtable();

	vmemmap_init();
	page_alloc_init();
	memblock_free_all();

	kmalloc_init();
	vmalloc_init();
	uvm_space_cache_init();

	console_register(&early_console);

	dtb_init_scan_cpu();
	dtb_init_scan_virtio_dev();

	irq_init();
	rtc_init();
	timekeeper_init();
	timer_init();

	chrdev_registry_init();
	blkdev_registry_init();
	ns16550a_driver_init();
	virtio_blk_init();
	dtb_init_scan_serial();
	virtio_init_scan();

	irq_init_hart(hart_id);
	ns16550a_enable_irq(hart_id);
	virtio_blk_enable_irq(hart_id);
	trap_init_hart(hart_id);

	page_cache_init();
	fs_dentry_cache_init();
	fs_inode_cache_init();
	fs_file_cache_init();
	task_cache_init();
	task_init_user();

	intr_on();

	task_scheduler();
}
