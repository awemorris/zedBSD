/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's Message Access Profile, the client side (MCE) (ws197-p003,
 * plan/ws197/phase003/phase.md section 8): on the phone's link, the
 * phone's Message Access Server (MAS) for SMS, reached over an RFCOMM DLC
 * with OBEX, and bluetoothd's own Message Notification Server (MNS), which
 * the phone connects to and pushes its events to.
 *
 * Once the link is ready and the owner wants messages, it finds the MAS by
 * SDP, connects (MAP 1.1, GOEP 1.1 over RFCOMM), sets the folder to
 * telecom/msg and registers for notifications.  Then it runs one
 * operation at a time from a queue: the pages of the first synchronisation
 * (PHONE PAGE), marking a message read (PHONE READ), sending (PHONE SEND),
 * and fetching each message an event announces, which goes before the
 * pages.  What it answers and announces goes out as lines through its
 * hooks; the lines are those of section 9.
 *
 * It keeps no message: what it reads goes out at once.  Without system
 * calls: the daemon hands in the DLCs' bytes and the profile's events and
 * gives the clocks through hooks; the host tests build it.
 */

#ifndef BLUETOOTHD_MAP_H
#define BLUETOOTHD_MAP_H

#include "userland/base/bluetoothd/bmsg.h"
#include "userland/base/bluetoothd/mapxml.h"
#include "userland/base/bluetoothd/obex.h"
#include "userland/base/bluetoothd/sdp.h"

#include <stddef.h>
#include <stdint.h>

/* How many operations wait at most, how many PAGE requests run at once, and how many announced messages are fetched at once. */
#define BTD_MAP_QUEUE_MAX		24U
#define BTD_MAP_PAGES_MAX		2U
#define BTD_MAP_LIVE_MAX		8U

/* How many of its own sent messages it follows, and how many events wait for a PushMessage's answer. */
#define BTD_MAP_SENT_MAX		32U
#define BTD_MAP_HELD_MAX		8U

/* How many events of the MNS wait to be handled, and how many DLCs wait to be closed. */
#define BTD_MAP_EVENTS_MAX		8U
#define BTD_MAP_CLOSING_MAX		4U

/* The most messages one page asks for, a folder gives in a synchronisation, and a search for an announced message lists. */
#define BTD_MAP_PAGE_COUNT_MAX		32U
#define BTD_MAP_FOLDER_LIMIT		500U
#define BTD_MAP_LOCATE_COUNT		32U

/* The longest body of a listing or a message taken. */
#define BTD_MAP_BODY_MAX		65536U

/*
 * The times (milliseconds): the wait for Connect's answer while the phone
 * asks its user, the wait for a busy SDP query, the wait for the MNS to
 * come back, how long a page waits for its client to read, how long an
 * event waits for a PushMessage's answer, the wait before an announced
 * message is searched again, and the waits before setting up again after
 * a failure (doubling to the longest, the longest after a refusal).
 */
#define BTD_MAP_CONNECT_MS		60000U
#define BTD_MAP_BUSY_MS			2000U
#define BTD_MAP_MNS_MS			30000U
#define BTD_MAP_SLOW_MS			30000U
#define BTD_MAP_HELD_MS			10000U
#define BTD_MAP_LOCATE_AGAIN_MS		2000U
#define BTD_MAP_RETRY_FIRST_MS		30000U
#define BTD_MAP_RETRY_LAST_MS		600000U

/* How much room a page's client must have before the next message is fetched for it. */
#define BTD_MAP_ROOM_MIN		32768

/* How far back an announced message is searched (seconds). */
#define BTD_MAP_LOCATE_SECONDS		3600

/*
 * The states: off (no ready link, or messages not wanted), finding the
 * MAS by SDP, its DLC asked for, OBEX's Connect sent, the folder being
 * set, the notifications being registered, ready, and failed (set up
 * again at retry_at, or never).
 */
#define BTD_MAP_OFF			0U
#define BTD_MAP_SDP			1U
#define BTD_MAP_OPENING			2U
#define BTD_MAP_CONNECTING		3U
#define BTD_MAP_SETPATH			4U
#define BTD_MAP_REGISTERING		5U
#define BTD_MAP_READY			6U
#define BTD_MAP_FAILED			7U

/* The operations of the queue (section 8.4). */
#define BTD_MAP_OP_NONE			0U
#define BTD_MAP_OP_COUNT		1U
#define BTD_MAP_OP_LIST			2U
#define BTD_MAP_OP_LOCATE		3U
#define BTD_MAP_OP_GET			4U
#define BTD_MAP_OP_UNREAD		5U
#define BTD_MAP_OP_READ			6U
#define BTD_MAP_OP_PUSH			7U
#define BTD_MAP_OP_REGISTER		8U

/* The folders a synchronisation reads, in order: the inbox, then the sent folder. */
#define BTD_MAP_FOLDER_INBOX		0U
#define BTD_MAP_FOLDER_SENT		1U
#define BTD_MAP_FOLDERS			2U

/* No page or announced message (an operation's index of either). */
#define BTD_MAP_NONE			0xffffffffU

/*
 * What MAP asks of the daemon.  clock gives the monotonic milliseconds,
 * wall the time of day in seconds since 1970 UTC, local_offset zedBSD's
 * zone at that time (seconds east of UTC).  wanted says whether the
 * phone's owner wants messages.  sdp_query, dlc_open, dlc_write and
 * dlc_close are the phone link's (the answers come back to btd_map_*).
 * answer writes a line (without its newline) and the bytes after it to a
 * request's client, named by its token; emit writes them to the
 * subscribers; room tells how many bytes a client's queue still takes
 * (-1: the client is gone).  changed says the state that PHONE SHOW tells
 * has changed; up says MAP became ready (the phone link's waits start
 * again from the first); log writes a line of the daemon's log.
 */
struct btd_map_hooks {
	void *context;
	uint64_t (*clock)(void *context);
	int64_t (*wall)(void *context);
	int32_t (*local_offset)(void *context, int64_t seconds);
	int (*wanted)(void *context);
	int (*sdp_query)(void *context, uint16_t uuid);
	int (*dlc_open)(void *context, unsigned server_channel);
	int (*dlc_write)(void *context, unsigned dlci, const uint8_t *data, size_t length, size_t *written);
	void (*dlc_close)(void *context, unsigned dlci);
	void (*answer)(void *context, uint64_t token, const char *line, const uint8_t *bytes, size_t length);
	void (*emit)(void *context, const char *line, const uint8_t *bytes, size_t length);
	long (*room)(void *context, uint64_t token);
	void (*changed)(void *context);
	void (*up)(void *context);
	void (*log)(void *context, const char *line);
};

/*
 * One operation of the queue: its kind, the client it answers (token 0:
 * none), the page or announced message it serves (BTD_MAP_NONE: neither),
 * whether it is an announced message's (those go first), its folder,
 * offset and count (for a page's message, offset is its entry), the
 * message's handle, whether a filter's time was written with zedBSD's
 * zone because the phone's was not known yet (and that zone), and for
 * PushMessage its request number and its bMessage (allocated, freed when
 * the operation ends).
 */
struct btd_map_op {
	unsigned kind;
	uint64_t token;
	unsigned page;
	unsigned live;
	int urgent;
	unsigned folder;
	unsigned offset;
	unsigned count;
	uint64_t handle;
	int guessed;
	int32_t guessed_offset;
	unsigned request;
	uint8_t *message;
	size_t message_length;
};

/*
 * One PAGE request running: its client, its since and the folder,
 * offset and count of this page; the entries of its listing and the next
 * one to fetch; what it gave and left out; whether its count was asked
 * again with the phone's zone; whether it waits for its client to read
 * (since when); and whether its client went (the operation running for it
 * ends quietly).
 */
struct btd_map_page {
	int used;
	uint64_t token;
	int64_t since;
	unsigned folder;
	unsigned offset;
	unsigned count;
	size_t listed;
	size_t entry_count;
	size_t next_entry;
	unsigned given;
	unsigned skipped;
	int recounted;
	int waiting;
	uint64_t waiting_since;
	int cancelled;
	struct btd_map_entry entries[BTD_MAP_PAGE_COUNT_MAX];
};

/*
 * One message an event announced, being fetched: its handle and folder,
 * whether the search found it (with its listing entry), how many times it
 * was searched, when it is searched again (0: not waiting), and whether
 * its listing said it was unread.
 */
struct btd_map_live {
	int used;
	uint64_t handle;
	unsigned folder;
	int found;
	unsigned searches;
	uint64_t search_at;
	struct btd_map_entry entry;
};

/*
 * A message bluetoothd sent, followed for its events: its handle, the
 * request that sent it, and the states told already (bits: sent,
 * delivered, failed).
 */
struct btd_map_sent {
	int used;
	uint64_t handle;
	unsigned request;
	unsigned told;
};

/* An event of a handle not known yet, held until a PushMessage's answer names it or it expires. */
struct btd_map_held {
	int used;
	uint64_t until;
	struct btd_map_event event;
};

/*
 * The MAP client of the phone link.  It lives in the daemon for the
 * daemon's life; a ready link starts it and the link's end stops it.  It
 * holds: its hooks; its state, why it failed and when it tries again;
 * the MAS found (its RFCOMM channel, instance, features, message types);
 * the two DLCs (0: none) and their OBEX connections; the MAP session (its
 * number, which names the handles of this connection) and the phone's
 * zone from MSETime; whether notifications and sending are on; the
 * listing sizes counted this session; the queue and the operation
 * running; what OBEX told that is still to be handled; the body being
 * received and the room its listing or bMessage is read into; the pages,
 * announced messages, sent messages and held events; the events of the
 * MNS waiting; and its counters.
 */
struct btd_map {
	struct btd_map_hooks hooks;

	/*
	 * The state, why it failed, whether the phone link is ready, the step
	 * of the wait after failures and when MAP is set up again, when a busy
	 * SDP query is asked again, and the step of SetPath.
	 */
	unsigned state;
	const char *why;
	int link_ready;
	unsigned retry_step;
	uint64_t retry_at;
	uint64_t sdp_again_at;
	unsigned setpath_step;

	/* The MAS found: its RFCOMM channel, its instance, its MapSupportedFeatures, its SupportedMessageTypes. */
	unsigned mas_channel;
	uint32_t mas_instance;
	uint32_t features;
	uint32_t message_types;

	/*
	 * The DLCs (0: none) and those to close at the next tick, whether the
	 * phone's OBEX connected on the MNS,
	 * until when the MNS is awaited after it went (0: not awaited), and the
	 * two OBEX connections.
	 */
	unsigned mas_dlci;
	unsigned mns_dlci;
	unsigned closing_count;
	unsigned closing[BTD_MAP_CLOSING_MAX];
	int mns_connected;
	uint64_t mns_wait_until;
	struct btd_obex mas;
	struct btd_obex mns;

	/*
	 * The MAP session (its number names the handles of this connection),
	 * the phone's zone from MSETime when known, and whether notifications
	 * and sending are on.
	 */
	uint32_t session;
	int mse_known;
	int32_t mse_offset;
	int notify;
	int send;

	/* The size of each folder's listing counted this session, and for which time. */
	int sizes_known[BTD_MAP_FOLDERS];
	int64_t sizes_since[BTD_MAP_FOLDERS];
	unsigned sizes[BTD_MAP_FOLDERS];

	/* The queue, and the operation running (and whether one runs). */
	unsigned queue_count;
	struct btd_map_op queue[BTD_MAP_QUEUE_MAX];
	int running;
	struct btd_map_op current;

	/*
	 * What OBEX noted for map_settle: a request that finished (its error
	 * and code), what its answer said (the listing's size, the phone's
	 * zone, a sent message's handle), and a failure to handle (its kind of
	 * wait).
	 */
	int finished;
	int finished_error;
	uint8_t finished_code;
	int have_size;
	unsigned listing_size;
	int have_mse;
	int32_t mse_answer_offset;
	int have_name;
	uint64_t name_handle;
	const char *failing;
	unsigned failing_kind;

	/* The body being received, and the room a bMessage or a search's listing is read into. */
	size_t body_length;
	uint8_t body[BTD_MAP_BODY_MAX];
	struct btd_bmsg message;
	struct btd_map_entry located[BTD_MAP_LOCATE_COUNT];

	/*
	 * The pages, the announced messages, the sent messages followed (and
	 * the slot the next takes), the held events, and the last request
	 * number given.
	 */
	struct btd_map_page pages[BTD_MAP_PAGES_MAX];
	struct btd_map_live lives[BTD_MAP_LIVE_MAX];
	struct btd_map_sent sents[BTD_MAP_SENT_MAX];
	unsigned sent_next;
	struct btd_map_held helds[BTD_MAP_HELD_MAX];
	unsigned next_request;

	/* The MNS's events waiting for map_settle, and whether some were lost to a full list. */
	unsigned event_count;
	struct btd_map_event events[BTD_MAP_EVENTS_MAX];
	int events_dropped;

	/* The counters: announced messages dropped, the ways texts were found. */
	unsigned dropped;
	unsigned form_counts[5];
};

void btd_map_init(struct btd_map *map, const struct btd_map_hooks *hooks, uint32_t first_session);
void btd_map_ready(void *context);
void btd_map_ended(void *context);
void btd_map_sdp_done(void *context, const struct btd_sdp *sdp, int error);
int btd_map_accept(void *context, unsigned server_channel);
void btd_map_opened(void *context, unsigned dlci, unsigned server_channel, int ours);
void btd_map_data(void *context, unsigned dlci, const uint8_t *data, size_t length);
void btd_map_writable(void *context, unsigned dlci);
void btd_map_closed(void *context, unsigned dlci, int reason);
void btd_map_open_failed(void *context, unsigned server_channel);
const char *btd_map_page(struct btd_map *map, uint64_t token, int64_t since, const char *cursor, unsigned count);
const char *btd_map_read(struct btd_map *map, uint64_t token, const char *handle);
const char *btd_map_send(struct btd_map *map, uint64_t token, const char *number, const uint8_t *text, size_t length);
void btd_map_cancel(struct btd_map *map, uint64_t token);
void btd_map_check(struct btd_map *map);
void btd_map_pump(struct btd_map *map);
void btd_map_tick(struct btd_map *map, uint64_t now);
uint64_t btd_map_deadline(const struct btd_map *map);
int btd_map_state_text(const struct btd_map *map, char *text, size_t size);

#endif
