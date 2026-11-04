#ifndef BRK_NET_IRQTEST_H
#define BRK_NET_IRQTEST_H

#include <stddef.h>

int irq_exttest_run(const char *buf, size_t size);
int irq_exttest_show(char *buf, size_t size);

#endif
