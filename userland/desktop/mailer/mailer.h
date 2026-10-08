/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail (WS169): Keiland's mail application.
 *
 * The accounts are kept on the disk (account.c, the passwords in
 * secret.c), their messages in memory while the program runs (store.c),
 * and a thread of their own talks to the servers (sync.c, with the
 * backend of mail.h): it gets the folders' latest messages, waits for new
 * ones with IDLE, sends, marks read and moves.
 *
 * The view (view.c) draws a frame with libkeiland's canvas and widgets and
 * knows nothing of the window or the servers -- what the user asks of the
 * servers it queues as requests the window takes (ml_view_take_request),
 * so that the host tests draw it into pictures; the window (main.c) feeds
 * it the input and the thread's results.
 */

#ifndef MAILER_MAILER_H
#define MAILER_MAILER_H

#include "mail.h"
#include "sync.h"

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* The longest path of a file, with its NUL. */
#define ML_PATH_MAX		1024U

/*
 * One message as the view shows it: its account and folder (ML_FOLDERS
 * for one moved away and no longer shown), what it is (ML_*), its UID in
 * the folder (0 for one not on a server), its date, the sender's name,
 * address and color, to whom and in copy, its subject, its date as words
 * (short for the list, long for the reader), its words, the file it
 * carries ("name" and "type, size"; NULL for none), a sign-in code (NULL
 * for none), and its Message-ID (for a reply).
 *
 * The strings are the store's (store.c), allocated for each message.
 */
struct ml_message {
	int account;
	enum ml_folder folder;
	unsigned flags;
	uint32_t uid;
	time_t date;
	char *from_name;
	char *from_address;
	kl_color color;
	char *to;
	char *cc;
	char *subject;
	char *date_short;
	char *date_long;
	char *body;
	char *file_name;
	char *file_detail;
	char *code;
	char *message_id;
};

/* The actions of the menu, the keys and the buttons. */
#define ML_ACTION_NEW		1U
#define ML_ACTION_SEND		2U
#define ML_ACTION_REPLY		3U
#define ML_ACTION_REPLY_ALL	4U
#define ML_ACTION_FORWARD	5U
#define ML_ACTION_GET		6U
#define ML_ACTION_ARCHIVE	7U
#define ML_ACTION_DELETE	8U
#define ML_ACTION_CANCEL	9U
#define ML_ACTION_QUIT		10U
#define ML_ACTION_ADD_ACCOUNT	11U
#define ML_ACTION_SIGN_IN	12U
#define ML_ACTION_SEEN		13U
#define ML_ACTION_CODES		14U
#define ML_ACTION_EDIT_ACCOUNT	15U	/* the form with the account shown (ws177-p015) */
#define ML_ACTION_ASK_REMOVE	16U	/* the form's Remove Account: asks first */
#define ML_ACTION_REMOVE_ACCOUNT	17U	/* the account edited removed (the request's message is its index) */
#define ML_ACTION_TRUST		18U	/* the answer about a certificate (the request's message: 1 trusted, 0 not) */

/* The questions the view asks over the window (ws177-p015). */
#define ML_QUESTION_NONE	0U
#define ML_QUESTION_TRUST	1U	/* whether to trust a server's certificate that does not verify */
#define ML_QUESTION_REMOVE	2U	/* whether to remove the account edited */

/* The longest message body quoted in a reply, with its NUL. */
#define ML_BODY_MAX		8192U

/* The most requests the view queues for the window between two frames. */
#define ML_REQUESTS_MAX		8U

/*
 * Something the view asks of the servers (ML_ACTION_SEND, _GET, _ARCHIVE,
 * _DELETE, _SIGN_IN, _SEEN, _CODES, _REMOVE_ACCOUNT, _TRUST), for the
 * window to carry out: the action and the message it is about (-1 for
 * none; an account's index or an answer for the last two).
 */
struct ml_request {
	unsigned action;
	long message;
};

/*
 * The view's state.
 *
 * What is shown: the account and folder, the message (-1 for none),
 * whether a narrow window shows the message instead of the list, whether
 * the last frame was narrow, the scrolls of the list and of the message,
 * and (ws177-p015) the scroll of the accounts' folders with their height
 * as the last frame measured it, and the messages the list shows, in its
 * order (allocated and grown as the folder grows, so that a list has no
 * limit).
 *
 * The search, and the message being written: whether it is open, its
 * fields and its body (libkeiland's text area, ws090-p022), and the ID of
 * the message it answers (empty for a new one).
 *
 * An account being added or edited: whether its form shows, the account
 * edited (-1 for a new one), its fields, and whether the browser may fill
 * in sign-in codes from Mail (the desktop's setting, as the window last
 * read it).
 *
 * The question asked over the window (ML_QUESTION_*), its title and its
 * words.
 *
 * The requests queued for the window, the status under Get Mail (when
 * mail was last got, or what failed), the notice shown at the bottom until
 * a time (empty for none), whether the window stands on glass, and
 * whether the program is to end.
 */
struct ml_view {
	int account;
	enum ml_folder folder;
	long selected;
	int opened;
	int narrow;
	struct kl_scroll list_scroll;
	struct kl_scroll reader_scroll;
	struct kl_scroll sidebar_scroll;
	int sidebar_height;
	size_t *shown;
	size_t shown_capacity;

	struct kl_field search;
	int composing;
	struct kl_field to;
	struct kl_field cc;
	struct kl_field subject;
	struct kl_text_area body;
	char reply_id[ML_TEXT_MAX];

	int adding;
	int editing;
	struct kl_field setup_name;
	struct kl_field setup_address;
	struct kl_field setup_password;
	struct kl_field setup_imap;
	struct kl_field setup_smtp;
	int codes_allowed;

	unsigned question;
	char question_title[ML_TEXT_MAX];
	char question_body[ML_TEXT_MAX * 2U];

	struct ml_request requests[ML_REQUESTS_MAX];
	size_t request_count;
	char status[ML_TEXT_MAX];
	char notice[160];
	uint64_t notice_until;
	int glass;
	int quit;
};

/* The messages and accounts of the run (store.c). */
const struct ml_account_config *ml_accounts(size_t *count);
const struct ml_message *ml_messages(size_t *count);
const char *ml_folder_name(enum ml_folder folder);
int ml_store_add_account(const struct ml_account_config *config);
int ml_store_set_account(int index, const struct ml_account_config *config);
int ml_store_remove_account(int index);
void ml_store_drop_messages(int account);
int ml_store_redate(time_t now);
int ml_store_insert(const struct ml_message *message);
int ml_store_add_parsed(int account, enum ml_folder folder, uint32_t uid, unsigned flags, const struct ml_parsed *parsed, time_t now);
long ml_store_find(int account, enum ml_folder folder, uint32_t uid);
uint32_t ml_store_last_uid(int account, enum ml_folder folder);
struct ml_message *ml_store_at(long index);
void ml_store_release(void);

/* The accounts on the disk (account.c, secret.c). */
int ml_config_folder(char *folder, size_t size);
int ml_accounts_load(const char *folder, struct ml_account_config *accounts, size_t capacity, size_t *count);
int ml_accounts_save(const char *folder, const struct ml_account_config *accounts, size_t count);
int ml_secret_load(const char *folder, const char *address, char *password, size_t size);
int ml_secret_save(const char *folder, const char *address, const char *password);

/* The view (view.c). */
int ml_view_init(struct ml_view *view);
void ml_view_release(struct ml_view *view);
void ml_view_action(struct ml_view *view, unsigned action, uint64_t now_us);
void ml_view_key(struct ml_view *view, uint32_t key, unsigned modifiers, uint64_t now_us);
void ml_view_draw(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, uint64_t now_us);
size_t ml_view_panels(struct ml_view *view, int width, int height, struct kl_glass_panel *panels, size_t capacity);
int ml_view_wait(const struct ml_view *view, uint64_t now_us);
int ml_view_take_request(struct ml_view *view, struct ml_request *request);
void ml_view_notice(struct ml_view *view, const char *message, uint64_t now_us);
void ml_view_ask(struct ml_view *view, unsigned question, const char *title, const char *body);
void ml_view_question(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, uint64_t now_us);

/* The log for the tests (main.c, and the host tests' own). */
void ml_log(const char *format, ...);

#endif
