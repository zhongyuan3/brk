#include <brk/fs.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/refcnt.h>
#include <brk/slab.h>

static LIST_DEFINE(super_blocks);
static SPINLOCK_DEFINE(sb_lock);

struct fs_state *alloc_super(struct fs_driver *type)
{
	struct fs_state *sb;

	sb = kzalloc(sizeof(*sb));
	if (!sb)
		return NULL;

	list_init(&sb->s_list);
	sb->s_type = type;
	list_init(&sb->s_instances);
	refcnt_init(&sb->s_count, 1);

	sleeplock_init(&sb->s_lock, "super_block.s_lock");
	spinlock_init(&sb->s_inode_lock, "super_block.s_inode_lock");
	list_init(&sb->s_inodes);
	list_init(&sb->s_dirty);
	spinlock_init(&sb->s_mount_lock, "super_block.s_mount_lock");
	list_init(&sb->s_mounts);

	spinlock_acquire(&sb_lock);
	list_add_tail(&sb->s_list, &super_blocks);
	spinlock_release(&sb_lock);

	return sb;
}

void free_super(struct fs_state *sb)
{
	spinlock_acquire(&sb_lock);
	list_del_init(&sb->s_list);
	spinlock_release(&sb_lock);

	kfree(sb);
}

void super_dup(struct fs_state *sb)
{
	refcnt_inc(&sb->s_count);
}

void super_put(struct fs_state *sb)
{
	if (refcnt_dec_fetch(&sb->s_count) > 0)
		return;

	if (sb->s_op->put_super)
		sb->s_op->put_super(sb);
	free_super(sb);
}
