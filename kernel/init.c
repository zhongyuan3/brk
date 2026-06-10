#include <arch/irqflags.h>
#include <brk/blkdev.h>
#include <brk/chrdev.h>
#include <brk/console.h>
#include <brk/cpu.h>
#include <brk/dcache.h>
#include <brk/device.h>
#include <brk/dtb.h>
#include <brk/fs.h>
#include <brk/init.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/pagecache.h>
#include <brk/pgalloc.h>
#include <brk/rtc.h>
#include <brk/task.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/uart.h>
#include <brk/virtio.h>
#include <brk/virtio_blk.h>
#include <brk/vmalloc.h>
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
