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

/* The commands the phone link sends: Disconnect. */
#define PHONE_DISCONNECT		0x0406U

/* The events it takes: Disconnection Complete, Encryption Change (and its second version). */
#define PHONE_EVENT_DISCONNECTED	0x05U
#define PHONE_EVENT_ENCRYPTION		0x08U
#define PHONE_EVENT_ENCRYPTION_V2	0x59U

/* The reason of a disconnection by bluetoothd, and of one for the link's security. */
#define PHONE_REASON_USER		0x13U
#define PHONE_REASON_SECURITY		0x05U

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

/* The OBEX type of a folder listing, its NUL included. */
#define PHONE_LISTING_TYPE		"x-obex/folder-listing"
#define PHONE_LISTING_TYPE_BYTES	22U

/* The probe's room for a Get's headers. */
#define PHONE_HEADERS_MAX		64U

/*
 * The Targets of OBEX's Connect (p001 section 6.2): MAP's MAS
 * (bb582b40-420c-11db-b0de-0800200c9a66) and PBAP's PSE
 * (796135f0-f0c5-11d8-0966-0800200c9a66), most significant byte first.
 */
static const uint8_t phone_target_mas[16] = {
	0xbbU, 0x58U, 0x2bU, 0x40U, 0x42U, 0x0cU, 0x11U, 0xdbU,
	0xb0U, 0xdeU, 0x08U, 0x00U, 0x20U, 0x0cU, 0x9aU, 0x66U
};
static const uint8_t phone_target_pse[16] = {
	0x79U, 0x61U, 0x35U, 0xf0U, 0xf0U, 0xc5U, 0x11U, 0xd8U,
	0x09U, 0x66U, 0x08U, 0x00U, 0x20U, 0x0cU, 0x9aU, 0x66U
};

static int phone_accept(void *context, uint16_t handle, uint16_t psm, uint16_t *result, uint16_t *status);
static void phone_event(struct btd_phone *phone, const uint8_t *parameters, size_t length, uint8_t code);
static void phone_acl(struct btd_phone *phone, const uint8_t *packet, size_t length);
static void phone_frame(struct btd_phone *phone, uint16_t cid, const uint8_t *payload, size_t length);
static void phone_signal(struct btd_phone *phone, const uint8_t *payload, size_t length);
static void phone_opened(struct btd_phone *phone, uint16_t cid);
static void phone_closed(struct btd_phone *phone, uint16_t cid);
static void phone_notice(struct btd_phone *phone, const uint8_t *packet);
static void phone_lost_frame(struct btd_phone *phone, uint16_t cid);
static int phone_carries(const struct btd_phone *phone, uint16_t cid);
static void phone_close_channel(struct btd_phone *phone, uint16_t cid);
static void phone_disconnect(struct btd_phone *phone, uint8_t reason);
static void phone_ended(struct btd_phone *phone, const char *why);
static int phone_send(struct btd_phone *phone, uint16_t cid, const uint8_t *payload, size_t length);
static void phone_flush(struct btd_phone *phone);
static void phone_sdps_input(struct btd_phone *phone, struct btd_phone_sdps *slot, const uint8_t *pdu, size_t length);
static void phone_sdp_send(struct btd_phone *phone);
static void phone_sdp_input(struct btd_phone *phone, const uint8_t *pdu, size_t length);
static void phone_rfcomm_open(struct btd_phone *phone);
static void phone_probe_dlc(struct btd_phone *phone);
static void phone_probe_fail(struct btd_phone *phone, const char *why);
static void phone_probe_finish(struct btd_phone *phone);
static int phone_rf_send(void *context, const uint8_t *payload, size_t length);
static int phone_rf_accept(void *context, unsigned server_channel);
static void phone_rf_opened(void *context, unsigned dlci);
static void phone_rf_data(void *context, unsigned dlci, const uint8_t *data, size_t length);
static void phone_rf_writable(void *context, unsigned dlci);
static void phone_rf_closed(void *context, unsigned dlci, int reason);
static void phone_rf_ended(void *context, int reason);
static int phone_ob_write(void *context, const uint8_t *data, size_t length, size_t *written);
static void phone_ob_done(void *context, unsigned operation, int error, uint8_t code, const uint8_t *headers, size_t length);
static int phone_ob_body(void *context, const uint8_t *data, size_t length);
static uint64_t phone_earlier(uint64_t earliest, uint64_t deadline);
static uint16_t phone_get16(const uint8_t *bytes);
static void phone_put16(uint8_t *bytes, uint16_t value);
static int phone_take_record(struct btd_phone *phone, const struct btd_pair_handoff *handoff, const char **why);
static void phone_limit(struct btd_phone *phone);
static const char *phone_link_name(const struct btd_phone *phone);
static void phone_profiles_text(unsigned profiles, char *text, size_t size);

/*
 * Prepares the phone link of a session: no phone yet.  The answer hook
 * hears the end of each probe.
 */
void
btd_phone_init(
	struct btd_phone *phone,
	struct btd_session *session,
	struct btd_router *router,
	struct btd_hid *hid,
	const struct btd_sdps_db *db,
	const char *keys_folder,
	const struct btd_phone_hooks *hooks,
	btd_phone_answer_fn answer,
	void *context)
{
	/* Nothing under way, and no record read yet. */
	memset(phone, 0, sizeof(*phone));
	phone->session = session;
	phone->router = router;
	phone->hid = hid;
	phone->db = db;
	phone->keys_folder = keys_folder;
	phone->hooks = *hooks;
	phone->answer = answer;
	phone->answer_context = context;
	phone->state = BTD_PHONE_NONE;
	btd_l2cap_init(&phone->l2cap);
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

	/* Succeeded: the link is the phone link's. */
	return 1;
}

/*
 * Tells the router whether the phone link wants a device that connects to
 * bluetoothd: never in this Phase (a phone that connects by itself is
 * authenticated by p003's reconnection).
 */
int
btd_phone_wants(
	void *context,
	const uint8_t *address)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(address);

	/* Not wanted. */
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
 * Ends what passed its deadline: RFCOMM's and OBEX's answers, a probe that
 * took too long, and asks for the RFCOMM channel again after a refusal.
 */
void
btd_phone_tick(
	struct btd_phone *phone,
	uint64_t now)
{
	/* No phone. */
	if (phone->state == BTD_PHONE_NONE)
		return;

	/* RFCOMM's timers. */
	if (phone->rfcomm_active)
		btd_rfcomm_tick(&phone->rfcomm, now);

	/* OBEX's answer of a probe. */
	if (phone->probe.step >= BTD_PHONE_PROBE_CONNECT)
		btd_obex_tick(&phone->obex, now);

	/* No probe under way. */
	if (phone->probe.step == BTD_PHONE_PROBE_NONE)
		return;

	/* A probe that took too long. */
	if (now >= phone->probe.deadline) {
		phone_probe_fail(phone, "timeout");
		return;
	}

	/* Succeeded: the RFCOMM channel asked for again when its wait is over. */
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

	/* No phone. */
	if (phone->state == BTD_PHONE_NONE)
		return 0U;

	/* The probe's. */
	earliest = 0U;
	if (phone->probe.step != BTD_PHONE_PROBE_NONE)
		earliest = phone->probe.deadline;

	/* RFCOMM's answers. */
	if (phone->rfcomm_active) {
		deadline = btd_rfcomm_deadline(&phone->rfcomm);
		earliest = phone_earlier(earliest, deadline);
	}

	/* OBEX's answer. */
	if (phone->probe.step >= BTD_PHONE_PROBE_CONNECT) {
		deadline = btd_obex_deadline(&phone->obex);
		earliest = phone_earlier(earliest, deadline);
	}

	/* The RFCOMM channel's next request. */
	earliest = phone_earlier(earliest, phone->rfcomm_retry_at);

	/* Succeeded: the earliest, or 0. */
	return earliest;
}

/*
 * Forgets the phone's link because the controller went or was reset: no
 * command can be sent, a probe ends as lost.
 */
void
btd_phone_lost(
	struct btd_phone *phone)
{
	/* No phone. */
	if (phone->state == BTD_PHONE_NONE)
		return;

	/* Succeeded: ended. */
	phone_ended(phone, "lost");
}

/*
 * Starts a probe of the phone's link (PHONE PROBE, root): SDP finds the
 * phone's MAS (0x1132) or PSE (0x112F), RFCOMM opens a DLC to it, OBEX
 * connects, gets a folder listing and disconnects; the answer hook hears
 * how.  Returns 0 when it started, ENOTCONN when the device is not the
 * phone of a link, EINVAL for another service class, or EBUSY while a
 * probe runs.
 */
int
btd_phone_probe(
	struct btd_phone *phone,
	const uint8_t *address,
	uint16_t uuid,
	uint64_t now)
{
	uint8_t request[16];
	size_t length;
	int same;
	int error;

	/* The phone of a link that is ready. */
	if (phone->state != BTD_PHONE_READY)
		return ENOTCONN;
	same = memcmp(phone->address, address, BTD_ADDRESS_BYTES);
	if (same != 0)
		return ENOTCONN;

	/* MAP's MAS or PBAP's PSE. */
	if (uuid != BTD_SDP_UUID_MAS && uuid != BTD_SDP_UUID_PSE)
		return EINVAL;

	/* One probe, and one SDP query, at a time. */
	if (phone->probe.step != BTD_PHONE_PROBE_NONE || phone->sdp_cid != 0U)
		return EBUSY;

	/* The probe, for BTD_PHONE_PROBE_MS at most. */
	memset(&phone->probe, 0, sizeof(phone->probe));
	phone->probe.uuid = uuid;
	phone->probe.deadline = now + BTD_PHONE_PROBE_MS;
	phone->rfcomm_tries = 0U;
	phone->rfcomm_retry_at = 0U;

	/* SDP's channel first: its Connection Request. */
	error = btd_l2cap_connect(&phone->l2cap, phone->handle, PHONE_PSM_SDP, request, sizeof(request), &length, &phone->sdp_cid);
	if (error != 0)
		return EBUSY;
	phone->probe.step = BTD_PHONE_PROBE_SDP_CHANNEL;
	(void)phone_send(phone, BTD_CID_SIGNALLING, request, length);

	/* Succeeded: under way. */
	return 0;
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

	/* Succeeded: ended by the user. */
	phone_disconnect(phone, PHONE_REASON_USER);
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

	/* Succeeded: the HID host's limit follows the record. */
	phone_limit(phone);
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
	struct btd_phonerec record;
	int valid;
	int same;
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
	phone_limit(phone);

	/* Off ends the phone's link. */
	same = memcmp(phone->address, address, BTD_ADDRESS_BYTES);
	if (!on && same == 0)
		phone_disconnect(phone, PHONE_REASON_USER);

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

	/* The HID host's limit follows. */
	phone_limit(phone);

	/* The phone's link ends. */
	same = memcmp(phone->address, address, BTD_ADDRESS_BYTES);
	if (same == 0)
		phone_disconnect(phone, PHONE_REASON_USER);

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

/* Takes an event of the phone's link: its end, and its encryption turned off. */
static void
phone_event(
	struct btd_phone *phone,
	const uint8_t *parameters,
	size_t length,
	uint8_t code)
{
	uint16_t handle;

	/* An event of a link with its status and handle at all, of the phone's link. */
	if (length < 3U || phone->state == BTD_PHONE_NONE)
		return;
	handle = (uint16_t)(phone_get16(parameters + 1) & 0x0fffU);
	if (handle != phone->handle)
		return;

	/* Disconnection Complete that happened: the link went. */
	if (code == PHONE_EVENT_DISCONNECTED) {
		if (parameters[0] == 0x00U)
			phone_ended(phone, "lost");
		return;
	}

	/* Encryption turned off: the phone's data is not sent in the clear. */
	if (code == PHONE_EVENT_ENCRYPTION || code == PHONE_EVENT_ENCRYPTION_V2) {
		if (length >= 4U &&
		    parameters[0] == 0x00U &&
		    parameters[3] == 0x00U) {
			phone->encrypted = 0;
			phone_disconnect(phone, PHONE_REASON_SECURITY);
		}
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

	/* The channels that closed; our RFCOMM channel refused (the phone's request crossed it, section 8.2) is asked for again later. */
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

	/* Succeeded: those that opened. */
	for (index = 0U; index < effect.opened_count; index++)
		phone_opened(phone, effect.opened[index]);

	/* Our refused RFCOMM channel: asked for again after a wait, three times at most, unless the phone's opened meanwhile. */
	if (phone->rfcomm_refused) {
		phone->rfcomm_refused = 0;
		if (phone->rfcomm_cid != 0U || phone->probe.step != BTD_PHONE_PROBE_RFCOMM)
			return;
		if (phone->rfcomm_tries >= PHONE_RFCOMM_TRIES) {
			phone_probe_fail(phone, "rfcomm-refused");
			return;
		}

		/* The wait: 100 to 500 ms, spread by the clock so both sides do not ask again together. */
		now = btd_now_ms();
		phone->rfcomm_retry_at = now + PHONE_RFCOMM_WAIT_MS + (now % PHONE_RFCOMM_SPREAD_MS);
	}
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

	/* Our SDP channel: the query's first request. */
	if (channel->psm == PHONE_PSM_SDP && cid == phone->sdp_cid) {
		phone->sdp_transaction++;
		btd_sdp_init(&phone->sdp, phone->probe.uuid, phone->sdp_transaction);
		phone->probe.step = BTD_PHONE_PROBE_SDP;
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

	/* The phone's while ours is asked for. */
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

	/* Ours: bluetoothd starts the multiplexer (the phone's waits for the phone's SABM). */
	if (!channel->inbound) {
		error = btd_rfcomm_start(&phone->rfcomm, btd_now_ms());
		if (error != 0) {
			phone_close_channel(phone, cid);
			return;
		}
	}

	/* Succeeded: a probe that waits for the session opens its DLC. */
	if (phone->probe.step == BTD_PHONE_PROBE_RFCOMM && phone->probe.dlci == 0U)
		phone_probe_dlc(phone);
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

	/* The SDP query's: a probe still asking fails. */
	if (phone->sdp_cid != 0U && cid == phone->sdp_cid) {
		phone->sdp_cid = 0U;
		if (phone->probe.step == BTD_PHONE_PROBE_SDP_CHANNEL || phone->probe.step == BTD_PHONE_PROBE_SDP)
			phone_probe_fail(phone, "sdp");
		return;
	}

	/* Not RFCOMM's. */
	if (phone->rfcomm_cid == 0U || cid != phone->rfcomm_cid)
		return;

	/* RFCOMM's, before it opened: a probe that waits for it fails. */
	phone->rfcomm_cid = 0U;
	if (!phone->rfcomm_active) {
		if (phone->probe.step == BTD_PHONE_PROBE_RFCOMM)
			phone_probe_fail(phone, "rfcomm");
		return;
	}

	/* Succeeded: the session's DLCs are lost (their owners hear it, a probe fails). */
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
		phone_disconnect(phone, PHONE_REASON_USER);
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
		phone_disconnect(phone, PHONE_REASON_USER);
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
	phone_disconnect(phone, PHONE_REASON_USER);
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

/* Ends the phone's link with a reason: Disconnect; the end comes with its Disconnection Complete. */
static void
phone_disconnect(
	struct btd_phone *phone,
	uint8_t reason)
{
	uint8_t disconnect[3];
	int error;

	/* Ending already. */
	if (phone->state != BTD_PHONE_READY)
		return;

	/* Disconnect. */
	phone_put16(disconnect, phone->handle);
	disconnect[2] = reason;
	error = btd_session_command(phone->session, PHONE_DISCONNECT, disconnect, sizeof(disconnect));
	if (error != 0) {
		phone_ended(phone, "lost");
		return;
	}

	/* Succeeded: closing. */
	phone->state = BTD_PHONE_CLOSING;
}

/* Forgets the phone's link: a probe ends with why, RFCOMM's DLCs are lost, the HID host may use every link again. */
static void
phone_ended(
	struct btd_phone *phone,
	const char *why)
{
	unsigned index;

	/* Closing from now on (nothing more is sent on the link), and a probe under way ends. */
	phone->state = BTD_PHONE_CLOSING;
	if (phone->probe.step != BTD_PHONE_PROBE_NONE)
		phone_probe_fail(phone, why);

	/* RFCOMM's session goes. */
	if (phone->rfcomm_active) {
		phone->rfcomm_active = 0;
		btd_rfcomm_lost(&phone->rfcomm);
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

	/* Succeeded: no phone (the HID host's limit follows the record, not the link: ws197-p003 section 3.4). */
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

/* Sends the SDP query's next request; one that cannot be built fails the probe. */
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
		phone_probe_fail(phone, "sdp");
		return;
	}

	/* Succeeded: sent on the query's channel. */
	(void)phone_send(phone, phone->sdp_cid, request, length);
}

/*
 * Takes an answer of the SDP query: the next request, or the phone's
 * server channel of the class asked for, then the query's channel closes
 * and RFCOMM comes next.
 */
static void
phone_sdp_input(
	struct btd_phone *phone,
	const uint8_t *pdu,
	size_t length)
{
	unsigned channel;
	int meant;
	int error;

	/* Only while the probe asks. */
	if (phone->probe.step != BTD_PHONE_PROBE_SDP)
		return;

	/* The answer: more to ask, the lists whole, or failed. */
	meant = btd_sdp_input(&phone->sdp, pdu, length);
	if (meant == BTD_SDP_MORE) {
		phone_sdp_send(phone);
		return;
	}

	/* A failed query. */
	if (meant != BTD_SDP_DONE) {
		phone_probe_fail(phone, "sdp");
		return;
	}

	/* The server channel of the first record of the class. */
	error = btd_sdp_rfcomm_channel(&phone->sdp, phone->probe.uuid, 0U, &channel);
	if (error != 0) {
		phone_probe_fail(phone, "no-service");
		return;
	}

	/* Succeeded: RFCOMM next (the step moves first: the query's channel closing does not fail the probe). */
	phone->probe.channel = channel;
	phone->probe.step = BTD_PHONE_PROBE_RFCOMM;
	phone_close_channel(phone, phone->sdp_cid);
	phone_rfcomm_open(phone);
}

/*
 * Brings the probe to RFCOMM: the DLC at once on a session there is, else
 * bluetoothd's RFCOMM channel asked for (the session starts when it
 * opens).
 */
static void
phone_rfcomm_open(
	struct btd_phone *phone)
{
	uint8_t request[16];
	size_t length;
	int error;

	/* Only while the probe waits for RFCOMM. */
	if (phone->probe.step != BTD_PHONE_PROBE_RFCOMM)
		return;

	/* A session there is: the DLC. */
	if (phone->rfcomm_active) {
		phone_probe_dlc(phone);
		return;
	}

	/* A channel asked for already (ours, or the phone's opening). */
	if (phone->rfcomm_cid != 0U)
		return;

	/* Succeeded: our RFCOMM channel asked for. */
	phone->rfcomm_tries++;
	error = btd_l2cap_connect(&phone->l2cap, phone->handle, PHONE_PSM_RFCOMM, request, sizeof(request), &length, &phone->rfcomm_cid);
	if (error != 0) {
		phone->rfcomm_cid = 0U;
		phone_probe_fail(phone, "rfcomm");
		return;
	}

	/* Sent on the signalling channel. */
	(void)phone_send(phone, BTD_CID_SIGNALLING, request, length);
}

/* Opens the probe's DLC to the phone's server channel. */
static void
phone_probe_dlc(
	struct btd_phone *phone)
{
	unsigned dlci;
	int error;

	/* The DLC (opened tells when OBEX may connect). */
	error = btd_rfcomm_connect(&phone->rfcomm, phone->probe.channel, btd_now_ms(), &dlci);
	if (error != 0) {
		phone_probe_fail(phone, "rfcomm");
		return;
	}

	/* Succeeded: waiting for it. */
	phone->probe.dlci = dlci;
}

/* Ends a probe that failed: the answer hook hears why, and its DLC and SDP channel close. */
static void
phone_probe_fail(
	struct btd_phone *phone,
	const char *why)
{
	char line[BTD_PHONE_ANSWER_MAX];
	unsigned dlci;
	uint16_t sdp_cid;

	/* No probe under way. */
	if (phone->probe.step == BTD_PHONE_PROBE_NONE)
		return;

	/* Over from now on (closing its parts below calls back here and finds none). */
	dlci = phone->probe.dlci;
	sdp_cid = phone->sdp_cid;
	phone->probe.step = BTD_PHONE_PROBE_NONE;
	phone->probe.dlci = 0U;
	phone->rfcomm_retry_at = 0U;

	/* Its DLC, and its SDP channel. */
	if (dlci != 0U && phone->rfcomm_active)
		(void)btd_rfcomm_close(&phone->rfcomm, dlci, btd_now_ms());
	if (sdp_cid != 0U && phone->state == BTD_PHONE_READY)
		phone_close_channel(phone, sdp_cid);

	/* Succeeded: why, to the one who asked. */
	(void)snprintf(line, sizeof(line), "ERROR %s", why);
	if (phone->answer != NULL)
		phone->answer(phone->answer_context, line);
}

/* Ends a probe that went through: each step's result and the listing's size, never its content (p001 R22), and the DLC closes. */
static void
phone_probe_finish(
	struct btd_phone *phone)
{
	char line[BTD_PHONE_ANSWER_MAX];
	unsigned dlci;

	/* Over from now on. */
	dlci = phone->probe.dlci;
	phone->probe.step = BTD_PHONE_PROBE_NONE;
	phone->probe.dlci = 0U;

	/* The DLC closes. */
	if (dlci != 0U && phone->rfcomm_active)
		(void)btd_rfcomm_close(&phone->rfcomm, dlci, btd_now_ms());

	/* Succeeded: the line. */
	(void)snprintf(line,
		       sizeof(line),
		       "PROBE uuid=0x%04x channel=%u connect=0x%02x get=0x%02x bytes=%u disconnect=0x%02x",
		       (unsigned)phone->probe.uuid,
		       phone->probe.channel,
		       (unsigned)phone->probe.connect_code,
		       (unsigned)phone->probe.get_code,
		       (unsigned)phone->probe.bytes,
		       (unsigned)phone->probe.disconnect_code);
	if (phone->answer != NULL)
		phone->answer(phone->answer_context, line);
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

/* RFCOMM's question whether a server channel of bluetoothd's is offered: none in this Phase (MNS and HF come with their profiles). */
static int
phone_rf_accept(
	void *context,
	unsigned server_channel)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(server_channel);

	/* Not offered. */
	return 0;
}

/* RFCOMM's DLC opened: the probe's connects OBEX to the class's Target. */
static void
phone_rf_opened(
	void *context,
	unsigned dlci)
{
	struct btd_phone *phone;
	struct btd_obex_events events;
	const uint8_t *target;
	int error;

	/* The probe's DLC. */
	phone = context;
	if (phone->probe.step != BTD_PHONE_PROBE_RFCOMM || dlci != phone->probe.dlci)
		return;

	/* An OBEX client on it. */
	memset(&events, 0, sizeof(events));
	events.context = phone;
	events.write = phone_ob_write;
	events.done = phone_ob_done;
	events.body = phone_ob_body;
	btd_obex_init(&phone->obex, &events, BTD_OBEX_CLIENT);

	/* Connect with MAS's or PSE's Target. */
	target = phone_target_mas;
	if (phone->probe.uuid == BTD_SDP_UUID_PSE)
		target = phone_target_pse;
	phone->probe.step = BTD_PHONE_PROBE_CONNECT;
	error = btd_obex_connect(&phone->obex, target, sizeof(phone_target_mas), btd_now_ms());
	if (error != 0)
		phone_probe_fail(phone, "obex");
}

/* RFCOMM's data of a DLC: the probe's goes to OBEX. */
static void
phone_rf_data(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length)
{
	struct btd_phone *phone;

	/* The probe's DLC while OBEX runs. */
	phone = context;
	if (phone->probe.step < BTD_PHONE_PROBE_CONNECT || dlci != phone->probe.dlci)
		return;

	/* Succeeded: to OBEX. */
	btd_obex_input(&phone->obex, data, length, btd_now_ms());
}

/* RFCOMM's DLC writable again: OBEX's rest goes. */
static void
phone_rf_writable(
	void *context,
	unsigned dlci)
{
	struct btd_phone *phone;

	/* The probe's DLC while OBEX runs. */
	phone = context;
	if (phone->probe.step < BTD_PHONE_PROBE_CONNECT || dlci != phone->probe.dlci)
		return;

	/* Succeeded: OBEX's rest. */
	btd_obex_pump(&phone->obex);
}

/* RFCOMM's DLC closed: the probe's, before its end, fails. */
static void
phone_rf_closed(
	void *context,
	unsigned dlci,
	int reason)
{
	struct btd_phone *phone;

	UNUSED_PARAMETER(reason);

	/* The probe's DLC. */
	phone = context;
	if (phone->probe.step == BTD_PHONE_PROBE_NONE || dlci != phone->probe.dlci)
		return;

	/* Succeeded: the probe fails (its DLC is gone already). */
	phone->probe.dlci = 0U;
	phone_probe_fail(phone, "rfcomm");
}

/* RFCOMM's session ended: its L2CAP channel closes. */
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

	/* A probe on the session fails. */
	if (phone->probe.step >= BTD_PHONE_PROBE_RFCOMM)
		phone_probe_fail(phone, "rfcomm");

	/* Succeeded: the channel closes (its DLCs were told). */
	cid = phone->rfcomm_cid;
	phone->rfcomm_active = 0;
	phone->rfcomm_cid = 0U;
	phone_close_channel(phone, cid);
}

/* OBEX's write hook: bytes on the probe's DLC, as far as RFCOMM's credits take them. */
static int
phone_ob_write(
	void *context,
	const uint8_t *data,
	size_t length,
	size_t *written)
{
	struct btd_phone *phone;
	int error;

	/* The probe's DLC on the session. */
	phone = context;
	*written = 0U;
	if (!phone->rfcomm_active || phone->probe.dlci == 0U)
		return ENOTCONN;

	/* Written as far as it goes. */
	error = btd_rfcomm_write(&phone->rfcomm, phone->probe.dlci, data, length, written);
	if (error != 0)
		return error;

	/* Succeeded: what went is in written. */
	return 0;
}

/* OBEX's operation ended: Connect leads to the Get, the Get to Disconnect, Disconnect to the probe's end. */
static void
phone_ob_done(
	void *context,
	unsigned operation,
	int error,
	uint8_t code,
	const uint8_t *headers,
	size_t length)
{
	struct btd_phone *phone;
	struct btd_obex_writer writer;
	uint8_t request[PHONE_HEADERS_MAX];
	int failed;

	UNUSED_PARAMETER(headers);
	UNUSED_PARAMETER(length);

	/* The probe's connection. */
	phone = context;
	if (phone->probe.step < BTD_PHONE_PROBE_CONNECT)
		return;

	/* Connect: accepted, or the probe fails. */
	if (operation == BTD_OBEX_OP_CONNECT) {
		phone->probe.connect_code = code;
		if (error != 0 || code != BTD_OBEX_SUCCESS) {
			phone_probe_fail(phone, "obex-connect");
			return;
		}

		/* The folder listing: its Type alone (no Name, the current folder). */
		btd_obex_writer_init(&writer, request, sizeof(request));
		btd_obex_put_bytes(&writer, BTD_OBEX_TYPE, (const uint8_t *)PHONE_LISTING_TYPE, PHONE_LISTING_TYPE_BYTES);
		phone->probe.step = BTD_PHONE_PROBE_GET;
		failed = btd_obex_get(&phone->obex, request, writer.used, BTD_PHONE_LISTING_MAX, btd_now_ms());
		if (failed != 0)
			phone_probe_fail(phone, "obex-get");
		return;
	}

	/* The Get: its response code kept whatever it is (a refusal is an answer too), then Disconnect; no response at all fails. */
	if (operation == BTD_OBEX_OP_GET) {
		phone->probe.get_code = code;
		if (error != 0 && code == 0U) {
			phone_probe_fail(phone, "obex-get");
			return;
		}

		/* Disconnect. */
		phone->probe.step = BTD_PHONE_PROBE_DISCONNECT;
		failed = btd_obex_disconnect(&phone->obex, btd_now_ms());
		if (failed != 0)
			phone_probe_fail(phone, "obex-disconnect");
		return;
	}

	/* Not the probe's last step. */
	if (operation != BTD_OBEX_OP_DISCONNECT)
		return;

	/* Succeeded: Disconnect's answer ends the probe. */
	phone->probe.disconnect_code = code;
	if (error != 0) {
		phone_probe_fail(phone, "obex-disconnect");
		return;
	}

	/* The answer. */
	phone_probe_finish(phone);
}

/* OBEX's body of the listing: counted, never kept (p001 R22). */
static int
phone_ob_body(
	void *context,
	const uint8_t *data,
	size_t length)
{
	struct btd_phone *phone;

	UNUSED_PARAMETER(data);

	/* Counted; the Get goes on. */
	phone = context;
	phone->probe.bytes += length;
	return 0;
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
