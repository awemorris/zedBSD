/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The vCard of the Phone Book Access Profile (ws197-p005,
 * plan/ws197/phase005/phase.md section 4): the cards a phone gives for
 * its phone book and for its call history, in vCard 2.1 or 3.0.
 *
 * A phone book object is cards one after another, each from a
 * BEGIN:VCARD line to its END:VCARD line; btd_vcard_next cuts them out.
 * A card is lines of [group.]NAME[;parameter]*:value.  The reader joins
 * the lines a card folded (a line starting with a space or a TAB goes on
 * the line before it: in 3.0 without that space, in 2.1 with it) and the
 * soft line breaks of a quoted-printable value (a line ending in '='),
 * takes names and parameters in either case and 2.1's bare parameters
 * (TEL;CELL is TEL;TYPE=CELL), undoes quoted-printable and 3.0's escapes,
 * and keeps only the properties a contact or a call needs: VERSION, FN,
 * N, TEL, UID and X-IRMC-CALL-DATETIME.  A value in a character set
 * other than UTF-8, or in BASE64 (a photograph the phone sent anyway),
 * is left out.  A malformed UTF-8 sequence or a control character in a
 * text becomes U+FFFD (a line break or a TAB becomes a space), and a text
 * is cut at its room without splitting a character.
 *
 * A contact is written back as a reduced vCard 3.0 (FN, N, TEL and UID)
 * for the Phone application to keep.  A contact and a call each get a
 * key, the 16 hexadecimal digits of a 64-bit FNV-1a hash, by which the
 * application knows the same one again on the next reading.
 *
 * Without system calls; the host tests build it.
 */

#ifndef BLUETOOTHD_VCARD_H
#define BLUETOOTHD_VCARD_H

#include <stddef.h>
#include <stdint.h>

/* The most one card is, in bytes; a longer one is not read. */
#define BTD_VCARD_CARD_MAX		16384U

/* The longest line a card's property is read from, once its folds are joined; a longer one is left out. */
#define BTD_VCARD_LINE_MAX		4096U

/* The most numbers one contact keeps; the ones after them are left out. */
#define BTD_VCARD_TELS_MAX		8U

/* The room of a text (256 bytes and a NUL), of a number (32 digits and a NUL), of a key and of a datetime. */
#define BTD_VCARD_TEXT_SIZE		257U
#define BTD_VCARD_NUMBER_SIZE		33U
#define BTD_VCARD_KEY_SIZE		17U
#define BTD_VCARD_DATETIME_SIZE		24U

/* The parts of N: family name, given name, additional names, prefixes, suffixes. */
#define BTD_VCARD_N_PARTS		5U

/* The room a reduced vCard of any contact within the limits above needs. */
#define BTD_VCARD_REDUCED_MAX		6144U

/* The version a card names: not named or another, 2.1, or 3.0. */
#define BTD_VCARD_VERSION_UNKNOWN	0
#define BTD_VCARD_VERSION_21		1
#define BTD_VCARD_VERSION_30		2

/* The types of a number, as its TYPE parameters name them. */
#define BTD_VCARD_TEL_CELL		0x01U
#define BTD_VCARD_TEL_HOME		0x02U
#define BTD_VCARD_TEL_WORK		0x04U
#define BTD_VCARD_TEL_VOICE		0x08U
#define BTD_VCARD_TEL_FAX		0x10U
#define BTD_VCARD_TEL_PAGER		0x20U
#define BTD_VCARD_TEL_PREF		0x40U

/* What kind of call a card of the history is: not said, one taken, one made, or one not answered. */
#define BTD_VCARD_CALL_UNKNOWN		0
#define BTD_VCARD_CALL_RECEIVED		1
#define BTD_VCARD_CALL_DIALED		2
#define BTD_VCARD_CALL_MISSED		3

/*
 * How a call's time was read: no time (none given, or one not readable),
 * by the zone the datetime gave itself (a Z or an offset), or as the
 * phone's local time taken in zedBSD's zone.
 */
#define BTD_VCARD_ZONE_NONE		0
#define BTD_VCARD_ZONE_PHONE		1
#define BTD_VCARD_ZONE_LOCAL		2

/*
 * One number of a contact: the digits and the + * # it has (everything
 * else of the value left out), and its types (BTD_VCARD_TEL_*).
 */
struct btd_vcard_tel {
	char number[BTD_VCARD_NUMBER_SIZE];
	unsigned types;
};

/*
 * One contact of the phone book: the version its card named; the name to
 * show (its FN, else "given family" from its N, else its first number);
 * its FN and the parts of its N as written (has_n says whether it had an
 * N); its numbers in the order written, and how many more were left out
 * (past BTD_VCARD_TELS_MAX, empty, or longer than 32 digits); its UID;
 * and its key.  Every text is UTF-8 and NUL-terminated.
 */
struct btd_vcard_contact {
	int version;
	char name[BTD_VCARD_TEXT_SIZE];
	char formatted[BTD_VCARD_TEXT_SIZE];
	int has_n;
	char n[BTD_VCARD_N_PARTS][BTD_VCARD_TEXT_SIZE];
	size_t tel_count;
	size_t tels_dropped;
	struct btd_vcard_tel tels[BTD_VCARD_TELS_MAX];
	char uid[BTD_VCARD_TEXT_SIZE];
	char key[BTD_VCARD_KEY_SIZE];
};

/*
 * One call of the history: the version its card named, its kind
 * (BTD_VCARD_CALL_*), the phone's datetime string as given (empty when
 * none was given or it was too long to be one), its first number (empty
 * for a number withheld), the name the phone gave it (its FN, else
 * "given family" from its N, else empty), and its key.
 */
struct btd_vcard_call {
	int version;
	int kind;
	char datetime[BTD_VCARD_DATETIME_SIZE];
	char number[BTD_VCARD_NUMBER_SIZE];
	char name[BTD_VCARD_TEXT_SIZE];
	char key[BTD_VCARD_KEY_SIZE];
};

int btd_vcard_next(const uint8_t *body, size_t length, size_t *at, size_t *card_start, size_t *card_length);
unsigned btd_vcard_count(const uint8_t *body, size_t length);
int btd_vcard_contact_read(const uint8_t *card, size_t length, struct btd_vcard_contact *contact);
int btd_vcard_reduce(const struct btd_vcard_contact *contact, char *output, size_t size, size_t *used);
int btd_vcard_call_read(const uint8_t *card, size_t length, int folder_kind, struct btd_vcard_call *call);
int btd_vcard_call_time(const struct btd_vcard_call *call, int32_t local_offset, int64_t *seconds, int *zone);

#endif
