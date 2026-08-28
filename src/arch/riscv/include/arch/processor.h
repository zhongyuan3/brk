#ifndef ARCH_PROCESSOR_H
#define ARCH_PROCESSOR_H

#include <brk/base/types.h>

struct switch_frame {
	uint64_t ra;
	uint64_t sp;
	uint64_t s0;
	uint64_t s1;
	uint64_t s2;
	uint64_t s3;
	uint64_t s4;
	uint64_t s5;
	uint64_t s6;
	uint64_t s7;
	uint64_t s8;
	uint64_t s9;
	uint64_t s10;
	uint64_t s11;
};

void switch_context(struct switch_frame *prev, struct switch_frame *next);

#endif
