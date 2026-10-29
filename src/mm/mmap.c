#include <arch/asm/page.h>
#include <arch/asm/vmlayout.h>
#include <arch/csr.h>
#include <arch/mm.h>
#include <arch/pgtable.h>
#include <brk/assert.h>
#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/kmalloc.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/pagecache.h>
#include <brk/pgalloc.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/mman.h>
#include <uapi/stat.h>

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

static int map_region(struct uvm_space *mm, u64 addr, size_t size,
		      int prot, struct fs_file *fp, loff_t file_off)
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

		if (fp) {
			loff_t pos = file_off + (loff_t)(i * PAGE_SIZE);
			size_t got = 0;

			if (!fp->ops || !fp->ops->read) {
				err = -EINVAL;
				page_free(pg, 0);
				break;
			}

			while (got < PAGE_SIZE) {
				ssize_t n = fp->ops->read(
					fp, (char *)va + got,
					PAGE_SIZE - got, &pos);

				if (n < 0) {
					err = (int)n;
					break;
				}
				if (n == 0)
					break;
				got += (size_t)n;
			}
			if (err) {
				page_free(pg, 0);
				break;
			}
		}

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

	err = map_region(mm, addr, (size_t)size, prot, NULL, 0);
	if (err)
		return err;

	*addr_out = addr;
	return 0;
}

int uvm_mmap_file_private(struct uvm_space *mm, u64 hint, size_t len,
			  int prot, int flags, int fd, u64 offset,
			  u64 *addr_out)
{
	struct fs_file *fp;
	u64 size;
	u64 addr = 0;
	loff_t file_off;
	int err;

	if (!mm || !addr_out || len == 0)
		return -EINVAL;
	if (flags & MAP_SHARED)
		return -EOPNOTSUPP;
	if (flags & MAP_ANONYMOUS)
		return -EINVAL;
	if (flags & ~(MAP_PRIVATE | MAP_FIXED))
		return -EINVAL;
	if (!(flags & MAP_PRIVATE))
		return -EINVAL;
	if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
		return -EINVAL;
	if (fd < 0)
		return -EBADF;
	if (offset & (PAGE_SIZE - 1))
		return -EINVAL;
	if (offset > 0x7fffffffffffffffULL)
		return -EINVAL;
	if (len > USER_SPACE_SIZE_MAX)
		return -ENOMEM;

	size = (u64)len;
	size = (size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
	if (size == 0 || size > USER_SPACE_SIZE_MAX)
		return -ENOMEM;
	if (offset > 0x7fffffffffffffffULL - size)
		return -EINVAL;

	file_off = (loff_t)offset;

	fp = fdtable_get_file(current_task()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!fp->inode || !S_ISREG(fp->inode->mode)) {
		err = -ENODEV;
		goto out_put;
	}
	if (!(fp->mode & FMODE_READ)) {
		err = -EACCES;
		goto out_put;
	}

	err = choose_addr(mm, hint, (size_t)size, flags, &addr);
	if (err)
		goto out_put;

	err = map_region(mm, addr, (size_t)size, prot, fp, file_off);
	if (err)
		goto out_put;

	*addr_out = addr;
	err = 0;

out_put:
	fs_file_put(fp);
	return err;
}

int uvm_mmap_file_shared(struct uvm_space *mm, u64 hint, size_t len,
			 int prot, int flags, int fd, u64 offset,
			 u64 *addr_out)
{
	struct fs_file *fp;
	struct fs_inode *inode;
	struct uvm_region *vma;
	struct cached_page **cached;
	u64 size;
	u64 addr = 0;
	unsigned int pte_flags;
	size_t nr_pages;
	int err = 0;

	if (!mm || !addr_out || len == 0)
		return -EINVAL;
	if (!(flags & MAP_SHARED) || (flags & (MAP_PRIVATE | MAP_ANONYMOUS)))
		return -EINVAL;
	if (flags & ~(MAP_SHARED | MAP_FIXED))
		return -EINVAL;
	if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
		return -EINVAL;
	if (fd < 0)
		return -EBADF;
	if (offset & (PAGE_SIZE - 1))
		return -EINVAL;
	if (offset > 0x7fffffffffffffffULL)
		return -EINVAL;
	if (len > USER_SPACE_SIZE_MAX)
		return -ENOMEM;

	size = (u64)len;
	size = (size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
	if (size == 0 || size > USER_SPACE_SIZE_MAX)
		return -ENOMEM;
	if (offset > 0x7fffffffffffffffULL - size)
		return -EINVAL;

	fp = fdtable_get_file(current_task()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	inode = fp->inode;
	if (!inode || !S_ISREG(inode->mode)) {
		err = -ENODEV;
		goto out_put;
	}
	if (!inode->mapping) {
		err = -EOPNOTSUPP;
		goto out_put;
	}
	if ((prot & (PROT_READ | PROT_EXEC)) &&
	    !(fp->mode & FMODE_READ)) {
		err = -EACCES;
		goto out_put;
	}
	if ((prot & PROT_WRITE) && !(fp->mode & FMODE_WRITE)) {
		err = -EACCES;
		goto out_put;
	}

	/*
	 * Shared writable mappings can extend the file.  Do it up front so
	 * writeback has a valid inode size for pages past the old EOF.
	 */
	if ((prot & PROT_WRITE) && (u64)inode->size < offset + size) {
		err = fs_file_truncate(fp, (loff_t)(offset + size));
		if (err)
			goto out_put;
	}

	err = choose_addr(mm, hint, (size_t)size, flags, &addr);
	if (err)
		goto out_put;

	nr_pages = (size_t)size / PAGE_SIZE;
	pte_flags = prot_to_pte(prot);

	vma = uvm_region_alloc();
	if (!vma) {
		err = -ENOMEM;
		goto out_put;
	}
	cached = kcalloc(nr_pages, sizeof(*cached));
	if (!cached) {
		uvm_region_free(vma);
		err = -ENOMEM;
		goto out_put;
	}

	vma->kind = UVM_REGION_SHARED_FILE;
	vma->addr = addr;
	vma->size = (size_t)size;
	vma->nr_pages = 0;
	vma->flags = pte_flags;
	vma->cached = cached;
	vma->inode = fs_inode_get(inode);
	vma->file = fs_file_get(fp);
	vma->file_offset = (loff_t)offset;

	for (size_t i = 0; i < nr_pages; i++) {
		struct cached_page *cp = read_mapping_page(
			inode->mapping,
			(pgoff_t)((offset >> PAGE_SHIFT) + i));

		if (IS_ERR(cp)) {
			err = PTR_ERR(cp);
			break;
		}

		err = map_user_pages(mm->pgd, addr + i * PAGE_SIZE,
				     PAGE_SIZE, page_to_phys(cp->page),
				     pte_flags);
		if (err) {
			cached_page_put(cp);
			break;
		}
		cached[i] = cp;
		vma->nr_pages = i + 1;
	}

	if (err) {
		for (size_t i = 0; i < vma->nr_pages; i++)
			unmap_user_pages(mm->pgd, addr + i * PAGE_SIZE,
					 PAGE_SIZE);
		uvm_region_free_range(vma, 0, vma->nr_pages);
		kfree(vma->cached);
		fs_inode_put(vma->inode);
		if (vma->file)
			fs_file_put(vma->file);
		uvm_region_free(vma);
		goto out_put;
	}

	list_add_tail(&vma->list, &mm->seg);
	*addr_out = addr;
	err = 0;

out_put:
	fs_file_put(fp);
	return err;
}

static struct uvm_region *find_region_exact(struct uvm_space *mm, u64 addr,
					     size_t size)
{
	struct uvm_region *r;

	list_for_each_entry(r, &mm->seg, list) {
		if (r->addr == addr && r->size == size)
			return r;
	}
	return NULL;
}

static int region_grow_in_place(struct uvm_space *mm, struct uvm_region *r,
				size_t new_size)
{
	size_t old_nr = r->nr_pages;
	size_t new_nr = new_size / PAGE_SIZE;
	int err = 0;

	if (r->kind == UVM_REGION_SHARED_FILE) {
		struct cached_page **newc;

		if ((r->flags & PTE_W) && r->file &&
		    (u64)r->inode->size <
			    (u64)r->file_offset + new_size) {
			int err = fs_file_truncate(
				r->file,
				(loff_t)(r->file_offset + new_size));

			if (err)
				return err;
		}

		newc = kcalloc(new_nr, sizeof(newc[0]));
		if (!newc)
			return -ENOMEM;
		memcpy(newc, r->cached, old_nr * sizeof(newc[0]));

		for (size_t i = old_nr; i < new_nr; i++) {
			struct cached_page *cp = read_mapping_page(
				r->inode->mapping,
				(pgoff_t)((r->file_offset >> PAGE_SHIFT) + i));

			if (IS_ERR(cp)) {
				err = PTR_ERR(cp);
				break;
			}
			err = map_user_pages(mm->pgd,
					     r->addr + i * PAGE_SIZE,
					     PAGE_SIZE,
					     page_to_phys(cp->page), r->flags);
			if (err) {
				cached_page_put(cp);
				break;
			}
			newc[i] = cp;
			r->nr_pages = i + 1;
		}

		if (err) {
			for (size_t i = old_nr; i < r->nr_pages; i++) {
				unmap_user_pages(mm->pgd,
						 r->addr + i * PAGE_SIZE,
						 PAGE_SIZE);
				cached_page_put(newc[i]);
			}
			kfree(newc);
			r->nr_pages = old_nr;
			return err;
		}

		kfree(r->cached);
		r->cached = newc;
	} else {
		struct page **newp = kcalloc(new_nr, sizeof(newp[0]));

		if (!newp)
			return -ENOMEM;
		memcpy(newp, r->pages, old_nr * sizeof(newp[0]));

		for (size_t i = old_nr; i < new_nr; i++) {
			struct page *pg = page_alloc(0);

			if (!pg) {
				err = -ENOMEM;
				break;
			}
			memset((void *)page_to_virt(pg), 0, PAGE_SIZE);
			err = map_user_pages(mm->pgd,
					     r->addr + i * PAGE_SIZE,
					     PAGE_SIZE, page_to_phys(pg),
					     r->flags);
			if (err) {
				page_free(pg, 0);
				break;
			}
			newp[i] = pg;
			r->nr_pages = i + 1;
		}

		if (err) {
			for (size_t i = old_nr; i < r->nr_pages; i++) {
				unmap_user_pages(mm->pgd,
						 r->addr + i * PAGE_SIZE,
						 PAGE_SIZE);
				page_free(newp[i], 0);
			}
			kfree(newp);
			r->nr_pages = old_nr;
			return err;
		}

		kfree(r->pages);
		r->pages = newp;
	}

	r->nr_pages = new_nr;
	r->size = new_size;
	sfence_vma();
	return 0;
}

static int region_clone_to(struct uvm_space *mm, struct uvm_region *old,
			   u64 addr, size_t size, struct uvm_region **out)
{
	struct uvm_region *r;
	size_t nr = size / PAGE_SIZE;
	size_t i = 0;
	int err = 0;

	r = uvm_region_alloc();
	if (!r)
		return -ENOMEM;

	r->kind = old->kind;
	r->addr = addr;
	r->size = size;
	r->flags = old->flags;
	r->file_offset = old->file_offset;

	if (old->kind == UVM_REGION_SHARED_FILE) {
		r->inode = fs_inode_get(old->inode);
		r->file = fs_file_get(old->file);
		r->cached = kcalloc(nr, sizeof(r->cached[0]));
		if (!r->cached) {
			fs_inode_put(r->inode);
			if (r->file)
				fs_file_put(r->file);
			uvm_region_free(r);
			return -ENOMEM;
		}

		for (i = 0; i < nr; i++) {
			struct cached_page *cp;
			unsigned int flags;

			if (i < old->nr_pages) {
				cp = old->cached[i];
				cached_page_get(cp);
				err = uvm_pte_flags(mm,
						    old->addr + i * PAGE_SIZE,
						    &flags);
				if (err) {
					cached_page_put(cp);
					break;
				}
			} else {
				cp = read_mapping_page(
					old->inode->mapping,
					(pgoff_t)((old->file_offset >>
						   PAGE_SHIFT) +
						  i));
				if (IS_ERR(cp)) {
					err = PTR_ERR(cp);
					break;
				}
				flags = old->flags;
			}

			err = map_user_pages(mm->pgd, addr + i * PAGE_SIZE,
					     PAGE_SIZE,
					     page_to_phys(cp->page), flags);
			if (err) {
				cached_page_put(cp);
				break;
			}
			r->cached[i] = cp;
			r->nr_pages = i + 1;
		}
	} else {
		r->pages = kcalloc(nr, sizeof(r->pages[0]));
		if (!r->pages) {
			uvm_region_free(r);
			return -ENOMEM;
		}

		for (i = 0; i < nr; i++) {
			struct page *pg = page_alloc(0);
			unsigned int flags;

			if (!pg) {
				err = -ENOMEM;
				break;
			}
			memset((void *)page_to_virt(pg), 0, PAGE_SIZE);
			if (i < old->nr_pages) {
				memcpy((void *)page_to_virt(pg),
				       (void *)page_to_virt(old->pages[i]),
				       PAGE_SIZE);
				err = uvm_pte_flags(mm,
						    old->addr + i * PAGE_SIZE,
						    &flags);
				if (err) {
					page_free(pg, 0);
					break;
				}
			} else {
				flags = old->flags;
			}
			err = map_user_pages(mm->pgd, addr + i * PAGE_SIZE,
					     PAGE_SIZE, page_to_phys(pg),
					     flags);
			if (err) {
				page_free(pg, 0);
				break;
			}
			r->pages[i] = pg;
			r->nr_pages = i + 1;
		}
	}

	if (err) {
		for (size_t j = 0; j < r->nr_pages; j++)
			unmap_user_pages(mm->pgd, addr + j * PAGE_SIZE,
					 PAGE_SIZE);
		uvm_region_release(r);
		return err;
	}

	*out = r;
	return 0;
}

int uvm_mremap(struct uvm_space *mm, u64 old_addr, size_t old_len,
	       size_t new_len, int flags, u64 new_addr, u64 *addr_out)
{
	struct uvm_region *r, *new_r;
	u64 old_size, new_size;
	bool maymove = flags & MREMAP_MAYMOVE;
	bool fixed = flags & MREMAP_FIXED;
	u64 dst = 0;
	int err;

	if (!mm || !addr_out || old_len == 0 || new_len == 0)
		return -EINVAL;
	if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED))
		return -EINVAL;
	if (fixed && !maymove)
		return -EINVAL;
	if (old_addr & (PAGE_SIZE - 1))
		return -EINVAL;

	old_size = (u64)old_len;
	old_size = (old_size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
	new_size = (u64)new_len;
	new_size = (new_size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
	if (old_size == 0 || new_size == 0 || old_size > USER_SPACE_SIZE_MAX ||
	    new_size > USER_SPACE_SIZE_MAX)
		return -EINVAL;
	if (old_addr > USER_SPACE_SIZE_MAX ||
	    old_size > USER_SPACE_SIZE_MAX - old_addr)
		return -EINVAL;

	r = find_region_exact(mm, old_addr, (size_t)old_size);
	if (!r)
		return -EINVAL;

	if (new_size == old_size && !fixed) {
		*addr_out = old_addr;
		return 0;
	}

	if (new_size < old_size && !fixed) {
		unmap_user_pages(mm->pgd, old_addr + new_size,
				 (size_t)(old_size - new_size));
		uvm_region_free_range(r, new_size / PAGE_SIZE,
				      (old_size - new_size) / PAGE_SIZE);
		r->nr_pages = new_size / PAGE_SIZE;
		r->size = (size_t)new_size;
		sfence_vma();
		*addr_out = old_addr;
		return 0;
	}

	/* Prefer growing in place if the following range is free. */
	if (!fixed && new_size > old_size &&
	    old_addr + new_size <= USER_SPACE_SIZE_MAX &&
	    !find_conflict(mm, old_addr + old_size,
			   (size_t)(new_size - old_size))) {
		err = region_grow_in_place(mm, r, (size_t)new_size);
		if (!err) {
			*addr_out = old_addr;
			return 0;
		}
		if (!maymove)
			return err;
	}

	if (!maymove)
		return -ENOMEM;

	if (fixed) {
		if (!new_addr || (new_addr & (PAGE_SIZE - 1)))
			return -EINVAL;
		if (new_addr > USER_SPACE_SIZE_MAX ||
		    new_size > USER_SPACE_SIZE_MAX - new_addr)
			return -ENOMEM;
		if (new_addr < mm->heap->addr)
			return -ENOMEM;
		if (ranges_overlap(new_addr, (size_t)new_size, old_addr,
				   (size_t)old_size))
			return -EINVAL;
		if (find_conflict(mm, new_addr, (size_t)new_size))
			return -ENOMEM;
		dst = new_addr;
	} else {
		err = choose_addr(mm, 0, (size_t)new_size, 0, &dst);
		if (err)
			return err;
	}

	err = region_clone_to(mm, r, dst, (size_t)new_size, &new_r);
	if (err)
		return err;

	list_add_tail(&new_r->list, &mm->seg);
	list_del(&r->list);
	unmap_user_pages(mm->pgd, r->addr, r->size);
	uvm_region_release(r);
	sfence_vma();

	*addr_out = dst;
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
		void **right_pages = NULL;
		bool split = (lo != rstart && hi != rend);
		bool shared = (r->kind == UVM_REGION_SHARED_FILE);

		if (split) {
			size_t right_nr = r->nr_pages - off - nr;

			right = uvm_region_alloc();
			if (!right)
				return -ENOMEM;
			right_pages = kcalloc(right_nr, sizeof(void *));
			if (!right_pages) {
				uvm_region_free(right);
				return -ENOMEM;
			}
		}

		unmap_user_pages(mm->pgd, lo, nr * PAGE_SIZE);

		if (lo == rstart && hi == rend) {
			list_del(&r->list);
			uvm_region_release(r);
		} else {
			uvm_region_free_range(r, off, nr);

			if (lo == rstart) {
				if (shared) {
					memmove(r->cached, r->cached + nr,
						(r->nr_pages - nr) *
							sizeof(r->cached[0]));
					r->file_offset += nr * PAGE_SIZE;
				} else {
					memmove(r->pages, r->pages + nr,
						(r->nr_pages - nr) *
							sizeof(r->pages[0]));
				}
				r->nr_pages -= nr;
				r->addr = hi;
				r->size -= nr * PAGE_SIZE;
			} else if (hi == rend) {
				r->nr_pages -= nr;
				r->size -= nr * PAGE_SIZE;
			} else {
				size_t right_nr = r->nr_pages - off - nr;

				right->kind = r->kind;
				right->addr = hi;
				right->size = rend - hi;
				right->nr_pages = right_nr;
				right->flags = r->flags;
				right->file_offset =
					r->file_offset + (hi - rstart);

				if (shared) {
					right->inode =
						fs_inode_get(r->inode);
					right->file =
						fs_file_get(r->file);
					right->cached = (struct cached_page **)
						right_pages;
					memcpy(right->cached,
					       r->cached + off + nr,
					       right_nr *
						       sizeof(r->cached[0]));
				} else {
					right->pages = (struct page **)
						right_pages;
					memcpy(right->pages,
					       r->pages + off + nr,
					       right_nr *
						       sizeof(r->pages[0]));
				}
				list_add(&right->list, &r->list);

				r->nr_pages = off;
				r->size = lo - rstart;
			}
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

	struct uvm_region *r;

	list_for_each_entry(r, &mm->seg, list) {
		if (r->addr >= addr && r->addr + r->size <= end)
			r->flags = pte_flags;
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
	u64 size;
	u64 end;
	bool any = false;

	if (!mm || len == 0)
		return -EINVAL;
	if (addr & (PAGE_SIZE - 1))
		return -EINVAL;
	if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC))
		return -EINVAL;
	if ((flags & MS_ASYNC) && (flags & MS_SYNC))
		return -EINVAL;

	size = (u64)len;
	size = (size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
	if (addr > USER_SPACE_SIZE_MAX ||
	    size > USER_SPACE_SIZE_MAX - addr)
		return -EINVAL;
	end = addr + size;

	struct uvm_region *r;

	list_for_each_entry(r, &mm->seg, list) {
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

		any = true;
		if (r->kind != UVM_REGION_SHARED_FILE)
			continue;

		if ((r->flags & PTE_W) && r->file &&
		    (u64)r->inode->size <
			    (u64)r->file_offset + r->size) {
			int err = fs_file_truncate(
				r->file,
				(loff_t)(r->file_offset + r->size));

			if (err)
				return err;
		}

		for (size_t i = 0; i < nr; i++)
			cached_page_mark_dirty(r->cached[off + i]);

		if (flags & (MS_SYNC | MS_INVALIDATE)) {
			loff_t fstart = r->file_offset + (loff_t)(lo - rstart);
			loff_t fend = fstart + (loff_t)(nr * PAGE_SIZE);
			int err = page_cache_flush_range(r->inode->mapping,
							fstart, fend);

			if (err)
				return err;
			if (flags & MS_SYNC) {
				err = sync_filesystem(r->inode->sb);
				if (err)
					return err;
			}
		}
	}

	if (!any)
		return -ENOMEM;
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

	if (flags & MAP_ANONYMOUS) {
		if (flags & MAP_SHARED)
			return -EOPNOTSUPP;
		err = uvm_mmap_anon(task->mm, hint, len, prot, flags, fd,
				    offset, &addr);
	} else if (flags & MAP_SHARED) {
		err = uvm_mmap_file_shared(task->mm, hint, len, prot, flags,
					    fd, offset, &addr);
	} else {
		err = uvm_mmap_file_private(task->mm, hint, len, prot, flags,
					    fd, offset, &addr);
	}
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

u64 sys_mremap(void)
{
	struct task *task = current_task();
	u64 old_addr = syscall_arg_raw(0);
	size_t old_len = (size_t)syscall_arg_raw(1);
	size_t new_len = (size_t)syscall_arg_raw(2);
	int flags = syscall_arg_int(3);
	u64 new_addr = syscall_arg_raw(4);
	u64 out = 0;
	int err;

	if (!task || !task->mm)
		return -EFAULT;
	err = uvm_mremap(task->mm, old_addr, old_len, new_len, flags,
			 new_addr, &out);
	if (err)
		return (u64)(long)err;
	return out;
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
