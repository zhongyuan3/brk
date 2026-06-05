#ifndef BRK_INIT_H
#define BRK_INIT_H

#include <brk/types.h>

/*
 * Boot is split into phases; see kernel/init.c.
 *
 * VFS slab caches are initialized on the boot CPU (boot_subsys_init).
 * Mounting the root filesystem happens later in the init process (fs_init).
 */

void boot_run_primary(usize_t hart_id, u64 dtb, usize_t load_offset);

#if ENABLE_SMP
void boot_run_secondary(u64 hart_id);
void smp_boot_release_secondary_harts(void);
#endif

#endif
