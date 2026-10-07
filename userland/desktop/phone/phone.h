/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Phone (WS170): the application that puts a person's messages (SMS, MMS,
 * RCS) and calls (the line's and VoIP) in one timeline.
 *
 * The contacts and their timelines are kept under ~/Documents/Phone, one
 * file for each contact and for each item (store.c, plan/ws170/phase001/
 * phase.md section 1), so that a folder synchronized with the cloud keeps
 * them without conflicts.  Messages go out and come in, and calls are
 * made, through the compositor's phone (kl_system_phone_*, WS170 p004),
 * whose backend the desktop chooses.
 *
 * The view (view.c) draws a frame with libkeiland's canvas and widgets and
 * knows nothing of the window or the compositor -- what the user asks of
 * the phone it queues as requests the window takes -- so that the host
 * tests draw it into a picture; the window (main.c) feeds it the input.
 */

#ifndef PHONE_PHONE_H
#define PHONE_PHONE_H

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* What a timeline item is: a message, a call, a picture or another file. */
enum ph_kind {
	PH_TEXT,
	PH_CALL,
	PH_PHOTO,
	PH_FILE
};

/* The way an item went: the carrier's messages, RCS, the line's call, or a VoIP call. */
enum ph_channel {
	PH_SMS,
	PH_MMS,
	PH_RCS,
	PH_LINE,
	PH_VOIP
};

/* An item's state, as its file keeps it (State:). */
enum ph_state {
	PH_STATE_NONE,
	PH_STATE_UNREAD,
	PH_STATE_READ,
	PH_STATE_SENDING,
	PH_STATE_SENT,
	PH_STATE_DELIVERED,
	PH_STATE_FAILED,
	PH_STATE_ANSWERED,
	PH_STATE_MISSED,
	PH_STATE_NO_ANSWER,
	PH_STATES
};

/*
 * One item of a timeline: its kind and channel, whether it went out, the
 * day and time it happened (as words, and as a time), its state, and its
 * text -- a message's words, a file's name, a picture's caption (NULL for
 * none) -- with a detail: a message's state ("Delivered"), a call's
 * length (NULL for a call not answered), a file's type and size.  path is
 * its file.
 *
 * The strings are the store's (store.c), allocated for each item.
 */
struct ph_item {
	enum ph_kind kind;
	enum ph_channel channel;
	int outgoing;
	time_t date;
	enum ph_state state;
	char *day;
	char *time;
	char *text;
	char *detail;
	char *path;
};

/*
 * One contact: its ID (the name of its file and of its folder of items),
 * the name, the number, the initials and color of the picture standing for
 * the person, the messages not read, and the timeline (oldest first; the
 * array allocated, item_capacity its room).
 */
struct ph_contact {
	char *id;
	char *name;
	char *number;
	char initials[8];
	kl_color color;
	unsigned unread;
	struct ph_item *items;
	size_t item_count;
	size_t item_capacity;
};

/* The actions of the menu, the keys and the buttons. */
#define PH_ACTION_SEND		1U
#define PH_ACTION_CALL		2U
#define PH_ACTION_ATTACH	3U
#define PH_ACTION_QUIT		4U
#define PH_ACTION_ADD		5U	/* the form of a new contact */
#define PH_ACTION_SAVE		6U	/* the new contact saved */
#define PH_ACTION_READ		7U	/* a contact's messages read */
#define PH_ACTION_CANCEL	8U

/* The most requests the view queues for the window between two frames. */
#define PH_REQUESTS_MAX		8U

/* Something the view asks of the window: the action and the contact it is about. */
struct ph_request {
	unsigned action;
	long contact;
};

/* The longest the contacts' filter keeps, with its NUL. */
#define PH_FILTER_MAX		64U

/*
 * The view's state: the search field and its filter, the message being
 * written, the scrolls of the contacts and of the timeline, the contact
 * shown (-1 for none), whether a narrow window shows the timeline instead
 * of the contacts (and whether the last frame was narrow), whether the
 * view stands on the compositor's glass (cards with the desktop between), whether the
 * timeline goes to its end at the next frame, the contacts whose messages
 * were read (a bit each of the first 32), the notice shown at the bottom
 * until a time, and whether the program is to end.
 */
struct ph_view {
	struct kl_field search;
	struct kl_field message;
	struct kl_scroll contacts_scroll;
	struct kl_scroll timeline_scroll;
	long selected;
	int opened;
	int narrow;
	int glass;
	int to_end;
	const char *notice;
	char notice_text[160];
	uint64_t notice_until;
	int adding;
	struct kl_field new_name;
	struct kl_field new_number;
	struct ph_request requests[PH_REQUESTS_MAX];
	size_t request_count;
	int quit;
};

/* The contacts and their timelines (store.c). */
const struct ph_contact *ph_contacts(size_t *count);
int ph_store_open(const char *root);
void ph_store_close(void);
int ph_store_add_contact(const char *name, const char *number, long *index);
long ph_store_find_number(const char *number);
int ph_store_add_item(long contact, enum ph_kind kind, enum ph_channel channel, int outgoing, time_t date, enum ph_state state, const char *text, const char *detail, size_t *item);
int ph_store_set_state(long contact, size_t item, enum ph_state state, const char *detail);
int ph_store_mark_read(long contact);
const char *ph_channel_word(enum ph_channel channel);

/* The view (view.c). */
int ph_view_init(struct ph_view *view);
void ph_view_release(struct ph_view *view);
void ph_view_select(struct ph_view *view, long index);
void ph_view_action(struct ph_view *view, unsigned action, uint64_t now_us);
void ph_view_key(struct ph_view *view, uint32_t key, unsigned modifiers, uint64_t now_us);
void ph_view_draw(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, uint64_t now_us);
size_t ph_view_panels(struct ph_view *view, int width, int height, struct kl_glass_panel *panels, size_t capacity);
int ph_view_wait(const struct ph_view *view, uint64_t now_us);
int ph_view_take_request(struct ph_view *view, struct ph_request *request);
void ph_view_notice(struct ph_view *view, const char *message, uint64_t now_us);

/* The log for the tests (main.c, and the host tests' own). */
void ph_log(const char *format, ...);

#endif
