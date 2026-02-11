#ifndef AOSD_IOREMAP_H
#define AOSD_IOREMAP_H

#include <aosd/types.h>

void *ioremap(uint64_t paddr, size_t size, unsigned int flags);
void iounmap(void *addr, size_t size);

#endif
