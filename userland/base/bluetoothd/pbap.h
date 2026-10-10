/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Phone Book Access Profile's client, the PCE (ws197-p005,
 * plan/ws197/phase005/phase.md section 5): bluetoothd reads the phone's
 * phone book (telecom/pb.vcf) and its call history (telecom/ich.vcf,
 * och.vcf, mch.vcf: calls taken, made and not answered) for the phone's
 * owner, a page at a time, and keeps nothing of them.
 *
 * Set up when the phone link is ready and the owner wants contacts: the
 * SDP query for the phone's PSE, a DLC to its RFCOMM channel, OBEX's
 * Connect with PBAP's Target (PBAP 1.1 over GOEP 1.1, no application
 * parameters, Q16 (a)), whose answer may wait a minute while the phone
 * asks its user.  The phone may close the idle connection; it is opened
 * again by the next page.  A refusal (the phone's DM or DISC before the
 * connection was ever ready, or Connect's Unauthorized or Forbidden) or
 * an unanswered prompt waits ten minutes before the next try; another
 * failure waits 30 s, doubling to ten minutes.
 *
 * A page (PHONE PAGE contacts|calls) is one PullPhoneBook of up to 32
 * cards from a cursor: its items are written to the client as
 * PHONE CONTACT or PHONE CALL-LOG lines, then PHONE PAGE-END with the
 * next cursor, then DONE.  The cursor names the phone record's
 * generation, the object, the offset and the object's size at the start
 * of the pass; it is not tied to the OBEX connection (an offset names a
 * card of the phone book whatever the connection).
 *
 * Without system calls; the host tests build it.
 */

#ifndef BLUETOOTHD_PBAP_H
#define BLUETOOTHD_PBAP_H

#include "userland/base/bluetoothd/obex.h"
#include "userland/base/bluetoothd/sdp.h"

#include <stddef.h>
#include <stdint.h>

/* What a page reads: the phone book, or the call history. */
#define BTD_PBAP_WHAT_CONTACTS		1U
#define BTD_PBAP_WHAT_CALLS		2U

/* The objects: the phone book, the calls taken, made, and not answered. */
#define BTD_PBAP_OBJECT_PB		0U
#define BTD_PBAP_OBJECT_ICH		1U
#define BTD_PBAP_OBJECT_OCH		2U
#define BTD_PBAP_OBJECT_MCH		3U
#define BTD_PBAP_OBJECTS		4U

/*
 * The most pages running, cards a page asks for, contacts a pass reads
 * (after the owner's card), and calls of each object.
 */
#define BTD_PBAP_PAGES_MAX		2U
#define BTD_PBAP_COUNT_MAX		32U
#define BTD_PBAP_CONTACTS_MAX		5000U
#define BTD_PBAP_CALLS_MAX		500U

/* The most one Get's body is (a larger one is asked again for half the cards). */
#define BTD_PBAP_BODY_MAX		262144U

/* An object's size not known. */
#define BTD_PBAP_SIZE_UNKNOWN		0xffffffffU

/*
 * The bits of a page's end (PHONE PAGE-END's capped): stopped at the
 * limit, the phone book's size changed during the pass, a card of this
 * page did not end (the count of the page cannot be trusted).
 */
#define BTD_PBAP_CAPPED_LIMIT		1U
#define BTD_PBAP_CAPPED_CHANGED		2U
#define BTD_PBAP_CAPPED_UNTRUSTED	4U

/*
 * The times (milliseconds): Connect's answer (the phone asks its user),
 * a busy SDP query or channel asked again, a client that does not read,
 * the first and the last wait after a failure, and the longest a page
 * waits for the connection to be ready.
 */
#define BTD_PBAP_CONNECT_MS		60000U
#define BTD_PBAP_BUSY_MS		2000U
#define BTD_PBAP_SLOW_MS		30000U
#define BTD_PBAP_RETRY_FIRST_MS		30000U
#define BTD_PBAP_RETRY_LAST_MS		600000U
#define BTD_PBAP_PAGE_CONNECT_MS	100000U

/* The room a client's queue must have before the next item is written to it. */
#define BTD_PBAP_ROOM_MIN		32768

/*
 * The states: off; the SDP query; the DLC being opened; OBEX's Connect;
 * ready; idle (the phone closed the connection, the next page opens it
 * again; told as ready); failed, waiting to try again.
 */
#define BTD_PBAP_OFF			0U
#define BTD_PBAP_SDP			1U
#define BTD_PBAP_OPENING		2U
#define BTD_PBAP_CONNECTING		3U
#define BTD_PBAP_READY			4U
#define BTD_PBAP_IDLE			5U
#define BTD_PBAP_FAILED			6U

/*
 * What PBAP asks of the daemon: as MAP's hooks (map.h), without emit (the
 * phone book has no events), with address: the phone's address on the
 * link now (0, or ENOENT), whose change makes the cursors of the earlier
 * phone stale.
 */
struct btd_pbap_hooks {
	void *context;
	uint64_t (*clock)(void *context);
	int64_t (*wall)(void *context);
	int32_t (*local_offset)(void *context, int64_t seconds);
	int (*wanted)(void *context);
	int (*address)(void *context, uint8_t *address);
	int (*sdp_query)(void *context, uint16_t uuid);
	int (*dlc_open)(void *context, unsigned server_channel);
	int (*dlc_write)(void *context, unsigned dlci, const uint8_t *data, size_t length, size_t *written);
	void (*dlc_close)(void *context, unsigned dlci);
	void (*answer)(void *context, uint64_t token, const char *line, const uint8_t *bytes, size_t length);
	long (*room)(void *context, uint64_t token);
	void (*changed)(void *context);
	void (*up)(void *context);
	void (*log)(void *context, const char *line);
};

/*
 * One PAGE running: its client (token), what it reads and since when
 * (calls), the object, offset and the object's size at the pass's start
 * it reads from, the cards asked for (count) and in the Get now (ask,
 * halved for a body too large), its step and when it came (for the
 * connection's wait), what it gave and left out and its end's bits; the
 * body's cards being written to its client (where the next starts, the
 * index of that card in the object, how many cards the body has),
 * whether it waits for its client to read (since when), whether its
 * client went, and where the next page starts once this one ends.
 */
struct btd_pbap_page {
	int used;
	uint32_t serial;
	uint64_t token;
	unsigned what;
	int64_t since;
	unsigned object;
	unsigned offset;
	unsigned start_size;
	unsigned count;
	unsigned ask;
	unsigned step;
	uint64_t arrived;
	unsigned given;
	unsigned skipped;
	unsigned capped;
	size_t body_at;
	unsigned card_index;
	unsigned body_cards;
	int waiting;
	uint64_t waiting_since;
	int cancelled;
	unsigned next_object;
	unsigned next_offset;
	unsigned next_size;
	int more;
};

/*
 * The PBAP client of the phone link.  It lives in the daemon for the
 * daemon's life; a ready link starts it and the link's end stops it.  It
 * holds its hooks; its state, why it failed, whether the link is ready,
 * the wait's step and when it tries again, when a busy SDP query or a
 * busy channel is asked again, whether this connection ever became ready; the
 * PSE found (its channel, whether it has a phone book, whether its
 * record names its features); the DLC (0: none), DLCs to close at the
 * next tick and the OBEX connection; the generation the cursors name and
 * the phone it belongs to; the pages; the operation running; what OBEX
 * told that is still to be handled; and the body being received.
 */
struct btd_pbap {
	struct btd_pbap_hooks hooks;

	/* The state, why it failed, the link, the waits, whether this attempt reached ready. */
	unsigned state;
	const char *why;
	int link_ready;
	unsigned retry_step;
	uint64_t retry_at;
	uint64_t sdp_again_at;
	uint64_t open_again_at;
	int reached_ready;

	/* The PSE found. */
	unsigned pse_channel;
	uint32_t repositories;
	int has_features;

	/* The DLC (0: none), those to close at the next tick, the OBEX connection. */
	unsigned dlci;
	unsigned closing_count;
	unsigned closing[4];
	struct btd_obex obex;

	/* The generation of the cursors, and the phone it belongs to (have_phone 0: none yet). */
	uint32_t generation;
	int have_phone;
	uint8_t phone[6];

	/* The pages, and the serial the next takes (the oldest runs first). */
	struct btd_pbap_page pages[BTD_PBAP_PAGES_MAX];
	uint32_t next_serial;

	/* The operation running: whether one runs, its kind, its page. */
	int running;
	unsigned op;
	unsigned op_page;

	/* What OBEX noted: an operation's end (its error and code), the size an answer gave. */
	int finished;
	int finished_error;
	uint8_t finished_code;
	int have_size;
	unsigned size;

	/* The body being received. */
	size_t body_length;
	uint8_t body[BTD_PBAP_BODY_MAX];

	/* The counters: cards read, cards left out, pages ended. */
	unsigned cards;
	unsigned left_out;
	unsigned pages_done;
};

void btd_pbap_init(struct btd_pbap *pbap, const struct btd_pbap_hooks *hooks, uint32_t first_generation);
void btd_pbap_ready(void *context);
void btd_pbap_ended(void *context);
void btd_pbap_sdp_done(void *context, const struct btd_sdp *sdp, int error);
int btd_pbap_accept(void *context, unsigned server_channel);
void btd_pbap_opened(void *context, unsigned dlci, unsigned server_channel, int ours);
void btd_pbap_data(void *context, unsigned dlci, const uint8_t *data, size_t length);
void btd_pbap_writable(void *context, unsigned dlci);
void btd_pbap_closed(void *context, unsigned dlci, int reason);
void btd_pbap_open_failed(void *context, unsigned server_channel);
const char *btd_pbap_page(struct btd_pbap *pbap, uint64_t token, unsigned what, int64_t since, const char *cursor, unsigned count);
void btd_pbap_cancel(struct btd_pbap *pbap, uint64_t token);
void btd_pbap_check(struct btd_pbap *pbap);
void btd_pbap_pump(struct btd_pbap *pbap);
void btd_pbap_tick(struct btd_pbap *pbap, uint64_t now);
uint64_t btd_pbap_deadline(const struct btd_pbap *pbap);
int btd_pbap_state_text(const struct btd_pbap *pbap, char *text, size_t size);

#endif
