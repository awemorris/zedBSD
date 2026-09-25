/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_TREE_H
#define LIBC_SYS_TREE_H

/*
 * Ordered trees built from links the caller embeds in its own records.
 *
 * Two shapes.  A splay tree keeps nothing but the two child pointers and
 * rebalances by moving whatever was just touched to the root, so a record
 * looked up twice is found quickly the second time; the cost of any one
 * operation is not bounded, only the cost of a run of them.  A red-black
 * tree pays one parent pointer and one colour per record and bounds every
 * operation, which is what to reach for when a single slow lookup would
 * matter.
 *
 * The comparison is supplied by the caller and must order two records
 * totally: negative, zero or positive, as strcmp does.
 *
 * PROTOTYPE declares the operations and GENERATE defines them, so a header
 * can name a tree that one translation unit implements.
 */

/*
 * Splay tree.
 */
#define SPLAY_HEAD(name, type)						\
struct name {								\
	struct type *sph_root;						\
}

#define SPLAY_INITIALIZER(root) { NULL }

#define SPLAY_ENTRY(type)						\
struct {								\
	struct type *spe_left;						\
	struct type *spe_right;						\
}

#define SPLAY_LEFT(elm, field)	((elm)->field.spe_left)
#define SPLAY_RIGHT(elm, field)	((elm)->field.spe_right)
#define SPLAY_ROOT(head)	((head)->sph_root)
#define SPLAY_EMPTY(head)	(SPLAY_ROOT(head) == NULL)

#define SPLAY_INIT(head) do {						\
	SPLAY_ROOT(head) = NULL;					\
} while (0)

/* The three steps of a top-down splay: hang a subtree off one side... */
#define SPLAY_LINKLEFT(head, tmp, field) do {				\
	SPLAY_LEFT(tmp, field) = SPLAY_ROOT(head);			\
	(tmp) = SPLAY_ROOT(head);					\
	SPLAY_ROOT(head) = SPLAY_LEFT(SPLAY_ROOT(head), field);		\
} while (0)

#define SPLAY_LINKRIGHT(head, tmp, field) do {				\
	SPLAY_RIGHT(tmp, field) = SPLAY_ROOT(head);			\
	(tmp) = SPLAY_ROOT(head);					\
	SPLAY_ROOT(head) = SPLAY_RIGHT(SPLAY_ROOT(head), field);	\
} while (0)

/* ...rotate when the search goes twice the same way... */
#define SPLAY_ROTATE_RIGHT(head, tmp, field) do {			\
	SPLAY_LEFT(SPLAY_ROOT(head), field) = SPLAY_RIGHT(tmp, field);	\
	SPLAY_RIGHT(tmp, field) = SPLAY_ROOT(head);			\
	SPLAY_ROOT(head) = (tmp);					\
} while (0)

#define SPLAY_ROTATE_LEFT(head, tmp, field) do {			\
	SPLAY_RIGHT(SPLAY_ROOT(head), field) = SPLAY_LEFT(tmp, field);	\
	SPLAY_LEFT(tmp, field) = SPLAY_ROOT(head);			\
	SPLAY_ROOT(head) = (tmp);					\
} while (0)

/* ...and put the two hanging subtrees back under the new root. */
#define SPLAY_ASSEMBLE(head, node, left, right, field) do {		\
	SPLAY_RIGHT(left, field) = SPLAY_LEFT(SPLAY_ROOT(head), field);	\
	SPLAY_LEFT(right, field) = SPLAY_RIGHT(SPLAY_ROOT(head), field);	\
	SPLAY_LEFT(SPLAY_ROOT(head), field) = SPLAY_RIGHT(node, field);	\
	SPLAY_RIGHT(SPLAY_ROOT(head), field) = SPLAY_LEFT(node, field);	\
} while (0)

#define SPLAY_PROTOTYPE(name, type, field, cmp)				\
void name##_SPLAY(struct name *, struct type *);			\
void name##_SPLAY_MINMAX(struct name *, int);				\
struct type *name##_SPLAY_INSERT(struct name *, struct type *);		\
struct type *name##_SPLAY_REMOVE(struct name *, struct type *);		\
									\
static __inline struct type *						\
name##_SPLAY_FIND(struct name *head, struct type *elm)			\
{									\
	if (SPLAY_EMPTY(head))						\
		return NULL;						\
	name##_SPLAY(head, elm);					\
	if ((cmp)(elm, SPLAY_ROOT(head)) == 0)				\
		return SPLAY_ROOT(head);				\
	return NULL;							\
}									\
									\
static __inline struct type *						\
name##_SPLAY_NEXT(struct name *head, struct type *elm)			\
{									\
	name##_SPLAY(head, elm);					\
	if (SPLAY_RIGHT(elm, field) != NULL) {				\
		elm = SPLAY_RIGHT(elm, field);				\
		while (SPLAY_LEFT(elm, field) != NULL)			\
			elm = SPLAY_LEFT(elm, field);			\
	} else								\
		elm = NULL;						\
	return elm;							\
}									\
									\
static __inline struct type *						\
name##_SPLAY_MIN_MAX(struct name *head, int val)			\
{									\
	name##_SPLAY_MINMAX(head, val);					\
	return SPLAY_ROOT(head);					\
}

#define SPLAY_NEGINF (-1)
#define SPLAY_INF    1

#define SPLAY_GENERATE(name, type, field, cmp)				\
struct type *								\
name##_SPLAY_INSERT(struct name *head, struct type *elm)		\
{									\
	int comp;							\
									\
	if (SPLAY_EMPTY(head)) {					\
		SPLAY_LEFT(elm, field) = NULL;				\
		SPLAY_RIGHT(elm, field) = NULL;				\
	} else {							\
		name##_SPLAY(head, elm);				\
		comp = (cmp)(elm, SPLAY_ROOT(head));			\
		if (comp < 0) {						\
			SPLAY_LEFT(elm, field) =			\
			    SPLAY_LEFT(SPLAY_ROOT(head), field);	\
			SPLAY_RIGHT(elm, field) = SPLAY_ROOT(head);	\
			SPLAY_LEFT(SPLAY_ROOT(head), field) = NULL;	\
		} else if (comp > 0) {					\
			SPLAY_RIGHT(elm, field) =			\
			    SPLAY_RIGHT(SPLAY_ROOT(head), field);	\
			SPLAY_LEFT(elm, field) = SPLAY_ROOT(head);	\
			SPLAY_RIGHT(SPLAY_ROOT(head), field) = NULL;	\
		} else							\
			return SPLAY_ROOT(head);			\
	}								\
	SPLAY_ROOT(head) = (elm);					\
	return NULL;							\
}									\
									\
struct type *								\
name##_SPLAY_REMOVE(struct name *head, struct type *elm)		\
{									\
	struct type *tmp;						\
									\
	if (SPLAY_EMPTY(head))						\
		return NULL;						\
	name##_SPLAY(head, elm);					\
	if ((cmp)(elm, SPLAY_ROOT(head)) != 0)				\
		return NULL;						\
									\
	/* The root is going: its two subtrees are joined instead. */	\
	if (SPLAY_LEFT(SPLAY_ROOT(head), field) == NULL) {		\
		SPLAY_ROOT(head) = SPLAY_RIGHT(SPLAY_ROOT(head), field); \
	} else {							\
		tmp = SPLAY_RIGHT(SPLAY_ROOT(head), field);		\
		SPLAY_ROOT(head) = SPLAY_LEFT(SPLAY_ROOT(head), field);	\
		name##_SPLAY(head, elm);				\
		SPLAY_RIGHT(SPLAY_ROOT(head), field) = tmp;		\
	}								\
	return (elm);							\
}									\
									\
void									\
name##_SPLAY(struct name *head, struct type *elm)			\
{									\
	struct type node;						\
	struct type *left;						\
	struct type *right;						\
	struct type *tmp;						\
	int comp;							\
									\
	if (SPLAY_EMPTY(head))						\
		return;							\
	SPLAY_LEFT(&node, field) = NULL;				\
	SPLAY_RIGHT(&node, field) = NULL;				\
	left = &node;							\
	right = &node;							\
									\
	/* Walks down, hanging off what is passed, until elm is found. */ \
	for (;;) {							\
		comp = (cmp)(elm, SPLAY_ROOT(head));			\
		if (comp < 0) {						\
			tmp = SPLAY_LEFT(SPLAY_ROOT(head), field);	\
			if (tmp == NULL)				\
				break;					\
			if ((cmp)(elm, tmp) < 0) {			\
				SPLAY_ROTATE_RIGHT(head, tmp, field);	\
				if (SPLAY_LEFT(SPLAY_ROOT(head),	\
				    field) == NULL)			\
					break;				\
			}						\
			SPLAY_LINKLEFT(head, right, field);		\
		} else if (comp > 0) {					\
			tmp = SPLAY_RIGHT(SPLAY_ROOT(head), field);	\
			if (tmp == NULL)				\
				break;					\
			if ((cmp)(elm, tmp) > 0) {			\
				SPLAY_ROTATE_LEFT(head, tmp, field);	\
				if (SPLAY_RIGHT(SPLAY_ROOT(head),	\
				    field) == NULL)			\
					break;				\
			}						\
			SPLAY_LINKRIGHT(head, left, field);		\
		} else							\
			break;						\
	}								\
	SPLAY_ASSEMBLE(head, &node, left, right, field);		\
}									\
									\
/* The same walk, driven to one end rather than to a key. */		\
void									\
name##_SPLAY_MINMAX(struct name *head, int val)				\
{									\
	struct type node;						\
	struct type *left;						\
	struct type *right;						\
	struct type *tmp;						\
									\
	if (SPLAY_EMPTY(head))						\
		return;							\
	SPLAY_LEFT(&node, field) = NULL;				\
	SPLAY_RIGHT(&node, field) = NULL;				\
	left = &node;							\
	right = &node;							\
									\
	for (;;) {							\
		if (val < 0) {						\
			tmp = SPLAY_LEFT(SPLAY_ROOT(head), field);	\
			if (tmp == NULL)				\
				break;					\
			SPLAY_ROTATE_RIGHT(head, tmp, field);		\
			if (SPLAY_LEFT(SPLAY_ROOT(head), field) == NULL) \
				break;					\
			SPLAY_LINKLEFT(head, right, field);		\
		} else {						\
			tmp = SPLAY_RIGHT(SPLAY_ROOT(head), field);	\
			if (tmp == NULL)				\
				break;					\
			SPLAY_ROTATE_LEFT(head, tmp, field);		\
			if (SPLAY_RIGHT(SPLAY_ROOT(head), field) == NULL) \
				break;					\
			SPLAY_LINKRIGHT(head, left, field);		\
		}							\
	}								\
	SPLAY_ASSEMBLE(head, &node, left, right, field);		\
}

#define SPLAY_INSERT(name, head, elm)	name##_SPLAY_INSERT(head, elm)
#define SPLAY_REMOVE(name, head, elm)	name##_SPLAY_REMOVE(head, elm)
#define SPLAY_FIND(name, head, elm)	name##_SPLAY_FIND(head, elm)
#define SPLAY_NEXT(name, head, elm)	name##_SPLAY_NEXT(head, elm)
#define SPLAY_MIN(name, head)						\
	(SPLAY_EMPTY(head) ? NULL : name##_SPLAY_MIN_MAX(head, SPLAY_NEGINF))
#define SPLAY_MAX(name, head)						\
	(SPLAY_EMPTY(head) ? NULL : name##_SPLAY_MIN_MAX(head, SPLAY_INF))

#define SPLAY_FOREACH(variable, name, head)				\
	for ((variable) = SPLAY_MIN(name, head);			\
	     (variable) != NULL;					\
	     (variable) = SPLAY_NEXT(name, head, variable))

/*
 * Red-black tree.
 *
 * Every path from a record to a leaf passes the same number of black
 * records, and no red record has a red child; together those keep the
 * longest path within twice the shortest, which is what bounds each
 * operation.  Insertion and removal restore both rules by recolouring
 * where they can and rotating where they cannot.
 */
#define RB_BLACK 0
#define RB_RED   1

#define RB_HEAD(name, type)						\
struct name {								\
	struct type *rbh_root;						\
}

#define RB_INITIALIZER(root) { NULL }

#define RB_ENTRY(type)							\
struct {								\
	struct type *rbe_left;						\
	struct type *rbe_right;						\
	struct type *rbe_parent;					\
	int rbe_color;							\
}

#define RB_LEFT(elm, field)	((elm)->field.rbe_left)
#define RB_RIGHT(elm, field)	((elm)->field.rbe_right)
#define RB_PARENT(elm, field)	((elm)->field.rbe_parent)
#define RB_COLOR(elm, field)	((elm)->field.rbe_color)
#define RB_ROOT(head)		((head)->rbh_root)
#define RB_EMPTY(head)		(RB_ROOT(head) == NULL)

#define RB_INIT(head) do {						\
	RB_ROOT(head) = NULL;						\
} while (0)

#define RB_PROTOTYPE(name, type, field, cmp)				\
void name##_RB_INSERT_COLOR(struct name *, struct type *);		\
void name##_RB_REMOVE_COLOR(struct name *, struct type *, struct type *); \
struct type *name##_RB_REMOVE(struct name *, struct type *);		\
struct type *name##_RB_INSERT(struct name *, struct type *);		\
struct type *name##_RB_FIND(struct name *, struct type *);		\
struct type *name##_RB_NFIND(struct name *, struct type *);		\
struct type *name##_RB_NEXT(struct type *);				\
struct type *name##_RB_PREV(struct type *);				\
struct type *name##_RB_MINMAX(struct name *, int);

#define RB_NEGINF (-1)
#define RB_INF    1

#define RB_GENERATE(name, type, field, cmp)				\
/* Moves a record's right child above it, keeping the order. */		\
static void								\
name##_RB_ROTATE_LEFT(struct name *head, struct type *elm)		\
{									\
	struct type *tmp = RB_RIGHT(elm, field);			\
									\
	RB_RIGHT(elm, field) = RB_LEFT(tmp, field);			\
	if (RB_LEFT(tmp, field) != NULL)				\
		RB_PARENT(RB_LEFT(tmp, field), field) = (elm);		\
	RB_PARENT(tmp, field) = RB_PARENT(elm, field);			\
	if (RB_PARENT(elm, field) == NULL)				\
		RB_ROOT(head) = tmp;					\
	else if ((elm) == RB_LEFT(RB_PARENT(elm, field), field))	\
		RB_LEFT(RB_PARENT(elm, field), field) = tmp;		\
	else								\
		RB_RIGHT(RB_PARENT(elm, field), field) = tmp;		\
	RB_LEFT(tmp, field) = (elm);					\
	RB_PARENT(elm, field) = tmp;					\
}									\
									\
/* The mirror of the same move. */					\
static void								\
name##_RB_ROTATE_RIGHT(struct name *head, struct type *elm)		\
{									\
	struct type *tmp = RB_LEFT(elm, field);				\
									\
	RB_LEFT(elm, field) = RB_RIGHT(tmp, field);			\
	if (RB_RIGHT(tmp, field) != NULL)				\
		RB_PARENT(RB_RIGHT(tmp, field), field) = (elm);		\
	RB_PARENT(tmp, field) = RB_PARENT(elm, field);			\
	if (RB_PARENT(elm, field) == NULL)				\
		RB_ROOT(head) = tmp;					\
	else if ((elm) == RB_LEFT(RB_PARENT(elm, field), field))	\
		RB_LEFT(RB_PARENT(elm, field), field) = tmp;		\
	else								\
		RB_RIGHT(RB_PARENT(elm, field), field) = tmp;		\
	RB_RIGHT(tmp, field) = (elm);					\
	RB_PARENT(elm, field) = tmp;					\
}									\
									\
/*									\
 * Restores the rules after a red record was added.			\
 *									\
 * A red record under a red parent is the only breakage.  When the	\
 * parent's sibling is also red the pair can be painted black and the	\
 * problem handed to the grandparent; otherwise one or two rotations	\
 * settle it for good.							\
 */									\
void									\
name##_RB_INSERT_COLOR(struct name *head, struct type *elm)		\
{									\
	struct type *parent;						\
	struct type *gparent;						\
	struct type *uncle;						\
									\
	while ((parent = RB_PARENT(elm, field)) != NULL &&		\
	    RB_COLOR(parent, field) == RB_RED) {			\
		gparent = RB_PARENT(parent, field);			\
		if (gparent == NULL)					\
			break;						\
		if (parent == RB_LEFT(gparent, field)) {		\
			uncle = RB_RIGHT(gparent, field);		\
			if (uncle != NULL &&				\
			    RB_COLOR(uncle, field) == RB_RED) {		\
				RB_COLOR(uncle, field) = RB_BLACK;	\
				RB_COLOR(parent, field) = RB_BLACK;	\
				RB_COLOR(gparent, field) = RB_RED;	\
				elm = gparent;				\
				continue;				\
			}						\
			if (RB_RIGHT(parent, field) == (elm)) {		\
				name##_RB_ROTATE_LEFT(head, parent);	\
				elm = parent;				\
				parent = RB_PARENT(elm, field);		\
				gparent = RB_PARENT(parent, field);	\
			}						\
			RB_COLOR(parent, field) = RB_BLACK;		\
			RB_COLOR(gparent, field) = RB_RED;		\
			name##_RB_ROTATE_RIGHT(head, gparent);		\
		} else {						\
			uncle = RB_LEFT(gparent, field);		\
			if (uncle != NULL &&				\
			    RB_COLOR(uncle, field) == RB_RED) {		\
				RB_COLOR(uncle, field) = RB_BLACK;	\
				RB_COLOR(parent, field) = RB_BLACK;	\
				RB_COLOR(gparent, field) = RB_RED;	\
				elm = gparent;				\
				continue;				\
			}						\
			if (RB_LEFT(parent, field) == (elm)) {		\
				name##_RB_ROTATE_RIGHT(head, parent);	\
				elm = parent;				\
				parent = RB_PARENT(elm, field);		\
				gparent = RB_PARENT(parent, field);	\
			}						\
			RB_COLOR(parent, field) = RB_BLACK;		\
			RB_COLOR(gparent, field) = RB_RED;		\
			name##_RB_ROTATE_LEFT(head, gparent);		\
		}							\
	}								\
	RB_COLOR(RB_ROOT(head), field) = RB_BLACK;			\
}									\
									\
/*									\
 * Restores the rules after a black record was taken out.		\
 *									\
 * Its side is now one black short.  Each turn either borrows a black	\
 * from the sibling by rotating, or paints the sibling red so that both	\
 * sides are equally short and passes the shortfall up.  elm may be	\
 * NULL, which is a leaf and counts as black.				\
 */									\
void									\
name##_RB_REMOVE_COLOR(struct name *head, struct type *parent,		\
    struct type *elm)							\
{									\
	struct type *sibling;						\
									\
	while ((elm == NULL || RB_COLOR(elm, field) == RB_BLACK) &&	\
	    elm != RB_ROOT(head) && parent != NULL) {			\
		if (RB_LEFT(parent, field) == elm) {			\
			sibling = RB_RIGHT(parent, field);		\
			if (sibling == NULL)				\
				break;					\
			if (RB_COLOR(sibling, field) == RB_RED) {	\
				RB_COLOR(sibling, field) = RB_BLACK;	\
				RB_COLOR(parent, field) = RB_RED;	\
				name##_RB_ROTATE_LEFT(head, parent);	\
				sibling = RB_RIGHT(parent, field);	\
				if (sibling == NULL)			\
					break;				\
			}						\
			if ((RB_LEFT(sibling, field) == NULL ||		\
			    RB_COLOR(RB_LEFT(sibling, field),		\
			    field) == RB_BLACK) &&			\
			    (RB_RIGHT(sibling, field) == NULL ||	\
			    RB_COLOR(RB_RIGHT(sibling, field),		\
			    field) == RB_BLACK)) {			\
				RB_COLOR(sibling, field) = RB_RED;	\
				elm = parent;				\
				parent = RB_PARENT(elm, field);		\
				continue;				\
			}						\
			if (RB_RIGHT(sibling, field) == NULL ||		\
			    RB_COLOR(RB_RIGHT(sibling, field),		\
			    field) == RB_BLACK) {			\
				if (RB_LEFT(sibling, field) != NULL) {	\
					RB_COLOR(RB_LEFT(sibling,	\
					    field), field) = RB_BLACK;	\
				}					\
				RB_COLOR(sibling, field) = RB_RED;	\
				name##_RB_ROTATE_RIGHT(head, sibling);	\
				sibling = RB_RIGHT(parent, field);	\
				if (sibling == NULL)			\
					break;				\
			}						\
			RB_COLOR(sibling, field) =			\
			    RB_COLOR(parent, field);			\
			RB_COLOR(parent, field) = RB_BLACK;		\
			if (RB_RIGHT(sibling, field) != NULL) {		\
				RB_COLOR(RB_RIGHT(sibling, field),	\
				    field) = RB_BLACK;			\
			}						\
			name##_RB_ROTATE_LEFT(head, parent);		\
			elm = RB_ROOT(head);				\
			break;						\
		} else {						\
			sibling = RB_LEFT(parent, field);		\
			if (sibling == NULL)				\
				break;					\
			if (RB_COLOR(sibling, field) == RB_RED) {	\
				RB_COLOR(sibling, field) = RB_BLACK;	\
				RB_COLOR(parent, field) = RB_RED;	\
				name##_RB_ROTATE_RIGHT(head, parent);	\
				sibling = RB_LEFT(parent, field);	\
				if (sibling == NULL)			\
					break;				\
			}						\
			if ((RB_LEFT(sibling, field) == NULL ||		\
			    RB_COLOR(RB_LEFT(sibling, field),		\
			    field) == RB_BLACK) &&			\
			    (RB_RIGHT(sibling, field) == NULL ||	\
			    RB_COLOR(RB_RIGHT(sibling, field),		\
			    field) == RB_BLACK)) {			\
				RB_COLOR(sibling, field) = RB_RED;	\
				elm = parent;				\
				parent = RB_PARENT(elm, field);		\
				continue;				\
			}						\
			if (RB_LEFT(sibling, field) == NULL ||		\
			    RB_COLOR(RB_LEFT(sibling, field),		\
			    field) == RB_BLACK) {			\
				if (RB_RIGHT(sibling, field) != NULL) {	\
					RB_COLOR(RB_RIGHT(sibling,	\
					    field), field) = RB_BLACK;	\
				}					\
				RB_COLOR(sibling, field) = RB_RED;	\
				name##_RB_ROTATE_LEFT(head, sibling);	\
				sibling = RB_LEFT(parent, field);	\
				if (sibling == NULL)			\
					break;				\
			}						\
			RB_COLOR(sibling, field) =			\
			    RB_COLOR(parent, field);			\
			RB_COLOR(parent, field) = RB_BLACK;		\
			if (RB_LEFT(sibling, field) != NULL) {		\
				RB_COLOR(RB_LEFT(sibling, field),	\
				    field) = RB_BLACK;			\
			}						\
			name##_RB_ROTATE_RIGHT(head, parent);		\
			elm = RB_ROOT(head);				\
			break;						\
		}							\
	}								\
	if (elm != NULL)						\
		RB_COLOR(elm, field) = RB_BLACK;			\
}									\
									\
/*									\
 * Takes a record out.							\
 *									\
 * A record with two children cannot simply be unlinked, so the one	\
 * that follows it in order takes its place and its colour, and the	\
 * shortfall is judged where that successor used to be.			\
 */									\
struct type *								\
name##_RB_REMOVE(struct name *head, struct type *elm)			\
{									\
	struct type *child;						\
	struct type *parent;						\
	struct type *old = (elm);					\
	struct type *left;						\
	int color;							\
									\
	if (RB_LEFT(elm, field) == NULL) {				\
		child = RB_RIGHT(elm, field);				\
	} else if (RB_RIGHT(elm, field) == NULL) {			\
		child = RB_LEFT(elm, field);				\
	} else {							\
		elm = RB_RIGHT(elm, field);				\
		while ((left = RB_LEFT(elm, field)) != NULL)		\
			elm = left;					\
		child = RB_RIGHT(elm, field);				\
		parent = RB_PARENT(elm, field);				\
		color = RB_COLOR(elm, field);				\
		if (child != NULL)					\
			RB_PARENT(child, field) = parent;		\
		if (parent == old)					\
			RB_RIGHT(parent, field) = child;		\
		else							\
			RB_LEFT(parent, field) = child;			\
									\
		/* The successor now stands exactly where old stood. */	\
		RB_PARENT(elm, field) = RB_PARENT(old, field);		\
		RB_LEFT(elm, field) = RB_LEFT(old, field);		\
		RB_RIGHT(elm, field) = RB_RIGHT(old, field);		\
		RB_COLOR(elm, field) = RB_COLOR(old, field);		\
		if (RB_PARENT(old, field) == NULL)			\
			RB_ROOT(head) = (elm);				\
		else if (RB_LEFT(RB_PARENT(old, field), field) == old)	\
			RB_LEFT(RB_PARENT(old, field), field) = (elm);	\
		else							\
			RB_RIGHT(RB_PARENT(old, field), field) = (elm);	\
		if (RB_LEFT(elm, field) != NULL)			\
			RB_PARENT(RB_LEFT(elm, field), field) = (elm);	\
		if (RB_RIGHT(elm, field) != NULL)			\
			RB_PARENT(RB_RIGHT(elm, field), field) = (elm);	\
									\
		/* The gap is under the successor when it was a child. */ \
		if (parent == old)					\
			parent = (elm);					\
		if (color == RB_BLACK)					\
			name##_RB_REMOVE_COLOR(head, parent, child);	\
		return (old);						\
	}								\
									\
	parent = RB_PARENT(elm, field);					\
	color = RB_COLOR(elm, field);					\
	if (child != NULL)						\
		RB_PARENT(child, field) = parent;			\
	if (parent == NULL)						\
		RB_ROOT(head) = child;					\
	else if (RB_LEFT(parent, field) == (elm))			\
		RB_LEFT(parent, field) = child;				\
	else								\
		RB_RIGHT(parent, field) = child;			\
	if (color == RB_BLACK)						\
		name##_RB_REMOVE_COLOR(head, parent, child);		\
	return (old);							\
}									\
									\
/* Puts a record in, or reports the one already there. */		\
struct type *								\
name##_RB_INSERT(struct name *head, struct type *elm)			\
{									\
	struct type *parent = NULL;					\
	struct type *walk = RB_ROOT(head);				\
	int comp = 0;							\
									\
	while (walk != NULL) {						\
		parent = walk;						\
		comp = (cmp)(elm, parent);				\
		if (comp < 0)						\
			walk = RB_LEFT(walk, field);			\
		else if (comp > 0)					\
			walk = RB_RIGHT(walk, field);			\
		else							\
			return (walk);					\
	}								\
	RB_PARENT(elm, field) = parent;					\
	RB_LEFT(elm, field) = NULL;					\
	RB_RIGHT(elm, field) = NULL;					\
	RB_COLOR(elm, field) = RB_RED;					\
	if (parent == NULL)						\
		RB_ROOT(head) = (elm);					\
	else if (comp < 0)						\
		RB_LEFT(parent, field) = (elm);				\
	else								\
		RB_RIGHT(parent, field) = (elm);			\
	name##_RB_INSERT_COLOR(head, elm);				\
	return NULL;							\
}									\
									\
struct type *								\
name##_RB_FIND(struct name *head, struct type *key)			\
{									\
	struct type *walk = RB_ROOT(head);				\
	int comp;							\
									\
	while (walk != NULL) {						\
		comp = (cmp)(key, walk);				\
		if (comp < 0)						\
			walk = RB_LEFT(walk, field);			\
		else if (comp > 0)					\
			walk = RB_RIGHT(walk, field);			\
		else							\
			return (walk);					\
	}								\
	return NULL;							\
}									\
									\
/* The first record that is not less than the key. */			\
struct type *								\
name##_RB_NFIND(struct name *head, struct type *key)			\
{									\
	struct type *walk = RB_ROOT(head);				\
	struct type *result = NULL;					\
	int comp;							\
									\
	while (walk != NULL) {						\
		comp = (cmp)(key, walk);				\
		if (comp < 0) {						\
			result = walk;					\
			walk = RB_LEFT(walk, field);			\
		} else if (comp > 0) {					\
			walk = RB_RIGHT(walk, field);			\
		} else							\
			return (walk);					\
	}								\
	return (result);						\
}									\
									\
/* The next record in order: down the right subtree, or up and left. */	\
struct type *								\
name##_RB_NEXT(struct type *elm)					\
{									\
	if (RB_RIGHT(elm, field) != NULL) {				\
		elm = RB_RIGHT(elm, field);				\
		while (RB_LEFT(elm, field) != NULL)			\
			elm = RB_LEFT(elm, field);			\
	} else {							\
		while (RB_PARENT(elm, field) != NULL &&			\
		    elm == RB_RIGHT(RB_PARENT(elm, field), field))	\
			elm = RB_PARENT(elm, field);			\
		elm = RB_PARENT(elm, field);				\
	}								\
	return (elm);							\
}									\
									\
struct type *								\
name##_RB_PREV(struct type *elm)					\
{									\
	if (RB_LEFT(elm, field) != NULL) {				\
		elm = RB_LEFT(elm, field);				\
		while (RB_RIGHT(elm, field) != NULL)			\
			elm = RB_RIGHT(elm, field);			\
	} else {							\
		while (RB_PARENT(elm, field) != NULL &&			\
		    elm == RB_LEFT(RB_PARENT(elm, field), field))	\
			elm = RB_PARENT(elm, field);			\
		elm = RB_PARENT(elm, field);				\
	}								\
	return (elm);							\
}									\
									\
struct type *								\
name##_RB_MINMAX(struct name *head, int val)				\
{									\
	struct type *walk = RB_ROOT(head);				\
	struct type *result = NULL;					\
									\
	while (walk != NULL) {						\
		result = walk;						\
		if (val < 0)						\
			walk = RB_LEFT(walk, field);			\
		else							\
			walk = RB_RIGHT(walk, field);			\
	}								\
	return (result);						\
}

#define RB_INSERT(name, head, elm)	name##_RB_INSERT(head, elm)
#define RB_REMOVE(name, head, elm)	name##_RB_REMOVE(head, elm)
#define RB_FIND(name, head, elm)	name##_RB_FIND(head, elm)
#define RB_NFIND(name, head, elm)	name##_RB_NFIND(head, elm)
#define RB_NEXT(name, head, elm)	name##_RB_NEXT(elm)
#define RB_PREV(name, head, elm)	name##_RB_PREV(elm)
#define RB_MIN(name, head)		name##_RB_MINMAX(head, RB_NEGINF)
#define RB_MAX(name, head)		name##_RB_MINMAX(head, RB_INF)

#define RB_FOREACH(variable, name, head)				\
	for ((variable) = RB_MIN(name, head);				\
	     (variable) != NULL;					\
	     (variable) = name##_RB_NEXT(variable))

/* Keeps the next record before the body runs, so the body may free it. */
#define RB_FOREACH_SAFE(variable, name, head, next)			\
	for ((variable) = RB_MIN(name, head);				\
	     (variable) != NULL &&					\
	     ((next) = name##_RB_NEXT(variable), 1);			\
	     (variable) = (next))

#define RB_FOREACH_REVERSE(variable, name, head)			\
	for ((variable) = RB_MAX(name, head);				\
	     (variable) != NULL;					\
	     (variable) = name##_RB_PREV(variable))

#define RB_FOREACH_REVERSE_SAFE(variable, name, head, prev)		\
	for ((variable) = RB_MAX(name, head);				\
	     (variable) != NULL &&					\
	     ((prev) = name##_RB_PREV(variable), 1);			\
	     (variable) = (prev))

#endif
