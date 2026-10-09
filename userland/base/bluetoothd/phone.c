/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's phone link, the transport part (ws197-p002, see phone.h and
 * plan/ws197/phase002/phase.md section 11).
 *
 * The router hands it the packets of the phone's link; it answers the
 * link's signalling, serves SDP on the channels the phone opens, runs one
 * RFCOMM session on PSM 3 (the phone's or its own), and queues the frames
 * it sends, moving them to the session as the link's share of the
 * controller's buffers allows (btd_phone_pump, each round of the daemon's
 * loop).  The session calls it from btd_session_input only, so it is never
 * entered twice.
 */

#include "userland/base/bluetoothd/phone.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <uapi/bluetooth.h>

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/*
 * The commands the phone link sends (Core 5.4 Vol 4 Part E 7.1, 7.5.7):
 * Create Connection, its Cancel, Accept and Reject Connection Request,
 * Link Key Request Reply and Negative Reply, Disconnect, Authentication
 * Requested, Set Connection Encryption, Read Encryption Key Size.
 */
#define PHONE_CREATE_CONNECTION		0x0405U
#define PHONE_DISCONNECT		0x0406U
#define PHONE_CANCEL_CONNECTION		0x0408U
#define PHONE_ACCEPT_CONNECTION		0x0409U
#define PHONE_REJECT_CONNECTION		0x040aU
#define PHONE_LINK_KEY_REPLY		0x040bU
#define PHONE_LINK_KEY_NEGATIVE		0x040cU
#define PHONE_AUTHENTICATION		0x0411U
#define PHONE_SET_ENCRYPTION		0x0413U
#define PHONE_READ_KEY_SIZE		0x1408U

/*
 * The events it takes: Connection Complete, Connection Request,
 * Disconnection Complete, Authentication Complete, Encryption Change (and
 * its second version), Link Key Request.
 */
#define PHONE_EVENT_CONNECTED		0x03U
#define PHONE_EVENT_REQUEST		0x04U
#define PHONE_EVENT_DISCONNECTED	0x05U
#define PHONE_EVENT_AUTHENTICATED	0x06U
#define PHONE_EVENT_ENCRYPTION		0x08U
#define PHONE_EVENT_KEY_REQUEST		0x17U
#define PHONE_EVENT_ENCRYPTION_V2	0x59U

/*
 * The statuses it tells apart (Core 5.4 Vol 1 Part F): Unknown Connection
 * Identifier (a cancelled page), Page Timeout, PIN or Key Missing,
 * Connection Already Exists, Command Disallowed, LMP Error Transaction
 * Collision, Different Transaction Collision.
 */
#define PHONE_STATUS_UNKNOWN		0x02U
#define PHONE_STATUS_PAGE_TIMEOUT	0x04U
#define PHONE_STATUS_KEY_MISSING	0x06U
#define PHONE_STATUS_EXISTS		0x0bU
#define PHONE_STATUS_DISALLOWED		0x0cU
#define PHONE_STATUS_COLLISION		0x23U
#define PHONE_STATUS_DIFFERENT_COLLISION 0x2aU

/*
 * The reasons: a disconnection by bluetoothd (Remote User Terminated, the
 * value a host sends), one for the link's security (Authentication
 * Failure), a refusal for limited resources, and what a disconnection's
 * event says: the supervision timeout, the phone's user, the phone's power
 * off, bluetoothd's own end.
 */
#define PHONE_REASON_USER		0x13U
#define PHONE_REASON_SECURITY		0x05U
#define PHONE_REASON_RESOURCES		0x0dU
#define PHONE_REASON_TIMEOUT		0x08U
#define PHONE_REASON_POWER_OFF		0x15U
#define PHONE_REASON_LOCAL_HOST		0x16U

/* Accept Connection Request's role: stay the peripheral (no role switch, section 5.3). */
#define PHONE_ROLE_PERIPHERAL		0x01U

/* Create Connection's packet types: DM1, DH1, DM3, DH3, DM5 and DH5, as the HID host's page. */
#define PHONE_PACKET_TYPES		0xcc18U

/* The PSMs of the phone's link: SDP and RFCOMM. */
#define PHONE_PSM_SDP			0x0001U
#define PHONE_PSM_RFCOMM		0x0003U

/* The link key types the phone link takes (Core Vol 4 Part E §7.7.24): authenticated P-192 and P-256. */
#define PHONE_KEY_P192_MITM		0x05U
#define PHONE_KEY_P256_MITM		0x08U

/* The only key size taken (KNOB), and the major device class of a phone (bits 12 to 8 of the class of device). */
#define PHONE_KEY_SIZE			16U
#define PHONE_MAJOR_PHONE		0x02U

/* How many frames of the controller's buffers the phone's link leaves to the other links (section 4.2). */
#define PHONE_BUFFERS_LEFT		2U

/* How often bluetoothd asks for the RFCOMM channel again after the phone refused it, and the least wait before it does (section 8.2). */
#define PHONE_RFCOMM_TRIES		3U
#define PHONE_RFCOMM_WAIT_MS		100U
#define PHONE_RFCOMM_SPREAD_MS		400U

/*
 * The waits between bluetoothd's pages, by step (ws197-p003 section 5.2):
 * at once, then 30 s doubling up to 10 minutes.  Fixed for the daemon's
 * life.
 */
static const uint64_t phone_backoff_ms[BTD_PHONE_BACKOFF_STEPS] = {
	0U, 30000U, 60000U, 120000U, 240000U, 480000U, 600000U
};

static int phone_accept(void *context, uint16_t handle, uint16_t psm, uint16_t *result, uint16_t *status);
static void phone_event(struct btd_phone *phone, const uint8_t *parameters, size_t length, uint8_t code);
static void phone_presence(struct btd_phone *phone, uint64_t now);
static void phone_stop(struct btd_phone *phone, const char *why, uint64_t now);
static void phone_schedule(struct btd_phone *phone, unsigned after, uint64_t now);
static void phone_page(struct btd_phone *phone, uint64_t now);
static void phone_cancel_page(struct btd_phone *phone, uint64_t now);
static void phone_request(struct btd_phone *phone, const uint8_t *parameters, size_t length);
static void phone_accept_request(struct btd_phone *phone, uint64_t now);
static void phone_reject(struct btd_phone *phone, const uint8_t *address, uint64_t now);
static void phone_connected(struct btd_phone *phone, const uint8_t *parameters, size_t length);
static void phone_connect_failed(struct btd_phone *phone, uint8_t status, uint64_t now);
static void phone_link_up(struct btd_phone *phone, uint16_t handle, uint8_t encrypted, uint64_t now);
static int phone_secure_auth(struct btd_phone *phone);
static void phone_secure_tick(struct btd_phone *phone, uint64_t now);
static void phone_key_request(struct btd_phone *phone, const uint8_t *parameters, size_t length);
static void phone_authenticated(struct btd_phone *phone, const uint8_t *parameters, size_t length);
static void phone_encryption(struct btd_phone *phone, const uint8_t *parameters, size_t length);
static void phone_key_size(struct btd_phone *phone);
static void phone_ready(struct btd_phone *phone, uint64_t now);
static void phone_profile_ready(struct btd_phone *phone);
static void phone_disconnected(struct btd_phone *phone, const uint8_t *parameters, size_t length);
static void phone_end_handle(struct btd_phone *phone, uint16_t handle);
static void phone_sdp_finish(struct btd_phone *phone, int error);
static void phone_sdp_done(struct btd_phone *phone, int error);
static void phone_pending_connect(struct btd_phone *phone);
static void phone_pending_failed(struct btd_phone *phone);
static void phone_acl(struct btd_phone *phone, const uint8_t *packet, size_t length);
static void phone_frame(struct btd_phone *phone, uint16_t cid, const uint8_t *payload, size_t length);
static void phone_signal(struct btd_phone *phone, const uint8_t *payload, size_t length);
static void phone_opened(struct btd_phone *phone, uint16_t cid);
static void phone_closed(struct btd_phone *phone, uint16_t cid);
static void phone_notice(struct btd_phone *phone, const uint8_t *packet);
static void phone_lost_frame(struct btd_phone *phone, uint16_t cid);
static int phone_carries(const struct btd_phone *phone, uint16_t cid);
static void phone_close_channel(struct btd_phone *phone, uint16_t cid);
static void phone_disconnect(struct btd_phone *phone, uint8_t reason, unsigned after);
static void phone_ended(struct btd_phone *phone, const char *why);
static int phone_send(struct btd_phone *phone, uint16_t cid, const uint8_t *payload, size_t length);
static void phone_flush(struct btd_phone *phone);
static void phone_sdps_input(struct btd_phone *phone, struct btd_phone_sdps *slot, const uint8_t *pdu, size_t length);
static void phone_sdp_send(struct btd_phone *phone);
static void phone_sdp_input(struct btd_phone *phone, const uint8_t *pdu, size_t length);
static void phone_rfcomm_open(struct btd_phone *phone);
static int phone_rf_send(void *context, const uint8_t *payload, size_t length);
static int phone_rf_accept(void *context, unsigned server_channel);
static void phone_rf_opened(void *context, unsigned dlci);
static void phone_rf_data(void *context, unsigned dlci, const uint8_t *data, size_t length);
static void phone_rf_writable(void *context, unsigned dlci);
static void phone_rf_closed(void *context, unsigned dlci, int reason);
static void phone_rf_ended(void *context, int reason);
static uint64_t phone_earlier(uint64_t earliest, uint64_t deadline);
static uint16_t phone_get16(const uint8_t *bytes);
static void phone_put16(uint8_t *bytes, uint16_t value);
static int phone_take_record(struct btd_phone *phone, const struct btd_pair_handoff *handoff, const char **why);
static void phone_limit(struct btd_phone *phone);
static const char *phone_link_name(const struct btd_phone *phone);
static void phone_profiles_text(unsigned profiles, char *text, size_t size);

/*
 * Prepares the phone link of a session: no record read, no seat, no
 * link, no profile.
 */
void
btd_phone_init(
	struct btd_phone *phone,
	struct btd_session *session,
	struct btd_router *router,
	struct btd_hid *hid,
	const struct btd_sdps_db *db,
	const char *keys_folder,
	const struct btd_phone_hooks *hooks)
{
	/* Nothing under way, and no record read yet. */
	memset(phone, 0, sizeof(*phone));
	phone->session = session;
	phone->router = router;
	phone->hid = hid;
	phone->db = db;
	phone->keys_folder = keys_folder;
	phone->hooks = *hooks;
	phone->state = BTD_PHONE_NONE;
	btd_l2cap_init(&phone->l2cap);
}

/* Sets the profile that uses the phone's link (ws197-p003 section 5.8). */
void
btd_phone_set_profile(
	struct btd_phone *phone,
	const struct btd_phone_profile *profile)
{
	/* Succeeded: its hooks kept. */
	phone->profile = *profile;
	phone->have_profile = 1;
}

/*
 * Tells the phone link who sits at the seat (ws197-p003 section 3.3; the
 * daemon looks every few seconds, and says none while Bluetooth is off):
 * the record's owner arriving starts the pages at once, leaving ends the
 * link.
 */
void
btd_phone_set_seat(
	struct btd_phone *phone,
	int have_seat,
	uid_t uid,
	uint64_t now)
{
	/* The seat as it is now. */
	phone->have_seat = have_seat;
	phone->seat_uid = uid;

	/* Succeeded: the presence follows. */
	phone_presence(phone, now);
}

/*
 * Takes the end of a sleep (ws197-p003 section 5.6): the pages start
 * again at once (a link that died under the sleep ends by its supervision
 * timeout).
 */
void
btd_phone_resume(
	struct btd_phone *phone,
	uint64_t now)
{
	/* Succeeded: the wait and the stop forgotten. */
	phone->backoff_step = 0U;
	phone->stopped = 0;
	phone->peer_closed = 0U;
	phone->next_page_at = now;
}

/*
 * Asks the phone's SDP server for the records of a service class (a
 * profile's query, ws197-p003 section 5.8): the end comes to the
 * profile's sdp_done.  Returns 0, ENOTCONN without a ready link, or EBUSY
 * while another query runs.
 */
int
btd_phone_sdp_query(
	struct btd_phone *phone,
	uint16_t uuid)
{
	uint8_t request[16];
	size_t length;
	int error;

	/* A ready link. */
	if (phone->state != BTD_PHONE_READY)
		return ENOTCONN;

	/* One query at a time. */
	if (phone->sdp_cid != 0U || phone->sdp_uuid != 0U)
		return EBUSY;

	/* SDP's channel first: its Connection Request (the query starts when it opens). */
	error = btd_l2cap_connect(&phone->l2cap, phone->handle, PHONE_PSM_SDP, request, sizeof(request), &length, &phone->sdp_cid);
	if (error != 0) {
		phone->sdp_cid = 0U;
		return EBUSY;
	}

	/* Succeeded: under way. */
	phone->sdp_uuid = uuid;
	(void)phone_send(phone, BTD_CID_SIGNALLING, request, length);
	return 0;
}

/*
 * Opens a DLC to a server channel of the phone (a profile's, ws197-p003
 * section 5.8): at once on the RFCOMM session there is, else once
 * bluetoothd's RFCOMM channel and session are up.  The profile hears
 * opened (or closed, or open_failed).  Returns 0, ENOTCONN without a ready
 * link, EBUSY when too many DLCs wait for the session, or RFCOMM's error.
 */
int
btd_phone_dlc_open(
	struct btd_phone *phone,
	unsigned server_channel,
	uint64_t now)
{
	unsigned dlci;
	int error;

	/* A ready link. */
	if (phone->state != BTD_PHONE_READY)
		return ENOTCONN;

	/* A session there is: the DLC now. */
	if (phone->rfcomm_active) {
		error = btd_rfcomm_connect(&phone->rfcomm, server_channel, now, &dlci);
		if (error != 0)
			return error;
		return 0;
	}

	/* Room among the DLCs that wait for the session. */
	if (phone->pending_count >= BTD_PHONE_PENDING_DLCS)
		return EBUSY;

	/* Succeeded: it waits, and the session is asked for (unless it is already). */
	phone->pending_channels[phone->pending_count] = server_channel;
	phone->pending_count++;
	phone->rfcomm_tries = 0U;
	phone_rfcomm_open(phone);
	return 0;
}

/* Writes on a DLC of the phone's RFCOMM session as far as its credits take it.  Returns 0 with *written, or RFCOMM's error. */
int
btd_phone_dlc_write(
	struct btd_phone *phone,
	unsigned dlci,
	const uint8_t *data,
	size_t length,
	size_t *written)
{
	int error;

	/* A session. */
	*written = 0U;
	if (!phone->rfcomm_active)
		return ENOTCONN;

	/* Written as far as it goes. */
	error = btd_rfcomm_write(&phone->rfcomm, dlci, data, length, written);
	if (error != 0)
		return error;

	/* Succeeded: what went is in written. */
	return 0;
}

/* Closes a DLC of the phone's RFCOMM session (the profile hears closed).  Returns 0 or RFCOMM's error. */
int
btd_phone_dlc_close(
	struct btd_phone *phone,
	unsigned dlci,
	uint64_t now)
{
	int error;

	/* A session. */
	if (!phone->rfcomm_active)
		return ENOTCONN;

	/* Closed. */
	error = btd_rfcomm_close(&phone->rfcomm, dlci, now);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Takes over the connection of a phone's pairing that succeeded (the
 * pairing's phone hook, section 7.4), when every condition holds in turn:
 * BR/EDR, an authenticated link key, a key of 16 bytes, a phone's Class of
 * Device (or none known: the user asked for a phone), a free phone slot
 * and room among the HID host's links.  Returns 1 when it took the link
 * (its route, its channels moved from the pairing's table and the held
 * ones answered), or 0 with *why.
 */
int
btd_phone_handoff(
	void *context,
	const struct btd_pair_handoff *handoff,
	const char **why)
{
	uint64_t now;
	struct btd_phone *phone;
	uint8_t answer[BTD_SIGNAL_MAX];
	unsigned moved;
	unsigned left;
	unsigned links;
	unsigned major;
	unsigned buffers;
	size_t length;
	int authenticated;
	int error;

	/* The phone link this hook serves. */
	phone = context;

	/* A phone is a BR/EDR device. */
	if (handoff->type != BTD_ADDRESS_BREDR) {
		*why = "le";
		return 0;
	}

	/* A key made with a number the user confirmed (Just Works is refused, p001 section 3.4). */
	authenticated = 0;
	if (handoff->bond->have_link_key && handoff->bond->link_key_type == PHONE_KEY_P192_MITM)
		authenticated = 1;
	if (handoff->bond->have_link_key && handoff->bond->link_key_type == PHONE_KEY_P256_MITM)
		authenticated = 1;
	if (!authenticated) {
		*why = "unauthenticated";
		return 0;
	}

	/* A key of 16 bytes (KNOB). */
	if (handoff->key_size != PHONE_KEY_SIZE) {
		*why = "key-size";
		return 0;
	}

	/* A phone's Class of Device, when the scan saw one. */
	if (handoff->have_class) {
		major = (unsigned)((handoff->class_of_device >> 8) & 0x1fU);
		if (major != PHONE_MAJOR_PHONE) {
			*why = "not-phone";
			return 0;
		}
	}

	/* One phone at a time. */
	if (phone->state != BTD_PHONE_NONE) {
		*why = "busy";
		return 0;
	}

	/* Room among the session's links: the HID host keeps one fewer while a phone is there (section 7.5). */
	links = btd_hid_link_count(phone->hid);
	if (links > BTD_PHONE_HID_LIMIT) {
		*why = "busy-links";
		return 0;
	}

	/* The owner's record, written before the link moves: a phone whose record cannot be written is not taken (ws197-p003 section 3.2). */
	error = phone_take_record(phone, handoff, why);
	if (error != 0)
		return 0;

	/* The link's route is the phone link's from now on. */
	error = btd_router_assign(phone->router, handoff->handle, BTD_OWNER_PHONE);
	if (error != 0) {
		*why = "busy";
		return 0;
	}

	/* The phone: its device, its link (encrypted with a key of 16 bytes by the pairing), who paired it. */
	memcpy(phone->address, handoff->address, BTD_ADDRESS_BYTES);
	phone->handle = handoff->handle;
	phone->uid = handoff->uid;
	phone->encrypted = 1;
	phone->key_size = handoff->key_size;
	phone->class_unknown = 0;
	if (!handoff->have_class)
		phone->class_unknown = 1;
	phone->queue_first = 0U;
	phone->queue_count = 0U;
	phone->state = BTD_PHONE_READY;

	/* The pairing's channels move to the phone link's table with their CIDs, and its frame being put together comes along. */
	btd_l2cap_init(&phone->l2cap);
	btd_l2cap_set_accept(&phone->l2cap, phone_accept, phone);
	btd_l2cap_move(handoff->l2cap, &phone->l2cap, handoff->handle, &moved, &left);
	phone->reassembly = *handoff->reassembly;
	phone->frame_cid = 0U;

	/* A channel whose slot was taken is refused, one command a frame (section 7.3). */
	while (left != 0U) {
		error = btd_l2cap_refuse_left(handoff->l2cap, handoff->handle, answer, sizeof(answer), &length);
		if (error != 0 || length == 0U)
			break;
		(void)phone_send(phone, BTD_CID_SIGNALLING, answer, length);
	}

	/* The link's share of the session: 8 frames waiting, and all but two of the controller's buffers (section 4.2). */
	buffers = 1U;
	if (phone->session->acl_pool.total > PHONE_BUFFERS_LEFT + 1U)
		buffers = phone->session->acl_pool.total - PHONE_BUFFERS_LEFT;
	(void)btd_session_set_link_limits(phone->session, phone->handle, BTD_SEND_PHONE_FRAMES, buffers);

	/* The HID host keeps a link free for the phone while its record is on (ws197-p003 section 3.4). */
	phone_limit(phone);

	/* The channels held Pending during the pairing are accepted now that the link is the phone link's. */
	error = btd_l2cap_answer_pending(&phone->l2cap, phone->handle, BTD_L2CAP_SUCCESS, answer, sizeof(answer), &length);
	if (error == 0 && length != 0U)
		(void)phone_send(phone, BTD_CID_SIGNALLING, answer, length);

	/* Ready since now; the presence follows the record just written (the owner paired it at the seat). */
	now = btd_now_ms();
	phone->ready_since = now;
	phone->inbound = 0;
	phone->page_outstanding = 0;
	phone_presence(phone, now);

	/* The owner not at the seat: the link ends (Q14); else the profile hears it is ready. */
	if (!phone->present)
		phone_disconnect(phone, PHONE_REASON_USER, BTD_PHONE_AFTER_NONE);
	else
		phone_profile_ready(phone);

	/* Succeeded: the link is the phone link's. */
	return 1;
}

/*
 * Tells the router whether the phone link wants a device's Connection
 * Request and Link Key Request (ws197-p003 section 5.3): the record's
 * phone while its owner is at the seat, in any state (the key is asked
 * again on a ready link; the request of a link there already is refused).
 */
int
btd_phone_wants(
	void *context,
	const uint8_t *address)
{
	struct btd_phone *phone;
	int same;

	/* The phone link this hook serves, its phone wanted. */
	phone = context;
	if (!phone->present)
		return 0;

	/* The record's phone. */
	same = memcmp(phone->record.address, address, BTD_ADDRESS_BYTES);
	if (same == 0)
		return 1;

	/* Another device. */
	return 0;
}

/*
 * Tells the router whether a connection to a device is the phone link's:
 * the phone's while its link is up.
 */
int
btd_phone_claims(
	void *context,
	const uint8_t *address)
{
	struct btd_phone *phone;
	int same;

	/* The phone link this hook serves, with a phone. */
	phone = context;
	if (phone->state == BTD_PHONE_NONE)
		return 0;

	/* The phone's address. */
	same = memcmp(phone->address, address, BTD_ADDRESS_BYTES);
	if (same == 0)
		return 1;

	/* Another device. */
	return 0;
}

/*
 * Takes a packet the router gives the phone link (its handler): an event
 * of its link, ACL data, or the session's notice of dropped packets.
 */
void
btd_phone_handle(
	void *context,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	struct btd_phone *phone;

	UNUSED_PARAMETER(session);

	/* The phone link this handler serves. */
	phone = context;

	/* ACL data of the link. */
	if (length >= 1U && packet[0] == BT_PACKET_ACL) {
		phone_acl(phone, packet, length);
		return;
	}

	/* The session's notice of packets it dropped on the link (section 3.5). */
	if (length == BTD_DROP_LENGTH && packet[0] == BTD_PACKET_DROP) {
		phone_notice(phone, packet);
		return;
	}

	/* Succeeded: an event, whose parameters' length the session checked. */
	if (length < 3U || packet[0] != BT_PACKET_EVENT)
		return;
	phone_event(phone, packet + 3, (size_t)packet[2], packet[1]);
}

/*
 * Moves what waits on to the session (each round of the daemon's loop):
 * RFCOMM's control frames and credits, OBEX's rest, and the queued frames
 * as far as the link's share of the controller's buffers allows.
 */
void
btd_phone_pump(
	struct btd_phone *phone)
{
	/* No phone. */
	if (phone->state == BTD_PHONE_NONE)
		return;

	/* RFCOMM's frames that waited for room. */
	if (phone->rfcomm_active)
		btd_rfcomm_pump(&phone->rfcomm);

	/* Succeeded: the queued frames, as far as the session takes them. */
	phone_flush(phone);
}

/*
 * Moves the phone link on with time: a page when its wait is over, the
 * deadlines of making and securing a link, the RFCOMM and SDP timers, a
 * stable link's wait forgotten, and bluetoothd's RFCOMM channel asked for
 * again after a refusal.
 */
void
btd_phone_tick(
	struct btd_phone *phone,
	uint64_t now)
{
	/* A refusal's own Connection Complete is awaited a while only. */
	if (phone->reject_pending && now >= phone->reject_until)
		phone->reject_pending = 0;

	/* No link: a page when the phone is wanted and its wait is over. */
	if (phone->state == BTD_PHONE_NONE) {
		if (phone->present && !phone->stopped && now >= phone->next_page_at)
			phone_page(phone, now);
		return;
	}

	/* A page that took too long is cancelled (its Connection Complete still comes, section 5.2). */
	if (phone->state == BTD_PHONE_PAGING && now >= phone->state_deadline) {
		phone_cancel_page(phone, now);
		return;
	}

	/* A cancel or an acceptance that took too long: no link, the next page after its wait. */
	if ((phone->state == BTD_PHONE_CANCELLING || phone->state == BTD_PHONE_ACCEPTING) && now >= phone->state_deadline) {
		phone->page_outstanding = 0;
		phone->state = BTD_PHONE_NONE;
		phone_schedule(phone, BTD_PHONE_AFTER_STEP, now);
		return;
	}

	/* The securing: its deadline, the phone's own encryption awaited, authentication asked again. */
	if (phone->state == BTD_PHONE_SECURING) {
		phone_secure_tick(phone, now);
		return;
	}

	/* A link ready long enough: the wait between pages starts over (section 5.2). */
	if (phone->state == BTD_PHONE_READY && phone->backoff_step != 0U && now >= phone->ready_since + BTD_PHONE_STABLE_MS) {
		phone->backoff_step = 0U;
		phone->peer_closed = 0U;
	}

	/* RFCOMM's timers. */
	if (phone->rfcomm_active)
		btd_rfcomm_tick(&phone->rfcomm, now);

	/* Succeeded: bluetoothd's RFCOMM channel asked for again when its wait is over. */
	if (phone->rfcomm_retry_at != 0U && now >= phone->rfcomm_retry_at) {
		phone->rfcomm_retry_at = 0U;
		phone_rfcomm_open(phone);
	}
}

/*
 * Gives the earliest deadline of the phone link (the daemon's loop wakes
 * then), or 0 when nothing waits.
 */
uint64_t
btd_phone_deadline(
	const struct btd_phone *phone)
{
	uint64_t earliest;
	uint64_t deadline;

	/* A refusal awaited. */
	earliest = 0U;
	if (phone->reject_pending)
		earliest = phone->reject_until;

	/* No link: the next page, while the phone is wanted. */
	if (phone->state == BTD_PHONE_NONE) {
		if (phone->present && !phone->stopped)
			earliest = phone_earlier(earliest, phone->next_page_at);
		return earliest;
	}

	/* The state's deadline. */
	if (phone->state == BTD_PHONE_PAGING ||
	    phone->state == BTD_PHONE_CANCELLING ||
	    phone->state == BTD_PHONE_ACCEPTING)
		earliest = phone_earlier(earliest, phone->state_deadline);

	/* The securing's deadlines. */
	if (phone->state == BTD_PHONE_SECURING) {
		earliest = phone_earlier(earliest, phone->secure_deadline);
		earliest = phone_earlier(earliest, phone->auth_again_at);
		if (phone->secure_step == BTD_PHONE_SECURE_WAIT_PEER)
			earliest = phone_earlier(earliest, phone->state_deadline);
	}

	/* A ready link's wait forgotten when it is stable. */
	if (phone->state == BTD_PHONE_READY && phone->backoff_step != 0U)
		earliest = phone_earlier(earliest, phone->ready_since + BTD_PHONE_STABLE_MS);

	/* RFCOMM's answers. */
	if (phone->rfcomm_active) {
		deadline = btd_rfcomm_deadline(&phone->rfcomm);
		earliest = phone_earlier(earliest, deadline);
	}

	/* Bluetoothd's RFCOMM channel's next request. */
	earliest = phone_earlier(earliest, phone->rfcomm_retry_at);

	/* Succeeded: the earliest, or 0. */
	return earliest;
}

/*
 * Forgets the phone's link because the controller went or was reset: no
 * command can be sent; the next page waits for the controller.
 */
void
btd_phone_lost(
	struct btd_phone *phone)
{
	/* No link. */
	if (phone->state == BTD_PHONE_NONE)
		return;

	/* Succeeded: ended. */
	phone->page_outstanding = 0;
	phone_ended(phone, "lost");
}

/*
 * Ends the phone's link (PHONE DROP, root): Disconnect; the end comes with
 * its Disconnection Complete.  Returns 0, or ENOTCONN when the device is
 * not the phone of a link.
 */
int
btd_phone_drop(
	struct btd_phone *phone,
	const uint8_t *address)
{
	int same;

	/* The phone of a link. */
	if (phone->state != BTD_PHONE_READY)
		return ENOTCONN;
	same = memcmp(phone->address, address, BTD_ADDRESS_BYTES);
	if (same != 0)
		return ENOTCONN;

	/* Succeeded: ended by the user (paged again after the next step's wait). */
	phone_disconnect(phone, PHONE_REASON_USER, BTD_PHONE_AFTER_STEP);
	return 0;
}

/*
 * Tells whether a connection is the phone's link (its packets are hidden
 * in the btsnoop record, section 11).
 */
int
btd_phone_owns(
	const struct btd_phone *phone,
	uint16_t handle)
{
	/* The phone's link while there is one. */
	if (phone->state != BTD_PHONE_NONE && handle == phone->handle)
		return 1;

	/* Another connection. */
	return 0;
}

/*
 * Reads the record of the phone used as a phone when the controller opens
 * (ws197-p003 section 3.1): records without a bond go, and the one valid
 * record is kept (or the first invalid one, which owns nothing but is
 * shown).  The HID host's limit follows.  Returns 0, ENXIO without the
 * controller's address, EEXIST when two records are valid (none is used
 * until one is forgotten), or the error of reading the folder.
 */
int
btd_phone_load(
	struct btd_phone *phone)
{
	struct btd_phonerec records[BTD_PHONEREC_LIST_MAX];
	unsigned count;
	unsigned valid_count;
	unsigned first_valid;
	unsigned index;
	int valid;
	int error;

	/* No record until one is read. */
	phone->have_record = 0;
	phone->record_valid = 0;

	/* The records are the controller's. */
	if (!phone->session->have_address) {
		phone_limit(phone);
		return ENXIO;
	}

	/* Records whose bond went while the daemon did not run go too (a failure leaves them, read below as invalid). */
	(void)btd_phonerec_prune(phone->keys_folder, phone->session->address);

	/* The records there are. */
	error = btd_phonerec_list(phone->keys_folder, phone->session->address, records, BTD_PHONEREC_LIST_MAX, &count);
	if (error != 0) {
		phone_limit(phone);
		return error;
	}

	/* The valid ones. */
	valid_count = 0U;
	first_valid = 0U;
	for (index = 0U; index < count; index++) {
		valid = btd_phonerec_valid(phone->keys_folder, phone->session->address, &records[index], phone->hooks.account, phone->hooks.context);
		if (!valid)
			continue;
		if (valid_count == 0U)
			first_valid = index;
		valid_count++;
	}

	/* Two phones each say they are the one: neither is used. */
	if (valid_count >= 2U) {
		phone_limit(phone);
		return EEXIST;
	}

	/* The valid record, or the first invalid one, kept. */
	if (valid_count == 1U) {
		phone->record = records[first_valid];
		phone->have_record = 1;
		phone->record_valid = 1;
	} else if (count != 0U) {
		phone->record = records[0];
		phone->have_record = 1;
	}

	/* Succeeded: the presence (and the HID host's limit) follows the record. */
	phone_presence(phone, btd_now_ms());
	return 0;
}

/*
 * Checks a pairing before it starts (ws197-p003 section 3.2): a phone's
 * pairing (phone=1) is the seat's user's alone; nobody but the owner and
 * root pairs a phone whose valid record is another's; the phone of a link
 * is not paired again under it.  Returns NULL when the pairing may start,
 * or why not: "phone-seat", "owned" or "busy".
 */
const char *
btd_phone_pair_check(
	struct btd_phone *phone,
	const uint8_t *address,
	uid_t uid,
	int phone_pairing,
	int seated)
{
	struct btd_phonerec record;
	int valid;
	int same;
	int error;

	/* The phone of a link is not paired again under it. */
	if (phone->state != BTD_PHONE_NONE) {
		same = memcmp(phone->address, address, BTD_ADDRESS_BYTES);
		if (same == 0)
			return "busy";
	}

	/* A phone's owner is the seat's user. */
	if (phone_pairing && !seated)
		return "phone-seat";

	/* Without the controller's address there are no records to read. */
	if (!phone->session->have_address)
		return NULL;

	/* The device's record, when it has one. */
	error = btd_phonerec_read(phone->keys_folder, phone->session->address, address, &record);
	if (error != 0)
		return NULL;

	/* An invalid record owns nothing. */
	valid = btd_phonerec_valid(phone->keys_folder, phone->session->address, &record, phone->hooks.account, phone->hooks.context);
	if (!valid)
		return NULL;

	/* Its owner, or root. */
	if (record.uid == uid || uid == 0)
		return NULL;

	/* Another's phone. */
	return "owned";
}

/*
 * Turns the phone link of a phone on or off (PHONE LINK, ws197-p003
 * section 3.2), and its profiles (profiles < 0 keeps them): the valid
 * record's owner and root alone.  Off ends the link.  Returns 0, ENOENT
 * when the device has no valid record, EPERM for anyone else, or the
 * error of writing the record.
 */
int
btd_phone_link_set(
	struct btd_phone *phone,
	const uint8_t *address,
	uid_t uid,
	int on,
	int profiles)
{
	uint64_t now;
	struct btd_phonerec record;
	int valid;
	int error;

	/* The records are the controller's. */
	if (!phone->session->have_address)
		return ENOENT;

	/* The device's record. */
	error = btd_phonerec_read(phone->keys_folder, phone->session->address, address, &record);
	if (error != 0)
		return ENOENT;

	/* A valid one: an invalid record owns nothing to switch. */
	valid = btd_phonerec_valid(phone->keys_folder, phone->session->address, &record, phone->hooks.account, phone->hooks.context);
	if (!valid)
		return ENOENT;

	/* Its owner, or root. */
	if (record.uid != uid && uid != 0)
		return EPERM;

	/* The switch and the profiles, written. */
	record.enabled = 0;
	if (on)
		record.enabled = 1;
	if (profiles >= 0)
		record.profiles = (unsigned)profiles & BTD_PHONEREC_PROFILES;
	error = btd_phonerec_write(phone->keys_folder, phone->session->address, &record);
	if (error != 0)
		return error;

	/* Kept, and the HID host's limit follows. */
	phone->record = record;
	phone->have_record = 1;
	phone->record_valid = 1;

	/* On pages at once, whatever stopped the pages before (section 5.5). */
	now = btd_now_ms();
	if (on) {
		phone->stopped = 0;
		phone->backoff_step = 0U;
		phone->peer_closed = 0U;
		phone->next_page_at = now;
	}

	/* The presence follows (off ends the link). */
	phone_presence(phone, now);

	/* Succeeded. */
	return 0;
}

/*
 * Removes the record of a phone before its bond is forgotten (FORGET,
 * ws197-p003 section 3.2): a valid record by its owner and root alone, an
 * invalid one by anyone the daemon lets forget.  The link ends.  Returns
 * 0, ENOENT when there was none (the bond is forgotten as ever), EPERM,
 * or the error of removing the file.
 */
int
btd_phone_forget(
	struct btd_phone *phone,
	const uint8_t *address,
	uid_t uid)
{
	struct btd_phonerec record;
	int valid;
	int same;
	int error;

	/* The records are the controller's. */
	if (!phone->session->have_address)
		return ENOENT;

	/* The device's record (a malformed one is invalid). */
	error = btd_phonerec_read(phone->keys_folder, phone->session->address, address, &record);
	if (error == ENOENT)
		return ENOENT;
	valid = 0;
	if (error == 0)
		valid = btd_phonerec_valid(phone->keys_folder, phone->session->address, &record, phone->hooks.account, phone->hooks.context);

	/* A valid record by its owner, or root. */
	if (valid && record.uid != uid && uid != 0)
		return EPERM;

	/* The file goes. */
	error = btd_phonerec_forget(phone->keys_folder, phone->session->address, address);
	if (error != 0)
		return error;

	/* The record kept is gone with it. */
	same = memcmp(phone->record.address, address, BTD_ADDRESS_BYTES);
	if (phone->have_record && same == 0) {
		phone->have_record = 0;
		phone->record_valid = 0;
	}

	/* The presence follows (the forgotten phone's link ends). */
	phone_presence(phone, btd_now_ms());

	/* Succeeded: forgotten. */
	return 0;
}

/*
 * Writes the line of PHONE SHOW (ws197-p003 section 9.2): the whole state
 * to the owner and root, the address and the switch to another the daemon
 * lets change things.  Returns 0, or ENOENT when there is no record or
 * nothing to show to that uid.
 */
int
btd_phone_show(
	const struct btd_phone *phone,
	uid_t uid,
	int permitted,
	char *line,
	size_t size)
{
	char address[24];
	char owner[16];
	char profiles[8];
	int whole;
	int mine;

	/* A record. */
	if (!phone->have_record)
		return ENOENT;
	btd_format_address(phone->record.address, address, sizeof(address));

	/* Whose: the owner's (of a valid record), and root sees it whole too. */
	mine = 0;
	if (phone->record_valid && phone->record.uid == uid)
		mine = 1;
	whole = mine;
	if (uid == 0)
		whole = 1;

	/* Anyone else the daemon lets change things sees the address and the switch. */
	if (!whole) {
		if (!permitted)
			return ENOENT;
		(void)snprintf(line, size, "PHONE address=%s mine=0 enabled=%d", address, phone->record.enabled);
		return 0;
	}

	/* The owner, or "invalid". */
	(void)snprintf(owner, sizeof(owner), "%lu", (unsigned long)phone->record.uid);
	if (!phone->record_valid)
		(void)snprintf(owner, sizeof(owner), "%s", "invalid");
	phone_profiles_text(phone->record.profiles, profiles, sizeof(profiles));

	/* Succeeded: the whole line. */
	(void)snprintf(line,
		       size,
		       "PHONE address=%s owner=%s mine=%d enabled=%d profiles=%s present=0 link=%s messages=off send=0 notify=0",
		       address,
		       owner,
		       mine,
		       phone->record.enabled,
		       profiles,
		       phone_link_name(phone));
	return 0;
}

/*
 * Answers a channel the phone asks for (the accept hook of the phone
 * link's table): SDP while a server is free, RFCOMM while no RFCOMM
 * channel is open or asked for (one session a link, section 8.2), each
 * Pending until the link is encrypted with a key of 16 bytes; anything
 * else is refused as PSM not supported.
 */
static int
phone_accept(
	void *context,
	uint16_t handle,
	uint16_t psm,
	uint16_t *result,
	uint16_t *status)
{
	struct btd_phone *phone;
	const struct btd_channel *channel;
	unsigned servers;
	unsigned sessions;
	unsigned index;

	/* The phone link whose table asks, and the link's channels of SDP served and of RFCOMM. */
	phone = context;
	servers = 0U;
	sessions = 0U;
	for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
		channel = &phone->l2cap.channels[index];
		if (channel->state == BTD_CHANNEL_FREE || channel->handle != handle)
			continue;
		if (channel->psm == PHONE_PSM_SDP && channel->inbound)
			servers++;
		if (channel->psm == PHONE_PSM_RFCOMM)
			sessions++;
	}

	/* Another PSM: not supported. */
	if (psm != PHONE_PSM_SDP && psm != PHONE_PSM_RFCOMM)
		return 1;

	/* No room: SDP's servers in use. */
	*status = 0x0000U;
	if (psm == PHONE_PSM_SDP && servers >= BTD_PHONE_SDP_SERVERS) {
		*result = BTD_L2CAP_NO_RESOURCES;
		return 0;
	}

	/* No room: an RFCOMM channel there already (the phone's request crossed ours). */
	if (psm == PHONE_PSM_RFCOMM && sessions != 0U) {
		*result = BTD_L2CAP_NO_RESOURCES;
		return 0;
	}

	/* A link not secured yet: Pending. */
	if (!phone->encrypted || phone->key_size != PHONE_KEY_SIZE) {
		*result = BTD_L2CAP_PENDING;
		return 0;
	}

	/* Succeeded: accepted. */
	*result = BTD_L2CAP_SUCCESS;
	return 0;
}

/* Takes an event of the phone's link or device: a connection asked for, made or ended, its key asked, its authentication and encryption. */
static void
phone_event(
	struct btd_phone *phone,
	const uint8_t *parameters,
	size_t length,
	uint8_t code)
{
	/* Each event the phone link knows; anything else is passed over. */
	switch (code) {
	case PHONE_EVENT_REQUEST:
		phone_request(phone, parameters, length);
		break;
	case PHONE_EVENT_CONNECTED:
		phone_connected(phone, parameters, length);
		break;
	case PHONE_EVENT_KEY_REQUEST:
		phone_key_request(phone, parameters, length);
		break;
	case PHONE_EVENT_AUTHENTICATED:
		phone_authenticated(phone, parameters, length);
		break;
	case PHONE_EVENT_ENCRYPTION:
	case PHONE_EVENT_ENCRYPTION_V2:
		phone_encryption(phone, parameters, length);
		break;
	case PHONE_EVENT_DISCONNECTED:
		phone_disconnected(phone, parameters, length);
		break;
	default:
		break;
	}
}

/*
 * Follows the presence of the record's owner at the seat (section 3.3):
 * arriving, the pages start at once and page scan is wanted; leaving, the
 * link ends (Q14) and page scan is no longer wanted.  The HID host's
 * limit follows the record.
 */
static void
phone_presence(
	struct btd_phone *phone,
	uint64_t now)
{
	struct btd_linkmgr *linkmgr;
	int present;

	/* Wanted: a valid record that is on, its owner at the seat. */
	present = 0;
	if (phone->have_record &&
	    phone->record_valid &&
	    phone->record.enabled &&
	    phone->have_seat &&
	    phone->seat_uid == phone->record.uid)
		present = 1;

	/* The HID host's limit follows the record. */
	phone_limit(phone);

	/* No change. */
	if (present == phone->present)
		return;
	phone->present = present;

	/* Page scan, wanted while the owner is there. */
	linkmgr = phone->router->linkmgr;
	if (linkmgr != NULL)
		(void)btd_linkmgr_want_scan(linkmgr, BTD_LINKMGR_PHONE, present);

	/* Arriving: the pages start at once, and a link ready meanwhile goes to the profile. */
	if (present) {
		phone->backoff_step = 0U;
		phone->stopped = 0;
		phone->peer_closed = 0U;
		phone->next_page_at = now;
		phone_profile_ready(phone);
		return;
	}

	/* Succeeded: leaving ends the link. */
	phone_stop(phone, "absent", now);
}

/*
 * Ends whatever link there is with no page after it (the owner left, the
 * record was switched off or forgotten): a page is cancelled, a
 * connection being made is ended when it comes, a link is disconnected.
 */
static void
phone_stop(
	struct btd_phone *phone,
	const char *why,
	uint64_t now)
{
	/* The reason, and no page after it. */
	phone->why = why;
	phone->after = BTD_PHONE_AFTER_NONE;

	/* A page: cancelled, its Connection Complete ends it. */
	if (phone->state == BTD_PHONE_PAGING) {
		phone->stop_wanted = 1;
		phone_cancel_page(phone, now);
		return;
	}

	/* A connection on its way: ended when it comes. */
	if (phone->state == BTD_PHONE_CANCELLING || phone->state == BTD_PHONE_ACCEPTING) {
		phone->stop_wanted = 1;
		return;
	}

	/* Succeeded: a link (securing or ready) is disconnected. */
	phone_disconnect(phone, PHONE_REASON_USER, BTD_PHONE_AFTER_NONE);
}

/*
 * Sets when the next page may go after a link or a page ended (section
 * 5.5): none, the wait of the next step, or the longest wait.
 */
static void
phone_schedule(
	struct btd_phone *phone,
	unsigned after,
	uint64_t now)
{
	/* No page after it: until the next sign (the owner at the seat again, a sleep's end, PHONE LINK on). */
	if (after == BTD_PHONE_AFTER_NONE) {
		phone->stopped = 1;
		return;
	}

	/* The longest wait. */
	if (after == BTD_PHONE_AFTER_LONG)
		phone->backoff_step = BTD_PHONE_BACKOFF_STEPS - 1U;

	/* Succeeded: this step's wait, and the next step for the next time. */
	phone->next_page_at = now + phone_backoff_ms[phone->backoff_step];
	if (phone->backoff_step + 1U < BTD_PHONE_BACKOFF_STEPS)
		phone->backoff_step++;
}

/*
 * Pages the phone (section 5.2): one BR/EDR page of the controller at a
 * time (the link manager), and not while the HID host uses every link it
 * may; either waits a little, its step not counted.
 */
static void
phone_page(
	struct btd_phone *phone,
	uint64_t now)
{
	struct btd_linkmgr *linkmgr;
	uint8_t parameters[13];
	unsigned links;
	int error;

	/* A link left by the HID host. */
	links = btd_hid_link_count(phone->hid);
	if (links > BTD_PHONE_HID_LIMIT) {
		phone->why = "busy-links";
		phone->next_page_at = now + BTD_PHONE_WAIT_MS;
		return;
	}

	/* The controller's one page. */
	linkmgr = phone->router->linkmgr;
	if (linkmgr != NULL) {
		error = btd_linkmgr_page_begin(linkmgr, BTD_LINKMGR_PHONE, phone->record.address, now);
		if (error != 0) {
			phone->next_page_at = now + BTD_PHONE_WAIT_MS;
			return;
		}
	}

	/* The phone, its address the record's. */
	memcpy(phone->address, phone->record.address, BTD_ADDRESS_BYTES);
	phone->uid = phone->record.uid;
	phone->inbound = 0;
	phone->stop_wanted = 0;
	phone->page_outstanding = 1;
	phone->state = BTD_PHONE_PAGING;
	phone->state_deadline = now + BTD_PHONE_PAGE_MS;

	/* Create Connection: the address, DM1 to DH5, R1, no clock offset, a role switch allowed (as the HID host's page). */
	memset(parameters, 0, sizeof(parameters));
	memcpy(parameters, phone->address, BTD_ADDRESS_BYTES);
	phone_put16(parameters + 6, PHONE_PACKET_TYPES);
	parameters[8] = 0x01U;
	parameters[9] = 0x00U;
	phone_put16(parameters + 10, 0x0000U);
	parameters[12] = 0x01U;
	error = btd_session_command(phone->session, PHONE_CREATE_CONNECTION, parameters, sizeof(parameters));
	if (error == 0)
		return;

	/* Refused at once: no page out, the next after its wait. */
	if (linkmgr != NULL)
		btd_linkmgr_page_end(linkmgr, BTD_LINKMGR_PHONE, phone->address);
	phone->page_outstanding = 0;
	phone->state = BTD_PHONE_NONE;
	phone->why = "unreachable";
	phone_schedule(phone, BTD_PHONE_AFTER_STEP, now);
}

/*
 * Cancels bluetoothd's page (its guard ran out, or the owner left,
 * section 5.2): Create Connection Cancel; the page's Connection Complete
 * still comes and ends the cancel (its status only logged: the command
 * waits synchronously, so the Connection Complete is handed after it).
 */
static void
phone_cancel_page(
	struct btd_phone *phone,
	uint64_t now)
{
	/* Cancelled, awaited a while. */
	(void)btd_session_command(phone->session, PHONE_CANCEL_CONNECTION, phone->address, BTD_ADDRESS_BYTES);
	phone->state = BTD_PHONE_CANCELLING;

	/* Succeeded: its Connection Complete awaited. */
	phone->state_deadline = now + BTD_PHONE_CANCEL_MS;
}

/*
 * Takes Connection Request (address, class, link type) of the record's
 * phone (section 5.3): accepted with no role switch when there is no
 * link; when bluetoothd's page crosses it, the page is cancelled first and
 * the phone's accepted, or refused when the page made its link already;
 * refused while a link is there or the HID host uses every link it may.
 */
static void
phone_request(
	struct btd_phone *phone,
	const uint8_t *parameters,
	size_t length)
{
	uint64_t now;
	unsigned links;
	int same;
	int error;

	/* A whole event of the record's phone. */
	if (length < 10U)
		return;
	same = memcmp(parameters, phone->record.address, BTD_ADDRESS_BYTES);
	if (same != 0)
		return;
	now = btd_now_ms();

	/* No link: accepted, unless the HID host uses every link it may. */
	if (phone->state == BTD_PHONE_NONE) {
		links = btd_hid_link_count(phone->hid);
		if (links > BTD_PHONE_HID_LIMIT) {
			phone_reject(phone, parameters, now);
			return;
		}
		memcpy(phone->address, parameters, BTD_ADDRESS_BYTES);
		phone->uid = phone->record.uid;
		phone->stop_wanted = 0;
		phone->page_outstanding = 0;
		phone_accept_request(phone, now);
		return;
	}

	/* Bluetoothd's page crossing it: cancelled first; a page that made its link already keeps it. */
	if (phone->state == BTD_PHONE_PAGING) {
		error = btd_session_command(phone->session, PHONE_CANCEL_CONNECTION, phone->address, BTD_ADDRESS_BYTES);
		if (error != 0 && phone->session->status == PHONE_STATUS_EXISTS) {
			phone_reject(phone, parameters, now);
			return;
		}
		phone_accept_request(phone, now);
		return;
	}

	/* A cancelled page: the phone's connection accepted (the page's Connection Complete still comes). */
	if (phone->state == BTD_PHONE_CANCELLING) {
		phone_accept_request(phone, now);
		return;
	}

	/* Succeeded: refused while a link is there or on its way. */
	phone_reject(phone, parameters, now);
}

/* Accepts the phone's connection with no role switch (the phone is often another device's central, section 5.3). */
static void
phone_accept_request(
	struct btd_phone *phone,
	uint64_t now)
{
	uint8_t accept[BTD_ADDRESS_BYTES + 1U];
	int error;

	/* Accept Connection Request: the address, the peripheral's role kept. */
	memcpy(accept, phone->address, BTD_ADDRESS_BYTES);
	accept[BTD_ADDRESS_BYTES] = PHONE_ROLE_PERIPHERAL;
	error = btd_session_command(phone->session, PHONE_ACCEPT_CONNECTION, accept, sizeof(accept));
	if (error != 0) {
		phone->state = BTD_PHONE_NONE;
		phone_schedule(phone, BTD_PHONE_AFTER_STEP, now);
		return;
	}

	/* Succeeded: its Connection Complete awaited. */
	phone->inbound = 1;
	phone->state = BTD_PHONE_ACCEPTING;
	phone->state_deadline = now + BTD_PHONE_ACCEPT_MS;
}

/*
 * Refuses a Connection Request for limited resources; the refusal's own
 * Connection Complete (Core 5.4 Vol 4 Part E 7.1.9, its status not given
 * there) is awaited and passed over once (section 5.3, review-2 n1).
 */
static void
phone_reject(
	struct btd_phone *phone,
	const uint8_t *address,
	uint64_t now)
{
	uint8_t reject[BTD_ADDRESS_BYTES + 1U];

	/* Reject Connection Request: the address, limited resources. */
	memcpy(reject, address, BTD_ADDRESS_BYTES);
	reject[BTD_ADDRESS_BYTES] = PHONE_REASON_RESOURCES;
	(void)btd_session_command(phone->session, PHONE_REJECT_CONNECTION, reject, sizeof(reject));

	/* Succeeded: its Connection Complete awaited. */
	memcpy(phone->reject_address, address, BTD_ADDRESS_BYTES);
	phone->reject_pending = 1;
	phone->reject_until = now + BTD_PHONE_REJECT_MS;
}

/*
 * Takes Connection Complete (status, handle, address, link type,
 * encryption) of the record's phone (sections 5.2 to 5.4): a refusal's
 * own and a cancelled page's are passed over; a failure ends the page or
 * the acceptance; a success makes the link the phone link's and starts
 * securing it (or ends it at once when it is no longer wanted).
 */
static void
phone_connected(
	struct btd_phone *phone,
	const uint8_t *parameters,
	size_t length)
{
	uint64_t now;
	uint8_t status;
	int refusal;
	int same;

	/* A whole event of the phone. */
	if (length < 11U)
		return;
	same = memcmp(parameters + 3, phone->address, BTD_ADDRESS_BYTES);
	if (same != 0 || phone->state == BTD_PHONE_NONE)
		return;
	status = parameters[0];
	now = btd_now_ms();

	/* A refusal's own Connection Complete: passed over once. */
	refusal = memcmp(parameters + 3, phone->reject_address, BTD_ADDRESS_BYTES);
	if (status != 0U && phone->reject_pending && refusal == 0) {
		phone->reject_pending = 0;
		return;
	}

	/* A failure: a cancelled page's while the phone's connection is accepted or made, else the end of the page or the acceptance. */
	if (status != 0U) {
		phone_connect_failed(phone, status, now);
		return;
	}

	/* A success while no connection was on its way (a second link of the phone): ended. */
	if (phone->state != BTD_PHONE_PAGING &&
	    phone->state != BTD_PHONE_CANCELLING &&
	    phone->state != BTD_PHONE_ACCEPTING) {
		phone_end_handle(phone, phone_get16(parameters + 1));
		return;
	}

	/* Bluetoothd's page made it, or it was the phone's: no page out any more once a page's state saw it. */
	if (phone->state != BTD_PHONE_ACCEPTING)
		phone->page_outstanding = 0;

	/* Succeeded: the link, secured next. */
	phone_link_up(phone, (uint16_t)(phone_get16(parameters + 1) & 0x0fffU), parameters[10], now);
}

/*
 * Takes a failed Connection Complete of the phone: the cancelled page's
 * (Unknown Connection Identifier, Page Timeout, Connection Already
 * Exists) while the phone's connection is accepted or made is passed over;
 * else the page or the acceptance ends and the next page waits.
 */
static void
phone_connect_failed(
	struct btd_phone *phone,
	uint8_t status,
	uint64_t now)
{
	int pages_end;

	/* The cancelled page's, while the phone's own connection goes on. */
	pages_end = 0;
	if (status == PHONE_STATUS_UNKNOWN || status == PHONE_STATUS_PAGE_TIMEOUT || status == PHONE_STATUS_EXISTS)
		pages_end = 1;
	if (phone->page_outstanding && pages_end && phone->state != BTD_PHONE_PAGING && phone->state != BTD_PHONE_CANCELLING) {
		phone->page_outstanding = 0;
		return;
	}

	/* Not a connection on its way. */
	if (phone->state != BTD_PHONE_PAGING &&
	    phone->state != BTD_PHONE_CANCELLING &&
	    phone->state != BTD_PHONE_ACCEPTING)
		return;

	/* Succeeded: no link, the next page after its wait (or none, when it was stopped). */
	phone->page_outstanding = 0;
	phone->state = BTD_PHONE_NONE;
	phone->why = "unreachable";
	if (phone->stop_wanted) {
		phone_schedule(phone, BTD_PHONE_AFTER_NONE, now);
		return;
	}
	phone_schedule(phone, BTD_PHONE_AFTER_STEP, now);
}

/*
 * Takes the phone's new ACL link (section 5.4): its route, its share of
 * the session, its channels' table; then the securing (a link no longer
 * wanted ends at once).
 */
static void
phone_link_up(
	struct btd_phone *phone,
	uint16_t handle,
	uint8_t encrypted,
	uint64_t now)
{
	unsigned buffers;
	int error;

	/* The route is the phone link's (the router gave it already when it claimed the connection; this makes sure). */
	phone->handle = handle;
	(void)btd_router_assign(phone->router, handle, BTD_OWNER_PHONE);

	/* The link's share of the session (section 4.2 of p002). */
	buffers = 1U;
	if (phone->session->acl_pool.total > PHONE_BUFFERS_LEFT + 1U)
		buffers = phone->session->acl_pool.total - PHONE_BUFFERS_LEFT;
	(void)btd_session_set_link_limits(phone->session, handle, BTD_SEND_PHONE_FRAMES, buffers);

	/* The link's channels and frame, none yet. */
	btd_l2cap_init(&phone->l2cap);
	btd_l2cap_set_accept(&phone->l2cap, phone_accept, phone);
	memset(&phone->reassembly, 0, sizeof(phone->reassembly));
	phone->frame_cid = 0U;
	phone->queue_first = 0U;
	phone->queue_count = 0U;
	phone->encrypted = 0;
	if (encrypted != 0U)
		phone->encrypted = 1;
	phone->key_size = 0U;
	phone->class_unknown = 0;

	/* No longer wanted: ended at once. */
	phone->state = BTD_PHONE_SECURING;
	if (phone->stop_wanted) {
		phone_disconnect(phone, PHONE_REASON_USER, BTD_PHONE_AFTER_NONE);
		return;
	}

	/* The securing: the phone's own encryption awaited a while when it connected, else authentication at once. */
	phone->secure_deadline = now + BTD_PHONE_SECURE_MS;
	phone->auth_again_at = 0U;
	phone->auth_retried = 0;
	if (phone->inbound) {
		phone->secure_step = BTD_PHONE_SECURE_WAIT_PEER;
		phone->state_deadline = now + BTD_PHONE_PEER_MS;
		return;
	}

	/* Succeeded: authentication asked for. */
	error = phone_secure_auth(phone);
	if (error != 0)
		phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_STEP);
}

/* Asks for the link's authentication (Authentication Requested): the controller asks for the key.  Returns 0, or the command's error (Command Disallowed sends the securing back to wait for the phone). */
static int
phone_secure_auth(
	struct btd_phone *phone)
{
	uint8_t handle[2];
	int error;

	/* Authentication Requested. */
	phone->secure_step = BTD_PHONE_SECURE_AUTH;
	phone_put16(handle, phone->handle);
	error = btd_session_command(phone->session, PHONE_AUTHENTICATION, handle, sizeof(handle));

	/* The phone started its own already: its encryption awaited a while more. */
	if (error != 0 && phone->session->status == PHONE_STATUS_DISALLOWED) {
		phone->secure_step = BTD_PHONE_SECURE_WAIT_PEER;
		phone->state_deadline = btd_now_ms() + BTD_PHONE_PEER_MS;
		return 0;
	}

	/* Refused. */
	if (error != 0)
		return error;

	/* Succeeded: its Authentication Complete awaited. */
	return 0;
}

/* Moves the securing on with time (section 5.4): its deadline, the phone's own encryption awaited, authentication asked again after a collision. */
static void
phone_secure_tick(
	struct btd_phone *phone,
	uint64_t now)
{
	int error;

	/* The whole securing took too long. */
	if (now >= phone->secure_deadline) {
		phone->why = "security";
		phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_STEP);
		return;
	}

	/* The phone did not secure the link itself in time: authentication asked for. */
	if (phone->secure_step == BTD_PHONE_SECURE_WAIT_PEER && now >= phone->state_deadline) {
		error = phone_secure_auth(phone);
		if (error != 0)
			phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_STEP);
		return;
	}

	/* Succeeded: authentication asked again after a collision. */
	if (phone->auth_again_at != 0U && now >= phone->auth_again_at) {
		phone->auth_again_at = 0U;
		error = phone_secure_auth(phone);
		if (error != 0)
			phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_STEP);
	}
}

/*
 * Answers Link Key Request (address) of the record's phone (section 5.3):
 * the bond's key when it was made with a number the user confirmed, else
 * none; the key is not kept.
 */
static void
phone_key_request(
	struct btd_phone *phone,
	const uint8_t *parameters,
	size_t length)
{
	struct btd_bond bond;
	uint8_t reply[BTD_ADDRESS_BYTES + 16U];
	int authenticated;
	int same;
	int error;

	/* A whole event. */
	if (length < BTD_ADDRESS_BYTES)
		return;

	/* The record's phone, its record valid, its bond there. */
	error = ENOENT;
	same = memcmp(parameters, phone->record.address, BTD_ADDRESS_BYTES);
	if (same == 0 && phone->have_record && phone->record_valid)
		error = btd_keys_read(phone->keys_folder, phone->session->address, parameters, BTD_ADDRESS_BREDR, &bond);

	/* Its key made with a number the user confirmed. */
	authenticated = 0;
	if (error == 0 && bond.have_link_key && bond.link_key_type == PHONE_KEY_P192_MITM)
		authenticated = 1;
	if (error == 0 && bond.have_link_key && bond.link_key_type == PHONE_KEY_P256_MITM)
		authenticated = 1;

	/* None: Negative Reply. */
	if (!authenticated) {
		memset(&bond, 0, sizeof(bond));
		(void)btd_session_command(phone->session, PHONE_LINK_KEY_NEGATIVE, parameters, BTD_ADDRESS_BYTES);
		return;
	}

	/* Succeeded: Link Key Request Reply, and the key forgotten. */
	memcpy(reply, parameters, BTD_ADDRESS_BYTES);
	memcpy(reply + BTD_ADDRESS_BYTES, bond.link_key, sizeof(bond.link_key));
	(void)btd_session_command(phone->session, PHONE_LINK_KEY_REPLY, reply, sizeof(reply));
	memset(&bond, 0, sizeof(bond));
	memset(reply, 0, sizeof(reply));
}

/*
 * Takes Authentication Complete (status, handle) of the link being
 * secured (section 5.4): encryption next; a collision asks again once; a
 * key the phone no longer has stops the pages; any other failure ends the
 * link.  Once encrypted, a failure is passed over.
 */
static void
phone_authenticated(
	struct btd_phone *phone,
	const uint8_t *parameters,
	size_t length)
{
	uint8_t encryption[3];
	uint16_t handle;
	int error;

	/* A whole event of the link being secured, not encrypted yet. */
	if (length < 3U || phone->state != BTD_PHONE_SECURING)
		return;
	handle = (uint16_t)(phone_get16(parameters + 1) & 0x0fffU);
	if (handle != phone->handle || phone->encrypted)
		return;

	/* A collision with the phone's own: asked again a little later, once. */
	if ((parameters[0] == PHONE_STATUS_COLLISION || parameters[0] == PHONE_STATUS_DIFFERENT_COLLISION) && !phone->auth_retried) {
		phone->auth_retried = 1;
		phone->auth_again_at = btd_now_ms() + BTD_PHONE_AUTH_AGAIN_MS;
		return;
	}

	/* The phone forgot the bond: no page until it is paired again. */
	if (parameters[0] == PHONE_STATUS_KEY_MISSING) {
		phone->why = "key-missing";
		phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_NONE);
		return;
	}

	/* Any other failure. */
	if (parameters[0] != 0U) {
		phone->why = "security";
		phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_STEP);
		return;
	}

	/* Succeeded: Set Connection Encryption, on. */
	phone->secure_step = BTD_PHONE_SECURE_ENCRYPT;
	phone_put16(encryption, phone->handle);
	encryption[2] = 0x01U;
	error = btd_session_command(phone->session, PHONE_SET_ENCRYPTION, encryption, sizeof(encryption));
	if (error != 0) {
		phone->why = "security";
		phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_STEP);
	}
}

/*
 * Takes Encryption Change (status, handle, enabled) of the phone's link:
 * while securing, encryption on leads to the key's size whatever the step
 * (it uses the bond's key the link key request gave); on a ready link,
 * encryption off ends it (the phone's data is not sent in the clear).
 */
static void
phone_encryption(
	struct btd_phone *phone,
	const uint8_t *parameters,
	size_t length)
{
	uint16_t handle;

	/* A whole event of the phone's link. */
	if (length < 4U || phone->state == BTD_PHONE_NONE)
		return;
	handle = (uint16_t)(phone_get16(parameters + 1) & 0x0fffU);
	if (handle != phone->handle)
		return;

	/* A ready link whose encryption went off. */
	if (phone->state == BTD_PHONE_READY) {
		if (parameters[0] == 0x00U && parameters[3] == 0x00U) {
			phone->encrypted = 0;
			phone->why = "security";
			phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_STEP);
		}
		return;
	}

	/* Only while securing. */
	if (phone->state != BTD_PHONE_SECURING)
		return;

	/* The phone forgot the bond. */
	if (parameters[0] == PHONE_STATUS_KEY_MISSING) {
		phone->why = "key-missing";
		phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_NONE);
		return;
	}

	/* Not encrypted. */
	if (parameters[0] != 0x00U || parameters[3] == 0x00U) {
		phone->why = "security";
		phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_STEP);
		return;
	}

	/* Succeeded: encrypted, its key's size next. */
	phone->encrypted = 1;
	phone_key_size(phone);
}

/* Reads the encryption key's size (KNOB: 16 bytes only): the link is ready, or ends. */
static void
phone_key_size(
	struct btd_phone *phone)
{
	uint8_t handle[2];
	int error;

	/* Read Encryption Key Size: the handle, then the size. */
	phone->secure_step = BTD_PHONE_SECURE_KEY_SIZE;
	phone_put16(handle, phone->handle);
	error = btd_session_command(phone->session, PHONE_READ_KEY_SIZE, handle, sizeof(handle));
	if (error != 0 || phone->session->returned_length < 3U) {
		phone->why = "key-size";
		phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_LONG);
		return;
	}

	/* A shorter key is refused. */
	phone->key_size = phone->session->returned[2];
	if (phone->key_size != PHONE_KEY_SIZE) {
		phone->why = "key-size";
		phone_disconnect(phone, PHONE_REASON_SECURITY, BTD_PHONE_AFTER_LONG);
		return;
	}

	/* Succeeded: ready. */
	phone_ready(phone, btd_now_ms());
}

/*
 * Makes the secured link ready (section 5.4): the channels the phone
 * asked for meanwhile are accepted, and the profile hears it while the
 * owner is at the seat.
 */
static void
phone_ready(
	struct btd_phone *phone,
	uint64_t now)
{
	uint8_t answer[BTD_SIGNAL_MAX];
	size_t length;
	int error;

	/* Ready since now. */
	phone->state = BTD_PHONE_READY;
	phone->ready_since = now;
	phone->why = NULL;

	/* The channels held Pending are accepted. */
	error = btd_l2cap_answer_pending(&phone->l2cap, phone->handle, BTD_L2CAP_SUCCESS, answer, sizeof(answer), &length);
	if (error == 0 && length != 0U)
		(void)phone_send(phone, BTD_CID_SIGNALLING, answer, length);

	/* Succeeded: the profile, when the owner is there. */
	phone_profile_ready(phone);
}

/* Tells the profile the link is ready, once a link, while the owner is at the seat. */
static void
phone_profile_ready(
	struct btd_phone *phone)
{
	/* A ready link, its owner there, a profile not told yet. */
	if (phone->state != BTD_PHONE_READY || !phone->present)
		return;
	if (!phone->have_profile || phone->profile_started)
		return;

	/* Succeeded: told. */
	phone->profile_started = 1;
	if (phone->profile.ready != NULL)
		phone->profile.ready(phone->profile.context);
}

/*
 * Takes Disconnection Complete (status, handle, reason) of the phone's
 * link (section 5.5): the link ends, and the reason sets the next page:
 * the phone gone out of range or switched off is paged again after the
 * next step's wait; the phone's user ending short links three times stops
 * the pages; a link bluetoothd ended follows what it was ended for.
 */
static void
phone_disconnected(
	struct btd_phone *phone,
	const uint8_t *parameters,
	size_t length)
{
	uint64_t now;
	uint16_t handle;
	uint8_t reason;
	unsigned after;
	int short_link;

	/* A whole event that happened, of the phone's link. */
	if (length < 4U || phone->state == BTD_PHONE_NONE || parameters[0] != 0x00U)
		return;
	handle = (uint16_t)(phone_get16(parameters + 1) & 0x0fffU);
	if (handle != phone->handle)
		return;
	if (phone->state != BTD_PHONE_SECURING && phone->state != BTD_PHONE_READY && phone->state != BTD_PHONE_CLOSING)
		return;
	reason = parameters[3];
	now = btd_now_ms();

	/* A link that did not last: the next page waits a step more. */
	short_link = 1;
	if (phone->ready_since != 0U && now >= phone->ready_since + BTD_PHONE_STABLE_MS)
		short_link = 0;

	/* What follows: bluetoothd's own end as it was meant, the phone's user's end of short links three times stops. */
	after = BTD_PHONE_AFTER_STEP;
	if (reason == PHONE_REASON_LOCAL_HOST)
		after = phone->after;
	if (reason == PHONE_REASON_USER && short_link) {
		phone->peer_closed++;
		if (phone->peer_closed >= BTD_PHONE_PEER_CLOSED_MAX) {
			phone->why = "peer-closed";
			after = BTD_PHONE_AFTER_NONE;
		}
	}

	/* The link ends. */
	phone_ended(phone, phone->why);

	/* Succeeded: the next page set (a link's end without a reason of bluetoothd's is "lost"). */
	if (phone->why == NULL)
		phone->why = "lost";
	phone_schedule(phone, after, now);
}

/* Ends a second link of the phone that came up while it had one: Disconnect, not the phone link's. */
static void
phone_end_handle(
	struct btd_phone *phone,
	uint16_t handle)
{
	uint8_t disconnect[3];

	/* Succeeded: Disconnect, the link refused for its security's sake. */
	phone_put16(disconnect, (uint16_t)(handle & 0x0fffU));
	disconnect[2] = PHONE_REASON_USER;
	(void)btd_session_command(phone->session, PHONE_DISCONNECT, disconnect, sizeof(disconnect));
}

/* Ends the SDP query: its channel closes, and the profile hears the end. */
static void
phone_sdp_finish(
	struct btd_phone *phone,
	int error)
{
	uint16_t cid;

	/* The channel closes (its closing finds no query any more). */
	cid = phone->sdp_cid;
	phone->sdp_cid = 0U;
	if (cid != 0U && phone->state == BTD_PHONE_READY)
		phone_close_channel(phone, cid);

	/* Succeeded: the profile hears it. */
	phone_sdp_done(phone, error);
}

/* Tells the profile the end of its SDP query (error 0: the records in phone->sdp), once. */
static void
phone_sdp_done(
	struct btd_phone *phone,
	int error)
{
	/* No query. */
	if (phone->sdp_uuid == 0U)
		return;

	/* Over. */
	phone->sdp_uuid = 0U;

	/* Succeeded: the profile hears it. */
	if (phone->have_profile && phone->profile.sdp_done != NULL)
		phone->profile.sdp_done(phone->profile.context, &phone->sdp, error);
}

/* Asks the session for the DLCs that waited for it (the profile hears opened, or closed). */
static void
phone_pending_connect(
	struct btd_phone *phone)
{
	unsigned channels[BTD_PHONE_PENDING_DLCS];
	unsigned count;
	unsigned index;
	unsigned dlci;
	int error;

	/* The DLCs that wait, taken (a failure's hook may ask for another). */
	count = phone->pending_count;
	memcpy(channels, phone->pending_channels, sizeof(channels));
	phone->pending_count = 0U;

	/* Each, asked for; one the session refuses at once fails. */
	for (index = 0U; index < count; index++) {
		error = btd_rfcomm_connect(&phone->rfcomm, channels[index], btd_now_ms(), &dlci);
		if (error == 0)
			continue;
		if (phone->have_profile && phone->profile.open_failed != NULL)
			phone->profile.open_failed(phone->profile.context, channels[index]);
	}
}

/* Fails the DLCs that waited for an RFCOMM session that did not come. */
static void
phone_pending_failed(
	struct btd_phone *phone)
{
	unsigned channels[BTD_PHONE_PENDING_DLCS];
	unsigned count;
	unsigned index;

	/* The DLCs that wait, taken. */
	count = phone->pending_count;
	memcpy(channels, phone->pending_channels, sizeof(channels));
	phone->pending_count = 0U;
	phone->rfcomm_retry_at = 0U;

	/* Succeeded: each fails. */
	for (index = 0U; index < count; index++) {
		if (phone->have_profile && phone->profile.open_failed != NULL)
			phone->profile.open_failed(phone->profile.context, channels[index]);
	}
}

/* Takes an ACL packet of the phone's link: a frame, once whole, goes to its channel; a frame lost on the way is told. */
static void
phone_acl(
	struct btd_phone *phone,
	const uint8_t *packet,
	size_t length)
{
	struct btd_acl acl;
	const uint8_t *payload;
	unsigned dropped;
	uint16_t started;
	uint16_t cid;
	size_t payload_length;
	int whole;
	int error;

	/* The packet's connection, the phone's. */
	error = btd_acl_parse(packet, length, &acl);
	if (error != 0)
		return;
	if (phone->state == BTD_PHONE_NONE || acl.handle != phone->handle)
		return;

	/* The channel of the frame being put together, should it be dropped. */
	started = 0U;
	if (phone->reassembly.active && phone->reassembly.used >= BTD_L2CAP_HEADER)
		started = phone_get16(phone->reassembly.frame + 2);
	dropped = phone->reassembly.dropped;

	/* The frame, once whole. */
	whole = btd_reassembly_feed(&phone->reassembly, &acl);
	if (phone->reassembly.dropped != dropped) {
		phone_lost_frame(phone, started);
		if (phone->state != BTD_PHONE_READY)
			return;
	}

	/* Not whole yet. */
	if (whole <= 0)
		return;

	/* Succeeded: the frame to its channel. */
	payload = phone->reassembly.frame + BTD_L2CAP_HEADER;
	payload_length = phone->reassembly.expected - BTD_L2CAP_HEADER;
	cid = phone_get16(phone->reassembly.frame + 2);
	phone_frame(phone, cid, payload, payload_length);
}

/* Gives a whole frame to its channel: the signalling, the SDP query, RFCOMM, or an SDP server; anything else is passed over. */
static void
phone_frame(
	struct btd_phone *phone,
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	unsigned index;

	/* The signalling. */
	if (cid == BTD_CID_SIGNALLING) {
		phone_signal(phone, payload, length);
		return;
	}

	/* The query of the phone's records. */
	if (phone->sdp_cid != 0U && cid == phone->sdp_cid) {
		phone_sdp_input(phone, payload, length);
		return;
	}

	/* RFCOMM, once its channel opened. */
	if (phone->rfcomm_active && cid == phone->rfcomm_cid) {
		btd_rfcomm_input(&phone->rfcomm, payload, length, btd_now_ms());
		return;
	}

	/* Succeeded: an SDP server's request, or passed over. */
	for (index = 0U; index < BTD_PHONE_SDP_SERVERS; index++) {
		if (phone->sdps[index].cid != 0U && cid == phone->sdps[index].cid) {
			phone_sdps_input(phone, &phone->sdps[index], payload, length);
			return;
		}
	}
}

/* Takes a signalling frame: its answers go back, and the channels that closed and opened move the link's parts on. */
static void
phone_signal(
	struct btd_phone *phone,
	const uint8_t *payload,
	size_t length)
{
	struct btd_signal_effect effect;
	uint8_t answer[BTD_SIGNAL_MAX];
	uint64_t now;
	size_t answer_length;
	unsigned index;

	/* The commands, answered. */
	(void)btd_l2cap_signal(&phone->l2cap, phone->handle, 0, payload, length, answer, sizeof(answer), &answer_length, &effect);
	if (answer_length != 0U)
		(void)phone_send(phone, BTD_CID_SIGNALLING, answer, answer_length);

	/* The channels that closed; bluetoothd's RFCOMM channel refused (the phone's request crossed it, section 8.2 of p002) is asked for again later. */
	for (index = 0U; index < effect.closed_count; index++) {
		if (effect.closed[index] == phone->rfcomm_cid &&
		    !phone->rfcomm_active &&
		    effect.closed_reason[index] == BTD_L2CAP_CLOSED_REFUSED) {
			phone->rfcomm_cid = 0U;
			phone->rfcomm_refused = 1;
			continue;
		}

		/* Any other: its parts forgotten. */
		phone_closed(phone, effect.closed[index]);
	}

	/* Those that opened. */
	for (index = 0U; index < effect.opened_count; index++)
		phone_opened(phone, effect.opened[index]);

	/* Bluetoothd's refused RFCOMM channel: asked for again after a wait, three times at most, unless the phone's opened meanwhile. */
	if (!phone->rfcomm_refused)
		return;
	phone->rfcomm_refused = 0;
	if (phone->rfcomm_cid != 0U || phone->pending_count == 0U)
		return;
	if (phone->rfcomm_tries >= PHONE_RFCOMM_TRIES) {
		phone_pending_failed(phone);
		return;
	}

	/* Succeeded: the wait, 100 to 500 ms, spread by the clock so both sides do not ask again together. */
	now = btd_now_ms();
	phone->rfcomm_retry_at = now + PHONE_RFCOMM_WAIT_MS + (now % PHONE_RFCOMM_SPREAD_MS);
}

/* Moves the link's parts on for a channel that opened: an SDP server, the SDP query, or the RFCOMM session. */
static void
phone_opened(
	struct btd_phone *phone,
	uint16_t cid)
{
	struct btd_rfcomm_events events;
	struct btd_channel *channel;
	unsigned mtu;
	unsigned index;
	int error;

	/* The channel and its PSM. */
	channel = btd_l2cap_channel(&phone->l2cap, cid);
	if (channel == NULL)
		return;

	/* The phone's SDP channel: a free server for it, or it is closed. */
	if (channel->psm == PHONE_PSM_SDP && channel->inbound) {
		for (index = 0U; index < BTD_PHONE_SDP_SERVERS; index++) {
			if (phone->sdps[index].cid != 0U)
				continue;
			phone->sdps[index].cid = cid;
			btd_sdps_init(&phone->sdps[index].server, phone->db, channel->remote_mtu);
			return;
		}

		/* No server free. */
		phone_close_channel(phone, cid);
		return;
	}

	/* Bluetoothd's SDP channel: the query's first request. */
	if (channel->psm == PHONE_PSM_SDP && cid == phone->sdp_cid) {
		phone->sdp_transaction++;
		btd_sdp_init(&phone->sdp, phone->sdp_uuid, phone->sdp_transaction);
		phone_sdp_send(phone);
		return;
	}

	/* Another channel than RFCOMM's is not the phone link's. */
	if (channel->psm != PHONE_PSM_RFCOMM)
		return;

	/* A second RFCOMM channel (one session a link): a session there already. */
	if (phone->rfcomm_active) {
		phone_close_channel(phone, cid);
		return;
	}

	/* The phone's while bluetoothd's is asked for. */
	if (channel->inbound && phone->rfcomm_cid != 0U) {
		phone_close_channel(phone, cid);
		return;
	}

	/* The RFCOMM session, with the smaller MTU of the two sides. */
	mtu = BTD_L2CAP_MTU;
	if (channel->remote_mtu < mtu)
		mtu = channel->remote_mtu;
	memset(&events, 0, sizeof(events));
	events.context = phone;
	events.send = phone_rf_send;
	events.accept = phone_rf_accept;
	events.opened = phone_rf_opened;
	events.data = phone_rf_data;
	events.writable = phone_rf_writable;
	events.closed = phone_rf_closed;
	events.ended = phone_rf_ended;
	phone->rfcomm_cid = cid;
	phone->rfcomm_active = 1;
	btd_rfcomm_init(&phone->rfcomm, &events, mtu);

	/* Bluetoothd's server channels offered on every session, whoever opened it (section 5.8). */
	(void)btd_rfcomm_listen(&phone->rfcomm, BTD_PHONE_MNS_CHANNEL);

	/* Bluetoothd's: it starts the multiplexer (the phone's waits for the phone's SABM). */
	if (!channel->inbound) {
		error = btd_rfcomm_start(&phone->rfcomm, btd_now_ms());
		if (error != 0) {
			phone_close_channel(phone, cid);
			return;
		}
	}

	/* Succeeded: the DLCs that waited for a session are asked for. */
	phone_pending_connect(phone);
}

/* Forgets the parts of a channel that closed: an SDP server, the SDP query, or the RFCOMM session. */
static void
phone_closed(
	struct btd_phone *phone,
	uint16_t cid)
{
	unsigned index;

	/* An SDP server's. */
	for (index = 0U; index < BTD_PHONE_SDP_SERVERS; index++) {
		if (phone->sdps[index].cid == cid)
			phone->sdps[index].cid = 0U;
	}

	/* The SDP query's: a query still asking fails. */
	if (phone->sdp_cid != 0U && cid == phone->sdp_cid) {
		phone->sdp_cid = 0U;
		phone_sdp_done(phone, EIO);
		return;
	}

	/* Not RFCOMM's. */
	if (phone->rfcomm_cid == 0U || cid != phone->rfcomm_cid)
		return;

	/* RFCOMM's, before it opened: the DLCs that waited for it fail. */
	phone->rfcomm_cid = 0U;
	if (!phone->rfcomm_active) {
		phone_pending_failed(phone);
		return;
	}

	/* Succeeded: the session's DLCs are lost (the profile hears it). */
	phone->rfcomm_active = 0;
	btd_rfcomm_lost(&phone->rfcomm);
}

/*
 * Takes the session's notice of packets dropped on the phone's link
 * (section 3.5): the frame being put together goes; lost signalling, or a
 * lost packet of no known channel, ends the link; lost data of an SDP or
 * RFCOMM channel closes that channel alone, of another channel ends the
 * link; lost events are counted (the link waits for none in this Phase).
 */
static void
phone_notice(
	struct btd_phone *phone,
	const uint8_t *packet)
{
	uint16_t handle;
	uint16_t cid;
	uint8_t flags;
	int carried;

	/* The phone's link. */
	handle = phone_get16(packet + 1);
	if (phone->state == BTD_PHONE_NONE || handle != phone->handle) {
		phone->notices++;
		return;
	}

	/* The frame being put together is not finished with packets that are not there. */
	phone->reassembly.active = 0;
	flags = packet[3];
	cid = phone_get16(packet + 4);

	/* Lost signalling, or a packet of no known channel: the link is made again. */
	if ((flags & (BTD_DROP_SIGNAL | BTD_DROP_UNKNOWN)) != 0U) {
		phone_disconnect(phone, PHONE_REASON_USER, BTD_PHONE_AFTER_STEP);
		return;
	}

	/* Lost data: its channel alone when it is SDP's or RFCOMM's, else the link. */
	if ((flags & BTD_DROP_DATA) != 0U) {
		carried = phone_carries(phone, cid);
		if (carried) {
			phone_close_channel(phone, cid);
			return;
		}

		/* Another channel's. */
		phone_disconnect(phone, PHONE_REASON_USER, BTD_PHONE_AFTER_STEP);
		return;
	}

	/* Succeeded: lost events counted. */
	phone->notices++;
}

/*
 * Takes a frame the reassembly dropped (a piece of it was lost): as the
 * session's lost data of its channel (cid 0: the channel is not known, a
 * packet of no known channel).
 */
static void
phone_lost_frame(
	struct btd_phone *phone,
	uint16_t cid)
{
	int carried;

	/* A frame of SDP's or RFCOMM's channel: that channel closes. */
	carried = phone_carries(phone, cid);
	if (carried) {
		phone_close_channel(phone, cid);
		return;
	}

	/* Succeeded: any other frame ends the link. */
	phone_disconnect(phone, PHONE_REASON_USER, BTD_PHONE_AFTER_STEP);
}

/* Tells whether a channel is one of the phone link's SDP or RFCOMM channels. */
static int
phone_carries(
	const struct btd_phone *phone,
	uint16_t cid)
{
	unsigned index;

	/* No channel. */
	if (cid == 0U)
		return 0;

	/* The SDP query's and RFCOMM's. */
	if (cid == phone->sdp_cid || cid == phone->rfcomm_cid)
		return 1;

	/* An SDP server's. */
	for (index = 0U; index < BTD_PHONE_SDP_SERVERS; index++) {
		if (cid == phone->sdps[index].cid)
			return 1;
	}

	/* Another channel. */
	return 0;
}

/* Closes one channel of the phone's link: its Disconnection Request, and its parts forgotten now. */
static void
phone_close_channel(
	struct btd_phone *phone,
	uint16_t cid)
{
	uint8_t request[16];
	size_t length;
	int error;

	/* The request (a channel closing or gone already sends none). */
	error = btd_l2cap_disconnect(&phone->l2cap, cid, request, sizeof(request), &length);
	if (error == 0)
		(void)phone_send(phone, BTD_CID_SIGNALLING, request, length);

	/* Succeeded: its parts forgotten. */
	phone_closed(phone, cid);
}

/* Ends the phone's link with a reason (section 5.5), and what follows it: Disconnect; the end comes with its Disconnection Complete. */
static void
phone_disconnect(
	struct btd_phone *phone,
	uint8_t reason,
	unsigned after)
{
	uint8_t disconnect[3];
	int error;

	/* A link to end (not one ending already). */
	if (phone->state != BTD_PHONE_READY && phone->state != BTD_PHONE_SECURING)
		return;

	/* Disconnect. */
	phone->after = after;
	phone_put16(disconnect, phone->handle);
	disconnect[2] = reason;
	error = btd_session_command(phone->session, PHONE_DISCONNECT, disconnect, sizeof(disconnect));
	if (error != 0) {
		phone_ended(phone, "lost");
		phone_schedule(phone, after, btd_now_ms());
		return;
	}

	/* Succeeded: closing. */
	phone->state = BTD_PHONE_CLOSING;
}

/*
 * Forgets the phone's link: the profile hears its DLCs, its query and the
 * link end; nothing of the link is left.  The record, the seat and the
 * pages stay.
 */
static void
phone_ended(
	struct btd_phone *phone,
	const char *why)
{
	unsigned index;

	/* Closing from now on (nothing more is sent on the link, a profile's request is refused). */
	phone->state = BTD_PHONE_CLOSING;
	if (why != NULL)
		phone->why = why;

	/* RFCOMM's session goes (its DLCs' closing reaches the profile). */
	if (phone->rfcomm_active) {
		phone->rfcomm_active = 0;
		btd_rfcomm_lost(&phone->rfcomm);
	}

	/* The DLCs that waited for a session, and the SDP query, fail. */
	phone_pending_failed(phone);
	phone_sdp_done(phone, ENOTCONN);

	/* The profile hears the end. */
	if (phone->profile_started) {
		phone->profile_started = 0;
		if (phone->profile.ended != NULL)
			phone->profile.ended(phone->profile.context);
	}

	/* Nothing of the link is left. */
	btd_l2cap_init(&phone->l2cap);
	memset(&phone->reassembly, 0, sizeof(phone->reassembly));
	for (index = 0U; index < BTD_PHONE_SDP_SERVERS; index++)
		phone->sdps[index].cid = 0U;
	phone->sdp_cid = 0U;
	phone->rfcomm_cid = 0U;
	phone->rfcomm_retry_at = 0U;
	phone->queue_first = 0U;
	phone->queue_count = 0U;
	phone->encrypted = 0;
	phone->ready_since = 0U;
	phone->secure_step = 0U;
	phone->stop_wanted = 0;

	/* Succeeded: no link (the HID host's limit follows the record, not the link). */
	phone->state = BTD_PHONE_NONE;
}

/*
 * Queues an L2CAP frame of the phone's link, on the signalling channel or
 * on one of the link's channels named by its local CID (the frame goes to
 * the phone's end of it, its remote CID), and sends what the session takes
 * now.  Returns 0, ENOBUFS when the queue is full (the frame is not queued;
 * RFCOMM keeps its control frames and data to write again), ENOTCONN for a
 * channel whose phone's end is not known, or EMSGSIZE for a payload longer
 * than a frame.
 */
static int
phone_send(
	struct btd_phone *phone,
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	struct btd_phone_frame *frame;
	struct btd_channel *channel;
	uint16_t destination;
	unsigned slot;

	/* A payload a frame holds. */
	if (length > BTD_L2CAP_MAX)
		return EMSGSIZE;

	/* The signalling channel is fixed; a dynamic channel goes to the phone's CID, once known. */
	destination = cid;
	if (cid != BTD_CID_SIGNALLING) {
		channel = btd_l2cap_channel(&phone->l2cap, cid);
		if (channel == NULL || channel->remote_cid == 0U)
			return ENOTCONN;
		destination = channel->remote_cid;
	}

	/* Room in the queue. */
	if (phone->queue_count >= BTD_PHONE_QUEUE) {
		phone->queue_dropped++;
		return ENOBUFS;
	}

	/* At the queue's end. */
	slot = (phone->queue_first + phone->queue_count) % BTD_PHONE_QUEUE;
	frame = &phone->queue[slot];
	frame->cid = destination;
	frame->length = length;
	if (length != 0U)
		memcpy(frame->bytes, payload, length);
	phone->queue_count++;

	/* Succeeded: queued, and sent as far as the session takes it. */
	phone_flush(phone);
	return 0;
}

/* Moves the queued frames to the session as far as the link's share takes them, oldest first. */
static void
phone_flush(
	struct btd_phone *phone)
{
	struct btd_phone_frame *frame;
	unsigned room;
	int error;

	/* Each frame while the link has room. */
	while (phone->queue_count != 0U) {
		room = btd_session_link_room(phone->session, phone->handle);
		if (room == 0U)
			return;

		/* The oldest frame; one the session has no room for waits. */
		frame = &phone->queue[phone->queue_first];
		error = btd_session_send(phone->session, phone->handle, frame->cid, frame->bytes, frame->length);
		if (error == ENOBUFS)
			return;

		/* Sent, or not sendable at all (the link went): it leaves the queue. */
		phone->queue_first = (phone->queue_first + 1U) % BTD_PHONE_QUEUE;
		phone->queue_count--;
	}
}

/* Answers a request on an SDP server's channel. */
static void
phone_sdps_input(
	struct btd_phone *phone,
	struct btd_phone_sdps *slot,
	const uint8_t *pdu,
	size_t length)
{
	uint8_t answer[BTD_L2CAP_MAX];
	size_t answer_length;
	int error;

	/* The server's answer. */
	error = btd_sdps_input(&slot->server, pdu, length, answer, sizeof(answer), &answer_length);
	if (error != 0 || answer_length == 0U)
		return;

	/* Succeeded: sent on its channel. */
	(void)phone_send(phone, slot->cid, answer, answer_length);
}

/* Sends the SDP query's next request; one that cannot be built fails the query. */
static void
phone_sdp_send(
	struct btd_phone *phone)
{
	uint8_t request[BTD_SDP_REQUEST_MAX];
	size_t length;
	int error;

	/* The request. */
	error = btd_sdp_request(&phone->sdp, request, sizeof(request), &length);
	if (error != 0) {
		phone_sdp_finish(phone, EIO);
		return;
	}

	/* Succeeded: sent on the query's channel. */
	(void)phone_send(phone, phone->sdp_cid, request, length);
}

/* Takes an answer of the SDP query: the next request, or the records whole (or a failure), then the query's channel closes. */
static void
phone_sdp_input(
	struct btd_phone *phone,
	const uint8_t *pdu,
	size_t length)
{
	int meant;

	/* Only while a query asks. */
	if (phone->sdp_uuid == 0U)
		return;

	/* The answer: more to ask, the lists whole, or failed. */
	meant = btd_sdp_input(&phone->sdp, pdu, length);
	if (meant == BTD_SDP_MORE) {
		phone_sdp_send(phone);
		return;
	}

	/* A failed query. */
	if (meant != BTD_SDP_DONE) {
		phone_sdp_finish(phone, EIO);
		return;
	}

	/* Succeeded: the records to the profile. */
	phone_sdp_finish(phone, 0);
}

/*
 * Asks for bluetoothd's RFCOMM channel for the DLCs that wait (the session
 * starts when it opens), unless a channel is asked for or opening already.
 */
static void
phone_rfcomm_open(
	struct btd_phone *phone)
{
	uint8_t request[16];
	size_t length;
	int error;

	/* Nothing waits, or a session there is. */
	if (phone->pending_count == 0U || phone->state != BTD_PHONE_READY)
		return;
	if (phone->rfcomm_active) {
		phone_pending_connect(phone);
		return;
	}

	/* A channel asked for already (bluetoothd's, or the phone's opening). */
	if (phone->rfcomm_cid != 0U)
		return;

	/* Bluetoothd's RFCOMM channel asked for. */
	phone->rfcomm_tries++;
	error = btd_l2cap_connect(&phone->l2cap, phone->handle, PHONE_PSM_RFCOMM, request, sizeof(request), &length, &phone->rfcomm_cid);
	if (error != 0) {
		phone->rfcomm_cid = 0U;
		phone_pending_failed(phone);
		return;
	}

	/* Succeeded: sent on the signalling channel. */
	(void)phone_send(phone, BTD_CID_SIGNALLING, request, length);
}

/* RFCOMM's send hook: a payload on the session's channel, queued. */
static int
phone_rf_send(
	void *context,
	const uint8_t *payload,
	size_t length)
{
	struct btd_phone *phone;
	int error;

	/* The phone link, with its RFCOMM channel. */
	phone = context;
	if (phone->rfcomm_cid == 0U)
		return ENOTCONN;

	/* Queued. */
	error = phone_send(phone, phone->rfcomm_cid, payload, length);
	if (error != 0)
		return error;

	/* Succeeded: it goes. */
	return 0;
}

/* RFCOMM's question whether a server channel of bluetoothd's is offered: the profile's answer (none without a profile). */
static int
phone_rf_accept(
	void *context,
	unsigned server_channel)
{
	struct btd_phone *phone;
	int offered;

	/* The phone link, with a profile told the link is ready. */
	phone = context;
	if (!phone->profile_started || phone->profile.accept == NULL)
		return 0;

	/* Succeeded: as the profile says. */
	offered = phone->profile.accept(phone->profile.context, server_channel);
	return offered;
}

/* RFCOMM's DLC opened: to the profile. */
static void
phone_rf_opened(
	void *context,
	unsigned dlci)
{
	struct btd_phone *phone;

	/* Succeeded: the profile hears it. */
	phone = context;
	if (phone->have_profile && phone->profile.opened != NULL)
		phone->profile.opened(phone->profile.context, dlci);
}

/* RFCOMM's data of a DLC: to the profile. */
static void
phone_rf_data(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length)
{
	struct btd_phone *phone;

	/* Succeeded: the profile hears it. */
	phone = context;
	if (phone->have_profile && phone->profile.data != NULL)
		phone->profile.data(phone->profile.context, dlci, data, length);
}

/* RFCOMM's DLC writable again: to the profile. */
static void
phone_rf_writable(
	void *context,
	unsigned dlci)
{
	struct btd_phone *phone;

	/* Succeeded: the profile hears it. */
	phone = context;
	if (phone->have_profile && phone->profile.writable != NULL)
		phone->profile.writable(phone->profile.context, dlci);
}

/* RFCOMM's DLC closed: to the profile. */
static void
phone_rf_closed(
	void *context,
	unsigned dlci,
	int reason)
{
	struct btd_phone *phone;

	/* Succeeded: the profile hears it. */
	phone = context;
	if (phone->have_profile && phone->profile.closed != NULL)
		phone->profile.closed(phone->profile.context, dlci, reason);
}

/* RFCOMM's session ended: its L2CAP channel closes (its DLCs were told). */
static void
phone_rf_ended(
	void *context,
	int reason)
{
	struct btd_phone *phone;
	uint16_t cid;

	UNUSED_PARAMETER(reason);

	/* The phone link, with its session (one forgotten already says nothing more). */
	phone = context;
	if (!phone->rfcomm_active)
		return;

	/* Succeeded: the channel closes. */
	cid = phone->rfcomm_cid;
	phone->rfcomm_active = 0;
	phone->rfcomm_cid = 0U;
	phone_close_channel(phone, cid);
}

/* Gives the earlier of two deadlines, 0 standing for none. */
static uint64_t
phone_earlier(
	uint64_t earliest,
	uint64_t deadline)
{
	/* No new deadline, or none before. */
	if (deadline == 0U)
		return earliest;
	if (earliest == 0U)
		return deadline;

	/* The earlier. */
	if (deadline < earliest)
		return deadline;

	/* The one there was. */
	return earliest;
}

/* Reads two bytes, least significant first. */
static uint16_t
phone_get16(
	const uint8_t *bytes)
{
	unsigned value;

	/* The two bytes. */
	value = (unsigned)bytes[0] | ((unsigned)bytes[1] << 8);

	/* Succeeded: the value. */
	return (uint16_t)value;
}

/* Writes a 16-bit value least significant byte first. */
static void
phone_put16(
	uint8_t *bytes,
	uint16_t value)
{
	/* The two bytes. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)(value >> 8);
}

/*
 * Writes the owner's record of a phone the pairing hands over (ws197-p003
 * section 3.2, steps 6 to 9): no other phone's valid record, no other
 * owner's record of this phone; the same owner's profiles are kept and the
 * link is wanted again; invalid records of other phones go.  Returns 0
 * with the record kept, or an errno value with *why.
 */
static int
phone_take_record(
	struct btd_phone *phone,
	const struct btd_pair_handoff *handoff,
	const char **why)
{
	struct btd_phonerec records[BTD_PHONEREC_LIST_MAX];
	int valids[BTD_PHONEREC_LIST_MAX];
	struct btd_phonerec record;
	unsigned profiles;
	unsigned count;
	unsigned index;
	int same;
	int error;

	/* The records are the controller's. */
	if (!phone->session->have_address) {
		*why = "store";
		return ENXIO;
	}

	/* The records there are. */
	error = btd_phonerec_list(phone->keys_folder, phone->session->address, records, BTD_PHONEREC_LIST_MAX, &count);
	if (error != 0) {
		*why = "store";
		return error;
	}

	/* Each valid one: another phone's, or this phone's of another owner, refuses; the same owner's profiles are kept. */
	profiles = BTD_PHONEREC_PROFILES;
	for (index = 0U; index < count; index++) {
		valids[index] = btd_phonerec_valid(phone->keys_folder, phone->session->address, &records[index], phone->hooks.account, phone->hooks.context);
		if (!valids[index])
			continue;

		/* Another phone is the one already (one phone, Q1). */
		same = memcmp(records[index].address, handoff->address, BTD_ADDRESS_BYTES);
		if (same != 0) {
			*why = "other-phone";
			return EEXIST;
		}

		/* This phone is another owner's (the check before the pairing, made again: section 3.2's 7). */
		if (records[index].uid != handoff->uid) {
			*why = "owned";
			return EPERM;
		}

		/* The same owner paired it again: its profiles stay. */
		profiles = records[index].profiles;
	}

	/* The record: the owner, the profiles, wanted (a pairing again means "use it", review-2 n8). */
	memset(&record, 0, sizeof(record));
	memcpy(record.address, handoff->address, BTD_ADDRESS_BYTES);
	record.uid = handoff->uid;
	record.profiles = profiles;
	record.enabled = 1;

	/* The owner's account name, which tells a reused uid later. */
	error = phone->hooks.account(phone->hooks.context, handoff->uid, record.user, sizeof(record.user));
	if (error != 0) {
		*why = "store";
		return error;
	}

	/* Written (a name the record cannot keep is refused here). */
	error = btd_phonerec_write(phone->keys_folder, phone->session->address, &record);
	if (error != 0) {
		*why = "store";
		return error;
	}

	/* Invalid records of other phones go: they own nothing and would stand beside this one. */
	for (index = 0U; index < count; index++) {
		same = memcmp(records[index].address, handoff->address, BTD_ADDRESS_BYTES);
		if (same == 0 || valids[index])
			continue;
		(void)btd_phonerec_forget(phone->keys_folder, phone->session->address, records[index].address);
	}

	/* Succeeded: the record is the phone link's. */
	phone->record = record;
	phone->have_record = 1;
	phone->record_valid = 1;
	return 0;
}

/* Sets the HID host's limit from the record (ws197-p003 section 3.4): one link fewer while a valid record is on. */
static void
phone_limit(
	struct btd_phone *phone)
{
	unsigned limit;

	/* Six, or five for a phone that is wanted. */
	limit = BTD_HID_MAX;
	if (phone->have_record && phone->record_valid && phone->record.enabled)
		limit = BTD_PHONE_HID_LIMIT;

	/* Succeeded: the HID host's limit. */
	btd_hid_set_limit(phone->hid, limit);
}

/* Names the phone's link for PHONE SHOW: none, paging, securing, ready or closing. */
static const char *
phone_link_name(
	const struct btd_phone *phone)
{
	/* Each state's name. */
	switch (phone->state) {
	case BTD_PHONE_READY:
		return "ready";
	case BTD_PHONE_CLOSING:
		return "closing";
	default:
		break;
	}

	/* No link. */
	return "none";
}

/* Writes the profiles on as "m,c,h" (each letter when on), or "-" for none. */
static void
phone_profiles_text(
	unsigned profiles,
	char *text,
	size_t size)
{
	size_t used;

	/* Each profile on, in order. */
	used = 0U;
	text[0] = '\0';
	if ((profiles & BTD_PHONEREC_MESSAGES) != 0U)
		used += (size_t)snprintf(text + used, size - used, "%s", "m,");
	if ((profiles & BTD_PHONEREC_CONTACTS) != 0U)
		used += (size_t)snprintf(text + used, size - used, "%s", "c,");
	if ((profiles & BTD_PHONEREC_CALLS) != 0U)
		used += (size_t)snprintf(text + used, size - used, "%s", "h,");

	/* None. */
	if (used == 0U) {
		(void)snprintf(text, size, "%s", "-");
		return;
	}

	/* The last comma goes. */
	text[used - 1U] = '\0';
}
