#ifndef UAPI_AUXV_H
#define UAPI_AUXV_H

/*
 * Auxiliary vector: key/value pairs the kernel appends after the
 * envp NULL terminator on the initial stack.  ld.so and crt code
 * walk it from the envp end pointer.
 */

#define AT_NULL 0 /* end of vector */
#define AT_IGNORE 1
#define AT_PHDR 3 /* program headers of the image */
#define AT_PHENT 4 /* size of one program header */
#define AT_PHNUM 5 /* number of program headers */
#define AT_PAGESZ 6 /* system page size */
#define AT_BASE 7 /* interpreter/PIE load base */
#define AT_FLAGS 8
#define AT_ENTRY 9 /* entry point of the image */
#define AT_UID 11 /* real user id */
#define AT_EUID 12 /* effective user id */
#define AT_GID 13 /* real group id */
#define AT_EGID 14 /* effective group id */
#define AT_SECURE 23 /* exec of a set-id binary */
#define AT_RANDOM 25 /* 16 stack-canary bytes */
#define AT_EXECFN 31 /* pathname used to exec */

#endif
