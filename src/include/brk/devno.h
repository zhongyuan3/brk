#ifndef BRK_DEVNO_H
#define BRK_DEVNO_H

#include <brk/bitmap.h>
#include <brk/device.h>
#include <brk/list.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>

/*
 * Common major/minor region allocator used by both the character and block
 * device registries.  The two classes have independent number spaces, so
 * each registry owns one devno_allocator.
 */
struct devno_region {
	struct hlist_node entry;
	unsigned int minor_start;
	unsigned int minor_count;
};

struct devno_allocator {
	struct hlist_head regions[MAJOR_MAX];
	unsigned long major_pool[BITS_TO_LONGS(MAJOR_MAX)];
	spinlock_t lock;
};

void devno_allocator_init(struct devno_allocator *a, unsigned int first_dynamic);

int devno_alloc_major(struct devno_allocator *a, unsigned int *major_out);
void devno_free_major(struct devno_allocator *a, unsigned int major);

int devno_alloc_minor(struct devno_allocator *a, unsigned int major,
		      unsigned int *minor_out);
void devno_free_minor(struct devno_allocator *a, unsigned int major,
		      unsigned int minor);

int devno_alloc_region(struct devno_allocator *a, unsigned int major,
		       unsigned int base_minor, unsigned int count,
		       dev_t *dev_out);
void devno_free_region(struct devno_allocator *a, unsigned int major,
		       unsigned int minor, unsigned int count);

bool devno_minor_is_allocated(struct devno_allocator *a, unsigned int major,
			      unsigned int minor);

#endif
