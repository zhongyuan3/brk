#ifndef BRK_DT_H
#define BRK_DT_H

#include <brk/types.h>

void dt_set_fdt(unsigned long addr);
void *dt_get_fdt(void);

int dt_init_scan_mem(void);
int dt_init_scan_rsvd_mem(void);

int dt_init_scan_cpu(void);

int dt_get_timebase_freq(u32 *freq_out);

#endif
