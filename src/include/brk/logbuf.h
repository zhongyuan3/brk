#ifndef BRK_LOGBUF_H
#define BRK_LOGBUF_H

#include <brk/types.h>

/* One record in the log ring buffer. 8-byte aligned in the buffer. */
struct log_record {
	u32 len; /* Text length; LOG_RECORD_MARK marks a wrap point */
	u8 level;
	u8 flags;
	u16 pad;
	u64 ts_nsec;
};

void log_store(u8 level, const char *text, u32 len);
void log_flush(void);

#endif