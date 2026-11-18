/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The OHCI driver's transfer descriptors.
 *
 * A control transfer is a setup descriptor (DATA0), its data cut into
 * descriptors of a page each (the first DATA1, the rest carrying the
 * toggle on), and a status descriptor of no data in the other direction
 * (DATA1).  A bulk or interrupt transfer is its data cut the same way,
 * every descriptor carrying the endpoint's toggle; no data is one empty
 * descriptor, and a zero-length packet after an OUT that fills its last
 * packet is one more.  Only the last descriptor of the data may end
 * short without an error (buffer rounding): a short packet in an earlier
 * one halts the endpoint with a data underrun, which ends the transfer
 * at that packet.  Every descriptor starts "not accessed", and only the
 * last asks for an interrupt when it is retired.
 *
 * The controller retires a descriptor by writing its condition code and
 * its current buffer pointer (zero when every byte moved, else the next
 * byte's address) and moving the endpoint's head past it; a descriptor
 * with an error halts the endpoint.
 *
 * Values referenced: OpenHCI Open Host Controller Interface
 * Specification for USB, Release 1.0a (1999), sections 4.2 (the
 * endpoint descriptor: FA bits 0 to 6, EN 7 to 10, D 11 and 12, S 13, K
 * 14, MPS 16 to 26; the head pointer's halted bit 0 and toggle carry bit
 * 1), 4.3.1 (the general transfer descriptor: R bit 18, DP 19 and 20, DI
 * 21 to 23, T 24 and 25, EC 26 and 27, CC 28 to 31; a buffer that may
 * cross one page boundary) and 4.3.3 (the condition codes).
 */

#include "drivers/pci/pci-ohci-td.h"

/* The control word of a descriptor before its direction, rounding and toggle. */
#define TD_BASE		((OHCI_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) | OHCI_TD_DELAY_NONE)

/* The descriptor's delay field: zero asks for the interrupt at once. */
#define TD_DELAY_MASK	0x00e00000U

static unsigned data_td_count(const struct ohci_transfer_plan *plan);
static void put_td(struct ohci_td_words *td, uint32_t control, uint32_t buffer, size_t length);
static size_t td_moved(const struct ohci_td_words *td, uint32_t start, size_t length);

/*
 * Counts the descriptors a transfer takes, not counting the endpoint's
 * empty tail.
 */
unsigned
ohci_td_count(
	const struct ohci_transfer_plan *plan)
{
	unsigned count;

	/* The data's descriptors, and a control transfer's setup and status. */
	count = data_td_count(plan);
	if (plan->control)
		count += 2U;

	/* Succeeded: the descriptors the transfer takes. */
	return count;
}

/*
 * Fills the descriptors of a transfer and links each to the next; the
 * last links to the endpoint's empty tail, the descriptor after it.
 * Returns 0, or -1 when they do not fit in capacity.
 */
int
ohci_td_build(
	const struct ohci_transfer_plan *plan,
	struct ohci_td_words *tds,
	unsigned capacity,
	unsigned *count)
{
	uint32_t direction;
	uint32_t toggle;
	uint32_t control;
	size_t offset;
	size_t chunk;
	unsigned data_count;
	unsigned index;
	unsigned at;

	/* Refuses a transfer the array cannot hold. */
	*count = ohci_td_count(plan);
	if (*count > capacity || *count > OHCI_TD_MAX)
		return -1;

	/* The data's direction. */
	direction = OHCI_TD_PID_OUT;
	if (plan->input)
		direction = OHCI_TD_PID_IN;

	/* A control transfer's setup packet, DATA0. */
	at = 0;
	if (plan->control) {
		put_td(&tds[at], TD_BASE | OHCI_TD_PID_SETUP | OHCI_TD_TOGGLE_DATA0, plan->setup_address, 8U);
		at++;
	}

	/* The data, a page a descriptor; a control transfer's starts at DATA1. */
	data_count = data_td_count(plan);
	offset = 0;
	toggle = OHCI_TD_TOGGLE_CARRY;
	if (plan->control)
		toggle = OHCI_TD_TOGGLE_DATA1;
	for (index = 0; index < data_count; index++) {
		/* The bytes of this descriptor: up to a page, none for an empty packet. */
		chunk = plan->length - offset;
		if (chunk > OHCI_TD_DATA_MAX)
			chunk = OHCI_TD_DATA_MAX;

		/* Only the last may end short without halting the endpoint; a control transfer's always may. */
		control = TD_BASE | direction | toggle;
		if (plan->input && (plan->control || index + 1U == data_count))
			control |= OHCI_TD_ROUNDING;
		put_td(&tds[at], control, plan->data_address + (uint32_t)offset, chunk);
		at++;

		/* The next descriptor carries the toggle on from this one. */
		offset += chunk;
		toggle = OHCI_TD_TOGGLE_CARRY;
	}

	/* A control transfer's status stage: no data, the other direction, DATA1. */
	if (plan->control) {
		direction = OHCI_TD_PID_IN;
		if (plan->input && plan->length != 0)
			direction = OHCI_TD_PID_OUT;
		put_td(&tds[at], TD_BASE | direction | OHCI_TD_TOGGLE_DATA1, 0, 0);
		at++;
	}

	/* Links each descriptor to the one after it, the last to the tail. */
	for (index = 0; index < at; index++)
		tds[index].next = plan->td_address + (index + 1U) * OHCI_DESCRIPTOR_SIZE;

	/* Only the last asks for an interrupt, at once. */
	tds[at - 1U].control &= ~TD_DELAY_MASK;

	/* Succeeded: the descriptors are filled. */
	return 0;
}

/*
 * Makes an endpoint descriptor's control word: the function's address,
 * the endpoint's number, the direction (from each descriptor for a
 * control endpoint), the speed and the largest packet.
 */
uint32_t
ohci_ed_control(
	unsigned address,
	unsigned endpoint,
	int control,
	int input,
	int low_speed,
	unsigned packet)
{
	uint32_t word;

	/* The function and endpoint numbers, and the largest packet. */
	word = (uint32_t)(address & 0x7fU);
	word |= (uint32_t)(endpoint & 0xfU) << OHCI_ED_ENDPOINT_SHIFT;
	word |= (uint32_t)(packet & 0x7ffU) << OHCI_ED_PACKET_SHIFT;

	/* The direction: each descriptor's own for a control endpoint. */
	if (control) {
		word |= OHCI_ED_DIRECTION_TD;
	} else if (input) {
		word |= OHCI_ED_DIRECTION_IN;
	} else {
		word |= OHCI_ED_DIRECTION_OUT;
	}

	/* A low-speed function. */
	if (low_speed)
		word |= OHCI_ED_LOW_SPEED;

	/* Succeeded: the control word. */
	return word;
}

/*
 * Tells how a transfer ended from its descriptors as the controller left
 * them and the endpoint's head pointer.  *actual receives the data bytes
 * moved and *condition the condition code that ended it (zero when none
 * did).  A transfer whose endpoint is neither halted nor past its last
 * descriptor is still pending.
 */
enum ohci_transfer_result
ohci_td_outcome(
	const struct ohci_transfer_plan *plan,
	const struct ohci_td_words *tds,
	unsigned count,
	uint32_t head,
	size_t *actual,
	unsigned *condition)
{
	uint32_t tail;
	uint32_t code;
	size_t offset;
	size_t chunk;
	unsigned first_data;
	unsigned data_count;
	unsigned index;

	/* Pending while the endpoint runs and has descriptors left. */
	*actual = 0;
	*condition = 0;
	tail = plan->td_address + count * OHCI_DESCRIPTOR_SIZE;
	if ((head & OHCI_ED_HEAD_HALTED) == 0 && (head & OHCI_ED_POINTER_MASK) != tail)
		return OHCI_TRANSFER_PENDING;

	/* The data moved by each data descriptor the controller reached. */
	first_data = 0;
	if (plan->control)
		first_data = 1;
	data_count = data_td_count(plan);
	offset = 0;
	for (index = 0; index < data_count; index++) {
		chunk = plan->length - offset;
		if (chunk > OHCI_TD_DATA_MAX)
			chunk = OHCI_TD_DATA_MAX;
		code = tds[first_data + index].control >> OHCI_TD_CC_SHIFT;
		if (code == OHCI_CC_NOT_ACCESSED || code == OHCI_CC_NOT_ACCESSED_ALSO)
			break;
		*actual += td_moved(&tds[first_data + index], plan->data_address + (uint32_t)offset, chunk);
		offset += chunk;
	}

	/* The first descriptor that ended with an error decides the outcome. */
	for (index = 0; index < count; index++) {
		code = tds[index].control >> OHCI_TD_CC_SHIFT;
		if (code == OHCI_CC_NO_ERROR)
			continue;
		if (code == OHCI_CC_NOT_ACCESSED || code == OHCI_CC_NOT_ACCESSED_ALSO)
			continue;
		*condition = code;

		/* A short packet ends a transfer whose data it was in; anywhere else it is an error. */
		if (code == OHCI_CC_DATA_UNDERRUN &&
		    index >= first_data &&
		    index < first_data + data_count)
			return OHCI_TRANSFER_COMPLETE;
		if (code == OHCI_CC_STALL)
			return OHCI_TRANSFER_STALL;
		return OHCI_TRANSFER_ERROR;
	}

	/* A halt with no descriptor to blame is an error. */
	if ((head & OHCI_ED_HEAD_HALTED) != 0)
		return OHCI_TRANSFER_ERROR;

	/* Succeeded: every descriptor was retired without an error. */
	return OHCI_TRANSFER_COMPLETE;
}

/* Counts the data descriptors: a page each, an empty one for no data, one for a closing empty packet. */
static unsigned
data_td_count(
	const struct ohci_transfer_plan *plan)
{
	unsigned count;

	/* The pages the data covers. */
	count = (unsigned)((plan->length + OHCI_TD_DATA_MAX - 1U) / OHCI_TD_DATA_MAX);

	/* A control transfer without data has no data stage. */
	if (plan->control)
		return count;

	/* Bulk and interrupt: an empty packet for no data, or after a full last one when asked. */
	if (count == 0 || plan->zero_packet)
		count++;

	/* Succeeded: the data descriptors. */
	return count;
}

/* Fills one descriptor's control word and buffer; no data is a zero buffer. */
static void
put_td(
	struct ohci_td_words *td,
	uint32_t control,
	uint32_t buffer,
	size_t length)
{
	/* The control word. */
	td->control = control;
	td->next = 0;

	/* An empty buffer is all zero; otherwise its first and last byte. */
	if (length == 0) {
		td->buffer = 0;
		td->end = 0;
		return;
	}
	td->buffer = buffer;
	td->end = buffer + (uint32_t)length - 1U;
}

/* Reports how many bytes a retired descriptor moved. */
static size_t
td_moved(
	const struct ohci_td_words *td,
	uint32_t start,
	size_t length)
{
	/* A zero pointer means every byte moved. */
	if (td->buffer == 0)
		return length;

	/* A pointer outside the buffer moved nothing the driver can trust. */
	if (td->buffer < start || td->buffer > start + length)
		return 0;

	/* Succeeded: the bytes before the pointer. */
	return td->buffer - start;
}
