#include <brk/lib/assert.h>
#include <brk/lib/string.h>
#include <brk/mm/kmalloc.h>
#include <brk/mm/pgalloc.h>
#include <brk/mm/slab.h>

#define NR_KMALLOC_ALLOCATORS 10

static struct slab_allocator kmalloc_allocators[NR_KMALLOC_ALLOCATORS];

static struct page *kmalloc_block_head(struct page *pg, unsigned int order)
{
	usize_t pfn = page_to_pfn(pg);
	usize_t head_pfn = pfn & ~(((usize_t)1 << order) - 1);

	return pfn_to_page(head_pfn);
}

static void kmalloc_mark_block(struct page *head, unsigned int order)
{
	unsigned int nr = 1U << order;

	for (unsigned int i = 0; i < nr; ++i) {
		struct page *p = head + i;

		p->flags |= PAGE_FLAGS_KMALLOC;
		p->buddy_page_order = order;
	}
}

static void kmalloc_unmark_block(struct page *head, unsigned int order)
{
	unsigned int nr = 1U << order;

	for (unsigned int i = 0; i < nr; ++i)
		(head + i)->flags &= ~PAGE_FLAGS_KMALLOC;
}

static int kmalloc_allocator_index(usize_t size)
{
	if (size <= 8)
		return 0;
	else if (size <= 16)
		return 1;
	else if (size <= 32)
		return 2;
	else if (size <= 64)
		return 3;
	else if (size <= 128)
		return 4;
	else if (size <= 256)
		return 5;
	else if (size <= 512)
		return 6;
	else if (size <= 1024)
		return 7;
	else if (size <= 2048)
		return 8;
	else if (size <= 4096)
		return 9;
	else
		return -1;
}

void kmalloc_init(void)
{
	usize_t sz[NR_KMALLOC_ALLOCATORS] = {
		8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096,
	};
	usize_t align = alignof(max_align_t);
	for (int i = 0; i < NR_KMALLOC_ALLOCATORS; ++i)
		slab_init(&kmalloc_allocators[i], sz[i], align, "kmalloc");
}

void *kmalloc(usize_t size)
{
	if (size == 0)
		return NULL;

	int index = kmalloc_allocator_index(size);
	if (index >= 0)
		return slab_alloc(&kmalloc_allocators[index]);

	unsigned int order = page_order(size);
	struct page *pg = page_alloc(order);

	if (!pg)
		return NULL;

	if (order > 0)
		kmalloc_mark_block(pg, order);

	return (void *)page_to_virt(pg);
}

void *kcalloc(usize_t nmemb, usize_t size)
{
	usize_t bytes;

	if (nmemb != 0 && size > SIZE_MAX / nmemb)
		return NULL;

	bytes = nmemb * size;
	void *ptr = kmalloc(bytes);

	if (!ptr)
		return NULL;

	memset(ptr, 0, bytes);
	return ptr;
}

void *kzalloc(usize_t size)
{
	return kcalloc(1, size);
}

void kfree(void *ptr)
{
	if (!ptr)
		return;
	struct page *pg = virt_to_page((u64)ptr);

	if (pg->flags & PAGE_FLAGS_SLAB) {
		slab_free(pg->slab_cache, ptr);
	} else if (pg->flags & PAGE_FLAGS_KMALLOC) {
		struct page *head =
			kmalloc_block_head(pg, pg->buddy_page_order);

		ASSERT(head->flags & PAGE_FLAGS_HEAD);
		kmalloc_unmark_block(head, head->buddy_page_order);
		page_free(head, head->buddy_page_order);
	} else {
		ASSERT(pg);
		page_free(pg, pg->buddy_page_order);
	}
}
