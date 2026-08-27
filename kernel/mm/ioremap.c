#include <arch/page.h>
#include <brk/ioremap.h>
#include <brk/kernel.h>
#include <brk/vmalloc.h>

void *ioremap(uint64_t paddr, size_t size, unsigned int flags)
{
	size = round_up(size, PAGE_SIZE);
	void *virt = vmalloc_nomap(size);
	if (!virt)
		return NULL;

	if (kvmap((uint64_t)virt, size, paddr, flags)) {
		vfree_nomap(virt);
		return NULL;
	}

	return virt;
}

void iounmap(void *addr, size_t size)
{
	size = round_up(size, PAGE_SIZE);
	kvunmap((uint64_t)addr, size);
	vfree_nomap(addr);
}
