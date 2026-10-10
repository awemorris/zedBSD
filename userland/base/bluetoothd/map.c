/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's Message Access Profile, the client side (ws197-p003, see
 * map.h).
 *
 * OBEX's events come inside the DLC's input, so they only note what
 * happened; map_settle does the rest once the input is over, at the end
 * of every entry point: it fails MAP, handles what an operation answered,
 * the events of the MNS, and starts the next operation.
 */

#include "userland/base/bluetoothd/map.h"

#include "userland/base/bluetoothd/phoneio.h"
#include "userland/base/bluetoothd/rfcomm.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* bluetoothd's RFCOMM server channel of the MNS (as the SDP record says). */
#define MAP_MNS_CHANNEL			16U

/* The attributes of a MAS record (Assigned Numbers): its instance, its message types, its features. */
#define MAP_ATTRIBUTE_INSTANCE		0x0315U
#define MAP_ATTRIBUTE_TYPES		0x0316U
#define MAP_ATTRIBUTE_FEATURES		0x0317U

/* The message types of a MAS record (SupportedMessageTypes): GSM/CDMA SMS and MMS. */
#define MAP_TYPES_SMS_GSM		0x02U
#define MAP_TYPES_SMS_CDMA		0x04U
#define MAP_TYPES_MMS			0x08U

/*
 * The features of a MAS record (MapSupportedFeatures): notification
 * registration, notification, uploading; and what a MAS without the
 * attribute is taken to have (MAP section 7.1.1).
 */
#define MAP_FEATURE_REGISTRATION	0x01U
#define MAP_FEATURE_NOTIFICATION	0x02U
#define MAP_FEATURE_UPLOADING		0x08U
#define MAP_FEATURES_ASSUMED		0x1fU

/*
 * The tags of MAP's application parameters (MAP section 6.3.1): the most
 * a listing gives, where it starts, the types left out, the time it
 * starts from, attachments, notifications, the MAS's instance, the
 * listing's fields, its size, the character set, the status set and its
 * value, the phone's time.
 */
#define MAP_TAG_MAX_LIST_COUNT		0x01U
#define MAP_TAG_START_OFFSET		0x02U
#define MAP_TAG_FILTER_TYPE		0x03U
#define MAP_TAG_FILTER_BEGIN		0x04U
#define MAP_TAG_ATTACHMENT		0x0aU
#define MAP_TAG_NOTIFICATION		0x0eU
#define MAP_TAG_INSTANCE		0x0fU
#define MAP_TAG_PARAMETER_MASK		0x10U
#define MAP_TAG_LISTING_SIZE		0x12U
#define MAP_TAG_CHARSET			0x14U
#define MAP_TAG_STATUS_INDICATOR	0x17U
#define MAP_TAG_STATUS_VALUE		0x18U
#define MAP_TAG_MSE_TIME		0x19U

/*
 * The values a listing asks with: e-mail left out (IM's bit is
 * reserved for a 1.1 phone), and the fields wanted: datetime, the
 * sender's and the recipient's names and addresses, type, reception
 * status and read.
 */
#define MAP_FILTER_NOT_TEXT		0x04U
#define MAP_PARAMETER_MASK		0x0000117eUL

/* A message's text in UTF-8, without attachments; the read status set, as unread or read. */
#define MAP_CHARSET_UTF8		0x01U
#define MAP_STATUS_READ_INDICATOR	0x00U
#define MAP_STATUS_UNREAD		0x00U
#define MAP_STATUS_READ			0x01U

/* The one byte the body of a status or registration Put carries (MAP section 5.7). */
#define MAP_FILLER			0x30U

/* The room of the application parameters of one request. */
#define MAP_PARAMETERS_MAX		64U

/* The room of the headers of one request. */
#define MAP_HEADERS_MAX			256U

/* What follows a failure: a wait that grows, the long wait after a refusal, or no new attempt. */
#define MAP_RETRY_STEP			1U
#define MAP_RETRY_REFUSED		2U
#define MAP_RETRY_NEVER			3U

/* The states told of a sent message, as bits. */
#define MAP_TOLD_SENT			0x01U
#define MAP_TOLD_DELIVERED		0x02U
#define MAP_TOLD_FAILED			0x04U

/* The 64-bit FNV-1a hash's start and prime, which make a message's key. */
#define MAP_FNV_BASIS			0xcbf29ce484222325ULL
#define MAP_FNV_PRIME			0x100000001b3ULL

/* MAP's MAS Target (MAP 1.4.2 section 6.4.1, bb582b40-420c-11db-b0de-0800200c9a66). */
static const uint8_t map_mas_uuid[16] = {
	0xbbU, 0x58U, 0x2bU, 0x40U, 0x42U, 0x0cU, 0x11U, 0xdbU,
	0xb0U, 0xdeU, 0x08U, 0x00U, 0x20U, 0x0cU, 0x9aU, 0x66U
};

/* MAP's MNS Target (bb582b41-420c-11db-b0de-0800200c9a66). */
static const uint8_t map_mns_uuid[16] = {
	0xbbU, 0x58U, 0x2bU, 0x41U, 0x42U, 0x0cU, 0x11U, 0xdbU,
	0xb0U, 0xdeU, 0x08U, 0x00U, 0x20U, 0x0cU, 0x9aU, 0x66U
};

/* The names of the folders a synchronisation reads, by number. */
static const char *const map_folder_names[BTD_MAP_FOLDERS] = {
	"inbox",
	"sent"
};

static uint64_t map_now(const struct btd_map *map);
static void map_log(struct btd_map *map, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void map_changed(struct btd_map *map);
static void map_start(struct btd_map *map);
static void map_fail(struct btd_map *map, const char *why, unsigned retry);
static void map_close_all(struct btd_map *map);
static void map_close_later(struct btd_map *map, unsigned dlci);
static void map_end_requests(struct btd_map *map, const char *word);
static void map_shutdown(struct btd_map *map);
static void map_settle(struct btd_map *map);
static void map_setup_finished(struct btd_map *map);
static void map_setpath(struct btd_map *map);
static void map_register_or_ready(struct btd_map *map);
static void map_become_ready(struct btd_map *map);
static int map_enqueue(struct btd_map *map, const struct btd_map_op *op);
static void map_op_init(struct btd_map_op *op, unsigned kind);
static void map_next(struct btd_map *map);
static int map_run(struct btd_map *map, struct btd_map_op *op);
static int map_run_listing(struct btd_map *map, struct btd_map_op *op);
static int map_run_get(struct btd_map *map, const struct btd_map_op *op);
static int map_run_status(struct btd_map *map, const struct btd_map_op *op, uint8_t value);
static int map_run_push(struct btd_map *map, const struct btd_map_op *op);
static int map_run_register(struct btd_map *map);
static void map_op_finished(struct btd_map *map);
static void map_count_done(struct btd_map *map, struct btd_map_op *op);
static void map_list_done(struct btd_map *map, struct btd_map_op *op);
static void map_get_done(struct btd_map *map, struct btd_map_op *op);
static void map_unread_done(struct btd_map *map, struct btd_map_op *op);
static void map_read_done(struct btd_map *map, struct btd_map_op *op);
static void map_page_get_done(struct btd_map *map, struct btd_map_op *op);
static void map_locate_done(struct btd_map *map, struct btd_map_op *op);
static void map_live_get_done(struct btd_map *map, struct btd_map_op *op);
static void map_push_done(struct btd_map *map, struct btd_map_op *op);
static void map_page_continue(struct btd_map *map, unsigned index);
static void map_page_end(struct btd_map *map, unsigned index, const char *word);
static void map_page_free(struct btd_map *map, unsigned index);
static void map_live_start(struct btd_map *map, uint64_t handle, unsigned folder);
static void map_live_search(struct btd_map *map, unsigned index);
static void map_live_free(struct btd_map *map, unsigned index);
static void map_events(struct btd_map *map);
static void map_event(struct btd_map *map, const struct btd_map_event *event);
static int map_event_own(struct btd_map *map, const struct btd_map_event *event);
static void map_hold(struct btd_map *map, const struct btd_map_event *event);
static void map_held_answered(struct btd_map *map, uint64_t handle);
static void map_sent_tell(struct btd_map *map, struct btd_map_sent *sent, unsigned told, const char *state);
static struct btd_map_sent *map_sent_find(struct btd_map *map, uint64_t handle);
static void map_dropped(struct btd_map *map);
static int map_folder_of(const char *folder, unsigned *number);
static const uint8_t *map_message_body(const struct btd_bmsg *message, size_t *length);
static int map_item(struct btd_map *map, char *line, size_t size, uint64_t handle, unsigned folder, const struct btd_map_entry *entry);
static uint64_t map_key(unsigned folder, const char *datetime, const char *peer, const uint8_t *text, size_t length);
static uint64_t map_hash(uint64_t hash, const uint8_t *bytes, size_t length);
static const char *map_error_word(int error, uint8_t code);
static int map_parse_cursor(const char *cursor, uint32_t *session, uint64_t *since, unsigned *folder, unsigned *offset, int *capped);
static int map_parse_handle(const char *text, uint32_t *session, uint64_t *handle);
static int map_hex(const char *text, size_t length, uint64_t *value);
static void map_period(struct btd_map *map, int64_t since, char *text, size_t size, int *guessed, int32_t *offset);
static void map_parameter(uint8_t *bytes, size_t *used, uint8_t tag, const uint8_t *value, size_t length);
static void map_parameter16(uint8_t *bytes, size_t *used, uint8_t tag, unsigned value);
static void map_parameter8(uint8_t *bytes, size_t *used, uint8_t tag, unsigned value);
static int map_mas_write(void *context, const uint8_t *data, size_t length, size_t *written);
static void map_mas_done(void *context, unsigned operation, int error, uint8_t code, const uint8_t *headers, size_t length);
static void map_mas_response(void *context, unsigned operation, uint8_t code, const uint8_t *headers, size_t length);
static int map_mas_body(void *context, const uint8_t *data, size_t length);
static int map_mns_write(void *context, const uint8_t *data, size_t length, size_t *written);
static int map_mns_target(void *context, const uint8_t *target, size_t length);
static uint8_t map_mns_put(void *context, const uint8_t *headers, size_t length, const uint8_t *body, size_t body_length);
static int map_name_handle(const struct btd_obex_header *header, uint64_t *handle);
static uint64_t map_earlier(uint64_t earliest, uint64_t deadline);

/*
 * Prepares the MAP client: off, nothing queued; the sessions of its MAS
 * connections and its sent messages' request numbers count from
 * first_session (a number the daemon draws at its start, so a handle or a
 * request of an earlier run is not taken for one of this run).
 */
void
btd_map_init(
	struct btd_map *map,
	const struct btd_map_hooks *hooks,
	uint32_t first_session)
{
	/* Nothing under way. */
	memset(map, 0, sizeof(*map));
	map->hooks = *hooks;
	map->state = BTD_MAP_OFF;
	map->session = first_session;
	map->next_request = first_session;
}

/*
 * Takes the phone link's readiness (the profile's ready hook): MAP starts
 * when the owner wants messages.
 */
void
btd_map_ready(
	void *context)
{
	struct btd_map *map;
	int wanted;

	/* The link is there for MAP. */
	map = context;
	map->link_ready = 1;
	map->retry_step = 0U;

	/* Started when messages are wanted. */
	wanted = map->hooks.wanted(map->hooks.context);
	if (wanted && map->state == BTD_MAP_OFF)
		map_start(map);

	/* Succeeded: what follows is settled. */
	map_settle(map);
}

/*
 * Takes the end of the phone link (the profile's ended hook): every
 * request is answered as lost and MAP is off until the next link.
 */
void
btd_map_ended(
	void *context)
{
	struct btd_map *map;

	/* The link is gone. */
	map = context;
	map->link_ready = 0;

	/* Succeeded: off. */
	map_shutdown(map);
}

/*
 * Takes the end of the SDP query for the MAS (the profile's sdp_done
 * hook): the first record that offers SMS is chosen, and its DLC asked
 * for.
 */
void
btd_map_sdp_done(
	void *context,
	const struct btd_sdp *sdp,
	int error)
{
	struct btd_map *map;
	unsigned count;
	unsigned nth;
	unsigned channel;
	uint32_t types;
	uint32_t value;
	int found;

	/* Only the query MAP asked for. */
	map = context;
	if (map->state != BTD_MAP_SDP)
		return;

	/* A query that failed: tried again later. */
	if (error != 0) {
		map_fail(map, "sdp", MAP_RETRY_STEP);
		map_settle(map);
		return;
	}

	/* Each MAS record until one offers SMS and has a channel. */
	found = 0;
	channel = 0U;
	types = 0U;
	count = btd_sdp_records(sdp, BTD_SDP_UUID_MAS);
	for (nth = 0U; nth < count; nth++) {
		/* Its message types: SMS of GSM or CDMA. */
		error = btd_sdp_uint_attribute(sdp, BTD_SDP_UUID_MAS, nth, MAP_ATTRIBUTE_TYPES, &types);
		if (error != 0)
			continue;
		if ((types & (MAP_TYPES_SMS_GSM | MAP_TYPES_SMS_CDMA | MAP_TYPES_MMS)) == 0U)
			continue;

		/* Its RFCOMM channel. */
		error = btd_sdp_rfcomm_channel(sdp, BTD_SDP_UUID_MAS, nth, &channel);
		if (error != 0)
			continue;

		/* This one. */
		found = 1;
		break;
	}

	/* No MAS for SMS: not tried again on this link. */
	if (!found) {
		map_fail(map, "no-mas", MAP_RETRY_NEVER);
		map_settle(map);
		return;
	}

	/* The instance, and the features (all of MAP 1.0's when not said). */
	map->mas_channel = channel;
	map->message_types = types;
	map->mas_instance = 0U;
	error = btd_sdp_uint_attribute(sdp, BTD_SDP_UUID_MAS, nth, MAP_ATTRIBUTE_INSTANCE, &value);
	if (error == 0)
		map->mas_instance = value;
	map->features = MAP_FEATURES_ASSUMED;
	error = btd_sdp_uint_attribute(sdp, BTD_SDP_UUID_MAS, nth, MAP_ATTRIBUTE_FEATURES, &value);
	if (error == 0)
		map->features = value;

	/*
	 * The DLC to the MAS.  A channel whose earlier DLC is still closing is
	 * busy (ws197-p005 section 3.1): the setting up starts again shortly,
	 * from the SDP query.
	 */
	map->state = BTD_MAP_OPENING;
	error = map->hooks.dlc_open(map->hooks.context, channel);
	if (error == EBUSY) {
		map->state = BTD_MAP_SDP;
		map->sdp_again_at = map_now(map) + BTD_MAP_BUSY_MS;
	} else if (error != 0) {
		map_fail(map, "rfcomm", MAP_RETRY_STEP);
	}

	/* Succeeded: what follows is settled. */
	map_settle(map);
}

/*
 * Tells RFCOMM whether a server channel of bluetoothd's is offered to the
 * phone now (the profile's accept hook): the MNS's, while MAP is being set
 * up or ready and no MNS DLC is open.
 */
int
btd_map_accept(
	void *context,
	unsigned server_channel)
{
	struct btd_map *map;

	/* The MNS's channel. */
	map = context;
	if (server_channel != MAP_MNS_CHANNEL)
		return 0;

	/* MAP under way. */
	if (map->state < BTD_MAP_OPENING || map->state > BTD_MAP_READY)
		return 0;

	/* One MNS DLC at a time. */
	if (map->mns_dlci != 0U)
		return 0;

	/* Succeeded: offered. */
	return 1;
}

/*
 * Takes a DLC that opened (the profile's opened hook): bluetoothd's own to
 * the MAS carries OBEX's Connect at once; the phone's to the MNS gets an
 * OBEX server.  Any other is closed.
 */
void
btd_map_opened(
	void *context,
	unsigned dlci,
	unsigned server_channel,
	int ours)
{
	struct btd_obex_events events;
	struct btd_map *map;
	int error;

	/* The DLC to the MAS asked for. */
	map = context;
	if (ours &&
	    map->state == BTD_MAP_OPENING &&
	    server_channel == map->mas_channel &&
	    map->mas_dlci == 0U) {
		/* OBEX's client on it, waiting long for Connect's answer while the phone asks its user. */
		map->mas_dlci = dlci;
		memset(&events, 0, sizeof(events));
		events.context = map;
		events.write = map_mas_write;
		events.done = map_mas_done;
		events.response = map_mas_response;
		events.body = map_mas_body;
		btd_obex_init(&map->mas, &events, BTD_OBEX_CLIENT);
		btd_obex_set_timeout(&map->mas, BTD_MAP_CONNECT_MS);

		/* Connect with the MAS's Target. */
		map->state = BTD_MAP_CONNECTING;
		error = btd_obex_connect(&map->mas, map_mas_uuid, sizeof(map_mas_uuid), map_now(map));
		if (error != 0)
			map_fail(map, "obex", MAP_RETRY_STEP);
		map_settle(map);
		return;
	}

	/* The phone's DLC to the MNS. */
	if (!ours &&
	    server_channel == MAP_MNS_CHANNEL &&
	    map->mns_dlci == 0U) {
		/* OBEX's server on it, serving the MNS's Target. */
		map->mns_dlci = dlci;
		map->mns_connected = 0;
		memset(&events, 0, sizeof(events));
		events.context = map;
		events.write = map_mns_write;
		events.target = map_mns_target;
		events.put = map_mns_put;
		btd_obex_init(&map->mns, &events, BTD_OBEX_SERVER);
		return;
	}

	/* Succeeded: another DLC is not MAP's, and goes at the next tick. */
	map_close_later(map, dlci);
}

/* Takes the data of a DLC (the profile's data hook): to the OBEX connection on it. */
void
btd_map_data(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length)
{
	struct btd_map *map;
	uint64_t now;

	/* The MAS's or the MNS's. */
	map = context;
	now = map_now(map);
	if (dlci != 0U && dlci == map->mas_dlci) {
		btd_obex_input(&map->mas, data, length, now);
	} else if (dlci != 0U && dlci == map->mns_dlci) {
		map_log(map, "map: MNS input bytes=%lu", (unsigned long)length);
		btd_obex_input(&map->mns, data, length, now);
	}

	/* Succeeded: what OBEX told is settled. */
	map_settle(map);
}

/* Takes a DLC writable again (the profile's writable hook): its OBEX packet goes on. */
void
btd_map_writable(
	void *context,
	unsigned dlci)
{
	struct btd_map *map;

	/* The MAS's or the MNS's. */
	map = context;
	if (dlci != 0U && dlci == map->mas_dlci) {
		btd_obex_pump(&map->mas);
	} else if (dlci != 0U && dlci == map->mns_dlci) {
		btd_obex_pump(&map->mns);
	}

	/* Succeeded: what OBEX told is settled. */
	map_settle(map);
}

/*
 * Takes a DLC that closed (the profile's closed hook): the MAS's fails
 * MAP; the MAS's DLC asked for that closed without opening (the phone's
 * DM, or RFCOMM giving up, ws197-p005 section 3.2) fails MAP by its
 * reason; the MNS's alone asks the phone for notifications again (it
 * turned them off, MAP section 4.1) and waits for the MNS to come back.
 */
void
btd_map_closed(
	void *context,
	unsigned dlci,
	int reason)
{
	struct btd_map_op op;
	struct btd_map *map;
	const char *why;

	/* The MAS's: MAP fails, tried again later. */
	map = context;
	if (dlci != 0U && dlci == map->mas_dlci) {
		map->mas_dlci = 0U;
		map_fail(map, "closed", MAP_RETRY_STEP);
		map_settle(map);
		return;
	}

	/* The MAS's DLC asked for, closed before it opened: MAP fails by why, tried again later. */
	if (dlci != 0U &&
	    map->state == BTD_MAP_OPENING &&
	    map->mas_dlci == 0U &&
	    (dlci >> 1) == map->mas_channel) {
		why = "closed";
		if (reason == BTD_RFCOMM_CLOSED_REFUSED) {
			why = "refused";
		} else if (reason == BTD_RFCOMM_CLOSED_TIMEOUT) {
			why = "timeout";
		}

		/* Failed, and what follows settled. */
		map_fail(map, why, MAP_RETRY_STEP);
		map_settle(map);
		return;
	}

	/* Another DLC is not MAP's. */
	if (dlci == 0U || dlci != map->mns_dlci)
		return;

	/* The MNS's: gone, its notifications with it. */
	map->mns_dlci = 0U;
	map->mns_connected = 0;

	/* While ready with notifications: registered again, and the MNS awaited. */
	if (map->state == BTD_MAP_READY && map->notify) {
		map_op_init(&op, BTD_MAP_OP_REGISTER);
		op.urgent = 1;
		(void)map_enqueue(map, &op);
		map->mns_wait_until = map_now(map) + BTD_MAP_MNS_MS;
	}

	/* Succeeded: what follows is settled. */
	map_settle(map);
}

/* Takes a DLC that could not be asked for (the profile's open_failed hook): the MAS's fails MAP. */
void
btd_map_open_failed(
	void *context,
	unsigned server_channel)
{
	struct btd_map *map;

	/* The MAS's DLC asked for. */
	map = context;
	if (map->state != BTD_MAP_OPENING || server_channel != map->mas_channel)
		return;

	/* Succeeded: MAP fails, tried again later. */
	map_fail(map, "rfcomm", MAP_RETRY_STEP);
	map_settle(map);
}

/*
 * Starts a PHONE PAGE request (section 8.5, ws197-p004 section 5.1):
 * count messages from the folder and offset the cursor names (empty: the
 * inbox's first), of the time since on, at most limit messages of a
 * folder (0: no limit).  The answers go to token: the messages, PHONE
 * PAGE-END with the next cursor and whether a folder was cut at the
 * limit, DONE; or ERROR.  Returns NULL when it started, or the word of an
 * ERROR to answer now: not-ready, argument, stale-cursor (a cursor of
 * another session or since), busy.
 */
const char *
btd_map_page(
	struct btd_map *map,
	uint64_t token,
	int64_t since,
	unsigned limit,
	const char *cursor,
	unsigned count)
{
	struct btd_map_page *page;
	struct btd_map_op op;
	char line[96];
	uint32_t session;
	uint64_t cursor_since;
	unsigned folder;
	unsigned offset;
	unsigned index;
	int capped;
	int error;

	/* MAP ready. */
	if (map->state != BTD_MAP_READY)
		return "not-ready";

	/* A count, a limit and a time that can be asked. */
	if (count == 0U || count > BTD_MAP_PAGE_COUNT_MAX)
		return "argument";
	if (limit > BTD_MAP_FOLDER_LIMIT)
		return "argument";
	if (since < 0)
		return "argument";

	/* The folder and offset: the first, or the cursor's of this session and since. */
	folder = BTD_MAP_FOLDER_INBOX;
	offset = 0U;
	capped = 0;
	if (cursor[0] != '\0') {
		error = map_parse_cursor(cursor, &session, &cursor_since, &folder, &offset, &capped);
		if (error != 0)
			return "argument";
		if (session != map->session || cursor_since != (uint64_t)since)
			return "stale-cursor";
	}

	/* Past the last folder: nothing more. */
	if (folder >= BTD_MAP_FOLDERS) {
		(void)snprintf(line, sizeof(line), "PHONE PAGE-END cursor=%s more=0 count=0 skipped=0 capped=%d", cursor, capped);
		map->hooks.answer(map->hooks.context, token, line, NULL, 0U);
		map->hooks.answer(map->hooks.context, token, "DONE", NULL, 0U);
		return NULL;
	}

	/* A free page. */
	page = NULL;
	for (index = 0U; index < BTD_MAP_PAGES_MAX; index++) {
		if (!map->pages[index].used) {
			page = &map->pages[index];
			break;
		}
	}

	/* None free. */
	if (page == NULL)
		return "busy";

	/* Its first operation: the folder's count at its start, else its listing. */
	map_op_init(&op, BTD_MAP_OP_LIST);
	if (offset == 0U)
		op.kind = BTD_MAP_OP_COUNT;
	op.token = token;
	op.page = index;
	op.folder = folder;
	op.offset = offset;
	op.count = count;
	error = map_enqueue(map, &op);
	if (error != 0)
		return "busy";

	/* The page. */
	memset(page, 0, sizeof(*page));
	page->used = 1;
	page->token = token;
	page->since = since;
	page->folder = folder;
	page->offset = offset;
	page->count = count;
	page->limit = limit;
	page->capped = capped;

	/* Succeeded: under way. */
	map_settle(map);
	return NULL;
}

/*
 * Starts a PHONE READ request: the message the handle names is marked
 * read on the phone.  The answer goes to token: DONE, or ERROR.  Returns
 * NULL when it started, or the word of an ERROR to answer now:
 * not-ready, argument, stale (a handle of another session), busy.
 */
const char *
btd_map_read(
	struct btd_map *map,
	uint64_t token,
	const char *handle)
{
	struct btd_map_op op;
	uint32_t session;
	uint64_t value;
	int error;

	/* MAP ready. */
	if (map->state != BTD_MAP_READY)
		return "not-ready";

	/* A handle of this session. */
	error = map_parse_handle(handle, &session, &value);
	if (error != 0)
		return "argument";
	if (session != map->session)
		return "stale";

	/* Queued. */
	map_op_init(&op, BTD_MAP_OP_READ);
	op.token = token;
	op.handle = value;
	error = map_enqueue(map, &op);
	if (error != 0)
		return "busy";

	/* Succeeded: under way. */
	map_settle(map);
	return NULL;
}

/*
 * Starts a PHONE SEND request: the text (UTF-8) to the number, through
 * the phone's outbox.  The answer goes to token: PHONE SENT with its
 * request number and handle, DONE; or ERROR.  Returns NULL when it
 * started, or the word of an ERROR to answer now: not-ready, no-send (the
 * phone takes no message to send), number, argument (a text that is not
 * UTF-8), busy.
 */
const char *
btd_map_send(
	struct btd_map *map,
	uint64_t token,
	const char *number,
	const uint8_t *text,
	size_t length)
{
	struct btd_map_op op;
	uint8_t *message;
	size_t used;
	int type;
	int ok;
	int error;

	/* MAP ready, and the phone takes messages to send. */
	if (map->state != BTD_MAP_READY)
		return "not-ready";
	if (!map->send)
		return "no-send";

	/* A number a message can go to. */
	ok = btd_bmsg_number_ok(number);
	if (!ok)
		return "number";

	/* Room in the queue. */
	if (map->queue_count >= BTD_MAP_QUEUE_MAX)
		return "busy";

	/* The type the phone takes: GSM's unless it takes only CDMA's. */
	type = BTD_MAP_TYPE_SMS_GSM;
	if ((map->message_types & MAP_TYPES_SMS_GSM) == 0U)
		type = BTD_MAP_TYPE_SMS_CDMA;

	/* The bMessage's room. */
	message = malloc(BTD_BMSG_BUILD_MAX);
	if (message == NULL)
		return "busy";

	/* The bMessage. */
	error = btd_bmsg_build(number, text, length, type, message, BTD_BMSG_BUILD_MAX, &used);
	if (error != 0) {
		free(message);
		return "argument";
	}

	/* Queued, with its request number. */
	map->next_request++;
	map_op_init(&op, BTD_MAP_OP_PUSH);
	op.token = token;
	op.request = map->next_request;
	op.message = message;
	op.message_length = used;
	error = map_enqueue(map, &op);
	if (error != 0) {
		free(message);
		return "busy";
	}

	/* Succeeded: under way. */
	map_settle(map);
	return NULL;
}

/*
 * Forgets the requests of a client that went (the token's): its queued
 * operations go, its pages end quietly (an operation running for it ends
 * without an answer).
 */
void
btd_map_cancel(
	struct btd_map *map,
	uint64_t token)
{
	unsigned index;
	unsigned kept;

	/* No client. */
	if (token == 0U)
		return;

	/* The queued operations of the token go. */
	kept = 0U;
	for (index = 0U; index < map->queue_count; index++) {
		if (map->queue[index].token == token) {
			free(map->queue[index].message);
			continue;
		}

		/* Another operation is kept, in its order. */
		map->queue[kept] = map->queue[index];
		kept++;
	}

	/* The queue holds what was kept. */
	map->queue_count = kept;

	/* Its pages: freed, unless their operation runs (it ends quietly). */
	for (index = 0U; index < BTD_MAP_PAGES_MAX; index++) {
		if (!map->pages[index].used || map->pages[index].token != token)
			continue;

		/* The running operation's page is freed when it ends. */
		if (map->running && map->current.page == index) {
			map->pages[index].cancelled = 1;
			continue;
		}

		/* Freed now. */
		map_page_free(map, index);
	}

	/* The running operation of the token answers nobody. */
	if (map->running && map->current.token == token)
		map->current.token = 0U;

	/* Succeeded: what follows is settled. */
	map_settle(map);
}

/*
 * Looks again whether messages are wanted (the daemon calls it after
 * PHONE LINK): not wanted, MAP stops; wanted, it starts, or a failed one
 * is tried again at once.
 */
void
btd_map_check(
	struct btd_map *map)
{
	int wanted;

	/* No link: nothing to do now. */
	if (!map->link_ready)
		return;

	/* Not wanted: off. */
	wanted = map->hooks.wanted(map->hooks.context);
	if (!wanted) {
		if (map->state != BTD_MAP_OFF)
			map_shutdown(map);
		return;
	}

	/* Wanted: started, or tried again at once. */
	if (map->state == BTD_MAP_OFF) {
		map->retry_step = 0U;
		map_start(map);
	} else if (map->state == BTD_MAP_FAILED) {
		map->retry_step = 0U;
		map->retry_at = map_now(map);
	}

	/* Succeeded: what follows is settled. */
	map_settle(map);
}

/*
 * Goes on with the pages that wait for their clients to read (the daemon
 * calls it after writing to its clients).
 */
void
btd_map_pump(
	struct btd_map *map)
{
	unsigned index;

	/* Each waiting page, looked at again. */
	for (index = 0U; index < BTD_MAP_PAGES_MAX; index++) {
		if (map->pages[index].used && map->pages[index].waiting)
			map_page_continue(map, index);
	}

	/* Succeeded: what follows is settled. */
	map_settle(map);
}

/*
 * Runs MAP's timers: OBEX's answer, a busy SDP query, the attempt after a
 * failure, the MNS awaited, pages whose clients do not read, announced
 * messages searched again, events held past their time.
 */
void
btd_map_tick(
	struct btd_map *map,
	uint64_t now)
{
	struct btd_map_event event;
	unsigned closing[BTD_MAP_CLOSING_MAX];
	unsigned count;
	unsigned index;
	int wanted;

	/* The DLCs noted to close (out of the list first: closing one may note none, but keeps the list consistent). */
	count = map->closing_count;
	memcpy(closing, map->closing, count * sizeof(closing[0]));
	map->closing_count = 0U;
	for (index = 0U; index < count; index++)
		map->hooks.dlc_close(map->hooks.context, closing[index]);

	/* OBEX's answer awaited. */
	if (map->mas_dlci != 0U)
		btd_obex_tick(&map->mas, now);

	/* A busy SDP query asked again. */
	if (map->state == BTD_MAP_SDP &&
	    map->sdp_again_at != 0U &&
	    now >= map->sdp_again_at) {
		map->sdp_again_at = 0U;
		map_start(map);
	}

	/* A failed MAP set up again. */
	if (map->state == BTD_MAP_FAILED &&
	    map->retry_at != 0U &&
	    now >= map->retry_at) {
		map->retry_at = 0U;
		wanted = map->hooks.wanted(map->hooks.context);
		if (map->link_ready && wanted)
			map_start(map);
	}

	/* The MNS did not come back: no notifications. */
	if (map->mns_wait_until != 0U && now >= map->mns_wait_until) {
		map->mns_wait_until = 0U;
		map->notify = 0;
		map_log(map, "map: the MNS did not come back");
		map_changed(map);
	}

	/* Each page whose client does not read: given up after its time, else looked at again. */
	for (index = 0U; index < BTD_MAP_PAGES_MAX; index++) {
		if (!map->pages[index].used || !map->pages[index].waiting)
			continue;

		/* Its client too slow. */
		if (now - map->pages[index].waiting_since >= BTD_MAP_SLOW_MS) {
			map_page_end(map, index, "slow");
			continue;
		}

		/* Looked at again. */
		map_page_continue(map, index);
	}

	/* Each announced message whose search waits. */
	for (index = 0U; index < BTD_MAP_LIVE_MAX; index++) {
		if (map->lives[index].used &&
		    map->lives[index].search_at != 0U &&
		    now >= map->lives[index].search_at) {
			map->lives[index].search_at = 0U;
			map_live_search(map, index);
		}
	}

	/* Each held event past its time: a shift is a message sent from the phone, anything else is dropped. */
	for (index = 0U; index < BTD_MAP_HELD_MAX; index++) {
		if (!map->helds[index].used || now < map->helds[index].until)
			continue;

		/* The event out of the hold. */
		event = map->helds[index].event;
		map->helds[index].used = 0;
		if (event.type == BTD_MAP_EVENT_MESSAGE_SHIFT)
			map_live_start(map, event.handle, BTD_MAP_FOLDER_SENT);
	}

	/* Succeeded: what follows is settled. */
	map_settle(map);
}

/* Tells when MAP's next timer is due (0: none), for the daemon's poll. */
uint64_t
btd_map_deadline(
	const struct btd_map *map)
{
	uint64_t earliest;
	uint64_t deadline;
	unsigned index;

	/* DLCs to close: at once. */
	if (map->closing_count != 0U)
		return map_now(map);

	/* OBEX's answer. */
	earliest = 0U;
	if (map->mas_dlci != 0U)
		earliest = btd_obex_deadline(&map->mas);

	/* A busy SDP query, an attempt after a failure, the MNS awaited. */
	if (map->state == BTD_MAP_SDP)
		earliest = map_earlier(earliest, map->sdp_again_at);
	if (map->state == BTD_MAP_FAILED)
		earliest = map_earlier(earliest, map->retry_at);
	earliest = map_earlier(earliest, map->mns_wait_until);

	/* The pages waiting for their clients. */
	for (index = 0U; index < BTD_MAP_PAGES_MAX; index++) {
		if (!map->pages[index].used || !map->pages[index].waiting)
			continue;
		deadline = map->pages[index].waiting_since + BTD_MAP_SLOW_MS;
		earliest = map_earlier(earliest, deadline);
	}

	/* The announced messages searched again. */
	for (index = 0U; index < BTD_MAP_LIVE_MAX; index++) {
		if (map->lives[index].used)
			earliest = map_earlier(earliest, map->lives[index].search_at);
	}

	/* The held events. */
	for (index = 0U; index < BTD_MAP_HELD_MAX; index++) {
		if (map->helds[index].used)
			earliest = map_earlier(earliest, map->helds[index].until);
	}

	/* The earliest, or none. */
	return earliest;
}

/*
 * Writes MAP's part of PHONE SHOW and PHONE STATE (section 9.2):
 * messages=off|connecting|ready|failed, send=0|1, notify=0|1, and why it
 * failed.  Returns 0, or ENOSPC.
 */
int
btd_map_state_text(
	const struct btd_map *map,
	char *text,
	size_t size)
{
	const char *name;
	int written;

	/* The state's name. */
	if (map->state == BTD_MAP_OFF) {
		name = "off";
	} else if (map->state == BTD_MAP_READY) {
		name = "ready";
	} else if (map->state == BTD_MAP_FAILED) {
		name = "failed";
	} else {
		name = "connecting";
	}

	/* The fields, why when it failed. */
	if (map->state == BTD_MAP_FAILED && map->why != NULL) {
		written = snprintf(text, size, "messages=%s send=%d notify=%d why=%s", name, map->send, map->notify, map->why);
	} else {
		written = snprintf(text, size, "messages=%s send=%d notify=%d", name, map->send, map->notify);
	}

	/* The text within its room. */
	if (written < 0 || (size_t)written >= size)
		return ENOSPC;

	/* Succeeded: the text. */
	return 0;
}

/* Gives the daemon's monotonic milliseconds. */
static uint64_t
map_now(
	const struct btd_map *map)
{
	uint64_t now;

	/* The daemon's clock. */
	now = map->hooks.clock(map->hooks.context);

	/* The milliseconds. */
	return now;
}

/* Writes a line of the daemon's log (never a message's text, number or name). */
static void
map_log(
	struct btd_map *map,
	const char *format,
	...)
{
	char line[160];
	va_list arguments;

	/* No log. */
	if (map->hooks.log == NULL)
		return;

	/* The line. */
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);

	/* Succeeded: logged. */
	map->hooks.log(map->hooks.context, line);
}

/* Tells the daemon that what PHONE SHOW tells of MAP changed. */
static void
map_changed(
	struct btd_map *map)
{
	/* Succeeded: told. */
	if (map->hooks.changed != NULL)
		map->hooks.changed(map->hooks.context);
}

/* Starts setting MAP up: the SDP query for the MAS (asked again shortly when another query runs). */
static void
map_start(
	struct btd_map *map)
{
	int error;

	/* A new attempt, nothing found yet. */
	map->state = BTD_MAP_SDP;
	map->why = NULL;
	map->sdp_again_at = 0U;
	map->mas_channel = 0U;

	/* The query. */
	error = map->hooks.sdp_query(map->hooks.context, BTD_SDP_UUID_MAS);
	if (error == EBUSY) {
		map->sdp_again_at = map_now(map) + BTD_MAP_BUSY_MS;
		return;
	}

	/* A link that cannot query. */
	if (error != 0) {
		map_fail(map, "sdp", MAP_RETRY_STEP);
		return;
	}

	/* Succeeded: the answer comes to btd_map_sdp_done. */
	map_changed(map);
}

/*
 * Fails MAP: its DLCs close, every request is answered as lost, and it is
 * set up again after a wait that grows (MAP_RETRY_STEP), after the long
 * wait (MAP_RETRY_REFUSED: the phone's user refused, section 8.1.1), or
 * not on this link (MAP_RETRY_NEVER).
 */
static void
map_fail(
	struct btd_map *map,
	const char *why,
	unsigned retry)
{
	uint64_t wait;
	unsigned step;

	/* Logged. */
	map_log(map, "map: failed (%s)", why);

	/* The DLCs, the requests. */
	map_close_all(map);
	map_end_requests(map, "lost");

	/* The wait: 30 s doubling to 600 s, or 600 s after a refusal, or none. */
	map->state = BTD_MAP_FAILED;
	map->why = why;
	map->retry_at = 0U;
	if (retry == MAP_RETRY_STEP) {
		wait = BTD_MAP_RETRY_FIRST_MS;
		for (step = 0U; step < map->retry_step && wait < BTD_MAP_RETRY_LAST_MS; step++)
			wait *= 2U;
		if (wait > BTD_MAP_RETRY_LAST_MS)
			wait = BTD_MAP_RETRY_LAST_MS;
		map->retry_step++;
		map->retry_at = map_now(map) + wait;
	} else if (retry == MAP_RETRY_REFUSED) {
		map->retry_at = map_now(map) + BTD_MAP_RETRY_LAST_MS;
	}

	/* Succeeded: failed, and told. */
	map_changed(map);
}

/* Closes MAP's DLCs and forgets what was on them. */
static void
map_close_all(
	struct btd_map *map)
{
	/* The MAS's DLC, closed at the next tick (not from inside RFCOMM's events). */
	if (map->mas_dlci != 0U) {
		map_close_later(map, map->mas_dlci);
		map->mas_dlci = 0U;
	}

	/* The MNS's DLC, the same way. */
	if (map->mns_dlci != 0U) {
		map_close_later(map, map->mns_dlci);
		map->mns_dlci = 0U;
	}

	/* No notifications, no sending, nothing told by OBEX. */
	map->mns_connected = 0;
	map->mns_wait_until = 0U;
	map->notify = 0;
	map->send = 0;
	map->finished = 0;
	map->failing = NULL;
	map->event_count = 0U;
}

/*
 * Notes a DLC to close at the next tick: closing it from inside RFCOMM's
 * events (a DLC's closing, the session's loss) could pull the session
 * from under RFCOMM.
 */
static void
map_close_later(
	struct btd_map *map,
	unsigned dlci)
{
	/* No room: closed by the session's end instead. */
	if (map->closing_count >= BTD_MAP_CLOSING_MAX) {
		map_log(map, "map: a DLC left open");
		return;
	}

	/* Succeeded: noted. */
	map->closing[map->closing_count] = dlci;
	map->closing_count++;
}

/*
 * Answers every request as ERROR word and forgets the queue, the
 * operation running, the pages, the announced messages, the sent ones and
 * the held events.
 */
static void
map_end_requests(
	struct btd_map *map,
	const char *word)
{
	char line[48];
	unsigned index;
	uint64_t token;

	/* The answer. */
	(void)snprintf(line, sizeof(line), "ERROR %s", word);

	/* The operation running: answered when it is a request's own (a page's is answered with its page). */
	if (map->running) {
		token = map->current.token;
		if (token != 0U && map->current.page == BTD_MAP_NONE) {
			map->hooks.answer(map->hooks.context, token, line, NULL, 0U);
			map->hooks.answer(map->hooks.context, token, "DONE", NULL, 0U);
		}

		/* Its bMessage freed, nothing running. */
		free(map->current.message);
		map->current.message = NULL;
		map->running = 0;
	}

	/* Each queued operation: answered the same way. */
	for (index = 0U; index < map->queue_count; index++) {
		token = map->queue[index].token;
		if (token != 0U && map->queue[index].page == BTD_MAP_NONE) {
			map->hooks.answer(map->hooks.context, token, line, NULL, 0U);
			map->hooks.answer(map->hooks.context, token, "DONE", NULL, 0U);
		}

		/* Its bMessage freed. */
		free(map->queue[index].message);
	}

	/* The queue empty. */
	map->queue_count = 0U;

	/* Each page. */
	for (index = 0U; index < BTD_MAP_PAGES_MAX; index++) {
		if (!map->pages[index].used)
			continue;

		/* Its client told, unless it went. */
		if (!map->pages[index].cancelled) {
			map->hooks.answer(map->hooks.context, map->pages[index].token, line, NULL, 0U);
			map->hooks.answer(map->hooks.context, map->pages[index].token, "DONE", NULL, 0U);
		}

		/* The page freed. */
		map_page_free(map, index);
	}

	/* Releases the operation input when all outstanding requests have ended. */
	free(map->body);
	map->body = NULL;
	map->body_capacity = 0U;
	map->body_length = 0U;

	/* Succeeded: the announced messages, the sent ones and the held events forgotten. */
	memset(map->lives, 0, sizeof(map->lives));
	memset(map->sents, 0, sizeof(map->sents));
	memset(map->helds, 0, sizeof(map->helds));
}

/* Turns MAP off: its DLCs close and every request is answered as lost. */
static void
map_shutdown(
	struct btd_map *map)
{
	int was_off;

	/* What was. */
	was_off = 0;
	if (map->state == BTD_MAP_OFF)
		was_off = 1;

	/* The DLCs, the requests. */
	map_close_all(map);
	map_end_requests(map, "lost");

	/* Off, the waits forgotten. */
	map->state = BTD_MAP_OFF;
	map->why = NULL;
	map->retry_at = 0U;
	map->retry_step = 0U;
	map->sdp_again_at = 0U;

	/* Succeeded: told when it changed. */
	if (!was_off)
		map_changed(map);
}

/*
 * Does what OBEX's events noted, outside OBEX's input: fails MAP, ends the
 * setting up or the operation that finished, handles the MNS's events and
 * its connection, then starts the next operation.
 */
static void
map_settle(
	struct btd_map *map)
{
	const char *why;
	int round;

	/* Until nothing more is noted (each round ends at least one operation, and the queue is bounded). */
	for (round = 0; round < 64; round++) {
		/* A failure noted inside OBEX. */
		if (map->failing != NULL) {
			why = map->failing;
			map->failing = NULL;
			map->finished = 0;
			map_fail(map, why, map->failing_kind);
			continue;
		}

		/* A MAS connection broken (an answer too late, a malformed packet). */
		if (map->mas_dlci != 0U && map->mas.state == BTD_OBEX_BROKEN) {
			why = "obex";
			if (map->finished && map->finished_error == ETIMEDOUT)
				why = "timeout";
			map->finished = 0;
			map_fail(map, why, MAP_RETRY_STEP);
			continue;
		}

		/* A setting up or an operation that finished. */
		if (map->finished) {
			map->finished = 0;
			if (map->state == BTD_MAP_READY && map->running) {
				map_op_finished(map);
			} else {
				map_setup_finished(map);
			}

			/* Looked at again. */
			continue;
		}

		/* The MNS connected: notifications on. */
		if (map->mns_connected &&
		    !map->notify &&
		    map->state == BTD_MAP_READY) {
			map->notify = 1;
			map->mns_wait_until = 0U;
			map_changed(map);
		}

		/* The MNS back after it went. */
		if (map->mns_connected && map->mns_wait_until != 0U)
			map->mns_wait_until = 0U;

		/* The MNS's events. */
		if (map->event_count != 0U) {
			map_events(map);
			continue;
		}

		/* The next operation. */
		map_next(map);
		if (!map->finished && map->failing == NULL)
			break;
	}
}

/*
 * Ends a step of setting MAP up whose answer came: Connect, then SetPath
 * to telecom and msg, then the notifications' registration, then ready.
 */
static void
map_setup_finished(
	struct btd_map *map)
{
	int error;
	uint8_t code;

	/* The answer. */
	error = map->finished_error;
	code = map->finished_code;

	/* Connect's answer. */
	if (map->state == BTD_MAP_CONNECTING) {
		/* Refused by the phone's user. */
		if (code == BTD_OBEX_UNAUTHORIZED || code == BTD_OBEX_FORBIDDEN) {
			map_fail(map, "permission", MAP_RETRY_REFUSED);
			return;
		}

		/* No answer in time. */
		if (error == ETIMEDOUT) {
			map_fail(map, "timeout", MAP_RETRY_STEP);
			return;
		}

		/* Refused otherwise. */
		if (error != 0) {
			map_fail(map, "refused", MAP_RETRY_STEP);
			return;
		}

		/* Connected: a new session, the usual wait for each answer, the folder next. */
		btd_obex_set_timeout(&map->mas, BTD_OBEX_TIMEOUT_MS);
		map->session++;
		map->mse_known = 0;
		memset(map->sizes_known, 0, sizeof(map->sizes_known));
		map->state = BTD_MAP_SETPATH;
		map->setpath_step = 0U;
		map_setpath(map);
		return;
	}

	/* SetPath's answer: the next step, or the registration. */
	if (map->state == BTD_MAP_SETPATH) {
		if (error != 0) {
			map_fail(map, "refused", MAP_RETRY_STEP);
			return;
		}

		/* telecom, then msg. */
		map->setpath_step++;
		if (map->setpath_step < 2U) {
			map_setpath(map);
			return;
		}

		/* The folder set. */
		map_register_or_ready(map);
		return;
	}

	/* The registration's answer: ready either way (without notifications when refused). */
	if (map->state == BTD_MAP_REGISTERING) {
		if (error != 0)
			map_log(map, "map: notifications refused (code 0x%02x)", code);
		map_become_ready(map);
	}
}

/* Sends SetPath to the folder of the step: telecom, then msg, never creating it. */
static void
map_setpath(
	struct btd_map *map)
{
	struct btd_obex_writer writer;
	uint8_t headers[MAP_HEADERS_MAX];
	const char *name;
	int error;

	/* The folder's Name. */
	name = "telecom";
	if (map->setpath_step == 1U)
		name = "msg";
	btd_obex_writer_init(&writer, headers, sizeof(headers));
	btd_obex_put_text(&writer, BTD_OBEX_NAME, name);

	/* Sent. */
	error = btd_obex_setpath(&map->mas, BTD_OBEX_SETPATH_NO_CREATE, headers, writer.used, map_now(map));
	if (error != 0)
		map_fail(map, "obex", MAP_RETRY_STEP);
}

/* Registers for notifications when the MAS offers them, else MAP is ready without them. */
static void
map_register_or_ready(
	struct btd_map *map)
{
	unsigned wanted;
	int error;

	/* Without notifications offered: ready. */
	wanted = MAP_FEATURE_REGISTRATION | MAP_FEATURE_NOTIFICATION;
	if ((map->features & wanted) != wanted) {
		map_log(map, "map: the phone offers no notifications");
		map_become_ready(map);
		return;
	}

	/* The registration. */
	map->state = BTD_MAP_REGISTERING;
	error = map_run_register(map);
	if (error != 0)
		map_fail(map, "obex", MAP_RETRY_STEP);
}

/* Makes MAP ready: sending when the MAS takes uploads, the waits from the first again, told. */
static void
map_become_ready(
	struct btd_map *map)
{
	/* Ready. */
	map->state = BTD_MAP_READY;
	map->why = NULL;
	map->retry_step = 0U;
	map->send = 0;
	if ((map->features & MAP_FEATURE_UPLOADING) != 0U)
		map->send = 1;

	/* An MNS already connected gives notifications. */
	if (map->mns_connected)
		map->notify = 1;
	map_log(map, "map: ready (session %08lx, features 0x%08lx)", (unsigned long)map->session, (unsigned long)map->features);

	/* The phone link's waits from the first. */
	if (map->hooks.up != NULL)
		map->hooks.up(map->hooks.context);

	/* Succeeded: told. */
	map_changed(map);
}

/*
 * Adds an operation to the queue: an announced message's after the
 * others of its kind and before the rest, any other at the end.  Returns
 * 0, or ENOSPC when the queue is full.
 */
static int
map_enqueue(
	struct btd_map *map,
	const struct btd_map_op *op)
{
	unsigned at;

	/* Room. */
	if (map->queue_count >= BTD_MAP_QUEUE_MAX)
		return ENOSPC;

	/* Its place: the end, or after the last urgent one. */
	at = map->queue_count;
	if (op->urgent) {
		at = 0U;
		while (at < map->queue_count && map->queue[at].urgent)
			at++;
	}

	/* Succeeded: the others after it move down. */
	memmove(&map->queue[at + 1U], &map->queue[at], (map->queue_count - at) * sizeof(map->queue[0]));
	map->queue[at] = *op;
	map->queue_count++;
	return 0;
}

/* Prepares an operation of a kind: no client, no page, no announced message. */
static void
map_op_init(
	struct btd_map_op *op,
	unsigned kind)
{
	/* Succeeded: nothing else. */
	memset(op, 0, sizeof(*op));
	op->kind = kind;
	op->page = BTD_MAP_NONE;
	op->live = BTD_MAP_NONE;
}

/* Runs the first operation of the queue when MAP is ready and none runs. */
static void
map_next(
	struct btd_map *map)
{
	int error;

	/* Ready, idle, something queued. */
	if (map->state != BTD_MAP_READY ||
	    map->running ||
	    map->queue_count == 0U)
		return;

	/* The first operation out of the queue. */
	map->current = map->queue[0];
	map->queue_count--;
	memmove(&map->queue[0], &map->queue[1], map->queue_count * sizeof(map->queue[0]));
	map->running = 1;

	/* Nothing heard of it yet. */
	map->have_size = 0;
	map->have_mse = 0;
	map->have_name = 0;
	map->body_length = 0U;

	/* Sent; a request that cannot go ends at once. */
	error = map_run(map, &map->current);
	if (error != 0) {
		map->finished = 1;
		map->finished_error = error;
		map->finished_code = 0U;
	}
}

/* Sends an operation's request on the MAS.  Returns 0, or OBEX's error. */
static int
map_run(
	struct btd_map *map,
	struct btd_map_op *op)
{
	int error;

	/* Each kind. */
	switch (op->kind) {
	case BTD_MAP_OP_COUNT:
	case BTD_MAP_OP_LIST:
	case BTD_MAP_OP_LOCATE:
		error = map_run_listing(map, op);
		break;
	case BTD_MAP_OP_GET:
		error = map_run_get(map, op);
		break;
	case BTD_MAP_OP_UNREAD:
		error = map_run_status(map, op, MAP_STATUS_UNREAD);
		break;
	case BTD_MAP_OP_READ:
		error = map_run_status(map, op, MAP_STATUS_READ);
		break;
	case BTD_MAP_OP_PUSH:
		error = map_run_push(map, op);
		break;
	case BTD_MAP_OP_REGISTER:
		error = map_run_register(map);
		break;
	default:
		error = EINVAL;
		break;
	}

	/* Reports whether the request went. */
	if (error != 0)
		return error;

	/* Succeeded: the answer comes to map_mas_done. */
	return 0;
}

/*
 * Sends a listing's Get (section 8.4): of the folder, its messages within
 * the filter (SMS since the page's time, or since an hour ago for a
 * search), the count asked (0 for COUNT, which gives only the size), from
 * the offset, with the fields wanted.
 */
static int
map_run_listing(
	struct btd_map *map,
	struct btd_map_op *op)
{
	static const char type[] = "x-bt/MAP-msg-listing";
	struct btd_obex_writer writer;
	uint8_t headers[MAP_HEADERS_MAX];
	uint8_t parameters[MAP_PARAMETERS_MAX];
	uint8_t mask[4];
	char begin[24];
	size_t used;
	int64_t since;
	int error;

	/* The time the listing starts from: the page's, or an hour ago for a search. */
	if (op->kind == BTD_MAP_OP_LOCATE) {
		since = map->hooks.wall(map->hooks.context) - BTD_MAP_LOCATE_SECONDS;
	} else {
		since = map->pages[op->page].since;
	}

	/* That time in the phone's zone. */
	map_period(map, since, begin, sizeof(begin), &op->guessed, &op->guessed_offset);

	/* The count and the offset: none for COUNT, the page's, or a search's. */
	used = 0U;
	if (op->kind == BTD_MAP_OP_COUNT) {
		map_parameter16(parameters, &used, MAP_TAG_MAX_LIST_COUNT, 0U);
	} else if (op->kind == BTD_MAP_OP_LIST) {
		map_parameter16(parameters, &used, MAP_TAG_MAX_LIST_COUNT, op->count);
		map_parameter16(parameters, &used, MAP_TAG_START_OFFSET, op->offset);
	} else {
		map_parameter16(parameters, &used, MAP_TAG_MAX_LIST_COUNT, BTD_MAP_LOCATE_COUNT);
	}

	/* The filter of types and of time. */
	map_parameter8(parameters, &used, MAP_TAG_FILTER_TYPE, MAP_FILTER_NOT_TEXT);
	map_parameter(parameters, &used, MAP_TAG_FILTER_BEGIN, (const uint8_t *)begin, strlen(begin));

	/* The fields wanted, for a listing that lists. */
	if (op->kind != BTD_MAP_OP_COUNT) {
		mask[0] = (uint8_t)((MAP_PARAMETER_MASK >> 24) & 0xffUL);
		mask[1] = (uint8_t)((MAP_PARAMETER_MASK >> 16) & 0xffUL);
		mask[2] = (uint8_t)((MAP_PARAMETER_MASK >> 8) & 0xffUL);
		mask[3] = (uint8_t)(MAP_PARAMETER_MASK & 0xffUL);
		map_parameter(parameters, &used, MAP_TAG_PARAMETER_MASK, mask, sizeof(mask));
	}

	/* The headers: Type (its NUL included), Name of the folder, the parameters. */
	btd_obex_writer_init(&writer, headers, sizeof(headers));
	btd_obex_put_bytes(&writer, BTD_OBEX_TYPE, (const uint8_t *)type, sizeof(type));
	btd_obex_put_text(&writer, BTD_OBEX_NAME, map_folder_names[op->folder]);
	btd_obex_put_bytes(&writer, BTD_OBEX_APPLICATION, parameters, used);
	if (writer.overflow)
		return EMSGSIZE;

	/* Sent. */
	error = btd_obex_get(&map->mas, headers, writer.used, 65536U, map_now(map));
	if (error != 0)
		return error;

	/* Succeeded: under way. */
	return 0;
}

/* Sends a message's Get (section 8.4): by its handle, without attachments, its text in UTF-8. */
static int
map_run_get(
	struct btd_map *map,
	const struct btd_map_op *op)
{
	static const char type[] = "x-bt/message";
	struct btd_obex_writer writer;
	uint8_t headers[MAP_HEADERS_MAX];
	uint8_t parameters[MAP_PARAMETERS_MAX];
	char name[24];
	size_t used;
	int error;

	/* The parameters. */
	used = 0U;
	map_parameter8(parameters, &used, MAP_TAG_ATTACHMENT, 1U);
	map_parameter8(parameters, &used, MAP_TAG_CHARSET, MAP_CHARSET_UTF8);

	/* The headers: Type, Name of the handle in 16 hex digits, the parameters. */
	(void)snprintf(name, sizeof(name), "%016llX", (unsigned long long)op->handle);
	btd_obex_writer_init(&writer, headers, sizeof(headers));
	btd_obex_put_bytes(&writer, BTD_OBEX_TYPE, (const uint8_t *)type, sizeof(type));
	btd_obex_put_text(&writer, BTD_OBEX_NAME, name);
	btd_obex_put_bytes(&writer, BTD_OBEX_APPLICATION, parameters, used);
	if (writer.overflow)
		return EMSGSIZE;

	/* Sent. */
	error = btd_obex_get(&map->mas, headers, writer.used, BTD_MAP_BODY_MAX, map_now(map));
	if (error != 0)
		return error;

	/* Succeeded: under way. */
	return 0;
}

/* Sends a message's status Put (section 8.4): its read status set to value, the body the filler byte. */
static int
map_run_status(
	struct btd_map *map,
	const struct btd_map_op *op,
	uint8_t value)
{
	static const char type[] = "x-bt/messageStatus";
	static const uint8_t filler[1] = { MAP_FILLER };
	struct btd_obex_writer writer;
	uint8_t headers[MAP_HEADERS_MAX];
	uint8_t parameters[MAP_PARAMETERS_MAX];
	char name[24];
	size_t used;
	int error;

	/* The parameters: the read status, and its value. */
	used = 0U;
	map_parameter8(parameters, &used, MAP_TAG_STATUS_INDICATOR, MAP_STATUS_READ_INDICATOR);
	map_parameter8(parameters, &used, MAP_TAG_STATUS_VALUE, value);

	/* The headers: Type, Name of the handle, the parameters. */
	(void)snprintf(name, sizeof(name), "%016llX", (unsigned long long)op->handle);
	btd_obex_writer_init(&writer, headers, sizeof(headers));
	btd_obex_put_bytes(&writer, BTD_OBEX_TYPE, (const uint8_t *)type, sizeof(type));
	btd_obex_put_text(&writer, BTD_OBEX_NAME, name);
	btd_obex_put_bytes(&writer, BTD_OBEX_APPLICATION, parameters, used);
	if (writer.overflow)
		return EMSGSIZE;

	/* Sent. */
	error = btd_obex_put(&map->mas, headers, writer.used, filler, sizeof(filler), map_now(map));
	if (error != 0)
		return error;

	/* Succeeded: under way. */
	return 0;
}

/* Sends PushMessage (section 8.4): the bMessage into the outbox, its text in UTF-8. */
static int
map_run_push(
	struct btd_map *map,
	const struct btd_map_op *op)
{
	static const char type[] = "x-bt/message";
	struct btd_obex_writer writer;
	uint8_t headers[MAP_HEADERS_MAX];
	uint8_t parameters[MAP_PARAMETERS_MAX];
	size_t used;
	int error;

	/* The parameters: the character set. */
	used = 0U;
	map_parameter8(parameters, &used, MAP_TAG_CHARSET, MAP_CHARSET_UTF8);

	/* The headers: Type, Name of the outbox, the parameters. */
	btd_obex_writer_init(&writer, headers, sizeof(headers));
	btd_obex_put_bytes(&writer, BTD_OBEX_TYPE, (const uint8_t *)type, sizeof(type));
	btd_obex_put_text(&writer, BTD_OBEX_NAME, "outbox");
	btd_obex_put_bytes(&writer, BTD_OBEX_APPLICATION, parameters, used);
	if (writer.overflow)
		return EMSGSIZE;

	/* Sent, the bMessage kept by the operation until it ends. */
	error = btd_obex_put(&map->mas, headers, writer.used, op->message, op->message_length, map_now(map));
	if (error != 0)
		return error;

	/* Succeeded: under way. */
	return 0;
}

/* Sends the notifications' registration (MAP section 5.2): on, the body the filler byte. */
static int
map_run_register(
	struct btd_map *map)
{
	static const char type[] = "x-bt/MAP-NotificationRegistration";
	static const uint8_t filler[1] = { MAP_FILLER };
	struct btd_obex_writer writer;
	uint8_t headers[MAP_HEADERS_MAX];
	uint8_t parameters[MAP_PARAMETERS_MAX];
	size_t used;
	int error;

	/* The parameters: notifications on. */
	used = 0U;
	map_parameter8(parameters, &used, MAP_TAG_NOTIFICATION, 1U);

	/* The headers: Type, the parameters. */
	btd_obex_writer_init(&writer, headers, sizeof(headers));
	btd_obex_put_bytes(&writer, BTD_OBEX_TYPE, (const uint8_t *)type, sizeof(type));
	btd_obex_put_bytes(&writer, BTD_OBEX_APPLICATION, parameters, used);
	if (writer.overflow)
		return EMSGSIZE;

	/* Sent. */
	error = btd_obex_put(&map->mas, headers, writer.used, filler, sizeof(filler), map_now(map));
	if (error != 0)
		return error;

	/* Succeeded: under way. */
	return 0;
}

/*
 * Ends the operation that ran: what its answer gives goes to its page,
 * announced message or client, and the operations that follow from it
 * are queued.
 */
static void
map_op_finished(
	struct btd_map *map)
{
	struct btd_map_op *op;

	/* The operation and its answer. */
	op = &map->current;

	/* Each kind. */
	switch (op->kind) {
	case BTD_MAP_OP_COUNT:
		map_count_done(map, op);
		break;
	case BTD_MAP_OP_LIST:
		map_list_done(map, op);
		break;
	case BTD_MAP_OP_LOCATE:
		map_locate_done(map, op);
		break;
	case BTD_MAP_OP_GET:
		map_get_done(map, op);
		break;
	case BTD_MAP_OP_UNREAD:
		map_unread_done(map, op);
		break;
	case BTD_MAP_OP_READ:
		map_read_done(map, op);
		break;
	case BTD_MAP_OP_PUSH:
		map_push_done(map, op);
		break;
	default:
		/* The registration asked again: a refusal is only logged (the MNS's wait tells the rest). */
		if (map->finished_error != 0)
			map_log(map, "map: notifications refused again (code 0x%02x)", map->finished_code);
		break;
	}

	/* Succeeded: nothing runs, the bMessage freed. */
	free(op->message);
	op->message = NULL;
	map->running = 0;
}

/*
 * Takes a folder's count (its listing's size and the phone's time): with
 * the phone's zone learned, a count whose filter was written in zedBSD's
 * zone is asked again once; then the page's listing.
 */
static void
map_count_done(
	struct btd_map *map,
	struct btd_map_op *op)
{
	struct btd_map_page *page;
	struct btd_map_op next;
	const char *word;
	int error;

	/* The page, unless its client went. */
	page = &map->pages[op->page];
	if (page->cancelled) {
		map_page_free(map, op->page);
		return;
	}

	/* A count refused ends the page. */
	if (map->finished_error != 0) {
		word = map_error_word(map->finished_error, map->finished_code);
		map_page_end(map, op->page, word);
		return;
	}

	/* The phone's zone, learned once a session. */
	if (map->have_mse && !map->mse_known) {
		map->mse_known = 1;
		map->mse_offset = map->mse_answer_offset;
	}

	/* A filter written in another zone than the phone's: the count asked again, once. */
	if (op->guessed &&
	    map->mse_known &&
	    map->mse_offset != op->guessed_offset &&
	    !page->recounted) {
		page->recounted = 1;
		next = *op;
		next.guessed = 0;
		error = map_enqueue(map, &next);
		if (error != 0)
			map_page_end(map, op->page, "busy");
		return;
	}

	/* The folder's size for this time, when the phone gave it. */
	map->sizes_known[op->folder] = 0;
	if (map->have_size) {
		map->sizes_known[op->folder] = 1;
		map->sizes_since[op->folder] = page->since;
		map->sizes[op->folder] = map->listing_size;
	}

	/* The listing next. */
	next = *op;
	next.kind = BTD_MAP_OP_LIST;
	next.guessed = 0;
	error = map_enqueue(map, &next);
	if (error != 0)
		map_page_end(map, op->page, "busy");
}

/*
 * Takes a page's listing: its entries are fetched in turn.  A listing too
 * large is asked again for half as many messages.
 */
static void
map_list_done(
	struct btd_map *map,
	struct btd_map_op *op)
{
	struct btd_mapxml_counts counts;
	struct btd_map_page *page;
	struct btd_map_op next;
	const char *word;
	int error;

	/* The page, unless its client went. */
	page = &map->pages[op->page];
	if (page->cancelled) {
		map_page_free(map, op->page);
		return;
	}

	/* Too large: half as many, down to one. */
	if (map->finished_error == EMSGSIZE) {
		if (op->count <= 1U) {
			map_page_end(map, op->page, "too-large");
			return;
		}

		/* Asked again. */
		next = *op;
		next.count = op->count / 2U;
		page->count = next.count;
		error = map_enqueue(map, &next);
		if (error != 0)
			map_page_end(map, op->page, "busy");
		return;
	}

	/* A listing refused ends the page. */
	if (map->finished_error != 0) {
		word = map_error_word(map->finished_error, map->finished_code);
		map_page_end(map, op->page, word);
		return;
	}

	/* The phone's zone, when the count did not give it. */
	if (map->have_mse && !map->mse_known) {
		map->mse_known = 1;
		map->mse_offset = map->mse_answer_offset;
	}

	/* The entries. */
	error = btd_mapxml_listing(map->body, map->body_length, page->entries, page->count, &counts);
	if (error != 0) {
		map_log(map, "map: a listing could not be read (%d)", error);
		map_page_end(map, op->page, "refused");
		return;
	}

	/* Succeeded: each entry in turn. */
	page->listed = counts.entries + counts.skipped + counts.dropped;
	page->entry_count = counts.entries;
	page->next_entry = 0U;
	page->skipped += (unsigned)counts.skipped;
	map_page_continue(map, op->page);
}

/* Takes a message's Get: a page's, or an announced one's. */
static void
map_get_done(
	struct btd_map *map,
	struct btd_map_op *op)
{
	/* A page's message. */
	if (op->page != BTD_MAP_NONE) {
		map_page_get_done(map, op);
		return;
	}

	/* Succeeded: an announced message, unless MAP forgot it. */
	if (op->live != BTD_MAP_NONE)
		map_live_get_done(map, op);
}

/*
 * Takes a message marked unread again (a failure is only logged): its
 * page goes on, or the announced message is done.
 */
static void
map_unread_done(
	struct btd_map *map,
	struct btd_map_op *op)
{
	/* A refusal logged. */
	if (map->finished_error != 0)
		map_log(map, "map: unread refused (code 0x%02x)", map->finished_code);

	/* A page's message: the page goes on. */
	if (op->page != BTD_MAP_NONE) {
		map_page_continue(map, op->page);
		return;
	}

	/* Succeeded: an announced message is done. */
	if (op->live != BTD_MAP_NONE)
		map_live_free(map, op->live);
}

/* Takes a message marked read (PHONE READ): its client hears DONE, after ERROR when it was refused. */
static void
map_read_done(
	struct btd_map *map,
	struct btd_map_op *op)
{
	char line[48];
	const char *word;

	/* A client that went. */
	if (op->token == 0U)
		return;

	/* A refusal. */
	if (map->finished_error != 0) {
		word = map_error_word(map->finished_error, map->finished_code);
		(void)snprintf(line, sizeof(line), "ERROR %s", word);
		map->hooks.answer(map->hooks.context, op->token, line, NULL, 0U);
	}

	/* Succeeded: the answer's end. */
	map->hooks.answer(map->hooks.context, op->token, "DONE", NULL, 0U);
}

/*
 * Takes a page's message: it goes to the page's client, and is marked
 * unread again when its listing said it was unread.  A message gone
 * meanwhile is skipped.
 */
static void
map_page_get_done(
	struct btd_map *map,
	struct btd_map_op *op)
{
	static char line[BTD_PHONEIO_OUT_MAX + 1U];
	struct btd_map_page *page;
	struct btd_map_entry *entry;
	struct btd_map_op next;
	const char *word;
	int error;
	const uint8_t *payload;
	size_t payload_length;

	/* The page, unless its client went. */
	page = &map->pages[op->page];
	if (page->cancelled) {
		map_page_free(map, op->page);
		return;
	}

	/* A message gone since the listing: skipped. */
	entry = &page->entries[op->offset];
	if (map->finished_code == BTD_OBEX_NOT_FOUND) {
		page->skipped++;
		map_page_continue(map, op->page);
		return;
	}

	/* A message refused otherwise ends the page. */
	if (map->finished_error != 0) {
		word = map_error_word(map->finished_error, map->finished_code);
		map_page_end(map, op->page, word);
		return;
	}

	/* The bMessage; one that cannot be read is skipped. */
	error = btd_bmsg_parse(map->body, map->body_length, &map->message);
	if (error != 0) {
		map_log(map, "map: a message could not be read (%d)", error);
		page->skipped++;
		map_page_continue(map, op->page);
		return;
	}

	/* The way its text was found, counted for the log. */
	map->form_counts[map->message.form]++;

	/* Selects MIME bytes for MMS and UTF-8 for ordinary messages. */
	payload = map_message_body(&map->message, &payload_length);

	/* Its line and text to the page's client. */
	error = map_item(map, line, sizeof(line), entry->handle, page->folder, entry);
	if (error == 0) {
		map->hooks.answer(map->hooks.context, page->token, line, payload, payload_length);
		page->given++;
	}

	/* Unread on the phone before the Get: unread again. */
	if (entry->read == BTD_MAP_READ_NO) {
		map_op_init(&next, BTD_MAP_OP_UNREAD);
		next.page = op->page;
		next.token = page->token;
		next.handle = entry->handle;
		error = map_enqueue(map, &next);
		if (error == 0)
			return;
	}

	/* Succeeded: the next message. */
	map_page_continue(map, op->page);
}

/*
 * Takes the search for an announced message: found, its entry is kept;
 * not found, it is searched once more a little later; then the message is
 * fetched.
 */
static void
map_locate_done(
	struct btd_map *map,
	struct btd_map_op *op)
{
	struct btd_mapxml_counts counts;
	struct btd_map_live *live;
	struct btd_map_op next;
	size_t index;
	int error;

	/* The announced message, unless MAP forgot it. */
	live = &map->lives[op->live];
	if (!live->used)
		return;

	/* Its entry in the listing, when it is there. */
	if (map->finished_error == 0) {
		error = btd_mapxml_listing(map->body, map->body_length, map->located, BTD_MAP_LOCATE_COUNT, &counts);
		if (error != 0)
			counts.entries = 0U;
		for (index = 0U; index < counts.entries; index++) {
			if (map->located[index].handle == live->handle) {
				live->entry = map->located[index];
				live->found = 1;
				break;
			}
		}
	}

	/* Not there yet: searched once more a little later. */
	if (!live->found && live->searches < 2U) {
		live->search_at = map_now(map) + BTD_MAP_LOCATE_AGAIN_MS;
		return;
	}

	/* The message itself, before the pages. */
	map_op_init(&next, BTD_MAP_OP_GET);
	next.live = op->live;
	next.urgent = 1;
	next.handle = live->handle;
	error = map_enqueue(map, &next);
	if (error != 0) {
		map_live_free(map, op->live);
		map_dropped(map);
	}
}

/*
 * Takes an announced message: it goes to the subscribers, and a message
 * of the inbox that was unread (or not found in the listing) is marked
 * unread again.
 */
static void
map_live_get_done(
	struct btd_map *map,
	struct btd_map_op *op)
{
	static char line[BTD_PHONEIO_OUT_MAX + 1U];
	struct btd_map_live *live;
	struct btd_map_op next;
	const struct btd_map_entry *entry;
	int unread;
	int error;
	const uint8_t *payload;
	size_t payload_length;

	/* The announced message, unless MAP forgot it. */
	live = &map->lives[op->live];
	if (!live->used)
		return;

	/* A message that could not be fetched (gone, or refused) is dropped. */
	if (map->finished_error != 0) {
		map_log(map, "map: an announced message could not be fetched (code 0x%02x)", map->finished_code);
		map_live_free(map, op->live);
		return;
	}

	/* The bMessage. */
	error = btd_bmsg_parse(map->body, map->body_length, &map->message);
	if (error != 0) {
		map_log(map, "map: an announced message could not be read (%d)", error);
		map_live_free(map, op->live);
		return;
	}

	/* The way its text was found, counted for the log. */
	map->form_counts[map->message.form]++;

	/* Selects MIME bytes for MMS and UTF-8 for ordinary messages. */
	payload = map_message_body(&map->message, &payload_length);

	/* Its line and text to the subscribers. */
	entry = NULL;
	if (live->found)
		entry = &live->entry;
	error = map_item(map, line, sizeof(line), live->handle, live->folder, entry);
	if (error == 0)
		map->hooks.emit(map->hooks.context, line, payload, payload_length);

	/* Unread in the inbox (or not known): unread again. */
	unread = 0;
	if (live->folder == BTD_MAP_FOLDER_INBOX) {
		if (!live->found || live->entry.read == BTD_MAP_READ_NO)
			unread = 1;
	}

	/* Marked unread again after the message. */
	if (unread) {
		map_op_init(&next, BTD_MAP_OP_UNREAD);
		next.live = op->live;
		next.urgent = 1;
		next.handle = live->handle;
		error = map_enqueue(map, &next);
		if (error == 0)
			return;
	}

	/* Succeeded: done with it. */
	map_live_free(map, op->live);
}

/*
 * Takes PushMessage's answer: the client hears the handle the phone gave
 * the message, which is followed for its events (those held for it are
 * handled now).
 */
static void
map_push_done(
	struct btd_map *map,
	struct btd_map_op *op)
{
	struct btd_map_sent *sent;
	char line[128];
	const char *word;

	/* Refused: the client told, unless it went. */
	if (map->finished_error != 0 && op->token != 0U) {
		word = map_error_word(map->finished_error, map->finished_code);
		(void)snprintf(line, sizeof(line), "ERROR %s", word);
		map->hooks.answer(map->hooks.context, op->token, line, NULL, 0U);
		map->hooks.answer(map->hooks.context, op->token, "DONE", NULL, 0U);
	}

	/* Refused, nothing more. */
	if (map->finished_error != 0)
		return;

	/* No handle in the answer: logged. */
	if (!map->have_name)
		map_log(map, "map: a sent message got no handle");

	/* No handle in the answer: the client hears none. */
	if (!map->have_name && op->token != 0U) {
		(void)snprintf(line, sizeof(line), "PHONE SENT request=%u handle=- state=pushed", op->request);
		map->hooks.answer(map->hooks.context, op->token, line, NULL, 0U);
		map->hooks.answer(map->hooks.context, op->token, "DONE", NULL, 0U);
	}

	/* No handle, nothing to follow. */
	if (!map->have_name)
		return;

	/* The message followed, the oldest followed one forgotten when all are taken. */
	sent = &map->sents[map->sent_next];
	map->sent_next = (map->sent_next + 1U) % BTD_MAP_SENT_MAX;
	sent->used = 1;
	sent->handle = map->name_handle;
	sent->request = op->request;
	sent->told = 0U;

	/* The client told. */
	if (op->token != 0U) {
		(void)snprintf(line,
			       sizeof(line),
			       "PHONE SENT request=%u handle=%08lx.%016llx state=pushed",
			       op->request,
			       (unsigned long)map->session,
			       (unsigned long long)sent->handle);
		map->hooks.answer(map->hooks.context, op->token, line, NULL, 0U);
		map->hooks.answer(map->hooks.context, op->token, "DONE", NULL, 0U);
	}

	/* Succeeded: the events held for it. */
	map_held_answered(map, sent->handle);
}

/*
 * Goes on with a page: its next SMS entry is fetched when its client has
 * room (else the page waits), and once every entry is done the client
 * hears PHONE PAGE-END with the next cursor (section 8.5).
 */
static void
map_page_continue(
	struct btd_map *map,
	unsigned index)
{
	struct btd_map_page *page;
	struct btd_map_entry *entry;
	struct btd_map_op op;
	char line[128];
	unsigned size;
	unsigned next_folder;
	unsigned next_offset;
	long room;
	int more;
	int error;

	/* The page, unless its client went. */
	page = &map->pages[index];
	if (page->cancelled) {
		map_page_free(map, index);
		return;
	}

	/* Each entry left: an SMS or MMS is fetched, other types are skipped. */
	while (page->next_entry < page->entry_count) {
		entry = &page->entries[page->next_entry];
		if (entry->type != BTD_MAP_TYPE_SMS_GSM && entry->type != BTD_MAP_TYPE_SMS_CDMA && entry->type != BTD_MAP_TYPE_MMS) {
			page->skipped++;
			page->next_entry++;
			continue;
		}

		/* The client's room: gone, the page ends; short, the page waits. */
		room = map->hooks.room(map->hooks.context, page->token);
		if (room < 0) {
			map_page_free(map, index);
			return;
		}

		/* Short of room: the page waits, from the first time it found its client short. */
		if (room < BTD_MAP_ROOM_MIN) {
			if (!page->waiting)
				page->waiting_since = map_now(map);
			page->waiting = 1;
			return;
		}

		/* The message's Get. */
		page->waiting = 0;
		map_op_init(&op, BTD_MAP_OP_GET);
		op.token = page->token;
		op.page = index;
		op.offset = (unsigned)page->next_entry;
		op.handle = entry->handle;
		page->next_entry++;
		error = map_enqueue(map, &op);
		if (error != 0)
			map_page_end(map, index, "busy");
		return;
	}

	/* The folder's size when the phone counted it (0xffffffff: not known). */
	size = 0xffffffffU;
	if (map->sizes_known[page->folder] && map->sizes_since[page->folder] == page->since)
		size = map->sizes[page->folder];

	/* The next page: further in this folder while it gave a whole page short of its size and of the limit, else the next folder. */
	next_folder = page->folder;
	next_offset = page->offset + (unsigned)page->listed;
	if (page->listed != page->count || next_offset >= size) {
		next_folder = page->folder + 1U;
		next_offset = 0U;
	} else if (page->limit != 0U && next_offset >= page->limit) {
		/* Messages left beyond the limit: cut, and said so. */
		next_folder = page->folder + 1U;
		next_offset = 0U;
		page->capped = 1;
	}

	/* More while a folder is left. */
	more = 0;
	if (next_folder < BTD_MAP_FOLDERS)
		more = 1;

	/* The end of the page and its answer. */
	(void)snprintf(line,
		       sizeof(line),
		       "PHONE PAGE-END cursor=%08lx.%llx.%u.%u.%d more=%d count=%u skipped=%u capped=%d",
		       (unsigned long)map->session,
		       (unsigned long long)page->since,
		       next_folder,
		       next_offset,
		       page->capped,
		       more,
		       page->given,
		       page->skipped,
		       page->capped);
	map->hooks.answer(map->hooks.context, page->token, line, NULL, 0U);
	map->hooks.answer(map->hooks.context, page->token, "DONE", NULL, 0U);

	/* Succeeded: the page done. */
	map_page_free(map, index);
}

/* Ends a page with ERROR word (its client told unless it went). */
static void
map_page_end(
	struct btd_map *map,
	unsigned index,
	const char *word)
{
	char line[48];

	/* The client told. */
	if (!map->pages[index].cancelled) {
		(void)snprintf(line, sizeof(line), "ERROR %s", word);
		map->hooks.answer(map->hooks.context, map->pages[index].token, line, NULL, 0U);
		map->hooks.answer(map->hooks.context, map->pages[index].token, "DONE", NULL, 0U);
	}

	/* Succeeded: the page done. */
	map_page_free(map, index);
}

/* Frees a page and the operations queued for it. */
static void
map_page_free(
	struct btd_map *map,
	unsigned index)
{
	unsigned at;
	unsigned kept;

	/* Its queued operations go. */
	kept = 0U;
	for (at = 0U; at < map->queue_count; at++) {
		if (map->queue[at].page == index)
			continue;

		/* Another operation is kept, in its order. */
		map->queue[kept] = map->queue[at];
		kept++;
	}

	/* The queue holds what was kept. */
	map->queue_count = kept;

	/* The running operation answers nobody. */
	if (map->running && map->current.page == index)
		map->current.page = BTD_MAP_NONE;

	/* Succeeded: free. */
	memset(&map->pages[index], 0, sizeof(map->pages[index]));
}

/* Starts fetching an announced message: its search first (a message already being fetched is not fetched twice). */
static void
map_live_start(
	struct btd_map *map,
	uint64_t handle,
	unsigned folder)
{
	struct btd_map_live *live;
	unsigned index;

	/* Already being fetched. */
	for (index = 0U; index < BTD_MAP_LIVE_MAX; index++) {
		if (map->lives[index].used && map->lives[index].handle == handle)
			return;
	}

	/* A free slot, else the subscribers hear that a message was dropped. */
	live = NULL;
	for (index = 0U; index < BTD_MAP_LIVE_MAX; index++) {
		if (!map->lives[index].used) {
			live = &map->lives[index];
			break;
		}
	}

	/* None free. */
	if (live == NULL) {
		map_dropped(map);
		return;
	}

	/* The message. */
	memset(live, 0, sizeof(*live));
	live->used = 1;
	live->handle = handle;
	live->folder = folder;

	/* Succeeded: its search. */
	map_live_search(map, index);
}

/* Searches an announced message in its folder's listing of the last hour, before the pages. */
static void
map_live_search(
	struct btd_map *map,
	unsigned index)
{
	struct btd_map_op op;
	int error;

	/* One more search. */
	map->lives[index].searches++;

	/* Queued before the pages; a full queue drops it. */
	map_op_init(&op, BTD_MAP_OP_LOCATE);
	op.live = index;
	op.urgent = 1;
	op.folder = map->lives[index].folder;
	error = map_enqueue(map, &op);
	if (error != 0) {
		map_live_free(map, index);
		map_dropped(map);
	}
}

/* Frees an announced message and the operations queued for it. */
static void
map_live_free(
	struct btd_map *map,
	unsigned index)
{
	unsigned at;
	unsigned kept;

	/* Its queued operations go. */
	kept = 0U;
	for (at = 0U; at < map->queue_count; at++) {
		if (map->queue[at].live == index)
			continue;

		/* Another operation is kept, in its order. */
		map->queue[kept] = map->queue[at];
		kept++;
	}

	/* The queue holds what was kept. */
	map->queue_count = kept;

	/* The running operation is no longer its. */
	if (map->running && map->current.live == index)
		map->current.live = BTD_MAP_NONE;

	/* Succeeded: free. */
	memset(&map->lives[index], 0, sizeof(map->lives[index]));
}

/* Handles the MNS's events that came, in order; some lost to a full list are told as dropped. */
static void
map_events(
	struct btd_map *map)
{
	struct btd_map_event events[BTD_MAP_EVENTS_MAX];
	unsigned count;
	unsigned index;
	int dropped;

	/* The events, out of the list (handling one may not add another). */
	count = map->event_count;
	memcpy(events, map->events, count * sizeof(events[0]));
	map->event_count = 0U;
	dropped = map->events_dropped;
	map->events_dropped = 0;

	/* Each event. */
	for (index = 0U; index < count; index++)
		map_event(map, &events[index]);

	/* Succeeded: those lost told. */
	if (dropped)
		map_dropped(map);
}

/*
 * Handles one event of the MNS (section 8.6): a new message of the inbox
 * or the sent folder is fetched; the events of a message bluetoothd sent
 * are told as PHONE SENT (or held while a PushMessage's answer is
 * awaited); a deleted message is told as PHONE MESSAGE-GONE.
 */
static void
map_event(
	struct btd_map *map,
	const struct btd_map_event *event)
{
	char line[96];
	unsigned folder;
	unsigned old_folder;
	int own;
	int known;
	int text_message;
	int pushing;

	/* Records event classification without a handle, peer or message text. */
	map_log(map, "map: event type=%d message-type=%d handle-present=%d", event->type, event->msg_type, event->has_handle);

	/* Events about one message. */
	if (!event->has_handle) {
		map_log(map, "map: an event without a message (type %d)", event->type);
		return;
	}

	/* SMS or MMS text, and a PushMessage awaiting its answer. */
	text_message = 0;
	if (event->msg_type == BTD_MAP_TYPE_SMS_GSM || event->msg_type == BTD_MAP_TYPE_SMS_CDMA || event->msg_type == BTD_MAP_TYPE_MMS)
		text_message = 1;
	pushing = 0;
	if (map->running && map->current.kind == BTD_MAP_OP_PUSH)
		pushing = 1;

	/* Each kind of event. */
	switch (event->type) {
	case BTD_MAP_EVENT_NEW_MESSAGE:
		/* A text message of the inbox or sent folder is fetched. */
		known = map_folder_of(event->folder, &folder);
		if (!text_message ||
		    !known ||
		    folder >= BTD_MAP_FOLDERS)
			break;
		map_live_start(map, event->handle, folder);
		break;
	case BTD_MAP_EVENT_MESSAGE_SHIFT:
		/* A message from the outbox to the sent folder: bluetoothd's own is sent. */
		known = map_folder_of(event->folder, &folder);
		if (!known || folder != BTD_MAP_FOLDER_SENT)
			break;
		known = map_folder_of(event->old_folder, &old_folder);
		if (!known || old_folder != BTD_MAP_FOLDERS)
			break;
		own = map_event_own(map, event);
		if (own)
			break;

		/* Perhaps bluetoothd's own whose handle is not known yet. */
		if (pushing) {
			map_hold(map, event);
			break;
		}

		/* Else a message the phone's user sent. */
		if (text_message)
			map_live_start(map, event->handle, BTD_MAP_FOLDER_SENT);
		break;
	case BTD_MAP_EVENT_SENDING_SUCCESS:
	case BTD_MAP_EVENT_DELIVERY_SUCCESS:
	case BTD_MAP_EVENT_SENDING_FAILURE:
	case BTD_MAP_EVENT_DELIVERY_FAILURE:
		/* bluetoothd's own, perhaps not known yet; else not bluetoothd's to tell. */
		own = map_event_own(map, event);
		if (!own && pushing)
			map_hold(map, event);
		break;
	case BTD_MAP_EVENT_MESSAGE_DELETED:
		/* The subscribers told. */
		(void)snprintf(line,
			       sizeof(line),
			       "PHONE MESSAGE-GONE handle=%08lx.%016llx",
			       (unsigned long)map->session,
			       (unsigned long long)event->handle);
		map->hooks.emit(map->hooks.context, line, NULL, 0U);
		break;
	default:
		/* Memory full or available, and kinds not known: logged. */
		map_log(map, "map: event of type %d", event->type);
		break;
	}
}

/* Tells the state an event gives of a message bluetoothd sent.  Returns 1 when the message is bluetoothd's own, else 0. */
static int
map_event_own(
	struct btd_map *map,
	const struct btd_map_event *event)
{
	struct btd_map_sent *sent;

	/* A message bluetoothd sent. */
	sent = map_sent_find(map, event->handle);
	if (sent == NULL)
		return 0;

	/* The state the event gives, told once. */
	if (event->type == BTD_MAP_EVENT_DELIVERY_SUCCESS) {
		map_sent_tell(map, sent, MAP_TOLD_DELIVERED, "delivered");
	} else if (event->type == BTD_MAP_EVENT_SENDING_FAILURE || event->type == BTD_MAP_EVENT_DELIVERY_FAILURE) {
		map_sent_tell(map, sent, MAP_TOLD_FAILED, "failed");
	} else {
		map_sent_tell(map, sent, MAP_TOLD_SENT, "sent");
	}

	/* Succeeded: bluetoothd's own. */
	return 1;
}

/* Holds an event until PushMessage's answer names its message (the oldest held is dropped when all are taken). */
static void
map_hold(
	struct btd_map *map,
	const struct btd_map_event *event)
{
	unsigned index;
	unsigned oldest;

	/* A free slot, else the oldest. */
	oldest = 0U;
	for (index = 0U; index < BTD_MAP_HELD_MAX; index++) {
		if (!map->helds[index].used)
			break;
		if (map->helds[index].until < map->helds[oldest].until)
			oldest = index;
	}

	/* None free: the oldest goes. */
	if (index == BTD_MAP_HELD_MAX) {
		map_log(map, "map: a held event dropped");
		index = oldest;
	}

	/* Succeeded: held for its time. */
	map->helds[index].used = 1;
	map->helds[index].until = map_now(map) + BTD_MAP_HELD_MS;
	map->helds[index].event = *event;
}

/* Handles the events held for a message whose handle PushMessage's answer gave. */
static void
map_held_answered(
	struct btd_map *map,
	uint64_t handle)
{
	unsigned index;

	/* Each event held for it. */
	for (index = 0U; index < BTD_MAP_HELD_MAX; index++) {
		if (!map->helds[index].used || map->helds[index].event.handle != handle)
			continue;

		/* Succeeded: out of the hold, told. */
		map->helds[index].used = 0;
		(void)map_event_own(map, &map->helds[index].event);
	}
}

/* Tells the subscribers a state of a sent message, once for each state. */
static void
map_sent_tell(
	struct btd_map *map,
	struct btd_map_sent *sent,
	unsigned told,
	const char *state)
{
	char line[128];

	/* Told already. */
	if ((sent->told & told) != 0U)
		return;

	/* Succeeded: told. */
	sent->told |= told;
	(void)snprintf(line,
		       sizeof(line),
		       "PHONE SENT request=%u handle=%08lx.%016llx state=%s",
		       sent->request,
		       (unsigned long)map->session,
		       (unsigned long long)sent->handle,
		       state);
	map->hooks.emit(map->hooks.context, line, NULL, 0U);
}

/* Finds a message bluetoothd sent by its handle, or NULL. */
static struct btd_map_sent *
map_sent_find(
	struct btd_map *map,
	uint64_t handle)
{
	unsigned index;

	/* Each one followed. */
	for (index = 0U; index < BTD_MAP_SENT_MAX; index++) {
		if (map->sents[index].used && map->sents[index].handle == handle)
			return &map->sents[index];
	}

	/* Not one of bluetoothd's. */
	return NULL;
}

/* Tells the subscribers that a message was dropped: they synchronise again. */
static void
map_dropped(
	struct btd_map *map)
{
	/* Counted and logged. */
	map->dropped++;
	map_log(map, "map: an announced message dropped");

	/* Succeeded: told. */
	map->hooks.emit(map->hooks.context, "PHONE DROPPED", NULL, 0U);
}

/*
 * Tells the folder an event names by its last part (case not minded):
 * the inbox, the sent folder, or the outbox (BTD_MAP_FOLDERS).  Returns
 * 1, or 0 for another folder.
 */
static int
map_folder_of(
	const char *folder,
	unsigned *number)
{
	const char *slash;
	const char *last;
	size_t length;
	size_t index;
	char name[8];
	int same;

	/* The last part. */
	slash = strrchr(folder, '/');
	last = folder;
	if (slash != NULL)
		last = slash + 1;
	length = strlen(last);
	if (length >= sizeof(name))
		return 0;

	/* In lower case. */
	for (index = 0U; index <= length; index++) {
		name[index] = last[index];
		if (name[index] >= 'A' && name[index] <= 'Z')
			name[index] = (char)(name[index] - 'A' + 'a');
	}

	/* The inbox. */
	same = strcmp(name, "inbox");
	if (same == 0) {
		*number = BTD_MAP_FOLDER_INBOX;
		return 1;
	}

	/* The sent folder. */
	same = strcmp(name, "sent");
	if (same == 0) {
		*number = BTD_MAP_FOLDER_SENT;
		return 1;
	}

	/* The outbox. */
	same = strcmp(name, "outbox");
	if (same == 0) {
		*number = BTD_MAP_FOLDERS;
		return 1;
	}

	/* Another folder. */
	return 0;
}

/* Selects the bytes whose complete length appears in the daemon protocol. */
static const uint8_t *
map_message_body(
	const struct btd_bmsg *message,
	size_t *length)
{
	/* MMS carries binary attachments separately from its extracted caption. */
	if (message->mime != NULL) {
		*length = message->mime_length;
		return message->mime;
	}

	/* Ordinary messages retain their original UTF-8 transport. */
	*length = message->text_length;
	return (const uint8_t *)message->text;
}

/*
 * Writes the line of a message (section 9.3) for the bMessage just read:
 * its handle, key, folder, direction, time and zone, the phone's datetime,
 * the peer's number and name, read, partial, truncated and the text's
 * length.  The entry is its listing's (NULL: not found, the message is
 * partial).  Returns 0, or ENOSPC.
 */
static int
map_item(
	struct btd_map *map,
	char *line,
	size_t size,
	uint64_t handle,
	unsigned folder,
	const struct btd_map_entry *entry)
{
	const struct btd_bmsg *message;
	const char *datetime;
	const char *peer;
	const char *name;
	const char *zone;
	const char *direction;
	const char *type_field;
	unsigned key_folder;
	uint64_t key_value;
	char key[20];
	int64_t wall;
	int64_t seconds;
	int32_t local;
	size_t used;
	int written;
	int partial;
	int read;
	int from;
	int error;
	const uint8_t *payload;
	size_t payload_length;

	/* The peer: the sender of a message received, the recipient of one sent; the listing's first, else the bMessage's. */
	message = &map->message;
	payload = map_message_body(message, &payload_length);
	datetime = "";
	if (folder == BTD_MAP_FOLDER_INBOX) {
		peer = message->originator_number;
		name = message->originator_name;
		if (entry != NULL && entry->sender_addressing[0] != '\0')
			peer = entry->sender_addressing;
		if (entry != NULL && entry->sender_name[0] != '\0')
			name = entry->sender_name;
	} else {
		peer = message->recipient_number;
		name = message->recipient_name;
		if (entry != NULL && entry->recipient_addressing[0] != '\0')
			peer = entry->recipient_addressing;
		if (entry != NULL && entry->recipient_name[0] != '\0')
			name = entry->recipient_name;
	}

	/* MMS keys have a separate identity domain; existing SMS keys stay stable. */
	key_folder = folder;
	if (message->type == BTD_MAP_TYPE_MMS)
		key_folder |= 0x100U;

	/* The phone's datetime, from the listing. */
	if (entry != NULL)
		datetime = entry->datetime;

	/* The time: the phone's datetime in seconds, else when it came. */
	wall = map->hooks.wall(map->hooks.context);
	local = map->hooks.local_offset(map->hooks.context, wall);
	error = btd_mapxml_time_unix(datetime, strlen(datetime), map->mse_known, map->mse_offset, local, &seconds, &from);
	if (error == 0) {
		zone = "local";
		if (from == BTD_MAP_FROM_PHONE)
			zone = "phone";
		if (from == BTD_MAP_FROM_MSE)
			zone = "mse";
		partial = 0;
		key_value = map_key(key_folder, datetime, peer, payload, payload_length);
		(void)snprintf(key,
			       sizeof(key),
			       "%016llx",
			       (unsigned long long)key_value);
	} else {
		seconds = wall;
		zone = "received";
		partial = 1;
		(void)snprintf(key, sizeof(key), "%s", "-");
	}

	/* A message not found in its listing is partial too. */
	if (entry == NULL)
		partial = 1;

	/* Read as the listing said; not said, a received one is taken as unread. */
	read = 1;
	if (folder == BTD_MAP_FOLDER_INBOX)
		read = 0;
	if (entry != NULL && entry->read == BTD_MAP_READ_YES)
		read = 1;
	if (entry != NULL && entry->read == BTD_MAP_READ_NO)
		read = 0;

	/* The direction: in for the inbox, out for the sent folder. */
	direction = "out";
	if (folder == BTD_MAP_FOLDER_INBOX)
		direction = "in";

	/* Existing SMS metadata stays compatible; MMS carries an explicit type. */
	type_field = "";
	if (message->type == BTD_MAP_TYPE_MMS)
		type_field = " type=mms format=mime";

	/* The fields before the strings. */
	written = snprintf(line,
			   size,
			   "PHONE MESSAGE handle=%08lx.%016llx key=%s folder=%s dir=%s time=%lld zone=%s%s datetime=",
			   (unsigned long)map->session,
			   (unsigned long long)handle,
			   key,
			   map_folder_names[folder],
			   direction,
			   (long long)seconds,
			   zone,
			   type_field);
	if (written < 0 || (size_t)written >= size)
		return ENOSPC;
	used = (size_t)written;

	/* The datetime, the peer's number and name. */
	error = btd_phoneio_quote(line, size, &used, datetime, BTD_PHONEIO_TEXT_MAX);
	if (error != 0)
		return error;
	written = snprintf(line + used, size - used, " peer=");
	if (written < 0 || (size_t)written >= size - used)
		return ENOSPC;
	used += (size_t)written;
	error = btd_phoneio_quote(line, size, &used, peer, BTD_PHONEIO_TEXT_MAX);
	if (error != 0)
		return error;
	written = snprintf(line + used, size - used, " name=");
	if (written < 0 || (size_t)written >= size - used)
		return ENOSPC;
	used += (size_t)written;
	error = btd_phoneio_quote(line, size, &used, name, BTD_PHONEIO_TEXT_MAX);
	if (error != 0)
		return error;

	/* The flags and the text's length. */
	written = snprintf(line + used,
			   size - used,
			   " read=%d partial=%d truncated=%d length=%lu",
			   read,
			   partial,
			   message->truncated,
			   (unsigned long)payload_length);
	if (written < 0 || (size_t)written >= size - used)
		return ENOSPC;

	/* Succeeded: the line. */
	return 0;
}

/*
 * Makes a message's key (section 8.3, the app's Source): FNV-1a of its
 * direction, the phone's datetime as written, the peer's number (only its
 * digits and dialling signs) and the text, parted by '|'.  It names the
 * message across MAP sessions, whose handles differ.
 */
static uint64_t
map_key(
	unsigned folder,
	const char *datetime,
	const char *peer,
	const uint8_t *text,
	size_t length)
{
	uint64_t hash;
	uint8_t byte;
	size_t index;

	/* The direction. */
	hash = MAP_FNV_BASIS;
	byte = 'o';
	if (folder == BTD_MAP_FOLDER_INBOX)
		byte = 'i';
	hash = map_hash(hash, &byte, 1U);
	hash = map_hash(hash, (const uint8_t *)"|", 1U);

	/* The datetime. */
	hash = map_hash(hash, (const uint8_t *)datetime, strlen(datetime));
	hash = map_hash(hash, (const uint8_t *)"|", 1U);

	/* The peer's digits and dialling signs. */
	for (index = 0U; peer[index] != '\0'; index++) {
		byte = (uint8_t)peer[index];
		if ((byte >= '0' && byte <= '9') ||
		    byte == '+' ||
		    byte == '*' ||
		    byte == '#')
			hash = map_hash(hash, &byte, 1U);
	}

	/* The part after the peer. */
	hash = map_hash(hash, (const uint8_t *)"|", 1U);

	/* The text. */
	hash = map_hash(hash, text, length);

	/* The key. */
	return hash;
}

/* Adds bytes to a 64-bit FNV-1a hash. */
static uint64_t
map_hash(
	uint64_t hash,
	const uint8_t *bytes,
	size_t length)
{
	size_t index;

	/* Each byte: mixed in, then multiplied by the prime. */
	for (index = 0U; index < length; index++) {
		hash ^= bytes[index];
		hash *= MAP_FNV_PRIME;
	}

	/* The hash so far. */
	return hash;
}

/* Gives the word of an ERROR for an operation's failure (section 8.4). */
static const char *
map_error_word(
	int error,
	uint8_t code)
{
	/* The codes told apart. */
	if (code == BTD_OBEX_NOT_FOUND)
		return "not-found";
	if (code == BTD_OBEX_SERVICE_UNAVAILABLE)
		return "unavailable";
	if (code == BTD_OBEX_UNAUTHORIZED || code == BTD_OBEX_FORBIDDEN)
		return "permission";

	/* No answer in time. */
	if (error == ETIMEDOUT)
		return "timeout";

	/* Any other refusal. */
	return "refused";
}

/*
 * Reads a cursor: <session, 8 hex digits>.<since, 1 to 16 hex
 * digits>.<folder>.<offset, decimal>.<capped, 0 or 1>.  Returns 0, or
 * EINVAL.
 */
static int
map_parse_cursor(
	const char *cursor,
	uint32_t *session,
	uint64_t *since,
	unsigned *folder,
	unsigned *offset,
	int *capped)
{
	const char *dot;
	const char *part;
	uint64_t value;
	size_t length;
	size_t index;
	int error;

	/* The session. */
	part = cursor;
	dot = strchr(part, '.');
	if (dot == NULL || dot - part != 8)
		return EINVAL;
	error = map_hex(part, 8U, &value);
	if (error != 0)
		return EINVAL;
	*session = (uint32_t)value;

	/* The time since. */
	part = dot + 1;
	dot = strchr(part, '.');
	if (dot == NULL)
		return EINVAL;
	error = map_hex(part, (size_t)(dot - part), since);
	if (error != 0)
		return EINVAL;

	/* The folder: one digit. */
	part = dot + 1;
	if (part[0] < '0' ||
	    part[0] > '9' ||
	    part[1] != '.')
		return EINVAL;
	*folder = (unsigned)(part[0] - '0');

	/* The offset: one to four decimal digits up to the last dot. */
	part += 2;
	dot = strchr(part, '.');
	if (dot == NULL)
		return EINVAL;
	length = (size_t)(dot - part);
	if (length == 0U || length > 4U)
		return EINVAL;
	*offset = 0U;
	for (index = 0U; index < length; index++) {
		if (part[index] < '0' || part[index] > '9')
			return EINVAL;
		*offset = *offset * 10U + (unsigned)(part[index] - '0');
	}

	/* Whether a folder was cut at the limit: 0 or 1, the end. */
	part = dot + 1;
	if ((part[0] != '0' && part[0] != '1') || part[1] != '\0')
		return EINVAL;
	*capped = part[0] - '0';

	/* Succeeded: the cursor read. */
	return 0;
}

/* Reads a handle of the socket: <session, 8 hex digits>.<handle, 1 to 16 hex digits>.  Returns 0, or EINVAL. */
static int
map_parse_handle(
	const char *text,
	uint32_t *session,
	uint64_t *handle)
{
	const char *dot;
	uint64_t value;
	size_t length;
	int error;

	/* The session. */
	dot = strchr(text, '.');
	if (dot == NULL || dot - text != 8)
		return EINVAL;
	error = map_hex(text, 8U, &value);
	if (error != 0)
		return EINVAL;
	*session = (uint32_t)value;

	/* The phone's handle. */
	length = strlen(dot + 1);
	error = map_hex(dot + 1, length, handle);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the handle read. */
	return 0;
}

/* Reads 1 to 16 hex digits.  Returns 0 with their value, or EINVAL. */
static int
map_hex(
	const char *text,
	size_t length,
	uint64_t *value)
{
	uint64_t number;
	size_t index;
	char letter;
	unsigned digit;

	/* 1 to 16 digits. */
	if (length == 0U || length > 16U)
		return EINVAL;

	/* Each digit. */
	number = 0U;
	for (index = 0U; index < length; index++) {
		letter = text[index];
		if (letter >= '0' && letter <= '9') {
			digit = (unsigned)(letter - '0');
		} else if (letter >= 'a' && letter <= 'f') {
			digit = (unsigned)(letter - 'a' + 10);
		} else if (letter >= 'A' && letter <= 'F') {
			digit = (unsigned)(letter - 'A' + 10);
		} else {
			return EINVAL;
		}

		/* The digit added. */
		number = number * 16U + digit;
	}

	/* Succeeded: the value. */
	*value = number;
	return 0;
}

/*
 * Writes the time a filter starts from (FilterPeriodBegin) in the phone's
 * zone: from MSETime when the session learned it, else zedBSD's
 * (*guessed set, with that zone).
 */
static void
map_period(
	struct btd_map *map,
	int64_t since,
	char *text,
	size_t size,
	int *guessed,
	int32_t *offset)
{
	int error;

	/* The phone's zone, or zedBSD's. */
	*guessed = 0;
	*offset = map->mse_offset;
	if (!map->mse_known) {
		*guessed = 1;
		*offset = map->hooks.local_offset(map->hooks.context, since);
	}

	/* The local time there; a time out of range starts from 1970. */
	error = btd_mapxml_time_format(since, *offset, text, size);
	if (error != 0)
		(void)snprintf(text, size, "%s", "19700101T000000");
}

/* Adds an application parameter: its tag, its length, its value. */
static void
map_parameter(
	uint8_t *bytes,
	size_t *used,
	uint8_t tag,
	const uint8_t *value,
	size_t length)
{
	/* A parameter that does not fit is left out (the room is made for every request). */
	if (length > 255U || *used + 2U + length > MAP_PARAMETERS_MAX)
		return;

	/* Succeeded: added. */
	bytes[*used] = tag;
	bytes[*used + 1U] = (uint8_t)length;
	memcpy(bytes + *used + 2U, value, length);
	*used += 2U + length;
}

/* Adds an application parameter of two bytes, big-endian. */
static void
map_parameter16(
	uint8_t *bytes,
	size_t *used,
	uint8_t tag,
	unsigned value)
{
	uint8_t number[2];

	/* Succeeded: added. */
	number[0] = (uint8_t)((value >> 8) & 0xffU);
	number[1] = (uint8_t)(value & 0xffU);
	map_parameter(bytes, used, tag, number, sizeof(number));
}

/* Adds an application parameter of one byte. */
static void
map_parameter8(
	uint8_t *bytes,
	size_t *used,
	uint8_t tag,
	unsigned value)
{
	uint8_t number;

	/* Succeeded: added. */
	number = (uint8_t)value;
	map_parameter(bytes, used, tag, &number, 1U);
}

/* Writes OBEX's bytes on the MAS's DLC (OBEX's write hook). */
static int
map_mas_write(
	void *context,
	const uint8_t *data,
	size_t length,
	size_t *written)
{
	struct btd_map *map;
	int error;

	/* No DLC. */
	map = context;
	*written = 0U;
	if (map->mas_dlci == 0U)
		return ENOTCONN;

	/* As far as the DLC takes them. */
	error = map->hooks.dlc_write(map->hooks.context, map->mas_dlci, data, length, written);
	if (error != 0)
		return error;

	/* Succeeded: what went is in written. */
	return 0;
}

/* Notes the end of the MAS's request (OBEX's done hook): map_settle handles it. */
static void
map_mas_done(
	void *context,
	unsigned operation,
	int error,
	uint8_t code,
	const uint8_t *headers,
	size_t length)
{
	struct btd_map *map;

	UNUSED_PARAMETER(operation);
	UNUSED_PARAMETER(headers);
	UNUSED_PARAMETER(length);

	/* Succeeded: noted. */
	map = context;
	map->finished = 1;
	map->finished_error = error;
	map->finished_code = code;
}

/*
 * Notes what an answer packet of the MAS says besides its body (OBEX's
 * response hook): a listing's size and the phone's time, a sent message's
 * handle.
 */
static void
map_mas_response(
	void *context,
	unsigned operation,
	uint8_t code,
	const uint8_t *headers,
	size_t length)
{
	struct btd_map_time time;
	struct btd_obex_header header;
	struct btd_map *map;
	const uint8_t *value;
	size_t offset;
	size_t at;
	size_t size;
	uint8_t tag;
	int read;
	int error;

	UNUSED_PARAMETER(operation);
	UNUSED_PARAMETER(code);

	/* Each header. */
	map = context;
	offset = 0U;
	for (;;) {
		read = btd_obex_header_next(headers, length, &offset, &header);
		if (read <= 0)
			break;

		/* A sent message's handle in Name. */
		if (header.id == BTD_OBEX_NAME) {
			error = map_name_handle(&header, &map->name_handle);
			if (error == 0)
				map->have_name = 1;
			continue;
		}

		/* Only the application parameters besides. */
		if (header.id != BTD_OBEX_APPLICATION)
			continue;

		/* Each parameter: its tag, its length, its value. */
		at = 0U;
		while (at + 2U <= header.length) {
			tag = header.data[at];
			size = header.data[at + 1U];
			value = header.data + at + 2U;
			if (at + 2U + size > header.length)
				break;
			at += 2U + size;

			/* The listing's size. */
			if (tag == MAP_TAG_LISTING_SIZE && size == 2U) {
				map->have_size = 1;
				map->listing_size = ((unsigned)value[0] << 8) | value[1];
				continue;
			}

			/* The phone's time, whose offset gives its zone. */
			if (tag == MAP_TAG_MSE_TIME) {
				error = btd_mapxml_time_parse((const char *)value, size, &time);
				if (error != 0 || time.zone == BTD_MAP_ZONE_NONE)
					continue;
				map->have_mse = 1;
				map->mse_answer_offset = time.offset;
			}
		}
	}
}

/* Gathers a Get's body from the MAS (OBEX's body hook); past the room it stops the Get. */
static int
map_mas_body(
	void *context,
	const uint8_t *data,
	size_t length)
{
	struct btd_map *map;
	uint8_t *body;
	size_t wanted;
	size_t capacity;

	/* Bounds the complete operation before growing its reusable input buffer. */
	map = context;
	if (length > BTD_MAP_BODY_MAX - map->body_length)
		return 1;
	wanted = map->body_length + length;
	if (wanted > map->body_capacity) {
		capacity = map->body_capacity;
		if (capacity == 0U)
			capacity = 65536U;

		/* Doubles the retained buffer up to the bounded MMS operation size. */
		while (capacity < wanted && capacity < BTD_MAP_BODY_MAX / 2U)
			capacity *= 2U;
		if (capacity < wanted)
			capacity = BTD_MAP_BODY_MAX;
		body = realloc(map->body, capacity);
		if (body == NULL)
			return 1;
		map->body = body;
		map->body_capacity = capacity;
	}

	/* Succeeded: gathered. */
	memcpy(map->body + map->body_length, data, length);
	map->body_length += length;
	return 0;
}

/* Writes OBEX's bytes on the MNS's DLC (OBEX's write hook). */
static int
map_mns_write(
	void *context,
	const uint8_t *data,
	size_t length,
	size_t *written)
{
	struct btd_map *map;
	int error;

	/* No DLC. */
	map = context;
	*written = 0U;
	if (map->mns_dlci == 0U)
		return ENOTCONN;

	/* As far as the DLC takes them. */
	error = map->hooks.dlc_write(map->hooks.context, map->mns_dlci, data, length, written);
	if (error != 0)
		return error;

	/* Succeeded: what went is in written. */
	return 0;
}

/* Answers whether the phone's Connect on the MNS names the MNS's Target (OBEX's target hook). */
static int
map_mns_target(
	void *context,
	const uint8_t *target,
	size_t length)
{
	struct btd_map *map;
	int same;

	/* The MNS's Target, 16 bytes. */
	map = context;
	if (target == NULL || length != sizeof(map_mns_uuid))
		return 0;
	same = memcmp(target, map_mns_uuid, sizeof(map_mns_uuid));
	if (same != 0)
		return 0;

	/* Succeeded: served, the MNS connected. */
	map->mns_connected = 1;
	return 1;
}

/*
 * Takes an event report the phone puts on the MNS (OBEX's put hook, MAP
 * section 5.1): of the MAS of this MAP (or of none named), read and kept
 * for map_settle.  Answers Success, Bad Request for a report that cannot
 * be read, Not Implemented for another type.
 */
static uint8_t
map_mns_put(
	void *context,
	const uint8_t *headers,
	size_t length,
	const uint8_t *body,
	size_t body_length)
{
	static const char type[] = "x-bt/MAP-event-report";
	struct btd_obex_header header;
	struct btd_map_event event;
	struct btd_map *map;
	size_t at;
	int found;
	int same;
	int error;

	/* The event report's type, its NUL not minded. */
	map = context;
	found = btd_obex_find(headers, length, BTD_OBEX_TYPE, &header);
	if (found <= 0)
		return BTD_OBEX_NOT_IMPLEMENTED;
	if (header.length > 0U && header.data[header.length - 1U] == 0U)
		header.length--;
	if (header.length != sizeof(type) - 1U)
		return BTD_OBEX_NOT_IMPLEMENTED;
	same = memcmp(header.data, type, header.length);
	if (same != 0)
		return BTD_OBEX_NOT_IMPLEMENTED;

	/* The MAS it is of: another MAS's event is taken and dropped. */
	found = btd_obex_find(headers, length, BTD_OBEX_APPLICATION, &header);
	if (found > 0) {
		at = 0U;
		while (at + 2U <= header.length) {
			if (header.data[at] == MAP_TAG_INSTANCE &&
			    header.data[at + 1U] == 1U &&
			    at + 3U <= header.length) {
				if (header.data[at + 2U] != map->mas_instance)
					return BTD_OBEX_SUCCESS;
				break;
			}

			/* The next parameter. */
			at += 2U + header.data[at + 1U];
		}
	}

	/* The report. */
	error = btd_mapxml_event(body, body_length, &event);
	if (error != 0) {
		map_log(map, "map: event report rejected error=%d bytes=%lu", error, (unsigned long)body_length);
		return BTD_OBEX_BAD_REQUEST;
	}

	/* Kept, or counted as lost when too many wait. */
	if (map->event_count >= BTD_MAP_EVENTS_MAX) {
		map->events_dropped = 1;
		return BTD_OBEX_SUCCESS;
	}

	/* Kept for map_settle. */
	map->events[map->event_count] = event;
	map->event_count++;

	/* Succeeded: taken. */
	return BTD_OBEX_SUCCESS;
}

/*
 * Reads a sent message's handle from a Name header: UTF-16BE of 1 to 16
 * hex digits, its NUL not minded.  Returns 0, or EINVAL.
 */
static int
map_name_handle(
	const struct btd_obex_header *header,
	uint64_t *handle)
{
	char text[17];
	size_t count;
	size_t index;
	int error;

	/* Each UTF-16 unit, an ASCII hex digit, up to the NUL. */
	count = 0U;
	for (index = 0U; index + 1U < header->length; index += 2U) {
		if (header->data[index] == 0U && header->data[index + 1U] == 0U)
			break;
		if (header->data[index] != 0U || count >= 16U)
			return EINVAL;
		text[count] = (char)header->data[index + 1U];
		count++;
	}

	/* The digits. */
	error = map_hex(text, count, handle);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the handle. */
	return 0;
}

/* Gives the earlier of two deadlines, 0 standing for none. */
static uint64_t
map_earlier(
	uint64_t earliest,
	uint64_t deadline)
{
	/* No deadline. */
	if (deadline == 0U)
		return earliest;

	/* The first, or an earlier one. */
	if (earliest == 0U || deadline < earliest)
		return deadline;

	/* The one there was. */
	return earliest;
}
