/** ffbase: doubly-linked list
2020, Simon Zolin
*/

/*
GET:
	fflist_sentl
	fflist_first fflist_last
	fflist_empty
	fflist_print
MODIFY:
	fflist_init
	fflist_add fflist_addfront
	fflist_rm
	fflist_movefront fflist_moveback
ITERATE:
	FFLIST_WALK FFLIST_RWALK FFLIST_FOR
*/

#pragma once

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif
#include <ffbase/chain.h>
#include <ffbase/string.h> // optional


/** Container - root list item used as a sentinel */
typedef struct fflist {
	ffchain_item root;
	ffsize len;
} fflist;

#define fflist_sentl(list)  (&(list)->root)

/** Initialize container before use */
static inline void fflist_init(fflist *list)
{
	list->root.next = list->root.prev = &list->root;
	list->len = 0;
}

#define fflist_first(list)  ((list)->root.next)
#define fflist_last(list)  ((list)->root.prev)

/** Return TRUE if list is empty */
#define fflist_empty(list)  ((list)->root.next == &(list)->root)

/** Add item to the end */
static inline void fflist_add(fflist *list, ffchain_item *item)
{
	ffchain_item_append(item, list->root.prev);
	list->len++;
}

/** Add item to the beginning */
static inline void fflist_addfront(fflist *list, ffchain_item *item)
{
	ffchain_item_prepend(item, list->root.next);
	list->len++;
}

/** Remove item */
static inline void fflist_rm(fflist *list, ffchain_item *item)
{
	FF_ASSERT(list->len != 0);
	ffchain_item_unlink(item);
	list->len--;
}

/** Move the item to the beginning */
static inline void fflist_movefront(fflist *list, ffchain_item *item)
{
	ffchain_item_unlink(item);
	ffchain_item_prepend(item, list->root.next);
}

/** Move the item to the end */
static inline void fflist_moveback(fflist *list, ffchain_item *item)
{
	ffchain_item_unlink(item);
	ffchain_item_append(item, list->root.prev);
}

#define FFLIST_WALK(list, it)  FFCHAIN_WALK(&(list)->root, it)
#define FFLIST_RWALK(list, it)  FFCHAIN_RWALK(&(list)->root, it)
#define FFLIST_FOR(list, it)  FFCHAIN_FOR(&(list)->root, it)

#ifdef _FFBASE_STRFORMAT_H

/** Get debug info on fflist object
Return newly allocated string; must free with ffstr_free() */
static inline ffstr fflist_print(fflist *list)
{
	ffstr s = {};
	ffsize cap = 0;
	ffstr_growfmt(&s, &cap, "list:%p  len:%L\n"
		, list, list->len);

	ffuint i = 0;
	ffchain_item *it;
	FFLIST_WALK(list, it) {
		ffstr_growfmt(&s, &cap, "#%2u %p  prev:%p  next:%p\n"
			, i++, it
			, it->prev, it->next);
	}

	return s;
}

#endif
