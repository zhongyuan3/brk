#include <aosd/align.h>
#include <aosd/asm.h>
#include <aosd/ioremap.h>
#include <aosd/list.h>
#include <aosd/slab.h>
#include <aosd/vmalloc.h>

void *ioremap(uint64_t paddr, size_t size, unsigned int flags)
{
	size = align_up(size, PAGE_SIZE);
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
	size = align_up(size, PAGE_SIZE);
	kvunmap((uint64_t)addr, size);
	vfree_nomap(addr);
}
