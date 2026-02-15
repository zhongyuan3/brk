#ifndef AOSD_MMIO_H
#define AOSD_MMIO_H

#include <aosd/types.h>

static inline uint8_t mmio_read8(volatile void *addr)
{
	return *(volatile uint8_t *)addr;
}

static inline void mmio_write8(uint8_t val, volatile void *addr)
{
	*(volatile uint8_t *)addr = val;
}

static inline uint16_t mmio_read16(volatile void *addr)
{
	return *(volatile uint16_t *)addr;
}

static inline void mmio_write16(uint16_t val, volatile void *addr)
{
	*(volatile uint16_t *)addr = val;
}

static inline uint32_t mmio_read32(volatile void *addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void mmio_write32(uint32_t val, volatile void *addr)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint64_t mmio_read64(volatile void *addr)
{
	return *(volatile uint64_t *)addr;
}

static inline void mmio_write64(uint64_t val, volatile void *addr)
{
	*(volatile uint64_t *)addr = val;
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
