/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The XML of the Message Access Profile and its times (ws197-p003,
 * plan/ws197/phase003/phase.md section 6): the message listing a phone
 * gives for a folder, the event report it pushes when a message arrives
 * or moves, and the datetime strings both carry.
 *
 * The reader takes only the small part of XML these documents use: a
 * root element holding empty child elements whose attributes carry
 * everything.  It skips a byte order mark, the XML declaration, comments
 * and a DOCTYPE without an internal subset, and refuses whatever else
 * (CDATA, processing instructions, text or elements inside a child).
 * Attributes it does not know are ignored, as the profile asks.
 *
 * Without system calls; the host tests build it.
 */

#ifndef BLUETOOTHD_MAPXML_H
#define BLUETOOTHD_MAPXML_H

#include <stddef.h>
#include <stdint.h>

/* The most one document is, in bytes. */
#define BTD_MAPXML_INPUT_MAX		65536U

/* The most attributes one element has, and the longest value once its references are undone. */
#define BTD_MAPXML_ATTRIBUTES_MAX	32U
#define BTD_MAPXML_VALUE_MAX		1024U

/* The most messages one listing holds. */
#define BTD_MAPXML_ENTRIES_MAX		1024U

/* The room of an entry's datetime (YYYYMMDDTHHMMSS, an offset, a NUL) and of its other texts. */
#define BTD_MAPXML_DATETIME_SIZE	24U
#define BTD_MAPXML_TEXT_SIZE		256U

/* The type of a message, as a listing or an event names it. */
#define BTD_MAP_TYPE_OTHER		0
#define BTD_MAP_TYPE_SMS_GSM		1
#define BTD_MAP_TYPE_SMS_CDMA		2
#define BTD_MAP_TYPE_MMS		3
#define BTD_MAP_TYPE_EMAIL		4
#define BTD_MAP_TYPE_IM			5

/* Whether a message was read: as the listing says, or not said. */
#define BTD_MAP_READ_UNKNOWN		(-1)
#define BTD_MAP_READ_NO			0
#define BTD_MAP_READ_YES		1

/* How much of a message the phone holds: not said, all of it, a part, or only its notification. */
#define BTD_MAP_RECEPTION_UNKNOWN	0
#define BTD_MAP_RECEPTION_COMPLETE	1
#define BTD_MAP_RECEPTION_FRACTIONED	2
#define BTD_MAP_RECEPTION_NOTIFICATION	3

/* The kind of an event report. */
#define BTD_MAP_EVENT_OTHER		0
#define BTD_MAP_EVENT_NEW_MESSAGE	1
#define BTD_MAP_EVENT_DELIVERY_SUCCESS	2
#define BTD_MAP_EVENT_SENDING_SUCCESS	3
#define BTD_MAP_EVENT_DELIVERY_FAILURE	4
#define BTD_MAP_EVENT_SENDING_FAILURE	5
#define BTD_MAP_EVENT_MEMORY_FULL	6
#define BTD_MAP_EVENT_MEMORY_AVAILABLE	7
#define BTD_MAP_EVENT_MESSAGE_DELETED	8
#define BTD_MAP_EVENT_MESSAGE_SHIFT	9

/* What a datetime says of its zone: nothing, an offset from UTC, or UTC itself (a Z). */
#define BTD_MAP_ZONE_NONE		0
#define BTD_MAP_ZONE_OFFSET		1
#define BTD_MAP_ZONE_UTC		2

/* Which offset turned a datetime into seconds: its own, the phone's MSETime, or zedBSD's. */
#define BTD_MAP_FROM_PHONE		1
#define BTD_MAP_FROM_MSE		2
#define BTD_MAP_FROM_LOCAL		3

/*
 * One message of a listing: its handle (valid for the MAP session that
 * listed it), the phone's datetime string as given (empty when it was
 * too long to be one), the names and addresses of its sender and
 * recipient (cut to fit, never inside a character), its type, whether it
 * was read, and how much of it the phone holds.
 */
struct btd_map_entry {
	uint64_t handle;
	char datetime[BTD_MAPXML_DATETIME_SIZE];
	char sender_name[BTD_MAPXML_TEXT_SIZE];
	char sender_addressing[BTD_MAPXML_TEXT_SIZE];
	char recipient_name[BTD_MAPXML_TEXT_SIZE];
	char recipient_addressing[BTD_MAPXML_TEXT_SIZE];
	int type;
	int read;
	int reception;
};

/*
 * What a listing held besides the entries kept: the entries kept, the
 * messages left out for a missing or malformed handle, and the messages
 * past the room the caller gave.
 */
struct btd_mapxml_counts {
	size_t entries;
	size_t skipped;
	size_t dropped;
};

/*
 * One event report: its kind, the message's handle (has_handle says
 * whether it named one), the folder the message is in and, for a shift,
 * the folder it came from, and the message's type.
 */
struct btd_map_event {
	int type;
	int has_handle;
	uint64_t handle;
	char folder[BTD_MAPXML_TEXT_SIZE];
	char old_folder[BTD_MAPXML_TEXT_SIZE];
	int msg_type;
};

/*
 * A datetime as written: its date and time of day, and what it says of
 * its zone (offset holds seconds east of UTC when zone is
 * BTD_MAP_ZONE_OFFSET, 0 otherwise).
 */
struct btd_map_time {
	int year;
	int month;
	int day;
	int hour;
	int minute;
	int second;
	int zone;
	int32_t offset;
};

int btd_mapxml_listing(const uint8_t *input, size_t length, struct btd_map_entry *entries, size_t capacity, struct btd_mapxml_counts *counts);
int btd_mapxml_event(const uint8_t *input, size_t length, struct btd_map_event *event);
size_t btd_mapxml_utf8_cut(const char *text, size_t length, size_t room);
int btd_mapxml_time_parse(const char *text, size_t length, struct btd_map_time *time);
int64_t btd_mapxml_days_from_civil(int year, int month, int day);
int btd_mapxml_time_unix(const char *text, size_t length, int mse_known, int32_t mse_offset, int32_t local_offset, int64_t *seconds, int *from);
int btd_mapxml_time_format(int64_t seconds, int32_t offset, char *text, size_t size);

#endif
