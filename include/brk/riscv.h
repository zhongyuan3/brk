#ifndef BRK_RISCV_H
#define BRK_RISCV_H

#include <brk/types.h>

static inline u64 read_tp(void)
{
	u64 x;
	asm volatile("mv %0, tp" : "=r"(x));
	return x;
}

static inline void write_tp(u64 x)
{
	asm volatile("mv tp, %0" : : "r"(x));
}

#define SSTATUS_SPP (1 << 8) /* Previous mode, 1=Supervisor, 0=User */
#define SSTATUS_SPIE (1 << 5) /* Supervisor Previous Interrupt Enable */
#define SSTATUS_UPIE (1 << 4) /* User Previous Interrupt Enable */
#define SSTATUS_SIE (1 << 1) /* Supervisor Interrupt Enable */
#define SSTATUS_UIE (1 << 0) /* User Interrupt Enable */
#define SSTATUS_SUM (1 << 18) /* Permit Supervisor User Memory access */

static inline u64 read_sstatus(void)
{
	u64 x;
	asm volatile("csrr %0, sstatus" : "=r"(x));
	return x;
}

static inline void write_sstatus(u64 x)
{
	asm volatile("csrw sstatus, %0" : : "r"(x));
}

static inline u64 read_satp(void)
{
	u64 x;
	asm volatile("csrr %0, satp" : "=r"(x));
	return x;
}

static inline u64 make_satp_sv39(u64 pt)
{
	return ((u64)8 << 60) | (pt >> 12);
}

static inline void write_satp(u64 x)
{
	asm volatile("csrw satp, %0" : : "r"(x));
}

static inline void sfence_vma(void)
{
	asm volatile("sfence.vma zero, zero");
}

static inline void fence_i(void)
{
	asm volatile("fence.i" ::: "memory");
}

static inline u64 read_stvec(void)
{
	u64 x;
	asm volatile("csrr %0, stvec" : "=r"(x));
	return x;
}

static inline void write_stvec(u64 x)
{
	asm volatile("csrw stvec, %0" : : "r"(x));
}

static inline u64 read_scause(void)
{
	u64 x;
	asm volatile("csrr %0, scause" : "=r"(x));
	return x;
}

static inline u64 read_sepc(void)
{
	u64 x;
	asm volatile("csrr %0, sepc" : "=r"(x));
	return x;
}

static inline void write_sepc(u64 x)
{
	asm volatile("csrw sepc, %0" : : "r"(x));
}

static inline u64 read_stval(void)
{
	u64 x;
	asm volatile("csrr %0, stval" : "=r"(x));
	return x;
}

#define SIE_SEIE (1L << 9) /* External */
#define SIE_STIE (1L << 5) /* Timer */
#define SIE_SSIE (1L << 1) /* Software */

#define SIP_SSIP (1L << 1) /* Supervisor software interrupt pending */

static inline u64 read_sie(void)
{
	u64 x;
	asm volatile("csrr %0, sie" : "=r"(x));
	return x;
}

static inline void write_sie(u64 x)
{
	asm volatile("csrw sie, %0" : : "r"(x));
}

static inline u64 read_time(void)
{
	u64 time;
	asm volatile("csrr %0, time" : "=r"(time));
	return time;
}

static inline void intr_on(void)
{
	write_sstatus(read_sstatus() | SSTATUS_SIE);
}

static inline void intr_off(void)
{
	write_sstatus(read_sstatus() & ~SSTATUS_SIE);
}

static inline bool intr_enabled(void)
{
	return read_sstatus() & SSTATUS_SIE;
}

/**
 * @brief Atomically disable the interrupts and return the previous state
 *
 * @return true if interrupts were enabled
 * @return false if interrupts were disabled
 */
static inline bool intr_off_get(void)
{
	u64 old;
	u64 i = SSTATUS_SIE;
	asm volatile("csrrc %0, sstatus, %1" : "=r"(old) : "r"(i));
	return (old & SSTATUS_SIE) != 0;
}

static inline u64 read_sscratch(void)
{
	u64 x;
	asm volatile("csrr %0, sscratch" : "=r"(x));
	return x;
}

static inline void write_sscratch(u64 x)
{
	asm volatile("csrw sscratch, %0" : : "r"(x));
}

static inline void clear_ssip_csr(void)
{
	asm volatile("csrc sip, %0" : : "r"((u64)SIP_SSIP));
}

#endif
