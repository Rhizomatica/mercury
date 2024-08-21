/** ffbase: merge-sort implementation
2020, Simon Zolin
*/

/* Merge sort algorithm:
{ n0 ... nMid ... nLast }
. if the number of elements is small: use insertion-sort algorithm
. sort elements in range [n0..nMid)
. sort elements in range [nMid..nLast]
. merge both sorted arrays together into temporary storage
. copy data from temp storage to the original buffer
*/

#include <ffbase/sort-insertion.h>


/**
tmp: buffer of size = n * elsize
*/
static inline void _ffsort_merge(void *tmp, void *data, ffsize n, ffsize elsize, ffsortcmp cmp, void *udata)
{
	if (n <= 1)
		return;
	if (n * elsize <= 64) {
		_ffsort_insertion(tmp, data, n, elsize, cmp, udata);
		return;
	}

	ffsize nL = n / 2
		, nR = n - nL;
	union u {
		char *i1;
		int *i4;
		ffint64 *i8;
	};
	union u L, R, T;
	L.i1 = (char*)data;
	R.i1 = (char*)data + nL * elsize;
	T.i1 = (char*)tmp;

	_ffsort_merge(tmp, L.i1, nL, elsize, cmp, udata);
	_ffsort_merge(tmp, R.i1, nR, elsize, cmp, udata);

	switch (elsize) {
	case 1:
		while (nL != 0 && nR != 0) {
			if (cmp(L.i1, R.i1, udata) <= 0) {
				*T.i1++ = *L.i1++;
				nL--;
			} else {
				*T.i1++ = *R.i1++;
				nR--;
			}
		}
		break;

	case 4:
		while (nL != 0 && nR != 0) {
			if (cmp(L.i4, R.i4, udata) <= 0) {
				*T.i4++ = *L.i4++;
				nL--;
			} else {
				*T.i4++ = *R.i4++;
				nR--;
			}
		}
		break;

	case 8:
		while (nL != 0 && nR != 0) {
			if (cmp(L.i8, R.i8, udata) <= 0) {
				*T.i8++ = *L.i8++;
				nL--;
			} else {
				*T.i8++ = *R.i8++;
				nR--;
			}
		}
		break;

	default:
		while (nL != 0 && nR != 0) {
			if (cmp(L.i1, R.i1, udata) <= 0) {
				ffmem_copy(T.i1, L.i1, elsize);
				T.i1 += elsize;
				L.i1 += elsize;
				nL--;
			} else {
				ffmem_copy(T.i1, R.i1, elsize);
				T.i1 += elsize;
				R.i1 += elsize;
				nR--;
			}
		}
		break;
	}

	/* copy the left tail to temp storage */
	if (nL != 0)
		ffmem_copy(T.i1, L.i1, nL * elsize);

	/* copy all but the right tail to the original buffer */
	ffmem_copy(data, tmp, (n - nR) * elsize);
}
