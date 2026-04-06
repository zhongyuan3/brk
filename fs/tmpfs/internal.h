#ifndef FS_TMPFS_INTERNAL_H
#define FS_TMPFS_INTERNAL_H

#include <aosd/asm.h>
#include <aosd/lock.h>
#include <aosd/mm_types.h>
#include <aosd/types.h>

#define TMPFS_MAGIC 0x706d74

struct tmpfs_dir_entry {
	uint32_t d_ino;
	int32_t d_off;
	uint16_t d_entry_len;
	uint8_t d_name_len;
	uint8_t d_type;
	char d_name[];
};

struct tmpfs_inode {
	struct page *i_page;
	uint32_t i_size;
	uint32_t i_no;
	mode_t i_mode;
	uint32_t i_nlink;
	uint32_t i_rdev;
};

struct tmpfs_super_block {
	struct page *s_imap;
	spinlock_t s_lock;
	uint32_t s_icnt;
};

struct tmpfs_dir_iter {
	struct page *pg;
	off_t in_pg_off;
};

#define TMPFS_ROOT_INO 1

#define TMPFS_INODE_PER_PAGE (PAGE_SIZE / sizeof(struct tmpfs_inode))

#define TMPFS_DIR_ENT_MIN_LEN 24

#endif
