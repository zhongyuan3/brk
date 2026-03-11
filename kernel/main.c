#include <aosd/asm.h>
#include <aosd/console.h>
#include <aosd/cpu.h>
#include <aosd/dcache.h>
#include <aosd/dev.h>
#include <aosd/dtb.h>
#include <aosd/fs.h>
#include <aosd/irq.h>
#include <aosd/lock.h>
#include <aosd/memblock.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/plic.h>
#include <aosd/printk.h>
#include <aosd/process.h>
#include <aosd/process_types.h>
#include <aosd/riscv.h>
#include <aosd/sbi.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/timer.h>
#include <aosd/trap.h>
#include <aosd/uart.h>
#include <aosd/virtio.h>
#include <aosd/virtio_blk.h>
#include <aosd/vmalloc.h>
#include <libfdt.h>

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

void start_kernel(size_t hart_id, uint64_t dtb, size_t load_offset)
{
	kernel_load_offset = load_offset;
	dtb_phys = dtb;
	dtb_virt = (void *)dtb;

	cpu_init_hart(hart_id);

	memblock_init();
	memblock_reserve(_SKERNEL_PHYS, _KERNEL_SIZE);
	memblock_reserve(dtb_phys,
			 align_up(fdt_totalsize(dtb_phys), PAGE_SIZE));
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

	dev_init();
	dentry_cache_init();
	file_cache_init();
	inode_cache_init();

	proc_init();

	start_other_harts(hart_id);

	proc_scheduler();
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

	proc_scheduler();
}
