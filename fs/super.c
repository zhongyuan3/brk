#include <aosd/assert.h>
#include <aosd/dcache.h>
#include <aosd/fs.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/slab.h>

static LIST_DEFINE(sblist);
static SPINLOCK_DEFINE(sblist_lock);

struct super_block *sblock_alloc(void)
{
	struct super_block *sb;

	sb = kzalloc(sizeof(*sb));
	if (!sb)
		return NULL;
	sb->s_rc = 1;
	return sb;
}

void sblock_free(struct super_block *sb)
{
	kfree(sb);
}

int sblock_add(struct super_block *sb)
{
	spinlock_acquire(&sblist_lock);
	list_add_tail(&sb->s_list, &sblist);
	spinlock_release(&sblist_lock);
	return 0;
}

refcnt_t sblock_rc(struct super_block *sb)
{
	refcnt_t rc;
	spinlock_acquire(&sblist_lock);
	rc = sb->s_rc;
	spinlock_release(&sblist_lock);
	return rc;
}

struct super_block *sblock_dup(struct super_block *sb)
{
	assert(sb->s_rc > 0);
	spinlock_acquire(&sblist_lock);
	++sb->s_rc;
	spinlock_release(&sblist_lock);
	return sb;
}

void sblock_put(struct super_block *sb)
{
	assert(sb->s_rc > 0);
	spinlock_acquire(&sblist_lock);
	--sb->s_rc;
	if (sb->s_rc == 1) {
		spinlock_release(&sblist_lock);
		dentry_put(sb->s_root);
	} else if (sb->s_rc == 0) {
		list_del(&sb->s_list);
		spinlock_release(&sblist_lock);
		sb->s_fs_type->deinit_sb(sb);
		sblock_free(sb);
	} else {
		spinlock_release(&sblist_lock);
	}
}
