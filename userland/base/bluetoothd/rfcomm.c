/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's RFCOMM (ws197-p002, see rfcomm.h).
 *
 * The frame and the multiplexer commands follow 3GPP TS 27.010 (GSM 07.10)
 * as RFCOMM 1.2 narrows it; the sections named below are those two
 * documents'.  Every length is checked against the L2CAP payload the frame
 * came in.
 */

#include "userland/base/bluetoothd/rfcomm.h"

#include <errno.h>
#include <string.h>

/* The address octet: the extension bit and the command/response bit (TS 27.010 section 5.2.1.2). */
#define RFCOMM_EA			0x01U
#define RFCOMM_CR			0x02U

/* The frame types of the control octet, and its poll/final bit (TS 27.010 section 5.2.1.3). */
#define RFCOMM_SABM			0x2fU
#define RFCOMM_UA			0x63U
#define RFCOMM_DM			0x0fU
#define RFCOMM_DISC			0x43U
#define RFCOMM_UIH			0xefU
#define RFCOMM_PF			0x10U

/* The multiplexer messages' types without their EA and C/R bits (TS 27.010 section 5.4.6.3). */
#define RFCOMM_MUX_TYPE_MASK		0xfcU
#define RFCOMM_MUX_PN			0x80U
#define RFCOMM_MUX_PSC			0x40U
#define RFCOMM_MUX_CLD			0xc0U
#define RFCOMM_MUX_TEST			0x20U
#define RFCOMM_MUX_FCON			0xa0U
#define RFCOMM_MUX_FCOFF		0x60U
#define RFCOMM_MUX_MSC			0xe0U
#define RFCOMM_MUX_NSC			0x10U
#define RFCOMM_MUX_RPN			0x90U
#define RFCOMM_MUX_RLS			0x50U

/* The length of a PN's value, and the CL values of credit based flow control (RFCOMM 1.2 table 5.3). */
#define RFCOMM_PN_LENGTH		8U
#define RFCOMM_PN_CL_REQUEST		0x0fU
#define RFCOMM_PN_CL_RESPONSE		0x0eU

/* The V.24 signals bluetoothd sends: EA, ready to communicate, ready to receive, data valid (TS 27.010 figure 10). */
#define RFCOMM_V24_SIGNALS		0x8dU

/* The port values answered to an RPN that only asks: 9600 bit/s, 8 data bits, no flow control, XON and XOFF. */
#define RFCOMM_RPN_LENGTH		8U
#define RFCOMM_RPN_BAUD_9600		0x03U
#define RFCOMM_RPN_DATA_8N1		0x03U
#define RFCOMM_RPN_XON			0x11U
#define RFCOMM_RPN_XOFF			0x13U

/* The header bytes taken out of the L2CAP MTU for a frame's size: address, control, two of length, FCS (RFCOMM 1.2 section 6.5.2). */
#define RFCOMM_FRAME_OVERHEAD		5U

/* What the receiving check of the FCS ends at (TS 27.010 annex B.3.4). */
#define RFCOMM_FCS_GOOD			0xcfU

/* The most credits one frame gives. */
#define RFCOMM_CREDITS_MAX		255U

/*
 * One frame taken apart: its DLCI, its type without the P/F bit, the
 * credit octet when it had one, and its information field (pointing into
 * the payload).
 */
struct rfcomm_frame {
	unsigned dlci;
	uint8_t type;
	int with_credit;
	unsigned credit;
	const uint8_t *data;
	size_t length;
};

static uint8_t rfcomm_crc(uint8_t crc, uint8_t byte);
static int rfcomm_parse(struct btd_rfcomm *rf, const uint8_t *payload, size_t length, struct rfcomm_frame *frame);
static void rfcomm_dispatch(struct btd_rfcomm *rf, const struct rfcomm_frame *frame, uint64_t now);
static unsigned rfcomm_direction(const struct btd_rfcomm *rf);
static unsigned rfcomm_n1_max(const struct btd_rfcomm *rf);
static size_t rfcomm_build(struct btd_rfcomm *rf, uint8_t *out, size_t size, unsigned dlci, int command, uint8_t control, int with_credit, unsigned credit, const uint8_t *data, size_t length);
static int rfcomm_send_control(struct btd_rfcomm *rf, unsigned dlci, int command, uint8_t control);
static int rfcomm_send_mux(struct btd_rfcomm *rf, uint8_t type, int command, const uint8_t *value, size_t length);
static int rfcomm_queue(struct btd_rfcomm *rf, const uint8_t *bytes, size_t length);
static int rfcomm_flush(struct btd_rfcomm *rf);
static int rfcomm_send_pn(struct btd_rfcomm *rf, const struct btd_rfcomm_dlc *dlc, int command, uint8_t cl, unsigned credits);
static int rfcomm_send_msc(struct btd_rfcomm *rf, unsigned dlci);
static void rfcomm_give_credits(struct btd_rfcomm *rf, struct btd_rfcomm_dlc *dlc);
static unsigned rfcomm_credit_share(const struct btd_rfcomm *rf, const struct btd_rfcomm_dlc *dlc);
static struct btd_rfcomm_dlc *rfcomm_find(struct btd_rfcomm *rf, unsigned dlci);
static struct btd_rfcomm_dlc *rfcomm_slot(struct btd_rfcomm *rf, unsigned dlci);
static int rfcomm_announced(const struct btd_rfcomm_dlc *dlc);
static unsigned rfcomm_active(const struct btd_rfcomm *rf);
static void rfcomm_free(struct btd_rfcomm *rf, struct btd_rfcomm_dlc *dlc, int reason, uint64_t now);
static void rfcomm_end(struct btd_rfcomm *rf, int reason, int tell);
static void rfcomm_settle(struct btd_rfcomm *rf);
static int rfcomm_server_offered(struct btd_rfcomm *rf, unsigned dlci);
static void rfcomm_session_opened(struct btd_rfcomm *rf, uint64_t now);
static void rfcomm_frame_session(struct btd_rfcomm *rf, uint8_t type, uint64_t now);
static void rfcomm_frame_sabm(struct btd_rfcomm *rf, unsigned dlci, uint64_t now);
static void rfcomm_frame_ua(struct btd_rfcomm *rf, unsigned dlci, uint64_t now);
static void rfcomm_frame_dm(struct btd_rfcomm *rf, unsigned dlci, uint64_t now);
static void rfcomm_frame_disc(struct btd_rfcomm *rf, unsigned dlci, uint64_t now);
static void rfcomm_frame_uih(struct btd_rfcomm *rf, unsigned dlci, int with_credit, unsigned credit, const uint8_t *data, size_t length, uint64_t now);
static void rfcomm_mux(struct btd_rfcomm *rf, const uint8_t *data, size_t length, uint64_t now);
static void rfcomm_pn_command(struct btd_rfcomm *rf, const uint8_t *value, uint64_t now);
static void rfcomm_pn_response(struct btd_rfcomm *rf, const uint8_t *value, uint64_t now);
static void rfcomm_msc(struct btd_rfcomm *rf, const uint8_t *value, size_t length, int command);
static void rfcomm_rpn(struct btd_rfcomm *rf, const uint8_t *value, size_t length);
static void rfcomm_connected(struct btd_rfcomm *rf, struct btd_rfcomm_dlc *dlc);
static void rfcomm_open_dlc(struct btd_rfcomm *rf, struct btd_rfcomm_dlc *dlc, uint64_t now);

/*
 * Computes the FCS of a frame's covered bytes (TS 27.010 section 5.2.1.6:
 * the reflected CRC-8 of x^8 + x^2 + x + 1 started at 0xff, complemented).
 */
uint8_t
btd_rfcomm_fcs(
	const uint8_t *bytes,
	size_t length)
{
	uint8_t crc;
	size_t index;

	/* Each covered byte through the CRC, from 0xff. */
	crc = 0xffU;
	for (index = 0U; index < length; index++)
		crc = rfcomm_crc(crc, bytes[index]);

	/* Succeeded: the ones complement is what is sent. */
	return (uint8_t)(0xffU - crc);
}

/*
 * Empties a session for an L2CAP channel just opened (its MTU is the
 * smaller of the two sides'); bluetoothd answers as the responder until
 * btd_rfcomm_start makes it the initiator.
 */
void
btd_rfcomm_init(
	struct btd_rfcomm *rf,
	const struct btd_rfcomm_events *events,
	unsigned mtu)
{
	/* No session, no DLC, no server channel offered. */
	memset(rf, 0, sizeof(*rf));
	rf->events = *events;
	rf->mtu = mtu;
	rf->state = BTD_RFCOMM_SESSION_CLOSED;
}

/*
 * Offers one of bluetoothd's server channels on the session (the owner's
 * accept hook still decides each request).  Returns 0, EINVAL for a channel
 * outside 1 to 30, or ENOSPC.
 */
int
btd_rfcomm_listen(
	struct btd_rfcomm *rf,
	unsigned server_channel)
{
	unsigned index;

	/* A channel a DLCI can name. */
	if (server_channel < BTD_RFCOMM_CHANNEL_FIRST || server_channel > BTD_RFCOMM_CHANNEL_LAST)
		return EINVAL;

	/* Offered already. */
	for (index = 0U; index < rf->server_count; index++) {
		if (rf->servers[index] == server_channel)
			return 0;
	}

	/* Room for one more. */
	if (rf->server_count >= BTD_RFCOMM_SERVERS_MAX)
		return ENOSPC;

	/* Succeeded: offered. */
	rf->servers[rf->server_count] = server_channel;
	rf->server_count++;
	return 0;
}

/*
 * Starts the multiplexer as the initiator (RFCOMM 1.2 section 5.2.1): SABM
 * on DLCI 0, answered by UA.  Returns 0, EALREADY for a session that is not
 * closed, or the send hook's error.
 */
int
btd_rfcomm_start(
	struct btd_rfcomm *rf,
	uint64_t now)
{
	int error;

	/* Only a session that has not begun. */
	if (rf->state != BTD_RFCOMM_SESSION_CLOSED)
		return EALREADY;

	/* This side sent the first SABM: its direction bit is 1 and its commands carry C/R 1. */
	rf->initiator = 1;

	/* SABM on DLCI 0, its answer within T1. */
	error = rfcomm_send_control(rf, 0U, 1, (uint8_t)(RFCOMM_SABM | RFCOMM_PF));
	if (error != 0)
		return error;

	/* Succeeded: opening. */
	rf->state = BTD_RFCOMM_SESSION_OPENING;
	rf->deadline = now + BTD_RFCOMM_T1_MS;
	return 0;
}

/*
 * Opens a DLC to one of the peer's server channels: PN, then SABM, then the
 * modem status; opened tells when data may flow.  The DLCI is the channel
 * with the inverse of this side's direction bit (RFCOMM 1.2 section 5.4).
 * Returns 0 with the DLCI, EINVAL for a channel outside 1 to 30, ENOTCONN
 * for a session that is not open or opening, EEXIST, ENOSPC, or the send
 * hook's error.
 */
int
btd_rfcomm_connect(
	struct btd_rfcomm *rf,
	unsigned server_channel,
	uint64_t now,
	unsigned *dlci)
{
	struct btd_rfcomm_dlc *dlc;
	unsigned wanted;
	unsigned credits;
	int error;

	/* A channel a DLCI can name. */
	if (server_channel < BTD_RFCOMM_CHANNEL_FIRST || server_channel > BTD_RFCOMM_CHANNEL_LAST)
		return EINVAL;

	/* A session that is open, or being opened by this side. */
	if (rf->state != BTD_RFCOMM_SESSION_OPEN && rf->state != BTD_RFCOMM_SESSION_OPENING)
		return ENOTCONN;

	/* The DLCI of the peer's server channel, not in use. */
	wanted = (server_channel << 1) | (rfcomm_direction(rf) ^ 1U);
	dlc = rfcomm_find(rf, wanted);
	if (dlc != NULL)
		return EEXIST;
	dlc = rfcomm_slot(rf, wanted);
	if (dlc == NULL)
		return ENOSPC;

	/* Ours, with the largest frames this link allows until the PN settles them. */
	dlc->ours = 1;
	dlc->n1 = rfcomm_n1_max(rf);
	*dlci = wanted;

	/* A session still opening: the PN goes when its UA comes. */
	if (rf->state == BTD_RFCOMM_SESSION_OPENING) {
		dlc->state = BTD_RFCOMM_DLC_WAITING;
		return 0;
	}

	/* The PN, offering credit based flow control and the first credits. */
	credits = rfcomm_credit_share(rf, dlc);
	if (credits > BTD_RFCOMM_PN_CREDITS_MAX)
		credits = BTD_RFCOMM_PN_CREDITS_MAX;
	error = rfcomm_send_pn(rf, dlc, 1, RFCOMM_PN_CL_REQUEST, credits);
	if (error != 0) {
		dlc->state = BTD_RFCOMM_DLC_FREE;
		return error;
	}

	/* Succeeded: negotiating, the answer within T2. */
	dlc->rx_credits = credits;
	dlc->state = BTD_RFCOMM_DLC_NEGOTIATING;
	dlc->deadline = now + BTD_RFCOMM_T2_MS;
	return 0;
}

/*
 * Writes data on a connected DLC, cut into frames of its size as far as the
 * peer's credits and the send hook take; what did not go is the caller's
 * to write again when writable says so.  Returns 0 with the bytes written,
 * ENOTCONN for a DLC that is not connected, or the send hook's error.
 */
int
btd_rfcomm_write(
	struct btd_rfcomm *rf,
	unsigned dlci,
	const uint8_t *data,
	size_t length,
	size_t *written)
{
	struct btd_rfcomm_dlc *dlc;
	size_t chunk;
	size_t frame;
	int error;

	/* Nothing went yet. */
	*written = 0U;

	/* A DLC whose modem status was exchanged. */
	dlc = rfcomm_find(rf, dlci);
	if (dlc == NULL || dlc->state != BTD_RFCOMM_DLC_CONNECTED)
		return ENOTCONN;

	/* Control frames that waited go first: data never passes them (an MSC before the data, RFCOMM 1.2 section 6.3). */
	error = rfcomm_flush(rf);
	if (error != 0 && error != ENOBUFS)
		return error;
	if (rf->pending_count != 0U)
		return 0;

	/* Frames while there is data, a credit and room. */
	while (*written < length && dlc->tx_credits != 0U) {
		/* The next piece, at most the DLC's frame size. */
		chunk = length - *written;
		if (chunk > dlc->n1)
			chunk = dlc->n1;

		/* The UIH frame, written. */
		frame = rfcomm_build(rf, rf->frame, sizeof(rf->frame), dlci, 1, RFCOMM_UIH, 0, 0U, data + *written, chunk);
		if (frame == 0U)
			return EMSGSIZE;
		error = rf->events.send(rf->events.context, rf->frame, frame);
		if (error == ENOBUFS)
			break;
		if (error != 0)
			return error;

		/* One credit spent for the frame. */
		dlc->tx_credits--;
		*written += chunk;
	}

	/* Succeeded: as much as went now. */
	return 0;
}

/*
 * Closes a DLC: DISC, answered by UA (closed tells the end).  A DLC not
 * opened yet is forgotten at once.  Returns 0, ENOENT for an unknown DLCI,
 * or the send hook's error.
 */
int
btd_rfcomm_close(
	struct btd_rfcomm *rf,
	unsigned dlci,
	uint64_t now)
{
	struct btd_rfcomm_dlc *dlc;
	int error;

	/* The DLC. */
	dlc = rfcomm_find(rf, dlci);
	if (dlc == NULL)
		return ENOENT;

	/* Closing already. */
	if (dlc->state == BTD_RFCOMM_DLC_CLOSING)
		return 0;

	/* Not asked of the peer yet, or only negotiated: forgotten without a frame (the peer's PN state resets with its next PN). */
	if (dlc->state == BTD_RFCOMM_DLC_WAITING ||
	    dlc->state == BTD_RFCOMM_DLC_NEGOTIATING ||
	    dlc->state == BTD_RFCOMM_DLC_NEGOTIATED) {
		rfcomm_free(rf, dlc, 0, now);
		return 0;
	}

	/* DISC, its answer within T1. */
	error = rfcomm_send_control(rf, dlci, 1, (uint8_t)(RFCOMM_DISC | RFCOMM_PF));
	if (error != 0)
		return error;

	/* Succeeded: closing. */
	dlc->state = BTD_RFCOMM_DLC_CLOSING;
	dlc->deadline = now + BTD_RFCOMM_T1_MS;
	return 0;
}

/*
 * Takes one L2CAP payload of the session's channel: one RFCOMM frame
 * (RFCOMM 1.2 section 5.1).  A malformed frame is counted and passed over;
 * BTD_RFCOMM_MALFORMED_MAX of them in a row end the session.
 */
void
btd_rfcomm_input(
	struct btd_rfcomm *rf,
	const uint8_t *payload,
	size_t length,
	uint64_t now)
{
	struct rfcomm_frame frame;
	int error;

	/* The frame taken apart and checked. */
	error = rfcomm_parse(rf, payload, length, &frame);
	if (error != 0) {
		rf->malformed++;
		rf->malformed_run++;
	} else {
		rf->malformed_run = 0U;
		rfcomm_dispatch(rf, &frame, now);
	}

	/* Too many malformed frames in a row: the peer and this side disagree on the frames. */
	if (rf->malformed_run >= BTD_RFCOMM_MALFORMED_MAX && rf->failed == 0)
		rf->failed = BTD_RFCOMM_CLOSED_ERROR;

	/* A session that must end ends now that nothing of it is in use. */
	rfcomm_settle(rf);
}

/*
 * Writes what waited for room (control frames, credits given back) and
 * tells each connected DLC with credits that it may write; the owner calls
 * it when its send queue has room again and after each round of input.
 */
void
btd_rfcomm_pump(
	struct btd_rfcomm *rf)
{
	struct btd_rfcomm_dlc *dlc;
	unsigned dlci;
	unsigned index;
	int error;

	/* The control frames, in their order; with them still waiting nothing else goes. */
	error = rfcomm_flush(rf);
	if (error != 0 && error != ENOBUFS && rf->failed == 0)
		rf->failed = BTD_RFCOMM_CLOSED_ERROR;
	if (rf->pending_count != 0U) {
		rfcomm_settle(rf);
		return;
	}

	/* Each connected DLC: its credits given back, then its writer told. */
	for (index = 0U; index < BTD_RFCOMM_DLCS_MAX; index++) {
		dlc = &rf->dlcs[index];
		if (dlc->state != BTD_RFCOMM_DLC_CONNECTED)
			continue;

		/* The credits the peer used, given back. */
		rfcomm_give_credits(rf, dlc);

		/* The writer, when there is a credit to write with (the DLC may close inside the call). */
		dlci = dlc->dlci;
		if (dlc->tx_credits != 0U && rf->events.writable != NULL)
			rf->events.writable(rf->events.context, dlci);
	}

	/* A session that must end ends now. */
	rfcomm_settle(rf);
}

/*
 * Carries out the deadlines that passed: a timer of the session or of a
 * DLC that ran out ends the session (RFCOMM 1.2 section 5.3), a PN the peer
 * never followed with SABM is forgotten, and a session idle since its last
 * DLC closed is closed.
 */
void
btd_rfcomm_tick(
	struct btd_rfcomm *rf,
	uint64_t now)
{
	struct btd_rfcomm_dlc *dlc;
	unsigned index;
	int error;

	/* The session's own SABM or DISC on DLCI 0 unanswered. */
	if (rf->deadline != 0U && now >= rf->deadline) {
		rf->deadline = 0U;
		if (rf->failed == 0)
			rf->failed = BTD_RFCOMM_CLOSED_TIMEOUT;
		rfcomm_settle(rf);
		return;
	}

	/* Each DLC's deadline. */
	for (index = 0U; index < BTD_RFCOMM_DLCS_MAX; index++) {
		dlc = &rf->dlcs[index];
		if (dlc->state == BTD_RFCOMM_DLC_FREE ||
		    dlc->deadline == 0U ||
		    now < dlc->deadline)
			continue;

		/* A peer's PN never followed by its SABM is forgotten quietly. */
		if (dlc->state == BTD_RFCOMM_DLC_NEGOTIATED) {
			rfcomm_free(rf, dlc, 0, now);
			continue;
		}

		/* Any other answer that did not come ends the session. */
		dlc->deadline = 0U;
		if (rf->failed == 0)
			rf->failed = BTD_RFCOMM_CLOSED_TIMEOUT;
	}

	/* An open session idle since its last DLC closed: DISC on DLCI 0 (RFCOMM 1.2 section 5.2.2). */
	if (rf->failed == 0 &&
	    rf->state == BTD_RFCOMM_SESSION_OPEN &&
	    rf->idle_since != 0U &&
	    now >= rf->idle_since + BTD_RFCOMM_IDLE_MS) {
		rf->idle_since = 0U;
		error = rfcomm_send_control(rf, 0U, 1, (uint8_t)(RFCOMM_DISC | RFCOMM_PF));
		if (error == 0) {
			rf->state = BTD_RFCOMM_SESSION_CLOSING;
			rf->deadline = now + BTD_RFCOMM_T1_MS;
		} else {
			rf->failed = BTD_RFCOMM_CLOSED_ERROR;
		}
	}

	/* A session that must end ends now. */
	rfcomm_settle(rf);
}

/*
 * Tells the earliest deadline the session waits for (0: none), for the
 * daemon's poll.
 */
uint64_t
btd_rfcomm_deadline(
	const struct btd_rfcomm *rf)
{
	const struct btd_rfcomm_dlc *dlc;
	uint64_t earliest;
	uint64_t idle;
	unsigned index;

	/* The session's own timer. */
	earliest = rf->deadline;

	/* Each DLC's. */
	for (index = 0U; index < BTD_RFCOMM_DLCS_MAX; index++) {
		dlc = &rf->dlcs[index];
		if (dlc->state == BTD_RFCOMM_DLC_FREE || dlc->deadline == 0U)
			continue;

		/* Earlier than any so far. */
		if (earliest == 0U || dlc->deadline < earliest)
			earliest = dlc->deadline;
	}

	/* The idle session's closing. */
	if (rf->state == BTD_RFCOMM_SESSION_OPEN && rf->idle_since != 0U) {
		idle = rf->idle_since + BTD_RFCOMM_IDLE_MS;
		if (earliest == 0U || idle < earliest)
			earliest = idle;
	}

	/* Succeeded: the earliest, or 0. */
	return earliest;
}

/*
 * Forgets the session after its L2CAP channel or link went (RFCOMM 1.2
 * section 5.2.3): each DLC the owner knows is told it was lost; the owner
 * is not told the session ended (it told this).
 */
void
btd_rfcomm_lost(
	struct btd_rfcomm *rf)
{
	/* Every DLC lost, the session closed. */
	rfcomm_end(rf, BTD_RFCOMM_CLOSED_LOST, 0);
}

/*
 * Finds a DLC by its DLCI, for the owner and the tests (NULL: none).
 */
const struct btd_rfcomm_dlc *
btd_rfcomm_dlc(
	const struct btd_rfcomm *rf,
	unsigned dlci)
{
	unsigned index;

	/* Each DLC in use. */
	for (index = 0U; index < BTD_RFCOMM_DLCS_MAX; index++) {
		if (rf->dlcs[index].state != BTD_RFCOMM_DLC_FREE && rf->dlcs[index].dlci == dlci)
			return &rf->dlcs[index];
	}

	/* None. */
	return NULL;
}

/* Adds one byte to a reflected CRC-8 of x^8 + x^2 + x + 1 (the table of TS 27.010 annex B.3, computed bit by bit). */
static uint8_t
rfcomm_crc(
	uint8_t crc,
	uint8_t byte)
{
	unsigned bit;
	unsigned value;

	/* The byte, least significant bit first, through the reflected polynomial 0xe0. */
	value = (unsigned)(crc ^ byte);
	for (bit = 0U; bit < 8U; bit++) {
		if ((value & 1U) != 0U) {
			value = (value >> 1) ^ 0xe0U;
		} else {
			value = value >> 1;
		}
	}

	/* Succeeded: the CRC so far. */
	return (uint8_t)value;
}

/*
 * Takes a frame apart and checks it: the address's extension bit, the
 * length in one or two octets, the credit octet, the frame filling the
 * payload, the FCS, and the information within the frame size.  Returns 0
 * or EINVAL.
 */
static int
rfcomm_parse(
	struct btd_rfcomm *rf,
	const uint8_t *payload,
	size_t length,
	struct rfcomm_frame *frame)
{
	struct btd_rfcomm_dlc *dlc;
	uint8_t control;
	uint8_t fcs;
	size_t header;
	size_t covered;
	unsigned limit;

	/* The address, the control, a length and the FCS at least, and the address in one octet. */
	if (length < 4U)
		return EINVAL;
	if ((payload[0] & RFCOMM_EA) == 0U)
		return EINVAL;
	memset(frame, 0, sizeof(*frame));
	frame->dlci = (unsigned)(payload[0] >> 2);
	control = payload[1];
	frame->type = (uint8_t)(control & ~RFCOMM_PF);

	/* The length: one octet when its extension bit is set, else two (TS 27.010 section 5.2.1.5). */
	if ((payload[2] & RFCOMM_EA) != 0U) {
		frame->length = (size_t)(payload[2] >> 1);
		header = 3U;
	} else {
		if (length < 5U)
			return EINVAL;
		frame->length = (size_t)(payload[2] >> 1) | ((size_t)payload[3] << 7);
		header = 4U;
	}

	/* The FCS's coverage starts as the header without a credit octet. */
	covered = header;

	/* A credit octet in a UIH frame with P/F set on a DLC of a session with credits (RFCOMM 1.2 section 6.5.2). */
	if (frame->type == RFCOMM_UIH &&
	    (control & RFCOMM_PF) != 0U &&
	    rf->credit_flow &&
	    frame->dlci != 0U) {
		if (length < header + 2U)
			return EINVAL;
		frame->with_credit = 1;
		frame->credit = payload[header];
		header++;
	}

	/* The frame fills the payload exactly: header, information, FCS. */
	if (header + frame->length + 1U != length)
		return EINVAL;
	frame->data = payload + header;

	/* The FCS covers the address and control of a UIH frame, and the length too of the others (RFCOMM 1.2 section 5.1.1). */
	if (frame->type == RFCOMM_UIH)
		covered = 2U;
	fcs = btd_rfcomm_fcs(payload, covered);
	if (fcs != payload[length - 1U])
		return EINVAL;

	/* No longer than the DLC's frame size (the credit octet counts against it), or the default on DLCI 0 and unknown DLCs. */
	limit = BTD_RFCOMM_N1_DEFAULT;
	dlc = rfcomm_find(rf, frame->dlci);
	if (dlc != NULL && frame->dlci != 0U)
		limit = dlc->n1;
	if (frame->length + (size_t)frame->with_credit > limit)
		return EINVAL;

	/* Succeeded: a good frame. */
	return 0;
}

/* Gives a good frame to its handler: DLCI 0's own frames, or a DLC's once the session is open. */
static void
rfcomm_dispatch(
	struct btd_rfcomm *rf,
	const struct rfcomm_frame *frame,
	uint64_t now)
{
	/* DLCI 0's frames other than UIH are the session's own. */
	if (frame->dlci == 0U && frame->type != RFCOMM_UIH) {
		rfcomm_frame_session(rf, frame->type, now);
		return;
	}

	/* Nothing but the opening of DLCI 0 is taken before the session is open; a DLC asked for is refused. */
	if (rf->state != BTD_RFCOMM_SESSION_OPEN) {
		if (frame->type == RFCOMM_SABM)
			(void)rfcomm_send_control(rf, frame->dlci, 0, (uint8_t)(RFCOMM_DM | RFCOMM_PF));
		return;
	}

	/* Each frame type. */
	switch (frame->type) {
	case RFCOMM_SABM:
		rfcomm_frame_sabm(rf, frame->dlci, now);
		break;
	case RFCOMM_UA:
		rfcomm_frame_ua(rf, frame->dlci, now);
		break;
	case RFCOMM_DM:
		rfcomm_frame_dm(rf, frame->dlci, now);
		break;
	case RFCOMM_DISC:
		rfcomm_frame_disc(rf, frame->dlci, now);
		break;
	case RFCOMM_UIH:
		rfcomm_frame_uih(rf, frame->dlci, frame->with_credit, frame->credit, frame->data, frame->length, now);
		break;
	default:
		rf->malformed++;
		break;
	}
}

/* Gives this side's direction bit: 1 for the session's initiator (RFCOMM 1.2 section 5.4). */
static unsigned
rfcomm_direction(
	const struct btd_rfcomm *rf)
{
	/* The initiator's. */
	if (rf->initiator)
		return 1U;

	/* The responder's. */
	return 0U;
}

/* Gives the largest frame size this side accepts: the link's MTU less the frame's overhead, within bluetoothd's buffer. */
static unsigned
rfcomm_n1_max(
	const struct btd_rfcomm *rf)
{
	unsigned n1;

	/* An MTU too small for a frame leaves the default size's floor out: the PN says what the link takes. */
	n1 = BTD_RFCOMM_N1_DEFAULT;
	if (rf->mtu > RFCOMM_FRAME_OVERHEAD)
		n1 = rf->mtu - RFCOMM_FRAME_OVERHEAD;

	/* Within the frame buffer. */
	if (n1 > BTD_RFCOMM_N1_MAX)
		n1 = BTD_RFCOMM_N1_MAX;

	/* Succeeded: the size. */
	return n1;
}

/*
 * Builds one frame: the address (DLCI with the C/R bit TS 27.010 table 1
 * and section 5.4.3.1 give for a command or a response of this side), the
 * control, the length, a credit octet when asked, the data and the FCS.
 * Returns its length, or 0 when it does not fit out.
 */
static size_t
rfcomm_build(
	struct btd_rfcomm *rf,
	uint8_t *out,
	size_t size,
	unsigned dlci,
	int command,
	uint8_t control,
	int with_credit,
	unsigned credit,
	const uint8_t *data,
	size_t length)
{
	size_t used;
	size_t covered;
	unsigned cr;

	/*
	 * The C/R bit: SABM, DISC and every UIH carry 1 from the initiator and
	 * 0 from the responder; UA and DM, the responses, the opposite.
	 */
	cr = rfcomm_direction(rf);
	if (!command)
		cr ^= 1U;

	/* Room for the header's five bytes at most, the data and the FCS. */
	if (length > 0x7fffU || size < length + RFCOMM_FRAME_OVERHEAD + 1U)
		return 0U;

	/* The address and the control. */
	out[0] = (uint8_t)(RFCOMM_EA | (cr << 1) | (dlci << 2));
	out[1] = control;

	/* The length, in one octet up to 127, else two. */
	if (length <= 0x7fU) {
		out[2] = (uint8_t)((length << 1) | RFCOMM_EA);
		used = 3U;
	} else {
		out[2] = (uint8_t)((length << 1) & 0xfeU);
		out[3] = (uint8_t)(length >> 7);
		used = 4U;
	}

	/* The FCS's coverage starts as the header written so far. */
	covered = used;

	/* The credit octet. */
	if (with_credit) {
		out[used] = (uint8_t)credit;
		used++;
	}

	/* The data. */
	if (length != 0U)
		memcpy(out + used, data, length);
	used += length;

	/* The FCS over the address and control of a UIH frame, and the length too of the others. */
	if ((control & ~RFCOMM_PF) == RFCOMM_UIH)
		covered = 2U;
	out[used] = btd_rfcomm_fcs(out, covered);
	used++;

	/* Succeeded: the frame's length. */
	return used;
}

/*
 * Sends one control frame (SABM, UA, DM or DISC) on a DLCI; one the send
 * hook has no room for waits in order.  Returns 0 or the hook's error.
 */
static int
rfcomm_send_control(
	struct btd_rfcomm *rf,
	unsigned dlci,
	int command,
	uint8_t control)
{
	uint8_t bytes[BTD_RFCOMM_CONTROL_MAX];
	size_t length;
	int error;

	/* The frame. */
	length = rfcomm_build(rf, bytes, sizeof(bytes), dlci, command, control, 0, 0U, NULL, 0U);
	if (length == 0U)
		return EMSGSIZE;

	/* Sent, or kept until there is room. */
	error = rfcomm_queue(rf, bytes, length);
	if (error != 0)
		return error;

	/* Succeeded: sent or waiting. */
	return 0;
}

/*
 * Sends one multiplexer message on DLCI 0 (TS 27.010 section 5.4.6.1): its
 * type with the message's own C/R bit (1 for a command), its length and its
 * value; RFCOMM puts one message in a frame (RFCOMM 1.2 section 5.5).
 */
static int
rfcomm_send_mux(
	struct btd_rfcomm *rf,
	uint8_t type,
	int command,
	const uint8_t *value,
	size_t length)
{
	uint8_t message[RFCOMM_PN_LENGTH + 2U];
	uint8_t bytes[BTD_RFCOMM_CONTROL_MAX];
	size_t frame;
	int error;

	/* A value bluetoothd sends fits one length octet and the buffer. */
	if (length > RFCOMM_PN_LENGTH)
		return EMSGSIZE;

	/* The type, the length and the value. */
	message[0] = (uint8_t)(type | RFCOMM_EA);
	if (command)
		message[0] |= RFCOMM_CR;
	message[1] = (uint8_t)((length << 1) | RFCOMM_EA);
	if (length != 0U)
		memcpy(message + 2, value, length);

	/* In a UIH frame on DLCI 0 (its C/R is the UIH's, whatever the message). */
	frame = rfcomm_build(rf, bytes, sizeof(bytes), 0U, 1, RFCOMM_UIH, 0, 0U, message, length + 2U);
	if (frame == 0U)
		return EMSGSIZE;

	/* Sent, or kept until there is room. */
	error = rfcomm_queue(rf, bytes, frame);
	if (error != 0)
		return error;

	/* Succeeded: sent or waiting. */
	return 0;
}

/*
 * Sends a control frame, or keeps it when the send hook has no room or
 * frames wait already (they keep their order).  A full queue fails the
 * session.  Returns 0 or the hook's error other than ENOBUFS.
 */
static int
rfcomm_queue(
	struct btd_rfcomm *rf,
	const uint8_t *bytes,
	size_t length)
{
	struct btd_rfcomm_pending *pending;
	int error;

	/* Sent now when nothing waits before it. */
	if (rf->pending_count == 0U) {
		error = rf->events.send(rf->events.context, bytes, length);
		if (error == 0)
			return 0;
		if (error != ENOBUFS)
			return error;
	}

	/* No room to keep it: the session cannot go on in order. */
	if (rf->pending_count >= BTD_RFCOMM_PENDING_MAX || length > BTD_RFCOMM_CONTROL_MAX) {
		rf->overrun++;
		if (rf->failed == 0)
			rf->failed = BTD_RFCOMM_CLOSED_ERROR;
		return 0;
	}

	/* Succeeded: kept at the end of those waiting. */
	pending = &rf->pending[rf->pending_count];
	pending->length = length;
	memcpy(pending->bytes, bytes, length);
	rf->pending_count++;
	return 0;
}

/* Sends the control frames that waited, oldest first; returns 0, ENOBUFS when some still wait, or the hook's error. */
static int
rfcomm_flush(
	struct btd_rfcomm *rf)
{
	int error;

	/* The oldest first, until the hook has no room. */
	while (rf->pending_count != 0U) {
		error = rf->events.send(rf->events.context, rf->pending[0].bytes, rf->pending[0].length);
		if (error != 0)
			return error;

		/* Sent: the later ones move up. */
		rf->pending_count--;
		memmove(&rf->pending[0], &rf->pending[1], sizeof(rf->pending[0]) * rf->pending_count);
	}

	/* Succeeded: nothing waits. */
	return 0;
}

/*
 * Sends a PN for a DLC (TS 27.010 table 3, RFCOMM 1.2 section 5.5.3): the
 * DLCI, I 0 and the CL given, priority 0, T 0, the frame size, NA 0, and
 * the credits given in K.
 */
static int
rfcomm_send_pn(
	struct btd_rfcomm *rf,
	const struct btd_rfcomm_dlc *dlc,
	int command,
	uint8_t cl,
	unsigned credits)
{
	uint8_t value[RFCOMM_PN_LENGTH];
	int error;

	/* The eight value octets. */
	value[0] = (uint8_t)(dlc->dlci & 0x3fU);
	value[1] = (uint8_t)(cl << 4);
	value[2] = 0x00U;
	value[3] = 0x00U;
	value[4] = (uint8_t)(dlc->n1 & 0xffU);
	value[5] = (uint8_t)(dlc->n1 >> 8);
	value[6] = 0x00U;
	value[7] = (uint8_t)(credits & 0x07U);

	/* Sent. */
	error = rfcomm_send_mux(rf, RFCOMM_MUX_PN, command, value, sizeof(value));
	if (error != 0)
		return error;

	/* Succeeded: sent or waiting. */
	return 0;
}

/* Sends this side's modem status command for a DLC just opened (TS 27.010 section 5.4.6.3.7). */
static int
rfcomm_send_msc(
	struct btd_rfcomm *rf,
	unsigned dlci)
{
	uint8_t value[2];
	int error;

	/* The DLCI octet (EA and bit 2 set) and this side's V.24 signals. */
	value[0] = (uint8_t)(RFCOMM_EA | RFCOMM_CR | (dlci << 2));
	value[1] = RFCOMM_V24_SIGNALS;

	/* Sent. */
	error = rfcomm_send_mux(rf, RFCOMM_MUX_MSC, 1, value, sizeof(value));
	if (error != 0)
		return error;

	/* Succeeded: sent or waiting. */
	return 0;
}

/*
 * Gives the peer back the credits it used on a connected DLC, up to the
 * DLC's share, once it holds half the share or less (a UIH with P/F set and
 * no data, RFCOMM 1.2 section 6.5.2).  A frame the hook refuses is given
 * again by the next pump: the difference is computed again.
 */
static void
rfcomm_give_credits(
	struct btd_rfcomm *rf,
	struct btd_rfcomm_dlc *dlc)
{
	uint8_t bytes[BTD_RFCOMM_CONTROL_MAX];
	unsigned share;
	unsigned give;
	size_t length;
	int error;

	/* Only a connected DLC of a credit based session, with control frames not waiting before it. */
	if (dlc->state != BTD_RFCOMM_DLC_CONNECTED ||
	    !rf->credit_flow ||
	    rf->pending_count != 0U)
		return;

	/* Enough credits still out. */
	share = rfcomm_credit_share(rf, dlc);
	if (dlc->rx_credits * 2U > share)
		return;
	give = share - dlc->rx_credits;
	if (give == 0U)
		return;

	/* The credit frame, sent now or not at all. */
	length = rfcomm_build(rf, bytes, sizeof(bytes), dlc->dlci, 1, (uint8_t)(RFCOMM_UIH | RFCOMM_PF), 1, give, NULL, 0U);
	if (length == 0U)
		return;
	error = rf->events.send(rf->events.context, bytes, length);
	if (error != 0)
		return;

	/* Succeeded: the peer may send that many more frames. */
	dlc->rx_credits += give;
}

/*
 * Gives a DLC's share of the session's credit bytes (BTD_RFCOMM_CREDIT_BYTES
 * over the DLCs in use, in frames of its size), at least 1 and at most 255.
 */
static unsigned
rfcomm_credit_share(
	const struct btd_rfcomm *rf,
	const struct btd_rfcomm_dlc *dlc)
{
	unsigned active;
	unsigned n1;
	unsigned share;

	/* The DLCs sharing the bytes, this one at least. */
	active = rfcomm_active(rf);
	if (active == 0U)
		active = 1U;

	/* Its frames' worth. */
	n1 = dlc->n1;
	if (n1 == 0U)
		n1 = BTD_RFCOMM_N1_DEFAULT;
	share = BTD_RFCOMM_CREDIT_BYTES / n1 / active;

	/* Within one frame and what a credit octet holds. */
	if (share == 0U)
		share = 1U;
	if (share > RFCOMM_CREDITS_MAX)
		share = RFCOMM_CREDITS_MAX;

	/* Succeeded: the share. */
	return share;
}

/* Finds a DLC in use by its DLCI, or NULL. */
static struct btd_rfcomm_dlc *
rfcomm_find(
	struct btd_rfcomm *rf,
	unsigned dlci)
{
	unsigned index;

	/* Each DLC in use. */
	for (index = 0U; index < BTD_RFCOMM_DLCS_MAX; index++) {
		if (rf->dlcs[index].state != BTD_RFCOMM_DLC_FREE && rf->dlcs[index].dlci == dlci)
			return &rf->dlcs[index];
	}

	/* None. */
	return NULL;
}

/* Takes a free DLC for a DLCI, emptied (state waiting, the caller sets the real one), or NULL when all are in use. */
static struct btd_rfcomm_dlc *
rfcomm_slot(
	struct btd_rfcomm *rf,
	unsigned dlci)
{
	struct btd_rfcomm_dlc *dlc;
	unsigned index;

	/* The first free one. */
	for (index = 0U; index < BTD_RFCOMM_DLCS_MAX; index++) {
		dlc = &rf->dlcs[index];
		if (dlc->state != BTD_RFCOMM_DLC_FREE)
			continue;

		/* Succeeded: emptied and in use, the session no longer idle. */
		memset(dlc, 0, sizeof(*dlc));
		dlc->state = BTD_RFCOMM_DLC_WAITING;
		dlc->dlci = dlci;
		dlc->n1 = BTD_RFCOMM_N1_DEFAULT;
		rf->idle_since = 0U;
		return dlc;
	}

	/* All in use. */
	return NULL;
}

/* Tells whether the owner knows a DLC: it asked for it, or was told it opened (or closed one it was told of). */
static int
rfcomm_announced(
	const struct btd_rfcomm_dlc *dlc)
{
	/* The owner's own. */
	if (dlc->ours)
		return 1;

	/* The peer's, told when connected. */
	if (dlc->state == BTD_RFCOMM_DLC_CONNECTED || dlc->state == BTD_RFCOMM_DLC_CLOSING)
		return 1;

	/* Never told. */
	return 0;
}

/* Counts the DLCs in use. */
static unsigned
rfcomm_active(
	const struct btd_rfcomm *rf)
{
	unsigned index;
	unsigned count;

	/* Each DLC not free. */
	count = 0U;
	for (index = 0U; index < BTD_RFCOMM_DLCS_MAX; index++) {
		if (rf->dlcs[index].state != BTD_RFCOMM_DLC_FREE)
			count++;
	}

	/* Succeeded: the count. */
	return count;
}

/*
 * Frees a DLC, telling the owner when it knew it (reason 0: the owner
 * closed it itself and is not told); the last DLC of an open session
 * starts its idle time.
 */
static void
rfcomm_free(
	struct btd_rfcomm *rf,
	struct btd_rfcomm_dlc *dlc,
	int reason,
	uint64_t now)
{
	unsigned dlci;
	unsigned active;
	int told;

	/* Whether the owner is told, decided before the DLC goes. */
	told = rfcomm_announced(dlc);
	if (reason == 0)
		told = 0;

	/* The DLC goes. */
	dlci = dlc->dlci;
	memset(dlc, 0, sizeof(*dlc));
	dlc->state = BTD_RFCOMM_DLC_FREE;

	/* The last one gone: the session is idle from now (its time starts at 1 at least). */
	active = rfcomm_active(rf);
	if (rf->state == BTD_RFCOMM_SESSION_OPEN && active == 0U) {
		rf->idle_since = now;
		if (rf->idle_since == 0U)
			rf->idle_since = 1U;
	}

	/* Succeeded: the owner told (it may act on the session inside the call). */
	if (told && rf->events.closed != NULL)
		rf->events.closed(rf->events.context, dlci, reason);
}

/*
 * Ends the session: every DLC the owner knows is told with the reason, the
 * session is closed, and the owner is told it ended (tell 0: it knew).
 */
static void
rfcomm_end(
	struct btd_rfcomm *rf,
	int reason,
	int tell)
{
	struct btd_rfcomm_dlc *dlc;
	unsigned dlci;
	unsigned index;
	int told;

	/* Closed first, so that the owner's calls inside the callbacks find no session. */
	rf->state = BTD_RFCOMM_SESSION_CLOSED;
	rf->deadline = 0U;
	rf->idle_since = 0U;
	rf->pending_count = 0U;
	rf->failed = 0;

	/* Each DLC: freed, and told when the owner knew it. */
	for (index = 0U; index < BTD_RFCOMM_DLCS_MAX; index++) {
		dlc = &rf->dlcs[index];
		if (dlc->state == BTD_RFCOMM_DLC_FREE)
			continue;
		told = rfcomm_announced(dlc);
		dlci = dlc->dlci;
		memset(dlc, 0, sizeof(*dlc));
		dlc->state = BTD_RFCOMM_DLC_FREE;

		/* The owner told of the ones it knew. */
		if (told && rf->events.closed != NULL)
			rf->events.closed(rf->events.context, dlci, reason);
	}

	/* The owner closes the L2CAP channel. */
	if (tell && rf->events.ended != NULL)
		rf->events.ended(rf->events.context, reason);
}

/* Ends the session when something marked it failed, once the entry point is done with it. */
static void
rfcomm_settle(
	struct btd_rfcomm *rf)
{
	int reason;

	/* Nothing failed, or nothing left to end. */
	if (rf->failed == 0)
		return;
	if (rf->state == BTD_RFCOMM_SESSION_CLOSED) {
		rf->failed = 0;
		return;
	}

	/* Succeeded: ended with the reason. */
	reason = rf->failed;
	rfcomm_end(rf, reason, 1);
}

/*
 * Tells whether a DLCI names one of bluetoothd's server channels offered
 * now: its direction bit is this side's (servers on the initiator are on
 * odd DLCIs, RFCOMM 1.2 section 5.4), the channel is listened on, and the
 * owner accepts it.
 */
static int
rfcomm_server_offered(
	struct btd_rfcomm *rf,
	unsigned dlci)
{
	unsigned channel;
	unsigned direction;
	unsigned index;
	int listened;
	int accepted;

	/* A DLCI pointing at this side. */
	direction = rfcomm_direction(rf);
	if ((dlci & 1U) != direction)
		return 0;

	/* A channel listened on. */
	channel = dlci >> 1;
	listened = 0;
	for (index = 0U; index < rf->server_count; index++) {
		if (rf->servers[index] == channel)
			listened = 1;
	}

	/* A channel nobody listens on. */
	if (!listened)
		return 0;

	/* No owner's hook: nothing is accepted. */
	if (rf->events.accept == NULL)
		return 0;

	/* The owner's answer now. */
	accepted = rf->events.accept(rf->events.context, channel);
	if (accepted != 1)
		return 0;

	/* Succeeded: offered. */
	return 1;
}

/* Moves the DLCs that waited for the session on: each sends its PN. */
static void
rfcomm_session_opened(
	struct btd_rfcomm *rf,
	uint64_t now)
{
	struct btd_rfcomm_dlc *dlc;
	unsigned index;
	unsigned credits;
	int error;

	/* Open: no timer of its own. */
	rf->state = BTD_RFCOMM_SESSION_OPEN;
	rf->deadline = 0U;

	/* Each waiting DLC's PN, as btd_rfcomm_connect would have sent it. */
	for (index = 0U; index < BTD_RFCOMM_DLCS_MAX; index++) {
		dlc = &rf->dlcs[index];
		if (dlc->state != BTD_RFCOMM_DLC_WAITING)
			continue;

		/* The PN with the first credits; a failure fails the session. */
		credits = rfcomm_credit_share(rf, dlc);
		if (credits > BTD_RFCOMM_PN_CREDITS_MAX)
			credits = BTD_RFCOMM_PN_CREDITS_MAX;
		error = rfcomm_send_pn(rf, dlc, 1, RFCOMM_PN_CL_REQUEST, credits);
		if (error != 0) {
			if (rf->failed == 0)
				rf->failed = BTD_RFCOMM_CLOSED_ERROR;
			return;
		}

		/* Negotiating. */
		dlc->rx_credits = credits;
		dlc->state = BTD_RFCOMM_DLC_NEGOTIATING;
		dlc->deadline = now + BTD_RFCOMM_T2_MS;
	}
}

/*
 * Takes a frame of DLCI 0 other than UIH: the multiplexer's start (the
 * peer's SABM, or the UA or DM to this side's) and its close-down (the
 * peer's DISC, or the answer to this side's).
 */
static void
rfcomm_frame_session(
	struct btd_rfcomm *rf,
	uint8_t type,
	uint64_t now)
{
	/* Each type of DLCI 0's frames. */
	switch (type) {
	case RFCOMM_SABM:
		/* The peer starts the multiplexer: this side answers as the responder (or again, for a repeated SABM). */
		if (rf->state == BTD_RFCOMM_SESSION_CLOSED && !rf->initiator) {
			(void)rfcomm_send_control(rf, 0U, 0, (uint8_t)(RFCOMM_UA | RFCOMM_PF));
			rf->state = BTD_RFCOMM_SESSION_OPEN;
		} else if (rf->state == BTD_RFCOMM_SESSION_OPEN && !rf->initiator) {
			(void)rfcomm_send_control(rf, 0U, 0, (uint8_t)(RFCOMM_UA | RFCOMM_PF));
		} else {
			(void)rfcomm_send_control(rf, 0U, 0, (uint8_t)(RFCOMM_DM | RFCOMM_PF));
		}

		break;
	case RFCOMM_UA:
		/* This side's SABM answered: open; its DISC answered: ended. */
		if (rf->state == BTD_RFCOMM_SESSION_OPENING) {
			rfcomm_session_opened(rf, now);
		} else if (rf->state == BTD_RFCOMM_SESSION_CLOSING) {
			if (rf->failed == 0)
				rf->failed = BTD_RFCOMM_CLOSED_LOCAL;
		}

		break;
	case RFCOMM_DM:
		/* This side's SABM refused: the session cannot open; its DISC refused: ended all the same. */
		if (rf->state == BTD_RFCOMM_SESSION_OPENING) {
			if (rf->failed == 0)
				rf->failed = BTD_RFCOMM_CLOSED_REFUSED;
		} else if (rf->state == BTD_RFCOMM_SESSION_CLOSING) {
			if (rf->failed == 0)
				rf->failed = BTD_RFCOMM_CLOSED_LOCAL;
		}

		break;
	case RFCOMM_DISC:
		/* The peer closes the multiplexer: UA, then everything ends. */
		(void)rfcomm_send_control(rf, 0U, 0, (uint8_t)(RFCOMM_UA | RFCOMM_PF));
		if (rf->failed == 0)
			rf->failed = BTD_RFCOMM_CLOSED_REMOTE;
		break;
	default:
		rf->malformed++;
		break;
	}
}

/*
 * Takes the peer's SABM on a DLC: one of bluetoothd's server channels
 * offered now is opened (UA, then this side's modem status), after a PN or
 * without one once the session agreed on credits (RFCOMM 1.2 section
 * 6.5.1); anything else is refused with DM.
 */
static void
rfcomm_frame_sabm(
	struct btd_rfcomm *rf,
	unsigned dlci,
	uint64_t now)
{
	struct btd_rfcomm_dlc *dlc;
	int offered;
	unsigned n1;

	/* An open DLC asked again: UA again. */
	dlc = rfcomm_find(rf, dlci);
	if (dlc != NULL &&
	    (dlc->state == BTD_RFCOMM_DLC_OPEN ||
	     dlc->state == BTD_RFCOMM_DLC_CONNECTED)) {
		(void)rfcomm_send_control(rf, dlci, 0, (uint8_t)(RFCOMM_UA | RFCOMM_PF));
		return;
	}

	/* One of this side's own DLCs (the DLCI cannot be the peer's to open). */
	if (dlc != NULL && dlc->state != BTD_RFCOMM_DLC_NEGOTIATED) {
		rf->refused++;
		(void)rfcomm_send_control(rf, dlci, 0, (uint8_t)(RFCOMM_DM | RFCOMM_PF));
		return;
	}

	/* A server channel offered now (asked again: the owner's answer may have changed since the PN). */
	offered = rfcomm_server_offered(rf, dlci);
	if (!offered) {
		if (dlc != NULL)
			rfcomm_free(rf, dlc, 0, now);
		rf->refused++;
		(void)rfcomm_send_control(rf, dlci, 0, (uint8_t)(RFCOMM_DM | RFCOMM_PF));
		return;
	}

	/* Without a PN: only once the session agreed on credits, with the default frame size and no credits yet. */
	if (dlc == NULL) {
		if (!rf->credit_flow) {
			rf->refused++;
			(void)rfcomm_send_control(rf, dlci, 0, (uint8_t)(RFCOMM_DM | RFCOMM_PF));
			return;
		}

		/* A free DLC for it. */
		dlc = rfcomm_slot(rf, dlci);
		if (dlc == NULL) {
			rf->refused++;
			(void)rfcomm_send_control(rf, dlci, 0, (uint8_t)(RFCOMM_DM | RFCOMM_PF));
			return;
		}

		/* The default frame size, within what this link takes. */
		n1 = rfcomm_n1_max(rf);
		if (n1 > BTD_RFCOMM_N1_DEFAULT)
			n1 = BTD_RFCOMM_N1_DEFAULT;
		dlc->n1 = n1;
		dlc->tx_credits = 0U;
		dlc->rx_credits = 0U;
	}

	/* Succeeded: UA, then the modem status exchange. */
	(void)rfcomm_send_control(rf, dlci, 0, (uint8_t)(RFCOMM_UA | RFCOMM_PF));
	rfcomm_open_dlc(rf, dlc, now);
}

/* Takes UA on a DLC: this side's SABM answered (the modem status next) or its DISC answered (closed). */
static void
rfcomm_frame_ua(
	struct btd_rfcomm *rf,
	unsigned dlci,
	uint64_t now)
{
	struct btd_rfcomm_dlc *dlc;

	/* A DLC waiting for it. */
	dlc = rfcomm_find(rf, dlci);
	if (dlc == NULL)
		return;

	/* SABM answered: open. */
	if (dlc->state == BTD_RFCOMM_DLC_CONNECTING) {
		rfcomm_open_dlc(rf, dlc, now);
		return;
	}

	/* DISC answered: closed as asked. */
	if (dlc->state == BTD_RFCOMM_DLC_CLOSING)
		rfcomm_free(rf, dlc, BTD_RFCOMM_CLOSED_LOCAL, now);
}

/* Takes DM on a DLC: this side's PN or SABM refused, its DISC on a DLC already gone, or the peer's end of an open DLC. */
static void
rfcomm_frame_dm(
	struct btd_rfcomm *rf,
	unsigned dlci,
	uint64_t now)
{
	struct btd_rfcomm_dlc *dlc;

	/* The DLC, if any. */
	dlc = rfcomm_find(rf, dlci);
	if (dlc == NULL)
		return;

	/* Each state's meaning. */
	if (dlc->state == BTD_RFCOMM_DLC_NEGOTIATING || dlc->state == BTD_RFCOMM_DLC_CONNECTING) {
		/* Refused. */
		rfcomm_free(rf, dlc, BTD_RFCOMM_CLOSED_REFUSED, now);
	} else if (dlc->state == BTD_RFCOMM_DLC_CLOSING) {
		/* Gone already: closed as asked. */
		rfcomm_free(rf, dlc, BTD_RFCOMM_CLOSED_LOCAL, now);
	} else {
		/* Ended by the peer. */
		rfcomm_free(rf, dlc, BTD_RFCOMM_CLOSED_REMOTE, now);
	}
}

/* Takes the peer's DISC on a DLC: UA and closed, or DM for a DLC that is not open. */
static void
rfcomm_frame_disc(
	struct btd_rfcomm *rf,
	unsigned dlci,
	uint64_t now)
{
	struct btd_rfcomm_dlc *dlc;

	/* No such DLC: DM. */
	dlc = rfcomm_find(rf, dlci);
	if (dlc == NULL) {
		(void)rfcomm_send_control(rf, dlci, 0, (uint8_t)(RFCOMM_DM | RFCOMM_PF));
		return;
	}

	/* Succeeded: UA, and the DLC closed by the peer. */
	(void)rfcomm_send_control(rf, dlci, 0, (uint8_t)(RFCOMM_UA | RFCOMM_PF));
	rfcomm_free(rf, dlc, BTD_RFCOMM_CLOSED_REMOTE, now);
}

/*
 * Takes a UIH frame: DLCI 0's are multiplexer messages; a DLC's carry
 * credits for this side and data, which spends one of the credits given
 * (data beyond them ends the session: the peer broke the flow control).
 */
static void
rfcomm_frame_uih(
	struct btd_rfcomm *rf,
	unsigned dlci,
	int with_credit,
	unsigned credit,
	const uint8_t *data,
	size_t length,
	uint64_t now)
{
	struct btd_rfcomm_dlc *dlc;
	unsigned before;

	/* The multiplexer's messages. */
	if (dlci == 0U) {
		rfcomm_mux(rf, data, length, now);
		return;
	}

	/* A DLC opened (an unknown one is passed over). */
	dlc = rfcomm_find(rf, dlci);
	if (dlc == NULL ||
	    (dlc->state != BTD_RFCOMM_DLC_OPEN &&
	     dlc->state != BTD_RFCOMM_DLC_CONNECTED &&
	     dlc->state != BTD_RFCOMM_DLC_CLOSING))
		return;

	/* Credits for this side's frames (a frame without data may carry them alone). */
	before = dlc->tx_credits;
	if (with_credit) {
		dlc->tx_credits += credit;
		if (dlc->tx_credits > RFCOMM_CREDITS_MAX)
			dlc->tx_credits = RFCOMM_CREDITS_MAX;
	}

	/* Data spends a credit given; none left means the peer sent past them. */
	if (length != 0U && rf->credit_flow) {
		if (dlc->rx_credits == 0U) {
			rf->overrun++;
			if (rf->failed == 0)
				rf->failed = BTD_RFCOMM_CLOSED_ERROR;
			return;
		}

		/* One credit spent. */
		dlc->rx_credits--;
	}

	/* Data before the modem status exchange is passed over (RFCOMM 1.2 section 6.3); the owner's comes to it. */
	if (length != 0U && dlc->state == BTD_RFCOMM_DLC_CONNECTED && rf->events.data != NULL)
		rf->events.data(rf->events.context, dlci, data, length);

	/* The DLC may have closed inside the call. */
	dlc = rfcomm_find(rf, dlci);
	if (dlc == NULL)
		return;

	/* The frame was processed: its credit goes back when the share runs low. */
	rfcomm_give_credits(rf, dlc);

	/* Credits that came to a DLC that had none: its writer may go on. */
	if (before == 0U &&
	    dlc->tx_credits != 0U &&
	    dlc->state == BTD_RFCOMM_DLC_CONNECTED &&
	    rf->events.writable != NULL)
		rf->events.writable(rf->events.context, dlci);
}

/*
 * Takes one multiplexer message (TS 27.010 section 5.4.6): its type (one
 * octet; a longer type is not supported), its length and its value.
 * Commands are answered; the answers to this side's commands move its DLCs
 * on.
 */
static void
rfcomm_mux(
	struct btd_rfcomm *rf,
	const uint8_t *data,
	size_t length,
	uint64_t now)
{
	uint8_t type;
	uint8_t kind;
	int command;
	size_t value_length;
	const uint8_t *value;
	uint8_t nsc[1];

	/* A type and a length at least. */
	if (length < 2U) {
		rf->malformed++;
		return;
	}

	/* The type, with the message's own command/response bit. */
	type = data[0];
	command = 0;
	if ((type & RFCOMM_CR) != 0U)
		command = 1;
	kind = (uint8_t)(type & RFCOMM_MUX_TYPE_MASK);

	/* A type of more than one octet: none is defined, so a command of one is not supported. */
	if ((type & RFCOMM_EA) == 0U) {
		if (command) {
			nsc[0] = type;
			(void)rfcomm_send_mux(rf, RFCOMM_MUX_NSC, 0, nsc, sizeof(nsc));
		}

		/* Passed over. */
		return;
	}

	/* The length octet (RFCOMM's values fit one) and the value within the frame. */
	if ((data[1] & RFCOMM_EA) == 0U) {
		rf->malformed++;
		return;
	}

	/* The value's length. */
	value_length = (size_t)(data[1] >> 1);
	if (value_length > length - 2U) {
		rf->malformed++;
		return;
	}

	/* The value. */
	value = data + 2;

	/* Each message type. */
	switch (kind) {
	case RFCOMM_MUX_PN:
		if (value_length < RFCOMM_PN_LENGTH) {
			rf->malformed++;
			break;
		}

		/* The peer's command, or the answer to this side's. */
		if (command) {
			rfcomm_pn_command(rf, value, now);
		} else {
			rfcomm_pn_response(rf, value, now);
		}

		break;
	case RFCOMM_MUX_MSC:
		if (value_length < 2U) {
			rf->malformed++;
			break;
		}

		/* The modem status. */
		rfcomm_msc(rf, value, value_length, command);
		break;
	case RFCOMM_MUX_RPN:
		if (command)
			rfcomm_rpn(rf, value, value_length);
		break;
	case RFCOMM_MUX_RLS:
	case RFCOMM_MUX_TEST:
		/* The line status and the test: answered with the value they came with (a test's is at most what bluetoothd sends back). */
		if (command && value_length <= RFCOMM_PN_LENGTH)
			(void)rfcomm_send_mux(rf, kind, 0, value, value_length);
		break;
	case RFCOMM_MUX_FCON:
	case RFCOMM_MUX_FCOFF:
		/* Answered; with credits they carry no meaning (RFCOMM 1.2 section 6.5.3). */
		if (command)
			(void)rfcomm_send_mux(rf, kind, 0, NULL, 0U);
		break;
	case RFCOMM_MUX_NSC:
		/* The peer did not know one of this side's commands. */
		rf->refused++;
		break;
	default:
		/* Any other command is not supported (RFCOMM 1.2 section 4.3), the close-down command included. */
		if (command) {
			nsc[0] = type;
			(void)rfcomm_send_mux(rf, RFCOMM_MUX_NSC, 0, nsc, sizeof(nsc));
		}

		break;
	}
}

/*
 * Takes the peer's PN (RFCOMM 1.2 section 5.5.3): for a DLC open already
 * its present values come back unchanged; for one of bluetoothd's server
 * channels offered now, with credit based flow control asked for (CL 0xf),
 * the answer sets the frame size (at most what was asked) and the credits
 * of each side; anything else is refused with DM on its DLCI.
 */
static void
rfcomm_pn_command(
	struct btd_rfcomm *rf,
	const uint8_t *value,
	uint64_t now)
{
	struct btd_rfcomm_dlc *dlc;
	unsigned dlci;
	unsigned cl;
	unsigned n1;
	unsigned credits;
	unsigned answer_cl;
	int offered;

	/* The DLCI, CL, frame size and credits asked. */
	dlci = (unsigned)(value[0] & 0x3fU);
	cl = (unsigned)(value[1] >> 4);
	n1 = (unsigned)value[4] | ((unsigned)value[5] << 8);
	credits = (unsigned)(value[7] & 0x07U);

	/* DLCI 0 has no parameters. */
	if (dlci == 0U) {
		rf->malformed++;
		return;
	}

	/* An open DLC: its present values, no new credits, CL as the request's was 0xf or not. */
	dlc = rfcomm_find(rf, dlci);
	if (dlc != NULL && dlc->state != BTD_RFCOMM_DLC_NEGOTIATED) {
		answer_cl = 0U;
		if (cl == RFCOMM_PN_CL_REQUEST)
			answer_cl = RFCOMM_PN_CL_RESPONSE;
		(void)rfcomm_send_pn(rf, dlc, 0, (uint8_t)answer_cl, 0U);
		return;
	}

	/* A server channel offered now, credit based flow control asked for, and a frame size. */
	offered = rfcomm_server_offered(rf, dlci);
	if (!offered ||
	    cl != RFCOMM_PN_CL_REQUEST ||
	    n1 == 0U) {
		if (dlc != NULL)
			rfcomm_free(rf, dlc, 0, now);
		rf->refused++;
		(void)rfcomm_send_control(rf, dlci, 0, (uint8_t)(RFCOMM_DM | RFCOMM_PF));
		return;
	}

	/* A DLC for it (a PN repeated before the SABM settles again). */
	if (dlc == NULL)
		dlc = rfcomm_slot(rf, dlci);
	if (dlc == NULL) {
		rf->refused++;
		(void)rfcomm_send_control(rf, dlci, 0, (uint8_t)(RFCOMM_DM | RFCOMM_PF));
		return;
	}

	/* The frame size: the asked one, or less when this side takes less. */
	dlc->n1 = rfcomm_n1_max(rf);
	if (n1 < dlc->n1)
		dlc->n1 = n1;

	/* The session uses credits from now on; the peer gives these, this side its share up to 7. */
	rf->credit_flow = 1;
	dlc->tx_credits = credits;
	credits = rfcomm_credit_share(rf, dlc);
	if (credits > BTD_RFCOMM_PN_CREDITS_MAX)
		credits = BTD_RFCOMM_PN_CREDITS_MAX;
	dlc->rx_credits = credits;

	/* Succeeded: answered, the SABM awaited (forgotten if it does not come). */
	dlc->state = BTD_RFCOMM_DLC_NEGOTIATED;
	dlc->deadline = now + BTD_RFCOMM_T1_DLC_MS;
	(void)rfcomm_send_pn(rf, dlc, 0, RFCOMM_PN_CL_RESPONSE, credits);
}

/*
 * Takes the answer to this side's PN: credit based flow control agreed
 * (CL 0xe) and a frame size no larger than asked move the DLC on to its
 * SABM; anything else refuses the DLC (DM, RFCOMM 1.2 section 5.5.3).
 */
static void
rfcomm_pn_response(
	struct btd_rfcomm *rf,
	const uint8_t *value,
	uint64_t now)
{
	struct btd_rfcomm_dlc *dlc;
	unsigned dlci;
	unsigned cl;
	unsigned n1;
	int error;

	/* The DLC waiting for it. */
	dlci = (unsigned)(value[0] & 0x3fU);
	dlc = rfcomm_find(rf, dlci);
	if (dlc == NULL || dlc->state != BTD_RFCOMM_DLC_NEGOTIATING)
		return;
	cl = (unsigned)(value[1] >> 4);
	n1 = (unsigned)value[4] | ((unsigned)value[5] << 8);

	/* A peer without credits (Bluetooth 1.0B) or a larger frame than asked: refused. */
	if (cl != RFCOMM_PN_CL_RESPONSE ||
	    n1 == 0U ||
	    n1 > dlc->n1) {
		rf->refused++;
		(void)rfcomm_send_control(rf, dlci, 0, (uint8_t)(RFCOMM_DM | RFCOMM_PF));
		rfcomm_free(rf, dlc, BTD_RFCOMM_CLOSED_REFUSED, now);
		return;
	}

	/* The agreed size and the peer's first credits; the session uses credits from now on. */
	rf->credit_flow = 1;
	dlc->n1 = n1;
	dlc->tx_credits = (unsigned)(value[7] & 0x07U);

	/* SABM, its answer within the DLC's T1. */
	error = rfcomm_send_control(rf, dlci, 1, (uint8_t)(RFCOMM_SABM | RFCOMM_PF));
	if (error != 0) {
		if (rf->failed == 0)
			rf->failed = BTD_RFCOMM_CLOSED_ERROR;
		return;
	}

	/* Succeeded: connecting. */
	dlc->state = BTD_RFCOMM_DLC_CONNECTING;
	dlc->deadline = now + BTD_RFCOMM_T1_DLC_MS;
}

/*
 * Takes a modem status message: the peer's command is answered with a copy
 * of its signals and counts as received; the answer to this side's counts
 * as answered.  Both done connect the DLC.
 */
static void
rfcomm_msc(
	struct btd_rfcomm *rf,
	const uint8_t *value,
	size_t length,
	int command)
{
	struct btd_rfcomm_dlc *dlc;
	unsigned dlci;
	size_t answered;

	/* The DLC named. */
	dlci = (unsigned)(value[0] >> 2);
	dlc = rfcomm_find(rf, dlci);

	/* The peer's status: answered with the signals it sent, its break octet too (TS 27.010 section 5.4.6.3.7). */
	answered = length;
	if (answered > 3U)
		answered = 3U;
	if (command) {
		(void)rfcomm_send_mux(rf, RFCOMM_MUX_MSC, 0, value, answered);
		if (dlc == NULL)
			return;
		dlc->msc_received = 1;
	} else {
		if (dlc == NULL || !dlc->msc_sent)
			return;
		dlc->msc_answered = 1;
	}

	/* Both directions done on an open DLC: connected. */
	if (dlc->state == BTD_RFCOMM_DLC_OPEN &&
	    dlc->msc_received &&
	    dlc->msc_answered)
		rfcomm_connected(rf, dlc);
}

/*
 * Answers the peer's RPN (RFCOMM 1.2 section 5.5.1: recognized and
 * answered): a request of values gets them back with every parameter
 * accepted; a request that only asks gets the defaults.
 */
static void
rfcomm_rpn(
	struct btd_rfcomm *rf,
	const uint8_t *value,
	size_t length)
{
	uint8_t answer[RFCOMM_RPN_LENGTH];

	/* The values asked for, accepted as they are. */
	if (length >= RFCOMM_RPN_LENGTH) {
		(void)rfcomm_send_mux(rf, RFCOMM_MUX_RPN, 0, value, RFCOMM_RPN_LENGTH);
		return;
	}

	/* A DLCI octet at least. */
	if (length < 1U) {
		rf->malformed++;
		return;
	}

	/* Succeeded: the defaults, every parameter named. */
	answer[0] = value[0];
	answer[1] = RFCOMM_RPN_BAUD_9600;
	answer[2] = RFCOMM_RPN_DATA_8N1;
	answer[3] = 0x00U;
	answer[4] = RFCOMM_RPN_XON;
	answer[5] = RFCOMM_RPN_XOFF;
	answer[6] = 0x7fU;
	answer[7] = 0x3fU;
	(void)rfcomm_send_mux(rf, RFCOMM_MUX_RPN, 0, answer, sizeof(answer));
}

/* Connects a DLC whose modem status went both ways: the owner is told, credits go out, its writer may begin. */
static void
rfcomm_connected(
	struct btd_rfcomm *rf,
	struct btd_rfcomm_dlc *dlc)
{
	unsigned dlci;

	/* Connected: no answer awaited. */
	dlc->state = BTD_RFCOMM_DLC_CONNECTED;
	dlc->deadline = 0U;
	dlci = dlc->dlci;

	/* The owner told (it may write or close inside the call). */
	if (rf->events.opened != NULL)
		rf->events.opened(rf->events.context, dlci);

	/* The DLC may have closed meanwhile. */
	dlc = rfcomm_find(rf, dlci);
	if (dlc == NULL || dlc->state != BTD_RFCOMM_DLC_CONNECTED)
		return;

	/* Succeeded: the peer's first credits after a SABM without a PN, or more when the share allows. */
	rfcomm_give_credits(rf, dlc);
}

/* Opens a DLC after its SABM and UA: this side's modem status goes out, the peer's is awaited within T2. */
static void
rfcomm_open_dlc(
	struct btd_rfcomm *rf,
	struct btd_rfcomm_dlc *dlc,
	uint64_t now)
{
	int error;

	/* Open, the modem status exchange begins. */
	dlc->state = BTD_RFCOMM_DLC_OPEN;
	dlc->deadline = now + BTD_RFCOMM_T2_MS;

	/* This side's status first (RFCOMM 1.2 section 6.3: before any data). */
	error = rfcomm_send_msc(rf, dlc->dlci);
	if (error != 0) {
		if (rf->failed == 0)
			rf->failed = BTD_RFCOMM_CLOSED_ERROR;
		return;
	}

	/* Succeeded: sent (or waiting in order). */
	dlc->msc_sent = 1;
}
