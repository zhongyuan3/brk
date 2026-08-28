#ifndef BRK_INITCALL_H
#define BRK_INITCALL_H

typedef void (*initcall_t)(void);

#define __define_initcall(fn, id)             \
	static initcall_t __initcall_##fn##id \
		__attribute__((__used__, __section__(".initcall" #id))) = fn

/*
 * The levels run in order in do_initcalls().  Anything that other
 * initcalls depend on must go into an earlier level.
 */
#define core_initcall(fn) __define_initcall(fn, 0)
#define postcore_initcall(fn) __define_initcall(fn, 1)
#define subsys_initcall(fn) __define_initcall(fn, 2)
#define device_initcall(fn) __define_initcall(fn, 3)

void do_initcalls(void);

#endif