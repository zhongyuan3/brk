#include <brk/asm.h>
#include <brk/console.h>
#include <brk/cpu.h>
#include <brk/dcache.h>
#include <brk/dev.h>
#include <brk/dtb.h>
#include <brk/fs.h>
#include <brk/init.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/pagecache.h>
#include <brk/pgalloc.h>
#include <brk/pgtable.h>
#include <brk/process.h>
#include <brk/riscv.h>
#include <brk/rtc.h>
#include <brk/sbi.h>
#include <brk/slab.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/uart.h>
#include <brk/virtio.h>
#include <brk/virtio_blk.h>
#include <brk/vmalloc.h>
#include <libfdt.h>

#define VIRTIO_BLK_QUEUE_SIZE 16

static void boot_identity(usize_t hart_id, u64 dtb, usize_t load_offset)
{
	write_tp(hart_id);
	kernel_load_offset = load_offset;
	dtb_phys = dtb;
	init_cpuid = hart_id;
}

/* Physical memory description and switch to the final kernel page tables. */
static void boot_early_mm_init(void)
{
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
}

/* Kernel heap allocators and per-process mm cache. */
static void boot_core_mm_init(void)
{
	kmalloc_init();
	vmalloc_init();
	mm_cache_init();
}

/* Device tree probes that need a virtual address space. */
static void boot_fdt_init(void)
{
	dtb_init_scan_cpu();
	dtb_init_scan_virtio_dev();
}

static void boot_time_init(void)
{
	rtc_init();
	timekeeper_init();
	timer_init();
}

/* Interrupt controller and trap vectors (per-hart enable comes later). */
static void boot_arch_init(void)
{
	irq_init();
	boot_time_init();
}

static void boot_drivers_init(void)
{
	console_init();
	uart_init();

	struct virtio_device *blk = virtio_dev_get(VIRTIO_DEVICE_ID_BLK);
	virtio_blk_init(blk, VIRTIO_BLK_QUEUE_SIZE);
}

void boot_hart_online(u32 hart_id)
{
	irq_init_hart(hart_id);
	uart_init_hart(hart_id);
	virtio_blk_init_hart(hart_id);
	trap_init_hart(hart_id);
}

void boot_hart_irq_on(void)
{
	intr_on();
}

/* Device nodes, VFS caches, and the user init process skeleton. */
static void boot_subsys_init(void)
{
	dev_init();

	pagecache_init();
	dentry_cache_init();
	inode_cache_init();
	file_cache_init();
	proc_cache_init();
	proc_init_user();
}

#if ENABLE_SMP

static void smp_wake_secondary_harts(u64 init_hart_id)
{
	u64 start_addr = symbol_phys(hart_entry);

	for (u64 id = 0; id < NR_CPUS; ++id) {
		if (id == init_hart_id)
			continue;
		sbi_hart_start(id, start_addr, 0);
	}
}

#endif

void boot_run_primary(usize_t hart_id, u64 dtb, usize_t load_offset)
{
	boot_identity(hart_id, dtb, load_offset);
	boot_early_mm_init();
	boot_core_mm_init();
	boot_fdt_init();
	boot_arch_init();
	boot_drivers_init();
	boot_hart_online(hart_id);
	boot_subsys_init();
	boot_hart_irq_on();

#if ENABLE_SMP
	smp_wake_secondary_harts(hart_id);
#endif

	proc_scheduler();
}

#if ENABLE_SMP

void boot_run_secondary(u64 hart_id)
{
	write_tp(hart_id);

	write_satp(make_satp_sv39(symbol_phys(kernel_pgdir)));
	sfence_vma();

	boot_hart_online(hart_id);
	boot_hart_irq_on();

	proc_scheduler();
}

#endif
