/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clock of resident file pages: one ring of the pages the page cache may
 * give back, a hand that sweeps it, and a referenced bit on each page that a
 * hit sets and the hand clears, so that a page used since the hand last
 * passed it gets a second chance before it is offered.  The ring and the
 * hand are the caller's to lock; the referenced bit may be set without that
 * lock.  Pure, so that the host can test it.
 */

#ifndef KERN_PAGE_CLOCK_H
#define KERN_PAGE_CLOCK_H

#include <stddef.h>

/*
 * One page's place on the clock, embedded in the page's descriptor.
 *
 * A node is on at most one clock.  linked says whether it is; previous and
 * next are meaningful only while it is.  referenced is a hint: it is read and
 * written with relaxed atomic accesses, because a hit sets it under the
 * page's own lock while the hand clears it under the clock's.
 */
struct page_clock_node {
	struct page_clock_node *previous;
	struct page_clock_node *next;
	unsigned linked;
	unsigned referenced;
};

/*
 * A ring of nodes and its hand.
 *
 * The hand is the node the next sweep examines first, and NULL exactly when
 * the ring is empty.  A node linked in goes just behind the hand, so the
 * hand reaches it after every node already on the ring.
 */
struct page_clock {
	struct page_clock_node *hand;
	size_t count;
};

/*
 * Puts a node on the ring, just behind the hand, with its referenced bit
 * clear.
 */
static inline void
page_clock_link(
	struct page_clock *clock,
	struct page_clock_node *node)
{
	struct page_clock_node *hand;

	/* A node alone makes a ring of one, and the hand is on it. */
	__atomic_store_n(&node->referenced, 0U, __ATOMIC_RELAXED);
	hand = clock->hand;
	if (hand == NULL) {
		node->previous = node;
		node->next = node;
		clock->hand = node;
	} else {
		/* Otherwise it goes between the hand and the node before it. */
		node->previous = hand->previous;
		node->next = hand;
		hand->previous->next = node;
		hand->previous = node;
	}

	/* Records the membership the unlink relies on. */
	node->linked = 1U;
	clock->count++;
}

/*
 * Takes a node off the ring; the hand moves to the next node when it was on
 * this one.  A node that is not on the ring is left alone.
 */
static inline void
page_clock_unlink(
	struct page_clock *clock,
	struct page_clock_node *node)
{
	/* Leaves a node that was never linked, or was already taken off. */
	if (!node->linked)
		return;

	/* The last node empties the ring; otherwise the hand steps past it. */
	if (node->next == node) {
		clock->hand = NULL;
	} else {
		if (clock->hand == node)
			clock->hand = node->next;
		node->previous->next = node->next;
		node->next->previous = node->previous;
	}

	/* Forgets the place, so that a second unlink does nothing. */
	node->previous = NULL;
	node->next = NULL;
	node->linked = 0U;
	clock->count--;
}

/*
 * Records a use of the page, giving it a second chance the next time the
 * hand reaches it.
 */
static inline void
page_clock_touch(
	struct page_clock_node *node)
{
	__atomic_store_n(&node->referenced, 1U, __ATOMIC_RELAXED);
}

/*
 * Moves a node to the hand with its referenced bit clear, so that it is the
 * next one offered: a page read once in a long sequential stream goes there
 * instead of pushing out pages that are used again.
 */
static inline void
page_clock_demote(
	struct page_clock *clock,
	struct page_clock_node *node)
{
	/* Leaves a node that is not on the ring. */
	if (!node->linked)
		return;

	/* Takes it out and puts it back just behind the hand, then turns the hand to it. */
	page_clock_unlink(clock, node);
	page_clock_link(clock, node);
	clock->hand = node;
}

/*
 * Sweeps the hand over at most *budget nodes and offers the first one whose
 * referenced bit was clear; the bits of the nodes passed on the way are
 * cleared, and *budget loses one for every node examined.  The hand is left
 * after the offered node, so a node the caller cannot take is offered again
 * only after a whole turn.  Reports NULL for an empty ring or when the
 * budget runs out first.
 */
static inline struct page_clock_node *
page_clock_sweep(
	struct page_clock *clock,
	size_t *budget)
{
	struct page_clock_node *node;
	unsigned referenced;

	/* Examines one node a step until one has not been used since the last turn. */
	while (*budget != 0U && clock->hand != NULL) {
		node = clock->hand;
		clock->hand = node->next;
		(*budget)--;

		/* A used page loses its bit and waits for the next turn. */
		referenced = __atomic_exchange_n(&node->referenced, 0U, __ATOMIC_RELAXED);
		if (referenced != 0U)
			continue;

		/* Offers a page nobody used since the hand last passed it. */
		return node;
	}

	/* Nothing to offer within the budget. */
	return NULL;
}

#endif
