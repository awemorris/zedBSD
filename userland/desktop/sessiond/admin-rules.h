/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of the admin actions (admin-rules.c): the definitions in
 * /etc/keiland/admin.d that name the programs a session may have run as
 * root once the person at the screen allows it, who is asked and how,
 * the words a request may carry, and the table of the permissions kept
 * for later requests.  Nothing here starts a process or changes a file:
 * a definition and a program are only opened and read, under a root and
 * an owner the caller names, so the host tests run all of it alone.
 */

#ifndef SESSIOND_ADMIN_RULES_H
#define SESSIOND_ADMIN_RULES_H

#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* Where the definitions are, below the root the caller names. */
#define SESSIOND_ADMIN_DIRECTORY	"/etc/keiland/admin.d"

/* The longest action name, and the largest definition file. */
#define SESSIOND_ADMIN_NAME_MAX		32U
#define SESSIOND_ADMIN_FILE_MAX		4096U

/* The longest program path of a definition. */
#define SESSIOND_ADMIN_PATH_MAX		256U

/*
 * The words after the program: at most 8 in one allowed set, each at
 * most 64 bytes, at most 256 bytes joined by single spaces, and at most
 * 16 allowed sets in one definition.
 */
#define SESSIOND_ADMIN_WORDS_MAX	8U
#define SESSIOND_ADMIN_WORD_MAX		64U
#define SESSIOND_ADMIN_WORDS_BYTES	256U
#define SESSIOND_ADMIN_ROWS_MAX		16U

/* The longest title and message, and how many languages each may have besides the one without a language. */
#define SESSIOND_ADMIN_TITLE_MAX	80U
#define SESSIOND_ADMIN_MESSAGE_MAX	400U
#define SESSIOND_ADMIN_LANGUAGES_MAX	8U

/* The longest language tag: two or three letters, a dash, and up to eight more. */
#define SESSIOND_ADMIN_LANGUAGE_MAX	12U

/* The longest locale a request names: "ll_CC.UTF-8" with a three-letter language. */
#define SESSIOND_ADMIN_LOCALE_MAX	12U

/* The longest time a permission is kept for, in seconds. */
#define SESSIOND_ADMIN_KEEP_SECONDS_MAX	300U

/* How many kept permissions the table holds; the oldest goes for a new one. */
#define SESSIOND_ADMIN_KEEP_MAX		8U

/*
 * The streams of a program: the bits are the ones the desktop library
 * gives its processes (KL_PROCESS_INPUT, _OUTPUT and _ERROR), and the
 * status pipe the runner writes how the program ended to.
 */
#define SESSIOND_ADMIN_INPUT		0x1U
#define SESSIOND_ADMIN_OUTPUT		0x2U
#define SESSIOND_ADMIN_ERROR		0x4U
#define SESSIOND_ADMIN_STATUS		0x8U
#define SESSIOND_ADMIN_STREAMS		0x7U

/* How the session's user is asked: allowing, the user's own password, an administrator's password, or not at all. */
enum sessiond_admin_method {
	SESSIOND_ADMIN_CONSENT,
	SESSIOND_ADMIN_PASSWORD,
	SESSIOND_ADMIN_ADMIN_PASSWORD,
	SESSIOND_ADMIN_REFUSE
};

/* How long a permission is kept: not at all, while the client is connected, or for some seconds. */
enum sessiond_admin_keep_kind {
	SESSIOND_ADMIN_KEEP_NONE,
	SESSIOND_ADMIN_KEEP_CLIENT,
	SESSIOND_ADMIN_KEEP_SECONDS
};

/*
 * The decision for a request: the dialog asks for an allowing, the
 * user's password or an administrator's; or the user may not run the
 * action; or the definition asks for a way of confirming not made yet.
 */
enum sessiond_admin_verdict {
	SESSIOND_ADMIN_VERDICT_CONSENT,
	SESSIOND_ADMIN_VERDICT_PASSWORD_SELF,
	SESSIOND_ADMIN_VERDICT_PASSWORD_ADMIN,
	SESSIOND_ADMIN_VERDICT_NOT_ALLOWED,
	SESSIOND_ADMIN_VERDICT_METHOD
};

/*
 * One text of the dialog in one language: the language tag (empty for
 * the text without a language) and the text, UTF-8 without control
 * characters.
 */
struct sessiond_admin_text {
	char language[SESSIOND_ADMIN_LANGUAGE_MAX + 1U];
	char text[SESSIOND_ADMIN_MESSAGE_MAX + 1U];
};

/*
 * One set of words: an allowed set of a definition (the words after the
 * program, in their order), or the words a request carries.
 */
struct sessiond_admin_words {
	unsigned count;
	char words[SESSIOND_ADMIN_WORDS_MAX][SESSIOND_ADMIN_WORD_MAX + 1U];
};

/*
 * One action as its definition file says: its name (the file's), the
 * program run as root, the sets of words a request may carry, the
 * dialog's title and message in their languages, how a member of wheel
 * and anyone else is asked, how long a permission is kept, the streams
 * the program may be given, and whether it is a program of the desktop
 * (display wayland): the compositor then lends it a connection of its
 * own, so that a program run as root shows its windows and reads the
 * seat without reaching the user's socket.  A request keeps the definition it
 * was asked with, so what is run is what the person was shown.
 */
struct sessiond_admin_definition {
	char name[SESSIOND_ADMIN_NAME_MAX + 1U];
	char program[SESSIOND_ADMIN_PATH_MAX];
	unsigned row_count;
	struct sessiond_admin_words rows[SESSIOND_ADMIN_ROWS_MAX];
	unsigned title_count;
	struct sessiond_admin_text titles[SESSIOND_ADMIN_LANGUAGES_MAX + 1U];
	unsigned message_count;
	struct sessiond_admin_text messages[SESSIOND_ADMIN_LANGUAGES_MAX + 1U];
	enum sessiond_admin_method wheel;
	enum sessiond_admin_method others;
	enum sessiond_admin_keep_kind keep;
	unsigned keep_seconds;
	unsigned streams;
	int display;
};

/*
 * What a definition file was when it was read: a kept permission ends
 * when the file is replaced or changed.
 */
struct sessiond_admin_identity {
	dev_t device;
	ino_t inode;
	off_t size;
	time_t modified;
};

/*
 * Where definitions and programs are opened from: the root every path
 * is below ("" for the machine's own root), and the owner every
 * directory and file on the way must have (root on the machine).
 */
struct sessiond_admin_place {
	const char *root;
	uid_t owner;
};

/*
 * An ADMIN ASK request: the compositor's number for it, its client's
 * number, the desktop's locale (empty for none; it chooses the dialog's
 * texts and is a desktop program's LANG), the streams asked for, the
 * action, and the words.
 */
struct sessiond_admin_ask {
	unsigned long id;
	unsigned long client;
	char locale[SESSIOND_ADMIN_LOCALE_MAX + 1U];
	unsigned streams;
	char action[SESSIOND_ADMIN_NAME_MAX + 1U];
	struct sessiond_admin_words words;
};

/*
 * One kept permission: the session's generation, the client, the
 * action, how it is kept and until when (in sessiond_milliseconds), the
 * definition file as it was, and the order it was made in (the oldest
 * goes first when the table is full).
 */
struct sessiond_admin_kept {
	int used;
	unsigned long long generation;
	unsigned long client;
	char action[SESSIOND_ADMIN_NAME_MAX + 1U];
	enum sessiond_admin_keep_kind kind;
	long long expires_ms;
	struct sessiond_admin_identity identity;
	unsigned long long order;
};

/*
 * The table of kept permissions of the running session.  It lives in
 * sessiond and is emptied when the session ends, its compositor's
 * socket closes, and the lock screen shows.
 */
struct sessiond_admin_keep {
	struct sessiond_admin_kept entries[SESSIOND_ADMIN_KEEP_MAX];
	unsigned long long order;
};

int sessiond_admin_name_valid(const char *name);
int sessiond_admin_parse(const char *name, const char *text, size_t length, struct sessiond_admin_definition *definition);
int sessiond_admin_match(const struct sessiond_admin_definition *definition, const struct sessiond_admin_words *words);
const char *sessiond_admin_text(const struct sessiond_admin_text *texts, unsigned count, const char *locale);
enum sessiond_admin_verdict sessiond_admin_decide(const struct sessiond_admin_definition *definition, uid_t uid, int in_wheel);
int sessiond_admin_parse_id(const char *text, unsigned long *id, const char **rest);
int sessiond_admin_parse_ask(const char *arguments, struct sessiond_admin_ask *ask);

int sessiond_admin_open(const struct sessiond_admin_place *place, const char *path, int *descriptor, struct stat *status);
int sessiond_admin_load(const struct sessiond_admin_place *place, const char *name, struct sessiond_admin_definition *definition, struct sessiond_admin_identity *identity);
int sessiond_admin_open_program(const struct sessiond_admin_place *place, const struct sessiond_admin_definition *definition, int *descriptor);
void sessiond_admin_identity_of(const struct stat *status, struct sessiond_admin_identity *identity);

void sessiond_admin_keep_clear(struct sessiond_admin_keep *keep);
int sessiond_admin_keep_find(struct sessiond_admin_keep *keep, unsigned long long generation, unsigned long client, const char *action, const struct sessiond_admin_identity *identity, long long now_ms);
void sessiond_admin_keep_add(struct sessiond_admin_keep *keep, unsigned long long generation, unsigned long client, const struct sessiond_admin_definition *definition, const struct sessiond_admin_identity *identity, long long now_ms);
void sessiond_admin_keep_forget(struct sessiond_admin_keep *keep, unsigned long client);
void sessiond_admin_keep_expire(struct sessiond_admin_keep *keep, long long now_ms);

#endif
