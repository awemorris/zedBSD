/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The applications' notifications as the compositor keeps them (notify.c,
 * ws156-p002, plan/ws156/phase001/phase.md sections 2, 3.3, 4 and 5): the
 * ones waiting to be shown, in the order they came, the one shown, and the
 * log, newest first.
 *
 * A notification is posted by a client (or by the compositor itself,
 * client 0) and gets a number of its own; a post that names an earlier
 * number of the same client replaces that one where it is (its words, in
 * the queue, on show or in the log).  A client has at most
 * KWL_NOTIFY_PER_CLIENT notifications waiting, shown and logged together.
 * At most KWL_NOTIFY_QUEUE wait; one more sends the oldest waiting straight
 * to the log.  The log keeps KWL_NOTIFY_LOG, the oldest going first
 * (expired).  A dismissed notification (its x) and one withdrawn are
 * dropped, not logged.  Clearing empties the log.
 *
 * It knows nothing of the server: the caller (notify-shell.c) tells the
 * clients what happened from the closed notifications it is given back.
 * So the host tests run it alone.
 */

#ifndef KWL_NOTIFY_H
#define KWL_NOTIFY_H

#include <stddef.h>
#include <stdint.h>

/* The longest application name, title and body, in bytes without the NUL (section 2). */
#define KWL_NOTIFY_APP_MAX	64U
#define KWL_NOTIFY_TITLE_MAX	128U
#define KWL_NOTIFY_BODY_MAX	512U

/* How many notifications a client may have, how many wait, and how many the log keeps. */
#define KWL_NOTIFY_PER_CLIENT	32U
#define KWL_NOTIFY_QUEUE	8U
#define KWL_NOTIFY_LOG		100U

/*
 * How fast one client may post (ws177-p005): at most KWL_NOTIFY_RATE_POSTS
 * new notifications or replacements in KWL_NOTIFY_RATE_MS; more in the
 * same span are refused as busy.  The rate is kept for the
 * KWL_NOTIFY_RATE_CLIENTS clients that posted last.
 */
#define KWL_NOTIFY_RATE_POSTS	10U
#define KWL_NOTIFY_RATE_MS	1000U
#define KWL_NOTIFY_RATE_CLIENTS	16U

/* Where a notification is. */
#define KWL_NOTIFY_WAITING	1U
#define KWL_NOTIFY_SHOWN	2U
#define KWL_NOTIFY_LOGGED	3U

/* Its flags (kl_system_notify_v1.post's): it shows over a fullscreen window; a click of its body is told to its client. */
#define KWL_NOTIFY_URGENT	0x1U
#define KWL_NOTIFY_ACTION	0x2U

/* Why one closed (kl_system_notify_v1.closed's reason). */
#define KWL_NOTIFY_DISMISSED	1U
#define KWL_NOTIFY_EXPIRED	2U
#define KWL_NOTIFY_CLEARED	3U
#define KWL_NOTIFY_WITHDRAWN	4U

/*
 * One notification: its number, its client (0: the compositor's) and the
 * client's object it came by, where it is and its place in its order (a
 * serial that grows: the waiting by when they came, the log by when they
 * entered it), its flags, and its words.  lock_text is what the lock
 * screen shows of it (ws197-p004c, "New message from <name>"; empty: the
 * lock screen shows nothing of it).
 */
struct kwl_notification {
	uint32_t id;
	uint64_t client;
	uint32_t object;
	unsigned where;
	uint64_t order;
	unsigned flags;
	char app[KWL_NOTIFY_APP_MAX + 1U];
	char title[KWL_NOTIFY_TITLE_MAX + 1U];
	char body[KWL_NOTIFY_BODY_MAX + 1U];
	char lock_text[KWL_NOTIFY_TITLE_MAX + 1U];
};

/* A notification that closed, for its client to be told. */
struct kwl_notify_closed {
	uint32_t id;
	uint64_t client;
	uint32_t object;
	unsigned reason;
};

/*
 * The posts of one client in the current span of the rate (ws177-p005):
 * the client (0: a free row), when its span began, and how many it posted
 * in it.
 */
struct kwl_notify_rate {
	uint64_t client;
	uint64_t start_ms;
	unsigned posts;
};

/*
 * The notifications: a table of every one kept (waiting, shown, logged),
 * the next number, the serial their order is taken from, and the rate of
 * the clients that posted last.
 */
struct kwl_notify_model {
	struct kwl_notification *items;
	size_t count;
	size_t capacity;
	uint32_t next_id;
	uint64_t serial;
	/* The rate of the clients that posted last (kwl_notify_rate_take). */
	struct kwl_notify_rate rates[KWL_NOTIFY_RATE_CLIENTS];
};

void kwl_notify_model_init(struct kwl_notify_model *model);
void kwl_notify_model_free(struct kwl_notify_model *model);
int kwl_notify_post(struct kwl_notify_model *model, uint64_t client, uint32_t object, uint32_t replaces, const char *app, const char *title,
		    const char *body, unsigned flags, uint32_t *id, struct kwl_notify_closed *closed, size_t *closed_count);
int kwl_notify_withdraw(struct kwl_notify_model *model, uint64_t client, uint32_t id, struct kwl_notify_closed *closed);
int kwl_notify_show_next(struct kwl_notify_model *model, uint32_t *id);
int kwl_notify_hide(struct kwl_notify_model *model, struct kwl_notify_closed *closed, size_t *closed_count);
int kwl_notify_dismiss(struct kwl_notify_model *model, uint32_t id, struct kwl_notify_closed *closed);
size_t kwl_notify_clear(struct kwl_notify_model *model, struct kwl_notify_closed *closed, size_t capacity);
const struct kwl_notification *kwl_notify_shown(const struct kwl_notify_model *model);
size_t kwl_notify_log(const struct kwl_notify_model *model, const struct kwl_notification **log, size_t capacity);
size_t kwl_notify_waiting(const struct kwl_notify_model *model);
const struct kwl_notification *kwl_notify_find(const struct kwl_notify_model *model, uint32_t id);
size_t kwl_notify_orphan(struct kwl_notify_model *model, uint64_t client, uint32_t object);
int kwl_notify_rate_take(struct kwl_notify_model *model, uint64_t client, uint64_t now_ms);
int kwl_notify_set_lock_text(struct kwl_notify_model *model, uint32_t id, const char *text);
size_t kwl_notify_clean(char *out, size_t room, const char *text, int lines);

#endif
