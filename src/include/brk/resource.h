#ifndef BRK_RESOURCE_H
#define BRK_RESOURCE_H

#include <brk/types.h>

#define IORESOURCE_MEM (1u << 0)
#define IORESOURCE_IRQ (1u << 1)

struct resource {
	u64 start;
	u64 end;
	unsigned long flags;
	const char *name;
};

static inline u64 resource_size(const struct resource *res)
{
	if (!res || res->end < res->start)
		return 0;
	return res->end - res->start + 1;
}

#endif
