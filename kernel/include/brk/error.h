#ifndef BRK_ERROR_H
#define BRK_ERROR_H

#include <brk/compiler.h>
#include <brk/types.h>

static inline __must_check void *__err_ptr(int err)
{
	return (void *)((unsigned long long)(err));
}

static inline __must_check int __ptr_err(const void *ptr)
{
	return (int)(unsigned long long)(ptr);
}

static inline __must_check bool __is_err(const void *ptr)
{
	return (ptr <= (void *)(-1) && ptr >= (void *)(-256));
}

static inline __must_check void *__err_cast(const void *ptr)
{
	return (void *)ptr;
}

#define ERR_PTR(err) __err_ptr(err)

#define PTR_ERR(ptr) __ptr_err(ptr)

#define IS_ERR(ptr) __is_err(ptr)

#define ERR_CAST(ptr) __err_cast(ptr)

#endif
