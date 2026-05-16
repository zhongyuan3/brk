#include <brk/asm.h>
#include <brk/console.h>
#include <brk/cpu.h>
#include <brk/dcache.h>
#include <brk/dev.h>
#include <brk/dtb.h>
#include <brk/fs.h>
#include <brk/irq.h>
#include <brk/lock.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/pagecache.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/pgtable.h>
#include <brk/plic.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/riscv.h>
#include <brk/rtc.h>
#include <brk/timekeeper.h>
#include <brk/sbi.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/uart.h>
#include <brk/virtio.h>
#include <brk/virtio_blk.h>
#include <brk/vmalloc.h>
#include <libfdt.h>

#if ENABLE_SMP

static void wake_up_other_harts(u64 init_hart_id)
{
	u64 start_addr;

	start_addr = symbol_phys(hart_entry);
	for (u64 id = 0; id < NR_CPUS; ++id) {
		if (id == init_hart_id)
			continue;

		sbi_hart_start(id, start_addr, 0);
	}
}

void start_hart(u64 hart_id)
{
	write_tp(hart_id);

	write_satp(make_satp_sv39(symbol_phys(kernel_pgdir)));
	sfence_vma();

	irq_init_hart(hart_id);
	uart_init_hart(hart_id);
	virtio_blk_init_hart(hart_id);
	trap_init_hart(hart_id);

	klog_info("hart %lu starting\n", hart_id);

	proc_scheduler();
}

#endif

void start_kernel(usize_t hart_id, u64 dtb, usize_t load_offset)
{
	write_tp(hart_id);

	kernel_load_offset = load_offset;
	dtb_phys = dtb;
	init_cpuid = hart_id;

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

	mm_cache_init();

	dtb_init_scan_cpu();

	irq_init();
	irq_init_hart(hart_id);

	console_init();
	uart_init();
	uart_init_hart(hart_id);

	rtc_init();
	timekeeper_init();

	trap_init();
	trap_init_hart(hart_id);

	dtb_init_scan_virtio_dev();

	struct virtio_device *blk = virtio_dev_get(VIRTIO_DEVICE_ID_BLK);
	virtio_blk_init(blk, 16);
	virtio_blk_init_hart(hart_id);

	dev_init();

	pagecache_init();
	dentry_cache_init();
	inode_cache_init();
	file_cache_init();
	proc_cache_init();
	proc_init_user();

#if ENABLE_SMP
	wake_up_other_harts(hart_id);
#endif

	proc_scheduler();
}
