#ifndef TMPFS_H
#define TMPFS_H

#include <arch/pgalloc.h>
#include <asm/page.h>
#include <brk/mm_types.h>
#include <brk/sleeplock_types.h>
#include <brk/types.h>

struct tmpfs_dir_entry {
	u32 d_ino;
	s32 d_off;
	u16 d_entry_len;
	u8 d_name_len;
	u8 d_type;
	char d_name[];
};

struct tmpfs_inode {
	struct page *i_page;
	u32 i_size;
	u32 i_no;
	umode_t i_mode;
	u32 i_nlink;
	u32 i_rdev;
	u32 i_atime;
	u32 i_atime_nsec;
	u32 i_mtime;
	u32 i_mtime_nsec;
	u32 i_ctime;
	u32 i_ctime_nsec;
	u32 i_uid;
	u32 i_gid;
};

struct tmpfs_super_block {
	struct page *s_imap;
	sleeplock_t s_lock;
	u32 s_icnt;
};

#define TMPFS_MAGIC 0x706d74

#define TMPFS_ROOT_INO 1
#define TMPFS_INODE_PER_PAGE (PAGE_SIZE / sizeof(struct tmpfs_inode))
#define TMPFS_DIR_ENT_MIN_LEN 24

#endif
