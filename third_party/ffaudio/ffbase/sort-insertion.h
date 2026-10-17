/** ffbase: insertion-sort implementation
2020, Simon Zolin
*/

/**
tmp: buffer of size = elsize
*/
static inline void _ffsort_insertion(void *tmp, void *data, ffsize n, ffsize elsize, ffsortcmp cmp, void *udata)
{
	ffsize i;
	ffssize j;
	union u {
		char *i1;
		int *i4;
		ffint64 *i8;
	};
	union u D, T;
	D.i1 = (char*)data;
	T.i1 = (char*)tmp;

	switch (elsize) {
	case 1:
		for (i = 0;  i != n;  i++) {
			*T.i1 = D.i1[i];
			for (j = i - 1;  j >= 0;  j--) {
				if (cmp(&D.i1[j], T.i1, udata) <= 0)
					break;
				D.i1[j + 1] = D.i1[j];
			}
			D.i1[j + 1] = *T.i1;
		}
		break;

	case 4:
		for (i = 0;  i != n;  i++) {
			*T.i4 = D.i4[i];
			for (j = i - 1;  j >= 0;  j--) {
				if (cmp(&D.i4[j], T.i4, udata) <= 0)
					break;
				D.i4[j + 1] = D.i4[j];
			}
			D.i4[j + 1] = *T.i4;
		}
		break;

	case 8:
		for (i = 0;  i != n;  i++) {
			*T.i8 = D.i8[i];
			for (j = i - 1;  j >= 0;  j--) {
				if (cmp(&D.i8[j], T.i8, udata) <= 0)
					break;
				D.i8[j + 1] = D.i8[j];
			}
			D.i8[j + 1] = *T.i8;
		}
		break;

	default:
		for (i = 0;  i != n;  i++) {
			ffmem_copy(T.i1, D.i1 + i * elsize, elsize);
			for (j = i - 1;  j >= 0;  j--) {
				if (cmp(D.i1 + j * elsize, T.i1, udata) <= 0)
					break;
				ffmem_copy(D.i1 + (j + 1) * elsize, D.i1 + j * elsize, elsize);
			}
			ffmem_copy(D.i1 + (j + 1) * elsize, T.i1, elsize);
		}
		break;
	}
}
