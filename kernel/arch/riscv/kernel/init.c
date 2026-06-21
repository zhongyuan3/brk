#include <arch/mm.h>
#include <brk/kernel/dtb.h>
#include <brk/kernel/init.h>
#include <brk/mm/memblock.h>
#include <libfdt.h>

void arch_init(void)
{
	memblock_reserve(_SKERNEL_PHYS, _KERNEL_SIZE);
	usize_t dtb_size = round_up(fdt_totalsize(dtb_phys), PAGE_SIZE);
	memblock_reserve(dtb_phys, dtb_size);
	dtb_early_init_scan_mem();
	dtb_early_init_scan_reserved_mem();
	phys_ram_base = memblock_get_ram_base();
	paging_init();
}
