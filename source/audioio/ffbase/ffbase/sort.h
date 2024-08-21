/** ffbase: sort arrays
2020, Simon Zolin
*/

#pragma once
#define _FFBASE_SORT_H

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif


/** Return 0: (a == b);
 -1: (a < b);
  1: (a > b) */
typedef int (*ffsortcmp)(const void *a, const void *b, void *udata);

/** Sort array elements.
Uses merge-sort with insertion-sort.
n: number of elements
elsize: size of 1 element
*/
FF_INLINE_EXTERN int ffsort(void *data, ffsize n, ffsize elsize, ffsortcmp cmp, void *udata);

#ifndef FFBASE_OPT_SIZE
#include <ffbase/sort.c>
#endif
