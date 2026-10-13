#ifndef BRK_IOREMAP_H
#define BRK_IOREMAP_H

#include <brk/types.h>

void *ioremap(u64 paddr, size_t size, unsigned int flags);
void iounmap(void *addr, size_t size);

#endif
