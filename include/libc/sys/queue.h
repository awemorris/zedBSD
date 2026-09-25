/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_QUEUE_H
#define LIBC_SYS_QUEUE_H

/*
 * Lists and queues built from links the caller embeds in its own records.
 *
 * Four shapes, each paying for what it offers.  A singly-linked list is two
 * words and forgets what came before, so removing a record means finding it
 * again.  A doubly-linked list holds the address of the link that points at
 * it, which makes removal a fixed cost without a second pointer per record.
 * A simple queue adds a tail pointer, so a record can be appended.  A tail
 * queue is doubly linked and has both ends, and is what to reach for unless
 * one of the cheaper ones is plainly enough.
 *
 * The names and shapes are the ones portable software is written against.
 */

/*
 * Singly-linked list.
 */
#define SLIST_HEAD(name, type)						\
struct name {								\
	struct type *slh_first;						\
}

#define SLIST_HEAD_INITIALIZER(head) { NULL }

#define SLIST_ENTRY(type)						\
struct {								\
	struct type *sle_next;						\
}

#define SLIST_FIRST(head)	((head)->slh_first)
#define SLIST_END(head)		NULL
#define SLIST_EMPTY(head)	(SLIST_FIRST(head) == SLIST_END(head))
#define SLIST_NEXT(element, field) ((element)->field.sle_next)

#define SLIST_INIT(head) do {						\
	SLIST_FIRST(head) = SLIST_END(head);				\
} while (0)

#define SLIST_FOREACH(variable, head, field)				\
	for ((variable) = SLIST_FIRST(head);				\
	     (variable) != SLIST_END(head);				\
	     (variable) = SLIST_NEXT(variable, field))

/* Keeps the next link before the body runs, so the body may free it. */
#define SLIST_FOREACH_SAFE(variable, head, field, next)			\
	for ((variable) = SLIST_FIRST(head);				\
	     (variable) != SLIST_END(head) &&				\
	     ((next) = SLIST_NEXT(variable, field), 1);			\
	     (variable) = (next))

#define SLIST_INSERT_HEAD(head, element, field) do {			\
	SLIST_NEXT(element, field) = SLIST_FIRST(head);			\
	SLIST_FIRST(head) = (element);					\
} while (0)

#define SLIST_INSERT_AFTER(listed, element, field) do {			\
	SLIST_NEXT(element, field) = SLIST_NEXT(listed, field);		\
	SLIST_NEXT(listed, field) = (element);				\
} while (0)

#define SLIST_REMOVE_HEAD(head, field) do {				\
	SLIST_FIRST(head) = SLIST_NEXT(SLIST_FIRST(head), field);	\
} while (0)

#define SLIST_REMOVE_AFTER(listed, field) do {				\
	SLIST_NEXT(listed, field) =					\
	    SLIST_NEXT(SLIST_NEXT(listed, field), field);		\
} while (0)

/* Walks to the link that points at the record, there being no back link. */
#define SLIST_REMOVE(head, element, type, field) do {			\
	if (SLIST_FIRST(head) == (element)) {				\
		SLIST_REMOVE_HEAD(head, field);				\
	} else {							\
		struct type *_queue_at = SLIST_FIRST(head);		\
									\
		while (SLIST_NEXT(_queue_at, field) != (element))	\
			_queue_at = SLIST_NEXT(_queue_at, field);	\
		SLIST_REMOVE_AFTER(_queue_at, field);			\
	}								\
} while (0)

#define SLIST_CONCAT(first, second, type, field) do {			\
	if (SLIST_EMPTY(first)) {					\
		if ((SLIST_FIRST(first) = SLIST_FIRST(second)) != NULL)	\
			SLIST_INIT(second);				\
	} else if (!SLIST_EMPTY(second)) {				\
		struct type *_queue_at = SLIST_FIRST(first);		\
									\
		while (SLIST_NEXT(_queue_at, field) != NULL)		\
			_queue_at = SLIST_NEXT(_queue_at, field);	\
		SLIST_NEXT(_queue_at, field) = SLIST_FIRST(second);	\
		SLIST_INIT(second);					\
	}								\
} while (0)

/*
 * Doubly-linked list.
 *
 * A record holds the address of the link that points at it rather than the
 * record that owns it, so removing one costs the same whether or not it is
 * the first, and the head needs no special case.
 */
#define LIST_HEAD(name, type)						\
struct name {								\
	struct type *lh_first;						\
}

#define LIST_HEAD_INITIALIZER(head) { NULL }

#define LIST_ENTRY(type)						\
struct {								\
	struct type *le_next;						\
	struct type **le_prev;						\
}

#define LIST_FIRST(head)	((head)->lh_first)
#define LIST_END(head)		NULL
#define LIST_EMPTY(head)	(LIST_FIRST(head) == LIST_END(head))
#define LIST_NEXT(element, field) ((element)->field.le_next)

#define LIST_INIT(head) do {						\
	LIST_FIRST(head) = LIST_END(head);				\
} while (0)

#define LIST_FOREACH(variable, head, field)				\
	for ((variable) = LIST_FIRST(head);				\
	     (variable) != LIST_END(head);				\
	     (variable) = LIST_NEXT(variable, field))

#define LIST_FOREACH_SAFE(variable, head, field, next)			\
	for ((variable) = LIST_FIRST(head);				\
	     (variable) != LIST_END(head) &&				\
	     ((next) = LIST_NEXT(variable, field), 1);			\
	     (variable) = (next))

#define LIST_INSERT_HEAD(head, element, field) do {			\
	if ((LIST_NEXT(element, field) = LIST_FIRST(head)) != NULL)	\
		LIST_FIRST(head)->field.le_prev =			\
		    &LIST_NEXT(element, field);				\
	LIST_FIRST(head) = (element);					\
	(element)->field.le_prev = &LIST_FIRST(head);			\
} while (0)

#define LIST_INSERT_AFTER(listed, element, field) do {			\
	if ((LIST_NEXT(element, field) = LIST_NEXT(listed, field)) != NULL) \
		LIST_NEXT(listed, field)->field.le_prev =		\
		    &LIST_NEXT(element, field);				\
	LIST_NEXT(listed, field) = (element);				\
	(element)->field.le_prev = &LIST_NEXT(listed, field);		\
} while (0)

#define LIST_INSERT_BEFORE(listed, element, field) do {			\
	(element)->field.le_prev = (listed)->field.le_prev;		\
	LIST_NEXT(element, field) = (listed);				\
	*(listed)->field.le_prev = (element);				\
	(listed)->field.le_prev = &LIST_NEXT(element, field);		\
} while (0)

#define LIST_REMOVE(element, field) do {				\
	if (LIST_NEXT(element, field) != NULL)				\
		LIST_NEXT(element, field)->field.le_prev =		\
		    (element)->field.le_prev;				\
	*(element)->field.le_prev = LIST_NEXT(element, field);		\
} while (0)

#define LIST_REPLACE(listed, element, field) do {			\
	if ((LIST_NEXT(element, field) = LIST_NEXT(listed, field)) != NULL) \
		LIST_NEXT(element, field)->field.le_prev =		\
		    &LIST_NEXT(element, field);				\
	(element)->field.le_prev = (listed)->field.le_prev;		\
	*(element)->field.le_prev = (element);				\
} while (0)

/*
 * Simple queue.
 *
 * A singly-linked list that also remembers where the last link is, so a
 * record can be appended without walking the whole of it.
 */
#define SIMPLEQ_HEAD(name, type)					\
struct name {								\
	struct type *sqh_first;						\
	struct type **sqh_last;						\
}

#define SIMPLEQ_HEAD_INITIALIZER(head) { NULL, &(head).sqh_first }

#define SIMPLEQ_ENTRY(type)						\
struct {								\
	struct type *sqe_next;						\
}

#define SIMPLEQ_FIRST(head)	((head)->sqh_first)
#define SIMPLEQ_END(head)	NULL
#define SIMPLEQ_EMPTY(head)	(SIMPLEQ_FIRST(head) == SIMPLEQ_END(head))
#define SIMPLEQ_NEXT(element, field) ((element)->field.sqe_next)

#define SIMPLEQ_INIT(head) do {						\
	(head)->sqh_first = NULL;					\
	(head)->sqh_last = &(head)->sqh_first;				\
} while (0)

#define SIMPLEQ_FOREACH(variable, head, field)				\
	for ((variable) = SIMPLEQ_FIRST(head);				\
	     (variable) != SIMPLEQ_END(head);				\
	     (variable) = SIMPLEQ_NEXT(variable, field))

#define SIMPLEQ_FOREACH_SAFE(variable, head, field, next)		\
	for ((variable) = SIMPLEQ_FIRST(head);				\
	     (variable) != SIMPLEQ_END(head) &&				\
	     ((next) = SIMPLEQ_NEXT(variable, field), 1);		\
	     (variable) = (next))

#define SIMPLEQ_INSERT_HEAD(head, element, field) do {			\
	if ((SIMPLEQ_NEXT(element, field) = (head)->sqh_first) == NULL)	\
		(head)->sqh_last = &SIMPLEQ_NEXT(element, field);	\
	(head)->sqh_first = (element);					\
} while (0)

#define SIMPLEQ_INSERT_TAIL(head, element, field) do {			\
	SIMPLEQ_NEXT(element, field) = NULL;				\
	*(head)->sqh_last = (element);					\
	(head)->sqh_last = &SIMPLEQ_NEXT(element, field);		\
} while (0)

#define SIMPLEQ_INSERT_AFTER(head, listed, element, field) do {		\
	if ((SIMPLEQ_NEXT(element, field) =				\
	    SIMPLEQ_NEXT(listed, field)) == NULL)			\
		(head)->sqh_last = &SIMPLEQ_NEXT(element, field);	\
	SIMPLEQ_NEXT(listed, field) = (element);			\
} while (0)

#define SIMPLEQ_REMOVE_HEAD(head, field) do {				\
	if (((head)->sqh_first = SIMPLEQ_NEXT((head)->sqh_first,	\
	    field)) == NULL)						\
		(head)->sqh_last = &(head)->sqh_first;			\
} while (0)

#define SIMPLEQ_REMOVE_AFTER(head, listed, field) do {			\
	if ((SIMPLEQ_NEXT(listed, field) =				\
	    SIMPLEQ_NEXT(SIMPLEQ_NEXT(listed, field), field)) == NULL)	\
		(head)->sqh_last = &SIMPLEQ_NEXT(listed, field);		\
} while (0)

#define SIMPLEQ_CONCAT(first, second) do {				\
	if (!SIMPLEQ_EMPTY(second)) {					\
		*(first)->sqh_last = (second)->sqh_first;		\
		(first)->sqh_last = (second)->sqh_last;			\
		SIMPLEQ_INIT(second);					\
	}								\
} while (0)

/*
 * Tail queue.
 *
 * Doubly linked and open at both ends, so a record can be added or removed
 * anywhere at a fixed cost.  It is the general case: reach for one of the
 * others only when what they leave out is plainly not needed.
 */
#define TAILQ_HEAD(name, type)						\
struct name {								\
	struct type *tqh_first;						\
	struct type **tqh_last;						\
}

#define TAILQ_HEAD_INITIALIZER(head) { NULL, &(head).tqh_first }

#define TAILQ_ENTRY(type)						\
struct {								\
	struct type *tqe_next;						\
	struct type **tqe_prev;						\
}

#define TAILQ_FIRST(head)	((head)->tqh_first)
#define TAILQ_END(head)		NULL
#define TAILQ_EMPTY(head)	(TAILQ_FIRST(head) == TAILQ_END(head))
#define TAILQ_NEXT(element, field) ((element)->field.tqe_next)

/*
 * The link before the head is the head's own last pointer, which is how the
 * queue is walked backwards without a separate back link in the head.
 */
#define TAILQ_LAST(head, headname)					\
	(*(((struct headname *)((head)->tqh_last))->tqh_last))

#define TAILQ_PREV(element, headname, field)				\
	(*(((struct headname *)((element)->field.tqe_prev))->tqh_last))

#define TAILQ_INIT(head) do {						\
	(head)->tqh_first = NULL;					\
	(head)->tqh_last = &(head)->tqh_first;				\
} while (0)

#define TAILQ_FOREACH(variable, head, field)				\
	for ((variable) = TAILQ_FIRST(head);				\
	     (variable) != TAILQ_END(head);				\
	     (variable) = TAILQ_NEXT(variable, field))

#define TAILQ_FOREACH_SAFE(variable, head, field, next)			\
	for ((variable) = TAILQ_FIRST(head);				\
	     (variable) != TAILQ_END(head) &&				\
	     ((next) = TAILQ_NEXT(variable, field), 1);			\
	     (variable) = (next))

#define TAILQ_FOREACH_REVERSE(variable, head, headname, field)		\
	for ((variable) = TAILQ_LAST(head, headname);			\
	     (variable) != TAILQ_END(head);				\
	     (variable) = TAILQ_PREV(variable, headname, field))

#define TAILQ_FOREACH_REVERSE_SAFE(variable, head, headname, field, prev) \
	for ((variable) = TAILQ_LAST(head, headname);			\
	     (variable) != TAILQ_END(head) &&				\
	     ((prev) = TAILQ_PREV(variable, headname, field), 1);	\
	     (variable) = (prev))

#define TAILQ_INSERT_HEAD(head, element, field) do {			\
	if ((TAILQ_NEXT(element, field) = (head)->tqh_first) != NULL)	\
		(head)->tqh_first->field.tqe_prev =			\
		    &TAILQ_NEXT(element, field);			\
	else								\
		(head)->tqh_last = &TAILQ_NEXT(element, field);		\
	(head)->tqh_first = (element);					\
	(element)->field.tqe_prev = &(head)->tqh_first;			\
} while (0)

#define TAILQ_INSERT_TAIL(head, element, field) do {			\
	TAILQ_NEXT(element, field) = NULL;				\
	(element)->field.tqe_prev = (head)->tqh_last;			\
	*(head)->tqh_last = (element);					\
	(head)->tqh_last = &TAILQ_NEXT(element, field);			\
} while (0)

#define TAILQ_INSERT_AFTER(head, listed, element, field) do {		\
	if ((TAILQ_NEXT(element, field) =				\
	    TAILQ_NEXT(listed, field)) != NULL)				\
		TAILQ_NEXT(element, field)->field.tqe_prev =		\
		    &TAILQ_NEXT(element, field);			\
	else								\
		(head)->tqh_last = &TAILQ_NEXT(element, field);		\
	TAILQ_NEXT(listed, field) = (element);				\
	(element)->field.tqe_prev = &TAILQ_NEXT(listed, field);		\
} while (0)

#define TAILQ_INSERT_BEFORE(listed, element, field) do {		\
	(element)->field.tqe_prev = (listed)->field.tqe_prev;		\
	TAILQ_NEXT(element, field) = (listed);				\
	*(listed)->field.tqe_prev = (element);				\
	(listed)->field.tqe_prev = &TAILQ_NEXT(element, field);		\
} while (0)

#define TAILQ_REMOVE(head, element, field) do {				\
	if ((TAILQ_NEXT(element, field)) != NULL)			\
		TAILQ_NEXT(element, field)->field.tqe_prev =		\
		    (element)->field.tqe_prev;				\
	else								\
		(head)->tqh_last = (element)->field.tqe_prev;		\
	*(element)->field.tqe_prev = TAILQ_NEXT(element, field);		\
} while (0)

#define TAILQ_REPLACE(head, listed, element, field) do {		\
	if ((TAILQ_NEXT(element, field) =				\
	    TAILQ_NEXT(listed, field)) != NULL)				\
		TAILQ_NEXT(element, field)->field.tqe_prev =		\
		    &TAILQ_NEXT(element, field);			\
	else								\
		(head)->tqh_last = &TAILQ_NEXT(element, field);		\
	(element)->field.tqe_prev = (listed)->field.tqe_prev;		\
	*(element)->field.tqe_prev = (element);				\
} while (0)

#define TAILQ_CONCAT(first, second, field) do {				\
	if (!TAILQ_EMPTY(second)) {					\
		*(first)->tqh_last = (second)->tqh_first;		\
		(second)->tqh_first->field.tqe_prev = (first)->tqh_last; \
		(first)->tqh_last = (second)->tqh_last;			\
		TAILQ_INIT(second);					\
	}								\
} while (0)

#endif
