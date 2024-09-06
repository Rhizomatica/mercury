/** ffbase: link multiple objects to each other (chain)
2020, Simon Zolin
*/

/*
ffchain_item_append ffchain_item_prepend
ffchain_item_unlink
FFCHAIN_WALK FFCHAIN_RWALK FFCHAIN_FOR
*/

/*
Empty chain:
SENTL <-> SENTL <-> ...

Chain with 2 items:
1 <-> 2 <-> SENTL <-> 1 <-> ...
*/

#pragma once
#include <ffbase/base.h>


typedef struct ffchain_item ffchain_item;
struct ffchain_item {
	ffchain_item *next, *prev;
};

/** L <-> R */
#define _ffchain_link2(L, R) \
do { \
	(L)->next = (R); \
	(R)->prev = (L); \
} while (0)

/** Add new item after existing:
... <-> AFTER (<-> NEW <->) 1 <-> ... */
static inline void ffchain_item_append(ffchain_item *item, ffchain_item *after)
{
	_ffchain_link2(item, after->next);
	_ffchain_link2(after, item);
}

/** Add new item before existing:
... <-> 2 (<-> NEW <->) BEFORE <-> ... */
static inline void ffchain_item_prepend(ffchain_item *item, ffchain_item *before)
{
	_ffchain_link2(before->prev, item);
	_ffchain_link2(item, before);
}

/** Remove item:
... <-> 1 [<-> DEL <->] 2 <-> ... */
static inline void ffchain_item_unlink(ffchain_item *item)
{
	_ffchain_link2(item->prev, item->next);
	item->prev = item->next = NULL;
}

/** Walk through items */
#define FFCHAIN_WALK(root, it) \
	for (it = (root)->next;  it != (root);  it = it->next)

/** Reverse-walk through items */
#define FFCHAIN_RWALK(root, it) \
	for (it = (root)->prev;  it != (root);  it = it->prev)

/**
Useful when item is modified inside the loop */
#define FFCHAIN_FOR(root, it) \
	for (it = (root)->next;  it != (root);  )
