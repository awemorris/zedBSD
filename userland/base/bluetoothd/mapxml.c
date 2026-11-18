/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The XML of the Message Access Profile and its times (ws197-p003, see
 * mapxml.h).
 */

#include "userland/base/bluetoothd/mapxml.h"

#include <errno.h>
#include <string.h>

/* The longest element or attribute name the reader takes. */
#define MAPXML_NAME_MAX			64U

/* The highest code point a character reference may name. */
#define MAPXML_CODE_MAX			0x10ffffUL

/* The seconds of a day, and the range of seconds whose year has four digits (0001 to 9999). */
#define MAPXML_DAY_SECONDS		86400
#define MAPXML_FIRST_SECOND		(-62135596800LL)
#define MAPXML_LAST_SECOND		253402300799LL

/* The length of a datetime without its zone, and of an offset (+hhmm). */
#define MAPXML_DATETIME_BYTES		15U
#define MAPXML_OFFSET_BYTES		5U

/* A run of the input: where it starts and how many bytes it has. */
struct mapxml_span {
	size_t start;
	size_t length;
};

/*
 * One attribute of a start tag as written: its name, and its value
 * between the quotes with its references not undone yet.
 */
struct mapxml_attribute {
	struct mapxml_span name;
	struct mapxml_span value;
};

/*
 * One start tag: the element's name, its attributes in the order
 * written, and whether it was closed at once ("/>").  Its spans point
 * into the reader's input and live as long as that input.
 */
struct mapxml_element {
	struct mapxml_span name;
	struct mapxml_attribute attributes[BTD_MAPXML_ATTRIBUTES_MAX];
	size_t attribute_count;
	int empty;
};

/*
 * The reading of one document: the input, where the reading is, and the
 * last attribute value undone (its references replaced by what they
 * stand for, NUL-terminated).
 */
struct mapxml_reader {
	const uint8_t *bytes;
	size_t length;
	size_t at;
	char value[BTD_MAPXML_VALUE_MAX + 1U];
	size_t value_length;
};

/*
 * Takes one child element of the root.  Returns 0 to read on, or the
 * error that fails the document.
 */
typedef int (*mapxml_child_fn)(void *context, struct mapxml_reader *reader, const struct mapxml_element *element);

/*
 * The fill of a listing: where its entries go, how many fit, and what
 * was counted so far (every message, kept or not, counts toward the
 * listing's limit).
 */
struct mapxml_listing {
	struct btd_map_entry *entries;
	size_t capacity;
	struct btd_mapxml_counts *counts;
	size_t messages;
};

/*
 * The fill of an event report: where the event goes, and how many event
 * elements were seen (a report holds exactly one).
 */
struct mapxml_report {
	struct btd_map_event *event;
	size_t events;
};

/*
 * A name the profile gives a value, and the value the daemon keeps for
 * it.
 */
struct mapxml_word {
	const char *name;
	int value;
};

/* The message types a listing or an event names. */
static const struct mapxml_word mapxml_message_types[] = {
	{ "SMS_GSM", BTD_MAP_TYPE_SMS_GSM },
	{ "SMS_CDMA", BTD_MAP_TYPE_SMS_CDMA },
	{ "MMS", BTD_MAP_TYPE_MMS },
	{ "EMAIL", BTD_MAP_TYPE_EMAIL },
	{ "IM", BTD_MAP_TYPE_IM },
	{ NULL, 0 }
};

/* How much of a message a listing says the phone holds. */
static const struct mapxml_word mapxml_receptions[] = {
	{ "complete", BTD_MAP_RECEPTION_COMPLETE },
	{ "fractioned", BTD_MAP_RECEPTION_FRACTIONED },
	{ "notification", BTD_MAP_RECEPTION_NOTIFICATION },
	{ NULL, 0 }
};

/* The entities XML predefines, and the characters they stand for. */
static const struct mapxml_word mapxml_entities[] = {
	{ "lt", '<' },
	{ "gt", '>' },
	{ "amp", '&' },
	{ "quot", '"' },
	{ "apos", '\'' },
	{ NULL, 0 }
};

/* The kinds of event a report names (version 1.0 of the report). */
static const struct mapxml_word mapxml_event_types[] = {
	{ "NewMessage", BTD_MAP_EVENT_NEW_MESSAGE },
	{ "DeliverySuccess", BTD_MAP_EVENT_DELIVERY_SUCCESS },
	{ "SendingSuccess", BTD_MAP_EVENT_SENDING_SUCCESS },
	{ "DeliveryFailure", BTD_MAP_EVENT_DELIVERY_FAILURE },
	{ "SendingFailure", BTD_MAP_EVENT_SENDING_FAILURE },
	{ "MemoryFull", BTD_MAP_EVENT_MEMORY_FULL },
	{ "MemoryAvailable", BTD_MAP_EVENT_MEMORY_AVAILABLE },
	{ "MessageDeleted", BTD_MAP_EVENT_MESSAGE_DELETED },
	{ "MessageShift", BTD_MAP_EVENT_MESSAGE_SHIFT },
	{ NULL, 0 }
};

static int mapxml_document(struct mapxml_reader *reader, const char *root, mapxml_child_fn child, void *context);
static int mapxml_prolog(struct mapxml_reader *reader);
static int mapxml_epilog(struct mapxml_reader *reader);
static int mapxml_comment(struct mapxml_reader *reader);
static int mapxml_declaration(struct mapxml_reader *reader);
static int mapxml_doctype(struct mapxml_reader *reader);
static int mapxml_start_tag(struct mapxml_reader *reader, struct mapxml_element *element);
static int mapxml_attribute(struct mapxml_reader *reader, struct mapxml_element *element);
static int mapxml_end_tag(struct mapxml_reader *reader, const struct mapxml_span *name);
static int mapxml_child_content(struct mapxml_reader *reader, const struct mapxml_element *element);
static int mapxml_name(struct mapxml_reader *reader, struct mapxml_span *name);
static int mapxml_value(struct mapxml_reader *reader, const struct mapxml_span *value);
static int mapxml_reference(struct mapxml_reader *reader, size_t at, size_t end, unsigned long *code, size_t *next);
static int mapxml_put_code(struct mapxml_reader *reader, unsigned long code);
static int mapxml_put_byte(struct mapxml_reader *reader, uint8_t byte);
static void mapxml_skip_space(struct mapxml_reader *reader);
static int mapxml_starts(const struct mapxml_reader *reader, const char *text);
static int mapxml_space(uint8_t byte);
static int mapxml_name_first(uint8_t byte);
static int mapxml_name_byte(uint8_t byte);
static int mapxml_span_is(const struct mapxml_reader *reader, const struct mapxml_span *span, const char *text);
static int mapxml_span_has_colon(const struct mapxml_reader *reader, const struct mapxml_span *span);
static int mapxml_same_word(const char *left, const char *right);
static int mapxml_word(const struct mapxml_word *words, const char *name, int otherwise);
static void mapxml_copy(char *text, size_t size, const struct mapxml_reader *reader);
static int mapxml_handle(const struct mapxml_reader *reader, uint64_t *handle);
static int mapxml_hex_digit(uint8_t byte);
static int mapxml_listing_child(void *context, struct mapxml_reader *reader, const struct mapxml_element *element);
static void mapxml_entry_attribute(struct btd_map_entry *entry, const struct mapxml_reader *reader, const struct mapxml_span *name);
static int mapxml_report_child(void *context, struct mapxml_reader *reader, const struct mapxml_element *element);
static int mapxml_digits(const char *text, size_t count, int *number);
static int mapxml_days_in_month(int year, int month);
static void mapxml_civil_from_days(int64_t days, int *year, int *month, int *day);

/*
 * Reads a message listing (MAP-msg-listing) into entries.  Each message
 * with a handle of 1 to 16 hex digits fills the next entry until
 * capacity entries are filled; a message without one is skipped and the
 * rest are dropped, both counted.  Returns 0 with counts, EINVAL for a
 * document that is not a listing or is malformed, or E2BIG for one past
 * the limits (its size, an element's attributes, a value's length, the
 * number of messages).
 */
int
btd_mapxml_listing(
	const uint8_t *input,
	size_t length,
	struct btd_map_entry *entries,
	size_t capacity,
	struct btd_mapxml_counts *counts)
{
	static struct mapxml_reader reader;
	struct mapxml_listing listing;
	int error;

	/* Nothing counted yet. */
	memset(counts, 0, sizeof(*counts));

	/* A document within the size the daemon reads. */
	if (length > BTD_MAPXML_INPUT_MAX)
		return E2BIG;

	/* The reading starts at the first byte. */
	memset(&reader, 0, sizeof(reader));
	reader.bytes = input;
	reader.length = length;

	/* Where the messages go. */
	listing.entries = entries;
	listing.capacity = capacity;
	listing.counts = counts;
	listing.messages = 0U;

	/* The root and each of its messages. */
	error = mapxml_document(&reader, "MAP-msg-listing", mapxml_listing_child, &listing);
	if (error != 0) {
		memset(counts, 0, sizeof(*counts));
		return error;
	}

	/* Succeeded: the messages kept are counted. */
	return 0;
}

/*
 * Reads an event report (MAP-event-report) that holds one event.
 * Returns 0 with the event, EINVAL for a document that is not a report,
 * is malformed, holds no event or more than one, or names an event
 * without a kind or with a malformed handle, or E2BIG for one past the
 * limits.
 */
int
btd_mapxml_event(
	const uint8_t *input,
	size_t length,
	struct btd_map_event *event)
{
	static struct mapxml_reader reader;
	struct mapxml_report report;
	int error;

	/* Nothing known of the event yet. */
	memset(event, 0, sizeof(*event));

	/* A document within the size the daemon reads. */
	if (length > BTD_MAPXML_INPUT_MAX)
		return E2BIG;

	/* The reading starts at the first byte. */
	memset(&reader, 0, sizeof(reader));
	reader.bytes = input;
	reader.length = length;

	/* Where the event goes. */
	report.event = event;
	report.events = 0U;

	/* The root and its event. */
	error = mapxml_document(&reader, "MAP-event-report", mapxml_report_child, &report);
	if (error != 0) {
		memset(event, 0, sizeof(*event));
		return error;
	}

	/* A report that named no event. */
	if (report.events != 1U) {
		memset(event, 0, sizeof(*event));
		return EINVAL;
	}

	/* Succeeded: the event. */
	return 0;
}

/*
 * Gives how many bytes of UTF-8 text to keep so that at most room bytes
 * remain and no character is cut in two.  Text that is not UTF-8 is cut
 * at room once three continuation bytes have been stepped back over.
 */
size_t
btd_mapxml_utf8_cut(
	const char *text,
	size_t length,
	size_t room)
{
	size_t cut;
	size_t back;

	/* All of it fits. */
	if (length <= room)
		return length;

	/*
	 * Steps back over the continuation bytes (10xxxxxx) of the character
	 * the cut would split, at most the three a character can have.
	 */
	cut = room;
	back = 0U;
	while (cut > 0U &&
	       back < 3U &&
	       ((uint8_t)text[cut] & 0xc0U) == 0x80U) {
		cut--;
		back++;
	}

	/* A run of continuation bytes longer than any character: no character to keep whole. */
	if (((uint8_t)text[cut] & 0xc0U) == 0x80U)
		return room;

	/* Succeeded: the cut before the split character. */
	return cut;
}

/*
 * Reads a datetime of the profile: YYYYMMDDTHHMMSS, then nothing, a Z
 * (UTC) or an offset +hhmm or -hhmm, then any number of NUL bytes.
 * Returns 0 with time, or EINVAL for one malformed or out of range (a
 * second of 60 is a leap second and is taken).
 */
int
btd_mapxml_time_parse(
	const char *text,
	size_t length,
	struct btd_map_time *time)
{
	int hours;
	int minutes;
	int days;
	int error;

	/* Nothing known yet. */
	memset(time, 0, sizeof(*time));

	/* The NUL bytes that end it are not part of it. */
	while (length > 0U && text[length - 1U] == '\0')
		length--;

	/* The date and the time, with a T between them. */
	if (length < MAPXML_DATETIME_BYTES)
		return EINVAL;
	if (text[8] != 'T')
		return EINVAL;

	/* The year. */
	error = mapxml_digits(text, 4U, &time->year);
	if (error != 0)
		return EINVAL;

	/* The month. */
	error = mapxml_digits(text + 4, 2U, &time->month);
	if (error != 0)
		return EINVAL;

	/* The day. */
	error = mapxml_digits(text + 6, 2U, &time->day);
	if (error != 0)
		return EINVAL;

	/* The hour. */
	error = mapxml_digits(text + 9, 2U, &time->hour);
	if (error != 0)
		return EINVAL;

	/* The minute. */
	error = mapxml_digits(text + 11, 2U, &time->minute);
	if (error != 0)
		return EINVAL;

	/* The second. */
	error = mapxml_digits(text + 13, 2U, &time->second);
	if (error != 0)
		return EINVAL;

	/* A year of the common era, and a month of it. */
	if (time->year < 1)
		return EINVAL;
	if (time->month < 1 || time->month > 12)
		return EINVAL;

	/* A day of that month. */
	days = mapxml_days_in_month(time->year, time->month);
	if (time->day < 1 || time->day > days)
		return EINVAL;

	/* A time of day, a leap second taken. */
	if (time->hour > 23 ||
	    time->minute > 59 ||
	    time->second > 60)
		return EINVAL;

	/* Nothing after it: its zone is not said. */
	if (length == MAPXML_DATETIME_BYTES) {
		time->zone = BTD_MAP_ZONE_NONE;
		return 0;
	}

	/* A Z: UTC. */
	if (length == MAPXML_DATETIME_BYTES + 1U && text[MAPXML_DATETIME_BYTES] == 'Z') {
		time->zone = BTD_MAP_ZONE_UTC;
		return 0;
	}

	/* Else an offset, signed, of hours and minutes. */
	if (length != MAPXML_DATETIME_BYTES + MAPXML_OFFSET_BYTES)
		return EINVAL;
	if (text[MAPXML_DATETIME_BYTES] != '+' && text[MAPXML_DATETIME_BYTES] != '-')
		return EINVAL;

	/* The offset's hours. */
	error = mapxml_digits(text + MAPXML_DATETIME_BYTES + 1U, 2U, &hours);
	if (error != 0)
		return EINVAL;

	/* The offset's minutes. */
	error = mapxml_digits(text + MAPXML_DATETIME_BYTES + 3U, 2U, &minutes);
	if (error != 0)
		return EINVAL;

	/* An offset within a day. */
	if (hours > 23 || minutes > 59)
		return EINVAL;

	/* The offset in seconds east of UTC. */
	time->zone = BTD_MAP_ZONE_OFFSET;
	time->offset = (int32_t)(hours * 3600 + minutes * 60);
	if (text[MAPXML_DATETIME_BYTES] == '-')
		time->offset = -time->offset;

	/* Succeeded: a datetime with its offset. */
	return 0;
}

/*
 * Gives the days from 1970-01-01 to a date of the proleptic Gregorian
 * calendar (negative before it).  The date is not checked.
 */
int64_t
btd_mapxml_days_from_civil(
	int year,
	int month,
	int day)
{
	int64_t era;
	int64_t year_of_era;
	int64_t day_of_year;
	int64_t day_of_era;
	int64_t shifted_year;
	int64_t shifted_month;

	/*
	 * Counts the year from March, so that a leap day is the last day of
	 * its year.
	 */
	shifted_year = year;
	if (month <= 2)
		shifted_year--;

	/* The 400-year era of the year and the year within it. */
	if (shifted_year >= 0) {
		era = shifted_year / 400;
	} else {
		era = (shifted_year - 399) / 400;
	}

	/* The year within the era. */
	year_of_era = shifted_year - era * 400;

	/* The day within the year counted from March. */
	if (month > 2) {
		shifted_month = month - 3;
	} else {
		shifted_month = month + 9;
	}

	/* The day within the year from the month's first day. */
	day_of_year = (153 * shifted_month + 2) / 5 + day - 1;

	/* The day within the era. */
	day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

	/* Succeeded: the days from 1970-01-01, which is day 719468 of era 0. */
	return era * 146097 + day_of_era - 719468;
}

/*
 * Turns the phone's datetime into seconds since 1970 UTC: by its own
 * zone when it says one, else by the phone's offset from its MSETime when
 * known (mse_known), else by zedBSD's local offset (the phone is taken
 * to be where zedBSD is).  Returns 0 with the seconds and which offset
 * was used (BTD_MAP_FROM_*), or EINVAL when the datetime cannot be read.
 */
int
btd_mapxml_time_unix(
	const char *text,
	size_t length,
	int mse_known,
	int32_t mse_offset,
	int32_t local_offset,
	int64_t *seconds,
	int *from)
{
	struct btd_map_time time;
	int64_t days;
	int32_t offset;
	int error;

	/* The datetime as written. */
	error = btd_mapxml_time_parse(text, length, &time);
	if (error != 0)
		return EINVAL;

	/* The offset: its own, the phone's, or zedBSD's. */
	if (time.zone == BTD_MAP_ZONE_OFFSET) {
		offset = time.offset;
		*from = BTD_MAP_FROM_PHONE;
	} else if (time.zone == BTD_MAP_ZONE_UTC) {
		offset = 0;
		*from = BTD_MAP_FROM_PHONE;
	} else if (mse_known) {
		offset = mse_offset;
		*from = BTD_MAP_FROM_MSE;
	} else {
		offset = local_offset;
		*from = BTD_MAP_FROM_LOCAL;
	}

	/* The days since 1970, then the seconds of the local time less its offset. */
	days = btd_mapxml_days_from_civil(time.year, time.month, time.day);
	*seconds = days * MAPXML_DAY_SECONDS;
	*seconds += (int64_t)time.hour * 3600 + (int64_t)time.minute * 60 + (int64_t)time.second;
	*seconds -= offset;

	/* Succeeded: the seconds since 1970 UTC. */
	return 0;
}

/*
 * Writes seconds since 1970 UTC as the local time of a zone offset
 * seconds east of UTC, YYYYMMDDTHHMMSS and a NUL (the form the version
 * 1.1 filters take, without an offset).  Returns 0, or EINVAL when size
 * has no room for 16 bytes or the year is not of four digits.
 */
int
btd_mapxml_time_format(
	int64_t seconds,
	int32_t offset,
	char *text,
	size_t size)
{
	int64_t local;
	int64_t days;
	int64_t second_of_day;
	int year;
	int month;
	int day;
	int digit;
	int fields[6];
	size_t widths[6];
	size_t field;
	size_t at;
	size_t place;
	int value;

	/* Room for the datetime and its NUL. */
	if (size < MAPXML_DATETIME_BYTES + 1U)
		return EINVAL;

	/* Seconds near the years with four digits, so adding the offset cannot overflow. */
	if (seconds < MAPXML_FIRST_SECOND - MAPXML_DAY_SECONDS || seconds > MAPXML_LAST_SECOND + MAPXML_DAY_SECONDS)
		return EINVAL;

	/* The local time, of a year with four digits. */
	local = seconds + offset;
	if (local < MAPXML_FIRST_SECOND || local > MAPXML_LAST_SECOND)
		return EINVAL;

	/* The day and the second within it, rounded down before 1970. */
	days = local / MAPXML_DAY_SECONDS;
	second_of_day = local % MAPXML_DAY_SECONDS;
	if (second_of_day < 0) {
		second_of_day += MAPXML_DAY_SECONDS;
		days--;
	}

	/* The date of the day. */
	mapxml_civil_from_days(days, &year, &month, &day);

	/* The fields in the order written, and their widths. */
	fields[0] = year;
	fields[1] = month;
	fields[2] = day;
	fields[3] = (int)(second_of_day / 3600);
	fields[4] = (int)(second_of_day / 60 % 60);
	fields[5] = (int)(second_of_day % 60);
	widths[0] = 4U;
	widths[1] = 2U;
	widths[2] = 2U;
	widths[3] = 2U;
	widths[4] = 2U;
	widths[5] = 2U;

	/* Each field in decimal, the T before the hour. */
	at = 0U;
	for (field = 0U; field < 6U; field++) {
		/* The T between the date and the time. */
		if (field == 3U) {
			text[at] = 'T';
			at++;
		}

		/* The digits from the last. */
		value = fields[field];
		for (place = widths[field]; place > 0U; place--) {
			digit = value % 10;
			text[at + place - 1U] = (char)('0' + digit);
			value /= 10;
		}

		/* Past the field. */
		at += widths[field];
	}

	/* Succeeded: the datetime and its NUL. */
	text[at] = '\0';
	return 0;
}

/*
 * Reads a document: its prolog, the root element named root, each child
 * of the root (given to child), the root's end and the epilog.  Returns
 * 0, the error child gave, EINVAL or E2BIG.
 */
static int
mapxml_document(
	struct mapxml_reader *reader,
	const char *root,
	mapxml_child_fn child,
	void *context)
{
	static struct mapxml_element root_element;
	static struct mapxml_element child_element;
	int same;
	int error;

	/* What comes before the root. */
	error = mapxml_prolog(reader);
	if (error != 0)
		return error;

	/* The root's start tag. */
	error = mapxml_start_tag(reader, &root_element);
	if (error != 0)
		return error;

	/* The root the caller reads. */
	same = mapxml_span_is(reader, &root_element.name, root);
	if (!same)
		return EINVAL;

	/* Each child until the root's end, unless the root is empty. */
	while (!root_element.empty) {
		/* The spaces between children. */
		mapxml_skip_space(reader);

		/* A document that ends inside the root. */
		if (reader->at >= reader->length)
			return EINVAL;

		/* The root's end tag ends the children. */
		same = mapxml_starts(reader, "</");
		if (same) {
			error = mapxml_end_tag(reader, &root_element.name);
			if (error != 0)
				return error;
			break;
		}

		/* A comment between children. */
		same = mapxml_starts(reader, "<!--");
		if (same) {
			error = mapxml_comment(reader);
			if (error != 0)
				return error;
			continue;
		}

		/* Text, CDATA, a processing instruction or a declaration: not of these documents. */
		if (reader->bytes[reader->at] != '<')
			return EINVAL;
		if (reader->at + 1U >= reader->length)
			return EINVAL;
		if (reader->bytes[reader->at + 1U] == '!' || reader->bytes[reader->at + 1U] == '?')
			return EINVAL;

		/* A child's start tag. */
		error = mapxml_start_tag(reader, &child_element);
		if (error != 0)
			return error;

		/* A child's name with a prefix: a namespace these documents do not use. */
		same = mapxml_span_has_colon(reader, &child_element.name);
		if (same)
			return EINVAL;

		/* A child that holds nothing but spaces and comments. */
		if (!child_element.empty) {
			error = mapxml_child_content(reader, &child_element);
			if (error != 0)
				return error;
		}

		/* The child to the caller. */
		error = child(context, reader, &child_element);
		if (error != 0)
			return error;
	}

	/* What comes after the root. */
	error = mapxml_epilog(reader);
	if (error != 0)
		return error;

	/* Succeeded: the document read. */
	return 0;
}

/*
 * Reads up to the root's start tag: a byte order mark, an XML
 * declaration, comments, a DOCTYPE without an internal subset, and
 * spaces.  Returns 0 at the root's '<', or EINVAL.
 */
static int
mapxml_prolog(
	struct mapxml_reader *reader)
{
	int declared;
	int same;
	int error;

	/* The UTF-8 byte order mark. */
	same = mapxml_starts(reader, "\xef\xbb\xbf");
	if (same)
		reader->at += 3U;

	/* Each part of the prolog until the root. */
	declared = 0;
	for (;;) {
		/* The spaces between parts. */
		mapxml_skip_space(reader);

		/* A document without a root. */
		if (reader->at + 1U >= reader->length)
			return EINVAL;

		/* The XML declaration, once. */
		same = mapxml_starts(reader, "<?xml");
		if (same && !declared) {
			error = mapxml_declaration(reader);
			if (error != 0)
				return error;
			declared = 1;
			continue;
		}

		/* A comment. */
		same = mapxml_starts(reader, "<!--");
		if (same) {
			error = mapxml_comment(reader);
			if (error != 0)
				return error;
			continue;
		}

		/* A document type declaration. */
		same = mapxml_starts(reader, "<!DOCTYPE");
		if (same) {
			error = mapxml_doctype(reader);
			if (error != 0)
				return error;
			continue;
		}

		/* Nothing else but the root may come. */
		break;
	}

	/* The root's start tag. */
	if (reader->bytes[reader->at] != '<')
		return EINVAL;
	same = mapxml_name_first(reader->bytes[reader->at + 1U]);
	if (!same)
		return EINVAL;

	/* Succeeded: at the root. */
	return 0;
}

/*
 * Reads what follows the root: spaces and comments, then any number of
 * NUL bytes to the end.  Returns 0 at the end, or EINVAL.
 */
static int
mapxml_epilog(
	struct mapxml_reader *reader)
{
	int same;
	int error;

	/* Each comment after the root. */
	for (;;) {
		/* The spaces between comments. */
		mapxml_skip_space(reader);

		/* A comment. */
		same = mapxml_starts(reader, "<!--");
		if (!same)
			break;
		error = mapxml_comment(reader);
		if (error != 0)
			return error;
	}

	/* The NUL bytes some phones end a body with. */
	while (reader->at < reader->length && reader->bytes[reader->at] == '\0')
		reader->at++;

	/* Nothing else after the root. */
	if (reader->at != reader->length)
		return EINVAL;

	/* Succeeded: the document's end. */
	return 0;
}

/* Skips a comment from its "<!--" past its "-->".  Returns 0, or EINVAL when it does not end. */
static int
mapxml_comment(
	struct mapxml_reader *reader)
{
	int same;

	/* The text of the comment up to its end. */
	reader->at += 4U;
	while (reader->at < reader->length) {
		/* The end of the comment. */
		same = mapxml_starts(reader, "-->");
		if (same) {
			reader->at += 3U;
			return 0;
		}

		/* A byte of the comment. */
		reader->at++;
	}

	/* A comment that never ends. */
	return EINVAL;
}

/*
 * Skips the XML declaration from its "<?xml" past its "?>".  Returns 0,
 * or EINVAL when it is another processing instruction or does not end.
 */
static int
mapxml_declaration(
	struct mapxml_reader *reader)
{
	int is_space;
	int same;

	/* "<?xml" followed by a space or the end: else a processing instruction named xml-something. */
	reader->at += 5U;
	if (reader->at >= reader->length)
		return EINVAL;
	is_space = mapxml_space(reader->bytes[reader->at]);
	same = mapxml_starts(reader, "?>");
	if (!is_space && !same)
		return EINVAL;

	/* The declaration's text up to its end. */
	while (reader->at < reader->length) {
		/* The end of the declaration. */
		same = mapxml_starts(reader, "?>");
		if (same) {
			reader->at += 2U;
			return 0;
		}

		/* A byte of the declaration. */
		reader->at++;
	}

	/* A declaration that never ends. */
	return EINVAL;
}

/*
 * Skips a DOCTYPE from its "<!DOCTYPE" past its '>'.  Returns 0, or
 * EINVAL when it has an internal subset ('[') or does not end.
 */
static int
mapxml_doctype(
	struct mapxml_reader *reader)
{
	uint8_t byte;

	/* The declaration's text up to its end. */
	reader->at += 9U;
	while (reader->at < reader->length) {
		byte = reader->bytes[reader->at];
		reader->at++;

		/* The end of the declaration. */
		if (byte == '>')
			return 0;

		/* An internal subset could declare entities: not read. */
		if (byte == '[')
			return EINVAL;
	}

	/* A declaration that never ends. */
	return EINVAL;
}

/*
 * Reads a start tag from its '<' past its '>' or "/>": the name, then
 * each attribute (its value checked).  Returns 0, EINVAL, or E2BIG for
 * too many attributes or a value too long.
 */
static int
mapxml_start_tag(
	struct mapxml_reader *reader,
	struct mapxml_element *element)
{
	uint8_t byte;
	int is_space;
	int error;

	/* Nothing of the element known yet. */
	memset(element, 0, sizeof(*element));

	/* The element's name after its '<'. */
	reader->at++;
	error = mapxml_name(reader, &element->name);
	if (error != 0)
		return error;

	/* Each attribute until the tag's end. */
	for (;;) {
		/* A tag that ends inside it. */
		if (reader->at >= reader->length)
			return EINVAL;

		/* A space must come before an attribute; the tag may end without one. */
		byte = reader->bytes[reader->at];
		is_space = mapxml_space(byte);
		mapxml_skip_space(reader);
		if (reader->at >= reader->length)
			return EINVAL;
		byte = reader->bytes[reader->at];

		/* The end of a tag that has content. */
		if (byte == '>') {
			reader->at++;
			element->empty = 0;
			return 0;
		}

		/* The end of an empty element. */
		if (byte == '/') {
			reader->at++;
			if (reader->at >= reader->length)
				return EINVAL;
			if (reader->bytes[reader->at] != '>')
				return EINVAL;
			reader->at++;
			element->empty = 1;
			return 0;
		}

		/* An attribute stuck to what came before it. */
		if (!is_space)
			return EINVAL;

		/* The attribute. */
		error = mapxml_attribute(reader, element);
		if (error != 0)
			return error;
	}
}

/*
 * Reads one attribute of a start tag, NAME = "VALUE" or 'VALUE', and
 * checks its value can be undone.  Returns 0, EINVAL for a malformed or
 * repeated attribute, or E2BIG past the attributes' or the value's limit.
 */
static int
mapxml_attribute(
	struct mapxml_reader *reader,
	struct mapxml_element *element)
{
	struct mapxml_attribute *attribute;
	struct mapxml_span name;
	uint8_t quote;
	uint8_t byte;
	size_t index;
	int same;
	int error;

	/* Room for another attribute. */
	if (element->attribute_count >= BTD_MAPXML_ATTRIBUTES_MAX)
		return E2BIG;

	/* The attribute's name. */
	error = mapxml_name(reader, &name);
	if (error != 0)
		return error;

	/* A name given once in a tag. */
	for (index = 0U; index < element->attribute_count; index++) {
		if (element->attributes[index].name.length != name.length)
			continue;

		/* The same bytes: the attribute repeated. */
		same = memcmp(reader->bytes + element->attributes[index].name.start, reader->bytes + name.start, name.length);
		if (same == 0)
			return EINVAL;
	}

	/* The '=', spaces around it taken. */
	mapxml_skip_space(reader);
	if (reader->at >= reader->length)
		return EINVAL;
	if (reader->bytes[reader->at] != '=')
		return EINVAL;
	reader->at++;
	mapxml_skip_space(reader);

	/* The opening quote, either kind. */
	if (reader->at >= reader->length)
		return EINVAL;
	quote = reader->bytes[reader->at];
	if (quote != '"' && quote != '\'')
		return EINVAL;
	reader->at++;

	/* The value up to the same quote. */
	attribute = &element->attributes[element->attribute_count];
	attribute->name = name;
	attribute->value.start = reader->at;
	for (;;) {
		/* A value that never ends. */
		if (reader->at >= reader->length)
			return EINVAL;

		/* The closing quote. */
		byte = reader->bytes[reader->at];
		if (byte == quote)
			break;

		/* A '<' or a NUL cannot be inside a value. */
		if (byte == '<' || byte == '\0')
			return EINVAL;
		reader->at++;
	}

	/* The value's length, and the reading past its closing quote. */
	attribute->value.length = reader->at - attribute->value.start;
	reader->at++;

	/* A value whose references can be undone, within its length. */
	error = mapxml_value(reader, &attribute->value);
	if (error != 0)
		return error;

	/* Succeeded: the attribute kept. */
	element->attribute_count++;
	return 0;
}

/*
 * Reads an end tag from its "</" past its '>' and checks it closes the
 * element named name.  Returns 0, or EINVAL.
 */
static int
mapxml_end_tag(
	struct mapxml_reader *reader,
	const struct mapxml_span *name)
{
	struct mapxml_span closed;
	int same;
	int error;

	/* The name after "</". */
	reader->at += 2U;
	error = mapxml_name(reader, &closed);
	if (error != 0)
		return error;

	/* The name of the element it closes. */
	if (closed.length != name->length)
		return EINVAL;
	same = memcmp(reader->bytes + closed.start, reader->bytes + name->start, name->length);
	if (same != 0)
		return EINVAL;

	/* Spaces, then the '>'. */
	mapxml_skip_space(reader);
	if (reader->at >= reader->length)
		return EINVAL;
	if (reader->bytes[reader->at] != '>')
		return EINVAL;
	reader->at++;

	/* Succeeded: the element closed. */
	return 0;
}

/*
 * Reads what a child element holds up to its end tag: only spaces and
 * comments.  Returns 0 after the end tag, or EINVAL for anything else.
 */
static int
mapxml_child_content(
	struct mapxml_reader *reader,
	const struct mapxml_element *element)
{
	int same;
	int error;

	/* Each comment inside the child until its end tag. */
	for (;;) {
		/* The spaces inside the child. */
		mapxml_skip_space(reader);

		/* The child's end. */
		same = mapxml_starts(reader, "</");
		if (same)
			break;

		/* A comment inside the child. */
		same = mapxml_starts(reader, "<!--");
		if (!same)
			return EINVAL;
		error = mapxml_comment(reader);
		if (error != 0)
			return error;
	}

	/* The end tag of the child. */
	error = mapxml_end_tag(reader, &element->name);
	if (error != 0)
		return error;

	/* Succeeded: an empty child. */
	return 0;
}

/*
 * Reads a name: a letter, '_' or ':', then letters, digits, '_', '.',
 * ':' and '-'.  Returns 0 with its span, or EINVAL for no name or one
 * longer than the reader takes.
 */
static int
mapxml_name(
	struct mapxml_reader *reader,
	struct mapxml_span *name)
{
	int first;
	int more;

	/* The first byte of a name. */
	if (reader->at >= reader->length)
		return EINVAL;
	first = mapxml_name_first(reader->bytes[reader->at]);
	if (!first)
		return EINVAL;

	/* The rest of the name. */
	name->start = reader->at;
	reader->at++;
	while (reader->at < reader->length) {
		more = mapxml_name_byte(reader->bytes[reader->at]);
		if (!more)
			break;
		reader->at++;
	}

	/* The name's length. */
	name->length = reader->at - name->start;

	/* A name of a length the reader takes. */
	if (name->length > MAPXML_NAME_MAX)
		return EINVAL;

	/* Succeeded: the name. */
	return 0;
}

/*
 * Undoes the references of an attribute's value into the reader's value
 * (the predefined entities and character references made UTF-8; a tab,
 * carriage return or line feed made a space, as XML normalizes values).
 * Returns 0, EINVAL for a malformed reference, or E2BIG for a value
 * longer than BTD_MAPXML_VALUE_MAX once undone.
 */
static int
mapxml_value(
	struct mapxml_reader *reader,
	const struct mapxml_span *value)
{
	unsigned long code;
	size_t end;
	size_t at;
	size_t next;
	uint8_t byte;
	int error;

	/* An empty value. */
	reader->value_length = 0U;
	reader->value[0] = '\0';

	/* Each byte or reference of the value. */
	end = value->start + value->length;
	at = value->start;
	while (at < end) {
		byte = reader->bytes[at];

		/* A reference: the character it stands for. */
		if (byte == '&') {
			error = mapxml_reference(reader, at, end, &code, &next);
			if (error != 0)
				return error;
			error = mapxml_put_code(reader, code);
			if (error != 0)
				return error;
			at = next;
			continue;
		}

		/* A tab, carriage return or line feed is a space in a value. */
		if (byte == '\t' ||
		    byte == '\r' ||
		    byte == '\n')
			byte = ' ';

		/* A byte as it is. */
		error = mapxml_put_byte(reader, byte);
		if (error != 0)
			return error;
		at++;
	}

	/* Succeeded: the value undone. */
	reader->value[reader->value_length] = '\0';
	return 0;
}

/*
 * Reads the reference at the '&' at at, before end: &lt; &gt; &amp;
 * &quot; &apos; or &#N; or &#xH;.  Returns 0 with the code point and
 * where the value goes on, or EINVAL for an unknown or malformed
 * reference or a code point that is not a character (0, a surrogate, past
 * U+10FFFF).
 */
static int
mapxml_reference(
	struct mapxml_reader *reader,
	size_t at,
	size_t end,
	unsigned long *code,
	size_t *next)
{
	size_t semicolon;
	size_t name_length;
	size_t length;
	size_t index;
	unsigned long digit;
	unsigned long value;
	unsigned long base;
	int hex;
	int same;

	/* The ';' that ends the reference. */
	semicolon = at + 1U;
	while (semicolon < end && reader->bytes[semicolon] != ';')
		semicolon++;
	if (semicolon >= end)
		return EINVAL;
	length = semicolon - (at + 1U);
	*next = semicolon + 1U;

	/* A named entity. */
	if (length > 0U && reader->bytes[at + 1U] != '#') {
		for (index = 0U; mapxml_entities[index].name != NULL; index++) {
			name_length = strlen(mapxml_entities[index].name);
			if (name_length != length)
				continue;

			/* The entity's name. */
			same = memcmp(reader->bytes + at + 1U, mapxml_entities[index].name, length);
			if (same == 0) {
				*code = (unsigned long)mapxml_entities[index].value;
				return 0;
			}
		}

		/* An entity not predefined. */
		return EINVAL;
	}

	/* A character reference: decimal, or hex after an x. */
	index = at + 2U;
	base = 10UL;
	if (index < semicolon && reader->bytes[index] == 'x') {
		base = 16UL;
		index++;
	}

	/* At least one digit. */
	if (index >= semicolon)
		return EINVAL;

	/* Each digit, the value kept within the code points. */
	value = 0UL;
	for (; index < semicolon; index++) {
		hex = mapxml_hex_digit(reader->bytes[index]);
		if (hex < 0)
			return EINVAL;

		/* A digit of the reference's base. */
		digit = (unsigned long)hex;
		if (digit >= base)
			return EINVAL;

		/* The value so far, refused once past the last code point. */
		value = value * base + digit;
		if (value > MAPXML_CODE_MAX)
			return EINVAL;
	}

	/* A code point that is a character. */
	if (value == 0UL)
		return EINVAL;
	if (value >= 0xd800UL && value <= 0xdfffUL)
		return EINVAL;

	/* Succeeded: the code point. */
	*code = value;
	return 0;
}

/* Adds a code point to the reader's value as UTF-8.  Returns 0, or E2BIG past the value's limit. */
static int
mapxml_put_code(
	struct mapxml_reader *reader,
	unsigned long code)
{
	uint8_t bytes[4];
	size_t count;
	size_t index;
	int error;

	/* The code point's bytes in UTF-8. */
	if (code < 0x80UL) {
		bytes[0] = (uint8_t)code;
		count = 1U;
	} else if (code < 0x800UL) {
		bytes[0] = (uint8_t)(0xc0UL | (code >> 6));
		bytes[1] = (uint8_t)(0x80UL | (code & 0x3fUL));
		count = 2U;
	} else if (code < 0x10000UL) {
		bytes[0] = (uint8_t)(0xe0UL | (code >> 12));
		bytes[1] = (uint8_t)(0x80UL | ((code >> 6) & 0x3fUL));
		bytes[2] = (uint8_t)(0x80UL | (code & 0x3fUL));
		count = 3U;
	} else {
		bytes[0] = (uint8_t)(0xf0UL | (code >> 18));
		bytes[1] = (uint8_t)(0x80UL | ((code >> 12) & 0x3fUL));
		bytes[2] = (uint8_t)(0x80UL | ((code >> 6) & 0x3fUL));
		bytes[3] = (uint8_t)(0x80UL | (code & 0x3fUL));
		count = 4U;
	}

	/* Each byte into the value. */
	for (index = 0U; index < count; index++) {
		error = mapxml_put_byte(reader, bytes[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the character added. */
	return 0;
}

/* Adds a byte to the reader's value.  Returns 0, or E2BIG past the value's limit. */
static int
mapxml_put_byte(
	struct mapxml_reader *reader,
	uint8_t byte)
{
	/* A value longer than the daemon takes. */
	if (reader->value_length >= BTD_MAPXML_VALUE_MAX)
		return E2BIG;

	/* Succeeded: the byte added. */
	reader->value[reader->value_length] = (char)byte;
	reader->value_length++;
	return 0;
}

/* Steps over the spaces at the reading. */
static void
mapxml_skip_space(
	struct mapxml_reader *reader)
{
	int is_space;

	/* Each space. */
	while (reader->at < reader->length) {
		is_space = mapxml_space(reader->bytes[reader->at]);
		if (!is_space)
			break;
		reader->at++;
	}
}

/* Tells whether the input at the reading starts with text. */
static int
mapxml_starts(
	const struct mapxml_reader *reader,
	const char *text)
{
	size_t length;
	int same;

	/* Enough input left. */
	length = strlen(text);
	if (reader->length - reader->at < length)
		return 0;

	/* The same bytes. */
	same = memcmp(reader->bytes + reader->at, text, length);
	if (same != 0)
		return 0;

	/* Succeeded: it starts with the text. */
	return 1;
}

/* Tells whether a byte is a space of XML (space, tab, carriage return, line feed). */
static int
mapxml_space(
	uint8_t byte)
{
	/* The four spaces. */
	if (byte == ' ' || byte == '\t')
		return 1;
	if (byte == '\r' || byte == '\n')
		return 1;

	/* Anything else. */
	return 0;
}

/* Tells whether a byte may start a name. */
static int
mapxml_name_first(
	uint8_t byte)
{
	/* A letter. */
	if (byte >= 'A' && byte <= 'Z')
		return 1;
	if (byte >= 'a' && byte <= 'z')
		return 1;

	/* An underscore or a colon. */
	if (byte == '_' || byte == ':')
		return 1;

	/* Anything else. */
	return 0;
}

/* Tells whether a byte may be inside a name after its first. */
static int
mapxml_name_byte(
	uint8_t byte)
{
	int first;

	/* What may start a name. */
	first = mapxml_name_first(byte);
	if (first)
		return 1;

	/* A digit. */
	if (byte >= '0' && byte <= '9')
		return 1;

	/* A dot or a hyphen. */
	if (byte == '.' || byte == '-')
		return 1;

	/* Anything else. */
	return 0;
}

/* Tells whether a span of the input is exactly text. */
static int
mapxml_span_is(
	const struct mapxml_reader *reader,
	const struct mapxml_span *span,
	const char *text)
{
	size_t length;
	int same;

	/* The same length. */
	length = strlen(text);
	if (span->length != length)
		return 0;

	/* The same bytes. */
	same = memcmp(reader->bytes + span->start, text, length);
	if (same != 0)
		return 0;

	/* Succeeded: the span is the text. */
	return 1;
}

/* Tells whether a name has a colon (a namespace's prefix). */
static int
mapxml_span_has_colon(
	const struct mapxml_reader *reader,
	const struct mapxml_span *span)
{
	const void *colon;

	/* A colon among its bytes. */
	colon = memchr(reader->bytes + span->start, ':', span->length);
	if (colon != NULL)
		return 1;

	/* No colon. */
	return 0;
}

/* Tells whether two words are the same, ignoring the case of ASCII letters. */
static int
mapxml_same_word(
	const char *left,
	const char *right)
{
	char a;
	char b;

	/* Each letter of both. */
	for (;;) {
		a = *left;
		b = *right;

		/* Both letters in lower case. */
		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z')
			b = (char)(b - 'A' + 'a');

		/* A difference. */
		if (a != b)
			return 0;

		/* Both ended together. */
		if (a == '\0')
			return 1;
		left++;
		right++;
	}
}

/* Gives the value of a word in a table (case not minded), or otherwise when the table lacks it. */
static int
mapxml_word(
	const struct mapxml_word *words,
	const char *name,
	int otherwise)
{
	size_t index;
	int same;

	/* Each word of the table. */
	for (index = 0U; words[index].name != NULL; index++) {
		same = mapxml_same_word(words[index].name, name);
		if (same)
			return words[index].value;
	}

	/* A word the table does not know. */
	return otherwise;
}

/* Copies the reader's value into a text of size bytes, cut so a character is not split. */
static void
mapxml_copy(
	char *text,
	size_t size,
	const struct mapxml_reader *reader)
{
	size_t kept;

	/* As much of the value as fits with its NUL. */
	kept = btd_mapxml_utf8_cut(reader->value, reader->value_length, size - 1U);
	memcpy(text, reader->value, kept);
	text[kept] = '\0';
}

/*
 * Reads the reader's value as a message handle: 1 to 16 hex digits.
 * Returns 0 with the handle, or EINVAL.
 */
static int
mapxml_handle(
	const struct mapxml_reader *reader,
	uint64_t *handle)
{
	uint64_t value;
	size_t index;
	int digit;

	/* 1 to 16 digits. */
	if (reader->value_length == 0U || reader->value_length > 16U)
		return EINVAL;

	/* Each hex digit. */
	value = 0U;
	for (index = 0U; index < reader->value_length; index++) {
		digit = mapxml_hex_digit((uint8_t)reader->value[index]);
		if (digit < 0)
			return EINVAL;

		/* The digit added to the handle. */
		value = value * 16U + (uint64_t)digit;
	}

	/* Succeeded: the handle. */
	*handle = value;
	return 0;
}

/* Gives the value of a hex digit, or -1 for a byte that is not one. */
static int
mapxml_hex_digit(
	uint8_t byte)
{
	/* A decimal digit. */
	if (byte >= '0' && byte <= '9')
		return byte - '0';

	/* A letter digit, in either case. */
	if (byte >= 'a' && byte <= 'f')
		return byte - 'a' + 10;
	if (byte >= 'A' && byte <= 'F')
		return byte - 'A' + 10;

	/* Not a hex digit. */
	return -1;
}

/*
 * Takes a child of a listing: a msg element fills the next entry (other
 * children are ignored).  Returns 0, or E2BIG past the most messages a
 * listing holds.
 */
static int
mapxml_listing_child(
	void *context,
	struct mapxml_reader *reader,
	const struct mapxml_element *element)
{
	struct mapxml_listing *listing;
	struct btd_map_entry *entry;
	const struct mapxml_attribute *attribute;
	uint64_t handle;
	size_t index;
	int found;
	int same;
	int error;

	/* Only the messages of the listing. */
	listing = context;
	same = mapxml_span_is(reader, &element->name, "msg");
	if (!same)
		return 0;

	/* One more message, within the most a listing holds. */
	listing->messages++;
	if (listing->messages > BTD_MAPXML_ENTRIES_MAX)
		return E2BIG;

	/* The message's handle. */
	found = 0;
	handle = 0U;
	for (index = 0U; index < element->attribute_count; index++) {
		attribute = &element->attributes[index];
		same = mapxml_span_is(reader, &attribute->name, "handle");
		if (!same)
			continue;

		/* The handle's value undone and read. */
		error = mapxml_value(reader, &attribute->value);
		if (error != 0)
			return error;
		error = mapxml_handle(reader, &handle);
		if (error == 0)
			found = 1;
	}

	/* A message without a handle is left out. */
	if (!found) {
		listing->counts->skipped++;
		return 0;
	}

	/* A message past the room the caller gave is dropped. */
	if (listing->counts->entries >= listing->capacity) {
		listing->counts->dropped++;
		return 0;
	}

	/* The entry, with what is not said. */
	entry = &listing->entries[listing->counts->entries];
	memset(entry, 0, sizeof(*entry));
	entry->handle = handle;
	entry->type = BTD_MAP_TYPE_OTHER;
	entry->read = BTD_MAP_READ_UNKNOWN;
	entry->reception = BTD_MAP_RECEPTION_UNKNOWN;

	/* Each attribute the entry keeps. */
	for (index = 0U; index < element->attribute_count; index++) {
		attribute = &element->attributes[index];

		/* The attribute's value undone. */
		error = mapxml_value(reader, &attribute->value);
		if (error != 0)
			return error;

		/* Kept when the entry knows it. */
		mapxml_entry_attribute(entry, reader, &attribute->name);
	}

	/* Succeeded: one more entry. */
	listing->counts->entries++;
	return 0;
}

/*
 * Keeps the value an attribute of a listing's msg gives (the reader's
 * value), when the entry knows its name.  A name with a colon, or one
 * not known, is ignored.
 */
static void
mapxml_entry_attribute(
	struct btd_map_entry *entry,
	const struct mapxml_reader *reader,
	const struct mapxml_span *name)
{
	int same;

	/* The datetime as written, or nothing when too long to be one. */
	same = mapxml_span_is(reader, name, "datetime");
	if (same) {
		if (reader->value_length < sizeof(entry->datetime))
			memcpy(entry->datetime, reader->value, reader->value_length + 1U);
		return;
	}

	/* The sender's name. */
	same = mapxml_span_is(reader, name, "sender_name");
	if (same) {
		mapxml_copy(entry->sender_name, sizeof(entry->sender_name), reader);
		return;
	}

	/* The sender's address (a number for SMS). */
	same = mapxml_span_is(reader, name, "sender_addressing");
	if (same) {
		mapxml_copy(entry->sender_addressing, sizeof(entry->sender_addressing), reader);
		return;
	}

	/* The recipient's name. */
	same = mapxml_span_is(reader, name, "recipient_name");
	if (same) {
		mapxml_copy(entry->recipient_name, sizeof(entry->recipient_name), reader);
		return;
	}

	/* The recipient's address. */
	same = mapxml_span_is(reader, name, "recipient_addressing");
	if (same) {
		mapxml_copy(entry->recipient_addressing, sizeof(entry->recipient_addressing), reader);
		return;
	}

	/* The message's type. */
	same = mapxml_span_is(reader, name, "type");
	if (same) {
		entry->type = mapxml_word(mapxml_message_types, reader->value, BTD_MAP_TYPE_OTHER);
		return;
	}

	/* Whether it was read: yes or no, else not said. */
	same = mapxml_span_is(reader, name, "read");
	if (same) {
		same = mapxml_same_word(reader->value, "yes");
		if (same) {
			entry->read = BTD_MAP_READ_YES;
			return;
		}

		/* No, or not said. */
		same = mapxml_same_word(reader->value, "no");
		if (same)
			entry->read = BTD_MAP_READ_NO;
		return;
	}

	/* How much of it the phone holds. */
	same = mapxml_span_is(reader, name, "reception_status");
	if (same)
		entry->reception = mapxml_word(mapxml_receptions, reader->value, BTD_MAP_RECEPTION_UNKNOWN);
}

/*
 * Takes a child of an event report: an event element fills the event
 * (other children are ignored).  Returns 0, or EINVAL for a second
 * event, an event without a type or with a malformed handle.
 */
static int
mapxml_report_child(
	void *context,
	struct mapxml_reader *reader,
	const struct mapxml_element *element)
{
	struct mapxml_report *report;
	struct btd_map_event *event;
	const struct mapxml_attribute *attribute;
	size_t index;
	int typed;
	int same;
	int error;

	/* Only the event of the report. */
	report = context;
	same = mapxml_span_is(reader, &element->name, "event");
	if (!same)
		return 0;

	/* A report of version 1.0 holds one event. */
	report->events++;
	if (report->events > 1U)
		return EINVAL;

	/* The event, with what is not said. */
	event = report->event;
	memset(event, 0, sizeof(*event));
	event->type = BTD_MAP_EVENT_OTHER;
	event->msg_type = BTD_MAP_TYPE_OTHER;

	/* Each attribute the event keeps. */
	typed = 0;
	for (index = 0U; index < element->attribute_count; index++) {
		attribute = &element->attributes[index];

		/* The attribute's value undone. */
		error = mapxml_value(reader, &attribute->value);
		if (error != 0)
			return error;

		/* The kind of the event. */
		same = mapxml_span_is(reader, &attribute->name, "type");
		if (same) {
			event->type = mapxml_word(mapxml_event_types, reader->value, BTD_MAP_EVENT_OTHER);
			typed = 1;
			continue;
		}

		/* The message's handle, which must be one. */
		same = mapxml_span_is(reader, &attribute->name, "handle");
		if (same) {
			error = mapxml_handle(reader, &event->handle);
			if (error != 0)
				return EINVAL;
			event->has_handle = 1;
			continue;
		}

		/* The folder the message is in. */
		same = mapxml_span_is(reader, &attribute->name, "folder");
		if (same) {
			mapxml_copy(event->folder, sizeof(event->folder), reader);
			continue;
		}

		/* The folder a shifted message came from. */
		same = mapxml_span_is(reader, &attribute->name, "old_folder");
		if (same) {
			mapxml_copy(event->old_folder, sizeof(event->old_folder), reader);
			continue;
		}

		/* The message's type. */
		same = mapxml_span_is(reader, &attribute->name, "msg_type");
		if (same)
			event->msg_type = mapxml_word(mapxml_message_types, reader->value, BTD_MAP_TYPE_OTHER);
	}

	/* An event says its kind. */
	if (!typed)
		return EINVAL;

	/* Succeeded: the event. */
	return 0;
}

/* Reads count decimal digits.  Returns 0 with their number, or EINVAL for a byte not a digit. */
static int
mapxml_digits(
	const char *text,
	size_t count,
	int *number)
{
	size_t index;
	int value;

	/* Each digit. */
	value = 0;
	for (index = 0U; index < count; index++) {
		if (text[index] < '0' || text[index] > '9')
			return EINVAL;
		value = value * 10 + (text[index] - '0');
	}

	/* Succeeded: the number. */
	*number = value;
	return 0;
}

/* Gives the days of a month of the Gregorian calendar. */
static int
mapxml_days_in_month(
	int year,
	int month)
{
	static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	/* February of a leap year: every fourth year, but not a century unless every fourth one. */
	if (month == 2 && year % 4 == 0) {
		if (year % 100 != 0 || year % 400 == 0)
			return 29;
	}

	/* The month's days in a common year. */
	return days[month - 1];
}

/*
 * Gives the date of a day counted from 1970-01-01 (the day must be of a
 * year from 0001, so the counting from era 0 never goes negative).
 */
static void
mapxml_civil_from_days(
	int64_t days,
	int *year,
	int *month,
	int *day)
{
	int64_t shifted;
	int64_t era;
	int64_t day_of_era;
	int64_t year_of_era;
	int64_t day_of_year;
	int64_t month_from_march;

	/* The day counted from 0000-03-01, the era it is in and the day within the era. */
	shifted = days + 719468;
	era = shifted / 146097;
	day_of_era = shifted - era * 146097;

	/* The year within the era (the year counted from March). */
	year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;

	/* The day within that year, and its month counted from March. */
	day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	month_from_march = (5 * day_of_year + 2) / 153;

	/* The day of the month. */
	*day = (int)(day_of_year - (153 * month_from_march + 2) / 5 + 1);

	/* The month: March is 0 counted from March. */
	if (month_from_march < 10) {
		*month = (int)(month_from_march + 3);
	} else {
		*month = (int)(month_from_march - 9);
	}

	/* The year: January and February belong to the next year. */
	*year = (int)(year_of_era + era * 400);
	if (*month <= 2)
		*year = *year + 1;
}
