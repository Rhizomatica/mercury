/** ffbase: sort arrays
2020, Simon Zolin */

#include <ffbase/sort.h>
#include <ffbase/sort-merge.h>

int ffsort(void *data, ffsize n, ffsize elsize, ffsortcmp cmp, void *udata)
{
	void *tmp;
	if (NULL == (tmp = _ffmem_alloc_stackorheap(n * elsize)))
		return -1;

	_ffsort_merge(tmp, data, n, elsize, cmp, udata);

	_ffmem_free_stackorheap(tmp, n * elsize);
	return 0;
}
