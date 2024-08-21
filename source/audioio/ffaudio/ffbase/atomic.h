/** ffbase: atomic functions
2020, Simon Zolin
*/

#pragma once

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif

/*
FFINT_READONCE
FFINT_WRITEONCE
ffint_fetch_add
ffint_cmpxchg
ffcpu_fence_release ffcpu_fence_acquire
ffatomic_load
ffatomic_store
ffatomic_fetch_add
ffatomic_fetch_and
ffatomic_fetch_or
ffatomic_cmpxchg
*/

#define FFINT_READONCE(obj)  (*(volatile __typeof__(obj)*)&(obj))

#define FFINT_WRITEONCE(obj, val)  (*(volatile __typeof__(obj)*)&(obj) = (val))

/** Atomically add value
Return old value */
#define ffint_fetch_add(ptr, add) \
	__sync_fetch_and_add((volatile __typeof__(*ptr)*)(ptr), add)

/** Atomically perform AND bit operation.
Return old value */
#define ffint_fetch_and(ptr, _and) \
	__sync_fetch_and_and((volatile __typeof__(*ptr)*)(ptr), _and)

/** Atomically perform OR bit operation.
Return old value */
#define ffint_fetch_or(ptr, _and) \
	__sync_fetch_and_or((volatile __typeof__(*ptr)*)(ptr), _and)

/** Compare and, if equal, set new value
Return old value */
#define ffint_cmpxchg(ptr, old, newval) \
	__sync_val_compare_and_swap((volatile __typeof__(*ptr)*)(ptr), old, newval)

/** Prevent the compiler from reordering operations */
#define ff_compiler_fence()  __asm volatile("" : : : "memory")

#define ffcpu_fence()  __sync_synchronize()
#if defined FF_AMD64 || defined FF_X86
	/** Ensure no "store-store" and "load-store" reorder by CPU */
	#define ffcpu_fence_release()  ff_compiler_fence()
	#define ffcpu_fence_acquire()  ff_compiler_fence()

#elif defined __aarch64__
	#define _ffcpu_dmb(opt)  __asm volatile("dmb " #opt : : : "memory")
	#define ffcpu_fence_release()  _ffcpu_dmb(st)
	#define ffcpu_fence_acquire()  _ffcpu_dmb(ld)

#else
	#define ffcpu_fence_release()  __sync_synchronize()
	#define ffcpu_fence_acquire()  __sync_synchronize()
#endif


typedef struct ffatomic {
	ffsize val;
} ffatomic;

/** Get value */
static inline ffsize ffatomic_load(ffatomic *a)
{
	return FFINT_READONCE(a->val);
}

/** Set value */
static inline void ffatomic_store(ffatomic *a, ffsize val)
{
	FFINT_WRITEONCE(a->val, val);
}

/** Add value
Return old value */
static inline ffsize ffatomic_fetch_add(ffatomic *a, ffssize add)
{
	return ffint_fetch_add(&a->val, add);
}

/** Atomically perform AND bit operation.
Return old value */
static inline ffsize ffatomic_fetch_and(ffatomic *a, ffsize _and)
{
	return ffint_fetch_and(&a->val, _and);
}

/** Atomically perform OR bit operation.
Return old value */
static inline ffsize ffatomic_fetch_or(ffatomic *a, ffsize _and)
{
	return ffint_fetch_or(&a->val, _and);
}

/** Compare and, if equal, set new value
Return old value */
static inline ffsize ffatomic_cmpxchg(ffatomic *a, ffsize old, ffsize newval)
{
	return ffint_cmpxchg(&a->val, old, newval);
}
