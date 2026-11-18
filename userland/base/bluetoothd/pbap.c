/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Phone Book Access Profile's client (ws197-p005, see pbap.h).
 */

#include "userland/base/bluetoothd/pbap.h"

#include "userland/base/bluetoothd/phoneio.h"
#include "userland/base/bluetoothd/rfcomm.h"
#include "userland/base/bluetoothd/vcard.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Marks a hook's parameter this implementation does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The PSE's record: its SupportedRepositories and PbapSupportedFeatures (Assigned Numbers, checked on the phone). */
#define PBAP_ATTRIBUTE_REPOSITORIES	0x0314U
#define PBAP_ATTRIBUTE_FEATURES		0x0317U

/* The local phone book's bit of SupportedRepositories, and what a record without the attribute is taken to have. */
#define PBAP_REPOSITORY_LOCAL		0x01U
#define PBAP_REPOSITORIES_ASSUMED	0x01U

/* The application parameters' tags (PBAP section 6.2.1). */
#define PBAP_TAG_MAX_LIST_COUNT		0x04U
#define PBAP_TAG_START_OFFSET		0x05U
#define PBAP_TAG_PROPERTY_SELECTOR	0x06U
#define PBAP_TAG_FORMAT			0x07U
#define PBAP_TAG_PHONEBOOK_SIZE		0x08U

/* vCard 3.0, PullPhoneBook's Format. */
#define PBAP_FORMAT_30			0x01U

/* The room of a request's headers and of its application parameters. */
#define PBAP_HEADERS_MAX		256U
#define PBAP_PARAMETERS_MAX		64U

/* The operations: an object's size alone (MaxListCount 0), and a page of its cards. */
#define PBAP_OP_SIZE			1U
#define PBAP_OP_PULL			2U

/*
 * A page's steps: the object's size at the pass's start, the cards'
 * Get, the cards written to the client, the phone book's size again at
 * the pass's end, the page's end written.
 */
#define PBAP_STEP_SIZE			1U
#define PBAP_STEP_PULL			2U
#define PBAP_STEP_EMIT			3U
#define PBAP_STEP_END_SIZE		4U
#define PBAP_STEP_FINISH		5U

/* The wait after a failure: none (not on this link), growing from 30 s, or ten minutes. */
#define PBAP_RETRY_NEVER		0U
#define PBAP_RETRY_STEP			1U
#define PBAP_RETRY_LONG			2U

/* The longest line the daemon writes, with its NUL. */
#define PBAP_LINE_MAX			(BTD_PHONEIO_OUT_MAX + 1U)

/* The most DLCs noted to close at the next tick (as pbap.h's closing list). */
#define PBAP_CLOSING_MAX		4U

/* PBAP's OBEX Target (PBAP section 6.4). */
static const uint8_t pbap_target[16] = {
	0x79U, 0x61U, 0x35U, 0xf0U, 0xf0U, 0xc5U, 0x11U, 0xd8U, 0x09U, 0x66U, 0x08U, 0x00U, 0x20U, 0x0cU, 0x9aU, 0x66U
};

/*
 * The vCard fields asked for (PropertySelector, PBAP table 5.1): the
 * phone book's VERSION, FN, N, TEL and UID (bits 0, 1, 2, 7, 21); the
 * history's VERSION, FN, N, TEL and X-IRMC-CALL-DATETIME (bit 28).
 */
static const uint8_t pbap_contact_fields[8] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x20U, 0x00U, 0x87U };
static const uint8_t pbap_call_fields[8] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x10U, 0x00U, 0x00U, 0x87U };

/* The objects' names, in BTD_PBAP_OBJECT_* order. */
static const char *const pbap_object_names[BTD_PBAP_OBJECTS] = {
	"telecom/pb.vcf", "telecom/ich.vcf", "telecom/och.vcf", "telecom/mch.vcf"
};

static uint64_t pbap_now(const struct btd_pbap *pbap);
static void pbap_log(struct btd_pbap *pbap, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void pbap_changed(struct btd_pbap *pbap);
static void pbap_start(struct btd_pbap *pbap);
static void pbap_open(struct btd_pbap *pbap);
static void pbap_fail(struct btd_pbap *pbap, const char *why, unsigned retry);
static void pbap_closed_unready(struct btd_pbap *pbap, int reason);
static void pbap_close_all(struct btd_pbap *pbap);
static void pbap_close_later(struct btd_pbap *pbap, unsigned dlci);
static void pbap_end_pages(struct btd_pbap *pbap, const char *word);
static void pbap_shutdown(struct btd_pbap *pbap);
static void pbap_settle(struct btd_pbap *pbap);
static void pbap_connect_finished(struct btd_pbap *pbap);
static void pbap_next(struct btd_pbap *pbap);
static int pbap_head(const struct btd_pbap *pbap, unsigned *index);
static void pbap_run(struct btd_pbap *pbap, unsigned index, unsigned op);
static int pbap_get(struct btd_pbap *pbap, const struct btd_pbap_page *page, unsigned op);
static void pbap_op_finished(struct btd_pbap *pbap);
static void pbap_size_finished(struct btd_pbap *pbap, struct btd_pbap_page *page, int error);
static void pbap_pull_finished(struct btd_pbap *pbap, struct btd_pbap_page *page, int error);
static void pbap_page_continue(struct btd_pbap *pbap, unsigned index);
static void pbap_emit_contact(struct btd_pbap *pbap, struct btd_pbap_page *page, const uint8_t *card, size_t length);
static void pbap_emit_call(struct btd_pbap *pbap, struct btd_pbap_page *page, const uint8_t *card, size_t length);
static void pbap_after_body(struct btd_pbap_page *page, unsigned received);
static void pbap_next_object(struct btd_pbap_page *page);
static void pbap_page_finish(struct btd_pbap *pbap, unsigned index);
static void pbap_page_end(struct btd_pbap *pbap, unsigned index, const char *word);
static void pbap_page_free(struct btd_pbap *pbap, unsigned index);
static int pbap_parse_cursor(const char *cursor, uint32_t *generation, unsigned *object, unsigned *offset, unsigned *size);
static int pbap_parse_number(const char **text, int hex, unsigned *value);
static void pbap_parameter(uint8_t *bytes, size_t *used, uint8_t tag, const uint8_t *value, size_t length);
static void pbap_parameter16(uint8_t *bytes, size_t *used, uint8_t tag, unsigned value);
static int pbap_write(void *context, const uint8_t *data, size_t length, size_t *written);
static void pbap_done(void *context, unsigned operation, int error, uint8_t code, const uint8_t *headers, size_t length);
static void pbap_response(void *context, unsigned operation, uint8_t code, const uint8_t *headers, size_t length);
static int pbap_body(void *context, const uint8_t *data, size_t length);
static uint64_t pbap_earlier(uint64_t earliest, uint64_t deadline);

/*
 * Prepares the PBAP client: off, no page; the cursors' generations count
 * from first_generation (a number the daemon draws at its start, so a
 * cursor of an earlier run is not taken for one of this run).
 */
void
btd_pbap_init(
	struct btd_pbap *pbap,
	const struct btd_pbap_hooks *hooks,
	uint32_t first_generation)
{
	/* Nothing under way. */
	memset(pbap, 0, sizeof(*pbap));
	pbap->hooks = *hooks;
	pbap->state = BTD_PBAP_OFF;
	pbap->generation = first_generation;
	pbap->next_serial = 1U;
}

/*
 * Takes the phone link's readiness (the profile's ready hook): a phone
 * other than the last one makes the earlier cursors stale; PBAP starts
 * when the owner wants contacts.
 */
void
btd_pbap_ready(
	void *context)
{
	struct btd_pbap *pbap;
	uint8_t address[6];
	int same;
	int error;
	int wanted;

	/* The link is there for PBAP. */
	pbap = context;
	pbap->link_ready = 1;
	pbap->retry_step = 0U;

	/* The phone on the link: another one than before starts a new generation of cursors. */
	error = pbap->hooks.address(pbap->hooks.context, address);
	if (error == 0) {
		same = memcmp(address, pbap->phone, sizeof(address));
		if (pbap->have_phone && same != 0)
			pbap->generation++;
		memcpy(pbap->phone, address, sizeof(address));
		pbap->have_phone = 1;
	}

	/* Started when contacts are wanted. */
	wanted = pbap->hooks.wanted(pbap->hooks.context);
	if (wanted && pbap->state == BTD_PBAP_OFF)
		pbap_start(pbap);

	/* Succeeded: what follows is settled. */
	pbap_settle(pbap);
}

/*
 * Takes the end of the phone link (the profile's ended hook): every page
 * is answered as lost and PBAP is off until the next link.
 */
void
btd_pbap_ended(
	void *context)
{
	struct btd_pbap *pbap;

	/* The link is gone. */
	pbap = context;
	pbap->link_ready = 0;

	/* Succeeded: off. */
	pbap_shutdown(pbap);
}

/*
 * Takes the end of the SDP query for the PSE (the profile's sdp_done
 * hook): the first PSE record with an RFCOMM channel is chosen, and its
 * DLC asked for.  A phone without a PSE, or whose PSE has no phone book
 * of its own, is not asked again on this link.
 */
void
btd_pbap_sdp_done(
	void *context,
	const struct btd_sdp *sdp,
	int error)
{
	struct btd_pbap *pbap;
	unsigned count;
	unsigned nth;
	unsigned channel;
	uint32_t value;
	int found;
	int has;

	/* Only the query PBAP asked for. */
	pbap = context;
	if (pbap->state != BTD_PBAP_SDP)
		return;

	/* A query that failed: tried again later. */
	if (error != 0) {
		pbap_fail(pbap, "sdp", PBAP_RETRY_STEP);
		pbap_settle(pbap);
		return;
	}

	/* The first PSE record with an RFCOMM channel. */
	found = 0;
	channel = 0U;
	nth = 0U;
	count = btd_sdp_records(sdp, BTD_SDP_UUID_PSE);
	for (nth = 0U; nth < count; nth++) {
		/* Its channel. */
		has = btd_sdp_rfcomm_channel(sdp, BTD_SDP_UUID_PSE, nth, &channel);
		if (has == 0) {
			found = 1;
			break;
		}
	}

	/* No PSE: not asked again on this link. */
	if (!found) {
		pbap_fail(pbap, "no-pse", PBAP_RETRY_NEVER);
		pbap_settle(pbap);
		return;
	}

	/* Its repositories (the local phone book when not said) and whether it names its features. */
	pbap->pse_channel = channel;
	pbap->repositories = PBAP_REPOSITORIES_ASSUMED;
	has = btd_sdp_uint_attribute(sdp, BTD_SDP_UUID_PSE, nth, PBAP_ATTRIBUTE_REPOSITORIES, &value);
	if (has == 0)
		pbap->repositories = value;
	pbap->has_features = 0;
	has = btd_sdp_uint_attribute(sdp, BTD_SDP_UUID_PSE, nth, PBAP_ATTRIBUTE_FEATURES, &value);
	if (has == 0)
		pbap->has_features = 1;

	/* A PSE without the phone's own phone book (a SIM's alone): not asked again on this link. */
	if ((pbap->repositories & PBAP_REPOSITORY_LOCAL) == 0U) {
		pbap_fail(pbap, "no-pb", PBAP_RETRY_NEVER);
		pbap_settle(pbap);
		return;
	}

	/* Succeeded: its DLC asked for. */
	pbap_open(pbap);
	pbap_settle(pbap);
}

/* Tells RFCOMM that PBAP offers no server channel of bluetoothd's (a PCE is a client). */
int
btd_pbap_accept(
	void *context,
	unsigned server_channel)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(server_channel);

	/* None offered. */
	return 0;
}

/*
 * Takes a DLC that opened (the profile's opened hook): the PSE's DLC
 * asked for carries OBEX's Connect at once, its answer awaited a minute
 * while the phone asks its user.  Any other DLC is closed.
 */
void
btd_pbap_opened(
	void *context,
	unsigned dlci,
	unsigned server_channel,
	int ours)
{
	struct btd_obex_events events;
	struct btd_pbap *pbap;
	int error;

	/* Not the DLC asked for (a late one of an earlier attempt): closed at the next tick. */
	pbap = context;
	if (!ours ||
	    pbap->state != BTD_PBAP_OPENING ||
	    server_channel != pbap->pse_channel ||
	    pbap->dlci != 0U) {
		pbap_close_later(pbap, dlci);
		return;
	}

	/* OBEX's client on it. */
	pbap->dlci = dlci;
	memset(&events, 0, sizeof(events));
	events.context = pbap;
	events.write = pbap_write;
	events.done = pbap_done;
	events.response = pbap_response;
	events.body = pbap_body;
	btd_obex_init(&pbap->obex, &events, BTD_OBEX_CLIENT);
	btd_obex_set_timeout(&pbap->obex, BTD_PBAP_CONNECT_MS);

	/* Connect with PBAP's Target, no application parameters (PBAP 1.1, section 5.1 of the design). */
	pbap->state = BTD_PBAP_CONNECTING;
	error = btd_obex_connect(&pbap->obex, pbap_target, sizeof(pbap_target), pbap_now(pbap));
	if (error != 0)
		pbap_fail(pbap, "obex", PBAP_RETRY_STEP);

	/* Succeeded: what follows is settled. */
	pbap_settle(pbap);
}

/* Takes the data of a DLC (the profile's data hook): to the OBEX connection on it. */
void
btd_pbap_data(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length)
{
	struct btd_pbap *pbap;

	/* The PSE's DLC only. */
	pbap = context;
	if (dlci == 0U || dlci != pbap->dlci)
		return;

	/* Succeeded: OBEX reads it, and what it noted is settled. */
	btd_obex_input(&pbap->obex, data, length, pbap_now(pbap));
	pbap_settle(pbap);
}

/* Takes a DLC that may be written again (the profile's writable hook): OBEX goes on. */
void
btd_pbap_writable(
	void *context,
	unsigned dlci)
{
	struct btd_pbap *pbap;

	/* The PSE's DLC only. */
	pbap = context;
	if (dlci == 0U || dlci != pbap->dlci)
		return;

	/* Succeeded: OBEX writes what waited. */
	btd_obex_pump(&pbap->obex);
	pbap_settle(pbap);
}

/*
 * Takes a DLC that closed (the profile's closed hook).  Before the
 * connection was ready, the phone's refusal (its DM, or its DISC) and an
 * unanswered prompt (RFCOMM giving up) wait ten minutes, anything else
 * the usual wait.  Once ready, the phone closing it while nothing runs
 * leaves PBAP idle (the next page opens it again); closing it under an
 * operation fails PBAP.
 */
void
btd_pbap_closed(
	void *context,
	unsigned dlci,
	int reason)
{
	struct btd_pbap *pbap;

	/* The PSE's DLC. */
	pbap = context;
	if (dlci != 0U && dlci == pbap->dlci) {
		pbap->dlci = 0U;

		/* Ready and nothing running: idle, its OBEX connection forgotten. */
		if (pbap->state == BTD_PBAP_READY && !pbap->running) {
			memset(&pbap->obex, 0, sizeof(pbap->obex));
			pbap->state = BTD_PBAP_IDLE;
			pbap_log(pbap, "pbap: idle (the phone closed the connection)");
			pbap_settle(pbap);
			return;
		}

		/* Ready under an operation: failed, its pages lost. */
		if (pbap->state == BTD_PBAP_READY) {
			pbap_fail(pbap, "closed", PBAP_RETRY_STEP);
			pbap_settle(pbap);
			return;
		}

		/* Before ready: by the reason. */
		if (pbap->state == BTD_PBAP_CONNECTING)
			pbap_closed_unready(pbap, reason);
		pbap_settle(pbap);
		return;
	}

	/* The PSE's DLC asked for, closed before it opened (the phone's DM, RFCOMM giving up). */
	if (dlci != 0U &&
	    pbap->state == BTD_PBAP_OPENING &&
	    pbap->dlci == 0U &&
	    (dlci >> 1) == pbap->pse_channel) {
		pbap_closed_unready(pbap, reason);
		pbap_settle(pbap);
	}
}

/* Takes a DLC that could not be asked for (the profile's open_failed hook): the PSE's fails PBAP. */
void
btd_pbap_open_failed(
	void *context,
	unsigned server_channel)
{
	struct btd_pbap *pbap;

	/* The PSE's DLC asked for. */
	pbap = context;
	if (pbap->state != BTD_PBAP_OPENING || server_channel != pbap->pse_channel)
		return;

	/* Succeeded: failed, tried again later. */
	pbap_fail(pbap, "rfcomm", PBAP_RETRY_STEP);
	pbap_settle(pbap);
}

/*
 * Starts a PHONE PAGE contacts|calls request (section 5.2): count cards
 * (1 to 32) from the cursor ("" at the pass's start), for calls only
 * those since a time.  The answers go to token: the items, PHONE
 * PAGE-END, DONE; or ERROR.  Returns NULL when it started, or the word of
 * an ERROR to answer now: not-ready, argument, stale-cursor, busy.  A
 * page that comes while the connection is idle or being made waits for
 * it, at most BTD_PBAP_PAGE_CONNECT_MS.
 */
const char *
btd_pbap_page(
	struct btd_pbap *pbap,
	uint64_t token,
	unsigned what,
	int64_t since,
	const char *cursor,
	unsigned count)
{
	struct btd_pbap_page *page;
	char line[96];
	uint32_t generation;
	unsigned object;
	unsigned offset;
	unsigned size;
	unsigned index;
	int error;

	/* What, how many and since when can be asked. */
	if (what != BTD_PBAP_WHAT_CONTACTS && what != BTD_PBAP_WHAT_CALLS)
		return "argument";
	if (count == 0U || count > BTD_PBAP_COUNT_MAX)
		return "argument";
	if (since < 0)
		return "argument";

	/* PBAP set up, or being set up, on a link. */
	if (!pbap->link_ready)
		return "not-ready";
	if (pbap->state == BTD_PBAP_OFF || pbap->state == BTD_PBAP_FAILED)
		return "not-ready";

	/* The start: the phone book, or the first object of the history. */
	object = BTD_PBAP_OBJECT_PB;
	if (what == BTD_PBAP_WHAT_CALLS)
		object = BTD_PBAP_OBJECT_ICH;
	offset = 0U;
	size = BTD_PBAP_SIZE_UNKNOWN;

	/* The cursor's place, of this generation and of what is read. */
	if (cursor[0] != '\0') {
		error = pbap_parse_cursor(cursor, &generation, &object, &offset, &size);
		if (error != 0)
			return "argument";
		if (generation != pbap->generation)
			return "stale-cursor";
		if (what == BTD_PBAP_WHAT_CONTACTS && object != BTD_PBAP_OBJECT_PB)
			return "argument";
		if (what == BTD_PBAP_WHAT_CALLS && object == BTD_PBAP_OBJECT_PB)
			return "argument";
	}

	/* Past the history's last object: nothing more. */
	if (object >= BTD_PBAP_OBJECTS) {
		(void)snprintf(line, sizeof(line), "PHONE PAGE-END cursor=%s more=0 count=0 skipped=0 capped=0", cursor);
		pbap->hooks.answer(pbap->hooks.context, token, line, NULL, 0U);
		pbap->hooks.answer(pbap->hooks.context, token, "DONE", NULL, 0U);
		return NULL;
	}

	/* A free page. */
	page = NULL;
	for (index = 0U; index < BTD_PBAP_PAGES_MAX; index++) {
		/* Free. */
		if (!pbap->pages[index].used) {
			page = &pbap->pages[index];
			break;
		}
	}

	/* None free. */
	if (page == NULL)
		return "busy";

	/* The page: its object's size first when the pass did not say it. */
	memset(page, 0, sizeof(*page));
	page->used = 1;
	page->serial = pbap->next_serial;
	pbap->next_serial++;
	page->token = token;
	page->what = what;
	page->since = since;
	page->object = object;
	page->offset = offset;
	page->start_size = size;
	page->count = count;
	page->ask = count;
	page->arrived = pbap_now(pbap);
	page->step = PBAP_STEP_PULL;
	if (size == BTD_PBAP_SIZE_UNKNOWN)
		page->step = PBAP_STEP_SIZE;

	/* Succeeded: under way (an idle connection is opened again by the page). */
	pbap_settle(pbap);
	return NULL;
}

/*
 * Forgets the pages of a client that went (the token's): a page whose
 * operation runs ends quietly when it ends.
 */
void
btd_pbap_cancel(
	struct btd_pbap *pbap,
	uint64_t token)
{
	unsigned index;

	/* No client. */
	if (token == 0U)
		return;

	/* Each page of the token. */
	for (index = 0U; index < BTD_PBAP_PAGES_MAX; index++) {
		/* Another client's, or free. */
		if (!pbap->pages[index].used || pbap->pages[index].token != token)
			continue;

		/* Its operation runs: freed when it ends. */
		if (pbap->running && pbap->op_page == index) {
			pbap->pages[index].cancelled = 1;
			continue;
		}

		/* Freed now. */
		pbap_page_free(pbap, index);
	}

	/* Succeeded: what follows is settled. */
	pbap_settle(pbap);
}

/*
 * Looks again whether contacts are wanted (the daemon calls it after
 * PHONE LINK and when the owner changes): not wanted, PBAP stops;
 * wanted, it starts, or a failed one is tried again at once.
 */
void
btd_pbap_check(
	struct btd_pbap *pbap)
{
	int wanted;

	/* No link: nothing to do now. */
	if (!pbap->link_ready)
		return;

	/* Not wanted: off. */
	wanted = pbap->hooks.wanted(pbap->hooks.context);
	if (!wanted) {
		if (pbap->state != BTD_PBAP_OFF)
			pbap_shutdown(pbap);
		return;
	}

	/* Wanted: started, or tried again at once. */
	if (pbap->state == BTD_PBAP_OFF) {
		pbap->retry_step = 0U;
		pbap_start(pbap);
	} else if (pbap->state == BTD_PBAP_FAILED) {
		pbap->retry_step = 0U;
		pbap->retry_at = pbap_now(pbap);
	}

	/* Succeeded: what follows is settled. */
	pbap_settle(pbap);
}

/* Goes on with the pages that wait for their clients to read (the daemon calls it after writing to its clients). */
void
btd_pbap_pump(
	struct btd_pbap *pbap)
{
	/* Succeeded: the pages looked at again with the rest. */
	pbap_settle(pbap);
}

/*
 * Runs PBAP's timers: the DLCs to close, OBEX's answer, a busy SDP query
 * or channel, the attempt after a failure, pages whose clients do not
 * read, and pages that waited too long for the connection.
 */
void
btd_pbap_tick(
	struct btd_pbap *pbap,
	uint64_t now)
{
	unsigned closing[PBAP_CLOSING_MAX];
	unsigned count;
	unsigned index;
	int wanted;

	/* The DLCs noted to close (out of the list first). */
	count = pbap->closing_count;
	memcpy(closing, pbap->closing, count * sizeof(closing[0]));
	pbap->closing_count = 0U;
	for (index = 0U; index < count; index++)
		pbap->hooks.dlc_close(pbap->hooks.context, closing[index]);

	/* OBEX's answer awaited. */
	if (pbap->dlci != 0U)
		btd_obex_tick(&pbap->obex, now);

	/* A busy SDP query asked again. */
	if (pbap->state == BTD_PBAP_SDP &&
	    pbap->sdp_again_at != 0U &&
	    now >= pbap->sdp_again_at) {
		pbap->sdp_again_at = 0U;
		pbap_start(pbap);
	}

	/* A busy channel asked again. */
	if (pbap->state == BTD_PBAP_OPENING &&
	    pbap->open_again_at != 0U &&
	    now >= pbap->open_again_at) {
		pbap->open_again_at = 0U;
		pbap_open(pbap);
	}

	/* A failed PBAP set up again. */
	if (pbap->state == BTD_PBAP_FAILED &&
	    pbap->retry_at != 0U &&
	    now >= pbap->retry_at) {
		pbap->retry_at = 0U;
		wanted = pbap->hooks.wanted(pbap->hooks.context);
		if (pbap->link_ready && wanted)
			pbap_start(pbap);
	}

	/* Each page: its client too slow, or the connection too long in coming. */
	for (index = 0U; index < BTD_PBAP_PAGES_MAX; index++) {
		/* Free. */
		if (!pbap->pages[index].used)
			continue;

		/* Its client does not read. */
		if (pbap->pages[index].waiting &&
		    now - pbap->pages[index].waiting_since >= BTD_PBAP_SLOW_MS) {
			pbap_page_end(pbap, index, "slow");
			continue;
		}

		/* The connection not ready in time (the attempt goes on for the next page). */
		if (pbap->state != BTD_PBAP_READY &&
		    pbap->pages[index].step != PBAP_STEP_EMIT &&
		    now - pbap->pages[index].arrived >= BTD_PBAP_PAGE_CONNECT_MS)
			pbap_page_end(pbap, index, "timeout");
	}

	/* Succeeded: what follows is settled. */
	pbap_settle(pbap);
}

/* Tells when PBAP's next timer is due (0: none), for the daemon's poll. */
uint64_t
btd_pbap_deadline(
	const struct btd_pbap *pbap)
{
	uint64_t earliest;
	uint64_t deadline;
	unsigned index;

	/* DLCs to close: at once. */
	if (pbap->closing_count != 0U)
		return pbap_now(pbap);

	/* OBEX's answer. */
	earliest = 0U;
	if (pbap->dlci != 0U)
		earliest = btd_obex_deadline(&pbap->obex);

	/* A busy SDP query or channel, an attempt after a failure. */
	if (pbap->state == BTD_PBAP_SDP)
		earliest = pbap_earlier(earliest, pbap->sdp_again_at);
	if (pbap->state == BTD_PBAP_OPENING)
		earliest = pbap_earlier(earliest, pbap->open_again_at);
	if (pbap->state == BTD_PBAP_FAILED)
		earliest = pbap_earlier(earliest, pbap->retry_at);

	/* Each page: its slow client, or its wait for the connection. */
	for (index = 0U; index < BTD_PBAP_PAGES_MAX; index++) {
		/* Free. */
		if (!pbap->pages[index].used)
			continue;

		/* Its client's time. */
		if (pbap->pages[index].waiting) {
			deadline = pbap->pages[index].waiting_since + BTD_PBAP_SLOW_MS;
			earliest = pbap_earlier(earliest, deadline);
		}

		/* The connection's time. */
		if (pbap->state != BTD_PBAP_READY && pbap->pages[index].step != PBAP_STEP_EMIT) {
			deadline = pbap->pages[index].arrived + BTD_PBAP_PAGE_CONNECT_MS;
			earliest = pbap_earlier(earliest, deadline);
		}
	}

	/* The earliest, or none. */
	return earliest;
}

/*
 * Writes PBAP's part of PHONE SHOW and PHONE STATE (section 5.4):
 * contacts=off|connecting|ready|failed (idle told as ready), and
 * contacts_why when it failed.  Returns 0, or ENOSPC.
 */
int
btd_pbap_state_text(
	const struct btd_pbap *pbap,
	char *text,
	size_t size)
{
	const char *name;
	int written;

	/* The state's name. */
	if (pbap->state == BTD_PBAP_OFF) {
		name = "off";
	} else if (pbap->state == BTD_PBAP_READY || pbap->state == BTD_PBAP_IDLE) {
		name = "ready";
	} else if (pbap->state == BTD_PBAP_FAILED) {
		name = "failed";
	} else {
		name = "connecting";
	}

	/* The fields, why when it failed. */
	if (pbap->state == BTD_PBAP_FAILED && pbap->why != NULL) {
		written = snprintf(text, size, "contacts=%s contacts_why=%s", name, pbap->why);
	} else {
		written = snprintf(text, size, "contacts=%s", name);
	}

	/* The text within its room. */
	if (written < 0 || (size_t)written >= size)
		return ENOSPC;

	/* Succeeded: the text. */
	return 0;
}

/* Gives the daemon's monotonic milliseconds. */
static uint64_t
pbap_now(
	const struct btd_pbap *pbap)
{
	uint64_t now;

	/* The daemon's clock. */
	now = pbap->hooks.clock(pbap->hooks.context);

	/* The milliseconds. */
	return now;
}

/* Writes a line of the daemon's log (never a name, a number or a card). */
static void
pbap_log(
	struct btd_pbap *pbap,
	const char *format,
	...)
{
	char line[160];
	va_list arguments;

	/* No log. */
	if (pbap->hooks.log == NULL)
		return;

	/* The line. */
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);

	/* Succeeded: logged. */
	pbap->hooks.log(pbap->hooks.context, line);
}

/* Tells the daemon that what PHONE SHOW tells of PBAP changed. */
static void
pbap_changed(
	struct btd_pbap *pbap)
{
	/* Succeeded: told. */
	if (pbap->hooks.changed != NULL)
		pbap->hooks.changed(pbap->hooks.context);
}

/* Starts setting PBAP up: the SDP query for the PSE (asked again shortly when another query runs). */
static void
pbap_start(
	struct btd_pbap *pbap)
{
	int error;

	/* A new attempt, nothing found yet. */
	pbap->state = BTD_PBAP_SDP;
	pbap->why = NULL;
	pbap->sdp_again_at = 0U;
	pbap->open_again_at = 0U;
	pbap->pse_channel = 0U;

	/* The query. */
	error = pbap->hooks.sdp_query(pbap->hooks.context, BTD_SDP_UUID_PSE);
	if (error == EBUSY) {
		pbap->sdp_again_at = pbap_now(pbap) + BTD_PBAP_BUSY_MS;
		pbap_changed(pbap);
		return;
	}

	/* A link that cannot query. */
	if (error != 0) {
		pbap_fail(pbap, "sdp", PBAP_RETRY_STEP);
		return;
	}

	/* Succeeded: the answer comes to btd_pbap_sdp_done. */
	pbap_changed(pbap);
}

/*
 * Asks for the DLC to the PSE's channel (after the SDP query, or from
 * idle): a channel whose earlier DLC is still closing is asked again
 * shortly.
 */
static void
pbap_open(
	struct btd_pbap *pbap)
{
	unsigned before;
	int error;

	/* Opening: this attempt has not been ready yet. */
	before = pbap->state;
	pbap->state = BTD_PBAP_OPENING;
	pbap->reached_ready = 0;
	pbap->open_again_at = 0U;

	/* The DLC. */
	error = pbap->hooks.dlc_open(pbap->hooks.context, pbap->pse_channel);
	if (error == EBUSY) {
		pbap->open_again_at = pbap_now(pbap) + BTD_PBAP_BUSY_MS;
		return;
	}

	/* A link that cannot open it. */
	if (error != 0) {
		pbap_fail(pbap, "rfcomm", PBAP_RETRY_STEP);
		return;
	}

	/* Succeeded: asked for, told when it was not shown as connecting before (idle is shown as ready). */
	if (before != BTD_PBAP_SDP)
		pbap_changed(pbap);
}

/*
 * Fails PBAP: its DLC closes, every page is answered as lost, and it is
 * set up again after a wait that grows (PBAP_RETRY_STEP), after ten
 * minutes (PBAP_RETRY_LONG: the phone's user refused, or did not answer),
 * or not on this link (PBAP_RETRY_NEVER).
 */
static void
pbap_fail(
	struct btd_pbap *pbap,
	const char *why,
	unsigned retry)
{
	uint64_t wait;
	unsigned step;

	/* Logged. */
	pbap_log(pbap, "pbap: failed (%s)", why);

	/* The DLC, the pages. */
	pbap_close_all(pbap);
	pbap_end_pages(pbap, "lost");

	/* The wait: 30 s doubling to 600 s, or 600 s, or none. */
	pbap->state = BTD_PBAP_FAILED;
	pbap->why = why;
	pbap->retry_at = 0U;
	pbap->sdp_again_at = 0U;
	pbap->open_again_at = 0U;
	if (retry == PBAP_RETRY_STEP) {
		wait = BTD_PBAP_RETRY_FIRST_MS;
		for (step = 0U; step < pbap->retry_step && wait < BTD_PBAP_RETRY_LAST_MS; step++)
			wait *= 2U;
		if (wait > BTD_PBAP_RETRY_LAST_MS)
			wait = BTD_PBAP_RETRY_LAST_MS;
		pbap->retry_step++;
		pbap->retry_at = pbap_now(pbap) + wait;
	} else if (retry == PBAP_RETRY_LONG) {
		pbap->retry_at = pbap_now(pbap) + BTD_PBAP_RETRY_LAST_MS;
	}

	/* Succeeded: failed, and told. */
	pbap_changed(pbap);
}

/*
 * Fails PBAP for its DLC closed before the connection was ready, by the
 * reason (section 5.1): the phone's DM or DISC is its user's refusal and
 * RFCOMM giving up an unanswered prompt, both waiting ten minutes (the
 * prompt is not shown again sooner); anything else the usual wait.
 */
static void
pbap_closed_unready(
	struct btd_pbap *pbap,
	int reason)
{
	/* The phone's refusal. */
	if (reason == BTD_RFCOMM_CLOSED_REFUSED || reason == BTD_RFCOMM_CLOSED_REMOTE) {
		pbap_fail(pbap, "permission", PBAP_RETRY_LONG);
		return;
	}

	/* No answer from the phone. */
	if (reason == BTD_RFCOMM_CLOSED_TIMEOUT) {
		pbap_fail(pbap, "timeout", PBAP_RETRY_LONG);
		return;
	}

	/* Succeeded: failed otherwise, tried again soon. */
	pbap_fail(pbap, "closed", PBAP_RETRY_STEP);
}

/* Closes PBAP's DLC (at the next tick) and forgets its OBEX connection and the operation running. */
static void
pbap_close_all(
	struct btd_pbap *pbap)
{
	/* The DLC, closed at the next tick (not from inside RFCOMM's events). */
	if (pbap->dlci != 0U) {
		pbap_close_later(pbap, pbap->dlci);
		pbap->dlci = 0U;
	}

	/* Nothing running, nothing noted. */
	memset(&pbap->obex, 0, sizeof(pbap->obex));
	pbap->running = 0;
	pbap->finished = 0;
	pbap->body_length = 0U;
}

/* Notes a DLC to close at the next tick. */
static void
pbap_close_later(
	struct btd_pbap *pbap,
	unsigned dlci)
{
	/* No room: closed by the session's end instead. */
	if (pbap->closing_count >= PBAP_CLOSING_MAX) {
		pbap_log(pbap, "pbap: a DLC left open");
		return;
	}

	/* Succeeded: noted. */
	pbap->closing[pbap->closing_count] = dlci;
	pbap->closing_count++;
}

/* Answers every page as ERROR word (its client told unless it went), nothing running. */
static void
pbap_end_pages(
	struct btd_pbap *pbap,
	const char *word)
{
	unsigned index;

	/* Nothing runs any more. */
	pbap->running = 0;

	/* Each page. */
	for (index = 0U; index < BTD_PBAP_PAGES_MAX; index++) {
		/* Ended and freed. */
		if (pbap->pages[index].used)
			pbap_page_end(pbap, index, word);
	}
}

/* Turns PBAP off: its DLC closes and every page is answered as lost. */
static void
pbap_shutdown(
	struct btd_pbap *pbap)
{
	int was_off;

	/* What was. */
	was_off = 0;
	if (pbap->state == BTD_PBAP_OFF)
		was_off = 1;

	/* The DLC, the pages. */
	pbap_close_all(pbap);
	pbap_end_pages(pbap, "lost");

	/* Off, the waits forgotten. */
	pbap->state = BTD_PBAP_OFF;
	pbap->why = NULL;
	pbap->retry_at = 0U;
	pbap->retry_step = 0U;
	pbap->sdp_again_at = 0U;
	pbap->open_again_at = 0U;

	/* Succeeded: told when it changed. */
	if (!was_off)
		pbap_changed(pbap);
}

/*
 * Does what OBEX's events noted, outside OBEX's input: fails PBAP for a
 * broken connection, ends Connect or the operation that finished, then
 * goes on with the pages.
 */
static void
pbap_settle(
	struct btd_pbap *pbap)
{
	const char *why;
	int round;

	/* Until nothing more is noted (each round ends something, and the pages are few). */
	for (round = 0; round < 64; round++) {
		/* The connection broken (an answer too late, a malformed packet). */
		if (pbap->dlci != 0U && pbap->obex.state == BTD_OBEX_BROKEN) {
			why = "obex";
			if (pbap->finished && pbap->finished_error == ETIMEDOUT)
				why = "timeout";
			pbap->finished = 0;
			if (pbap->state == BTD_PBAP_CONNECTING) {
				pbap_fail(pbap, why, PBAP_RETRY_LONG);
			} else {
				pbap_fail(pbap, why, PBAP_RETRY_STEP);
			}

			/* Looked at again. */
			continue;
		}

		/* Connect or an operation that finished. */
		if (pbap->finished) {
			pbap->finished = 0;
			if (pbap->state == BTD_PBAP_CONNECTING) {
				pbap_connect_finished(pbap);
			} else if (pbap->running) {
				pbap_op_finished(pbap);
			}

			/* Looked at again. */
			continue;
		}

		/* The pages go on. */
		pbap_next(pbap);
		if (!pbap->finished)
			break;
	}
}

/* Ends OBEX's Connect: ready, or failed by its answer. */
static void
pbap_connect_finished(
	struct btd_pbap *pbap)
{
	int error;
	uint8_t code;

	/* The answer. */
	error = pbap->finished_error;
	code = pbap->finished_code;

	/* Refused by the phone's user. */
	if (code == BTD_OBEX_UNAUTHORIZED || code == BTD_OBEX_FORBIDDEN) {
		pbap_fail(pbap, "permission", PBAP_RETRY_LONG);
		return;
	}

	/* No answer in time: the prompt not answered (not shown again sooner than ten minutes). */
	if (error == ETIMEDOUT) {
		pbap_fail(pbap, "timeout", PBAP_RETRY_LONG);
		return;
	}

	/* Refused otherwise. */
	if (error != 0) {
		pbap_fail(pbap, "refused", PBAP_RETRY_STEP);
		return;
	}

	/* Connected: the usual wait for each answer. */
	btd_obex_set_timeout(&pbap->obex, BTD_OBEX_TIMEOUT_MS);
	pbap->state = BTD_PBAP_READY;
	pbap->reached_ready = 1;
	pbap->retry_step = 0U;
	pbap->why = NULL;
	pbap_log(pbap, "pbap: ready");

	/* Succeeded: the phone link's waits start over, and the state is told. */
	if (pbap->hooks.up != NULL)
		pbap->hooks.up(pbap->hooks.context);
	pbap_changed(pbap);
}

/*
 * Goes on with the oldest page: writes its cards to its client, writes
 * its end, or starts its next operation (opening an idle connection
 * first).  The other page waits for it.
 */
static void
pbap_next(
	struct btd_pbap *pbap)
{
	struct btd_pbap_page *page;
	unsigned index;
	int found;
	int round;

	/* At most a few steps of each page in one call. */
	for (round = 0; round < 8; round++) {
		/* The oldest page; none, or an operation running. */
		found = pbap_head(pbap, &index);
		if (!found || pbap->running)
			return;
		page = &pbap->pages[index];

		/* Its cards to its client (it may wait for room). */
		if (page->step == PBAP_STEP_EMIT) {
			pbap_page_continue(pbap, index);
			if (pbap->pages[index].used && pbap->pages[index].step == PBAP_STEP_EMIT)
				return;
			continue;
		}

		/* Its end. */
		if (page->step == PBAP_STEP_FINISH) {
			pbap_page_finish(pbap, index);
			continue;
		}

		/* An operation needs the connection: an idle one is opened again. */
		if (pbap->state == BTD_PBAP_IDLE) {
			pbap_open(pbap);
			return;
		}

		/* A connection not ready yet: the page waits for it. */
		if (pbap->state != BTD_PBAP_READY)
			return;

		/* Succeeded: the operation of its step. */
		if (page->step == PBAP_STEP_PULL) {
			pbap_run(pbap, index, PBAP_OP_PULL);
		} else {
			pbap_run(pbap, index, PBAP_OP_SIZE);
		}

		/* One operation at a time. */
		return;
	}
}

/* Finds the oldest page.  Returns 1 with its index, or 0 when none is used. */
static int
pbap_head(
	const struct btd_pbap *pbap,
	unsigned *index)
{
	unsigned at;
	int found;

	/* Each page in use, the smallest serial. */
	found = 0;
	for (at = 0U; at < BTD_PBAP_PAGES_MAX; at++) {
		/* Free. */
		if (!pbap->pages[at].used)
			continue;

		/* Older than the one found. */
		if (!found || pbap->pages[at].serial < pbap->pages[*index].serial) {
			*index = at;
			found = 1;
		}
	}

	/* Whether one was found. */
	return found;
}

/* Starts an operation for a page; one that cannot go ends at once with its error. */
static void
pbap_run(
	struct btd_pbap *pbap,
	unsigned index,
	unsigned op)
{
	int error;

	/* Running, nothing heard of it yet. */
	pbap->running = 1;
	pbap->op = op;
	pbap->op_page = index;
	pbap->have_size = 0;
	pbap->size = 0U;
	pbap->body_length = 0U;

	/* Sent; a request that cannot go ends at once. */
	error = pbap_get(pbap, &pbap->pages[index], op);
	if (error != 0) {
		pbap->finished = 1;
		pbap->finished_error = error;
		pbap->finished_code = 0U;
	}
}

/*
 * Sends PullPhoneBook (PBAP section 5.1): Name the object, Type
 * x-bt/phonebook, and the application parameters: MaxListCount 0 for an
 * object's size alone; else the fields wanted, vCard 3.0, the cards asked
 * for and the offset.  Returns 0, or OBEX's error.
 */
static int
pbap_get(
	struct btd_pbap *pbap,
	const struct btd_pbap_page *page,
	unsigned op)
{
	static const char type[] = "x-bt/phonebook";
	struct btd_obex_writer writer;
	uint8_t headers[PBAP_HEADERS_MAX];
	uint8_t parameters[PBAP_PARAMETERS_MAX];
	uint8_t format;
	size_t used;
	int error;

	/* The size alone, or the fields, the format, the count and the offset. */
	used = 0U;
	if (op == PBAP_OP_SIZE) {
		pbap_parameter16(parameters, &used, PBAP_TAG_MAX_LIST_COUNT, 0U);
	} else {
		if (page->object == BTD_PBAP_OBJECT_PB) {
			pbap_parameter(parameters, &used, PBAP_TAG_PROPERTY_SELECTOR, pbap_contact_fields, sizeof(pbap_contact_fields));
		} else {
			pbap_parameter(parameters, &used, PBAP_TAG_PROPERTY_SELECTOR, pbap_call_fields, sizeof(pbap_call_fields));
		}

		/* vCard 3.0, the cards asked for, the offset. */
		format = PBAP_FORMAT_30;
		pbap_parameter(parameters, &used, PBAP_TAG_FORMAT, &format, 1U);
		pbap_parameter16(parameters, &used, PBAP_TAG_MAX_LIST_COUNT, page->ask);
		pbap_parameter16(parameters, &used, PBAP_TAG_START_OFFSET, page->offset);
	}

	/* The headers: Name, Type (its NUL included), the parameters. */
	btd_obex_writer_init(&writer, headers, sizeof(headers));
	btd_obex_put_text(&writer, BTD_OBEX_NAME, pbap_object_names[page->object]);
	btd_obex_put_bytes(&writer, BTD_OBEX_TYPE, (const uint8_t *)type, sizeof(type));
	btd_obex_put_bytes(&writer, BTD_OBEX_APPLICATION, parameters, used);
	if (writer.overflow)
		return EMSGSIZE;

	/* Sent. */
	error = btd_obex_get(&pbap->obex, headers, writer.used, BTD_PBAP_BODY_MAX, pbap_now(pbap));
	if (error != 0)
		return error;

	/* Succeeded: under way. */
	return 0;
}

/* Ends the operation that finished, for its page (a page whose client went is freed). */
static void
pbap_op_finished(
	struct btd_pbap *pbap)
{
	struct btd_pbap_page *page;
	unsigned index;
	int error;

	/* Nothing runs now. */
	pbap->running = 0;
	index = pbap->op_page;
	page = &pbap->pages[index];
	error = pbap->finished_error;

	/* A page gone, or whose client went. */
	if (!page->used)
		return;
	if (page->cancelled) {
		pbap_page_free(pbap, index);
		return;
	}

	/* Succeeded: by the operation. */
	if (pbap->op == PBAP_OP_SIZE) {
		pbap_size_finished(pbap, page, error);
	} else {
		pbap_pull_finished(pbap, page, error);
	}

	/* A page the answer ended with ERROR. */
	if (page->used && page->step == 0U)
		pbap_page_end(pbap, index, "refused");
}

/*
 * Ends an object's size: at the pass's start it is kept (unknown when the
 * phone did not give it); a history object the phone has no size for is
 * taken as empty; at the pass's end a size other than the start's, or
 * one not known, says the phone book changed.
 */
static void
pbap_size_finished(
	struct btd_pbap *pbap,
	struct btd_pbap_page *page,
	int error)
{
	/* The pass's end: the page ends after it. */
	if (page->step == PBAP_STEP_END_SIZE) {
		if (error != 0 ||
		    !pbap->have_size ||
		    page->start_size == BTD_PBAP_SIZE_UNKNOWN ||
		    pbap->size != page->start_size)
			page->capped |= BTD_PBAP_CAPPED_CHANGED;
		page->step = PBAP_STEP_FINISH;
		return;
	}

	/* The phone book without a size: the page is refused (step 0 ends it). */
	if (error != 0 && page->object == BTD_PBAP_OBJECT_PB) {
		page->step = 0U;
		return;
	}

	/* A history object without a size: taken as empty, the next object follows. */
	if (error != 0) {
		pbap_next_object(page);
		page->step = PBAP_STEP_FINISH;
		return;
	}

	/* Succeeded: the size kept (unknown when not given), the cards next. */
	page->start_size = BTD_PBAP_SIZE_UNKNOWN;
	if (pbap->have_size)
		page->start_size = pbap->size;
	page->step = PBAP_STEP_PULL;
}

/*
 * Ends a Get of cards: a body too large is asked again for half the cards
 * (a single card too large is left out); a Get at or past the object's
 * size that fails is the object's end (section 5.3: some phones answer
 * it with an error); another failure refuses the page; else the cards
 * are written to the client.
 */
static void
pbap_pull_finished(
	struct btd_pbap *pbap,
	struct btd_pbap_page *page,
	int error)
{
	/* Too large: half the cards again, or the one card left out. */
	if (error == EMSGSIZE) {
		if (page->ask > 1U) {
			page->ask /= 2U;
			page->step = PBAP_STEP_PULL;
			return;
		}

		/* One card too large: left out, the next after it. */
		page->skipped++;
		pbap->left_out++;
		pbap_after_body(page, 1U);
		return;
	}

	/* Failed past the size: the object's end. */
	if (error != 0 &&
	    page->start_size != BTD_PBAP_SIZE_UNKNOWN &&
	    page->offset >= page->start_size) {
		pbap_after_body(page, 0U);
		return;
	}

	/* Failed otherwise: the page is refused (step 0 ends it). */
	if (error != 0) {
		page->step = 0U;
		return;
	}

	/* Succeeded: the body's cards counted, written from its start. */
	page->body_cards = btd_vcard_count(pbap->body, pbap->body_length);
	page->body_at = 0U;
	page->card_index = page->offset;
	page->step = PBAP_STEP_EMIT;
}

/*
 * Writes the body's cards to the page's client, one at a time while its
 * queue has room (it waits otherwise); then decides where the next page
 * starts.  A card that does not end makes the page's count untrusted.
 */
static void
pbap_page_continue(
	struct btd_pbap *pbap,
	unsigned index)
{
	struct btd_pbap_page *page;
	size_t at;
	size_t start;
	size_t length;
	long room;
	int error;

	/* Each card left. */
	page = &pbap->pages[index];
	for (;;) {
		/* The next card; none left, or one the body ends inside. */
		at = page->body_at;
		error = btd_vcard_next(pbap->body, pbap->body_length, &at, &start, &length);
		if (error == ENOENT)
			break;
		if (error != 0) {
			page->capped |= BTD_PBAP_CAPPED_UNTRUSTED;
			break;
		}

		/* The client's room: gone, the page goes; short, the page waits. */
		room = pbap->hooks.room(pbap->hooks.context, page->token);
		if (room < 0) {
			pbap_page_free(pbap, index);
			return;
		}

		/* Short of room: the page waits, from the first time it found its client short. */
		if (room < BTD_PBAP_ROOM_MIN) {
			if (!page->waiting)
				page->waiting_since = pbap_now(pbap);
			page->waiting = 1;
			return;
		}

		/* The card written (or left out), the next one after it. */
		page->waiting = 0;
		pbap->cards++;
		if (page->what == BTD_PBAP_WHAT_CONTACTS) {
			pbap_emit_contact(pbap, page, pbap->body + start, length);
		} else {
			pbap_emit_call(pbap, page, pbap->body + start, length);
		}

		/* The next card after it. */
		page->body_at = at;
		page->card_index++;
	}

	/* Succeeded: the body done, where the next page starts. */
	page->waiting = 0;
	pbap_after_body(page, page->body_cards);
}

/*
 * Writes one card of the phone book to the page's client as PHONE
 * CONTACT and its reduced vCard.  The owner's card (index 0 of the phone
 * book, PBAP section 3.1.5.2) is not written nor counted; a card that
 * cannot be read is left out and counted as skipped.
 */
static void
pbap_emit_contact(
	struct btd_pbap *pbap,
	struct btd_pbap_page *page,
	const uint8_t *card,
	size_t length)
{
	struct btd_vcard_contact contact;
	char reduced[BTD_VCARD_REDUCED_MAX];
	char line[PBAP_LINE_MAX];
	const char *peer;
	size_t used;
	int error;

	/* The owner's card. */
	if (page->object == BTD_PBAP_OBJECT_PB && page->card_index == 0U)
		return;

	/* The card read. */
	error = btd_vcard_contact_read(card, length, &contact);
	if (error != 0) {
		page->skipped++;
		pbap->left_out++;
		return;
	}

	/* Its reduced vCard. */
	error = btd_vcard_reduce(&contact, reduced, sizeof(reduced), &used);
	if (error != 0) {
		page->skipped++;
		pbap->left_out++;
		return;
	}

	/* Its line, with its first number. */
	peer = "";
	if (contact.tel_count != 0U)
		peer = contact.tels[0].number;
	error = btd_phoneio_contact_line(line, sizeof(line), contact.key, (unsigned)contact.tel_count, used, peer, contact.name);
	if (error != 0) {
		page->skipped++;
		pbap->left_out++;
		return;
	}

	/* Succeeded: written to the client. */
	pbap->hooks.answer(pbap->hooks.context, page->token, line, (const uint8_t *)reduced, used);
	page->given++;
}

/*
 * Writes one card of the call history to the page's client as PHONE
 * CALL-LOG: its time read by its own zone or zedBSD's; a call before the
 * page's time is not written (nor counted); a call whose time cannot be
 * read is written with the time it was read at (zone none, partial); a
 * card that cannot be read is left out and counted as skipped.
 */
static void
pbap_emit_call(
	struct btd_pbap *pbap,
	struct btd_pbap_page *page,
	const uint8_t *card,
	size_t length)
{
	struct btd_vcard_call call;
	char line[PBAP_LINE_MAX];
	const char *kind;
	const char *zone_word;
	int64_t wall;
	int64_t seconds;
	int32_t local;
	int folder_kind;
	int partial;
	int zone;
	int error;

	/* The folder's kind of call. */
	folder_kind = BTD_VCARD_CALL_RECEIVED;
	if (page->object == BTD_PBAP_OBJECT_OCH) {
		folder_kind = BTD_VCARD_CALL_DIALED;
	} else if (page->object == BTD_PBAP_OBJECT_MCH) {
		folder_kind = BTD_VCARD_CALL_MISSED;
	}

	/* The card read. */
	error = btd_vcard_call_read(card, length, folder_kind, &call);
	if (error != 0) {
		page->skipped++;
		pbap->left_out++;
		return;
	}

	/* Its time: its own zone, zedBSD's, or none (the time now, partial). */
	wall = pbap->hooks.wall(pbap->hooks.context);
	local = pbap->hooks.local_offset(pbap->hooks.context, wall);
	(void)btd_vcard_call_time(&call, local, &seconds, &zone);
	partial = 0;
	zone_word = "phone";
	if (zone == BTD_VCARD_ZONE_LOCAL) {
		zone_word = "local";
	} else if (zone == BTD_VCARD_ZONE_NONE) {
		zone_word = "none";
		seconds = wall;
		partial = 1;
	}

	/* A call before the page's time is not written. */
	if (zone != BTD_VCARD_ZONE_NONE && seconds < page->since)
		return;

	/* Its kind as a word. */
	kind = "received";
	if (call.kind == BTD_VCARD_CALL_DIALED) {
		kind = "dialed";
	} else if (call.kind == BTD_VCARD_CALL_MISSED) {
		kind = "missed";
	}

	/* Its line. */
	error = btd_phoneio_call_line(line, sizeof(line), call.key, kind, seconds, zone_word, partial, call.datetime, call.number, call.name);
	if (error != 0) {
		page->skipped++;
		pbap->left_out++;
		return;
	}

	/* Succeeded: written to the client. */
	pbap->hooks.answer(pbap->hooks.context, page->token, line, NULL, 0U);
	page->given++;
}

/*
 * Decides where the next page starts once a Get's received cards are
 * done (section 5.3): fewer cards than asked for, or the pass's limit
 * (the owner's card and 5000 contacts, or 500 calls of an object), end
 * the object.  The phone book's end asks its size again; the history's
 * goes on to the next object, the last ending the pass.
 */
static void
pbap_after_body(
	struct btd_pbap_page *page,
	unsigned received)
{
	unsigned limit;
	int ended;

	/* The next offset in this object. */
	page->next_object = page->object;
	page->next_offset = page->offset + received;
	page->next_size = page->start_size;
	page->more = 1;

	/* The object's limit. */
	limit = BTD_PBAP_CALLS_MAX;
	if (page->object == BTD_PBAP_OBJECT_PB)
		limit = BTD_PBAP_CONTACTS_MAX + 1U;

	/*
	 * The object's end: fewer cards than asked for (unless a card did not
	 * end, which makes the count untrusted: the next page asks again), or
	 * its limit.
	 */
	ended = 0;
	if (received < page->ask && (page->capped & BTD_PBAP_CAPPED_UNTRUSTED) == 0U) {
		ended = 1;
	} else if (page->next_offset >= limit) {
		ended = 1;
		page->capped |= BTD_PBAP_CAPPED_LIMIT;
	}

	/* Not ended: the next page goes on in this object. */
	if (!ended) {
		page->step = PBAP_STEP_FINISH;
		return;
	}

	/* The phone book's end: its size asked again, the pass over. */
	if (page->object == BTD_PBAP_OBJECT_PB) {
		page->more = 0;
		page->step = PBAP_STEP_END_SIZE;
		return;
	}

	/* Succeeded: the history's next object, or the pass over. */
	pbap_next_object(page);
	page->step = PBAP_STEP_FINISH;
}

/* Sets the next page to the history's next object from its start (the last object's end ends the pass). */
static void
pbap_next_object(
	struct btd_pbap_page *page)
{
	/* The next object, its size not known. */
	page->next_object = page->object + 1U;
	page->next_offset = 0U;
	page->next_size = BTD_PBAP_SIZE_UNKNOWN;
	page->more = 1;

	/* Past the last: the pass is over. */
	if (page->next_object >= BTD_PBAP_OBJECTS)
		page->more = 0;
}

/* Writes a page's end (PHONE PAGE-END with the next cursor, then DONE) and frees it. */
static void
pbap_page_finish(
	struct btd_pbap *pbap,
	unsigned index)
{
	struct btd_pbap_page *page;
	char line[160];

	/* The end. */
	page = &pbap->pages[index];
	(void)snprintf(line,
		       sizeof(line),
		       "PHONE PAGE-END cursor=%08lx.%u.%u.%lx more=%d count=%u skipped=%u capped=%u",
		       (unsigned long)pbap->generation,
		       page->next_object,
		       page->next_offset,
		       (unsigned long)page->next_size,
		       page->more,
		       page->given,
		       page->skipped,
		       page->capped);
	pbap->hooks.answer(pbap->hooks.context, page->token, line, NULL, 0U);
	pbap->hooks.answer(pbap->hooks.context, page->token, "DONE", NULL, 0U);
	pbap_log(pbap, "pbap: page of object %u at %u: %u cards, %u skipped", page->object, page->offset, page->given, page->skipped);

	/* Succeeded: the page done. */
	pbap->pages_done++;
	pbap_page_free(pbap, index);
}

/* Ends a page with ERROR word (its client told unless it went). */
static void
pbap_page_end(
	struct btd_pbap *pbap,
	unsigned index,
	const char *word)
{
	char line[48];

	/* The client told. */
	if (!pbap->pages[index].cancelled) {
		(void)snprintf(line, sizeof(line), "ERROR %s", word);
		pbap->hooks.answer(pbap->hooks.context, pbap->pages[index].token, line, NULL, 0U);
		pbap->hooks.answer(pbap->hooks.context, pbap->pages[index].token, "DONE", NULL, 0U);
	}

	/* Succeeded: the page done. */
	pbap_page_free(pbap, index);
}

/* Frees a page (the operation running for it, if any, ends quietly). */
static void
pbap_page_free(
	struct btd_pbap *pbap,
	unsigned index)
{
	/* The operation running for it answers nobody. */
	if (pbap->running && pbap->op_page == index)
		pbap->pages[index].cancelled = 1;

	/* Succeeded: free, unless its operation still runs (it is freed when that ends). */
	if (pbap->running && pbap->op_page == index)
		return;
	memset(&pbap->pages[index], 0, sizeof(pbap->pages[index]));
}

/*
 * Reads a cursor: the generation in eight hexadecimal digits, the object
 * and the offset in decimal, the object's size at the pass's start in
 * hexadecimal (ffffffff: not known), a '.' between each.  Returns 0, or
 * EINVAL for one malformed or out of range.
 */
static int
pbap_parse_cursor(
	const char *cursor,
	uint32_t *generation,
	unsigned *object,
	unsigned *offset,
	unsigned *size)
{
	const char *text;
	size_t length;
	unsigned value;
	int error;

	/* The generation: eight hexadecimal digits and a '.'. */
	text = cursor;
	length = strlen(cursor);
	if (length < 9U || cursor[8] != '.')
		return EINVAL;
	error = pbap_parse_number(&text, 1, &value);
	if (error != 0 || text != cursor + 8)
		return EINVAL;
	*generation = value;

	/* The object, after its '.'. */
	text++;
	error = pbap_parse_number(&text, 0, object);
	if (error != 0 || *text != '.')
		return EINVAL;
	if (*object > BTD_PBAP_OBJECTS)
		return EINVAL;

	/* The offset, after its '.'. */
	text++;
	error = pbap_parse_number(&text, 0, offset);
	if (error != 0 || *text != '.')
		return EINVAL;
	if (*offset > BTD_PBAP_CONTACTS_MAX + 1U)
		return EINVAL;

	/* The size, after its '.', ending the cursor. */
	text++;
	error = pbap_parse_number(&text, 1, size);
	if (error != 0 || *text != '\0')
		return EINVAL;

	/* Succeeded: the cursor's place. */
	return 0;
}

/*
 * Reads a number of 1 to 8 hexadecimal digits (hex) or 1 to 9 decimal
 * digits at *text, and moves *text past it.  Returns 0, or EINVAL for no
 * digit or too many.
 */
static int
pbap_parse_number(
	const char **text,
	int hex,
	unsigned *value)
{
	const char *at;
	unsigned digits;
	unsigned most;
	unsigned digit;
	unsigned long number;
	char letter;

	/* The most digits a number of its base has here. */
	most = 9U;
	if (hex)
		most = 8U;

	/* Each digit. */
	at = *text;
	digits = 0U;
	number = 0UL;
	for (;;) {
		/* The digit's value, or the number's end. */
		letter = *at;
		if (letter >= '0' && letter <= '9') {
			digit = (unsigned)(letter - '0');
		} else if (hex && letter >= 'a' && letter <= 'f') {
			digit = (unsigned)(letter - 'a' + 10);
		} else {
			break;
		}

		/* Too many digits for the number. */
		digits++;
		if (digits > most)
			return EINVAL;

		/* Taken. */
		if (hex) {
			number = number * 16UL + digit;
		} else {
			number = number * 10UL + digit;
		}

		/* The next letter. */
		at++;
	}

	/* No digit. */
	if (digits == 0U)
		return EINVAL;

	/* Succeeded: the number, and the text after it. */
	*value = (unsigned)number;
	*text = at;
	return 0;
}

/* Adds an application parameter: its tag, its length, its value (the room is the caller's, PBAP_PARAMETERS_MAX). */
static void
pbap_parameter(
	uint8_t *bytes,
	size_t *used,
	uint8_t tag,
	const uint8_t *value,
	size_t length)
{
	/* The tag and the length. */
	bytes[*used] = tag;
	bytes[*used + 1U] = (uint8_t)length;
	*used += 2U;

	/* Succeeded: the value. */
	memcpy(bytes + *used, value, length);
	*used += length;
}

/* Adds an application parameter of two bytes, the high byte first. */
static void
pbap_parameter16(
	uint8_t *bytes,
	size_t *used,
	uint8_t tag,
	unsigned value)
{
	uint8_t pair[2];

	/* The value's two bytes. */
	pair[0] = (uint8_t)((value >> 8) & 0xffU);
	pair[1] = (uint8_t)(value & 0xffU);

	/* Succeeded: added. */
	pbap_parameter(bytes, used, tag, pair, sizeof(pair));
}

/* Writes OBEX's bytes on the PSE's DLC (OBEX's write hook). */
static int
pbap_write(
	void *context,
	const uint8_t *data,
	size_t length,
	size_t *written)
{
	struct btd_pbap *pbap;
	int error;

	/* No DLC. */
	pbap = context;
	*written = 0U;
	if (pbap->dlci == 0U)
		return ENOTCONN;

	/* As far as the DLC takes them. */
	error = pbap->hooks.dlc_write(pbap->hooks.context, pbap->dlci, data, length, written);
	if (error != 0)
		return error;

	/* Succeeded: what went is in written. */
	return 0;
}

/* Notes the end of a request (OBEX's done hook): pbap_settle handles it. */
static void
pbap_done(
	void *context,
	unsigned operation,
	int error,
	uint8_t code,
	const uint8_t *headers,
	size_t length)
{
	struct btd_pbap *pbap;

	UNUSED_PARAMETER(operation);
	UNUSED_PARAMETER(headers);
	UNUSED_PARAMETER(length);

	/* Succeeded: noted. */
	pbap = context;
	pbap->finished = 1;
	pbap->finished_error = error;
	pbap->finished_code = code;
}

/* Notes the PhonebookSize an answer packet gives (OBEX's response hook; the first packet of a Get, section 6.2.2). */
static void
pbap_response(
	void *context,
	unsigned operation,
	uint8_t code,
	const uint8_t *headers,
	size_t length)
{
	struct btd_obex_header header;
	struct btd_pbap *pbap;
	const uint8_t *value;
	size_t offset;
	size_t at;
	size_t size;
	uint8_t tag;
	int read;

	UNUSED_PARAMETER(operation);
	UNUSED_PARAMETER(code);

	/* Each header. */
	pbap = context;
	offset = 0U;
	for (;;) {
		/* The next header; only the application parameters count. */
		read = btd_obex_header_next(headers, length, &offset, &header);
		if (read <= 0)
			break;
		if (header.id != BTD_OBEX_APPLICATION)
			continue;

		/* Each parameter: its tag, its length, its value. */
		at = 0U;
		while (at + 2U <= header.length) {
			/* The parameter, within the header. */
			tag = header.data[at];
			size = header.data[at + 1U];
			value = header.data + at + 2U;
			if (at + 2U + size > header.length)
				break;
			at += 2U + size;

			/* The phone book's size. */
			if (tag == PBAP_TAG_PHONEBOOK_SIZE && size == 2U) {
				pbap->have_size = 1;
				pbap->size = ((unsigned)value[0] << 8) | value[1];
			}
		}
	}
}

/* Gathers a Get's body (OBEX's body hook); past the room it stops the Get. */
static int
pbap_body(
	void *context,
	const uint8_t *data,
	size_t length)
{
	struct btd_pbap *pbap;

	/* Past the room. */
	pbap = context;
	if (length > sizeof(pbap->body) - pbap->body_length)
		return 1;

	/* Succeeded: gathered. */
	memcpy(pbap->body + pbap->body_length, data, length);
	pbap->body_length += length;
	return 0;
}

/* Gives the earlier of two deadlines, 0 standing for none. */
static uint64_t
pbap_earlier(
	uint64_t earliest,
	uint64_t deadline)
{
	/* No deadline. */
	if (deadline == 0U)
		return earliest;

	/* The earlier. */
	if (earliest == 0U || deadline < earliest)
		return deadline;
	return earliest;
}
