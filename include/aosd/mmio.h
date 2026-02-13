#ifndef AOSD_MMIO_H
#define AOSD_MMIO_H

#include <aosd/types.h>

static inline uint8_t mmio_read8(volatile void *addr)
{
	return *(volatile uint8_t *)addr;
}

static inline void mmio_write8(volatile void *addr, uint8_t val)
{
	*(volatile uint8_t *)addr = val;
}

static inline uint16_t mmio_read16(volatile void *addr)
{
	return *(volatile uint16_t *)addr;
}

static inline void mmio_write16(volatile void *addr, uint16_t val)
{
	*(volatile uint16_t *)addr = val;
}

static inline uint32_t mmio_read32(volatile void *addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void mmio_write32(volatile void *addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint64_t mmio_read64(volatile void *addr)
{
	return *(volatile uint64_t *)addr;
}

static inline void mmio_write64(volatile void *addr, uint64_t val)
{
	*(volatile uint64_t *)addr = val;
}

#define readb(addr) mmio_read8((volatile void *)(addr))
#define writeb(addr, val) mmio_write8((volatile void *)(addr), (val))
#define readw(addr) mmio_read16((volatile void *)(addr))
#define writew(addr, val) mmio_write16((volatile void *)(addr), (val))
#define readl(addr) mmio_read32((volatile void *)(addr))
#define writel(addr, val) mmio_write32((volatile void *)(addr), (val))
#define readq(addr) mmio_read64((volatile void *)(addr))
#define writeq(addr, val) mmio_write64((volatile void *)(addr), (val))

#endif
