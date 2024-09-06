# ffslice

`ffslice` is a very simple array container consisting of 2 fields: data pointer and the number of elements.  It works with a static buffer or dynamically allocated buffer.  There's an assumption that user knows the array's capacity, when he performs append operations.


## Safety

* Allocation functions require the buffer pointer to be NULL - protection against misuse and memory leaks (FF_DEBUG only)
* Data-adding functions require user to pass the max. number of elements (capacity) - protection against buffer overflow
* Data-removing functions automatically check for buffer underflow (FF_DEBUG only)

Not safe (by design) in case:

* User passes an invalid capacity number to data-adding functions
* User calls ffslice_free() after ffslice_set()


## Include

	#include <ffbase/slice.h>


## Conventions

`ffslice_*T()` functions are macros that expect the type of an array's element as a last argument.


## Static buffer

An `ffslice` object can be assigned to a buffer (e.g. reserved on stack or allocated from heap) via `ffslice_set()`.  This is useful when you pass this array to other functions which read or modify it, but don't resize it.

> Don't call ffslice_free() in this case!

```c
	int N = ...;
	struct S array[N] = {...};

	// set buffer
	ffslice a = {};
	ffslice_set(&a, array, N);

	// walk through each element
	struct S *it;
	FFSLICE_WALK(&a, it) {
		...
	}
```


## Dynamically allocated buffer

The buffer is allocated via `ffslice_allocT()`, can be resized when necessary via `ffslice_reallocT()`.
Must free with `ffslice_free()`.

```c
	// allocate buffer
	int CAP = 10;
	ffslice a = {};
	ffslice_allocT(&a, CAP, char);

	// add data
	*ffslice_pushT(&a, CAP, char) = '0';
	*ffslice_pushT(&a, CAP, char) = '1';
	ffslice_addT(&a, CAP, "234", 3, char);

	// free buffer
	ffslice_free(&a);
```
