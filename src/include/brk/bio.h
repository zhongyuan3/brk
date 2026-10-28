#ifndef BRK_BIO_H
#define BRK_BIO_H

#include <brk/list.h>
#include <brk/sleeplock_types.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>

struct block_dev;
struct gendisk;
struct request;
struct request_queue;
struct task;

/*
 * Block I/O layer.
 *
 * A bio is one logical I/O against a block device view (whole disk or
 * partition).  Buffers are kernel virtual addresses: this kernel maps all
 * physical memory through the direct map, and the driver converts them with
 * virt_to_phys().
 *
 * Bios are grouped into requests by the request queue.  Adjacent bios with
 * the same operation are merged.  A per-queue worker thread calls the disk
 * driver's queue_rq() callback, so completion is asynchronous with respect to
 * submission; submit_bio_wait() is the synchronous wrapper used by the
 * filesystem and page cache.
 */
#define BIO_MAX_VECS 8

enum req_op {
	REQ_OP_READ = 0,
	REQ_OP_WRITE = 1,
	REQ_OP_FLUSH = 2,
};

struct bio_vec {
	void *bv_buf;
	unsigned int bv_len;
};

struct bio {
	struct block_dev *bi_bdev;
	/* Sector within bi_bdev, in 512-byte units. */
	u64 bi_sector;
	/* Translated sector within the whole disk (filled by submit_bio). */
	u64 bi_disk_sector;
	unsigned int bi_size;
	unsigned int bi_nr_vecs;
	enum req_op bi_op;
	int bi_status;
	bool bi_done;
	struct bio *bi_next;
	struct bio_vec bi_io_vec[BIO_MAX_VECS];
};

struct request {
	struct list_head rq_list;
	struct request_queue *rq_queue;
	struct bio *rq_bio;
	struct bio *rq_bio_tail;
	u64 rq_start;
	unsigned int rq_nr_sectors;
	enum req_op rq_op;
	int rq_status;
};

typedef int (*request_fn_t)(struct request_queue *, struct request *);

struct request_queue {
	spinlock_t lock;
	sleeplock_t worker_lock;
	struct list_head queue;
	request_fn_t queue_rq;
	struct gendisk *disk;
	struct task *worker;
	bool dead;
	bool worker_done;
};

void bio_init(struct bio *bio, struct block_dev *bd, enum req_op op);
int bio_add_buf(struct bio *bio, void *buf, size_t len);

static inline unsigned int bio_sectors(const struct bio *bio)
{
	return bio->bi_size >> 9;
}

int blk_init_queue(struct gendisk *disk, request_fn_t fn);
void blk_cleanup_queue(struct request_queue *q);

int submit_bio_wait(struct bio *bio);

#endif
