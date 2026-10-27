#ifndef BRK_KLOG_H
#define BRK_KLOG_H

/*
 * Start the kernel log thread.  It drains the printk ring buffer into
 * /kernel.log and, once the file is open, deactivates the UART kernel
 * console (the /dev/tty0 tty device remains usable).
 */
int klog_start(void);

#endif
