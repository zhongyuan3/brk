#ifndef BRK_MM_TYPES_H
#define BRK_MM_TYPES_H

#include <arch/pgtable_types.h>
#include <brk/refcnt_types.h>
#include <brk/types.h>

struct cached_page;
struct fs_inode;
struct fs_file;

/* VMA backing store. */
#define UVM_REGION_ANON 0
#define UVM_REGION_SHARED_FILE 1

struct page {
	unsigned int flags;

	union {
		struct { /* Buddy */
			struct list_head buddy_lru;
			/* Only valid when PAGE_FLAGS_HEAD is set */
			unsigned int buddy_page_order;
		};

		struct { /* SLAB */
			void *slab_free_objs;
			struct slab_allocator *slab_cache;
			struct list_head slab_list;
			size_t slab_free_count;
			size_t slab_objs_count;
		};

		struct { /* tmpfs */
			struct page *tmpfs_next;
		};
	};
};

struct uvm_region {
	struct list_head list;
	u64 addr;
	size_t size;
	struct page **pages;
	struct cached_page **cached;
	size_t nr_pages;
	unsigned int flags;
	unsigned int kind;

	/* Shared file mapping: keep the inode and file offset alive. */
	struct fs_inode *inode;
	struct fs_file *file;
	loff_t file_offset;
};

struct uvm_space {
	pgd_t *pgd;
	struct list_head seg;
	struct uvm_region *stack;
	struct uvm_region *heap;
	u64 brk;
	/* Top-down mmap allocation cursor, below the user stack. */
	u64 mmap_top;
	refcnt_t refcnt;
};

#endif
