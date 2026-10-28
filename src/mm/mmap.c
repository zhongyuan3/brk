#include <arch/asm/page.h>
#include <arch/asm/vmlayout.h>
#include <arch/csr.h>
#include <arch/mm.h>
#include <arch/pgtable.h>
#include <brk/assert.h>
#include <brk/kmalloc.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/pgalloc.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/mman.h>

static bool ranges_overlap(u64 a, size_t alen, u64 b, size_t blen)
{
	u64 aend;
	u64 bend;

	if (alen == 0 || blen == 0)
		return false;
	if (a > (u64)-1 - alen || b > (u64)-1 - blen)
		return true;
	aend = a + alen;
	bend = b + blen;
	return a < bend && b < aend;
}

static struct uvm_region *find_conflict(struct uvm_space *mm, u64 addr,
					size_t size)
{
	struct uvm_region *r;

	list_for_each_entry(r, &mm->seg, list) {
		if (ranges_overlap(addr, size, r->addr, r->size))
			return r;
	}
	if (mm->stack->size &&
	    ranges_overlap(addr, size, mm->stack->addr, mm->stack->size))
		return mm->stack;
	if (mm->heap->size &&
	    ranges_overlap(addr, size, mm->heap->addr, mm->heap->size))
		return mm->heap;
	return NULL;
}

bool uvm_range_conflicts(struct uvm_space *mm, u64 addr, size_t size)
{
	struct uvm_region *r;

	if (!mm)
		return true;

	list_for_each_entry(r, &mm->seg, list) {
		if (ranges_overlap(addr, size, r->addr, r->size))
			return true;
	}
	if (mm->stack->size &&
	    ranges_overlap(addr, size, mm->stack->addr, mm->stack->size))
		return true;
	if (mm->heap->size &&
	    ranges_overlap(addr, size, mm->heap->addr, mm->heap->size))
		return true;
	return false;
}

static u64 initial_mmap_top(struct uvm_space *mm)
{
	if (mm->mmap_top)
		return mm->mmap_top;
	if (mm->stack->size)
		return mm->stack->addr;
	return USER_SPACE_SIZE_MAX - USTACK_SIZE - 33 * PAGE_SIZE;
}

static int choose_addr(struct uvm_space *mm, u64 hint, size_t size,
		       int flags, u64 *addr_out)
{
	u64 heap_start = mm->heap->addr;
	u64 top;

	if (flags & MAP_FIXED) {
		if (!hint || (hint & (PAGE_SIZE - 1)))
			return -EINVAL;
		if (hint > USER_SPACE_SIZE_MAX ||
		    size > USER_SPACE_SIZE_MAX - hint)
			return -EINVAL;
		if (hint < heap_start)
			return -ENOMEM;
		if (find_conflict(mm, hint, size))
			return -ENOMEM;

		if (!mm->mmap_top || hint < mm->mmap_top)
			mm->mmap_top = hint;
		*addr_out = hint;
		return 0;
	}

	top = initial_mmap_top(mm);

	for (;;) {
		u64 candidate;
		struct uvm_region *conflict;

		if (top <= size || top - size < heap_start)
			return -ENOMEM;
		candidate = top - size;
		if (candidate < heap_start)
			return -ENOMEM;

		conflict = find_conflict(mm, candidate, size);
		if (!conflict) {
			mm->mmap_top = candidate;
			*addr_out = candidate;
			return 0;
		}
		/* Retry just below the conflicting region. */
		if (conflict->addr <= candidate || conflict->addr > top)
			return -ENOMEM;
		top = conflict->addr;
	}
}

static unsigned int prot_to_pte(int prot)
{
	unsigned int flags = 0;

	if (prot & PROT_READ)
		flags |= PTE_R;
	if (prot & PROT_WRITE)
		flags |= PTE_R | PTE_W;
	if (prot & PROT_EXEC)
		flags |= PTE_X;
	return flags;
}

static int map_anon_region(struct uvm_space *mm, u64 addr, size_t size,
			   int prot)
{
	size_t nr_pages = size / PAGE_SIZE;
	struct uvm_region *vma;
	struct page **pages;
	unsigned int pte_flags = prot_to_pte(prot);
	size_t i = 0;
	int err = 0;

	vma = uvm_region_alloc();
	if (!vma)
		return -ENOMEM;

	pages = kcalloc(nr_pages, sizeof(*pages));
	if (!pages) {
		uvm_region_free(vma);
		return -ENOMEM;
	}

	for (i = 0; i < nr_pages; i++) {
		struct page *pg = page_alloc(0);
		void *va;

		if (!pg) {
			err = -ENOMEM;
			break;
		}

		va = (void *)page_to_virt(pg);
		memset(va, 0, PAGE_SIZE);

		err = map_user_pages(mm->pgd, addr + i * PAGE_SIZE, PAGE_SIZE,
				     page_to_phys(pg), pte_flags);
		if (err) {
			page_free(pg, 0);
			break;
		}
		pages[i] = pg;
	}

	if (err) {
		for (size_t j = 0; j < i; j++) {
			unmap_user_pages(mm->pgd, addr + j * PAGE_SIZE,
					 PAGE_SIZE);
			page_free(pages[j], 0);
		}
		kfree(pages);
		uvm_region_free(vma);
		return err;
	}

	vma->addr = addr;
	vma->size = size;
	vma->pages = pages;
	vma->nr_pages = nr_pages;
	vma->flags = pte_flags;
	list_add_tail(&vma->list, &mm->seg);
	return 0;
}

int uvm_mmap_anon(struct uvm_space *mm, u64 hint, size_t len, int prot,
		  int flags, int fd, u64 offset, u64 *addr_out)
{
	u64 size;
	u64 addr = 0;
	int err;

	if (!mm || !addr_out || len == 0)
		return -EINVAL;
	(void)fd;
	if (offset != 0)
		return -EINVAL;
	if (flags & MAP_SHARED)
		return -EOPNOTSUPP;
	if (flags & ~(MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED))
		return -EINVAL;
	if (!(flags & MAP_ANONYMOUS) || !(flags & MAP_PRIVATE))
		return -EOPNOTSUPP;
	if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
		return -EINVAL;
	if (len > USER_SPACE_SIZE_MAX)
		return -ENOMEM;

	size = (u64)len;
	size = (size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
	if (size == 0 || size > USER_SPACE_SIZE_MAX)
		return -ENOMEM;

	err = choose_addr(mm, hint, (size_t)size, flags, &addr);
	if (err)
		return err;

	err = map_anon_region(mm, addr, (size_t)size, prot);
	if (err)
		return err;

	*addr_out = addr;
	return 0;
}

int uvm_munmap(struct uvm_space *mm, u64 addr, size_t len)
{
	u64 size;
	u64 end;
	bool changed = false;

	if (!mm || len == 0)
		return -EINVAL;
	if (addr & (PAGE_SIZE - 1))
		return -EINVAL;

	size = (u64)len;
	size = (size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
	if (addr > USER_SPACE_SIZE_MAX ||
	    size > USER_SPACE_SIZE_MAX - addr)
		return -EINVAL;
	end = addr + size;

	struct uvm_region *r, *next;

	list_for_each_entry_safe(r, next, &mm->seg, list) {
		u64 rstart = r->addr;
		u64 rend = rstart + r->size;
		u64 lo, hi;
		size_t off, nr;

		if (rend <= addr || rstart >= end)
			continue;

		lo = addr > rstart ? addr : rstart;
		hi = end < rend ? end : rend;
		off = (size_t)((lo - rstart) >> PAGE_SHIFT);
		nr = (size_t)((hi - lo) >> PAGE_SHIFT);
		if (nr == 0)
			continue;

		struct uvm_region *right = NULL;
		struct page **right_pages = NULL;
		bool split = (lo != rstart && hi != rend);

		if (split) {
			size_t right_nr = r->nr_pages - off - nr;

			right = uvm_region_alloc();
			if (!right)
				return -ENOMEM;
			right_pages = kcalloc(right_nr, sizeof(*right_pages));
			if (!right_pages) {
				uvm_region_free(right);
				return -ENOMEM;
			}
		}

		unmap_user_pages(mm->pgd, lo, nr * PAGE_SIZE);
		for (size_t i = 0; i < nr; i++)
			page_free(r->pages[off + i], 0);

		if (lo == rstart && hi == rend) {
			list_del(&r->list);
			kfree(r->pages);
			uvm_region_free(r);
		} else if (lo == rstart) {
			memmove(r->pages, r->pages + nr,
				(r->nr_pages - nr) * sizeof(r->pages[0]));
			r->nr_pages -= nr;
			r->addr = hi;
			r->size -= nr * PAGE_SIZE;
		} else if (hi == rend) {
			r->nr_pages -= nr;
			r->size -= nr * PAGE_SIZE;
		} else {
			size_t right_nr = r->nr_pages - off - nr;

			memcpy(right_pages, r->pages + off + nr,
			       right_nr * sizeof(right_pages[0]));
			right->addr = hi;
			right->size = rend - hi;
			right->nr_pages = right_nr;
			right->pages = right_pages;
			right->flags = r->flags;
			list_add(&right->list, &r->list);

			r->nr_pages = off;
			r->size = lo - rstart;
		}
		changed = true;
	}

	if (changed)
		sfence_vma();
	return 0;
}

int uvm_mprotect(struct uvm_space *mm, u64 addr, size_t len, int prot)
{
	u64 size;
	u64 end;
	unsigned int pte_flags;

	if (!mm || len == 0)
		return -EINVAL;
	if (addr & (PAGE_SIZE - 1))
		return -EINVAL;
	if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
		return -EINVAL;

	size = (u64)len;
	size = (size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
	if (addr > USER_SPACE_SIZE_MAX ||
	    size > USER_SPACE_SIZE_MAX - addr)
		return -EINVAL;
	end = addr + size;
	pte_flags = prot_to_pte(prot);

	for (u64 a = addr; a < end; a += PAGE_SIZE) {
		pgd_t *pgde = &mm->pgd[pgd_index(a)];
		pmd_t *pmd, *pmde;
		pte_t *pt, *pte;

		if (!pgd_is_valid(*pgde) || pgd_is_leaf(*pgde))
			return -ENOMEM;
		pmd = (pmd_t *)phys_to_virt(pgd_next(*pgde));
		pmde = &pmd[pmd_index(a)];
		if (!pmd_is_valid(*pmde) || pmd_is_leaf(*pmde))
			return -ENOMEM;
		pt = (pte_t *)phys_to_virt(pmd_next(*pmde));
		pte = &pt[pte_index(a)];
		if (!pte_is_valid(*pte))
			return -ENOMEM;

		pte_set_leaf(pte, pte_leaf(*pte), pte_flags | PTE_U);
	}

	sfence_vma();
	return 0;
}

int uvm_pte_flags(struct uvm_space *mm, u64 addr, unsigned int *flags)
{
	pgd_t *pgde;
	pmd_t *pmd, *pmde;
	pte_t *pt, *pte;

	if (!mm || !flags)
		return -EINVAL;

	pgde = &mm->pgd[pgd_index(addr)];
	if (!pgd_is_valid(*pgde) || pgd_is_leaf(*pgde))
		return -ENOMEM;
	pmd = (pmd_t *)phys_to_virt(pgd_next(*pgde));
	pmde = &pmd[pmd_index(addr)];
	if (!pmd_is_valid(*pmde) || pmd_is_leaf(*pmde))
		return -ENOMEM;
	pt = (pte_t *)phys_to_virt(pmd_next(*pmde));
	pte = &pt[pte_index(addr)];
	if (!pte_is_valid(*pte))
		return -ENOMEM;

	*flags = pte_flags(*pte) & (PTE_R | PTE_W | PTE_X);
	return 0;
}

int uvm_msync(struct uvm_space *mm, u64 addr, size_t len, int flags)
{
	if (!mm || len == 0)
		return -EINVAL;
	if (addr & (PAGE_SIZE - 1))
		return -EINVAL;
	if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC))
		return -EINVAL;
	if ((flags & MS_ASYNC) && (flags & MS_SYNC))
		return -EINVAL;

	/* Anonymous private memory has no backing store to sync. */
	return 0;
}

/* ---- syscall entry points ---- */

u64 sys_mmap(void)
{
	struct task *task = current_task();
	u64 hint = syscall_arg_raw(0);
	size_t len = (size_t)syscall_arg_raw(1);
	int prot = syscall_arg_int(2);
	int flags = syscall_arg_int(3);
	int fd = syscall_arg_int(4);
	u64 offset = syscall_arg_raw(5);
	u64 addr = 0;
	int err;

	if (!task || !task->mm)
		return -EFAULT;

	err = uvm_mmap_anon(task->mm, hint, len, prot, flags, fd, offset,
			    &addr);
	if (err)
		return (u64)(long)err;
	return addr;
}

u64 sys_munmap(void)
{
	struct task *task = current_task();
	u64 addr = syscall_arg_raw(0);
	size_t len = (size_t)syscall_arg_raw(1);

	if (!task || !task->mm)
		return -EFAULT;
	return (u64)(long)uvm_munmap(task->mm, addr, len);
}

u64 sys_mprotect(void)
{
	struct task *task = current_task();
	u64 addr = syscall_arg_raw(0);
	size_t len = (size_t)syscall_arg_raw(1);
	int prot = syscall_arg_int(2);

	if (!task || !task->mm)
		return -EFAULT;
	return (u64)(long)uvm_mprotect(task->mm, addr, len, prot);
}

u64 sys_msync(void)
{
	struct task *task = current_task();
	u64 addr = syscall_arg_raw(0);
	size_t len = (size_t)syscall_arg_raw(1);
	int flags = syscall_arg_int(2);

	if (!task || !task->mm)
		return -EFAULT;
	return (u64)(long)uvm_msync(task->mm, addr, len, flags);
}
