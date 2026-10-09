/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * sessiond's authentication (ws172-p002; docs/architecture/security.md,
 * "Login authentication"): every password, PIN and security key attempt,
 * and every change of a PIN or a key, is /sbin/passkey's.  sessiond starts
 * it for the attempt, serves its own loop while passkey works (the answer
 * is a descriptor it polls), keeps the failure counts in memory and delays
 * the answers to wrong ones.
 *
 * auth-policy.c holds the counts' rules (pure, host-tested); auth.c the
 * attempts, the requests of the greeter and the session, and the answers.
 */

#ifndef SESSIOND_AUTH_H
#define SESSIOND_AUTH_H

#include "sessiond.h"

/* The most accounts with counts, the PIN's wrong tries before it is turned off, and the delays. */
#define SESSIOND_COUNTS_MAX		64U
#define SESSIOND_PIN_TRIES		5U
#define SESSIOND_DELAY_SECONDS		2U
#define SESSIOND_DELAY_MAX		16U

/*
 * /sbin/passkey, and how long sessiond lets it run (its own deadline and 5
 * seconds more; a key's touch waits longer), then its grace after TERM.
 * The host test sets its own.
 */
#ifndef SESSIOND_PASSKEY
#define SESSIOND_PASSKEY		"/sbin/passkey"
#define SESSIOND_PASSKEY_MS		10000LL
#define SESSIOND_PASSKEY_KEY_MS		35000LL
#define SESSIOND_PASSKEY_GRACE_MS	2000LL
#endif

/* A key's reset: the key plugged in again (30 s) and touched (33 s), with room (ws199-p001 section 4.5). */
#ifndef SESSIOND_PASSKEY_RESET_MS
#define SESSIOND_PASSKEY_RESET_MS	75000LL
#endif

/* The fewest milliseconds between two KEYOWNER of the whole sessiond (ws199-p001 R10: the screen asks for the last key of a burst). */
#ifndef SESSIOND_KEYOWNER_MS
#define SESSIOND_KEYOWNER_MS		1000LL
#endif

/* The longest request written to passkey (its own bound), and how often a busy exchange is looked at (milliseconds). */
#define SESSIOND_REQUEST_SIZE		4096U
#define SESSIOND_EXCHANGE_TICK_MS	100

/* The styles. */
#define SESSIOND_STYLE_PASSWORD		0
#define SESSIOND_STYLE_PIN		1
#define SESSIOND_STYLE_FIDO2		2

/* The requests of the greeter and the session that are about credentials. */
enum sessiond_command {
	SESSIOND_COMMAND_STYLES,
	SESSIOND_COMMAND_AUTH,
	SESSIOND_COMMAND_UNLOCK,
	SESSIOND_COMMAND_ENROLLED,
	SESSIOND_COMMAND_ENROLL,
	SESSIOND_COMMAND_REMOVE,
	SESSIOND_COMMAND_KEYINFO,
	SESSIOND_COMMAND_KEYPIN,
	SESSIOND_COMMAND_KEYRESET,
	SESSIOND_COMMAND_SETOPTIONS,
	SESSIOND_COMMAND_KEYOWNER,
	SESSIOND_COMMAND_COUNT
};

/*
 * One account's counts (by user ID; the names no account has share one
 * entry): the failures in a row of every style, those of the PIN, and
 * whether the account has logged in or unlocked with its password or a
 * security key since sessiond started (until then no PIN is offered).
 */
struct sessiond_count {
	uid_t uid;
	int used;
	unsigned wrong;
	unsigned pin_wrong;
	int signed_in;
};

/* Every account's counts, for sessiond's life. */
struct sessiond_policy {
	struct sessiond_count counts[SESSIOND_COUNTS_MAX];
	struct sessiond_count unknown;
};

struct sessiond_count *sessiond_policy_count(struct sessiond_policy *policy, int known, uid_t uid);
char *sessiond_policy_word(char **cursor, char separator);
int sessiond_policy_style(const char *word);
int sessiond_policy_pin_allowed(const struct sessiond_count *count);
void sessiond_policy_attempt(struct sessiond_count *count, int style);
void sessiond_policy_success(struct sessiond_count *count, int style);
unsigned sessiond_policy_delay(const struct sessiond_count *count);
const char *sessiond_policy_reason(const char *passkey_reason);
void sessiond_policy_styles(const char *listed, const struct sessiond_count *count, char *out, size_t size);
int sessiond_policy_key_counts(const char *passkey_reason);

/*
 * A request in progress on one socket (the greeter's or the session's):
 * the request waiting for its secret lines, the passkey running for it,
 * and an answer held back by a failure's delay.
 */
struct sessiond_exchange {
	int socket;
	int session;
	const struct sessiond_account *owner;
	/* The request whose lines are still to come. */
	enum sessiond_command command;
	char name[SESSIOND_NAME_MAX];
	char argument[64];
	int style;
	unsigned lines_wanted;
	unsigned lines_have;
	char lines[2][SESSIOND_LINE_MAX];
	/* passkey: its process (its own group), its output, its deadline and when TERM went. */
	pid_t pid;
	int output;
	long long deadline_ms;
	long long term_ms;
	char answer[SESSIOND_LINE_MAX];
	size_t used;
	int done;
	int eof;
	struct sessiond_count *count;
	uid_t uid;
	/* A failure's answer, sent at reply_ms. */
	char held[SESSIOND_LINE_MAX];
	long long reply_ms;
	/* A key's reset whose password passkey-fido2 found right (status verified, ws199-p001). */
	int verified;
	/* A login that passkey granted (the greeter's): the account, in the greeter's login. */
	int logged_in;
	struct sessiond_account *login;
};

void sessiond_exchange_init(struct sessiond_exchange *exchange, int socket, const struct sessiond_account *owner, struct sessiond_account *login);
int sessiond_exchange_line(struct sessiond_exchange *exchange, char *line);
void sessiond_exchange_tick(struct sessiond_exchange *exchange);
int sessiond_exchange_fd(const struct sessiond_exchange *exchange);
int sessiond_exchange_busy(const struct sessiond_exchange *exchange);
void sessiond_exchange_stop(struct sessiond_exchange *exchange);

#endif
