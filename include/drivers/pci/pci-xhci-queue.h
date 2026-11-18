/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI xHCI bulk endpoint request queue decisions
 *
 * A bulk endpoint whose class driver asked for it keeps several transfer
 * descriptors (TDs) on its ring at once.  The controller runs the TDs of a
 * ring in order (xHCI 1.2 section 4.9.2), so the requests form a FIFO in
 * the order they were put on the ring: the head is the TD the controller
 * works on, and an event names the head unless it is a late event of a TD
 * that has already been retired.  These helpers hold no lock and touch no
 * hardware; the controller driver calls them under its own lock.
 */

#ifndef KERN_DRIVERS_PCI_XHCI_QUEUE_H
#define KERN_DRIVERS_PCI_XHCI_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include <drivers/pci/pci-xhci-lifecycle.h>

/* The most requests one endpoint may have on its ring at once. */
#define DRV_XHCI_QUEUE_MAX 4U

/*
 * The TRB places kept free behind the producer when a TD goes behind
 * others.  After a short packet the controller still walks the rest of the
 * retired TD's TRBs to reach the next TD (xHCI 1.2 section 4.10.1.1), so
 * the places a retired TD leaves just before the head are not reused at
 * once.
 */
#define DRV_XHCI_QUEUE_SLACK 32U

/*
 * The xHCI completion codes that halt a bulk or interrupt endpoint
 * (xHCI 1.2 section 4.10.2): Babble Detected, USB Transaction Error,
 * Stall and Split Transaction Error.  Any other code halts the endpoint
 * only when its context says so.
 */
#define DRV_XHCI_COMPLETION_BABBLE 3U
#define DRV_XHCI_COMPLETION_TRANSACTION 4U
#define DRV_XHCI_COMPLETION_STALL 6U
#define DRV_XHCI_COMPLETION_SPLIT 36U

/*
 * The completion codes of a TD that Stop Endpoint stopped (xHCI 1.2
 * section 6.4.5): Stopped, Stopped - Length Invalid and Stopped - Short
 * Packet (the TD had already seen a short packet).
 */
#define DRV_XHCI_COMPLETION_STOPPED 26U
#define DRV_XHCI_COMPLETION_STOPPED_LENGTH 27U
#define DRV_XHCI_COMPLETION_STOPPED_SHORT 28U

/*
 * The requests of one endpoint, oldest first: entries[head] is the head and
 * count says how many follow it around the array.  The entries are the
 * driver's own request objects.
 */
struct drv_xhci_queue {
	void *entries[DRV_XHCI_QUEUE_MAX];
	unsigned head;
	unsigned count;
};

/* Where one queued TD lies on its ring. */
struct drv_xhci_queue_td {
	unsigned first_trb;
	unsigned trb_count;
};

/* What admission decides for one more TD on an endpoint. */
enum drv_xhci_queue_admission {
	/* The TD may go on the ring. */
	DRV_XHCI_QUEUE_ADMIT = 0,
	/* The endpoint already has as many requests as its depth allows. */
	DRV_XHCI_QUEUE_DEPTH_FULL,
	/* The ring has no room for the TD behind those already on it. */
	DRV_XHCI_QUEUE_RING_FULL
};

/* Which queued request an event names. */
enum drv_xhci_queue_match {
	/* None: a late event of a retired TD, or of another endpoint. */
	DRV_XHCI_QUEUE_MATCH_NONE = 0,
	/* The head, the TD the controller works on. */
	DRV_XHCI_QUEUE_MATCH_HEAD,
	/* A request behind the head, which the controller has not reached. */
	DRV_XHCI_QUEUE_MATCH_BEHIND
};

/* Empties a queue. */
static inline void
drv_xhci_queue_init(
	struct drv_xhci_queue *queue)
{
	unsigned index;

	/* Forgets every entry. */
	for (index = 0; index < DRV_XHCI_QUEUE_MAX; index++)
		queue->entries[index] = NULL;
	queue->head = 0;
	queue->count = 0;
}

/* Returns the request at a position counted from the head, or NULL past the last. */
static inline void *
drv_xhci_queue_at(
	const struct drv_xhci_queue *queue,
	unsigned position)
{
	/* A position past the last request names nothing. */
	if (position >= queue->count)
		return NULL;

	/* Succeeded: the entry that many places behind the head. */
	return queue->entries[(queue->head + position) % DRV_XHCI_QUEUE_MAX];
}

/* Returns the position of a request in a queue, or -1 when it is not queued. */
static inline int
drv_xhci_queue_find(
	const struct drv_xhci_queue *queue,
	const void *entry)
{
	const void *queued;
	unsigned position;

	/* Looks at each request from the head. */
	for (position = 0; position < queue->count; position++) {
		/* The request is at this position. */
		queued = drv_xhci_queue_at(queue, position);
		if (queued == entry)
			return (int)position;
	}

	/* Not queued. */
	return -1;
}

/* Puts a request behind the others; 0, or -1 when the queue is full or the entry NULL. */
static inline int
drv_xhci_queue_push(
	struct drv_xhci_queue *queue,
	void *entry)
{
	/* A full queue and a missing entry are refused. */
	if (entry == NULL || queue->count >= DRV_XHCI_QUEUE_MAX)
		return -1;

	/* Places the entry behind the last one. */
	queue->entries[(queue->head + queue->count) % DRV_XHCI_QUEUE_MAX] = entry;
	queue->count++;

	/* Succeeded. */
	return 0;
}

/*
 * Takes a request out of a queue wherever it is, keeping the others in
 * order; 1 when it was queued, 0 when it was not.
 */
static inline int
drv_xhci_queue_remove(
	struct drv_xhci_queue *queue,
	const void *entry)
{
	unsigned from;
	unsigned to;
	int position;

	/* Finds the entry. */
	position = drv_xhci_queue_find(queue, entry);
	if (position < 0)
		return 0;

	/* The head goes by moving the head on. */
	if (position == 0) {
		queue->entries[queue->head] = NULL;
		queue->head = (queue->head + 1U) % DRV_XHCI_QUEUE_MAX;
		queue->count--;
		return 1;
	}

	/* Moves every later entry one place forward over the one taken. */
	for (to = (unsigned)position; to + 1U < queue->count; to++) {
		from = to + 1U;
		queue->entries[(queue->head + to) % DRV_XHCI_QUEUE_MAX] =
			queue->entries[(queue->head + from) % DRV_XHCI_QUEUE_MAX];
	}

	/* Forgets the slot the last entry left. */
	queue->entries[(queue->head + queue->count - 1U) % DRV_XHCI_QUEUE_MAX] = NULL;
	queue->count--;

	/* Succeeded. */
	return 1;
}

/*
 * Counts the TRB places the queued TDs take on a ring of ring_trbs entries
 * whose last is the Link TRB: from the head's first TRB up to the
 * producer's enqueue index.  An empty queue takes none.
 */
static inline unsigned
drv_xhci_queue_ring_used(
	unsigned ring_trbs,
	unsigned head_first_trb,
	unsigned enqueue,
	unsigned count)
{
	unsigned usable;

	/* An empty queue holds no TRB, and a malformed ring has no room to measure. */
	if (count == 0 || ring_trbs < 2U)
		return 0;
	usable = ring_trbs - 1U;

	/* Indexes outside the usable places cannot come from a valid ring. */
	if (head_first_trb >= usable || enqueue >= usable)
		return usable;

	/* Succeeded: the distance from the head around to the producer. */
	return (enqueue + usable - head_first_trb) % usable;
}

/*
 * Decides whether one more TD of trb_count TRBs may go on an endpoint with
 * count requests queued, depth allowed and used TRB places taken.  One
 * place always stays free, so the producer never reaches the head's first
 * TRB and a full ring never looks empty, and behind other TDs
 * DRV_XHCI_QUEUE_SLACK more.
 */
static inline enum drv_xhci_queue_admission
drv_xhci_queue_admit(
	unsigned ring_trbs,
	unsigned count,
	unsigned depth,
	unsigned used,
	unsigned trb_count)
{
	unsigned usable;
	unsigned reserve;

	/* A depth of zero is one, and none may exceed the queue's capacity. */
	if (depth == 0)
		depth = 1;
	if (depth > DRV_XHCI_QUEUE_MAX)
		depth = DRV_XHCI_QUEUE_MAX;

	/* The endpoint has as many requests as it may. */
	if (count >= depth)
		return DRV_XHCI_QUEUE_DEPTH_FULL;

	/* A ring too small for any TD has no room. */
	if (ring_trbs < 3U)
		return DRV_XHCI_QUEUE_RING_FULL;
	usable = ring_trbs - 1U;

	/* One free place, and the slack behind other TDs. */
	reserve = 1U;
	if (count != 0)
		reserve += DRV_XHCI_QUEUE_SLACK;

	/* The TD and the reserve must fit beside those already on the ring. */
	if (trb_count == 0 || used >= usable || reserve > usable - used)
		return DRV_XHCI_QUEUE_RING_FULL;
	if (trb_count > usable - used - reserve)
		return DRV_XHCI_QUEUE_RING_FULL;

	/* Succeeded. */
	return DRV_XHCI_QUEUE_ADMIT;
}

/*
 * Finds the queued TD a Transfer Event names.  tds lists the queue's TDs
 * from the head; position and trb_offset receive which one and which of
 * its TRBs.
 */
static inline enum drv_xhci_queue_match
drv_xhci_queue_match(
	uint64_t ring_address,
	unsigned ring_trbs,
	unsigned slot,
	unsigned dci,
	const struct drv_xhci_queue_td *tds,
	unsigned count,
	uint64_t event_pointer,
	unsigned event_slot,
	unsigned event_dci,
	unsigned *position,
	unsigned *trb_offset)
{
	unsigned index;
	int matches;

	/* Nothing found yet. */
	*position = 0;
	*trb_offset = 0;

	/* Looks at each TD from the head. */
	for (index = 0; index < count; index++) {
		/* Asks whether the event's TRB is one of this TD's. */
		matches = drv_xhci_transfer_event_matches(
			ring_address,
			ring_trbs,
			slot,
			dci,
			tds[index].first_trb,
			tds[index].trb_count,
			event_pointer,
			event_slot,
			event_dci,
			trb_offset);
		if (!matches)
			continue;

		/* The head is the TD the controller works on. */
		*position = index;
		if (index == 0)
			return DRV_XHCI_QUEUE_MATCH_HEAD;

		/* A TD behind the head. */
		return DRV_XHCI_QUEUE_MATCH_BEHIND;
	}

	/* No queued TD holds the TRB. */
	*trb_offset = 0;
	return DRV_XHCI_QUEUE_MATCH_NONE;
}

/*
 * Tells whether a completion leaves its endpoint Halted, from the code or
 * the Endpoint State the output context reports; a halted endpoint runs no
 * TD until Reset Endpoint, so the TDs behind it may be retired.
 */
static inline int
drv_xhci_queue_completion_halts(
	unsigned code,
	enum drv_xhci_endpoint_state state)
{
	/* The context already says so. */
	if (state == DRV_XHCI_ENDPOINT_HALTED)
		return 1;

	/* Choose by the completion code. */
	switch (code) {
	case DRV_XHCI_COMPLETION_BABBLE:
	case DRV_XHCI_COMPLETION_TRANSACTION:
	case DRV_XHCI_COMPLETION_STALL:
	case DRV_XHCI_COMPLETION_SPLIT:
		/* A code the specification says halts the endpoint. */
		return 1;
	default:
		/* Any other leaves the endpoint as it was. */
		return 0;
	}
}

/*
 * Tells whether a completion code says Stop Endpoint stopped the TD, which
 * a suspend leaves on its ring to run on after the resume.
 */
static inline int
drv_xhci_completion_stopped(
	unsigned code)
{
	/* Choose by the completion code. */
	switch (code) {
	case DRV_XHCI_COMPLETION_STOPPED:
	case DRV_XHCI_COMPLETION_STOPPED_LENGTH:
	case DRV_XHCI_COMPLETION_STOPPED_SHORT:
		/* One of the three Stopped codes. */
		return 1;
	default:
		/* The TD ended some other way. */
		return 0;
	}
}

#endif /* KERN_DRIVERS_PCI_XHCI_QUEUE_H */
