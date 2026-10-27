#ifndef ARCH_CONTEXT_H
#define ARCH_CONTEXT_H

struct riscv_context {
	unsigned long s0;
	unsigned long s1;
	unsigned long s2;
	unsigned long s3;
	unsigned long s4;
	unsigned long s5;
	unsigned long s6;
	unsigned long s7;
	unsigned long s8;
	unsigned long s9;
	unsigned long s10;
	unsigned long s11;
	unsigned long ra;
	unsigned long __pad;
};

#define CONTEXT_SIZE sizeof(struct riscv_context)

#endif
