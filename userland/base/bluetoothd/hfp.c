/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Hands-Free Profile's HF (ws197-p006; hfp.h).  The facts are HFP
 * 1.8's (the SLC's order of section 4.2, the AT commands and results of
 * section 4.34, the features' bits of sections 4.34.1 and 5.3) and 3GPP
 * TS 27.007's for the shared commands (+CIND, +CLCC, +CHLD, +VTS).
 */

#include "userland/base/bluetoothd/hfp.h"
#include "userland/base/bluetoothd/phoneio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The steps of the SLC: none (another command), +BRSF, +CIND=?, +CIND?, +CMER, +CHLD=?, the commands after it, AT+CLCC. */
#define HFP_STEP_NONE		0U
#define HFP_STEP_BRSF		1U
#define HFP_STEP_CIND_TEST	2U
#define HFP_STEP_CIND_READ	3U
#define HFP_STEP_CMER		4U
#define HFP_STEP_CHLD		5U
#define HFP_STEP_AFTER		6U
#define HFP_STEP_CLCC		7U

/* The SDP record's SupportedFeatures attribute. */
#define HFP_ATTRIBUTE_FEATURES	0x0311U

/* What a failure waits: the next step, or nothing more on this link. */
#define HFP_RETRY_STEP		0
#define HFP_RETRY_NEVER		1

static uint64_t hfp_now(struct btd_hfp *hfp);
static void hfp_log(struct btd_hfp *hfp, const char *line);
static void hfp_start(struct btd_hfp *hfp);
static void hfp_open(struct btd_hfp *hfp);
static void hfp_take(struct btd_hfp *hfp, unsigned dlci);
static void hfp_fail(struct btd_hfp *hfp, const char *why, int never);
static void hfp_stop(struct btd_hfp *hfp);
static void hfp_close_later(struct btd_hfp *hfp, unsigned dlci);
static void hfp_forget_commands(struct btd_hfp *hfp, const char *answer);
static int hfp_queue(struct btd_hfp *hfp, const char *text, unsigned step, uint64_t token, unsigned timeout_ms);
static void hfp_pump(struct btd_hfp *hfp);
static void hfp_line(void *context, const char *line);
static void hfp_finished(struct btd_hfp *hfp, int ok, const char *error);
static void hfp_slc_next(struct btd_hfp *hfp, unsigned step, int ok);
static void hfp_slc_done(struct btd_hfp *hfp);
static void hfp_indicators(struct btd_hfp *hfp, const char *rest, unsigned step);
static void hfp_indicator(struct btd_hfp *hfp, const char *rest);
static void hfp_clip(struct btd_hfp *hfp, const char *rest, int waiting);
static void hfp_listed(struct btd_hfp *hfp, const char *rest);
static void hfp_list_end(struct btd_hfp *hfp);
static void hfp_infer(struct btd_hfp *hfp);
static struct btd_hfp_call *hfp_find(struct btd_hfp *hfp, unsigned state);
static struct btd_hfp_call *hfp_slot(struct btd_hfp *hfp);
static void hfp_set(struct btd_hfp *hfp, struct btd_hfp_call *call, unsigned state, int outgoing);
static void hfp_end_calls(struct btd_hfp *hfp);
static void hfp_tell(struct btd_hfp *hfp);
static void hfp_call_line(const struct btd_hfp_call *call, unsigned state, char *line, size_t size);
static void hfp_hf_line(const struct btd_hfp *hfp, char *line, size_t size);
static long hfp_value(const struct btd_hfp *hfp, int at);
static const char *hfp_call_word(unsigned state);
static int hfp_number_ok(const char *number);

/*
 * Starts the HF off, with its hooks.
 */
void
btd_hfp_init(
	struct btd_hfp *hfp,
	const struct btd_hfp_hooks *hooks)
{
	/* Nothing under way, no indicator known, no volume told. */
	memset(hfp, 0, sizeof(*hfp));
	hfp->hooks = *hooks;
	hfp->state = BTD_HFP_OFF;
	hfp->why = "";
	hfp->at_call = -1;
	hfp->at_callsetup = -1;
	hfp->at_callheld = -1;
	hfp->at_service = -1;
	hfp->at_signal = -1;
	hfp->at_roam = -1;
	hfp->at_battchg = -1;
	hfp->speaker = -1;
	btd_at_input_init(&hfp->input);
}

/*
 * Takes the phone link's readiness (the profile's ready hook): the HF
 * starts when the owner wants calls.
 */
void
btd_hfp_ready(
	void *context)
{
	struct btd_hfp *hfp;
	int wanted;

	/* The link is there for the HF, its waits from the start. */
	hfp = context;
	hfp->link_ready = 1;
	hfp->retry_step = 0U;

	/* Started when calls are wanted. */
	wanted = hfp->hooks.wanted(hfp->hooks.context);
	if (wanted && hfp->state == BTD_HFP_OFF)
		hfp_start(hfp);

	/* What changed is told. */
	hfp_tell(hfp);
}

/*
 * Takes the end of the phone link (the profile's ended hook): the calls
 * end, the owner's requests are answered as lost, and the HF is off until
 * the next link.
 */
void
btd_hfp_ended(
	void *context)
{
	struct btd_hfp *hfp;

	/* The link is gone, and so is its DLC. */
	hfp = context;
	hfp->link_ready = 0;
	hfp->dlci = 0U;
	hfp->closing_count = 0U;

	/* Off, what changed told. */
	hfp_stop(hfp);
	hfp_tell(hfp);
}

/*
 * Takes the end of the SDP query for the AG (the profile's sdp_done hook):
 * the first AG record with an RFCOMM channel is chosen and its DLC asked
 * for; a phone without an AG is not asked again on this link.
 */
void
btd_hfp_sdp_done(
	void *context,
	const struct btd_sdp *sdp,
	int error)
{
	struct btd_hfp *hfp;
	unsigned channel;
	unsigned count;
	unsigned nth;
	uint32_t value;
	int found;
	int has;

	/* Only the query the HF asked for. */
	hfp = context;
	if (hfp->state != BTD_HFP_SDP)
		return;

	/* A query that failed: tried again later. */
	if (error != 0) {
		hfp_fail(hfp, "sdp", HFP_RETRY_STEP);
		hfp_tell(hfp);
		return;
	}

	/* The first AG record with an RFCOMM channel. */
	found = 0;
	channel = 0U;
	count = btd_sdp_records(sdp, BTD_SDP_UUID_AG);
	for (nth = 0U; nth < count; nth++) {
		has = btd_sdp_rfcomm_channel(sdp, BTD_SDP_UUID_AG, nth, &channel);
		if (has == 0) {
			found = 1;
			break;
		}
	}

	/* No AG: not asked again on this link. */
	if (!found) {
		hfp_fail(hfp, "no-ag", HFP_RETRY_NEVER);
		hfp_tell(hfp);
		return;
	}

	/* Its channel and the features its record names (none when it names none). */
	hfp->ag_channel = channel;
	hfp->ag_sdp_features = 0U;
	has = btd_sdp_uint_attribute(sdp, BTD_SDP_UUID_AG, nth, HFP_ATTRIBUTE_FEATURES, &value);
	if (has == 0)
		hfp->ag_sdp_features = value;

	/* Its DLC asked for. */
	hfp_open(hfp);
	hfp_tell(hfp);
}

/*
 * Tells RFCOMM whether the HF takes the phone's DLC on a server channel
 * of bluetoothd's: its own HF channel, while calls are wanted on a ready
 * link and no SLC runs.  Returns 1 or 0.
 */
int
btd_hfp_accept(
	void *context,
	unsigned server_channel)
{
	struct btd_hfp *hfp;
	int wanted;

	/* The HF's channel only. */
	hfp = context;
	if (server_channel != BTD_HFP_HF_CHANNEL)
		return 0;

	/* Calls wanted on a ready link. */
	if (!hfp->link_ready)
		return 0;
	wanted = hfp->hooks.wanted(hfp->hooks.context);
	if (!wanted)
		return 0;

	/* One SLC at a time. */
	if (hfp->dlci != 0U)
		return 0;

	/* Taken. */
	return 1;
}

/*
 * Takes a DLC that opened (the profile's opened hook): the AG's DLC the HF
 * asked for, or the AG's own DLC to the HF channel, carries the SLC; any
 * other is closed.
 */
void
btd_hfp_opened(
	void *context,
	unsigned dlci,
	unsigned server_channel,
	int ours)
{
	struct btd_hfp *hfp;

	/* One SLC at a time: a second DLC is closed. */
	hfp = context;
	if (hfp->dlci != 0U) {
		hfp_close_later(hfp, dlci);
		return;
	}

	/* The DLC asked for, while it is awaited. */
	if (ours && hfp->state == BTD_HFP_OPENING && server_channel == hfp->ag_channel) {
		hfp_take(hfp, dlci);
		hfp_tell(hfp);
		return;
	}

	/* The AG's own DLC to the HF channel. */
	if (!ours && server_channel == BTD_HFP_HF_CHANNEL) {
		hfp_take(hfp, dlci);
		hfp_tell(hfp);
		return;
	}

	/* Another one (a late one of an earlier attempt). */
	hfp_close_later(hfp, dlci);
}

/* Takes the data of a DLC (the profile's data hook): the AG's lines. */
void
btd_hfp_data(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length)
{
	struct btd_hfp *hfp;

	/* The SLC's DLC only. */
	hfp = context;
	if (dlci == 0U || dlci != hfp->dlci)
		return;

	/* Each line read, then the next command written and what changed told. */
	btd_at_input(&hfp->input, data, length, hfp_line, hfp);
	hfp_pump(hfp);
	hfp_tell(hfp);
}

/* Takes a DLC that may be written again (the profile's writable hook): the command being written goes on. */
void
btd_hfp_writable(
	void *context,
	unsigned dlci)
{
	struct btd_hfp *hfp;

	/* The SLC's DLC only. */
	hfp = context;
	if (dlci == 0U || dlci != hfp->dlci)
		return;

	/* The rest written. */
	hfp_pump(hfp);
}

/*
 * Takes a DLC that closed (the profile's closed hook): the SLC's ends the
 * calls and fails the HF (tried again after the wait); the DLC asked for,
 * closed before it opened, fails it too.
 */
void
btd_hfp_closed(
	void *context,
	unsigned dlci,
	int reason)
{
	struct btd_hfp *hfp;

	/* The reason does not change what follows. */
	UNUSED_PARAMETER(reason);

	/* The SLC's DLC. */
	hfp = context;
	if (dlci != 0U && dlci == hfp->dlci) {
		hfp->dlci = 0U;
		hfp_fail(hfp, "closed", HFP_RETRY_STEP);
		hfp_tell(hfp);
		return;
	}

	/* The DLC asked for, closed before it opened (the phone's DM, RFCOMM giving up). */
	if (dlci != 0U &&
	    hfp->state == BTD_HFP_OPENING &&
	    hfp->dlci == 0U &&
	    (dlci >> 1) == hfp->ag_channel) {
		hfp_fail(hfp, "refused", HFP_RETRY_STEP);
		hfp_tell(hfp);
	}
}

/* Takes a DLC that could not be asked for (the profile's open_failed hook): the AG's fails the HF. */
void
btd_hfp_open_failed(
	void *context,
	unsigned server_channel)
{
	struct btd_hfp *hfp;

	/* The AG's DLC asked for. */
	hfp = context;
	if (hfp->state != BTD_HFP_OPENING || server_channel != hfp->ag_channel)
		return;

	/* Failed, tried again later. */
	hfp_fail(hfp, "rfcomm", HFP_RETRY_STEP);
	hfp_tell(hfp);
}

/*
 * Starts an owner's request (BTD_HFP_ASK_*) with its argument (the number
 * to dial, CHLD's action 0 to 2, a DTMF key, the volume 0 to 15, voice 0
 * or 1; NULL for the others).  Its answer goes to token: DONE, or ERROR
 * with the AG's refusal.  Returns NULL when it started, or the word of an
 * ERROR to answer now: not-ready, argument, no-hold, no-voice, no-call,
 * busy.
 */
const char *
btd_hfp_ask(
	struct btd_hfp *hfp,
	uint64_t token,
	unsigned ask,
	const char *argument)
{
	char text[BTD_HFP_COMMAND_TEXT_MAX];
	struct btd_hfp_call *active;
	const char *key;
	unsigned timeout;
	long number;
	int error;
	int ok;

	/* An SLC that is ready. */
	if (hfp->state != BTD_HFP_READY)
		return "not-ready";

	/* Each request's command. */
	timeout = BTD_HFP_COMMAND_MS;
	switch (ask) {
	case BTD_HFP_ASK_DIAL:
		/* A number to dial. */
		if (argument == NULL)
			return "argument";
		ok = hfp_number_ok(argument);
		if (!ok)
			return "argument";
		(void)snprintf(text, sizeof(text), "ATD%s;", argument);
		timeout = BTD_HFP_DIAL_MS;
		break;
	case BTD_HFP_ASK_ANSWER:
		/* A call that rings. */
		active = hfp_find(hfp, BTD_HFP_CALL_INCOMING);
		if (active == NULL)
			return "no-call";
		(void)snprintf(text, sizeof(text), "ATA");
		break;
	case BTD_HFP_ASK_HANGUP:
		/* The call that rings, is being made, or is on (AT+CHUP ends or refuses it). */
		(void)snprintf(text, sizeof(text), "AT+CHUP");
		break;
	case BTD_HFP_ASK_HOLD:
		/* CHLD's action 0, 1 or 2, when both sides hold calls. */
		if ((hfp->ag_features & BTD_HFP_AG_THREE_WAY) == 0U)
			return "no-hold";
		if (argument == NULL || argument[0] < '0' || argument[0] > '2' || argument[1] != '\0')
			return "argument";
		(void)snprintf(text, sizeof(text), "AT+CHLD=%c", argument[0]);
		break;
	case BTD_HFP_ASK_DTMF:
		/* One key, during a call. */
		if (argument == NULL || argument[0] == '\0' || argument[1] != '\0')
			return "argument";
		key = strchr("0123456789*#ABCD", argument[0]);
		if (key == NULL)
			return "argument";
		active = hfp_find(hfp, BTD_HFP_CALL_ACTIVE);
		if (active == NULL)
			return "no-call";
		(void)snprintf(text, sizeof(text), "AT+VTS=%c", argument[0]);
		break;
	case BTD_HFP_ASK_VOLUME:
		/* 0 to 15. */
		if (argument == NULL)
			return "argument";
		error = btd_at_number(argument, 0U, &number);
		if (error != 0 || number < 0L || number > 15L)
			return "argument";
		(void)snprintf(text, sizeof(text), "AT+VGS=%ld", number);
		hfp->speaker = (int)number;
		break;
	case BTD_HFP_ASK_VOICE:
		/* 0 or 1, when the AG has voice recognition. */
		if ((hfp->ag_features & BTD_HFP_AG_VOICE) == 0U)
			return "no-voice";
		if (argument == NULL || (argument[0] != '0' && argument[0] != '1') || argument[1] != '\0')
			return "argument";
		(void)snprintf(text, sizeof(text), "AT+BVRA=%c", argument[0]);
		break;
	default:
		return "argument";
	}

	/* Queued behind the others. */
	error = hfp_queue(hfp, text, HFP_STEP_NONE, token, timeout);
	if (error != 0)
		return "busy";

	/* Succeeded: written when its turn comes. */
	hfp_pump(hfp);
	return NULL;
}

/*
 * Forgets a client that went (the token's): its waiting requests stay
 * queued (the AG still carries them out) but are answered to nobody.
 */
void
btd_hfp_cancel(
	struct btd_hfp *hfp,
	uint64_t token)
{
	unsigned index;

	/* Each of its commands answers nobody. */
	for (index = 0U; index < hfp->command_count; index++) {
		if (hfp->commands[index].token == token)
			hfp->commands[index].token = 0U;
	}
}

/*
 * Settles the HF after the owner's choice changed (calls on or off, the
 * record or the seat): started when wanted on a ready link, stopped when
 * not wanted.
 */
void
btd_hfp_check(
	struct btd_hfp *hfp)
{
	int wanted;

	/* Wanted now. */
	wanted = hfp->hooks.wanted(hfp->hooks.context);

	/* Not wanted: off, its DLC closed. */
	if (!wanted && hfp->state != BTD_HFP_OFF) {
		if (hfp->dlci != 0U)
			hfp_close_later(hfp, hfp->dlci);
		hfp->dlci = 0U;
		hfp_stop(hfp);
		hfp_tell(hfp);
		return;
	}

	/* Wanted on a ready link and off: started. */
	if (wanted && hfp->link_ready && hfp->state == BTD_HFP_OFF) {
		hfp->retry_step = 0U;
		hfp_start(hfp);
		hfp_tell(hfp);
	}
}

/*
 * Moves the HF on at a time: DLCs to close are closed, an unanswered
 * command fails, and a wait ended starts the HF or asks again.
 */
void
btd_hfp_tick(
	struct btd_hfp *hfp,
	uint64_t now)
{
	unsigned index;
	int wanted;

	/* The DLCs to close. */
	for (index = 0U; index < hfp->closing_count; index++)
		hfp->hooks.dlc_close(hfp->hooks.context, hfp->closing[index]);
	hfp->closing_count = 0U;

	/* A command unanswered: an SLC step fails the HF, an owner's request is answered as timed out. */
	if (hfp->written && now >= hfp->deadline)
		hfp_finished(hfp, 0, "timeout");

	/* A busy SDP query or channel asked again. */
	if (hfp->state == BTD_HFP_SDP && hfp->sdp_again_at != 0U && now >= hfp->sdp_again_at)
		hfp_start(hfp);
	if (hfp->state == BTD_HFP_OPENING && hfp->open_again_at != 0U && now >= hfp->open_again_at)
		hfp_open(hfp);

	/* The wait after a failure ended: started again when still wanted. */
	if (hfp->state == BTD_HFP_FAILED && hfp->retry_at != 0U && now >= hfp->retry_at && hfp->link_ready) {
		wanted = hfp->hooks.wanted(hfp->hooks.context);
		if (wanted)
			hfp_start(hfp);
	}

	/* What changed is told. */
	hfp_pump(hfp);
	hfp_tell(hfp);
}

/* Tells when the HF next needs a tick (0: no time waits). */
uint64_t
btd_hfp_deadline(
	const struct btd_hfp *hfp)
{
	uint64_t earliest;

	/* DLCs to close: at once. */
	if (hfp->closing_count != 0U)
		return 1U;

	/* The earliest of the waits that are set. */
	earliest = 0U;
	if (hfp->written)
		earliest = hfp->deadline;
	if (hfp->state == BTD_HFP_SDP && hfp->sdp_again_at != 0U && (earliest == 0U || hfp->sdp_again_at < earliest))
		earliest = hfp->sdp_again_at;
	if (hfp->state == BTD_HFP_OPENING && hfp->open_again_at != 0U && (earliest == 0U || hfp->open_again_at < earliest))
		earliest = hfp->open_again_at;
	if (hfp->state == BTD_HFP_FAILED && hfp->retry_at != 0U && (earliest == 0U || hfp->retry_at < earliest))
		earliest = hfp->retry_at;

	/* The earliest, or 0. */
	return earliest;
}

/* Gives the HF's state as the phone's line tells it: off, connecting, ready, failed. */
const char *
btd_hfp_state_word(
	const struct btd_hfp *hfp)
{
	/* Each state's word. */
	switch (hfp->state) {
	case BTD_HFP_SDP:
	case BTD_HFP_OPENING:
	case BTD_HFP_SLC:
		return "connecting";
	case BTD_HFP_READY:
		return "ready";
	case BTD_HFP_FAILED:
		return "failed";
	default:
		break;
	}

	/* Off. */
	return "off";
}

/*
 * Writes a line a new subscriber is told: 0 the PHONE HF line, 1 and 2
 * the PHONE CALL line of each call there is.  Returns 0, or ENOENT when
 * there is no such line.
 */
int
btd_hfp_line(
	const struct btd_hfp *hfp,
	unsigned index,
	char *line,
	size_t size)
{
	const struct btd_hfp_call *call;

	/* The HF's line. */
	if (index == 0U) {
		hfp_hf_line(hfp, line, size);
		return 0;
	}

	/* A call's, when there is one in the slot. */
	if (index > BTD_HFP_CALLS_MAX)
		return ENOENT;
	call = &hfp->calls[index - 1U];
	if (call->state == BTD_HFP_CALL_NONE)
		return ENOENT;
	hfp_call_line(call, call->state, line, size);

	/* Succeeded: the call's line. */
	return 0;
}

/* Gives the daemon's clock. */
static uint64_t
hfp_now(
	struct btd_hfp *hfp)
{
	uint64_t now;

	/* The hook's. */
	now = hfp->hooks.clock(hfp->hooks.context);
	return now;
}

/* Logs a line of the HF's progress when the daemon logs. */
static void
hfp_log(
	struct btd_hfp *hfp,
	const char *line)
{
	/* No log. */
	if (hfp->hooks.log == NULL)
		return;

	/* Logged. */
	hfp->hooks.log(hfp->hooks.context, line);
}

/* Starts the HF: the SDP query for the AG (asked again shortly when the link's query is busy). */
static void
hfp_start(
	struct btd_hfp *hfp)
{
	int error;

	/* The query. */
	hfp->state = BTD_HFP_SDP;
	hfp->why = "";
	hfp->sdp_again_at = 0U;
	error = hfp->hooks.sdp_query(hfp->hooks.context, BTD_SDP_UUID_AG);
	if (error == EBUSY) {
		hfp->sdp_again_at = hfp_now(hfp) + BTD_HFP_BUSY_MS;
		return;
	}

	/* A query that cannot be asked: tried again later. */
	if (error != 0) {
		hfp_fail(hfp, "sdp", HFP_RETRY_STEP);
		return;
	}

	/* Succeeded: the answer is awaited. */
	hfp_log(hfp, "hfp: looking for the phone's audio gateway");
}

/* Asks for the DLC to the AG's channel (asked again shortly when busy). */
static void
hfp_open(
	struct btd_hfp *hfp)
{
	int error;

	/* The DLC. */
	hfp->state = BTD_HFP_OPENING;
	hfp->open_again_at = 0U;
	error = hfp->hooks.dlc_open(hfp->hooks.context, hfp->ag_channel);
	if (error == EBUSY) {
		hfp->open_again_at = hfp_now(hfp) + BTD_HFP_BUSY_MS;
		return;
	}

	/* A DLC that cannot be asked for: tried again later. */
	if (error != 0)
		hfp_fail(hfp, "rfcomm", HFP_RETRY_STEP);
}

/* Takes a DLC for the SLC and starts it with AT+BRSF. */
static void
hfp_take(
	struct btd_hfp *hfp,
	unsigned dlci)
{
	char text[BTD_HFP_COMMAND_TEXT_MAX];
	int error;

	/* The DLC, its lines from nothing, no command and no indicator known. */
	hfp->dlci = dlci;
	hfp->state = BTD_HFP_SLC;
	hfp->command_count = 0U;
	hfp->written = 0;
	hfp->out_length = 0U;
	hfp->out_sent = 0U;
	hfp->indicator_count = 0U;
	hfp->ag_features = 0U;
	btd_at_input_init(&hfp->input);
	hfp_log(hfp, "hfp: service level connection starts");

	/* The HF's features first (HFP 1.8 section 4.2.1.1). */
	(void)snprintf(text, sizeof(text), "AT+BRSF=%u", (unsigned)BTD_HFP_HF_FEATURES);
	error = hfp_queue(hfp, text, HFP_STEP_BRSF, 0U, BTD_HFP_COMMAND_MS);
	if (error != 0) {
		hfp_fail(hfp, "queue", HFP_RETRY_STEP);
		return;
	}

	/* Succeeded: written at once. */
	hfp_pump(hfp);
}

/*
 * Fails the HF: the calls end, the requests are answered as lost, the
 * SLC's DLC is closed, and the HF waits (the next step's wait, or for the
 * next link).
 */
static void
hfp_fail(
	struct btd_hfp *hfp,
	const char *why,
	int never)
{
	char line[96];
	uint64_t wait;

	/* The SLC's DLC closed and forgotten, the calls ended, the requests answered. */
	if (hfp->dlci != 0U)
		hfp_close_later(hfp, hfp->dlci);
	hfp->dlci = 0U;
	hfp_end_calls(hfp);
	hfp_forget_commands(hfp, "ERROR lost");

	/* Failed, why. */
	hfp->state = BTD_HFP_FAILED;
	hfp->why = why;
	(void)snprintf(line, sizeof(line), "hfp: failed (%s)", why);
	hfp_log(hfp, line);

	/* Not tried again on this link. */
	if (never) {
		hfp->retry_at = 0U;
		return;
	}

	/* The next step's wait, doubling to the last. */
	wait = (uint64_t)BTD_HFP_RETRY_FIRST_MS << hfp->retry_step;
	if (wait > BTD_HFP_RETRY_LAST_MS)
		wait = BTD_HFP_RETRY_LAST_MS;
	else
		hfp->retry_step++;
	hfp->retry_at = hfp_now(hfp) + wait;
}

/* Stops the HF: the calls end, the requests are answered as lost, off. */
static void
hfp_stop(
	struct btd_hfp *hfp)
{
	/* The calls and the requests. */
	hfp_end_calls(hfp);
	hfp_forget_commands(hfp, "ERROR lost");

	/* Off, the indicators and the volume unknown. */
	hfp->state = BTD_HFP_OFF;
	hfp->why = "";
	hfp->retry_at = 0U;
	hfp->indicator_count = 0U;
	hfp->speaker = -1;
	hfp->voice = 0;
}

/* Closes a DLC at the next tick (from a hook the link must not be changed under). */
static void
hfp_close_later(
	struct btd_hfp *hfp,
	unsigned dlci)
{
	/* Room: a fifth is closed now, which the link tolerates outside its own calls. */
	if (hfp->closing_count >= sizeof(hfp->closing) / sizeof(hfp->closing[0])) {
		hfp->hooks.dlc_close(hfp->hooks.context, dlci);
		return;
	}

	/* Kept for the tick. */
	hfp->closing[hfp->closing_count] = dlci;
	hfp->closing_count++;
}

/* Answers every waiting owner's request with a line, then DONE, and forgets every command. */
static void
hfp_forget_commands(
	struct btd_hfp *hfp,
	const char *answer)
{
	unsigned index;

	/* Each owner's request. */
	for (index = 0U; index < hfp->command_count; index++) {
		if (hfp->commands[index].token == 0U)
			continue;
		hfp->hooks.answer(hfp->hooks.context, hfp->commands[index].token, answer, NULL, 0U);
		hfp->hooks.answer(hfp->hooks.context, hfp->commands[index].token, "DONE", NULL, 0U);
	}

	/* None left, nothing written. */
	hfp->command_count = 0U;
	hfp->written = 0;
	hfp->out_length = 0U;
	hfp->out_sent = 0U;
	hfp->listing = 0;
}

/* Puts a command at the end of the queue.  Returns 0, or EBUSY when it is full. */
static int
hfp_queue(
	struct btd_hfp *hfp,
	const char *text,
	unsigned step,
	uint64_t token,
	unsigned timeout_ms)
{
	struct btd_hfp_command *command;

	/* Room. */
	if (hfp->command_count >= BTD_HFP_COMMANDS_MAX)
		return EBUSY;

	/* The command. */
	command = &hfp->commands[hfp->command_count];
	(void)snprintf(command->text, sizeof(command->text), "%s", text);
	command->step = step;
	command->token = token;
	command->timeout_ms = timeout_ms;
	hfp->command_count++;

	/* Succeeded: queued. */
	return 0;
}

/* Writes the first command when none is written, and the rest of one being written, as far as the DLC takes it. */
static void
hfp_pump(
	struct btd_hfp *hfp)
{
	struct btd_hfp_command *command;
	size_t written;
	int error;

	/* A DLC to write on. */
	if (hfp->dlci == 0U)
		return;

	/* The first command's bytes, with its CR, when it is not written yet. */
	if (!hfp->written && hfp->command_count != 0U && hfp->out_length == 0U) {
		command = &hfp->commands[0];
		hfp->out_length = (size_t)snprintf(hfp->out, sizeof(hfp->out), "%s\r", command->text);
		hfp->out_sent = 0U;
		hfp->written = 1;
		hfp->deadline = hfp_now(hfp) + command->timeout_ms;
		if (command->step == HFP_STEP_CLCC)
			hfp->listing = 0;
	}

	/* Nothing to write. */
	if (hfp->out_sent >= hfp->out_length)
		return;

	/* As far as the DLC takes it; the rest when it is writable again. */
	written = 0U;
	error = hfp->hooks.dlc_write(hfp->hooks.context, hfp->dlci, (const uint8_t *)hfp->out + hfp->out_sent, hfp->out_length - hfp->out_sent, &written);
	if (error != 0)
		return;
	hfp->out_sent += written;
}

/* Reads one line of the AG: a command's final result, or a result or an unsolicited one. */
static void
hfp_line(
	void *context,
	const char *line)
{
	struct btd_hfp *hfp;
	struct btd_hfp_call *ringing;
	struct btd_hfp_call *on;
	const char *rest;
	long value;
	unsigned step;
	int same;
	int is;
	int error;

	/* The step of the command written (none when none is). */
	hfp = context;
	step = HFP_STEP_NONE;
	if (hfp->written && hfp->command_count != 0U)
		step = hfp->commands[0].step;

	/* OK: the command written succeeded. */
	same = strcmp(line, "OK");
	if (same == 0) {
		if (hfp->written)
			hfp_finished(hfp, 1, NULL);
		return;
	}

	/* ERROR, or +CME ERROR with its number: the command written failed. */
	same = strcmp(line, "ERROR");
	is = btd_at_result(line, "+CME ERROR", &rest);
	if (same == 0 || is) {
		if (hfp->written)
			hfp_finished(hfp, 0, "refused");
		return;
	}

	/* +BRSF: the AG's features. */
	is = btd_at_result(line, "+BRSF", &rest);
	if (is) {
		error = btd_at_number(rest, 0U, &value);
		if (error == 0 && value >= 0L)
			hfp->ag_features = (uint32_t)value;
		return;
	}

	/* +CIND: the indicators' names (CIND=?) or their values (CIND?). */
	is = btd_at_result(line, "+CIND", &rest);
	if (is) {
		hfp_indicators(hfp, rest, step);
		return;
	}

	/* +CIEV: one indicator changed. */
	is = btd_at_result(line, "+CIEV", &rest);
	if (is) {
		hfp_indicator(hfp, rest);
		return;
	}

	/* RING: an incoming call rings (its number by +CLIP), unless one rings or is on already. */
	same = strcmp(line, "RING");
	if (same == 0) {
		ringing = hfp_find(hfp, BTD_HFP_CALL_INCOMING);
		on = hfp_find(hfp, BTD_HFP_CALL_ACTIVE);
		if (ringing == NULL && on == NULL)
			hfp_set(hfp, hfp_slot(hfp), BTD_HFP_CALL_INCOMING, 0);
		return;
	}

	/* +CLIP: the ringing call's number and name. */
	is = btd_at_result(line, "+CLIP", &rest);
	if (is) {
		hfp_clip(hfp, rest, 0);
		return;
	}

	/* +CCWA: a call waiting during another, its number and name. */
	is = btd_at_result(line, "+CCWA", &rest);
	if (is) {
		hfp_clip(hfp, rest, 1);
		return;
	}

	/* +CLCC: one call of the list. */
	is = btd_at_result(line, "+CLCC", &rest);
	if (is) {
		hfp_listed(hfp, rest);
		return;
	}

	/* +VGS: the AG set the speaker's volume. */
	is = btd_at_result(line, "+VGS", &rest);
	if (is) {
		error = btd_at_number(rest, 0U, &value);
		if (error == 0 && value >= 0L && value <= 15L)
			hfp->speaker = (int)value;
		return;
	}

	/* +BVRA: voice recognition on or off. */
	is = btd_at_result(line, "+BVRA", &rest);
	if (is) {
		error = btd_at_number(rest, 0U, &value);
		if (error == 0)
			hfp->voice = value != 0L;
		return;
	}

	/* Anything else (+CHLD's list, +VGM, +BSIR, +BCS, a result not known) changes nothing here. */
}

/*
 * Ends the command written (ok, or failed with why): an SLC step moves
 * the SLC on or fails it, AT+CLCC's list replaces the calls, an owner's
 * request is answered.
 */
static void
hfp_finished(
	struct btd_hfp *hfp,
	int ok,
	const char *why)
{
	struct btd_hfp_command command;
	char line[48];

	/* Off the queue, nothing written. */
	command = hfp->commands[0];
	hfp->command_count--;
	memmove(&hfp->commands[0], &hfp->commands[1], hfp->command_count * sizeof(hfp->commands[0]));
	hfp->written = 0;
	hfp->out_length = 0U;
	hfp->out_sent = 0U;

	/* An owner's request: DONE, or ERROR and why. */
	if (command.token != 0U) {
		if (!ok) {
			(void)snprintf(line, sizeof(line), "ERROR %s", why);
			hfp->hooks.answer(hfp->hooks.context, command.token, line, NULL, 0U);
		}

		/* DONE ends the request's wait. */
		hfp->hooks.answer(hfp->hooks.context, command.token, "DONE", NULL, 0U);
	}

	/* AT+CLCC: its list (an AG that lists nothing or refuses leaves the indicators' guess). */
	if (command.step == HFP_STEP_CLCC) {
		if (ok)
			hfp_list_end(hfp);
		hfp->listing = 0;
		return;
	}

	/* An SLC step. */
	if (command.step != HFP_STEP_NONE)
		hfp_slc_next(hfp, command.step, ok);
}

/* Moves the SLC on after a step's answer (HFP 1.8 section 4.2.1): the next command, or the SLC ready, or failed. */
static void
hfp_slc_next(
	struct btd_hfp *hfp,
	unsigned step,
	int ok)
{
	int error;

	/* The commands after the SLC: a refusal changes nothing. */
	if (step == HFP_STEP_AFTER)
		return;

	/* A step of the SLC refused: failed, except CHLD=?, without which the HF holds no calls. */
	if (!ok && step != HFP_STEP_CHLD) {
		hfp_fail(hfp, "slc", HFP_RETRY_STEP);
		return;
	}

	/* Each step's next. */
	error = 0;
	switch (step) {
	case HFP_STEP_BRSF:
		error = hfp_queue(hfp, "AT+CIND=?", HFP_STEP_CIND_TEST, 0U, BTD_HFP_COMMAND_MS);
		break;
	case HFP_STEP_CIND_TEST:
		/* The indicators named, at least call and callsetup. */
		if (hfp->at_call < 0 || hfp->indicator_count == 0U) {
			hfp_fail(hfp, "cind", HFP_RETRY_STEP);
			return;
		}

		/* Their values. */
		error = hfp_queue(hfp, "AT+CIND?", HFP_STEP_CIND_READ, 0U, BTD_HFP_COMMAND_MS);
		break;
	case HFP_STEP_CIND_READ:
		error = hfp_queue(hfp, "AT+CMER=3,0,0,1", HFP_STEP_CMER, 0U, BTD_HFP_COMMAND_MS);
		break;
	case HFP_STEP_CMER:
		/* Both sides hold calls: what the AG's CHLD offers. */
		if ((hfp->ag_features & BTD_HFP_AG_THREE_WAY) != 0U) {
			error = hfp_queue(hfp, "AT+CHLD=?", HFP_STEP_CHLD, 0U, BTD_HFP_COMMAND_MS);
			break;
		}

		/* Otherwise the SLC is ready. */
		hfp_slc_done(hfp);
		return;
	case HFP_STEP_CHLD:
		hfp_slc_done(hfp);
		return;
	default:
		return;
	}

	/* A queue that cannot take the step. */
	if (error != 0)
		hfp_fail(hfp, "queue", HFP_RETRY_STEP);
}

/* The SLC is ready: the calling line, call waiting and the errors' numbers asked for, the calls listed, and told. */
static void
hfp_slc_done(
	struct btd_hfp *hfp)
{
	/* Ready, the waits from the start. */
	hfp->state = BTD_HFP_READY;
	hfp->retry_step = 0U;
	hfp_log(hfp, "hfp: ready");

	/* The commands after the SLC (their refusal changes nothing); the queue has room for them. */
	(void)hfp_queue(hfp, "AT+CLIP=1", HFP_STEP_AFTER, 0U, BTD_HFP_COMMAND_MS);
	if ((hfp->ag_features & BTD_HFP_AG_THREE_WAY) != 0U)
		(void)hfp_queue(hfp, "AT+CCWA=1", HFP_STEP_AFTER, 0U, BTD_HFP_COMMAND_MS);
	(void)hfp_queue(hfp, "AT+CMEE=1", HFP_STEP_AFTER, 0U, BTD_HFP_COMMAND_MS);

	/* The calls there are now: the AG's list, else the indicators' guess. */
	if ((hfp->ag_features & BTD_HFP_AG_CALL_STATUS) != 0U)
		(void)hfp_queue(hfp, "AT+CLCC", HFP_STEP_CLCC, 0U, BTD_HFP_COMMAND_MS);
	else
		hfp_infer(hfp);
}

/* Reads +CIND: the names in the AG's order for CIND=?, the values for CIND?. */
static void
hfp_indicators(
	struct btd_hfp *hfp,
	const char *rest,
	unsigned step)
{
	size_t count;
	size_t index;
	long value;
	int error;
	int same;

	/* CIND=?: the names, and where the ones the HF reads are. */
	if (step == HFP_STEP_CIND_TEST) {
		error = btd_at_indicators(rest, hfp->names, BTD_AT_INDICATORS_MAX, &count);
		if (error != 0)
			return;
		hfp->indicator_count = (unsigned)count;
		for (index = 0U; index < count; index++) {
			hfp->values[index] = 0L;
			same = strcmp(hfp->names[index], "call");
			if (same == 0)
				hfp->at_call = (int)index;
			same = strcmp(hfp->names[index], "callsetup");
			if (same == 0)
				hfp->at_callsetup = (int)index;
			same = strcmp(hfp->names[index], "call_setup");
			if (same == 0)
				hfp->at_callsetup = (int)index;
			same = strcmp(hfp->names[index], "callheld");
			if (same == 0)
				hfp->at_callheld = (int)index;
			same = strcmp(hfp->names[index], "service");
			if (same == 0)
				hfp->at_service = (int)index;
			same = strcmp(hfp->names[index], "signal");
			if (same == 0)
				hfp->at_signal = (int)index;
			same = strcmp(hfp->names[index], "roam");
			if (same == 0)
				hfp->at_roam = (int)index;
			same = strcmp(hfp->names[index], "battchg");
			if (same == 0)
				hfp->at_battchg = (int)index;
		}

		/* The names are known. */
		return;
	}

	/* CIND?: each value in the same order. */
	for (index = 0U; index < hfp->indicator_count; index++) {
		error = btd_at_number(rest, (unsigned)index, &value);
		if (error != 0)
			break;
		hfp->values[index] = value;
	}
}

/* Reads +CIEV: an indicator's number (1 the first) and value; a call's indicator changes the calls. */
static void
hfp_indicator(
	struct btd_hfp *hfp,
	const char *rest)
{
	unsigned index;
	long number;
	long value;
	int error;
	int at;

	/* The indicator and its value. */
	error = btd_at_number(rest, 0U, &number);
	if (error != 0)
		return;
	error = btd_at_number(rest, 1U, &value);
	if (error != 0)
		return;
	if (number < 1L || number > (long)hfp->indicator_count)
		return;
	at = (int)(number - 1L);
	hfp->values[at] = value;

	/* Not a call's indicator: told by the HF line. */
	if (at != hfp->at_call && at != hfp->at_callsetup && at != hfp->at_callheld)
		return;

	/* The calls: the indicators' guess at once, then the AG's list when it has one (one waits behind the one being read, which may be older than this change). */
	hfp_infer(hfp);
	if ((hfp->ag_features & BTD_HFP_AG_CALL_STATUS) == 0U || hfp->state != BTD_HFP_READY)
		return;
	for (index = 0U; index < hfp->command_count; index++) {
		if (hfp->commands[index].step == HFP_STEP_CLCC && (index != 0U || !hfp->written))
			return;
	}

	/* A list behind the others (the queue's refusal leaves the guess). */
	(void)hfp_queue(hfp, "AT+CLCC", HFP_STEP_CLCC, 0U, BTD_HFP_COMMAND_MS);
}

/* Reads +CLIP (the ringing call) or +CCWA (a waiting call): "number",type[,...,"name"]. */
static void
hfp_clip(
	struct btd_hfp *hfp,
	const char *rest,
	int waiting)
{
	struct btd_hfp_call *call;
	char number[BTD_HFP_NUMBER_MAX];
	char name[BTD_HFP_NAME_MAX];
	int error;

	/* The number, and the name (+CLIP's fifth field, +CCWA's fifth). */
	error = btd_at_field(rest, 0U, number, sizeof(number));
	if (error != 0)
		return;
	error = btd_at_field(rest, 4U, name, sizeof(name));
	if (error != 0)
		name[0] = '\0';

	/* The call it belongs to: the waiting one, or the ringing one (made when RING has not come yet). */
	if (waiting) {
		call = hfp_find(hfp, BTD_HFP_CALL_WAITING);
		if (call == NULL) {
			call = hfp_slot(hfp);
			hfp_set(hfp, call, BTD_HFP_CALL_WAITING, 0);
		}
	} else {
		call = hfp_find(hfp, BTD_HFP_CALL_INCOMING);
		if (call == NULL) {
			call = hfp_slot(hfp);
			hfp_set(hfp, call, BTD_HFP_CALL_INCOMING, 0);
		}
	}

	/* Its number and name. */
	(void)snprintf(call->number, sizeof(call->number), "%s", number);
	(void)snprintf(call->name, sizeof(call->name), "%s", name);
}

/*
 * Reads one +CLCC: index,direction,state,mode,multiparty[,"number",type[,"name"]]
 * into the list being read (TS 27.007 section 7.18: state 0 active, 1
 * held, 2 dialing, 3 alerting, 4 incoming, 5 waiting).
 */
static void
hfp_listed(
	struct btd_hfp *hfp,
	const char *rest)
{
	static const unsigned states[6] = {
		BTD_HFP_CALL_ACTIVE, BTD_HFP_CALL_HELD, BTD_HFP_CALL_DIALING,
		BTD_HFP_CALL_ALERTING, BTD_HFP_CALL_INCOMING, BTD_HFP_CALL_WAITING
	};
	struct btd_hfp_call *call;
	long index;
	long direction;
	long state;
	long mode;
	int error;

	/* A list from its first line. */
	if (!hfp->listing) {
		hfp->listing = 1;
		hfp->listed_count = 0U;
	}

	/* The index, the direction, the state, the mode: a voice call (mode 0) the HF follows. */
	error = btd_at_number(rest, 0U, &index);
	if (error == 0)
		error = btd_at_number(rest, 1U, &direction);
	if (error == 0)
		error = btd_at_number(rest, 2U, &state);
	if (error == 0)
		error = btd_at_number(rest, 3U, &mode);
	if (error != 0 || state < 0L || state > 5L || mode != 0L || index < 1L)
		return;

	/* Room for it. */
	if (hfp->listed_count >= BTD_HFP_CALLS_MAX)
		return;

	/* The call listed, its number and name when given. */
	call = &hfp->listed[hfp->listed_count];
	memset(call, 0, sizeof(*call));
	call->index = (unsigned)index;
	call->outgoing = direction == 0L;
	call->state = states[state];
	error = btd_at_field(rest, 5U, call->number, sizeof(call->number));
	if (error != 0)
		call->number[0] = '\0';
	error = btd_at_field(rest, 7U, call->name, sizeof(call->name));
	if (error != 0)
		call->name[0] = '\0';
	hfp->listed_count++;
}

/*
 * Takes AT+CLCC's whole list: a call listed takes the slot of the same
 * index (or a free one), keeping a name it had; a call not listed ends.
 */
static void
hfp_list_end(
	struct btd_hfp *hfp)
{
	struct btd_hfp_call *call;
	struct btd_hfp_call *listed;
	unsigned slot;
	unsigned index;
	int kept[BTD_HFP_CALLS_MAX];

	/* An OK without lines lists no call. */
	if (!hfp->listing)
		hfp->listed_count = 0U;

	/* Each call listed: its slot by index, else the call in the same state without an index, else a free one. */
	memset(kept, 0, sizeof(kept));
	for (index = 0U; index < hfp->listed_count; index++) {
		listed = &hfp->listed[index];
		call = NULL;
		for (slot = 0U; slot < BTD_HFP_CALLS_MAX; slot++) {
			if (hfp->calls[slot].state != BTD_HFP_CALL_NONE && hfp->calls[slot].index == listed->index && !kept[slot]) {
				call = &hfp->calls[slot];
				break;
			}
		}

		/* Else a call without an index yet (the indicators' guess). */
		for (slot = 0U; call == NULL && slot < BTD_HFP_CALLS_MAX; slot++) {
			if (hfp->calls[slot].state != BTD_HFP_CALL_NONE && hfp->calls[slot].index == 0U && !kept[slot])
				call = &hfp->calls[slot];
		}

		/* Else a free slot. */
		for (slot = 0U; call == NULL && slot < BTD_HFP_CALLS_MAX; slot++) {
			if (hfp->calls[slot].state == BTD_HFP_CALL_NONE && !kept[slot])
				call = &hfp->calls[slot];
		}

		/* No room for a third. */
		if (call == NULL)
			continue;

		/* The listed state, index and direction; its number and name when the list gives them. */
		kept[call - hfp->calls] = 1;
		call->state = listed->state;
		call->index = listed->index;
		call->outgoing = listed->outgoing;
		if (listed->number[0] != '\0')
			(void)snprintf(call->number, sizeof(call->number), "%s", listed->number);
		if (listed->name[0] != '\0')
			(void)snprintf(call->name, sizeof(call->name), "%s", listed->name);
	}

	/* A call not listed has ended. */
	for (slot = 0U; slot < BTD_HFP_CALLS_MAX; slot++) {
		if (!kept[slot] && hfp->calls[slot].state != BTD_HFP_CALL_NONE)
			hfp->calls[slot].state = BTD_HFP_CALL_NONE;
	}
}

/*
 * Guesses the calls from the indicators (HFP 1.8 section 4.10 onwards):
 * callsetup 1 rings (or waits during a call), 2 dials, 3 alerts; call 1
 * with no setup is a call on; callheld 1 holds one of two, 2 holds the
 * only one; call 0 and callsetup 0 end everything.
 */
static void
hfp_infer(
	struct btd_hfp *hfp)
{
	struct btd_hfp_call *call;
	struct btd_hfp_call *setup;
	long on;
	long setting;
	long held;
	unsigned slot;

	/* The three indicators (0 when the AG has not got one). */
	on = hfp_value(hfp, hfp->at_call);
	setting = hfp_value(hfp, hfp->at_callsetup);
	held = hfp_value(hfp, hfp->at_callheld);

	/* The call being set up, if any. */
	setup = hfp_find(hfp, BTD_HFP_CALL_INCOMING);
	if (setup == NULL)
		setup = hfp_find(hfp, BTD_HFP_CALL_WAITING);
	if (setup == NULL)
		setup = hfp_find(hfp, BTD_HFP_CALL_DIALING);
	if (setup == NULL)
		setup = hfp_find(hfp, BTD_HFP_CALL_ALERTING);

	/* A setup: ringing (waiting during a call), dialing, alerting. */
	if (setting == 1L) {
		if (setup == NULL)
			setup = hfp_slot(hfp);
		if (on == 1L)
			hfp_set(hfp, setup, BTD_HFP_CALL_WAITING, 0);
		else
			hfp_set(hfp, setup, BTD_HFP_CALL_INCOMING, 0);
	} else if (setting == 2L) {
		if (setup == NULL)
			setup = hfp_slot(hfp);
		hfp_set(hfp, setup, BTD_HFP_CALL_DIALING, 1);
	} else if (setting == 3L) {
		if (setup == NULL)
			setup = hfp_slot(hfp);
		hfp_set(hfp, setup, BTD_HFP_CALL_ALERTING, 1);
	} else if (setup != NULL) {
		/* The setup ended: on when a call is on and no other is, else gone. */
		call = hfp_find(hfp, BTD_HFP_CALL_ACTIVE);
		if (on == 1L && call == NULL)
			setup->state = BTD_HFP_CALL_ACTIVE;
		else
			setup->state = BTD_HFP_CALL_NONE;
	}

	/* No call on and nothing held: every call that is not being set up ended. */
	if (on == 0L && held == 0L) {
		for (slot = 0U; slot < BTD_HFP_CALLS_MAX; slot++) {
			call = &hfp->calls[slot];
			if (call->state == BTD_HFP_CALL_ACTIVE || call->state == BTD_HFP_CALL_HELD)
				call->state = BTD_HFP_CALL_NONE;
		}

		/* Nothing is on. */
		return;
	}

	/* A call on that the HF did not see start (it was on before the SLC). */
	call = hfp_find(hfp, BTD_HFP_CALL_ACTIVE);
	setup = hfp_find(hfp, BTD_HFP_CALL_HELD);
	if (on == 1L && setting == 0L && call == NULL && setup == NULL)
		hfp_set(hfp, hfp_slot(hfp), BTD_HFP_CALL_ACTIVE, 0);

	/* Held: the only call (2), or one of two (1, the other on). */
	call = hfp_find(hfp, BTD_HFP_CALL_ACTIVE);
	if (held == 2L && call != NULL)
		call->state = BTD_HFP_CALL_HELD;
}

/* Finds the first call in a state; NULL when there is none. */
static struct btd_hfp_call *
hfp_find(
	struct btd_hfp *hfp,
	unsigned state)
{
	unsigned slot;

	/* Each slot. */
	for (slot = 0U; slot < BTD_HFP_CALLS_MAX; slot++) {
		if (hfp->calls[slot].state == state)
			return &hfp->calls[slot];
	}

	/* None. */
	return NULL;
}

/* Finds a free slot, or the last one (two calls are the most HFP carries). */
static struct btd_hfp_call *
hfp_slot(
	struct btd_hfp *hfp)
{
	unsigned slot;

	/* The first free one. */
	for (slot = 0U; slot < BTD_HFP_CALLS_MAX; slot++) {
		if (hfp->calls[slot].state == BTD_HFP_CALL_NONE)
			return &hfp->calls[slot];
	}

	/* None free: the last. */
	return &hfp->calls[BTD_HFP_CALLS_MAX - 1U];
}

/* Sets a call's state and direction; a call new in its slot starts without a number or an index. */
static void
hfp_set(
	struct btd_hfp *hfp,
	struct btd_hfp_call *call,
	unsigned state,
	int outgoing)
{
	/* A new call: nothing known of it but what RING's +CLIP told. */
	UNUSED_PARAMETER(hfp);
	if (call->state == BTD_HFP_CALL_NONE) {
		call->index = 0U;
		call->number[0] = '\0';
		call->name[0] = '\0';
	}

	/* Its state and direction. */
	call->state = state;
	call->outgoing = outgoing;
}

/* Ends every call (the link or the SLC went). */
static void
hfp_end_calls(
	struct btd_hfp *hfp)
{
	unsigned slot;

	/* Each call. */
	for (slot = 0U; slot < BTD_HFP_CALLS_MAX; slot++)
		hfp->calls[slot].state = BTD_HFP_CALL_NONE;
}

/* Tells the subscribers what changed: each call's change (an ended one once), then the HF's line when it changed. */
static void
hfp_tell(
	struct btd_hfp *hfp)
{
	struct btd_hfp_call *call;
	char line[256];
	unsigned slot;
	int number_same;
	int name_same;
	int same;

	/* Each call that changed since it was told. */
	for (slot = 0U; slot < BTD_HFP_CALLS_MAX; slot++) {
		call = &hfp->calls[slot];

		/* An ended call told once, then the slot free of what was told. */
		if (call->state == BTD_HFP_CALL_NONE) {
			if (call->told_state != BTD_HFP_CALL_NONE) {
				hfp_call_line(call, BTD_HFP_CALL_NONE, line, sizeof(line));
				hfp->hooks.emit(hfp->hooks.context, line, NULL, 0U);
			}

			/* The slot free of what was told. */
			call->told_state = BTD_HFP_CALL_NONE;
			call->told_index = 0U;
			call->told_number[0] = '\0';
			call->told_name[0] = '\0';
			continue;
		}

		/* A call as it was told: nothing. */
		number_same = strcmp(call->number, call->told_number);
		name_same = strcmp(call->name, call->told_name);
		if (call->state == call->told_state && call->index == call->told_index && number_same == 0 && name_same == 0)
			continue;

		/* Told, and remembered. */
		hfp_call_line(call, call->state, line, sizeof(line));
		hfp->hooks.emit(hfp->hooks.context, line, NULL, 0U);
		call->told_state = call->state;
		call->told_index = call->index;
		memcpy(call->told_number, call->number, sizeof(call->told_number));
		memcpy(call->told_name, call->name, sizeof(call->told_name));
	}

	/* The HF's line, when it changed. */
	hfp_hf_line(hfp, line, sizeof(line));
	same = strcmp(line, hfp->told);
	if (same == 0)
		return;
	(void)snprintf(hfp->told, sizeof(hfp->told), "%s", line);
	hfp->hooks.emit(hfp->hooks.context, line, NULL, 0U);
}

/* Writes a call's PHONE CALL line in a state (ended for BTD_HFP_CALL_NONE). */
static void
hfp_call_line(
	const struct btd_hfp_call *call,
	unsigned state,
	char *line,
	size_t size)
{
	const char *direction;
	size_t used;
	int written;
	int error;

	/* Its index, state and direction. */
	direction = "in";
	if (call->outgoing)
		direction = "out";
	written = snprintf(line, size, "PHONE CALL index=%u state=%s dir=%s number=", call->index, hfp_call_word(state), direction);
	used = (size_t)written;
	if (used >= size) {
		line[size - 1U] = '\0';
		return;
	}

	/* Its number and name, quoted (cut when the line has no room). */
	error = btd_phoneio_quote(line, size, &used, call->number, BTD_HFP_NUMBER_MAX - 1U);
	if (error != 0)
		return;
	if (used + 6U >= size)
		return;
	memcpy(line + used, " name=", 7U);
	used += 6U;
	(void)btd_phoneio_quote(line, size, &used, call->name, BTD_HFP_NAME_MAX - 1U);
}

/* Writes the HF's PHONE HF line: its state and why, the indicators, the volume, voice recognition, whether it holds calls. */
static void
hfp_hf_line(
	const struct btd_hfp *hfp,
	char *line,
	size_t size)
{
	const char *why;
	int hold;

	/* Why it failed ("-" for nothing). */
	why = hfp->why;
	if (why == NULL || why[0] == '\0')
		why = "-";

	/* Whether calls can be held: both sides hold calls on a ready SLC. */
	hold = 0;
	if ((hfp->ag_features & BTD_HFP_AG_THREE_WAY) != 0U && hfp->state == BTD_HFP_READY)
		hold = 1;

	/* The line. */
	(void)snprintf(line, size, "PHONE HF state=%s why=%s service=%ld signal=%ld battery=%ld roam=%ld speaker=%d voice=%d hold=%d",
		       btd_hfp_state_word(hfp),
		       why,
		       hfp_value(hfp, hfp->at_service),
		       hfp_value(hfp, hfp->at_signal),
		       hfp_value(hfp, hfp->at_battchg),
		       hfp_value(hfp, hfp->at_roam),
		       hfp->speaker,
		       hfp->voice,
		       hold);
}

/* Gives an indicator's value (0 when the AG has not got it, or it is not known yet). */
static long
hfp_value(
	const struct btd_hfp *hfp,
	int at)
{
	/* Not there. */
	if (at < 0 || (unsigned)at >= hfp->indicator_count)
		return 0L;

	/* Its value. */
	return hfp->values[at];
}

/* Gives a call state's word. */
static const char *
hfp_call_word(
	unsigned state)
{
	/* Each state's. */
	switch (state) {
	case BTD_HFP_CALL_INCOMING:
		return "incoming";
	case BTD_HFP_CALL_DIALING:
		return "dialing";
	case BTD_HFP_CALL_ALERTING:
		return "alerting";
	case BTD_HFP_CALL_ACTIVE:
		return "active";
	case BTD_HFP_CALL_HELD:
		return "held";
	case BTD_HFP_CALL_WAITING:
		return "waiting";
	default:
		break;
	}

	/* No call: it ended. */
	return "ended";
}

/* Tells whether a number can be dialed: 1 to 32 of the digits, '+', '*' and '#'. */
static int
hfp_number_ok(
	const char *number)
{
	size_t length;
	size_t span;

	/* Its length and letters. */
	length = strlen(number);
	if (length == 0U || length >= BTD_HFP_NUMBER_MAX)
		return 0;
	span = strspn(number, "0123456789+*#");
	if (span != length)
		return 0;

	/* It can be dialed. */
	return 1;
}
