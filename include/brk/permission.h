#ifndef _BRK_PERMISSION_H
#define _BRK_PERMISSION_H

#include <brk/types.h>
#include <stdbool.h>
#include <uapi/capability.h>

struct inode;

/* Values match the low three permission bits. */
#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4

int inode_permission(const struct inode *inode, int mask);
bool inode_owner_or_capable(const struct inode *inode);
bool capable(int cap);

#endif
