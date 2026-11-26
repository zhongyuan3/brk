#ifndef _BRK_MM_H
#define _BRK_MM_H

#include <arch/pgtable_types.h>
#include <brk/compiler_attr.h>
#include <brk/mm_types.h>
#include <stdbool.h>

void switch_pgtable(pgd_t *pgd);

void user_access_enable(void);
void user_access_disable(void);

void uvm_space_cache_init(void);
struct uvm_space *uvm_space_create(void);
void uvm_space_free(struct uvm_space *mm);
int uvm_space_copy(struct uvm_space *dst, struct uvm_space *src);

/* Anonymous, private mmap; pages are demand-faulted on first touch. */
int uvm_mmap_anon(struct uvm_space *mm, user_addr_t hint, size_t len, int prot,
		  int flags, int fd, u64 offset, user_addr_t *addr_out);
/* File-backed, MAP_PRIVATE: eagerly copy the file into anonymous pages. */
int uvm_mmap_file_private(struct uvm_space *mm, user_addr_t hint, size_t len,
			  int prot, int flags, int fd, u64 offset,
			  user_addr_t *addr_out);
/* File-backed, MAP_SHARED: map page cache pages directly. */
int uvm_mmap_file_shared(struct uvm_space *mm, user_addr_t hint, size_t len,
			 int prot, int flags, int fd, u64 offset,
			 user_addr_t *addr_out);
/*
 * Resolve a user page fault.  @write/@instr describe the access type.
 * Returns 0 when the PTE now maps @addr, or a negative errno when the access
 * must raise SIGSEGV.
 */
int uvm_handle_fault(struct uvm_space *mm, user_addr_t addr, bool write,
		     bool instr);
/* Remove write permission and mark one page copy-on-write (fork). */
int uvm_cow_protect(struct uvm_space *mm, user_addr_t addr);
/* True when the leaf PTE for @addr carries the software COW marker. */
bool uvm_pte_is_cow(struct uvm_space *mm, user_addr_t addr);
int uvm_munmap(struct uvm_space *mm, user_addr_t addr, size_t len);
int uvm_mprotect(struct uvm_space *mm, user_addr_t addr, size_t len, int prot);
int uvm_msync(struct uvm_space *mm, user_addr_t addr, size_t len, int flags);
/* True if [addr, addr+size) overlaps an existing user mapping. */
bool uvm_range_conflicts(struct uvm_space *mm, user_addr_t addr, size_t size);
/* Read the current R/W/X permission bits of one mapped user page. */
int uvm_pte_flags(struct uvm_space *mm, user_addr_t addr, unsigned int *flags);
/* True when a leaf PTE currently maps @addr in @mm. */
bool uvm_page_is_mapped(struct uvm_space *mm, user_addr_t addr);

struct uvm_region *uvm_region_alloc(void);
void uvm_region_free(struct uvm_region *region);
void uvm_region_free_range(struct uvm_region *region, size_t start, size_t nr);
void uvm_region_unmap_range(struct uvm_space *mm, struct uvm_region *region,
			    size_t start, size_t nr);
void uvm_region_release(struct uvm_region *region);

struct map_pages_context {
	pgd_t *root;
	virt_addr_t addr;
	phys_addr_t paddr;
	phys_size_t size;
	unsigned int flags;
	phys_addr_t (*alloc_frame)(void);
	void *(*phys_to_virt)(phys_addr_t);
};

int map_pages(const struct map_pages_context *ctx);
void unmap_pages(pgd_t *root, virt_addr_t addr, phys_size_t size);

/* Convenience wrappers for final mappings, where page_alloc() is usable. */
int map_kernel_pages(virt_addr_t addr, phys_size_t size, phys_addr_t paddr,
		     unsigned int flags);
void unmap_kernel_pages(virt_addr_t addr, phys_size_t size);
int map_user_pages(pgd_t *root, virt_addr_t addr, phys_size_t size,
		   phys_addr_t paddr, unsigned int flags);
void unmap_user_pages(pgd_t *root, virt_addr_t addr, phys_size_t size);

void mm_init(void);

#endif
