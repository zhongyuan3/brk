#ifndef ARCH_PROCESSOR_H
#define ARCH_PROCESSOR_H

#include <brk/base/types.h>

#if __riscv_xlen == 32
typedef uint32_t reg_t;
#else
typedef uint64_t reg_t;
#endif

struct switch_frame {
	reg_t ra;
	reg_t sp;
	reg_t s0;
	reg_t s1;
	reg_t s2;
	reg_t s3;
	reg_t s4;
	reg_t s5;
	reg_t s6;
	reg_t s7;
	reg_t s8;
	reg_t s9;
	reg_t s10;
	reg_t s11;
};

void switch_context(struct switch_frame *prev, struct switch_frame *next);

#endif
