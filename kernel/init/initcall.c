#include <brk/init/initcall.h>

extern initcall_t __initcall_start[];
extern initcall_t __initcall_end[];

void do_initcalls(void)
{
	initcall_t *fn;

	for (fn = __initcall_start; fn < __initcall_end; ++fn)
		(*fn)();
}