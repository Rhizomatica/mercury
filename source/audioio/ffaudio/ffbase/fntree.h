/** ffbase: File name tree with economical memory management.
2022, Simon Zolin */

/*
fntree_create
fntree_free_all
fntree_add fntree_addz fntree_from_dirscan
fntree_attach
fntree_path fntree_name
fntree_entries
fntree_data
Traverse:
	fntree_cur_next fntree_cur_next_r fntree_cur_next_r_ctx
	fntree_cur_depth
Compare:
	fntree_cmp_init
	fntree_cmp_next
*/

/** Schematic representation:
=============
path="/"
{
  name_len
  name="dir1"
  children ----> ============
}                path="/dir1"
{                {
  ...              name_len
}                  name
=============    }
                 ...
                 ============
*/

#pragma once
#include <ffbase/string.h>

#ifdef _FFSYS_BASE_H
#include <ffsys/dirscan.h>
#endif

typedef struct fntree_block fntree_block;

typedef struct fntree_entry {
	fntree_block *children;
	ffushort name_len;
	ffbyte data_len;
	char name[0];
	// char data[]
	// padding[]
} fntree_entry;

struct fntree_block {
	ffuint cap;
	ffuint size;
	ffuint path_len;
	ffuint entries;
	char data[0];
	// path[path_len] '\0'
	// padding[]
	// fntree_entry[0]{}
	// fntree_entry[1]{}
	// ...
};

static void fntree_free_all(fntree_block *b);

/** Get the pointer to user data. */
static inline void* fntree_data(const fntree_entry *e)
{
	return (char*)e->name + e->name_len;
}

static ffuint _fntr_ent_size(ffuint name_len, ffuint data_len)
{
	return ffint_align_ceil2(FF_OFF(fntree_entry, name) + 1 + name_len + data_len, sizeof(void*));
}

static fntree_entry* _fntr_ent_first(const fntree_block *b)
{
	return (fntree_entry*)(b->data + ffint_align_ceil2(b->path_len + 1, sizeof(void*)));
}

static fntree_entry* _fntr_ent_next(const fntree_entry *e)
{
	return (fntree_entry*)((char*)e + ffint_align_ceil2(FF_OFF(fntree_entry, name) + 1 + e->name_len + e->data_len, sizeof(void*)));
}

static fntree_entry* _fntr_ent_end(const fntree_block *b)
{
	return (fntree_entry*)((char*)b + b->size);
}

/** Create file tree block. */
static inline fntree_block* fntree_create(ffstr path)
{
	fntree_block *b;
	ffuint cap = ffmax(path.len + 1, 512);
	cap = ffint_align_ceil2(cap, 512);
	if (NULL == (b = (fntree_block*)ffmem_alloc(cap)))
		return NULL;
	b->cap = cap;

	b->path_len = path.len;
	ffmem_copy(b->data, path.ptr, path.len);
	b->data[path.len] = '\0';

	b->entries = 0;
	b->size = ffint_align_ceil2(sizeof(fntree_block) + b->path_len + 1, sizeof(void*));
	return b;
}

/** Get the block path that was set by fntree_create(). */
static inline ffstr fntree_path(const fntree_block *b)
{
	ffstr s = FFSTR_INITN(b->data, b->path_len);
	return s;
}

/** Reallocate buffer and add new file entry. */
static inline fntree_entry* fntree_add(fntree_block **pb, ffstr name, ffuint data_len)
{
	if (name.len > 0xffff || data_len > 255)
		return NULL;

	fntree_block *b = *pb;
	ffuint newsize = b->size + _fntr_ent_size(name.len, data_len);
	if (newsize > b->cap) {
		ffuint cap = b->cap * 2;
		if (NULL == (b = (fntree_block*)ffmem_realloc(b, cap)))
			return NULL;
		*pb = b;
		b->cap = cap;
	}

	struct fntree_entry *e = _fntr_ent_end(b);
	e->children = NULL;
	e->name_len = name.len;
	ffmem_copy(e->name, name.ptr, name.len);
	e->data_len = data_len;

	b->entries++;
	b->size = newsize;
	return e;
}

#define fntree_addz(pb, namez, data_len)  fntree_add(pb, FFSTR_Z(namez), data_len)

#ifdef _FFSYS_DIRSCAN_H

/** Fill entries from ffdirscan object. */
static inline fntree_block* fntree_from_dirscan(ffstr path, ffdirscan *ds, ffuint data_len)
{
	fntree_block *b = fntree_create(path);
	const char *fn;
	while (NULL != (fn = ffdirscan_next(ds))) {
		if (NULL == fntree_addz(&b, fn, data_len)) {
			fntree_free_all(b);
			return NULL;
		}
	}
	return b;
}

#endif

/** Attach tree-block to the directory entry.
Return old block pointer. */
static inline fntree_block* fntree_attach(fntree_entry *e, fntree_block *b)
{
	fntree_block *old = e->children;
	e->children = b;
	return old;
}

/** Get N of entries in this block */
static inline ffuint fntree_entries(fntree_block *b)
{
	return b->entries;
}

/** Get entry name without path. */
static inline ffstr fntree_name(const fntree_entry *e)
{
	ffstr s = FFSTR_INITN(e->name, e->name_len);
	return s;
}


typedef struct fntree_cursor {
	const fntree_entry *cur;
	const fntree_block *curblock;
	const fntree_block *block_stk[64];
	const fntree_entry *ent_stk[64];
	ffuint depth;
} fntree_cursor;

static inline ffuint fntree_cur_depth(fntree_cursor *c) { return c->depth; }

/** Get next entry in the same block.
Return NULL if done. */
static inline fntree_entry* fntree_cur_next(fntree_cursor *c, const fntree_block *b)
{
	struct fntree_entry *e;
	if (c->cur == NULL)
		e = _fntr_ent_first(b);
	else
		e = _fntr_ent_next(c->cur);

	if (e == _fntr_ent_end(b))
		return NULL;

	c->cur = e;
	return e;
}

/** Store the current entry in stack.
Return its subblock or NULL on error. */
static const fntree_block* _fntr_cur_push(fntree_cursor *c, const fntree_block *b, const fntree_entry *e)
{
	if (c->depth == FF_COUNT(c->block_stk))
		return NULL;
	c->block_stk[c->depth] = b;
	c->ent_stk[c->depth] = e;
	c->depth++;
	c->cur = NULL;
	c->curblock = e->children;
	return c->curblock;
}

/** Restore the current entry from stack.
Return its block or NULL if stack is empty. */
static const fntree_block* _fntr_cur_pop(fntree_cursor *c)
{
	if (c->depth == 0)
		return NULL;
	c->depth--;
	c->cur = c->ent_stk[c->depth];
	c->curblock = c->block_stk[c->depth];
	return c->curblock;
}

/** Get next entry (recursive): parent directory BEFORE its children:
  (blk[0], blk[0][0], blk[1], ...)
root: [input] root directory; [output] current directory
Return NULL if done. */
static inline fntree_entry* fntree_cur_next_r(fntree_cursor *c, fntree_block **root)
{
	const fntree_entry *e = c->cur;

	const fntree_block *b = *root;
	if (c->curblock == NULL)
		c->curblock = b;
	else
		b = c->curblock;

	if (e != NULL && e->children != NULL) {
		// 2. Go inside the directory, remembering the current block and entry
		if (NULL == (b = _fntr_cur_push(c, c->curblock, e)))
			return NULL;
	}

	for (;;) {
		e = fntree_cur_next(c, b);
		if (e != NULL) {
			*root = (fntree_block*)b;
			return (fntree_entry*)e; // 1. Return file or directory entry
		}
		// 3. No more entries in this directory - restore parent directory's context and continue
		if (NULL == (b = _fntr_cur_pop(c)))
			return NULL; // 4. Root directory is complete
	}
}

/** Get next entry (recursive): all entries in the current directory BEFORE subdirectories:
  (blk[0], blk[1], ..., blk[N], blk[0][0], ...)
root: [input] root directory; [output] current directory
Return NULL if done. */
static inline fntree_entry* fntree_cur_next_r_ctx(fntree_cursor *c, fntree_block **root)
{
	const fntree_block *b = *root;
	if (c->curblock == NULL)
		c->curblock = b;
	else
		b = c->curblock;

	for (;;) {
		const fntree_entry *e = fntree_cur_next(c, b);
		if (e != NULL) {
			*root = (fntree_block*)b;
			return (fntree_entry*)e; // 1. Return file or directory entry
		}

		// 2. No more entries in this directory - scan it again stopping at the first subdirectory
		c->cur = NULL;
		for (;;) {
			e = fntree_cur_next(c, b);

			if (e == NULL) {
				// 4. Restore position in the parent directory
				if (NULL == (b = _fntr_cur_pop(c)))
					return NULL; // 5. Complete

			} else if (e->children != NULL) {
				// 3. Save the current position and enter subdirectory
				b = _fntr_cur_push(c, b, e);
				break;
			}
		}
	}
}

/** Get next block (recursive): parent block AFTER sub-block:
  (blk[0][0], ..., blk[0][N], blk[0], ...)
Return NULL if done. */
static inline fntree_block* _fntr_blk_next_r_post(fntree_cursor *c, fntree_block *root)
{
	const fntree_entry *e = c->cur;

	const fntree_block *b = root;
	if (c->curblock == NULL)
		c->curblock = b;
	else
		b = c->curblock;

	if (c->curblock == (void*)-1) {
		// 3. Restore parent directory's context and continue
		if (NULL == (b = _fntr_cur_pop(c)))
			return NULL; // 4. Root directory is complete
	}

	for (;;) {
		e = fntree_cur_next(c, b);
		if (e != NULL) {
			if (e->children != NULL) {
				// 1. Go inside the directory, remembering the current block and entry
				if (NULL == (b = _fntr_cur_push(c, b, e)))
					return NULL;
			}
			continue;
		}

		b = c->curblock;
		c->curblock = (fntree_block*)-1;
		return (fntree_block*)b; // 2. No more entries in this directory - return directory entry
	}
}


/** Free all tree-blocks. */
static inline void fntree_free_all(fntree_block *b)
{
	if (b == NULL)
		return;

	fntree_cursor c = {};
	for (;;) {
		b = _fntr_blk_next_r_post(&c, b);
		if (b == NULL)
			break;
		ffmem_free(b),  b = NULL;
	}
}


enum FNTREE_CMP {
	FNTREE_CMP_DONE = -1,
	FNTREE_CMP_EQ = 0,
	FNTREE_CMP_LEFT,
	FNTREE_CMP_RIGHT,
	FNTREE_CMP_NEQ,
};

/**
Return enum FNTREE_CMP */
typedef int (*fntree_cmp_func)(void *opaque, const fntree_entry *l, const fntree_entry *r);

/** Default compare function */
static inline int _fntree_cmp_default(void *opaque, const fntree_entry *l, const fntree_entry *r)
{
	(void)opaque;
	ffstr lname = fntree_name(l), rname = fntree_name(r);
	int i = ffstr_cmp2(&lname, &rname);
	if (i < 0)
		return FNTREE_CMP_LEFT;
	else if (i > 0)
		return FNTREE_CMP_RIGHT;
	return FNTREE_CMP_EQ;
}

typedef struct fntree_cmp {
	fntree_cursor lc, rc;
	fntree_cmp_func cmp;
	void *opaque;
} fntree_cmp;

/** Initialize comparator */
static inline void fntree_cmp_init(fntree_cmp *c, const fntree_block *lb, const fntree_block *rb, fntree_cmp_func cmp, void *opaque)
{
	if (lb != NULL)
		fntree_cur_next(&c->lc, lb);
	if (rb != NULL)
		fntree_cur_next(&c->rc, rb);
	c->lc.curblock = lb;
	c->rc.curblock = rb;
	c->cmp = cmp;
	c->opaque = opaque;
}

/** Recursively compare directories.
Return enum FNTREE_CMP */
static inline int fntree_cmp_next(fntree_cmp *c, fntree_entry **l, fntree_entry **r, fntree_block **lb, fntree_block **rb)
{
	*l = (fntree_entry*)c->lc.cur,  *r = (fntree_entry*)c->rc.cur;
	*lb = (fntree_block*)c->lc.curblock,  *rb = (fntree_block*)c->rc.curblock;

	const fntree_entry *l1 = c->lc.cur,  *r1 = c->rc.cur;
	if (c->lc.depth < c->rc.depth)
		l1 = NULL; // skip "right" entries until we exit their block
	else if (c->lc.depth > c->rc.depth)
		r1 = NULL; // skip "left" entries until we exit their block

	int i;
	if (l1 == NULL) {
		i = (r1 != NULL) ? FNTREE_CMP_RIGHT : FNTREE_CMP_DONE;
	} else if (r1 == NULL) {
		i = FNTREE_CMP_LEFT;
	} else {
		i = c->cmp(c->opaque, l1, r1);
	}

	fntree_block *b = NULL;
	if (i != FNTREE_CMP_RIGHT && c->lc.cur != NULL) {
		c->lc.cur = fntree_cur_next_r(&c->lc, &b);
	}

	if (i != FNTREE_CMP_LEFT && c->rc.cur != NULL) {
		c->rc.cur = fntree_cur_next_r(&c->rc, &b);
	}

	return i;
}
