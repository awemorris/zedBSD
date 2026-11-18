/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bookkeeping of an xHCI isochronous endpoint (pci-xhci-isoch.h,
 * ws197-p007a1).  Nothing here touches the controller: the driver writes
 * the TRBs at the slots enqueue gives, feeds each Transfer Event to
 * drv_xhci_isoch_event, issues the commands the reported bits ask for,
 * and takes the finished requests in their order.
 */

#include <drivers/pci/pci-xhci-isoch.h>

#include <uapi/errno.h>

#include <stddef.h>

static struct drv_xhci_isoch_request *isoch_request(struct drv_xhci_isoch *isoch, unsigned position);
static void isoch_record(struct drv_xhci_isoch_request *request, unsigned packet, enum drv_xhci_iso_result result, uint32_t actual);
static void isoch_skip(struct drv_xhci_isoch *isoch, unsigned position, unsigned packet);
static unsigned isoch_retire(struct drv_xhci_isoch *isoch);
static int isoch_running(const struct drv_xhci_isoch *isoch);

/*
 * Starts an endpoint's bookkeeping for a ring of slots TD slots (its
 * TRBs less the link TRB), empty and NORMAL.
 */
void
drv_xhci_isoch_init(
	struct drv_xhci_isoch *isoch,
	unsigned slots)
{
	unsigned position;

	/* Nothing queued, nothing counted. */
	isoch->slots = slots;
	isoch->producer = 0U;
	isoch->consumer = 0U;
	isoch->outstanding = 0U;
	isoch->state = DRV_XHCI_ISOCH_NORMAL;
	isoch->head = 0U;
	isoch->queued = 0U;
	isoch->missed_service = 0U;
	isoch->underruns = 0U;
	isoch->overruns = 0U;
	isoch->stopped = 0U;
	isoch->foreign_stops = 0U;
	isoch->strays = 0U;

	/* The requests' places, empty. */
	for (position = 0U; position < DRV_XHCI_ISOCH_QUEUE_MAX; position++) {
		isoch->requests[position].tag = 0U;
		isoch->requests[position].first = 0U;
		isoch->requests[position].count = 0U;
		isoch->requests[position].done = 0U;
		isoch->requests[position].retired = 0U;
	}
}

/*
 * Tells how many TDs the ring takes now: its slots less those on it and
 * one kept free, so that a full ring never looks empty.
 */
unsigned
drv_xhci_isoch_room(
	const struct drv_xhci_isoch *isoch)
{
	unsigned used;

	/* A ring too small for any TD. */
	if (isoch->slots < 2U)
		return 0U;

	/* The slots taken, and the one kept free. */
	used = isoch->outstanding + 1U;
	if (used >= isoch->slots)
		return 0U;

	/* Succeeded: the room. */
	return isoch->slots - used;
}

/*
 * Queues a request of count TDs (the lengths of its packets): it takes the
 * slots from the producer on, and *first is the first of them.  Returns 0,
 * EINVAL for a count out of range, EBUSY while an owner or a halt holds the
 * endpoint, or EAGAIN when the FIFO or the ring has no room.
 */
int
drv_xhci_isoch_enqueue(
	struct drv_xhci_isoch *isoch,
	uintptr_t tag,
	unsigned count,
	const uint32_t *lengths,
	unsigned *first)
{
	struct drv_xhci_isoch_request *request;
	unsigned room;
	unsigned packet;
	int running;

	/* A request of one to PACKETS_MAX TDs. */
	if (count == 0U || count > DRV_XHCI_ISOCH_PACKETS_MAX)
		return EINVAL;

	/* Never on a retained endpoint; only while nothing else holds it. */
	if (isoch->state == DRV_XHCI_ISOCH_RETAINED)
		return EIO;
	running = isoch_running(isoch);
	if (!running)
		return EBUSY;

	/* Room in the FIFO and on the ring. */
	if (isoch->queued == DRV_XHCI_ISOCH_QUEUE_MAX)
		return EAGAIN;
	room = drv_xhci_isoch_room(isoch);
	if (room < count)
		return EAGAIN;

	/* The request at the FIFO's tail, its TDs from the producer. */
	request = isoch_request(isoch, isoch->queued);
	request->tag = tag;
	request->first = isoch->producer;
	request->count = count;
	request->done = 0U;
	request->retired = 0U;
	for (packet = 0U; packet < count; packet++) {
		request->length[packet] = lengths[packet];
		request->actual[packet] = 0U;
		request->result[packet] = DRV_XHCI_ISO_PENDING;
	}

	/* The ring and the FIFO take it. */
	*first = isoch->producer;
	isoch->producer = (isoch->producer + count) % isoch->slots;
	isoch->outstanding += count;
	isoch->queued++;

	/* Succeeded: queued. */
	return 0;
}

/*
 * Tells whether the TD of a request at index (of count) blocks its event
 * interrupt (BEI): every TD but each eighth and the last, so that an
 * interrupt comes at least every DRV_XHCI_ISOCH_IOC_EVERY TDs.
 */
int
drv_xhci_isoch_bei(
	unsigned index,
	unsigned count)
{
	/* The last TD always interrupts. */
	if (index + 1U >= count)
		return 0;

	/* So does each eighth. */
	if ((index + 1U) % DRV_XHCI_ISOCH_IOC_EVERY == 0U)
		return 0;

	/* Succeeded: blocked. */
	return 1;
}

/*
 * Finds the request (its place in the FIFO from the head) and the packet
 * whose TD is at a ring slot.  A retired TD's slot may already hold a newer
 * TD, so only TDs not yet retired are found.  Returns 0, or ENOENT for a
 * slot no outstanding TD is at.
 */
int
drv_xhci_isoch_locate(
	const struct drv_xhci_isoch *isoch,
	unsigned slot,
	unsigned *position,
	unsigned *packet)
{
	const struct drv_xhci_isoch_request *request;
	unsigned offset;
	unsigned index;

	/* A slot of the ring only. */
	if (slot >= isoch->slots)
		return ENOENT;

	/* Each queued request's run of slots, the ring's wrap included. */
	for (index = 0U; index < isoch->queued; index++) {
		request = &isoch->requests[(isoch->head + index) % DRV_XHCI_ISOCH_QUEUE_MAX];
		offset = (slot + isoch->slots - request->first) % isoch->slots;
		if (offset < request->count && offset >= request->retired) {
			*position = index;
			*packet = offset;
			return 0;
		}
	}

	/* No queued TD there. */
	return ENOENT;
}

/*
 * Applies one Transfer Event of the endpoint: its completion code, whether
 * its TRB pointer names a TD (pointed) and the slot it names, and the
 * residual length.  Returns DRV_XHCI_ISOCH_* bits.
 */
unsigned
drv_xhci_isoch_event(
	struct drv_xhci_isoch *isoch,
	unsigned code,
	int pointed,
	unsigned slot,
	uint32_t residual)
{
	struct drv_xhci_isoch_request *request;
	enum drv_xhci_iso_result result;
	unsigned position;
	unsigned packet;
	unsigned flags;
	uint32_t actual;
	int running;
	int error;

	/* Stopped: counted only; the TD it names runs again (a Stop not this bookkeeping's owner's is counted apart). */
	running = isoch_running(isoch);
	if (code == DRV_XHCI_COMPLETION_STOPPED ||
	    code == DRV_XHCI_COMPLETION_STOPPED_LENGTH ||
	    code == DRV_XHCI_COMPLETION_STOPPED_SHORT) {
		isoch->stopped++;
		if (running)
			isoch->foreign_stops++;
		return 0U;
	}

	/* Missed Service, Underrun and Overrun: counted, and a skip wait while TDs are outstanding. */
	if (code == DRV_XHCI_COMPLETION_MISSED_SERVICE ||
	    code == DRV_XHCI_COMPLETION_RING_UNDERRUN ||
	    code == DRV_XHCI_COMPLETION_RING_OVERRUN) {
		if (code == DRV_XHCI_COMPLETION_MISSED_SERVICE)
			isoch->missed_service++;
		if (code == DRV_XHCI_COMPLETION_RING_UNDERRUN)
			isoch->underruns++;
		if (code == DRV_XHCI_COMPLETION_RING_OVERRUN)
			isoch->overruns++;

		/* Nothing to wait for, or an owner already holds the endpoint. */
		if (!running || isoch->outstanding == 0U)
			return 0U;

		/* The worker stops the endpoint if no TD event comes in time. */
		isoch->state = DRV_XHCI_ISOCH_SKIP_WAIT;
		return DRV_XHCI_ISOCH_WAKE;
	}

	/* A TD's event names its TD; one that names none is counted. */
	if (!pointed) {
		isoch->strays++;
		return 0U;
	}

	/* The TD it names, among those outstanding. */
	error = drv_xhci_isoch_locate(isoch, slot, &position, &packet);
	if (error != 0) {
		isoch->strays++;
		return 0U;
	}

	/* Its request. */
	request = isoch_request(isoch, position);

	/* The TD's result: whole, short by the residual, or an error. */
	flags = 0U;
	actual = 0U;
	if (code == DRV_XHCI_COMPLETION_SUCCESS) {
		result = DRV_XHCI_ISO_OK;
		actual = request->length[packet];
	} else if (code == DRV_XHCI_COMPLETION_SHORT_PACKET) {
		result = DRV_XHCI_ISO_SHORT;
		if (residual <= request->length[packet])
			actual = request->length[packet] - residual;
	} else {
		result = DRV_XHCI_ISO_ERROR;
	}

	/* While an owner or a halt holds the endpoint, the result is only recorded. */
	if (!running) {
		isoch_record(request, packet, result, actual);
		return 0U;
	}

	/* A babble or a transaction error may have halted the endpoint. */
	if (code == DRV_XHCI_COMPLETION_BABBLE || code == DRV_XHCI_COMPLETION_TRANSACTION)
		flags |= DRV_XHCI_ISOCH_CHECK_HALT;

	/* The skip rule, the TD's result, the skip wait over, and what can be retired. */
	isoch_skip(isoch, position, packet);
	isoch_record(request, packet, result, actual);
	isoch->state = DRV_XHCI_ISOCH_NORMAL;
	flags |= isoch_retire(isoch);

	/* Succeeded: what the driver does next. */
	return flags;
}

/*
 * Records that the endpoint context says Halted (read after a babble or a
 * transaction error): the worker recovers it.  An owner already holding the
 * endpoint reads the context itself.
 */
void
drv_xhci_isoch_halted(
	struct drv_xhci_isoch *isoch)
{
	int running;

	/* Only a running endpoint changes. */
	running = isoch_running(isoch);
	if (running)
		isoch->state = DRV_XHCI_ISOCH_HALT_PENDING;
}

/*
 * Records that an owner's command failed (it timed out, or the endpoint's
 * state could not be changed even after the context was read again): the
 * endpoint keeps its TDs, and with them their DMA, until a flush after
 * the controller's DMA is quiesced.
 */
void
drv_xhci_isoch_retain(
	struct drv_xhci_isoch *isoch)
{
	/* Held as it is. */
	isoch->state = DRV_XHCI_ISOCH_RETAINED;
}

/*
 * Makes the caller the endpoint's owner for a command: STOPPING (the
 * worker, after a skip wait), CANCELLING (a cancel or a quiesce, which
 * takes over a skip wait or a halt) or RECOVERING (the worker, after a
 * halt).  Returns 0, EALREADY when a cancel is already under way, EBUSY
 * when another owner holds it or there is nothing to do (wait and try
 * again), EIO on a retained endpoint, or EINVAL.
 */
int
drv_xhci_isoch_begin(
	struct drv_xhci_isoch *isoch,
	enum drv_xhci_isoch_state owner)
{
	/* A retained endpoint has no owner but the quiesce's flush. */
	if (isoch->state == DRV_XHCI_ISOCH_RETAINED)
		return EIO;

	/* A cancel under way answers every other cancel. */
	if (owner == DRV_XHCI_ISOCH_CANCELLING && isoch->state == DRV_XHCI_ISOCH_CANCELLING)
		return EALREADY;

	/* The owner each state lets in. */
	switch (owner) {
	case DRV_XHCI_ISOCH_STOPPING:
		if (isoch->state != DRV_XHCI_ISOCH_SKIP_WAIT)
			return EBUSY;
		break;
	case DRV_XHCI_ISOCH_CANCELLING:
		if (isoch->state == DRV_XHCI_ISOCH_STOPPING || isoch->state == DRV_XHCI_ISOCH_RECOVERING)
			return EBUSY;
		break;
	case DRV_XHCI_ISOCH_RECOVERING:
		if (isoch->state != DRV_XHCI_ISOCH_HALT_PENDING)
			return EBUSY;
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the caller owns the endpoint. */
	isoch->state = owner;
	return 0;
}

/*
 * Ends the worker's Stop after a skip wait: dequeue is the slot the
 * endpoint context's TR Dequeue Pointer names.  The TDs before it without
 * a result are MISSED; the TD there (stopped, it runs again) and those
 * after it stay.  Returns DRV_XHCI_ISOCH_* bits (RING when TDs are left).
 */
unsigned
drv_xhci_isoch_stop_done(
	struct drv_xhci_isoch *isoch,
	unsigned dequeue)
{
	struct drv_xhci_isoch_request *request;
	unsigned position;
	unsigned packet;
	unsigned slot;
	unsigned flags;
	int reached;

	/* Only the worker's Stop ends this way. */
	if (isoch->state != DRV_XHCI_ISOCH_STOPPING)
		return 0U;

	/* Each outstanding TD in the FIFO's order, until the one the controller stopped at. */
	reached = 0;
	for (position = 0U; position < isoch->queued && !reached; position++) {
		request = isoch_request(isoch, position);
		for (packet = request->retired; packet < request->count; packet++) {
			slot = (request->first + packet) % isoch->slots;
			if (slot == dequeue) {
				reached = 1;
				break;
			}

			/* Before it: missed, unless it has its result. */
			isoch_record(request, packet, DRV_XHCI_ISO_MISSED, 0U);
		}
	}

	/* Running again, what can be retired, and the endpoint rung when TDs are left. */
	isoch->state = DRV_XHCI_ISOCH_NORMAL;
	flags = isoch_retire(isoch);
	if (isoch->outstanding != 0U)
		flags |= DRV_XHCI_ISOCH_RING;

	/* Succeeded: what the driver does next. */
	return flags;
}

/*
 * Ends a cancel (result CANCELLED) or a recovery (result ERROR) after the
 * ring was emptied (xhci_endpoint_dequeue_all), or a retained endpoint
 * after the controller's DMA was quiesced: every TD without a result
 * takes the result, the ring is empty, and the endpoint runs again (it is
 * left stopped until the next enqueue rings it).  Returns
 * DRV_XHCI_ISOCH_COMPLETE when requests are to be taken.
 */
unsigned
drv_xhci_isoch_flush(
	struct drv_xhci_isoch *isoch,
	enum drv_xhci_iso_result result)
{
	struct drv_xhci_isoch_request *request;
	unsigned position;
	unsigned packet;

	/* Each TD without a result. */
	for (position = 0U; position < isoch->queued; position++) {
		request = isoch_request(isoch, position);
		for (packet = 0U; packet < request->count; packet++) {
			if (request->result[packet] == DRV_XHCI_ISO_PENDING)
				isoch_record(request, packet, result, 0U);
		}
	}

	/* The ring is empty: every TD retired, its consumer at its producer. */
	for (position = 0U; position < isoch->queued; position++) {
		request = isoch_request(isoch, position);
		request->retired = request->count;
	}

	/* Nothing on the ring. */
	isoch->consumer = isoch->producer;
	isoch->outstanding = 0U;
	isoch->state = DRV_XHCI_ISOCH_NORMAL;

	/* Succeeded: the requests to take. */
	if (isoch->queued == 0U)
		return 0U;
	return DRV_XHCI_ISOCH_COMPLETE;
}

/*
 * Takes the request at the FIFO's head when all its TDs have a result.
 * Returns 1 with a copy of it, or 0.
 */
int
drv_xhci_isoch_take(
	struct drv_xhci_isoch *isoch,
	struct drv_xhci_isoch_request *request)
{
	struct drv_xhci_isoch_request *head;

	/* A finished request at the head. */
	if (isoch->queued == 0U)
		return 0;
	head = isoch_request(isoch, 0U);
	if (head->done != head->count)
		return 0;

	/* Out of the FIFO. */
	*request = *head;
	isoch->head = (isoch->head + 1U) % DRV_XHCI_ISOCH_QUEUE_MAX;
	isoch->queued--;

	/* Succeeded: taken. */
	return 1;
}

/* Gives the request at a place of the FIFO, counted from its head. */
static struct drv_xhci_isoch_request *
isoch_request(
	struct drv_xhci_isoch *isoch,
	unsigned position)
{
	unsigned index;

	/* The circle's place. */
	index = (isoch->head + position) % DRV_XHCI_ISOCH_QUEUE_MAX;

	/* Succeeded: the request. */
	return &isoch->requests[index];
}

/* Gives a TD without a result its result; a TD's first result stands. */
static void
isoch_record(
	struct drv_xhci_isoch_request *request,
	unsigned packet,
	enum drv_xhci_iso_result result,
	uint32_t actual)
{
	/* A TD already ended keeps its result. */
	if (request->result[packet] != DRV_XHCI_ISO_PENDING)
		return;

	/* The result, and one more TD done. */
	request->result[packet] = (uint8_t)result;
	request->actual[packet] = actual;
	request->done++;
}

/* The skip rule: every TD before a TD (FIFO order) without a result is MISSED. */
static void
isoch_skip(
	struct drv_xhci_isoch *isoch,
	unsigned position,
	unsigned packet)
{
	struct drv_xhci_isoch_request *request;
	unsigned earlier;
	unsigned index;

	/* The requests before the TD's, whole. */
	for (earlier = 0U; earlier < position; earlier++) {
		request = isoch_request(isoch, earlier);
		for (index = 0U; index < request->count; index++)
			isoch_record(request, index, DRV_XHCI_ISO_MISSED, 0U);
	}

	/* Its own request's TDs before it. */
	request = isoch_request(isoch, position);
	for (index = 0U; index < packet; index++)
		isoch_record(request, index, DRV_XHCI_ISO_MISSED, 0U);
}

/*
 * Moves the consumer over the TDs with a result, in order from it, and
 * tells whether the head request is done (DRV_XHCI_ISOCH_COMPLETE).
 */
static unsigned
isoch_retire(
	struct drv_xhci_isoch *isoch)
{
	struct drv_xhci_isoch_request *request;
	unsigned position;

	/* Each request in order, each of its TDs from the first not retired, while it has a result. */
	for (position = 0U; position < isoch->queued; position++) {
		request = isoch_request(isoch, position);
		while (request->retired < request->count &&
		    request->result[request->retired] != DRV_XHCI_ISO_PENDING) {
			/* Retired: its slot is free. */
			request->retired++;
			isoch->consumer = (isoch->consumer + 1U) % isoch->slots;
			isoch->outstanding--;
		}

		/* A TD still outstanding stops the walk. */
		if (request->retired < request->count)
			break;
	}

	/* The head request done, to be taken. */
	if (isoch->queued == 0U)
		return 0U;
	request = isoch_request(isoch, 0U);
	if (request->done != request->count)
		return 0U;

	/* Succeeded: there is a request to take. */
	return DRV_XHCI_ISOCH_COMPLETE;
}

/* Tells whether the endpoint runs with no owner and no halt (NORMAL or SKIP_WAIT). */
static int
isoch_running(
	const struct drv_xhci_isoch *isoch)
{
	/* The two states without an owner. */
	if (isoch->state == DRV_XHCI_ISOCH_NORMAL)
		return 1;
	if (isoch->state == DRV_XHCI_ISOCH_SKIP_WAIT)
		return 1;

	/* An owner or a halt. */
	return 0;
}
