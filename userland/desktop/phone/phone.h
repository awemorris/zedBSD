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
	PH_STATE_UNKNOWN,	/* a text the phone may or may not have sent (ws197-p004b) */
	PH_STATES
};

/*
 * One item of a timeline: its kind and channel, whether it went out, the
 * day and time it happened (as words, and as a time), its state, and its
 * text -- a message's words, a file's name, a picture's caption (NULL for
 * none) -- with a detail: a message's state ("Delivered"), a call's
 * length (NULL for a call not answered), a file's type and size.  path is
 * its file.  A message of the paired phone (ws197-p004b) has its source
 * ("bt:<address>:map:<key>", NULL for none), the other side's name as the
 * phone gave it (NULL for none), whether its time was not known
 * (partial) and whether its words were cut; extra holds the header's
 * lines this program does not know, written back as they were.  serial
 * names the item for this run of the program (its index may move).
 *
 * The strings are the store's (store.c), allocated for each item.
 */
#define PH_MEDIA_MAX 16U

/* A receive operation owns these permanent paths until released. */
struct ph_received {
	char *paths[PH_MEDIA_MAX];
	unsigned video[PH_MEDIA_MAX];
	size_t count;
};

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
	char *source;
	char *name;
	char *extra;
	int partial;
	int truncated;
	unsigned long serial;
	char *media[PH_MEDIA_MAX];
	unsigned media_video[PH_MEDIA_MAX];
	size_t media_count;
};

/*
 * One contact: its ID (the name of its file and of its folder of items),
 * the name, the number, the initials and color of the picture standing for
 * the person, the messages not read, and the timeline (oldest first; the
 * array allocated, item_capacity its room).  conversation is 1 for a
 * number's conversation without a contact's file (ws197-p004b): its ID is
 * its folder, "n" and the digits or "a" and the sender's bytes in
 * hexadecimal.
 */
struct ph_contact {
	int conversation;
	int phone_named;
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
#define PH_ACTION_SYNC		9U

/* The most requests the view queues for the window between two frames. */
#define PH_REQUESTS_MAX		8U

/* Something the view asks of the window: the action and the contact it is about. */
struct ph_request {
	unsigned action;
	long contact;
};

/*
 * One message of the paired phone (ws197-p004b) as libkeiland gives it:
 * the phone's address, its key (16 hexadecimal digits, or "-" when its
 * time was not known), whether it went out, its time, the other side's
 * number and name (empty for none), whether the phone has it read and cut
 * it, and its words.
 */
struct ph_phone_message {
	enum ph_channel channel;
	const char *address;
	const char *key;
	int outgoing;
	time_t date;
	const char *peer;
	const char *name;
	int read;
	int truncated;
	const char *text;
};

/* What the store made of a phone's message: a new file, one it had, or one of its own it had without the key (its source added). */
#define PH_MERGE_NEW		0
#define PH_MERGE_KNOWN		1
#define PH_MERGE_OVERLAID	2

/* The longest number key (ws197-p004 section 0), with its NUL, and the country code used when none is set. */
#define PH_NUMBER_KEY_MAX	264U
#define PH_COUNTRY_DEFAULT	"81"

/*
 * How far a paired phone was brought in (sync/bt-<address>.state,
 * ws197-p004 section 6.2 and ws197-p005 section 7.1): the messages since
 * a time and the last deep synchronisation, the last whole reading of the
 * phone's contacts, and the calls since a time (UNIX seconds, 0 for none).
 */
struct ph_sync_marks {
	int64_t messages_since;
	int64_t deep_at;
	int64_t contacts_at;
	int64_t calls_since;
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
 * until a time, whether the paired phone takes no text to send (the send
 * button grey, ws197-p004b), and whether the program is to end.
 */
/* A finite draft keeps ordinary file paths and stays bound to a stable contact ID. */
#define PH_DRAFT_MAX 16U
struct ph_attachment {
	char *path;
	char *contact;
	int video;
	int temporary;
};

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
	struct ph_attachment attachments[PH_DRAFT_MAX];
	size_t attachment_count;
	size_t attachment_page;
	int cannot_send;
	int quit;
};

/* The contacts and their timelines (store.c). */
const struct ph_contact *ph_contacts(size_t *count);
int ph_receive_media(struct kl_system *system, int descriptor, struct ph_received *received);
void ph_received_release(struct ph_received *received);
int ph_store_media(long contact, size_t item, const struct ph_received *received);
int ph_decode(const char *path, struct kl_image *image);
int ph_fit(const struct kl_image *picture, int side, struct kl_image *fitted);

int ph_store_open(const char *root);
void ph_store_close(void);
int ph_store_add_contact(const char *name, const char *number, long *index);
long ph_store_find_number(const char *number);
int ph_store_add_item(long contact, enum ph_kind kind, enum ph_channel channel, int outgoing, time_t date, enum ph_state state, const char *text, const char *detail, size_t *item);
int ph_store_set_state(long contact, size_t item, enum ph_state state, const char *detail);
int ph_store_mark_read(long contact);
const char *ph_channel_word(enum ph_channel channel);
void ph_store_set_country(const char *code);
int ph_number_key(const char *number, char *key, size_t size);
long ph_store_conversation(const char *number, const char *name, int create);
int ph_store_phone_message(const struct ph_phone_message *message, long *contact, size_t *item, int *merge);
int ph_store_find_serial(unsigned long serial, long *contact, size_t *item);
int ph_store_sync_load(const char *address, struct ph_sync_marks *marks);
int ph_store_sync_save(const char *address, const struct ph_sync_marks *marks);
int ph_phonebook_prune_plan(const char *const *current, size_t current_count, const char *const *received, size_t received_count, const char *const *missing, size_t missing_count, int complete, unsigned capped, unsigned char *remove, unsigned char *missing_next);
void ph_phonebook_sort_keys(const char **keys, size_t count);
int ph_phonebook_forget(const char *copy_address, const struct kl_phone_link *link);

/* The isolated imported phonebook and its UI-thread number index. */
int ph_phonebook_open(const char *root);
void ph_phonebook_close(void);
int ph_phonebook_reindex(void);
const char *ph_phonebook_name(const char *number);
int ph_phonebook_begin(void);
int ph_phonebook_put(const char *address, const struct kl_phone_item *item);
int ph_phonebook_end(const char *address, int complete, unsigned capped);
int ph_phonebook_link(const struct kl_phone_link *link);
const char *ph_store_phone_name(const char *number);
void ph_store_apply_phone_names(void);
long ph_store_withheld_conversation(int create);
int ph_store_phone_call(const char *address, const struct kl_phone_item *call);

/* Unsent media drafts and dropped URI/text data (media.c). */
int ph_draft_add(struct ph_view *view, const char *path, const char *contact, int temporary);
void ph_draft_remove(struct ph_view *view, size_t index);
void ph_draft_release(struct ph_view *view);
size_t ph_draft_count(const struct ph_view *view, const char *contact);
int ph_draft_text(struct ph_view *view, const char *text, size_t length);
int ph_draft_uris(struct ph_view *view, const char *text, size_t length, const char *contact);

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
