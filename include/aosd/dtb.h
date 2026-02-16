#ifndef AOSD_DTB_H
#define AOSD_DTB_H

#include <aosd/plic.h>
#include <aosd/types.h>
#include <aosd/uart.h>

int dtb_early_init_scan_mem(void);
int dtb_early_init_scan_reserved_mem(void);
int dtb_init_scan_cpu(void);
int dtb_init_scan_virtio_dev(void);

int dtb_parse_plic(struct plic_device *plic);
int dtb_parse_uart(struct uart_device *uart);

extern uint64_t dtb_phys;
extern void *dtb_virt;

#endif
