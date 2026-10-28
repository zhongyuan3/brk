#include <brk/bio.h>
#include <brk/blkdev.h>
#include <brk/genhd.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/printk.h>
#include <brk/printf.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <uapi/errno.h>

void bio_init(struct bio *bio, struct block_dev *bd, enum req_op op)
{
	memset(bio, 0, sizeof(*bio));
	bio->bi_bdev = bd;
	bio->bi_op = op;
}

int bio_add_buf(struct bio *bio, void *buf, size_t len)
{
	if (!bio || !buf || len == 0 || (len & 511))
		return -EINVAL;
	if (bio->bi_nr_vecs >= BIO_MAX_VECS)
		return -ENOSPC;

	bio->bi_io_vec[bio->bi_nr_vecs].bv_buf = buf;
	bio->bi_io_vec[bio->bi_nr_vecs].bv_len = (unsigned int)len;
	bio->bi_nr_vecs++;
	bio->bi_size += (unsigned int)len;
	return 0;
}

static struct request_queue *bio_lookup_queue(struct bio *bio,
					      u64 *disk_sector)
{
	struct block_dev *bd = bio->bi_bdev;
	u64 sector = bio->bi_sector;

	while (bd && bd->bd_parent) {
		sector += bd->bd_start_sect;
		bd = bd->bd_parent;
	}

	if (!bd || !bd->bd_queue)
		return NULL;

	*disk_sector = sector;
	return bd->bd_queue;
}

static void blk_queue_worker(void *arg)
{
	struct request_queue *q = arg;

	for (;;) {
		struct request *rq;
		struct bio *bio, *next;
		irqstate_t flags;

		flags = spin_lock_irqsave(&q->lock);
		while (list_empty(&q->queue) && !q->dead)
			task_sleep(&q->queue, &q->lock);

		if (q->dead) {
			/* Fail everything still queued, then let cleanup run. */
			while (!list_empty(&q->queue)) {
				rq = list_first_entry(&q->queue, struct request,
						      rq_list);
				list_del_init(&rq->rq_list);
				for (bio = rq->rq_bio; bio; bio = next) {
					next = bio->bi_next;
					bio->bi_status = -ENODEV;
					bio->bi_done = true;
					task_wake_all(bio);
				}
				kfree(rq);
			}
			q->worker = NULL;
			q->worker_done = true;
			task_wake_all(&q->worker_done);
			spin_unlock_irqrestore(&q->lock, flags);
			return;
		}

		rq = list_first_entry(&q->queue, struct request, rq_list);
		list_del_init(&rq->rq_list);
		spin_unlock_irqrestore(&q->lock, flags);

		rq->rq_status = q->queue_rq(q, rq);

		flags = spin_lock_irqsave(&q->lock);
		for (bio = rq->rq_bio; bio; bio = next) {
			next = bio->bi_next;
			bio->bi_status = rq->rq_status;
			bio->bi_done = true;
			task_wake_all(bio);
		}
		spin_unlock_irqrestore(&q->lock, flags);
		kfree(rq);
	}
}

static int blk_queue_ensure_worker(struct request_queue *q)
{
	int err = 0;

	if (__atomic_load_n(&q->worker, __ATOMIC_ACQUIRE))
		return 0;

	sleeplock_acquire(&q->worker_lock);
	if (!q->worker) {
		struct task *task;
		irqstate_t flags;

		flags = spin_lock_irqsave(&q->lock);
		if (q->dead) {
			spin_unlock_irqrestore(&q->lock, flags);
			err = -ENODEV;
			goto out;
		}
		spin_unlock_irqrestore(&q->lock, flags);

		task = kthread_create(blk_queue_worker, q);
		if (!task) {
			err = -ENOMEM;
			goto out;
		}
		snprintf(task->name, sizeof(task->name), "blk:%s",
			 q->disk->disk_name);
		q->worker = task;
		kthread_join(task);
	}
out:
	sleeplock_release(&q->worker_lock);
	return err;
}

int blk_init_queue(struct gendisk *disk, request_fn_t fn)
{
	struct request_queue *q;

	if (!disk || !fn)
		return -EINVAL;

	q = kzalloc(sizeof(*q));
	if (!q)
		return -ENOMEM;

	spinlock_init(&q->lock, "request_queue");
	sleeplock_init(&q->worker_lock, "request_queue_worker");
	list_init(&q->queue);
	q->queue_rq = fn;
	q->disk = disk;
	disk->queue = q;
	disk->bdev->bd_queue = q;
	return 0;
}

void blk_cleanup_queue(struct request_queue *q)
{
	irqstate_t flags;

	if (!q)
		return;

	flags = spin_lock_irqsave(&q->lock);
	q->dead = true;
	task_wake_all(&q->queue);
	while (q->worker && !q->worker_done)
		task_sleep(&q->worker_done, &q->lock);
	spin_unlock_irqrestore(&q->lock, flags);

	kfree(q);
}

int submit_bio_wait(struct bio *bio)
{
	struct request_queue *q;
	struct request *rq;
	struct request *last;
	u64 disk_sector;
	irqstate_t flags;
	int err;

	if (!bio || !bio->bi_bdev)
		return -EINVAL;

	if (bio->bi_op == REQ_OP_FLUSH) {
		if (bio->bi_size)
			return -EINVAL;
	} else if (bio->bi_size == 0 || (bio->bi_size & 511)) {
		return -EINVAL;
	}

	if (bio->bi_bdev->phy_bsize != 512)
		return -EOPNOTSUPP;

	if (bio->bi_op != REQ_OP_FLUSH) {
		if (bio->bi_sector >= bio->bi_bdev->phy_bcnt ||
		    bio_sectors(bio) >
			    bio->bi_bdev->phy_bcnt - bio->bi_sector)
			return -ENXIO;
	}

	q = bio_lookup_queue(bio, &disk_sector);
	if (!q)
		return -ENODEV;

	err = blk_queue_ensure_worker(q);
	if (err)
		return err;

	bio->bi_disk_sector = disk_sector;
	bio->bi_status = 0;
	bio->bi_done = false;
	bio->bi_next = NULL;

	rq = kzalloc(sizeof(*rq));
	if (!rq)
		return -ENOMEM;

	rq->rq_queue = q;
	rq->rq_bio = rq->rq_bio_tail = bio;
	rq->rq_start = disk_sector;
	rq->rq_nr_sectors = bio_sectors(bio);
	rq->rq_op = bio->bi_op;

	flags = spin_lock_irqsave(&q->lock);
	if (q->dead) {
		spin_unlock_irqrestore(&q->lock, flags);
		kfree(rq);
		return -ENODEV;
	}

	last = list_empty(&q->queue) ?
		       NULL :
		       list_last_entry(&q->queue, struct request, rq_list);

	/*
	 * Merge adjacent read/write bios into the queued request.  The worker
	 * removes a request from the list before dispatching it, so a request
	 * is only extended while it is still waiting for dispatch.
	 */
	if (last && bio->bi_op != REQ_OP_FLUSH && last->rq_op == bio->bi_op &&
	    last->rq_start + last->rq_nr_sectors == disk_sector) {
		last->rq_bio_tail->bi_next = bio;
		last->rq_bio_tail = bio;
		last->rq_nr_sectors += bio_sectors(bio);
		spin_unlock_irqrestore(&q->lock, flags);
		kfree(rq);
	} else {
		list_add_tail(&rq->rq_list, &q->queue);
		spin_unlock_irqrestore(&q->lock, flags);
		task_wake_all(&q->queue);
	}

	flags = spin_lock_irqsave(&q->lock);
	while (!bio->bi_done)
		task_sleep(bio, &q->lock);
	spin_unlock_irqrestore(&q->lock, flags);

	return bio->bi_status;
}

