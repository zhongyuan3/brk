#include <brk/init.h>
#include <brk/printk.h>
#include <brk/types.h>

void start_kernel(usize_t hart_id, u64 dtb, usize_t load_offset)
{
	boot_run_primary(hart_id, dtb, load_offset);
}

#if ENABLE_SMP

void start_hart(u64 hart_id)
{
	boot_run_secondary(hart_id);
}

#endif
