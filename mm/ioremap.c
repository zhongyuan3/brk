#include <brk/asm.h>
#include <brk/ioremap.h>
#include <brk/kernel.h>
#include <brk/list.h>
#include <brk/slab.h>
#include <brk/vmalloc.h>

void *ioremap(u64 paddr, usize_t size, unsigned int flags)
{
	size = round_up(size, PAGE_SIZE);
	void *virt = vmalloc_nomap(size);
	if (!virt)
		return NULL;

	if (kvmap((u64)virt, size, paddr, flags)) {
		vfree_nomap(virt);
		return NULL;
	}

	return virt;
}

void iounmap(void *addr, usize_t size)
{
	size = round_up(size, PAGE_SIZE);
	kvunmap((u64)addr, size);
	vfree_nomap(addr);
}
