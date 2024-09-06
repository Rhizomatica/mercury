# ffbase

ffbase is a fast C library which implements base containers & algorithms.

C language doesn't have this functionality by default (contrary to almost all other languages which have their rich standard library out of the box) and ffbase attempts to fill this hole.

ffbase is header-only (`.h`-only) and doesn't need to be built into `.a/.so/.dll` before use - you just include `.h` file and that's all.

Contents:

* [Naming](#naming)
* [Features](#features)
* [Requirements](#requirements)
* [Where to use](#where-to-use)
* [How to use](#how-to-use)
* [Configure](#configure)
* [Develop](#develop)


## Naming

As there are no namespaces in C, everything here starts with an `ff` prefix.  Then follows the short prefix for the namespace (or a context) and then the name of the function.  For example:

* `ffstr` is a string container
* `ffstr_*()` are functions which work with it, e.g. `ffstr_cmp()` compares strings

All components follow this convention.


## Features

String/text:

| File | Description |
| --- | --- |
| [string.h](ffbase/string.h)   | String container |
| [stringz.h](ffbase/stringz.h) | NULL-terminated string functions |
| [unicode.h](ffbase/unicode.h) | Unicode functions |

Containers:

| File | Description |
| --- | --- |
| [slice.h](ffbase/slice.h)       | Simple array container |
| [vector.h](ffbase/vector.h)     | Array container |
| [sort.h](ffbase/sort.h)         | Array sorting (merge-sort) |
| [chain.h](ffbase/chain.h)       | Simple chain |
| [list.h](ffbase/list.h)         | Doubly-linked list |
| [rbtree.h](ffbase/rbtree.h)     | Red-black tree |
| [map.h](ffbase/map.h)           | Hash table |
| [ringueue.h](ffbase/ringueue.h) | Fixed-size lockless ring queue, multi-producer, multi-consumer |
| [ring.h](ffbase/ring.h)         | Fixed-size lockless ring buffer, multi-producer, multi-consumer |
| [fntree.h](ffbase/fntree.h)     | File name tree with economical memory management |

JSON:

| File | Description |
| --- | --- |
| [json.h](ffbase/json.h)               | Low-level JSON parser |
| [json-scheme.h](ffbase/json-scheme.h) | JSON parser with scheme |
| [json-writer.h](ffbase/json-writer.h) | JSON writer |

Atomic:

| File | Description |
| --- | --- |
| [atomic.h](ffbase/atomic.h) | Atomic operations |
| [lock.h](ffbase/lock.h)     | Spinlock |

Other:

| File | Description |
| --- | --- |
| [args.h](ffbase/args.h)   | Process command-line arguments |
| [conf.h](ffbase/conf.h)   | Low-level key-value settings parser (SSE4.2) |
| [time.h](ffbase/time.h)   | Date/time functions |
| [cpuid.h](ffbase/cpuid.h) | Get CPU features |

## Requirements:

* gcc or clang


## Where to use

In C and C++ projects that require fast and small code without unnecessary dependencies.


## How to use

1. Clone ffbase repo:

		$ git clone https://github.com/stsaz/ffbase

2. In your build script:

		-IFFBASE_DIR

where `FFBASE_DIR` is your ffbase/ directory.

3. And then just use the necessary files:

		#include <ffbase/slice.h>


### Use just 1 file

To avoid copying the whole ffbase repo into your project while all you need is, for example, "slices", you may just copy a few files to your project directory.

1. Clone ffbase repo and copy the necessary files:

		$ git clone https://github.com/stsaz/ffbase
		$ mkdir YOUR_PROJECT/include/ffbase/
		$ cp ffbase/ffbase/slice.h ffbase/ffbase/base.h  YOUR_PROJECT/include/ffbase/

	where `YOUR_PROJECT` is your project's directory.

2. Then, edit `YOUR_PROJECT/include/ffbase/slice.h` and remove the dependencies (marked as optional) you don't need, for example:

		#ifndef _FFBASE_BASE_H
		#include <ffbase/base.h>
		#endif
		-#include <ffbase/sort.h> // optional
		+// #include <ffbase/sort.h> // optional

	Or you can copy `sort.h` file too if you need it.

3. In your build script:

		-IYOUR_PROJECT/include

4. And then just use the necessary files:

		#include <ffbase/slice.h>


## Configure

Use these preprocessor definitions to enable new functionality or to change the existing logic:

| Preprocessor Flag | Description |
| --- | --- |
| `FF_DEBUG` | Use additional `assert()` checks when debugging |
| `FFBASE_HAVE_FFERR_STR` | Enable `"%E"` for format strings |
| `_FFBASE_MEM_ALLOC` | Don't define `ffmem_*` allocation functions (user must define them) |
| `FFBASE_MEM_ASSERT` | Call `assert()` when memory allocation fails |
| `FFBASE_OPT_SIZE` | Mark heavy functions as `extern`.  User must compile the appropriate `.c` files manually. |

To enable SSE4.2 code use `-msse4.2` compiler flag and provide storage for `int _ffcpu_features`.


## Develop

* code
* tests
* in-code documentation
* examples in doc/

### Add new file

Header:

	/** ffbase: description
	Year, Author Name
	*/

Unique ID for the optional `.h` files, so their includers can disable the functionality:

	#define _FFBASE_SORT_H

Don't use any standard functions directly, otherwise it will be impossible to substitute the lower level functions.  For example, don't use `memcpy()` - use `ffmem_copy()` instead.  This gives us the ability to easily add functionality such as counting the number of bytes copied per thread, before actually calling `memcpy()`.

### Test

	cd test
	make
	./fftest all

on FreeBSD:

	gmake

on Linux for Windows:

	make OS=windows

To another directory:

	mkdir /tmp/ffbase
	cd /tmp/ffbase
	make -Rrf MYSRC/ffbase/test/Makefile SRCDIR=MYSRC/ffbase
	cp -ruv MYSRC/ffbase/test/data /tmp/ffbase
	./fftest all

### Commit

Message:

* `+ component: message` - new feature
* `* component: message` - change, small improvement
* `- component: message` - bugfix
