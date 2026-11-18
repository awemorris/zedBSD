/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The vCard of the Phone Book Access Profile (ws197-p005, see vcard.h).
 */

#include "userland/base/bluetoothd/vcard.h"

#include "userland/base/bluetoothd/mapxml.h"

#include <errno.h>
#include <string.h>

/* The properties a card's reader tells apart; every other one is VCARD_NAME_OTHER. */
#define VCARD_NAME_OTHER		0U
#define VCARD_NAME_BEGIN		1U
#define VCARD_NAME_END			2U
#define VCARD_NAME_VERSION		3U
#define VCARD_NAME_FN			4U
#define VCARD_NAME_N			5U
#define VCARD_NAME_TEL			6U
#define VCARD_NAME_UID			7U
#define VCARD_NAME_DATETIME		8U

/* How a value is written: as it is, in quoted-printable, or in a way the reader does not take. */
#define VCARD_ENCODING_PLAIN		1U
#define VCARD_ENCODING_QP		2U
#define VCARD_ENCODING_UNUSABLE		3U

/* The kinds of call a datetime's parameters name, beside the types of a number. */
#define VCARD_TYPE_RECEIVED		0x100U
#define VCARD_TYPE_DIALED		0x200U
#define VCARD_TYPE_MISSED		0x400U

/* The types that are a number's, as opposed to a call's. */
#define VCARD_TYPE_TEL_MASK		0xffU

/* The deepest cards nest (a card inside an AGENT property of 2.1). */
#define VCARD_DEPTH_MAX			8

/* The basis and the prime of the 64-bit FNV-1a hash. */
#define VCARD_FNV_BASIS			0xcbf29ce484222325ULL
#define VCARD_FNV_PRIME			0x100000001b3ULL

/* U+FFFD, which stands for a malformed sequence or a control character of a text. */
#define VCARD_REPLACEMENT		"\xef\xbf\xbd"
#define VCARD_REPLACEMENT_BYTES		3U

/*
 * One line of the input as it lies there: where it starts, where its
 * text ends (its line break and a carriage return before it left out),
 * and where the next line starts.
 */
struct vcard_line {
	size_t start;
	size_t end;
	size_t next;
};

/* A word of a card, and the value the reader keeps for it. */
struct vcard_word {
	const char *name;
	unsigned value;
};

/*
 * What the part of a line before its value says: the property's name
 * (VCARD_NAME_*), the types its parameters gave (BTD_VCARD_TEL_* and
 * VCARD_TYPE_*), how its value is written (VCARD_ENCODING_*), whether its
 * character set is UTF-8 or not given, and where its value starts.
 */
struct vcard_head {
	unsigned name;
	unsigned types;
	unsigned encoding;
	int charset_ok;
	size_t value_start;
};

/*
 * One property of the card being read, at the card's own depth: its name,
 * its types, whether its value can be used (written in UTF-8 as text,
 * its line within BTD_VCARD_LINE_MAX), and its value with quoted-
 * printable undone but escapes not yet (in the reader's value room).
 */
struct vcard_property {
	unsigned name;
	unsigned types;
	int usable;
	size_t value_length;
};

/*
 * The reading of one card: the input and where the reading is, the
 * version the card named (which decides how a folded line is joined),
 * how deep in nested cards the reading is, the line being read with its
 * folds and soft line breaks joined (line_overflow when it was longer
 * than its room), and the room the value of a property is undone into.
 * It lives for one read.
 */
struct vcard_reader {
	const uint8_t *bytes;
	size_t length;
	size_t at;
	int version;
	int depth;
	uint8_t line[BTD_VCARD_LINE_MAX];
	size_t line_length;
	int line_overflow;
	uint8_t value[BTD_VCARD_LINE_MAX];
};

/* The properties the reader keeps. */
static const struct vcard_word vcard_names[] = {
	{ "BEGIN", VCARD_NAME_BEGIN },
	{ "END", VCARD_NAME_END },
	{ "VERSION", VCARD_NAME_VERSION },
	{ "FN", VCARD_NAME_FN },
	{ "N", VCARD_NAME_N },
	{ "TEL", VCARD_NAME_TEL },
	{ "UID", VCARD_NAME_UID },
	{ "X-IRMC-CALL-DATETIME", VCARD_NAME_DATETIME },
	{ NULL, 0U }
};

/*
 * The types a TYPE parameter (or a bare parameter of 2.1) names.  The
 * numbers' types come first, in the order a reduced vCard writes them.
 */
static const struct vcard_word vcard_types[] = {
	{ "CELL", BTD_VCARD_TEL_CELL },
	{ "HOME", BTD_VCARD_TEL_HOME },
	{ "WORK", BTD_VCARD_TEL_WORK },
	{ "VOICE", BTD_VCARD_TEL_VOICE },
	{ "FAX", BTD_VCARD_TEL_FAX },
	{ "PAGER", BTD_VCARD_TEL_PAGER },
	{ "PREF", BTD_VCARD_TEL_PREF },
	{ "RECEIVED", VCARD_TYPE_RECEIVED },
	{ "DIALED", VCARD_TYPE_DIALED },
	{ "MISSED", VCARD_TYPE_MISSED },
	{ NULL, 0U }
};

/* The ways a value is written, as ENCODING (or a bare parameter of 2.1) names them. */
static const struct vcard_word vcard_encodings[] = {
	{ "QUOTED-PRINTABLE", VCARD_ENCODING_QP },
	{ "8BIT", VCARD_ENCODING_PLAIN },
	{ "7BIT", VCARD_ENCODING_PLAIN },
	{ "BASE64", VCARD_ENCODING_UNUSABLE },
	{ "B", VCARD_ENCODING_UNUSABLE },
	{ NULL, 0U }
};

static int vcard_physical_line(const uint8_t *bytes, size_t length, size_t at, struct vcard_line *line);
static int vcard_same(const uint8_t *bytes, size_t start, size_t end, const char *word);
static void vcard_trim(const uint8_t *bytes, size_t *start, size_t *end);
static unsigned vcard_word(const struct vcard_word *words, const uint8_t *bytes, size_t start, size_t end);
static int vcard_head(const uint8_t *bytes, size_t length, struct vcard_head *head);
static void vcard_parameter(const uint8_t *bytes, size_t start, size_t end, struct vcard_head *head);
static void vcard_type_list(const uint8_t *bytes, size_t start, size_t end, struct vcard_head *head);
static int vcard_version_scan(const uint8_t *bytes, size_t length);
static void vcard_reader_start(struct vcard_reader *reader, const uint8_t *card, size_t length);
static void vcard_line_append(struct vcard_reader *reader, const uint8_t *bytes, size_t start, size_t end);
static int vcard_line_is_qp(const struct vcard_reader *reader);
static int vcard_logical_line(struct vcard_reader *reader);
static int vcard_property_next(struct vcard_reader *reader, struct vcard_property *property);
static size_t vcard_qp_decode(const uint8_t *input, size_t length, uint8_t *output);
static int vcard_hex_digit(uint8_t byte);
static size_t vcard_unescape(uint8_t *bytes, size_t start, size_t end);
static size_t vcard_utf8_length(const uint8_t *bytes, size_t available);
static int vcard_space(uint8_t byte);
static int vcard_number_byte(uint8_t byte);
static int vcard_datetime_byte(uint8_t byte);
static int vcard_escape_byte(char byte);
static void vcard_sanitize(char *text, size_t size, const uint8_t *bytes, size_t length);
static void vcard_text(struct vcard_reader *reader, const struct vcard_property *property, char *text, size_t size);
static void vcard_parts(struct vcard_reader *reader, const struct vcard_property *property, char (*parts)[BTD_VCARD_TEXT_SIZE], size_t count);
static int vcard_number(struct vcard_reader *reader, const struct vcard_property *property, char *number, size_t size);
static void vcard_datetime(const struct vcard_reader *reader, const struct vcard_property *property, char *datetime, size_t size);
static void vcard_name_from_n(char *text, size_t size, const char *family, const char *given);
static uint64_t vcard_fnv(uint64_t hash, const void *data, size_t length);
static void vcard_key_text(uint64_t hash, char *key);
static void vcard_contact_key(struct btd_vcard_contact *contact);
static int vcard_put(char *output, size_t size, size_t *used, const void *data, size_t length);
static int vcard_put_string(char *output, size_t size, size_t *used, const char *text);
static int vcard_put_escaped(char *output, size_t size, size_t *used, const char *text);
static int vcard_put_tel(char *output, size_t size, size_t *used, const struct btd_vcard_tel *tel);

/*
 * Cuts the next card out of a phone book object body, starting the
 * search at *at: from a BEGIN:VCARD line to the END:VCARD line that
 * closes it (cards nested in it counted), its line break included.
 * Lines before a BEGIN:VCARD are passed over.  Returns 0 with the card's
 * start and length and *at past it, ENOENT when no card is left, or
 * EINVAL when the body ends inside a card (*at is then the body's end).
 */
int
btd_vcard_next(
	const uint8_t *body,
	size_t length,
	size_t *at,
	size_t *card_start,
	size_t *card_length)
{
	struct vcard_line line;
	size_t start;
	int depth;
	int begin;
	int end;
	int error;

	/* Nothing found yet. */
	*card_start = 0U;
	*card_length = 0U;

	/* The lines up to the first that opens a card. */
	for (;;) {
		/* No card left in the body. */
		error = vcard_physical_line(body, length, *at, &line);
		if (error != 0) {
			*at = length;
			return ENOENT;
		}

		/* The search goes on after the line. */
		*at = line.next;

		/* A line that opens a card starts it. */
		begin = vcard_same(body, line.start, line.end, "BEGIN:VCARD");
		if (begin)
			break;
	}

	/* The card's lines, until the line that closes the card it opened. */
	start = line.start;
	depth = 1;
	while (depth > 0) {
		/* A body that ends inside the card. */
		error = vcard_physical_line(body, length, *at, &line);
		if (error != 0) {
			*at = length;
			return EINVAL;
		}

		/* The card goes on after the line. */
		*at = line.next;

		/* A card nested in it opens and closes inside it. */
		begin = vcard_same(body, line.start, line.end, "BEGIN:VCARD");
		end = vcard_same(body, line.start, line.end, "END:VCARD");
		if (begin) {
			depth++;
		} else if (end) {
			depth--;
		}
	}

	/* Succeeded: the card, its last line break included. */
	*card_start = start;
	*card_length = *at - start;
	return 0;
}

/*
 * Counts the cards of a body at its own level, as btd_vcard_next cuts
 * them: each BEGIN:VCARD line outside a card starts one (a card the body
 * ends inside is counted too), the cards nested in it are not counted.
 * The phone book's offsets count cards this way (ws197-p005 section 5.3).
 */
unsigned
btd_vcard_count(
	const uint8_t *body,
	size_t length)
{
	struct vcard_line line;
	size_t at;
	unsigned count;
	unsigned depth;
	int begin;
	int end;
	int error;

	/* Each line of the body. */
	count = 0U;
	depth = 0U;
	at = 0U;
	for (;;) {
		/* No line left. */
		error = vcard_physical_line(body, length, at, &line);
		if (error != 0)
			break;
		at = line.next;

		/* A card opens: one more at the body's level; a card closes. */
		begin = vcard_same(body, line.start, line.end, "BEGIN:VCARD");
		end = vcard_same(body, line.start, line.end, "END:VCARD");
		if (begin) {
			if (depth == 0U)
				count++;
			depth++;
		} else if (end && depth > 0U) {
			depth--;
		}
	}

	/* The cards counted. */
	return count;
}

/*
 * Reads one card of the phone book into contact.  Returns 0; ENOENT for
 * a card with neither a name nor a number (contact holds what was read);
 * EINVAL for one malformed (not opened by BEGIN:VCARD, never closed, or
 * nested deeper than eight cards); or E2BIG for one past
 * BTD_VCARD_CARD_MAX.
 */
int
btd_vcard_contact_read(
	const uint8_t *card,
	size_t length,
	struct btd_vcard_contact *contact)
{
	struct vcard_reader reader;
	struct vcard_property property;
	struct btd_vcard_tel *tel;
	char n_name[BTD_VCARD_TEXT_SIZE];
	int error;

	/* Nothing known of the contact yet. */
	memset(contact, 0, sizeof(*contact));

	/* A card within the size the daemon reads. */
	if (length > BTD_VCARD_CARD_MAX)
		return E2BIG;

	/* The reading starts at the card's first line. */
	vcard_reader_start(&reader, card, length);
	contact->version = reader.version;

	/* Each property of the card itself, until the card closes. */
	for (;;) {
		/* The card closed: everything is read. */
		error = vcard_property_next(&reader, &property);
		if (error == ENOENT)
			break;

		/* A card malformed. */
		if (error != 0) {
			memset(contact, 0, sizeof(*contact));
			return EINVAL;
		}

		/* A number whose value cannot be used counts as left out; any other such property is passed over. */
		if (!property.usable) {
			if (property.name == VCARD_NAME_TEL)
				contact->tels_dropped++;
			continue;
		}

		/* The property, by its name. */
		switch (property.name) {
		case VCARD_NAME_FN:
			/* The first FN that says something. */
			if (contact->formatted[0] == '\0')
				vcard_text(&reader, &property, contact->formatted, sizeof(contact->formatted));
			break;

		case VCARD_NAME_N:
			/* The first N, in its parts. */
			if (!contact->has_n) {
				vcard_parts(&reader, &property, contact->n, BTD_VCARD_N_PARTS);
				contact->has_n = 1;
			}

			break;

		case VCARD_NAME_TEL:
			/* A number past the room is left out. */
			if (contact->tel_count >= BTD_VCARD_TELS_MAX) {
				contact->tels_dropped++;
				break;
			}

			/* A number with no digits, or one too long, is left out. */
			tel = &contact->tels[contact->tel_count];
			error = vcard_number(&reader, &property, tel->number, sizeof(tel->number));
			if (error != 0) {
				memset(tel, 0, sizeof(*tel));
				contact->tels_dropped++;
				break;
			}

			/* The number is kept with its types. */
			tel->types = property.types & VCARD_TYPE_TEL_MASK;
			contact->tel_count++;
			break;

		case VCARD_NAME_UID:
			/* The first UID that says something. */
			if (contact->uid[0] == '\0')
				vcard_text(&reader, &property, contact->uid, sizeof(contact->uid));
			break;

		default:
			/* VERSION was read before; the history's datetime is not a contact's. */
			break;
		}
	}

	/* The name to show: FN, else the name N makes. */
	if (contact->formatted[0] != '\0') {
		memcpy(contact->name, contact->formatted, sizeof(contact->name));
	} else if (contact->has_n) {
		vcard_name_from_n(n_name, sizeof(n_name), contact->n[0], contact->n[1]);
		memcpy(contact->name, n_name, sizeof(contact->name));
	}

	/* Without a name the first number stands for it. */
	if (contact->name[0] == '\0' && contact->tel_count > 0U)
		memcpy(contact->name, contact->tels[0].number, sizeof(contact->tels[0].number));

	/* A card with neither a name nor a number has nothing to keep. */
	if (contact->name[0] == '\0')
		return ENOENT;

	/* The key the contact is known by. */
	vcard_contact_key(contact);

	/* Succeeded: the contact is read. */
	return 0;
}

/*
 * Writes contact as a reduced vCard 3.0: BEGIN, VERSION, FN (the name to
 * show), N (when the card had one), each number with its types, UID
 * (when it had one) and END, each line ending in CRLF, texts escaped as
 * 3.0 asks, no line folded, and a NUL after it.  Returns 0 with the bytes
 * written (the NUL not counted), or ENOBUFS when size has no room.
 */
int
btd_vcard_reduce(
	const struct btd_vcard_contact *contact,
	char *output,
	size_t size,
	size_t *used)
{
	size_t part;
	size_t tel;
	int error;

	/* Nothing written yet. */
	*used = 0U;

	/* The card opens, and names its version. */
	error = vcard_put_string(output, size, used, "BEGIN:VCARD\r\nVERSION:3.0\r\n");
	if (error != 0)
		return ENOBUFS;

	/* The name to show. */
	error = vcard_put_string(output, size, used, "FN:");
	if (error != 0)
		return ENOBUFS;
	error = vcard_put_escaped(output, size, used, contact->name);
	if (error != 0)
		return ENOBUFS;
	error = vcard_put_string(output, size, used, "\r\n");
	if (error != 0)
		return ENOBUFS;

	/* The parts of N, when the card had one. */
	if (contact->has_n) {
		/* The property's name. */
		error = vcard_put_string(output, size, used, "N:");
		if (error != 0)
			return ENOBUFS;

		/* Each part, a ';' between two. */
		for (part = 0U; part < BTD_VCARD_N_PARTS; part++) {
			/* The ';' before every part but the first. */
			if (part > 0U) {
				error = vcard_put_string(output, size, used, ";");
				if (error != 0)
					return ENOBUFS;
			}

			/* The part itself. */
			error = vcard_put_escaped(output, size, used, contact->n[part]);
			if (error != 0)
				return ENOBUFS;
		}

		/* The end of the line. */
		error = vcard_put_string(output, size, used, "\r\n");
		if (error != 0)
			return ENOBUFS;
	}

	/* Each number, with its types. */
	for (tel = 0U; tel < contact->tel_count; tel++) {
		error = vcard_put_tel(output, size, used, &contact->tels[tel]);
		if (error != 0)
			return ENOBUFS;
	}

	/* The UID, when the card had one. */
	if (contact->uid[0] != '\0') {
		/* The property's name. */
		error = vcard_put_string(output, size, used, "UID:");
		if (error != 0)
			return ENOBUFS;

		/* The UID and the end of its line. */
		error = vcard_put_escaped(output, size, used, contact->uid);
		if (error != 0)
			return ENOBUFS;
		error = vcard_put_string(output, size, used, "\r\n");
		if (error != 0)
			return ENOBUFS;
	}

	/* The card closes. */
	error = vcard_put_string(output, size, used, "END:VCARD\r\n");
	if (error != 0)
		return ENOBUFS;

	/* The NUL after it, not counted. */
	if (*used >= size)
		return ENOBUFS;
	output[*used] = '\0';

	/* Succeeded: the reduced vCard is written. */
	return 0;
}

/*
 * Reads one card of the call history into call.  folder_kind is the kind
 * the folder it came from says (BTD_VCARD_CALL_*); the parameters of the
 * card's X-IRMC-CALL-DATETIME say it instead when they name one.
 * Returns 0, EINVAL for a card malformed (as btd_vcard_contact_read), or
 * E2BIG for one past BTD_VCARD_CARD_MAX.
 */
int
btd_vcard_call_read(
	const uint8_t *card,
	size_t length,
	int folder_kind,
	struct btd_vcard_call *call)
{
	struct vcard_reader reader;
	struct vcard_property property;
	char formatted[BTD_VCARD_TEXT_SIZE];
	char parts[2][BTD_VCARD_TEXT_SIZE];
	int has_n;
	int seen_datetime;
	int error;
	const char *kind_word;
	uint64_t hash;

	/* Nothing known of the call yet. */
	memset(call, 0, sizeof(*call));
	memset(formatted, 0, sizeof(formatted));
	memset(parts, 0, sizeof(parts));
	has_n = 0;
	seen_datetime = 0;

	/* A card within the size the daemon reads. */
	if (length > BTD_VCARD_CARD_MAX)
		return E2BIG;

	/* The folder's kind, when it is one. */
	call->kind = BTD_VCARD_CALL_UNKNOWN;
	if (folder_kind >= BTD_VCARD_CALL_RECEIVED && folder_kind <= BTD_VCARD_CALL_MISSED)
		call->kind = folder_kind;

	/* The reading starts at the card's first line. */
	vcard_reader_start(&reader, card, length);
	call->version = reader.version;

	/* Each property of the card itself, until the card closes. */
	for (;;) {
		/* The card closed: everything is read. */
		error = vcard_property_next(&reader, &property);
		if (error == ENOENT)
			break;

		/* A card malformed. */
		if (error != 0) {
			memset(call, 0, sizeof(*call));
			return EINVAL;
		}

		/* A property whose value cannot be used is passed over. */
		if (!property.usable)
			continue;

		/* The property, by its name. */
		switch (property.name) {
		case VCARD_NAME_DATETIME:
			/* Only the first datetime counts. */
			if (seen_datetime)
				break;
			seen_datetime = 1;

			/* The kind its parameters name, over the folder's. */
			if ((property.types & VCARD_TYPE_MISSED) != 0U) {
				call->kind = BTD_VCARD_CALL_MISSED;
			} else if ((property.types & VCARD_TYPE_RECEIVED) != 0U) {
				call->kind = BTD_VCARD_CALL_RECEIVED;
			} else if ((property.types & VCARD_TYPE_DIALED) != 0U) {
				call->kind = BTD_VCARD_CALL_DIALED;
			}

			/* The datetime as the phone wrote it. */
			vcard_datetime(&reader, &property, call->datetime, sizeof(call->datetime));
			break;

		case VCARD_NAME_TEL:
			/* The first number that has digits; a withheld one leaves it empty. */
			if (call->number[0] == '\0') {
				error = vcard_number(&reader, &property, call->number, sizeof(call->number));
				if (error != 0)
					call->number[0] = '\0';
			}

			break;

		case VCARD_NAME_FN:
			/* The first FN that says something. */
			if (formatted[0] == '\0')
				vcard_text(&reader, &property, formatted, sizeof(formatted));
			break;

		case VCARD_NAME_N:
			/* The family and given names of the first N. */
			if (!has_n) {
				vcard_parts(&reader, &property, parts, 2U);
				has_n = 1;
			}

			break;

		default:
			/* Nothing else is a call's. */
			break;
		}
	}

	/* The name the phone gave: FN, else the name N makes, else none. */
	if (formatted[0] != '\0') {
		memcpy(call->name, formatted, sizeof(call->name));
	} else if (has_n) {
		vcard_name_from_n(call->name, sizeof(call->name), parts[0], parts[1]);
	}

	/* The letter of the kind the key starts with. */
	kind_word = "u|";
	if (call->kind == BTD_VCARD_CALL_RECEIVED) {
		kind_word = "r|";
	} else if (call->kind == BTD_VCARD_CALL_DIALED) {
		kind_word = "d|";
	} else if (call->kind == BTD_VCARD_CALL_MISSED) {
		kind_word = "m|";
	}

	/*
	 * The key: the kind, the datetime and the number.  Two calls of one
	 * kind from one number with no datetime share it (the history has
	 * nothing else that tells them apart from one reading to the next).
	 */
	hash = vcard_fnv(VCARD_FNV_BASIS, kind_word, 2U);
	hash = vcard_fnv(hash, call->datetime, strlen(call->datetime));
	hash = vcard_fnv(hash, "|", 1U);
	hash = vcard_fnv(hash, call->number, strlen(call->number));
	vcard_key_text(hash, call->key);

	/* Succeeded: the call is read. */
	return 0;
}

/*
 * Turns a call's datetime into seconds since 1970 UTC: by the zone it
 * gives itself (a Z or an offset), else as the phone's local time taken
 * in zedBSD's zone (local_offset seconds east of UTC; the phone's own
 * zone is not known in version 1.1).  Returns 0 with the seconds and the
 * zone (BTD_VCARD_ZONE_PHONE or _LOCAL), ENOENT when the call has no
 * datetime, or EINVAL when it cannot be read; both with seconds 0 and
 * BTD_VCARD_ZONE_NONE.
 */
int
btd_vcard_call_time(
	const struct btd_vcard_call *call,
	int32_t local_offset,
	int64_t *seconds,
	int *zone)
{
	size_t length;
	int from;
	int error;

	/* No time known yet. */
	*seconds = 0;
	*zone = BTD_VCARD_ZONE_NONE;

	/* A call the phone gave no datetime. */
	length = strlen(call->datetime);
	if (length == 0U)
		return ENOENT;

	/* The datetime read with the offset that applies to it. */
	error = btd_mapxml_time_unix(call->datetime, length, 0, 0, local_offset, seconds, &from);
	if (error != 0) {
		*seconds = 0;
		return EINVAL;
	}

	/* Which zone the seconds were read in. */
	*zone = BTD_VCARD_ZONE_PHONE;
	if (from == BTD_MAP_FROM_LOCAL)
		*zone = BTD_VCARD_ZONE_LOCAL;

	/* Succeeded: the seconds since 1970 UTC. */
	return 0;
}

/*
 * Finds the line of bytes that starts at at.  Returns 0 with it, or
 * ENOENT when at is the end of the input.
 */
static int
vcard_physical_line(
	const uint8_t *bytes,
	size_t length,
	size_t at,
	struct vcard_line *line)
{
	const uint8_t *newline;

	/* No line left. */
	if (at >= length)
		return ENOENT;

	/* The line ends at its line feed, or at the end of the input. */
	line->start = at;
	newline = memchr(bytes + at, '\n', length - at);
	if (newline == NULL) {
		line->end = length;
		line->next = length;
	} else {
		line->end = (size_t)(newline - bytes);
		line->next = line->end + 1U;
	}

	/* A carriage return before the line feed is not part of the text. */
	if (line->end > line->start && bytes[line->end - 1U] == '\r')
		line->end--;

	/* Succeeded: the line is found. */
	return 0;
}

/* Says whether bytes from start to end are word, spaces and TABs around them aside, in either case. */
static int
vcard_same(
	const uint8_t *bytes,
	size_t start,
	size_t end,
	const char *word)
{
	size_t length;
	size_t index;
	uint8_t left;
	uint8_t right;

	/* The run without the spaces around it. */
	vcard_trim(bytes, &start, &end);

	/* A run of another length. */
	length = strlen(word);
	if (end - start != length)
		return 0;

	/* Each byte, its case aside. */
	for (index = 0U; index < length; index++) {
		/* Upper case for the comparison. */
		left = bytes[start + index];
		right = (uint8_t)word[index];
		if (left >= 'a' && left <= 'z')
			left = (uint8_t)(left - 'a' + 'A');
		if (right >= 'a' && right <= 'z')
			right = (uint8_t)(right - 'a' + 'A');

		/* A byte that differs. */
		if (left != right)
			return 0;
	}

	/* Succeeded: the same word. */
	return 1;
}

/* Moves start and end inward past the spaces and TABs around a run of bytes. */
static void
vcard_trim(
	const uint8_t *bytes,
	size_t *start,
	size_t *end)
{
	int space;

	/* The spaces before the run. */
	while (*start < *end) {
		/* A byte that is not a space starts the run. */
		space = vcard_space(bytes[*start]);
		if (!space)
			break;
		(*start)++;
	}

	/* The spaces after it. */
	while (*end > *start) {
		/* A byte that is not a space ends the run. */
		space = vcard_space(bytes[*end - 1U]);
		if (!space)
			break;
		(*end)--;
	}
}

/* Finds the value of the word that bytes from start to end are, or 0 for a word not in words. */
static unsigned
vcard_word(
	const struct vcard_word *words,
	const uint8_t *bytes,
	size_t start,
	size_t end)
{
	size_t index;
	int same;

	/* Each word the table knows. */
	for (index = 0U; words[index].name != NULL; index++) {
		/* The word found. */
		same = vcard_same(bytes, start, end, words[index].name);
		if (same)
			return words[index].value;
	}

	/* A word not known. */
	return 0U;
}

/*
 * Reads the part of a line before its value: the property's name (its
 * group, up to the last '.', left out), and its parameters, each split
 * at ';' outside double quotes, up to the first ':' outside double
 * quotes.  Returns 0 with head, or EINVAL for a line without that ':'.
 */
static int
vcard_head(
	const uint8_t *bytes,
	size_t length,
	struct vcard_head *head)
{
	size_t colon;
	size_t name_end;
	size_t name_start;
	size_t index;
	size_t parameter_start;
	int quoted;
	int found;

	/* Nothing known yet: a value as it is, in a character set the reader takes. */
	memset(head, 0, sizeof(*head));
	head->name = VCARD_NAME_OTHER;
	head->encoding = VCARD_ENCODING_PLAIN;
	head->charset_ok = 1;

	/* The ':' that ends the head, outside double quotes. */
	quoted = 0;
	found = 0;
	colon = 0U;
	for (index = 0U; index < length; index++) {
		/* A double quote opens or closes a quoted parameter value. */
		if (bytes[index] == '"') {
			quoted = !quoted;
			continue;
		}

		/* The first ':' outside quotes. */
		if (bytes[index] == ':' && !quoted) {
			colon = index;
			found = 1;
			break;
		}
	}

	/* A line without a value. */
	if (!found)
		return EINVAL;
	head->value_start = colon + 1U;

	/* The name, up to the first ';' or the ':'. */
	name_end = colon;
	for (index = 0U; index < colon; index++) {
		/* The first ';' ends the name. */
		if (bytes[index] == ';') {
			name_end = index;
			break;
		}
	}

	/* The group before the name, up to its last '.', is left out. */
	name_start = 0U;
	for (index = 0U; index < name_end; index++) {
		/* A '.' ends the group so far. */
		if (bytes[index] == '.')
			name_start = index + 1U;
	}

	/* The property the name names. */
	head->name = vcard_word(vcard_names, bytes, name_start, name_end);

	/* Each parameter, between ';' outside quotes, up to the ':'. */
	quoted = 0;
	parameter_start = name_end + 1U;
	for (index = name_end + 1U; index <= colon; index++) {
		/* A double quote opens or closes a quoted parameter value. */
		if (index < colon && bytes[index] == '"') {
			quoted = !quoted;
			continue;
		}

		/* A ';' outside quotes, or the ':', ends the parameter. */
		if (index == colon || (bytes[index] == ';' && !quoted)) {
			vcard_parameter(bytes, parameter_start, index, head);
			parameter_start = index + 1U;
		}
	}

	/* Succeeded: the head is read. */
	return 0;
}

/*
 * Reads one parameter into head: TYPE (a list split at ','), ENCODING,
 * CHARSET, or a bare word of 2.1 that is an encoding or a type.  Other
 * parameters are passed over.
 */
static void
vcard_parameter(
	const uint8_t *bytes,
	size_t start,
	size_t end,
	struct vcard_head *head)
{
	const uint8_t *equals;
	size_t key_end;
	size_t value_start;
	size_t value_end;
	unsigned encoding;
	int is_type;
	int is_encoding;
	int is_charset;
	int utf8;
	int utf8_short;

	/* An empty parameter says nothing. */
	vcard_trim(bytes, &start, &end);
	if (start == end)
		return;

	/* A bare word of 2.1: an encoding, or else a type. */
	equals = memchr(bytes + start, '=', end - start);
	if (equals == NULL) {
		encoding = vcard_word(vcard_encodings, bytes, start, end);
		if (encoding != 0U) {
			head->encoding = encoding;
		} else {
			vcard_type_list(bytes, start, end, head);
		}

		/* A bare word has no value to read. */
		return;
	}

	/* The parameter's name, and its value without the double quotes around it. */
	key_end = (size_t)(equals - bytes);
	value_start = key_end + 1U;
	value_end = end;
	vcard_trim(bytes, &value_start, &value_end);
	if (value_end - value_start >= 2U &&
	    bytes[value_start] == '"' &&
	    bytes[value_end - 1U] == '"') {
		value_start++;
		value_end--;
	}

	/* Which parameter it is. */
	is_type = vcard_same(bytes, start, key_end, "TYPE");
	is_encoding = vcard_same(bytes, start, key_end, "ENCODING");
	is_charset = vcard_same(bytes, start, key_end, "CHARSET");

	/* The types it lists. */
	if (is_type) {
		vcard_type_list(bytes, value_start, value_end, head);
		return;
	}

	/* The way the value is written; one not known cannot be read. */
	if (is_encoding) {
		encoding = vcard_word(vcard_encodings, bytes, value_start, value_end);
		head->encoding = VCARD_ENCODING_UNUSABLE;
		if (encoding != 0U)
			head->encoding = encoding;
		return;
	}

	/* A character set: only UTF-8 is taken (PBAP section 3.1.4). */
	if (is_charset) {
		utf8 = vcard_same(bytes, value_start, value_end, "UTF-8");
		utf8_short = vcard_same(bytes, value_start, value_end, "UTF8");
		head->charset_ok = 0;
		if (utf8 || utf8_short)
			head->charset_ok = 1;
	}
}

/* Adds to head the types a list split at ',' names; words not known are passed over. */
static void
vcard_type_list(
	const uint8_t *bytes,
	size_t start,
	size_t end,
	struct vcard_head *head)
{
	size_t index;
	size_t word_start;

	/* Each word, up to a ',' or the list's end. */
	word_start = start;
	for (index = start; index <= end; index++) {
		/* A ',' or the end ends the word. */
		if (index == end || bytes[index] == ',') {
			head->types |= vcard_word(vcard_types, bytes, word_start, index);
			word_start = index + 1U;
		}
	}
}

/*
 * Finds the version a card names on its first VERSION line (a line that
 * does not start with a space, so not a fold).  Returns
 * BTD_VCARD_VERSION_21, _30 (also for 4.0, which folds as 3.0 does), or
 * _UNKNOWN.
 */
static int
vcard_version_scan(
	const uint8_t *bytes,
	size_t length)
{
	struct vcard_line line;
	struct vcard_head head;
	size_t at;
	size_t value_start;
	size_t value_end;
	int same;
	int error;

	/* Each line of the card. */
	at = 0U;
	for (;;) {
		/* No VERSION line. */
		error = vcard_physical_line(bytes, length, at, &line);
		if (error != 0)
			return BTD_VCARD_VERSION_UNKNOWN;
		at = line.next;

		/* An empty line, or a fold, is not a VERSION line. */
		if (line.end == line.start)
			continue;
		if (bytes[line.start] == ' ' || bytes[line.start] == '\t')
			continue;

		/* A line that is not a property, or not VERSION. */
		error = vcard_head(bytes + line.start, line.end - line.start, &head);
		if (error != 0)
			continue;
		if (head.name != VCARD_NAME_VERSION)
			continue;

		/* The version it names. */
		value_start = line.start + head.value_start;
		value_end = line.end;
		same = vcard_same(bytes, value_start, value_end, "2.1");
		if (same)
			return BTD_VCARD_VERSION_21;
		same = vcard_same(bytes, value_start, value_end, "3.0");
		if (same)
			return BTD_VCARD_VERSION_30;
		same = vcard_same(bytes, value_start, value_end, "4.0");
		if (same)
			return BTD_VCARD_VERSION_30;

		/* A version not known. */
		return BTD_VCARD_VERSION_UNKNOWN;
	}
}

/* Starts the reading of a card at its first line. */
static void
vcard_reader_start(
	struct vcard_reader *reader,
	const uint8_t *card,
	size_t length)
{
	/* The input, its version, and nothing read yet. */
	reader->bytes = card;
	reader->length = length;
	reader->at = 0U;
	reader->version = vcard_version_scan(card, length);
	reader->depth = 0;
	reader->line_length = 0U;
	reader->line_overflow = 0;
}

/*
 * Adds bytes from start to end to the line being read.  What does not
 * fit is left out and marks the line overflowed: its head is still read
 * (so the property is known and counted), its value is not.
 */
static void
vcard_line_append(
	struct vcard_reader *reader,
	const uint8_t *bytes,
	size_t start,
	size_t end)
{
	size_t length;
	size_t room;

	/* A line already past its room keeps nothing more. */
	length = end - start;
	if (reader->line_overflow)
		return;

	/* A part that does not fit is cut at the room, and the line is too long to be read. */
	room = BTD_VCARD_LINE_MAX - reader->line_length;
	if (length > room) {
		length = room;
		reader->line_overflow = 1;
	}

	/* The part goes on the line. */
	memcpy(reader->line + reader->line_length, bytes + start, length);
	reader->line_length += length;
}

/* Says whether the line read so far has a head that writes its value in quoted-printable. */
static int
vcard_line_is_qp(
	const struct vcard_reader *reader)
{
	struct vcard_head head;
	int error;

	/* A line whose head is not complete yet. */
	error = vcard_head(reader->line, reader->line_length, &head);
	if (error != 0)
		return 0;

	/* A value in quoted-printable. */
	if (head.encoding == VCARD_ENCODING_QP)
		return 1;

	/* A value written as it is. */
	return 0;
}

/*
 * Reads the next line of the card with what continues it joined: the
 * lines after a quoted-printable soft line break (a line ending in '='),
 * and the folds (lines starting with a space or a TAB; in 3.0 that one
 * space is not part of the text, in 2.1 it is).  Returns 0, or ENOENT
 * when no line is left.
 */
static int
vcard_logical_line(
	struct vcard_reader *reader)
{
	struct vcard_line line;
	size_t skip;
	int soft;
	int qp;
	int error;

	/* A new, empty line. */
	reader->line_length = 0U;
	reader->line_overflow = 0;

	/* The line's first part. */
	error = vcard_physical_line(reader->bytes, reader->length, reader->at, &line);
	if (error != 0)
		return ENOENT;
	vcard_line_append(reader, reader->bytes, line.start, line.end);
	reader->at = line.next;

	/* The parts that continue it. */
	for (;;) {
		/* A soft line break: the part ends in '=' and the value is in quoted-printable. */
		soft = 0;
		if (line.end > line.start && reader->bytes[line.end - 1U] == '=') {
			qp = vcard_line_is_qp(reader);
			if (qp)
				soft = 1;
		}

		/* The '=' goes, and the next line goes on as it is. */
		if (soft) {
			error = vcard_physical_line(reader->bytes, reader->length, reader->at, &line);
			if (error != 0)
				break;
			if (!reader->line_overflow)
				reader->line_length--;
			vcard_line_append(reader, reader->bytes, line.start, line.end);
			reader->at = line.next;
			continue;
		}

		/* A line that is not a fold ends this one. */
		if (reader->at >= reader->length)
			break;
		if (reader->bytes[reader->at] != ' ' && reader->bytes[reader->at] != '\t')
			break;

		/* The fold goes on, in 3.0 without its first space. */
		error = vcard_physical_line(reader->bytes, reader->length, reader->at, &line);
		if (error != 0)
			break;
		skip = 1U;
		if (reader->version == BTD_VCARD_VERSION_21)
			skip = 0U;
		vcard_line_append(reader, reader->bytes, line.start + skip, line.end);
		reader->at = line.next;
	}

	/* Succeeded: the line is read. */
	return 0;
}

/*
 * Reads the next property that belongs to the card itself (not to a card
 * nested in it), with its value undone from quoted-printable into the
 * reader's value room.  Returns 0 with property, ENOENT when the card
 * has closed, or EINVAL for a card malformed: a first line that does not
 * open a card, an end before the card closes, or cards nested deeper
 * than VCARD_DEPTH_MAX.
 */
static int
vcard_property_next(
	struct vcard_reader *reader,
	struct vcard_property *property)
{
	struct vcard_head head;
	size_t value_length;
	int is_card;
	int error;

	/* Each line, until one is a property of the card itself. */
	for (;;) {
		/* The card has already closed. */
		if (reader->depth < 0)
			return ENOENT;

		/* An input that ends before the card closes. */
		error = vcard_logical_line(reader);
		if (error != 0)
			return EINVAL;

		/* An empty line says nothing. */
		if (reader->line_length == 0U && !reader->line_overflow)
			continue;

		/* A line without a value: refused before the card opens, passed over inside it. */
		error = vcard_head(reader->line, reader->line_length, &head);
		if (error != 0) {
			if (reader->depth == 0)
				return EINVAL;
			continue;
		}

		/* Whether the line opens or closes a card. */
		is_card = 0;
		if (head.name == VCARD_NAME_BEGIN || head.name == VCARD_NAME_END)
			is_card = vcard_same(reader->line, head.value_start, reader->line_length, "VCARD");

		/* The first line must open the card. */
		if (reader->depth == 0) {
			if (head.name != VCARD_NAME_BEGIN || !is_card)
				return EINVAL;
			reader->depth = 1;
			continue;
		}

		/* A card nested in this one opens. */
		if (head.name == VCARD_NAME_BEGIN && is_card) {
			if (reader->depth >= VCARD_DEPTH_MAX)
				return EINVAL;
			reader->depth++;
			continue;
		}

		/* A card closes; when it is this one, the reading ends (a depth below 0 says so). */
		if (head.name == VCARD_NAME_END && is_card) {
			reader->depth--;
			if (reader->depth == 0) {
				reader->depth = -1;
				return ENOENT;
			}

			continue;
		}

		/* A property of a nested card, or one not kept. */
		if (reader->depth != 1)
			continue;
		if (head.name == VCARD_NAME_OTHER)
			continue;
		break;
	}

	/* The property, usable when its line fit and its value is UTF-8 text. */
	memset(property, 0, sizeof(*property));
	property->name = head.name;
	property->types = head.types;
	property->usable = 1;
	if (reader->line_overflow)
		property->usable = 0;
	if (!head.charset_ok)
		property->usable = 0;
	if (head.encoding == VCARD_ENCODING_UNUSABLE)
		property->usable = 0;

	/* A property that cannot be used has no value. */
	if (!property->usable)
		return 0;

	/* The value, undone from quoted-printable when written in it. */
	value_length = reader->line_length - head.value_start;
	if (head.encoding == VCARD_ENCODING_QP) {
		property->value_length = vcard_qp_decode(reader->line + head.value_start, value_length, reader->value);
	} else {
		memcpy(reader->value, reader->line + head.value_start, value_length);
		property->value_length = value_length;
	}

	/* Succeeded: a property of the card. */
	return 0;
}

/*
 * Undoes quoted-printable: each '=' and two hexadecimal digits becomes
 * the byte they name; an '=' not followed by two such digits stays as it
 * is.  The output is never longer than the input.  Returns its length.
 */
static size_t
vcard_qp_decode(
	const uint8_t *input,
	size_t length,
	uint8_t *output)
{
	size_t at;
	size_t used;
	int high;
	int low;

	/* Each byte of the input. */
	used = 0U;
	at = 0U;
	while (at < length) {
		/* An '=' with two hexadecimal digits after it is one byte. */
		if (input[at] == '=' && length - at >= 3U) {
			high = vcard_hex_digit(input[at + 1U]);
			low = vcard_hex_digit(input[at + 2U]);
			if (high >= 0 && low >= 0) {
				output[used] = (uint8_t)(high * 16 + low);
				used++;
				at += 3U;
				continue;
			}
		}

		/* Any other byte is itself. */
		output[used] = input[at];
		used++;
		at++;
	}

	/* The length of what was undone. */
	return used;
}

/* Finds the value of a hexadecimal digit, either case, or -1 for a byte that is not one. */
static int
vcard_hex_digit(
	uint8_t byte)
{
	/* A decimal digit. */
	if (byte >= '0' && byte <= '9')
		return byte - '0';

	/* A letter of either case. */
	if (byte >= 'A' && byte <= 'F')
		return byte - 'A' + 10;
	if (byte >= 'a' && byte <= 'f')
		return byte - 'a' + 10;

	/* Not a digit. */
	return -1;
}

/*
 * Undoes the escapes of a text in place, from start to end: \n and \N are
 * a line break, and \, \; \\ and \: the byte after the backslash; any
 * other backslash stays.  Returns where the text now ends.
 */
static size_t
vcard_unescape(
	uint8_t *bytes,
	size_t start,
	size_t end)
{
	size_t in;
	size_t out;
	uint8_t next;

	/* Each byte of the text. */
	out = start;
	for (in = start; in < end; in++) {
		/* An escape: a backslash and the byte after it. */
		if (bytes[in] == '\\' && in + 1U < end) {
			next = bytes[in + 1U];
			if (next == 'n' || next == 'N') {
				bytes[out] = '\n';
				out++;
				in++;
				continue;
			}

			/* A byte that stands for itself. */
			if (next == ',' ||
			    next == ';' ||
			    next == '\\' ||
			    next == ':') {
				bytes[out] = next;
				out++;
				in++;
				continue;
			}
		}

		/* Any other byte is itself. */
		bytes[out] = bytes[in];
		out++;
	}

	/* Where the text now ends. */
	return out;
}

/*
 * Finds the length of the well-formed UTF-8 sequence at bytes (no
 * overlong form, no surrogate, nothing past U+10FFFF), or 0 when the
 * sequence there is malformed or cut by the end.
 */
static size_t
vcard_utf8_length(
	const uint8_t *bytes,
	size_t available)
{
	uint8_t first;
	uint8_t low;
	uint8_t high;
	size_t length;
	size_t index;

	/* A byte of ASCII is a character of its own. */
	first = bytes[0];
	if (first < 0x80U)
		return 1U;

	/* The length the first byte gives, and the range its second byte must be in. */
	low = 0x80U;
	high = 0xbfU;
	if (first >= 0xc2U && first <= 0xdfU) {
		length = 2U;
	} else if (first == 0xe0U) {
		length = 3U;
		low = 0xa0U;
	} else if (first == 0xedU) {
		length = 3U;
		high = 0x9fU;
	} else if (first >= 0xe1U && first <= 0xefU) {
		length = 3U;
	} else if (first == 0xf0U) {
		length = 4U;
		low = 0x90U;
	} else if (first >= 0xf1U && first <= 0xf3U) {
		length = 4U;
	} else if (first == 0xf4U) {
		length = 4U;
		high = 0x8fU;
	} else {
		return 0U;
	}

	/* A sequence the end cuts. */
	if (available < length)
		return 0U;

	/* The second byte, in its range. */
	if (bytes[1] < low || bytes[1] > high)
		return 0U;

	/* The bytes after it, each a continuation byte. */
	for (index = 2U; index < length; index++) {
		/* A byte that does not continue the character. */
		if (bytes[index] < 0x80U || bytes[index] > 0xbfU)
			return 0U;
	}

	/* Succeeded: the length of the character. */
	return length;
}

/* Says whether a byte is a space or a TAB. */
static int
vcard_space(
	uint8_t byte)
{
	/* A space or a TAB. */
	if (byte == ' ')
		return 1;
	if (byte == '\t')
		return 1;

	/* Any other byte. */
	return 0;
}

/* Says whether a byte belongs to a number: a digit, +, * or #. */
static int
vcard_number_byte(
	uint8_t byte)
{
	/* A digit. */
	if (byte >= '0' && byte <= '9')
		return 1;

	/* The signs a number has. */
	if (byte == '+')
		return 1;
	if (byte == '*')
		return 1;
	if (byte == '#')
		return 1;

	/* Any other byte. */
	return 0;
}

/* Says whether a byte belongs to a datetime: a digit, T, Z, + or -. */
static int
vcard_datetime_byte(
	uint8_t byte)
{
	/* A digit. */
	if (byte >= '0' && byte <= '9')
		return 1;

	/* The letters and signs of a datetime. */
	if (byte == 'T')
		return 1;
	if (byte == 'Z')
		return 1;
	if (byte == '+')
		return 1;
	if (byte == '-')
		return 1;

	/* Any other byte. */
	return 0;
}

/*
 * Finds the letter that stands for a byte after a backslash in a vCard
 * 3.0 text: the byte itself for a backslash, ',' and ';', n for a line
 * break, or a NUL for a byte not escaped.
 */
static int
vcard_escape_byte(
	char byte)
{
	/* The bytes that stand for themselves after the backslash. */
	if (byte == '\\')
		return '\\';
	if (byte == ',')
		return ',';
	if (byte == ';')
		return ';';

	/* A line break. */
	if (byte == '\n')
		return 'n';

	/* A byte not escaped. */
	return '\0';
}

/*
 * Writes a text into text (size bytes with its NUL): a malformed UTF-8
 * sequence and a control character become U+FFFD, a line break or a TAB
 * a space, the spaces around it are left out, and it is cut at the room
 * without splitting a character.
 */
static void
vcard_sanitize(
	char *text,
	size_t size,
	const uint8_t *bytes,
	size_t length)
{
	const uint8_t *piece;
	size_t piece_length;
	size_t at;
	size_t used;
	int space;
	uint8_t byte;

	/* The spaces before the text. */
	at = 0U;
	while (at < length) {
		/* A byte that is not a space or a line break starts the text. */
		byte = bytes[at];
		if (byte != '\r' && byte != '\n') {
			space = vcard_space(byte);
			if (!space)
				break;
		}

		/* The byte is passed over. */
		at++;
	}

	/* Each character, while it fits. */
	used = 0U;
	while (at < length) {
		/* The bytes this character becomes. */
		byte = bytes[at];
		if (byte == '\n' || byte == '\r' || byte == '\t') {
			piece = (const uint8_t *)" ";
			piece_length = 1U;
			at++;
		} else if (byte < 0x20U || byte == 0x7fU) {
			piece = (const uint8_t *)VCARD_REPLACEMENT;
			piece_length = VCARD_REPLACEMENT_BYTES;
			at++;
		} else {
			piece_length = vcard_utf8_length(bytes + at, length - at);
			piece = bytes + at;
			at += piece_length;
			if (piece_length == 0U) {
				piece = (const uint8_t *)VCARD_REPLACEMENT;
				piece_length = VCARD_REPLACEMENT_BYTES;
				at++;
			}
		}

		/* A character that does not fit ends the text. */
		if (piece_length > size - 1U - used)
			break;

		/* The character is written. */
		memcpy(text + used, piece, piece_length);
		used += piece_length;
	}

	/* The spaces after the text. */
	while (used > 0U && text[used - 1U] == ' ')
		used--;
	text[used] = '\0';
}

/* Writes a property's value as one text, its escapes undone, into text. */
static void
vcard_text(
	struct vcard_reader *reader,
	const struct vcard_property *property,
	char *text,
	size_t size)
{
	size_t end;

	/* The escapes undone in the value room. */
	end = vcard_unescape(reader->value, 0U, property->value_length);

	/* The text written. */
	vcard_sanitize(text, size, reader->value, end);
}

/*
 * Writes the first count parts of a structured value (split at each ';'
 * that no backslash escapes) into parts, each its escapes undone; a part
 * not there is empty, and parts past count are left out.
 */
static void
vcard_parts(
	struct vcard_reader *reader,
	const struct vcard_property *property,
	char (*parts)[BTD_VCARD_TEXT_SIZE],
	size_t count)
{
	size_t starts[BTD_VCARD_N_PARTS];
	size_t ends[BTD_VCARD_N_PARTS];
	size_t found;
	size_t index;
	size_t end;

	/* No part found yet. */
	for (index = 0U; index < count; index++)
		parts[index][0] = '\0';

	/* Where each part starts and ends, at the ';' between two. */
	found = 1U;
	starts[0] = 0U;
	ends[0] = property->value_length;
	for (index = 0U; index < property->value_length; index++) {
		/* An escaped byte is not a ';' between two parts. */
		if (reader->value[index] == '\\') {
			index++;
			continue;
		}

		/* A ';' ends a part and starts the next, while there is room for it. */
		if (reader->value[index] == ';') {
			ends[found - 1U] = index;
			if (found == count)
				break;
			starts[found] = index + 1U;
			ends[found] = property->value_length;
			found++;
		}
	}

	/* Each part, its escapes undone and written as a text. */
	for (index = 0U; index < found; index++) {
		end = vcard_unescape(reader->value, starts[index], ends[index]);
		vcard_sanitize(parts[index], BTD_VCARD_TEXT_SIZE, reader->value + starts[index], end - starts[index]);
	}
}

/*
 * Writes the number a TEL's value holds into number: its digits and the
 * + * # it has, in order, everything else left out (a tel: URI's scheme
 * and the parameters after its ';' too).  Returns 0, EINVAL for a value
 * without one, or E2BIG for one longer than size allows.
 */
static int
vcard_number(
	struct vcard_reader *reader,
	const struct vcard_property *property,
	char *number,
	size_t size)
{
	size_t at;
	size_t end;
	size_t used;
	int uri;
	int kept;
	uint8_t byte;

	/* Nothing written yet. */
	number[0] = '\0';

	/* The value with its escapes undone, the spaces around it aside. */
	end = vcard_unescape(reader->value, 0U, property->value_length);
	at = 0U;
	vcard_trim(reader->value, &at, &end);

	/* A tel: URI starts with its scheme, and its parameters follow a ';'. */
	uri = 0;
	if (end - at >= 4U) {
		uri = vcard_same(reader->value, at, at + 4U, "tel:");
		if (uri)
			at += 4U;
	}

	/* Each byte that belongs to the number. */
	used = 0U;
	for (; at < end; at++) {
		/* The parameters of a URI are not the number. */
		byte = reader->value[at];
		if (uri && byte == ';')
			break;

		/* Only digits and + * # are kept. */
		kept = vcard_number_byte(byte);
		if (!kept)
			continue;

		/* A number longer than the room. */
		if (used + 1U >= size) {
			number[0] = '\0';
			return E2BIG;
		}

		/* The byte is kept. */
		number[used] = (char)byte;
		used++;
	}

	/* The number ends. */
	number[used] = '\0';

	/* A value without a number. */
	if (used == 0U)
		return EINVAL;

	/* Succeeded: the number is written. */
	return 0;
}

/*
 * Writes the datetime of a call as the phone wrote it into datetime,
 * the spaces around it aside; one too long for the room, or with a byte
 * no datetime has (a digit, T, Z, + or -), leaves it empty.
 */
static void
vcard_datetime(
	const struct vcard_reader *reader,
	const struct vcard_property *property,
	char *datetime,
	size_t size)
{
	size_t start;
	size_t end;
	size_t index;
	int taken;

	/* Nothing written yet. */
	datetime[0] = '\0';

	/* The value without the spaces around it, within the room. */
	start = 0U;
	end = property->value_length;
	vcard_trim(reader->value, &start, &end);
	if (end - start >= size)
		return;

	/* Each byte, one a datetime has. */
	for (index = start; index < end; index++) {
		/* A byte that no datetime has. */
		taken = vcard_datetime_byte(reader->value[index]);
		if (!taken)
			return;
	}

	/* The datetime written. */
	memcpy(datetime, reader->value + start, end - start);
	datetime[end - start] = '\0';
}

/*
 * Writes the name an N makes into text: the given name, a space and the
 * family name, or the one of them that is there.  It is cut at the room
 * without splitting a character.
 */
static void
vcard_name_from_n(
	char *text,
	size_t size,
	const char *family,
	const char *given)
{
	size_t used;
	size_t given_length;
	size_t family_length;
	size_t kept;

	/* The given name first. */
	given_length = strlen(given);
	kept = btd_mapxml_utf8_cut(given, given_length, size - 1U);
	memcpy(text, given, kept);
	used = kept;

	/* A space between the two names, when both are there and the space fits. */
	family_length = strlen(family);
	if (used > 0U && family_length > 0U && used + 1U < size - 1U) {
		text[used] = ' ';
		used++;
	}

	/* The family name after it, in the room that is left. */
	kept = btd_mapxml_utf8_cut(family, family_length, size - 1U - used);
	memcpy(text + used, family, kept);
	used += kept;

	/* A space that no family name followed. */
	while (used > 0U && text[used - 1U] == ' ')
		used--;
	text[used] = '\0';
}

/* Adds bytes to a 64-bit FNV-1a hash. */
static uint64_t
vcard_fnv(
	uint64_t hash,
	const void *data,
	size_t length)
{
	const uint8_t *bytes;
	size_t index;

	/* Each byte mixed in. */
	bytes = data;
	for (index = 0U; index < length; index++) {
		hash ^= bytes[index];
		hash *= VCARD_FNV_PRIME;
	}

	/* The hash so far. */
	return hash;
}

/* Writes a hash as a key: 16 lower-case hexadecimal digits, the highest first, and a NUL. */
static void
vcard_key_text(
	uint64_t hash,
	char *key)
{
	static const char digits[] = "0123456789abcdef";
	int index;

	/* Each digit, from the highest four bits down. */
	for (index = 0; index < 16; index++)
		key[index] = digits[(hash >> (60 - 4 * index)) & 0x0fU];
	key[16] = '\0';
}

/*
 * Writes the key a contact is known by: from its UID when it has one
 * ("u|" and the UID), else from its name to show and its numbers sorted
 * ("n|", the name, "|", the numbers with ',' between them).  A phone
 * without UIDs makes a contact whose name or numbers change a new one.
 */
static void
vcard_contact_key(
	struct btd_vcard_contact *contact)
{
	size_t order[BTD_VCARD_TELS_MAX];
	size_t index;
	size_t back;
	size_t moving;
	uint64_t hash;
	int compared;

	/* A UID names the contact. */
	if (contact->uid[0] != '\0') {
		hash = vcard_fnv(VCARD_FNV_BASIS, "u|", 2U);
		hash = vcard_fnv(hash, contact->uid, strlen(contact->uid));
		vcard_key_text(hash, contact->key);
		return;
	}

	/* The numbers in sorted order, by insertion. */
	for (index = 0U; index < contact->tel_count; index++) {
		/* The number moves back past every larger one. */
		moving = index;
		back = index;
		while (back > 0U) {
			/* A smaller or equal number before it stops it. */
			compared = strcmp(contact->tels[order[back - 1U]].number, contact->tels[moving].number);
			if (compared <= 0)
				break;
			order[back] = order[back - 1U];
			back--;
		}

		/* The number takes the place it stopped at. */
		order[back] = moving;
	}

	/* The name, then the sorted numbers. */
	hash = vcard_fnv(VCARD_FNV_BASIS, "n|", 2U);
	hash = vcard_fnv(hash, contact->name, strlen(contact->name));
	hash = vcard_fnv(hash, "|", 1U);
	for (index = 0U; index < contact->tel_count; index++) {
		/* A ',' between two numbers. */
		if (index > 0U)
			hash = vcard_fnv(hash, ",", 1U);
		hash = vcard_fnv(hash, contact->tels[order[index]].number, strlen(contact->tels[order[index]].number));
	}

	/* The key written. */
	vcard_key_text(hash, contact->key);
}

/* Adds bytes to the output.  Returns 0, or ENOBUFS when size has no room. */
static int
vcard_put(
	char *output,
	size_t size,
	size_t *used,
	const void *data,
	size_t length)
{
	/* No room. */
	if (length > size - *used)
		return ENOBUFS;

	/* Succeeded: added. */
	memcpy(output + *used, data, length);
	*used += length;
	return 0;
}

/* Adds a string to the output.  Returns 0, or ENOBUFS when size has no room. */
static int
vcard_put_string(
	char *output,
	size_t size,
	size_t *used,
	const char *text)
{
	int error;

	/* The string's bytes. */
	error = vcard_put(output, size, used, text, strlen(text));
	if (error != 0)
		return ENOBUFS;

	/* Succeeded: added. */
	return 0;
}

/*
 * Adds a text to the output escaped as vCard 3.0 asks: a backslash, ','
 * and ';' get a backslash before them, a line break becomes \n.  Returns
 * 0, or ENOBUFS when size has no room.
 */
static int
vcard_put_escaped(
	char *output,
	size_t size,
	size_t *used,
	const char *text)
{
	size_t index;
	char escaped[2];
	int error;

	/* Each byte of the text. */
	for (index = 0U; text[index] != '\0'; index++) {
		/* A byte that has to be escaped goes as a backslash and the letter that stands for it. */
		escaped[0] = '\\';
		escaped[1] = (char)vcard_escape_byte(text[index]);
		if (escaped[1] != '\0') {
			error = vcard_put(output, size, used, escaped, 2U);
		} else {
			error = vcard_put(output, size, used, &text[index], 1U);
		}

		/* No room for it. */
		if (error != 0)
			return ENOBUFS;
	}

	/* Succeeded: added. */
	return 0;
}

/*
 * Adds a number's line to the output: TEL, ";TYPE=" and its types with
 * ',' between them when it has any, ':' and the number.  Returns 0, or
 * ENOBUFS when size has no room.
 */
static int
vcard_put_tel(
	char *output,
	size_t size,
	size_t *used,
	const struct btd_vcard_tel *tel)
{
	size_t index;
	int first;
	int error;

	/* The property's name. */
	error = vcard_put_string(output, size, used, "TEL");
	if (error != 0)
		return ENOBUFS;

	/* Each type the number has, in the table's order. */
	first = 1;
	for (index = 0U; vcard_types[index].name != NULL; index++) {
		/* A type the number does not have, or one that is not a number's. */
		if ((vcard_types[index].value & VCARD_TYPE_TEL_MASK) == 0U)
			continue;
		if ((tel->types & vcard_types[index].value) == 0U)
			continue;

		/* ";TYPE=" before the first, ',' before the others. */
		if (first) {
			error = vcard_put_string(output, size, used, ";TYPE=");
		} else {
			error = vcard_put_string(output, size, used, ",");
		}

		/* No room for it; the types after it go with ','. */
		if (error != 0)
			return ENOBUFS;
		first = 0;

		/* The type's word. */
		error = vcard_put_string(output, size, used, vcard_types[index].name);
		if (error != 0)
			return ENOBUFS;
	}

	/* The number and the end of the line. */
	error = vcard_put_string(output, size, used, ":");
	if (error != 0)
		return ENOBUFS;
	error = vcard_put_string(output, size, used, tel->number);
	if (error != 0)
		return ENOBUFS;
	error = vcard_put_string(output, size, used, "\r\n");
	if (error != 0)
		return ENOBUFS;

	/* Succeeded: added. */
	return 0;
}
