#ifndef BRK_DT_H
#define BRK_DT_H

#include <brk/plic.h>
#include <brk/rtc.h>
#include <brk/types.h>
#include <brk/uart.h>

int dtb_early_init_scan_mem(unsigned long dtb_addr);
int dtb_early_init_scan_reserved_mem(unsigned long dtb_addr);
int dtb_init_scan_cpu(unsigned long dtb_addr);
int dtb_scan_platform_devices(unsigned long dtb_addr);

int dtb_init_scan_virtio_dev(unsigned long dtb_addr);
void dtb_init_scan_serial(unsigned long dtb_addr);

int dtb_parse_plic(unsigned long dtb_addr, struct plic_device *plic);
int dtb_parse_uart(unsigned long dtb_addr, struct ns16550a_device *uart);
int dtb_parse_rtc(unsigned long dtb_addr, struct rtc_device *rtc);

#endif
