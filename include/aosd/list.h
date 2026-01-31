#ifndef AOSD_LIST_H
#define AOSD_LIST_H

#include <aosd/macros.h>
#include <aosd/types.h>

#define LIST_HEAD_INIT(name) { &name, &name }

static inline void __list_add(struct list_head *new, struct list_head *prev,
			      struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void __list_del(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	entry->next = entry;
	entry->prev = entry;
}

static inline bool list_is_empty(struct list_head *list)
{
	return list->next == list;
}

static inline struct list_head *list_get_first(struct list_head *list)
{
	return list->next;
}

static inline struct list_head *list_get_last(struct list_head *list)
{
	return list->prev;
}

#define list_for_each(curr, list) \
	for (curr = list_get_first(list); curr != list; curr = curr->next)

#define list_for_each_rev(curr, list) \
	for (curr = list_get_last(list); curr != list; curr = curr->prev)

static inline void list_init(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void list_add_first(struct list_head *list, struct list_head *new)
{
	__list_add(new, list, list->next);
}

static inline void list_add_last(struct list_head *list, struct list_head *new)
{
	__list_add(new, list->prev, list);
}

static inline void list_del(struct list_head *entry)
{
	__list_del(entry);
}

static inline void list_del_first(struct list_head *list)
{
	if (!list_is_empty(list))
		__list_del(list_get_first(list));
}

static inline void list_del_last(struct list_head *list)
{
	if (!list_is_empty(list))
		__list_del(list_get_last(list));
}

static inline size_t list_count(struct list_head *list)
{
	size_t count = 0;
	struct list_head *curr;
	list_for_each(curr, list)
		++count;
	return count;
}

#define list_entry(list, type, member) container_of(list, type, member)

#define list_first_entry(list, type, member) \
	list_entry(list_get_first(list), type, member)

#define list_last_entry(list, type, member) \
	list_entry(list_get_last(list), type, member)

#endif
