#ifndef BRK_DT_H
#define BRK_DT_H

#include <brk/plic.h>
#include <brk/rtc.h>
#include <brk/types.h>
#include <brk/uart.h>

void dt_set_dtb(unsigned long dtb);

int dt_init_scan_mem(void);
int dt_init_scan_rsvd_mem(void);

int dt_init_scan_cpu(void);
int dt_scan_platform_devices(void);

int dt_init_scan_virtio_dev(void);
void dt_init_scan_serial(void);

int dt_parse_plic(struct plic_device *plic);
int dt_parse_uart(struct ns16550a_device *uart);
int dt_parse_rtc(struct rtc_device *rtc);

int dt_setup_timebase_freq(void);

#endif
