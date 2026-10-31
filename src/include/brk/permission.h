#ifndef BRK_PERMISSION_H
#define BRK_PERMISSION_H

#include <brk/types.h>

struct fs_inode;

/* Values match the low three permission bits. */
#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4

int inode_permission(const struct fs_inode *inode, int mask);
bool inode_owner_or_capable(const struct fs_inode *inode);

#endif
