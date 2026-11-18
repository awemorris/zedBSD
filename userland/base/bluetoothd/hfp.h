/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Hands-Free Profile's hands-free unit, the HF (ws197-p006,
 * plan/ws197/phase006/phase.md): the phone's calls on this computer.
 * Set up when the phone link is ready and the owner wants calls: the SDP
 * query for the phone's audio gateway (AG), a DLC to its RFCOMM channel
 * (or the AG's own DLC to bluetoothd's HF channel), and the service level
 * connection (SLC) over AT commands: AT+BRSF, AT+CIND=?, AT+CIND?,
 * AT+CMER, AT+CHLD=? when both sides hold calls, then AT+CLIP, AT+CCWA,
 * AT+CMEE and AT+CLCC.  One command is written at a time and its OK or
 * ERROR awaited.
 *
 * The calls (at most two) are followed from the indicators (+CIEV), RING,
 * +CLIP, +CCWA, and AT+CLCC's list when the AG has the enhanced call
 * status; each change is told to the phone's subscribers as a PHONE CALL
 * line, the indicators and the volume as a PHONE HF line.  The owner's
 * PHONE DIAL, ANSWER, HANGUP, HOLD, DTMF, VOLUME and VOICE become ATD,
 * ATA, AT+CHUP, AT+CHLD, AT+VTS, AT+VGS and AT+BVRA, answered DONE or
 * ERROR.  The calls' sound is p007's.
 *
 * Without system calls; the host tests build it.
 */

#ifndef BLUETOOTHD_HFP_H
#define BLUETOOTHD_HFP_H

#include "userland/base/bluetoothd/at.h"
#include "userland/base/bluetoothd/sdp.h"

#include <stddef.h>
#include <stdint.h>

/* Bluetoothd's RFCOMM server channel for the HF (its SDP record tells it; MAP's MNS has 16). */
#define BTD_HFP_HF_CHANNEL		17U

/*
 * The HF's features: AT+BRSF's (3-way calling, CLI presentation, voice
 * recognition, remote volume control, enhanced call status) and the SDP
 * record's (3-way, CLI, voice recognition, remote volume).  No EC/NR (the
 * AG keeps its own), no codec negotiation until mSBC (p007b).
 */
#define BTD_HFP_HF_FEATURES		0x003eU
#define BTD_HFP_SDP_FEATURES		0x001eU

/* The AG's features of +BRSF that the HF uses: 3-way calling, voice recognition, enhanced call status. */
#define BTD_HFP_AG_THREE_WAY		0x0001U
#define BTD_HFP_AG_VOICE		0x0004U
#define BTD_HFP_AG_CALL_STATUS		0x0040U

/*
 * The states: off; the SDP query; the DLC being opened; the SLC being
 * made; ready; failed, waiting to try again.
 */
#define BTD_HFP_OFF			0U
#define BTD_HFP_SDP			1U
#define BTD_HFP_OPENING			2U
#define BTD_HFP_SLC			3U
#define BTD_HFP_READY			4U
#define BTD_HFP_FAILED			5U

/* A call's states (0: no call in the slot). */
#define BTD_HFP_CALL_NONE		0U
#define BTD_HFP_CALL_INCOMING		1U
#define BTD_HFP_CALL_DIALING		2U
#define BTD_HFP_CALL_ALERTING		3U
#define BTD_HFP_CALL_ACTIVE		4U
#define BTD_HFP_CALL_HELD		5U
#define BTD_HFP_CALL_WAITING		6U

/* What the owner asks: dial, answer, hang up, hold (CHLD), a DTMF key, the speaker's volume, voice recognition. */
#define BTD_HFP_ASK_DIAL		1U
#define BTD_HFP_ASK_ANSWER		2U
#define BTD_HFP_ASK_HANGUP		3U
#define BTD_HFP_ASK_HOLD		4U
#define BTD_HFP_ASK_DTMF		5U
#define BTD_HFP_ASK_VOLUME		6U
#define BTD_HFP_ASK_VOICE		7U

/* The most calls followed, commands waiting, and the lengths of a number and a name kept (with their NULs). */
#define BTD_HFP_CALLS_MAX		2U
#define BTD_HFP_COMMANDS_MAX		12U
#define BTD_HFP_NUMBER_MAX		33U
#define BTD_HFP_NAME_MAX		64U
#define BTD_HFP_COMMAND_TEXT_MAX	48U

/*
 * The times (milliseconds): a command's answer, a dial's answer, a busy SDP
 * query or channel asked again, and the first and the last wait after a
 * failure.
 */
#define BTD_HFP_COMMAND_MS		5000U
#define BTD_HFP_DIAL_MS			20000U
#define BTD_HFP_BUSY_MS			2000U
#define BTD_HFP_RETRY_FIRST_MS		30000U
#define BTD_HFP_RETRY_LAST_MS		600000U

/*
 * What the HF asks of the daemon: as PBAP's hooks (pbap.h), with emit (an
 * event's line to the phone's subscribers).
 */
struct btd_hfp_hooks {
	void *context;
	uint64_t (*clock)(void *context);
	int (*wanted)(void *context);
	int (*sdp_query)(void *context, uint16_t uuid);
	int (*dlc_open)(void *context, unsigned server_channel);
	int (*dlc_write)(void *context, unsigned dlci, const uint8_t *data, size_t length, size_t *written);
	void (*dlc_close)(void *context, unsigned dlci);
	void (*answer)(void *context, uint64_t token, const char *line, const uint8_t *bytes, size_t length);
	void (*emit)(void *context, const char *line, const uint8_t *bytes, size_t length);
	void (*log)(void *context, const char *line);
};

/*
 * One call followed: its state, its index (the AG's, from AT+CLCC, or the
 * slot's), whether this side made it, its number and name, and what was
 * told last of it (its state, number and name), so that only changes are
 * told.
 */
struct btd_hfp_call {
	unsigned state;
	unsigned index;
	int outgoing;
	char number[BTD_HFP_NUMBER_MAX];
	char name[BTD_HFP_NAME_MAX];
	unsigned told_state;
	unsigned told_index;
	char told_number[BTD_HFP_NUMBER_MAX];
	char told_name[BTD_HFP_NAME_MAX];
};

/*
 * One command waiting or written: its text without the CR, the step of
 * the SLC it is (0: none), the owner's request's token (0: the HF's own),
 * and its answer's wait.
 */
struct btd_hfp_command {
	char text[BTD_HFP_COMMAND_TEXT_MAX];
	unsigned step;
	uint64_t token;
	unsigned timeout_ms;
};

/*
 * The HF of the phone link.  It lives in the daemon for the daemon's
 * life; a ready link starts it and the link's end stops it.  It holds its
 * hooks; its state, why it failed, whether the link is ready, the wait's
 * step and when it tries again, when a busy SDP query or channel is asked
 * again; the AG found (its channel, its features of SDP and of +BRSF);
 * the DLC (0: none), DLCs to close at the next tick and the lines being
 * read; the commands (the first written when written is 1, its answer
 * due at deadline) and the bytes of the one being written; the
 * indicators (their names in the AG's order, where call, callsetup,
 * callheld, service, signal, roam and battchg are, -1 for none, and their
 * values); the calls; the number of a RING not yet tied to a call; the
 * AT+CLCC list being read; the volume and voice recognition; and the
 * PHONE HF line told last.
 */
struct btd_hfp {
	struct btd_hfp_hooks hooks;

	/* The state, why it failed, the link, the waits. */
	unsigned state;
	const char *why;
	int link_ready;
	unsigned retry_step;
	uint64_t retry_at;
	uint64_t sdp_again_at;
	uint64_t open_again_at;

	/* The AG found. */
	unsigned ag_channel;
	uint32_t ag_sdp_features;
	uint32_t ag_features;

	/* The DLC (0: none), those to close at the next tick, the lines read. */
	unsigned dlci;
	unsigned closing_count;
	unsigned closing[4];
	struct btd_at_input input;

	/* The commands, a queue (the first is the one written), and the bytes of the one being written. */
	struct btd_hfp_command commands[BTD_HFP_COMMANDS_MAX];
	unsigned command_count;
	int written;
	uint64_t deadline;
	char out[BTD_HFP_COMMAND_TEXT_MAX + 1U];
	size_t out_length;
	size_t out_sent;

	/* The indicators. */
	char names[BTD_AT_INDICATORS_MAX][BTD_AT_NAME_MAX];
	unsigned indicator_count;
	int at_call;
	int at_callsetup;
	int at_callheld;
	int at_service;
	int at_signal;
	int at_roam;
	int at_battchg;
	long values[BTD_AT_INDICATORS_MAX];

	/* The calls, a RING's or +CLIP's number and name not yet tied to a call, and the AT+CLCC list being read. */
	struct btd_hfp_call calls[BTD_HFP_CALLS_MAX];
	char ring_number[BTD_HFP_NUMBER_MAX];
	char ring_name[BTD_HFP_NAME_MAX];
	int listing;
	struct btd_hfp_call listed[BTD_HFP_CALLS_MAX];
	unsigned listed_count;

	/* The speaker's volume (-1: not told), voice recognition, and the PHONE HF line told last. */
	int speaker;
	int voice;
	char told[256];
};

void btd_hfp_init(struct btd_hfp *hfp, const struct btd_hfp_hooks *hooks);
void btd_hfp_ready(void *context);
void btd_hfp_ended(void *context);
void btd_hfp_sdp_done(void *context, const struct btd_sdp *sdp, int error);
int btd_hfp_accept(void *context, unsigned server_channel);
void btd_hfp_opened(void *context, unsigned dlci, unsigned server_channel, int ours);
void btd_hfp_data(void *context, unsigned dlci, const uint8_t *data, size_t length);
void btd_hfp_writable(void *context, unsigned dlci);
void btd_hfp_closed(void *context, unsigned dlci, int reason);
void btd_hfp_open_failed(void *context, unsigned server_channel);
const char *btd_hfp_ask(struct btd_hfp *hfp, uint64_t token, unsigned ask, const char *argument);
void btd_hfp_cancel(struct btd_hfp *hfp, uint64_t token);
void btd_hfp_check(struct btd_hfp *hfp);
void btd_hfp_tick(struct btd_hfp *hfp, uint64_t now);
uint64_t btd_hfp_deadline(const struct btd_hfp *hfp);
const char *btd_hfp_state_word(const struct btd_hfp *hfp);
int btd_hfp_line(const struct btd_hfp *hfp, unsigned index, char *line, size_t size);

#endif
