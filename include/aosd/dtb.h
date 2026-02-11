#ifndef AOSD_DTB_H
#define AOSD_DTB_H

#include <aosd/types.h>

int dtb_early_init_scan_mem(void);
int dtb_early_init_scan_reserved_mem(void);

extern uint64_t dtb_phys;

#endif
