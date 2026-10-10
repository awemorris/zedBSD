/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bMessage of the Message Access Profile (ws197-p003,
 * plan/ws197/phase003/phase.md section 7): the text form a phone gives a
 * message in (GetMessage) and takes one to send in (PushMessage).  A
 * bMessage is lines of PROPERTY:VALUE between BEGIN: and END: markers:
 * the message's status, type and folder, the originator's vCard, then an
 * envelope (BENV, up to three nested) with the recipients' vCards and the
 * body (BBODY) whose text lies between BEGIN:MSG and END:MSG.
 *
 * The reader takes what an SMS needs: the status and type, the numbers
 * and names of the originator and of the first recipient of the outer
 * envelope, and the text of the first part, which it finds by the LENGTH
 * the phone gave (phones count it in more than one way, so each way is
 * tried in turn) or, failing that, by the first END:MSG line.  Only UTF-8
 * text is taken; a malformed sequence becomes U+FFFD.
 *
 * Without system calls; the host tests build it.
 */

#ifndef BLUETOOTHD_BMSG_H
#define BLUETOOTHD_BMSG_H

#include "userland/base/bluetoothd/mapxml.h"

#include <stddef.h>
#include <stdint.h>

/* The most one bMessage is, in bytes. */
#define BTD_BMSG_INPUT_MAX (16U * 1024U * 1024U + 1024U)

/* The longest text kept of a message read; a longer one is cut. */
#define BTD_BMSG_TEXT_MAX		16384U

/* The longest text a message sent carries, and the longest number it goes to. */
#define BTD_BMSG_SEND_MAX		8192U
#define BTD_BMSG_NUMBER_MAX		32U

/* The room a bMessage built for any text and number within those limits needs. */
#define BTD_BMSG_BUILD_MAX		(2U * BTD_BMSG_SEND_MAX + 512U)

/* The deepest envelopes nest. */
#define BTD_BMSG_ENVELOPES_MAX		3

/* Whether a message was read, as its STATUS says. */
#define BTD_BMSG_STATUS_UNKNOWN		0
#define BTD_BMSG_STATUS_READ		1
#define BTD_BMSG_STATUS_UNREAD		2

/*
 * How the text was found: by LENGTH counted as the profile says (from
 * BEGIN:MSG to the line break after END:MSG), by LENGTH counted as the
 * text alone, by LENGTH counted without the last line break, or by the
 * first END:MSG line when no count fit.
 */
#define BTD_BMSG_FORM_SPEC		1
#define BTD_BMSG_FORM_TEXT		2
#define BTD_BMSG_FORM_NO_BREAK		3
#define BTD_BMSG_FORM_SCAN		4

/*
 * One message read: its status, type (BTD_MAP_TYPE_*) and folder; the
 * number and name of its originator and of its first recipient (cut to
 * fit, never inside a character; a name from FN, else from N); which way
 * the text was found; and the text (UTF-8, NUL-terminated, cut at
 * BTD_BMSG_TEXT_MAX with truncated set).
 */
struct btd_bmsg {
	int status;
	int type;
	char folder[BTD_MAPXML_TEXT_SIZE];
	char originator_number[BTD_MAPXML_TEXT_SIZE];
	char originator_name[BTD_MAPXML_TEXT_SIZE];
	char recipient_number[BTD_MAPXML_TEXT_SIZE];
	char recipient_name[BTD_MAPXML_TEXT_SIZE];
	int form;
	int truncated;
	size_t text_length;
	char text[BTD_BMSG_TEXT_MAX + 1U];
	/* Borrows the complete MMS MIME span from the input until its next reuse. */
	const uint8_t *mime;
	size_t mime_length;
};

int btd_bmsg_parse(const uint8_t *input, size_t length, struct btd_bmsg *message);
int btd_bmsg_number_ok(const char *number);
int btd_bmsg_utf8_ok(const uint8_t *text, size_t length);
int btd_bmsg_build(const char *number, const uint8_t *text, size_t length, int type, uint8_t *output, size_t size, size_t *used);

#endif
