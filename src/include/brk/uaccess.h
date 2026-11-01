#ifndef BRK_UACCESS_H
#define BRK_UACCESS_H

#include <arch/asm/vmlayout.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

/*
 * This kernel does not implement demand paging fault fixups yet.  These
 * helpers at least validate that the range is inside the user address space
 * and keep user copies out of spinlock-protected critical sections.
 */
static inline bool user_range_ok(u64 addr, size_t len)
{
	if (addr > USER_SPACE_SIZE_MAX)
		return false;
	return len <= USER_SPACE_SIZE_MAX - addr;
}

static inline int copy_from_user(void *dst, const void *src, size_t len)
{
	if (len && !user_range_ok((u64)(uintptr_t)src, len))
		return -EFAULT;
	memcpy(dst, src, len);
	return 0;
}

static inline int copy_to_user(void *dst, const void *src, size_t len)
{
	if (len && !user_range_ok((u64)(uintptr_t)dst, len))
		return -EFAULT;
	memcpy(dst, src, len);
	return 0;
}

#endif
