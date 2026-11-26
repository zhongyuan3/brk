#ifndef _BRK_RWSEM_H
#define _BRK_RWSEM_H

#include <brk/rwsem_types.h>
#include <stdbool.h>

void rwsem_init(rwsem_t *sem, const char *name);

/*
 * Recursive read locking is not supported: a second down_read() from the
 * same thread may deadlock if a writer is already waiting.
 */
void down_read(rwsem_t *sem);
int down_read_interruptible(rwsem_t *sem);
bool down_read_trylock(rwsem_t *sem);
void up_read(rwsem_t *sem);

void down_write(rwsem_t *sem);
int down_write_interruptible(rwsem_t *sem);
bool down_write_trylock(rwsem_t *sem);
void up_write(rwsem_t *sem);

bool rwsem_is_locked(const rwsem_t *sem);
bool rwsem_is_write_locked_by_me(const rwsem_t *sem);

#endif
