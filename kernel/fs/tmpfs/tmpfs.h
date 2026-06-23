#ifndef TMPFS_H
#define TMPFS_H

#include <asm/page.h>
#include <brk/mm_types.h>
#include <brk/sleeplock_types.h>
#include <brk/types.h>

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
	umode_t i_mode;
	uint32_t i_nlink;
	uint32_t i_rdev;
	uint32_t i_atime;
	uint32_t i_atime_nsec;
	uint32_t i_mtime;
	uint32_t i_mtime_nsec;
	uint32_t i_ctime;
	uint32_t i_ctime_nsec;
	uint32_t i_uid;
	uint32_t i_gid;
};

struct tmpfs_super_block {
	struct page *s_imap;
	sleeplock_t s_lock;
	uint32_t s_icnt;
};

#define TMPFS_MAGIC 0x706d74

#define TMPFS_ROOT_INO 1
#define TMPFS_INODE_PER_PAGE (PAGE_SIZE / sizeof(struct tmpfs_inode))
#define TMPFS_DIR_ENT_MIN_LEN 24

#endif
