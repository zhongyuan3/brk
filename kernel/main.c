#include <aosd/dtb.h>
#include <aosd/mm.h>
#include <aosd/printk.h>

void start_kernel(size_t hart_id, uint64_t dtb, size_t load_offset)
{
	kernel_map.load_offset = load_offset;
	dtb_phys = dtb;
	mm_init();
	printk("kernel starting\n");
	printk("kernel starting\n");
	printk("kernel starting\n");

	while (1) {
	}
}
