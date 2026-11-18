/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI xHCI Normal TD layout and the events that end a Normal request
 */

#ifndef KERN_DRIVERS_PCI_XHCI_NORMAL_H
#define KERN_DRIVERS_PCI_XHCI_NORMAL_H

#include <drivers/pci/pci-xhci-lifecycle.h>

#include <stddef.h>
#include <stdint.h>

/* The Normal TRB's fields these builders set; ring_push() supplies the Cycle bit. */
#define DRV_XHCI_NORMAL_TRB_TYPE	(1U << 10)
#define DRV_XHCI_NORMAL_TRB_ISP		0x00000004U
#define DRV_XHCI_NORMAL_TRB_CHAIN	0x00000010U
#define DRV_XHCI_NORMAL_TRB_IOC		0x00000020U
#define DRV_XHCI_NORMAL_TD_SIZE_SHIFT	17U

/* The Transfer Event completion codes a Normal request is classified by. */
#define DRV_XHCI_NORMAL_COMPLETION_SUCCESS	1U
#define DRV_XHCI_NORMAL_COMPLETION_SHORT_PACKET	13U

/*
 * The status and control words of one Normal TRB.
 *
 * One instance is built for each TRB pushed and lives only until the push;
 * the parameter word (the buffer's address) is the caller's.
 */
struct drv_xhci_normal_words {
	uint32_t status;
	uint32_t control;
};

/*
 * What a Transfer Event means to a Normal request.
 *
 * A request whose payload fills its last packet exactly and that asked for
 * a terminating zero-length packet is two TDs on the ring: the payload, then
 * a TD of one zero-length TRB.  The payload TD's own event leaves the request
 * waiting for the second one.
 */
enum drv_xhci_normal_event {
	/* The event ends the request. */
	DRV_XHCI_NORMAL_EVENT_FINISH = 0,
	/* The payload TD ended; the zero-length TD's event ends the request. */
	DRV_XHCI_NORMAL_EVENT_WAIT_ZERO
};

/*
 * Reports whether a transfer ends with a zero-length packet TD.
 *
 * Only a bulk OUT transfer whose payload is a whole number of packets ends
 * with one, and only when its class driver asked: without it the device
 * cannot tell that the payload ended.  The zero-length packet has to be a TD
 * of its own: the controller moves a TD's data as one stream of packets, so
 * a zero-length TRB chained behind the payload adds no packet to it.  Linux
 * does the same (xhci_urb_enqueue gives such a URB a second TD, and
 * xhci_queue_bulk_tx makes it one Normal TRB of length 0 with IOC).
 */
static inline int
drv_xhci_normal_zero_packet(int bulk, int input, size_t length,
	int requested, size_t maximum_packet_size)
{
	if (!bulk || input || !requested)
		return 0;
	if (length == 0 || maximum_packet_size == 0)
		return 0;
	if (length % maximum_packet_size != 0)
		return 0;

	/* Succeeded: the payload needs a zero-length packet after it. */
	return 1;
}

/*
 * Builds the words of one payload TRB of a Normal TD.
 *
 * The last TRB of the payload ends its TD (IOC, no Chain) and reports no
 * packets after it in TD Size, even when a zero-length TD follows: that TD
 * is counted by itself.  An IN TRB asks for an event on a short packet.
 */
static inline void
drv_xhci_normal_payload_words(size_t chunk, size_t total_length,
	size_t cumulative_length, size_t maximum_packet_size, int input,
	int last_trb, struct drv_xhci_normal_words *words)
{
	uint32_t control;
	unsigned td_size;

	/* The TRB type, and the short-packet interrupt for an IN transfer. */
	control = DRV_XHCI_NORMAL_TRB_TYPE;
	if (input)
		control |= DRV_XHCI_NORMAL_TRB_ISP;

	/* The last TRB ends the TD and raises its event; the others chain on. */
	if (last_trb) {
		control |= DRV_XHCI_NORMAL_TRB_IOC;
	} else {
		control |= DRV_XHCI_NORMAL_TRB_CHAIN;
	}

	/* The packets of this TD still to come after this TRB. */
	td_size = drv_xhci_normal_td_size(total_length, cumulative_length,
	    maximum_packet_size, last_trb);

	/* Publishes the words. */
	words->status = (uint32_t)chunk | (td_size << DRV_XHCI_NORMAL_TD_SIZE_SHIFT);
	words->control = control;
}

/*
 * Builds the words of the zero-length packet's TD.
 *
 * One TRB, Transfer Length 0, TD Size 0, its own event.
 */
static inline void
drv_xhci_normal_zero_words(struct drv_xhci_normal_words *words)
{
	words->status = 0;
	words->control = DRV_XHCI_NORMAL_TRB_TYPE | DRV_XHCI_NORMAL_TRB_IOC;
}

/*
 * Classifies a Transfer Event that names a Normal request's TRB.
 *
 * zero_packet says the request ends with a zero-length TD, which is its last
 * TRB.  An event for an earlier TRB comes from the payload TD.  When the
 * payload ended without halting the endpoint (Success, or Short Packet,
 * after which the controller goes on to the next TD) the zero-length TD
 * still runs and the request waits for its event.  Any other code has
 * halted the endpoint, the zero-length TD never runs, and the event ends
 * the request.
 */
static inline enum drv_xhci_normal_event
drv_xhci_normal_event_kind(int zero_packet, unsigned trb_offset,
	unsigned trb_count, unsigned code)
{
	if (!zero_packet)
		return DRV_XHCI_NORMAL_EVENT_FINISH;
	if (trb_offset + 1U >= trb_count)
		return DRV_XHCI_NORMAL_EVENT_FINISH;
	if (code == DRV_XHCI_NORMAL_COMPLETION_SUCCESS)
		return DRV_XHCI_NORMAL_EVENT_WAIT_ZERO;
	if (code == DRV_XHCI_NORMAL_COMPLETION_SHORT_PACKET)
		return DRV_XHCI_NORMAL_EVENT_WAIT_ZERO;

	/* An error that halts the endpoint ends the request at its payload. */
	return DRV_XHCI_NORMAL_EVENT_FINISH;
}

/*
 * Reports whether the payload TD's event decides how a request ends.
 *
 * When a request with a zero-length TD has had its payload TD's event, a
 * payload that did not succeed decides the outcome, whatever the zero-length
 * TD reported after it; otherwise the zero-length TD's own event does.
 */
static inline int
drv_xhci_normal_payload_decides(int payload_seen, unsigned payload_code)
{
	if (!payload_seen)
		return 0;
	if (payload_code == DRV_XHCI_NORMAL_COMPLETION_SUCCESS)
		return 0;

	/* Succeeded: the payload's report ends the request. */
	return 1;
}

#endif
