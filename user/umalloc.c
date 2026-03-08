#include "ulib.h"

struct block {
	struct block *next;
	size_t size;
	bool free;
};

static struct block *head = NULL;

void *malloc(size_t size)
{
	struct block *curr, *prev, *new_blk;
	size_t tot_size;

	if (size <= 0)
		return NULL;

	curr = head;
	prev = NULL;

	while (curr) {
		if (curr->free && curr->size >= size) {
			curr->free = false;
			return curr + 1;
		}
		prev = curr;
		curr = curr->next;
	}

	tot_size = sizeof(struct block) + size;
	new_blk = sbrk(tot_size);
	if (new_blk == (void *)(-1))
		return NULL;

	new_blk->size = size;
	new_blk->free = false;
	new_blk->next = NULL;

	if (!prev)
		head = new_blk;
	else
		prev->next = new_blk;

	return new_blk + 1;
}

void *calloc(size_t nmemb, size_t size)
{
	void *ptr = malloc(nmemb * size);
	if (ptr)
		memset(ptr, 0, nmemb * size);
	return ptr;
}

void *realloc(void *ptr, size_t size)
{
	struct block *blk;
	void *new_ptr;

	new_ptr = malloc(size);
	if (!new_ptr)
		return NULL;

	blk = ((struct block *)(ptr)) - 1;
	memcpy(new_ptr, ptr, blk->size);
	free(ptr);
	return new_ptr;
}

void free(void *ptr)
{
	struct block *blk, *curr;

	if (!ptr)
		return;

	blk = ((struct block *)(ptr)) - 1;
	blk->free = true;

	curr = head;
	while (curr && curr->next) {
		if (curr->free && curr->next->free) {
			curr->size += sizeof(struct block);
			curr->next = curr->next->next;
		}
		curr = curr->next;
	}
}
