#ifndef BRK_DTB_H
#define BRK_DTB_H

#include <brk/plic.h>
#include <brk/rtc.h>
#include <brk/types.h>
#include <brk/uart.h>

int dtb_early_init_scan_mem(void);
int dtb_early_init_scan_reserved_mem(void);
int dtb_init_scan_cpu(void);
int dtb_init_scan_virtio_dev(void);
void dtb_init_scan_serial(void);

int dtb_parse_plic(struct plic_device *plic);
int dtb_parse_uart(struct ns16550a_device *uart);
int dtb_parse_rtc(struct rtc_device *rtc);

extern uint64_t dtb_phys;

#endif
