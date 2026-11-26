#ifndef _BRK_IOREMAP_H
#define _BRK_IOREMAP_H

#include <brk/types.h>
#include <stddef.h>

void *ioremap(phys_addr_t paddr, size_t size, unsigned int flags);
void iounmap(void *addr, size_t size);

#endif
