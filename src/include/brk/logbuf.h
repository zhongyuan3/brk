#ifndef BRK_LOGBUF_H
#define BRK_LOGBUF_H

#include <brk/types.h>

/* One record in the log ring buffer. 8-byte aligned in the buffer. */
struct log_record {
	uint32_t len; /* Text length; LOG_RECORD_MARK marks a wrap point */
	uint8_t level;
	uint8_t flags;
	uint16_t pad;
	uint64_t ts_nsec;
};

void log_store(uint8_t level, const char *text, uint32_t len);
void log_flush(void);

#endif