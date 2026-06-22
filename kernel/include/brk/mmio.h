#ifndef BRK_MMIO_H
#define BRK_MMIO_H

#include <brk/types.h>

static inline u8 mmio_read8(volatile void *addr)
{
	return *(volatile u8 *)addr;
}

static inline void mmio_write8(u8 val, volatile void *addr)
{
	*(volatile u8 *)addr = val;
}

static inline u16 mmio_read16(volatile void *addr)
{
	return *(volatile u16 *)addr;
}

static inline void mmio_write16(u16 val, volatile void *addr)
{
	*(volatile u16 *)addr = val;
}

static inline u32 mmio_read32(volatile void *addr)
{
	return *(volatile u32 *)addr;
}

static inline void mmio_write32(u32 val, volatile void *addr)
{
	*(volatile u32 *)addr = val;
}

static inline u64 mmio_read64(volatile void *addr)
{
	return *(volatile u64 *)addr;
}

static inline void mmio_write64(u64 val, volatile void *addr)
{
	*(volatile u64 *)addr = val;
}

#define readb(addr) mmio_read8((volatile void *)(addr))
#define writeb(val, addr) mmio_write8((val), (volatile void *)(addr))
#define readw(addr) mmio_read16((volatile void *)(addr))
#define writew(val, addr) mmio_write16((val), (volatile void *)(addr))
#define readl(addr) mmio_read32((volatile void *)(addr))
#define writel(val, addr) mmio_write32((val), (volatile void *)(addr))
#define readq(addr) mmio_read64((volatile void *)(addr))
#define writeq(val, addr) mmio_write64((val), (volatile void *)(addr))

#endif
