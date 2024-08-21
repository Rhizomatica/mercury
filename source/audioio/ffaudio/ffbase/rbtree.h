/** ffbase: red-black tree
2020, Simon Zolin
*/

#pragma once

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif

/*
ffrbt_init
ffrbt_empty
ffrbt_first
ffrbt_find
FFRBT_WALK FFRBT_FOR
ffrbt_insert
ffrbt_rm
ffrbt_node_min ffrbt_node_successor ffrbt_node_locate
*/

typedef struct ffrbt_node ffrbt_node;
struct ffrbt_node {
	ffrbt_node *left, *right, *parent;
	ffuint color;
	ffuint key;
};

/** Container */
typedef struct ffrbtree {
	ffsize len;
	ffrbt_node *root;
	ffrbt_node sentl;
	void (*insert)(ffrbt_node *node, ffrbt_node **root, ffrbt_node *sentl);
} ffrbtree;

static inline void _ffrbt_node_insert(ffrbt_node *node, ffrbt_node **root, ffrbt_node *sentl);

#define _FFRBT_BLACK  0
#define _FFRBT_RED  1

static inline void ffrbt_init(ffrbtree *tr)
{
	tr->len = 0;
	tr->sentl.color = _FFRBT_BLACK;
	tr->root = &tr->sentl;
	tr->insert = &_ffrbt_node_insert;
}

/** Return TRUE if empty */
#define ffrbt_empty(tr)  ((tr)->root == &(tr)->sentl)

/** Find bottom left node:
    root
     /
   ...
  /
min
*/
static inline ffrbt_node* ffrbt_node_min(ffrbt_node *node, ffrbt_node *sentl)
{
	if (node == sentl)
		return sentl;
	while (node->left != sentl)
		node = node->left;
	return node;
}

/** Find bottom left node */
static inline ffrbt_node* ffrbt_first(ffrbtree *tr)
{
	return ffrbt_node_min(tr->root, &tr->sentl);
}

/** Find next node */
static inline ffrbt_node* ffrbt_node_successor(ffrbt_node *node, ffrbt_node *sentl)
{
	FF_ASSERT(node != sentl);

	if (node->right != sentl)
		return ffrbt_node_min(node->right, sentl); // find minimum node in the right subtree

	// go up until we find a node that is the left child of its parent
	ffrbt_node *p = node->parent;
	while (p != sentl && node == p->right) {
		node = p;
		p = p->parent;
	}
	return p;
}

/** Find a node with key
If not found - return the leaf node where the search was stopped */
static inline ffrbt_node* ffrbt_node_locate(ffuint key, ffrbt_node *root, ffrbt_node *sentl)
{
	ffrbt_node *node = root;
	while (node != sentl) {
		root = node;
		if (key < node->key)
			node = node->left;
		else if (key > node->key)
			node = node->right;
		else { // key == node->key
			return node;
		}
	}
	return root;
}

/** Walk through the nodes */
#define FFRBT_WALK(tr, it) \
	for ((it) = ffrbt_node_min((tr)->root, &(tr)->sentl) \
		; (it) != &(tr)->sentl \
		; (it) = ffrbt_node_successor((it), &(tr)->sentl))

/** Get the minimum node and loop while it's valid
User must update the node pointer manually */
#define FFRBT_FOR(tr, it) \
	for ((it) = ffrbt_node_min((tr)->root, &(tr)->sentl) \
		; (it) != &(tr)->sentl \
		; )

/** Find a node with key
Return NULL if not found */
static inline ffrbt_node* ffrbt_find(ffrbtree *tr, ffuint key)
{
	ffrbt_node *node = tr->root;
	while (node != &tr->sentl) {
		if (key < node->key)
			node = node->left;
		else if (key > node->key)
			node = node->right;
		else { // key == node->key
			return node;
		}
	}
	return NULL;
}

/** Insert into binary tree */
static inline void _ffrbt_node_insert(ffrbt_node *node, ffrbt_node **root, ffrbt_node *sentl)
{
	if (*root == sentl) {
		*root = node; // set root node
		node->parent = sentl;

	} else {
		ffrbt_node **pchild;
		ffrbt_node *parent = *root;

		// find parent node and the pointer to its left/right node
		for (;;) {
			if (node->key < parent->key)
				pchild = &parent->left;
			else
				pchild = &parent->right;

			if (*pchild == sentl)
				break;
			parent = *pchild;
		}

		*pchild = node; // set parent's child
		node->parent = parent;
	}

	node->left = node->right = sentl;
}


/** Make node become a left child of 'parent':
                PARENT
(LEFT_CHILD <->)      <-> ... */
#define _ffrbt_link_leftchild(parnt, left_child) \
do { \
	(parnt)->left = (left_child); \
	(left_child)->parent = (parnt); \
} while (0)

/** Make node become a right child of 'parent':
       PARENT
... <->     (<-> RIGHT_CHILD) */
#define _ffrbt_link_rightchild(parnt, right_child) \
do { \
	(parnt)->right = (right_child); \
	(right_child)->parent = (parnt); \
} while (0)

/** Make node become a left/right child of parent of 'old' */
static inline void _ffrbt_relink_parent(ffrbt_node *old, ffrbt_node *nnew, ffrbt_node **proot, ffrbt_node *sentl)
{
	ffrbt_node *p = old->parent;
	if (p == sentl)
		*proot = nnew;
	else if (old == p->left)
		p->left = nnew;
	else
		p->right = nnew;
	nnew->parent = p;
}

static inline void _ffrbt_left_rotate(ffrbt_node *node, ffrbt_node **proot, ffrbt_node *sentl)
{
	ffrbt_node *r = node->right;
	FF_ASSERT(node != sentl && node->right != sentl);
	_ffrbt_link_rightchild(node, r->left);
	_ffrbt_relink_parent(node, r, proot, sentl);
	_ffrbt_link_leftchild(r, node);
}

static inline void _ffrbt_right_rotate(ffrbt_node *node, ffrbt_node **proot, ffrbt_node *sentl)
{
	ffrbt_node *l = node->left;
	FF_ASSERT(node != sentl && node->left != sentl);
	_ffrbt_link_leftchild(node, l->right);
	_ffrbt_relink_parent(node, l, proot, sentl);
	_ffrbt_link_rightchild(l, node);
}

/** Insert node
parent: search starting at this node (optional) */
static inline void ffrbt_insert(ffrbtree *tr, ffrbt_node *node, ffrbt_node *parent)
{
	(void)parent;
	ffrbt_node *sentl = &tr->sentl;
	ffrbt_node *root = tr->root;

	tr->insert(node, &root, sentl);
	node->color = _FFRBT_RED;

	// fixup after insert if the parent is also red
	while (node->parent->color == _FFRBT_RED) {

		ffrbt_node *p = node->parent;
		ffrbt_node *gp = node->parent->parent;
		FF_ASSERT(gp != sentl);// grandparent exists because parent is red
		ffrbt_node *uncle;

		if (p == gp->left) {
			uncle = gp->right;
			if (uncle->color == _FFRBT_BLACK) {

				if (node == p->right) {
					_ffrbt_left_rotate(p, &root, sentl);
					node = p;
				}

				(node->parent)->color = _FFRBT_BLACK;
				gp->color = _FFRBT_RED;
				_ffrbt_right_rotate(gp, &root, sentl);
				break;
			}

		} else {
			uncle = gp->left;
			if (uncle->color == _FFRBT_BLACK) {

				if (node == p->left) {
					_ffrbt_right_rotate(p, &root, sentl);
					node = p;
				}

				(node->parent)->color = _FFRBT_BLACK;
				gp->color = _FFRBT_RED;
				_ffrbt_left_rotate(gp, &root, sentl);
				break;
			}
		}

		// both parent and uncle are red
		p->color = _FFRBT_BLACK;
		uncle->color = _FFRBT_BLACK;
		gp->color = _FFRBT_RED;
		node = gp; // repeat the same procedure for grandparent
	}

	root->color = _FFRBT_BLACK;
	tr->root = root;
	tr->len++;
}

/** Remove from binary search tree */
static inline ffrbt_node* _ffrbt_node_rm(ffrbt_node *node, ffrbt_node **proot, ffrbt_node *sentl, ffrbt_node **pnext)
{
	ffrbt_node *x, *nnew;

	if (node->left == sentl) {
		x = node->right;
		nnew = node->right; // to replace node by its right child
		*pnext = NULL;

	} else if (node->right == sentl) {
		x = node->left;
		nnew = node->left; // to replace node by its left child
		*pnext = NULL;

	} else { // node has both children
		ffrbt_node *next = ffrbt_node_min(node->right, sentl);
		x = next->right;

		if (next == node->right)
			x->parent = next; //set parent in case x == sentl
		else {
			// 'next' is not a direct child of 'node'
			_ffrbt_link_leftchild(next->parent, next->right);
			_ffrbt_link_rightchild(next, node->right);
		}

		_ffrbt_link_leftchild(next, node->left);
		*pnext = next;
		nnew = next; // to replace node by its successor
	}

	_ffrbt_relink_parent(node, nnew, proot, sentl);

	node->left = node->right = node->parent = NULL;
	return x;
}

/** Remove node */
static inline void ffrbt_rm(ffrbtree *tr, ffrbt_node *node)
{
	ffrbt_node *root = tr->root, *x, *next;
	ffrbt_node *sentl = &tr->sentl;

	FF_ASSERT(tr->len != 0);
	x = _ffrbt_node_rm(node, &root, sentl, &next);
	tr->len--;

	if (next == NULL) {
		if (node->color == _FFRBT_RED)
			goto done; // black-height has not been changed

	} else {
		// exchange colors of the node and its successor
		int clr = next->color;
		next->color = node->color;
		if (clr == _FFRBT_RED)
			goto done; // black-height has not been changed
	}

	// fixup after delete
	while (x != root && x->color == _FFRBT_BLACK) {
		ffrbt_node *sib, *p = x->parent;

		if (x == p->left) {
			sib = p->right;

			if (sib->color == _FFRBT_RED) {
				sib->color = _FFRBT_BLACK;
				p->color = _FFRBT_RED;
				_ffrbt_left_rotate(p, &root, sentl);
				sib = p->right;
			}

			if (sib->left->color == _FFRBT_RED || sib->right->color == _FFRBT_RED) {
				if (sib->right->color == _FFRBT_BLACK) {
					sib->left->color = _FFRBT_BLACK;
					sib->color = _FFRBT_RED;
					_ffrbt_right_rotate(sib, &root, sentl);
					sib = p->right;
				}

				sib->color = p->color;
				p->color = _FFRBT_BLACK;
				sib->right->color = _FFRBT_BLACK;
				_ffrbt_left_rotate(p, &root, sentl);
				x = root;
				break;
			}

		} else {
			sib = p->left;

			if (sib->color == _FFRBT_RED) {
				sib->color = _FFRBT_BLACK;
				p->color = _FFRBT_RED;
				_ffrbt_right_rotate(p, &root, sentl);
				sib = p->left;
			}

			if (sib->left->color == _FFRBT_RED || sib->right->color == _FFRBT_RED) {
				if (sib->left->color == _FFRBT_BLACK) {
					sib->right->color = _FFRBT_BLACK;
					sib->color = _FFRBT_RED;
					_ffrbt_left_rotate(sib, &root, sentl);
					sib = p->left;
				}

				sib->color = p->color;
				p->color = _FFRBT_BLACK;
				sib->left->color = _FFRBT_BLACK;
				_ffrbt_right_rotate(p, &root, sentl);
				x = root;
				break;
			}
		}

		// both children of 'sib' are black
		sib->color = _FFRBT_RED;
		x = p; // repeat for parent
	}

done:
	x->color = _FFRBT_BLACK;
	tr->root = root;
}

#undef _FFRBT_BLACK
#undef _FFRBT_RED
