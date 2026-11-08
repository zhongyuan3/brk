#ifndef BRK_PRINTK_LOG_H
#define BRK_PRINTK_LOG_H

#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <stdalign.h>
#include <stddef.h>

#define PRINTK_RING_BUFFER_SIZE (1u << 16)
#define PRINTK_RING_BUFFER_MASK (PRINTK_RING_BUFFER_SIZE - 1u)

#define PRINTK_MAX_PAYLOAD 1024

#define PRINTK_LOG_MAGIC0 0x5a
#define PRINTK_LOG_MAGIC1 0xa5

#define PRINTK_FLAGS_CONTINUED (1u << 0) /* reserved: logical continuation */
#define PRINTK_FLAGS_MORE (1u << 1) /* reserved: more chunks follow */
#define PRINTK_FLAGS_WRAP (1u << 2) /* ring-boundary padding record */
#define PRINTK_FLAGS_TRUNCATED (1u << 3) /* payload was truncated */

struct printk_log_header {
	u8 magic[2];
	u8 level;
	u8 flags;
	u32 len;
	u64 nsec;
};

struct printk_ring_buffer {
	alignas(alignof(
		struct printk_log_header)) char buf[PRINTK_RING_BUFFER_SIZE];
	u64 head;
	u64 tail;
	spinlock_t lock;
};

struct printk_log_reader {
	struct printk_ring_buffer *ring;
	u64 cursor;
	u64 pending_lost;
};

enum printk_log_start {
	PRINTK_LOG_START_OLDEST,
	PRINTK_LOG_START_NEWEST,
};

size_t printk_log_write(u8 level, const char *text, size_t len, u64 nsec);

void printk_log_reader_init(struct printk_log_reader *reader,
			    enum printk_log_start start);

/*
 * Read one record.  Returns the payload length, 0 if the ring is empty,
 * -ENOSPC if the record did not fit in @buf (the record is consumed either
 * way), or -EIO after a corrupt record has been skipped.  The caller must
 * check reader->pending_lost after every call, including when 0 is returned.
 */
int printk_log_read(struct printk_log_reader *reader, char *buf, size_t size,
		    u8 *level, u64 *nsec, u8 *flags);

/*
 * Non-blocking variant for the panic path.  Returns -EAGAIN if the ring lock
 * is currently held by another CPU.
 */
int printk_log_read_try(struct printk_log_reader *reader, char *buf,
			size_t size, u8 *level, u64 *nsec, u8 *flags);

#endif
