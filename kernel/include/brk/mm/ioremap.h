#ifndef BRK_IOREMAP_H
#define BRK_IOREMAP_H

#include <brk/base/types.h>

void *ioremap(uint64_t paddr, size_t size, unsigned int flags);
void iounmap(void *addr, size_t size);

#endif
