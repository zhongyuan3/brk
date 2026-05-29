#include <brk/fs.h>
#include <brk/list.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/spinlock.h>

static LIST_DEFINE(super_blocks);
static SPINLOCK_DEFINE(sb_lock);

struct fs_super_block *fs_super_block_alloc(struct fs_driver *driver)
{
	struct fs_super_block *sb;

	sb = kzalloc(sizeof(*sb));
	if (!sb)
		return NULL;

	list_init(&sb->list);
	sb->driver = driver;
	list_init(&sb->instance);
	refcnt_init(&sb->count, 1);

	spinlock_init(&sb->inodes_lock, "super_block.inodes_lock");
	list_init(&sb->inodes);
	list_init(&sb->dirty_inodes);
	spinlock_init(&sb->mnt_states_lock, "super_block.mnt_states_lock");
	list_init(&sb->mnt_states);

	spinlock_acquire(&sb_lock);
	list_add_tail(&sb->list, &super_blocks);
	spinlock_release(&sb_lock);

	return sb;
}

void fs_super_block_free(struct fs_super_block *sb)
{
	spinlock_acquire(&sb_lock);
	list_del_init(&sb->list);
	spinlock_release(&sb_lock);

	kfree(sb);
}

void fs_super_block_get(struct fs_super_block *sb)
{
	refcnt_inc(&sb->count);
}

void fs_super_block_put(struct fs_super_block *sb)
{
	if (refcnt_dec_fetch(&sb->count) > 0)
		return;

	if (sb->ops->put_super)
		sb->ops->put_super(sb);
	fs_super_block_free(sb);
}
