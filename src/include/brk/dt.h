#ifndef BRK_DT_H
#define BRK_DT_H

#include <brk/rtc.h>
#include <brk/types.h>
#include <brk/uart.h>

void dt_set_dtb(unsigned long dtb);
void *dt_get_fdt(void);

int dt_init_scan_mem(void);
int dt_init_scan_rsvd_mem(void);

int dt_init_scan_cpu(void);

int dt_init_scan_virtio_dev(void);
void dt_init_scan_serial(void);

int dt_parse_rtc(struct rtc_device *rtc);

int dt_setup_timebase_freq(void);

/* Read the index'th "reg" entry of a DT node. */
int dt_get_reg(int node, int index, u64 *addr, u64 *size);

#endif
