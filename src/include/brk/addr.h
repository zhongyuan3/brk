#ifndef BRK_ADDR_H
#define BRK_ADDR_H

/**
 * @file brk/addr.h
 * @brief Kernel address type definitions.
 *
 * These types separate physical, virtual, user, DMA and PFN address spaces.
 * Architecture code may override the width of physical and DMA addresses, but
 * virtual and user addresses always follow the pointer width.
 */

/** @brief Physical address. */
typedef __UINT64_TYPE__ phys_addr_t;

/** @brief Size in the physical address space. */
typedef __UINT64_TYPE__ phys_size_t;

/** @brief Size stored in a struct resource. */
typedef __UINT64_TYPE__ resource_size_t;

/** @brief DMA/device address. */
typedef __UINT64_TYPE__ dma_addr_t;

/** @brief Physical page frame number. */
typedef __UINT64_TYPE__ pfn_t;

/** @brief Integer kernel virtual address, used for address arithmetic. */
typedef __UINTPTR_TYPE__ virt_addr_t;

/** @brief Integer user virtual address. */
typedef __UINTPTR_TYPE__ user_addr_t;

#endif
