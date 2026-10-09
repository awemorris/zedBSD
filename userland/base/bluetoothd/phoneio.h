/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lines of the PHONE requests and events on bluetoothd's socket
 * (ws197-p003, plan/ws197/phase003/phase.md sections 4.3 and 9.1): a
 * request's arguments are KEY=VALUE separated by spaces, VALUE a word
 * without spaces or a string in double quotes in which \" \\ and \xHH
 * stand for a quote, a backslash and a byte; a value of some length
 * (PHONE SEND's text) follows its line as that many bytes.
 *
 * Without system calls; the host tests build it.
 */

#ifndef BLUETOOTHD_PHONEIO_H
#define BLUETOOTHD_PHONEIO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The longest text PHONE SEND carries. */
#define BTD_PHONE_SEND_MAX		8192U

/* The longest key and value of a request's argument (a value's NUL included). */
#define BTD_PHONEIO_KEY_MAX		16U
#define BTD_PHONEIO_VALUE_MAX		256U

/* What btd_phoneio_next found: an argument, the line's end, or a malformed argument. */
#define BTD_PHONEIO_ARGUMENT		1
#define BTD_PHONEIO_END			0
#define BTD_PHONEIO_MALFORMED		(-1)

/* The longest request line a client writes (its newline included), as protocol.h's BTD_LINE_MAX. */
#define BTD_PHONEIO_LINE_MAX		512U

/*
 * The longest line the daemon writes (its newline included: the
 * compositor reads lines into 2048 bytes), and the longest name or number
 * a line carries before its escapes.
 */
#define BTD_PHONEIO_OUT_MAX		2047U
#define BTD_PHONEIO_TEXT_MAX		128U

/*
 * What a client's input gives the daemon: a request's line (nonzero
 * stops the reading: the client went), and a PHONE SEND's line with its
 * whole text.
 */
struct btd_phoneio_events {
	void *context;
	int (*line)(void *context, char *line);
	void (*text)(void *context, const char *line, const uint8_t *text, size_t length);
};

/*
 * The input of one client (ws197-p003 section 4.3): the bytes of a line
 * not ended yet, and while a PHONE SEND's text is read, its line and the
 * text (allocated while it is read).  A text's bytes are never read as
 * lines.
 */
struct btd_phoneio_input {
	char line[BTD_PHONEIO_LINE_MAX];
	size_t used;
	char send_line[BTD_PHONEIO_LINE_MAX];
	uint8_t *text;
	size_t text_length;
	size_t text_used;
};

/*
 * The phone's owner as the socket's rules see it (ws197-p004 section
 * 5.4): whether a valid record names one, and its uid.
 */
struct btd_phoneio_owner {
	int have_owner;
	uid_t owner;
};

/*
 * One client as the phone's rules see it: whether its slot is open, its
 * uid, whether it subscribed, and whether it waits for a phone request's
 * answer.
 */
struct btd_phoneio_client {
	int open;
	uid_t uid;
	int subscribed;
	int waits_phone;
};

/* What PHONE SUBSCRIBE is answered. */
#define BTD_PHONEIO_SUBSCRIBE_OK		0
#define BTD_PHONEIO_SUBSCRIBE_PERMISSION	1
#define BTD_PHONEIO_SUBSCRIBE_BUSY		2

int btd_phoneio_allowed(const struct btd_phoneio_owner *owner, uid_t uid);
int btd_phoneio_owner_changed(const struct btd_phoneio_owner *before, const struct btd_phoneio_owner *now);
unsigned btd_phoneio_to_close(const struct btd_phoneio_owner *owner, const struct btd_phoneio_client *clients, unsigned count, int *closes);
int btd_phoneio_subscribe(const struct btd_phoneio_owner *owner, uid_t uid, int have_record, unsigned subscribers, unsigned most);
int btd_phoneio_accept(uid_t uid, int seated, unsigned free_slots, unsigned reserved, unsigned held, unsigned per_uid);
int btd_phoneio_next(const char **cursor, char *key, size_t key_size, char *value, size_t value_size);
int btd_phoneio_send_length(const char *line, size_t *length);
int btd_phoneio_quote(char *line, size_t size, size_t *used, const char *text, size_t limit);
void btd_phoneio_input_init(struct btd_phoneio_input *input);
void btd_phoneio_input_room(struct btd_phoneio_input *input, uint8_t **room, size_t *size);
int btd_phoneio_input_got(struct btd_phoneio_input *input, size_t count, const struct btd_phoneio_events *events);
void btd_phoneio_input_clear(struct btd_phoneio_input *input);

#endif
