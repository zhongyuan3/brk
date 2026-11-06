#ifndef BRK_NETIRQTEST_H
#define BRK_NETIRQTEST_H

#include <stddef.h>

int irq_exttest_run(const char *buf, size_t size);
int irq_exttest_show(char *buf, size_t size);

#endif
