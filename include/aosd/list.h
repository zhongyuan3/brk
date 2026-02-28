#ifndef AOSD_LIST_H
#define AOSD_LIST_H

#include <aosd/macros.h>
#include <aosd/types.h>

#define list_head_initializer(name) { &name, &name }
#define list_head_define(name) \
	struct list_head name = list_head_initializer(name)

static inline void __list_add(struct list_head *node, struct list_head *prev,
			      struct list_head *next)
{
	next->prev = node;
	node->next = next;
	node->prev = prev;
	prev->next = node;
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	prev->next = next;
}

static inline bool list_empty(struct list_head *head)
{
	return head->next == head;
}

static inline void list_init_head(struct list_head *head)
{
	head->next = head;
	head->prev = head;
}

static inline void list_add(struct list_head *node, struct list_head *head)
{
	__list_add(node, head, head->next);
}

static inline void list_add_tail(struct list_head *node, struct list_head *head)
{
	__list_add(node, head->prev, head);
}

static inline void list_del(struct list_head *node)
{
	__list_del(node->prev, node->next);
	node->next = NULL;
	node->prev = NULL;
}

#define list_for_each(curr, head) \
	for (curr = (head)->next; curr != (head); curr = curr->next)

#define list_for_each_reverse(curr, head) \
	for (curr = (head)->prev; curr != (head); curr = curr->prev)

#define list_for_each_safe(curr, next, head)                         \
	for (curr = (head)->next, next = curr->next; curr != (head); \
	     curr = next, next = next->next)

#define list_for_each_reverse_safe(curr, next, head)                 \
	for (curr = (head)->prev, next = curr->prev; curr != (head); \
	     curr = next, next = next->prev)

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_first_entry(head, type, member) \
	list_entry((head)->next, type, member)

#define list_last_entry(head, type, member) \
	list_entry((head)->prev, type, member)

#define list_next_entry(curr, member) \
	list_entry((curr)->member.next, typeof(*(curr)), member)

#define list_prev_entry(curr, member) \
	list_entry((curr)->member.prev, typeof(*(curr)), member)

#define list_entry_is_head(curr, head, member) (&(curr)->member == (head))

#define list_for_each_entry(curr, head, member)                    \
	for (curr = list_first_entry(head, typeof(*curr), member); \
	     !list_entry_is_head(curr, head, member);              \
	     curr = list_next_entry(curr, member))

#define list_for_each_entry_reverse(curr, head, member)           \
	for (curr = list_last_entry(head, typeof(*curr), member); \
	     !list_entry_is_head(curr, head, member);             \
	     curr = list_prev_entry(curr, member))

#define list_for_each_entry_safe(curr, next, head, member)         \
	for (curr = list_first_entry(head, typeof(*curr), member), \
	    next = list_next_entry(curr, member);                  \
	     !list_entry_is_head(curr, head, member);              \
	     curr = next, next = list_next_entry(next, member))

#define list_for_each_entry_reverse_safe(curr, next, head, member) \
	for (curr = list_last_entry(head, typeof(*curr), member),  \
	    next = list_prev_entry(curr, member);                  \
	     !list_entry_is_head(curr, head, member);              \
	     curr = next, next = list_prev_entry(next, member))

static inline void hlist_add(struct hlist_node *node, struct hlist_head *head)
{
	node->next = head->first;
	node->pprev = &(head->first);
	if (node->next)
		node->next->pprev = &(node->next);
	head->first = node;
}

static inline void hlist_del(struct hlist_node *node)
{
	*(node->pprev) = node->next;
}

static inline bool hlist_empty(struct hlist_head *head)
{
	return head->first == NULL;
}

#define hlist_entry(ptr, type, member) container_of(ptr, type, member)

#define hlist_for_each(curr, head) \
	for (curr = (head)->first; curr; curr = curr->next)

#define hlist_for_each_entry(curr, head, member)                               \
	for (curr = hlist_entry((head)->first, typeof(*(curr)), member); curr; \
	     curr = hlist_entry((curr)->member.next, typeof(*(curr)), member))

#endif
