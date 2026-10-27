#include <arch/asm/page.h>
#include <brk/ioremap.h>
#include <brk/kernel.h>
#include <brk/mm.h>
#include <brk/vmalloc.h>

void *ioremap(u64 paddr, size_t size, unsigned int flags)
{
	size = round_up(size, PAGE_SIZE);
	void *virt = vmalloc_nomap(size);
	if (!virt)
		return NULL;

	if (kvmap((uintptr_t)virt, size, paddr, flags)) {
		vfree_nomap(virt);
		return NULL;
	}

	return virt;
}

void iounmap(void *addr, size_t size)
{
	size = round_up(size, PAGE_SIZE);
	kvunmap((uintptr_t)addr, size);
	vfree_nomap(addr);
}
