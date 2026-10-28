#ifndef BRK_DEVNODE_H
#define BRK_DEVNODE_H

#include <brk/types.h>

/*
 * A small /dev node registry.  Drivers register a name and dev_t at probe
 * time; devnode_populate() creates the actual nodes once /dev is mounted.
 * This keeps filesystem operations out of driver registration locks and
 * avoids one xxx_mknod() function per subsystem.
 */
int devnode_register(const char *path, dev_t dev, umode_t mode);
void devnode_unregister(const char *path);
int devnode_populate(void);

#endif
