/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calendar's events and memos on the disk (WS155 p002, plan/ws155/
 * phase001/phase.md section 1), under a root (~/Documents/Calendar):
 *
 *   <Work|Personal|Family|Study>/<UID>.ics  one event (iCalendar VEVENT)
 *   Memos/<UID>.ics                          one memo kept on a day (VJOURNAL)
 *   memo.txt                                 the application's memo
 *
 * One file for each thing, so that a folder synchronized with the cloud
 * rarely has two machines write the same file; a change writes the file
 * beside it and renames it over it, a deletion removes it.  The files are
 * read when the store opens.  Times are the local time without a zone
 * (iCalendar's floating times); a time in UTC ("Z") from another program
 * is read into the local time.  Only the window's thread touches the
 * store.
 */

#include "calendar.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The longest path, line and file read, with its NUL. */
#define STORE_PATH_MAX		2048U
#define STORE_ROOT_MAX		512U
#define STORE_LINE_MAX		4096U
#define STORE_FILE_MAX		65536U

/* The folder of the memos kept on days, and the application's memo's file. */
#define STORE_MEMOS		"Memos"
#define STORE_MEMO_FILE		"memo.txt"

/* The longest line of an iCalendar file before it is folded (RFC 5545 section 3.1). */
#define STORE_FOLD		75U

/* The folders of the calendars, in their order, and their colors. */
static const char *const store_lists[CAL_LISTS] = { "Work", "Personal", "Family", "Study" };
static const kl_color store_colors[CAL_LISTS] = {
	KL_RGB(0x3b82f6),
	KL_RGB(0xef4444),
	KL_RGB(0x22c55e),
	KL_RGB(0xf5b82e)
};

/* The root folder (empty while the store is closed). */
static char store_root[STORE_ROOT_MAX];

/* The events and memos (allocated), how many, and the room. */
static struct cal_item *store_items;
static size_t store_count;
static size_t store_capacity;

/* The application's memo (allocated, never NULL while the store is open). */
static char *store_memo;

/* The number in the next UID made, so that two made in one second differ. */
static unsigned long store_serial;

static int store_load_folder(const char *folder, int memo, enum cal_list list);
static int store_load_file(const char *path, int memo, enum cal_list list);
static int store_parse(char *text, struct cal_item *item);
static void store_parse_line(char *line, struct cal_item *item, int *inside, int *start_set);
static int store_parse_time(const char *name, const char *value, struct cal_date *date, int *minute, int *all_day);
static int store_write(const struct cal_item *item);
static size_t store_put(char *text, size_t at, size_t size, const char *line);
static size_t store_put_escaped(char *text, size_t at, size_t size, const char *name, const char *value);
static void store_unescape(char *value);
static int store_append(const struct cal_item *item);
static void store_free_item(struct cal_item *item);
static void store_path(const struct cal_item *item, char *path, size_t size);
static int store_write_file(const char *path, const char *text, size_t length);

/*
 * Opens the store at a root folder (its folders made when they are not
 * there) and reads every event, memo and the application's memo.
 * Returns 0 or an errno value.
 */
int
cal_store_open(
	const char *root)
{
	char path[STORE_PATH_MAX];
	FILE *file;
	size_t length;
	int list;
	int error;

	/* The root and its folders. */
	cal_store_close();
	(void)snprintf(store_root, sizeof(store_root), "%s", root);
	(void)mkdir(store_root, 0700);
	for (list = 0; list < CAL_LISTS; list++) {
		(void)snprintf(path, sizeof(path), "%s/%s", store_root, store_lists[list]);
		(void)mkdir(path, 0700);
		error = store_load_folder(path, 0, (enum cal_list)list);
		if (error != 0)
			return error;
	}

	/* The memos kept on days. */
	(void)snprintf(path, sizeof(path), "%s/%s", store_root, STORE_MEMOS);
	(void)mkdir(path, 0700);
	error = store_load_folder(path, 1, CAL_WORK);
	if (error != 0)
		return error;

	/* The application's memo (empty when there is none). */
	store_memo = calloc(1U, CAL_MEMO_MAX);
	if (store_memo == NULL)
		return ENOMEM;
	(void)snprintf(path, sizeof(path), "%s/%s", store_root, STORE_MEMO_FILE);
	file = fopen(path, "r");
	if (file != NULL) {
		length = fread(store_memo, 1U, CAL_MEMO_MAX - 1U, file);
		store_memo[length] = '\0';
		fclose(file);
	}

	/* Succeeded: everything is read. */
	return 0;
}

/*
 * Frees everything the store holds.
 */
void
cal_store_close(void)
{
	size_t index;

	/* Each item. */
	for (index = 0; index < store_count; index++)
		store_free_item(&store_items[index]);

	/* The array and the memo. */
	free(store_items);
	store_items = NULL;
	store_count = 0;
	store_capacity = 0;
	free(store_memo);
	store_memo = NULL;
	store_root[0] = '\0';
}

/*
 * Reports the events and memos kept, and how many there are.
 */
const struct cal_item *
cal_items(
	size_t *count)
{
	/* The items read and kept. */
	*count = store_count;
	return store_items;
}

/*
 * Keeps an event: a new one (index -1) gets a UID and is added, an old
 * one is written again (moved to its calendar's folder when its calendar
 * changed).  *kept is its index.  Returns 0 or an errno value.
 */
int
cal_store_save_event(
	long index,
	const struct cal_item *item,
	long *kept)
{
	struct cal_item made;
	struct cal_item *old;
	char old_path[STORE_PATH_MAX];
	char *text;
	int status;
	int error;

	/* A new event: a UID, then added and written. */
	if (index < 0) {
		made = *item;
		made.memo = 0;
		made.text = NULL;
		store_serial++;
		(void)snprintf(made.uid, sizeof(made.uid), "%lld-%lu-%ld@keiland", (long long)time(NULL), store_serial, (long)getpid());
		error = store_write(&made);
		if (error != 0)
			return error;
		error = store_append(&made);
		if (error != 0)
			return error;
		*kept = (long)store_count - 1L;
		return 0;
	}

	/* An old one that is there. */
	if ((size_t)index >= store_count)
		return EINVAL;
	old = &store_items[index];
	store_path(old, old_path, sizeof(old_path));

	/* Its new fields (its UID and memo's words stay). */
	text = old->text;
	made = *item;
	(void)snprintf(made.uid, sizeof(made.uid), "%s", old->uid);
	made.memo = old->memo;
	made.text = text;
	error = store_write(&made);
	if (error != 0)
		return error;

	/* A calendar changed: the file in the old folder goes (a file already gone does not matter). */
	if (made.list != old->list && !made.memo) {
		status = unlink(old_path);
		if (status != 0)
			(void)status;
	}

	/* The kept item takes the new fields. */
	*old = made;

	/* Succeeded: the event is kept. */
	*kept = index;
	return 0;
}

/*
 * Removes an event or a memo kept on a day: its file, and its place (the
 * items after it move up one).  Returns 0 or an errno value.
 */
int
cal_store_delete(
	long index)
{
	char path[STORE_PATH_MAX];
	int status;

	/* An item that is there. */
	if (index < 0 || (size_t)index >= store_count)
		return EINVAL;

	/* Its file. */
	store_path(&store_items[index], path, sizeof(path));
	status = unlink(path);
	if (status != 0 && errno != ENOENT)
		return errno;

	/* Its place. */
	store_free_item(&store_items[index]);
	memmove(&store_items[index], &store_items[index + 1], (store_count - (size_t)index - 1U) * sizeof(store_items[0]));
	store_count--;

	/* Succeeded: the item is gone. */
	return 0;
}

/*
 * Keeps a memo on a day: its first line as its title, all of its words as
 * its text.  Returns 0 or an errno value.
 */
int
cal_store_add_memo(
	const struct cal_date *date,
	const char *text)
{
	struct cal_item made;
	size_t length;
	int error;

	/* The memo: the day, a UID, the first line, the words. */
	memset(&made, 0, sizeof(made));
	made.memo = 1;
	made.date = *date;
	made.all_day = 1;
	store_serial++;
	(void)snprintf(made.uid, sizeof(made.uid), "%lld-%lu-%ld-memo@keiland", (long long)time(NULL), store_serial, (long)getpid());
	length = strcspn(text, "\n");
	if (length >= sizeof(made.title))
		length = sizeof(made.title) - 1U;
	memcpy(made.title, text, length);
	made.title[length] = '\0';
	made.text = (char *)text;

	/* Written, then kept. */
	error = store_write(&made);
	if (error != 0)
		return error;
	error = store_append(&made);
	if (error != 0)
		return error;

	/* Succeeded: the memo is on the day. */
	return 0;
}

/*
 * Reports the application's memo ("" when the store is closed).
 */
const char *
cal_store_memo(void)
{
	/* No store. */
	if (store_memo == NULL)
		return "";

	/* Its words. */
	return store_memo;
}

/*
 * Keeps the application's memo (its file written again).
 */
int
cal_store_set_memo(
	const char *text)
{
	char path[STORE_PATH_MAX];
	int error;

	/* No store. */
	if (store_memo == NULL)
		return EINVAL;

	/* Kept, then written. */
	(void)snprintf(store_memo, CAL_MEMO_MAX, "%s", text);
	(void)snprintf(path, sizeof(path), "%s/%s", store_root, STORE_MEMO_FILE);
	error = store_write_file(path, store_memo, strlen(store_memo));
	if (error != 0)
		return error;

	/* Succeeded: the memo is kept. */
	return 0;
}

/*
 * Reports a calendar's name (the name of its folder too).
 */
const char *
cal_list_name(
	enum cal_list list)
{
	/* A calendar not known. */
	if ((unsigned)list >= CAL_LISTS)
		return "";

	/* Its name. */
	return store_lists[list];
}

/*
 * Reports a calendar's color.
 */
kl_color
cal_list_color(
	enum cal_list list)
{
	/* A calendar not known. */
	if ((unsigned)list >= CAL_LISTS)
		return KL_RGB(0x828282);

	/* Its color. */
	return store_colors[list];
}

/* Reads every .ics file of a folder. */
static int
store_load_folder(
	const char *folder,
	int memo,
	enum cal_list list)
{
	char path[STORE_PATH_MAX + 256U + 2U];
	struct dirent *entry;
	size_t length;
	DIR *directory;
	int differs;
	int error;

	/* The folder (none: nothing in it). */
	directory = opendir(folder);
	if (directory == NULL)
		return 0;

	/* Each .ics file. */
	error = 0;
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Another file. */
		length = strlen(entry->d_name);
		if (length <= 4U)
			continue;
		differs = strcmp(entry->d_name + length - 4U, ".ics");
		if (differs != 0)
			continue;

		/* Its item. */
		(void)snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		error = store_load_file(path, memo, list);
		if (error != 0)
			break;
	}

	/* The folder is read; a failure to keep an item ends it. */
	closedir(directory);
	if (error != 0)
		return error;

	/* Succeeded: the folder is read. */
	return 0;
}

/* Reads one .ics file into an item (a file that is not one event or memo is skipped). */
static int
store_load_file(
	const char *path,
	int memo,
	enum cal_list list)
{
	struct cal_item item;
	size_t length;
	FILE *file;
	char *text;
	int parsed;
	int error;

	/* The file's bytes. */
	file = fopen(path, "r");
	if (file == NULL)
		return 0;
	text = malloc(STORE_FILE_MAX);
	if (text == NULL) {
		fclose(file);
		return ENOMEM;
	}

	/* Up to the most a file keeps. */
	length = fread(text, 1U, STORE_FILE_MAX - 1U, file);
	fclose(file);
	text[length] = '\0';

	/* Its event or memo, of the folder's calendar. */
	memset(&item, 0, sizeof(item));
	item.memo = memo;
	item.list = list;
	parsed = store_parse(text, &item);
	if (!parsed) {
		free(text);
		free(item.text);
		return 0;
	}

	/* Kept (its words copied). */
	error = store_append(&item);
	free(item.text);
	free(text);
	if (error != 0)
		return error;

	/* Succeeded: the item is read. */
	return 0;
}

/* Reads an iCalendar text (its lines unfolded) into an item; 1 when it held one event or memo with a day. */
static int
store_parse(
	char *text,
	struct cal_item *item)
{
	char *read_at;
	char *write_at;
	char *line;
	char *next;
	int inside;
	int start_set;

	/* Unfolded: a line end followed by a space or a tab joins the lines. */
	read_at = text;
	write_at = text;
	while (*read_at != '\0') {
		/* A fold after CR LF. */
		if (read_at[0] == '\r' && read_at[1] == '\n' && (read_at[2] == ' ' || read_at[2] == '\t')) {
			read_at += 3;
			continue;
		}

		/* A fold after a bare line feed. */
		if (read_at[0] == '\n' && (read_at[1] == ' ' || read_at[1] == '\t')) {
			read_at += 2;
			continue;
		}

		/* Any other byte. */
		*write_at = *read_at;
		write_at++;
		read_at++;
	}

	/* The unfolded text's end. */
	*write_at = '\0';

	/* Each line of the first VEVENT or VJOURNAL. */
	inside = 0;
	start_set = 0;
	for (line = text; line != NULL && *line != '\0'; line = next) {
		next = strchr(line, '\n');
		if (next != NULL) {
			*next = '\0';
			next++;
		}

		/* Without its CR. */
		line[strcspn(line, "\r")] = '\0';
		store_parse_line(line, item, &inside, &start_set);
		if (inside < 0)
			break;
	}

	/* Only an item with a day. */
	if (!start_set)
		return 0;

	/* An item. */
	return 1;
}

/* Reads one line of an item: inside is 1 within its VEVENT or VJOURNAL, -1 after its end. */
static void
store_parse_line(
	char *line,
	struct cal_item *item,
	int *inside,
	int *start_set)
{
	struct cal_date end;
	char *colon;
	char *name_end;
	char *copied;
	size_t length;
	int all_day;
	int minute;
	int same;
	int error;

	/* The start and the end of the component. */
	same = strcmp(line, "BEGIN:VEVENT");
	if (same != 0)
		same = strcmp(line, "BEGIN:VJOURNAL");
	if (same == 0) {
		*inside = 1;
		return;
	}

	/* Its end. */
	same = strcmp(line, "END:VEVENT");
	if (same != 0)
		same = strcmp(line, "END:VJOURNAL");
	if (same == 0 && *inside == 1) {
		*inside = -1;
		return;
	}

	/* Lines outside it. */
	if (*inside != 1)
		return;

	/* The name (before its parameters) and the value. */
	colon = strchr(line, ':');
	if (colon == NULL)
		return;
	*colon = '\0';
	colon++;
	name_end = strchr(line, ';');

	/* The name alone. */
	if (name_end != NULL)
		*name_end = '\0';

	/* The UID. */
	same = strcmp(line, "UID");
	if (same == 0) {
		(void)snprintf(item->uid, sizeof(item->uid), "%s", colon);
		return;
	}

	/* The title. */
	same = strcmp(line, "SUMMARY");
	if (same == 0) {
		store_unescape(colon);
		(void)snprintf(item->title, sizeof(item->title), "%s", colon);
		return;
	}

	/* A memo's words. */
	same = strcmp(line, "DESCRIPTION");
	if (same == 0) {
		store_unescape(colon);
		length = strlen(colon);
		copied = malloc(length + 1U);
		if (copied == NULL)
			return;
		memcpy(copied, colon, length + 1U);
		free(item->text);
		item->text = copied;
		return;
	}

	/* The start (its day). */
	same = strcmp(line, "DTSTART");
	if (same == 0) {
		error = store_parse_time(line, colon, &item->date, &minute, &all_day);
		if (error != 0)
			return;
		item->all_day = all_day;
		item->start = minute;
		*start_set = 1;
		return;
	}

	/* The end (its time only, on the start's day). */
	same = strcmp(line, "DTEND");
	if (same == 0) {
		error = store_parse_time(line, colon, &end, &minute, &all_day);
		if (error == 0 && !all_day)
			item->end = minute;
	}
}

/*
 * Reads a date or a date and time ("20261007", "20261007T100000",
 * "20261007T010000Z" in UTC, made local); all_day is 1 for a date.
 */
static int
store_parse_time(
	const char *name,
	const char *value,
	struct cal_date *date,
	int *minute,
	int *all_day)
{
	struct tm parts;
	struct tm local;
	const char *utc;
	time_t moment;
	int year;
	int month;
	int day;
	int hour;
	int minutes;
	int read;

	UNUSED_PARAMETER(name);

	/* The date. */
	read = sscanf(value, "%4d%2d%2d", &year, &month, &day);
	if (read != 3)
		return EINVAL;
	date->year = year;
	date->month = month;
	date->day = day;
	*minute = 0;
	*all_day = 1;

	/* A time after the T. */
	if (value[8] != 'T')
		return 0;
	read = sscanf(value + 9, "%2d%2d", &hour, &minutes);
	if (read != 2)
		return EINVAL;
	*all_day = 0;
	*minute = hour * 60 + minutes;

	/* In UTC: the local day and time of that moment. */
	utc = strchr(value, 'Z');
	if (utc != NULL) {
		memset(&parts, 0, sizeof(parts));
		parts.tm_year = year - 1900;
		parts.tm_mon = month - 1;
		parts.tm_mday = day;
		parts.tm_hour = hour;
		parts.tm_min = minutes;
		moment = timegm(&parts);
		(void)localtime_r(&moment, &local);
		date->year = local.tm_year + 1900;
		date->month = local.tm_mon + 1;
		date->day = local.tm_mday;
		*minute = local.tm_hour * 60 + local.tm_min;
	}

	/* Succeeded: the day and time are read. */
	return 0;
}

/* Writes an item's file (an event as a VEVENT, a memo as a VJOURNAL). */
static int
store_write(
	const struct cal_item *item)
{
	struct cal_date next;
	const char *component;
	char path[STORE_PATH_MAX];
	char line[256];
	char *text;
	size_t length;
	time_t now;
	struct tm stamp;
	int error;

	/* The text. */
	text = malloc(STORE_FILE_MAX);
	if (text == NULL)
		return ENOMEM;
	length = 0;
	length = store_put(text, length, STORE_FILE_MAX, "BEGIN:VCALENDAR");
	length = store_put(text, length, STORE_FILE_MAX, "VERSION:2.0");
	length = store_put(text, length, STORE_FILE_MAX, "PRODID:-//Keiland//Calendar//EN");
	component = "VEVENT";
	if (item->memo)
		component = "VJOURNAL";
	(void)snprintf(line, sizeof(line), "BEGIN:%s", component);
	length = store_put(text, length, STORE_FILE_MAX, line);
	(void)snprintf(line, sizeof(line), "UID:%s", item->uid);
	length = store_put(text, length, STORE_FILE_MAX, line);

	/* When it was written, in UTC. */
	now = time(NULL);
	(void)gmtime_r(&now, &stamp);
	(void)snprintf(line, sizeof(line), "DTSTAMP:%04d%02d%02dT%02d%02d%02dZ", stamp.tm_year + 1900, stamp.tm_mon + 1, stamp.tm_mday, stamp.tm_hour, stamp.tm_min, stamp.tm_sec);
	length = store_put(text, length, STORE_FILE_MAX, line);

	/* Its day, or its start and end in the local time. */
	if (item->all_day) {
		(void)snprintf(line, sizeof(line), "DTSTART;VALUE=DATE:%04d%02d%02d", item->date.year, item->date.month, item->date.day);
		length = store_put(text, length, STORE_FILE_MAX, line);
		if (!item->memo) {
			next = item->date;
			cal_add_days(&next, 1);
			(void)snprintf(line, sizeof(line), "DTEND;VALUE=DATE:%04d%02d%02d", next.year, next.month, next.day);
			length = store_put(text, length, STORE_FILE_MAX, line);
		}
	} else {
		(void)snprintf(line, sizeof(line), "DTSTART:%04d%02d%02dT%02d%02d00", item->date.year, item->date.month, item->date.day, item->start / 60, item->start % 60);
		length = store_put(text, length, STORE_FILE_MAX, line);
		(void)snprintf(line, sizeof(line), "DTEND:%04d%02d%02dT%02d%02d00", item->date.year, item->date.month, item->date.day, item->end / 60, item->end % 60);
		length = store_put(text, length, STORE_FILE_MAX, line);
	}

	/* Its title and a memo's words. */
	length = store_put_escaped(text, length, STORE_FILE_MAX, "SUMMARY", item->title);
	if (item->memo && item->text != NULL)
		length = store_put_escaped(text, length, STORE_FILE_MAX, "DESCRIPTION", item->text);
	if (!item->memo) {
		(void)snprintf(line, sizeof(line), "CATEGORIES:%s", store_lists[item->list]);
		length = store_put(text, length, STORE_FILE_MAX, line);
	}

	/* The ends. */
	(void)snprintf(line, sizeof(line), "END:%s", component);
	length = store_put(text, length, STORE_FILE_MAX, line);
	length = store_put(text, length, STORE_FILE_MAX, "END:VCALENDAR");

	/* Written. */
	store_path(item, path, sizeof(path));
	error = ENOSPC;
	if (length < STORE_FILE_MAX)
		error = store_write_file(path, text, length);
	free(text);
	if (error != 0)
		return error;

	/* Succeeded: the file holds the item. */
	return 0;
}

/* Appends a line with its CR LF, folded at 75 bytes (not inside a UTF-8 character); returns the length after. */
static size_t
store_put(
	char *text,
	size_t at,
	size_t size,
	const char *line)
{
	size_t length;
	size_t done;
	size_t part;
	size_t room;

	/* Each piece of the line. */
	length = strlen(line);
	done = 0;
	room = STORE_FOLD;
	while (done < length) {
		/* As much as fits on the line, back to a character's start. */
		part = length - done;
		if (part > room) {
			part = room;
			while (part > 0U && ((unsigned char)line[done + part] & 0xc0U) == 0x80U)
				part--;
		}

		/* The piece, and a fold when more follows. */
		if (at + part + 3U >= size)
			return size;
		memcpy(text + at, line + done, part);
		at += part;
		done += part;
		if (done < length) {
			memcpy(text + at, "\r\n ", 3U);
			at += 3U;
			room = STORE_FOLD - 1U;
		}
	}

	/* The line's end. */
	if (at + 2U >= size)
		return size;
	memcpy(text + at, "\r\n", 2U);
	return at + 2U;
}

/* Appends a property with its text escaped (backslash, semicolon, comma, line ends). */
static size_t
store_put_escaped(
	char *text,
	size_t at,
	size_t size,
	const char *name,
	const char *value)
{
	char line[STORE_LINE_MAX];
	size_t length;
	size_t index;

	/* The name and the escaped value. */
	length = (size_t)snprintf(line, sizeof(line), "%s:", name);
	for (index = 0; value[index] != '\0' && length + 3U < sizeof(line); index++) {
		/* A line end as \n. */
		if (value[index] == '\n') {
			line[length] = '\\';
			line[length + 1U] = 'n';
			length += 2U;
			continue;
		}

		/* A CR is dropped. */
		if (value[index] == '\r')
			continue;

		/* A backslash, a semicolon and a comma escaped. */
		if (value[index] == '\\' || value[index] == ';' || value[index] == ',') {
			line[length] = '\\';
			length++;
		}

		/* The byte. */
		line[length] = value[index];
		length++;
	}

	/* The line's end. */
	line[length] = '\0';

	/* The line. */
	return store_put(text, at, size, line);
}

/* Takes the escapes of an iCalendar text away in place. */
static void
store_unescape(
	char *value)
{
	size_t read_at;
	size_t write_at;

	/* Each byte; an escape gives the byte after it (\n a line end). */
	read_at = 0;
	write_at = 0;
	while (value[read_at] != '\0') {
		if (value[read_at] == '\\' && value[read_at + 1U] != '\0') {
			read_at++;
			value[write_at] = value[read_at];
			if (value[read_at] == 'n' || value[read_at] == 'N')
				value[write_at] = '\n';
		} else {
			value[write_at] = value[read_at];
		}

		/* The next. */
		read_at++;
		write_at++;
	}

	/* The text's end. */
	value[write_at] = '\0';
}

/* Appends an item (a memo's words copied). */
static int
store_append(
	const struct cal_item *item)
{
	struct cal_item *grown;
	struct cal_item *kept;
	size_t capacity;

	/* Room for one more. */
	if (store_count == store_capacity) {
		capacity = store_capacity * 2U;
		if (capacity == 0U)
			capacity = 64U;
		grown = realloc(store_items, capacity * sizeof(grown[0]));
		if (grown == NULL)
			return ENOMEM;
		store_items = grown;
		store_capacity = capacity;
	}

	/* The item, its words of its own. */
	kept = &store_items[store_count];
	*kept = *item;
	kept->text = NULL;
	if (item->text != NULL) {
		kept->text = malloc(strlen(item->text) + 1U);
		if (kept->text == NULL)
			return ENOMEM;
		memcpy(kept->text, item->text, strlen(item->text) + 1U);
	}

	/* Succeeded: one more item. */
	store_count++;
	return 0;
}

/* Frees an item's words. */
static void
store_free_item(
	struct cal_item *item)
{
	/* A memo's words. */
	free(item->text);
	item->text = NULL;
}

/* Makes an item's file's path: its calendar's folder (Memos for a memo) and its UID. */
static void
store_path(
	const struct cal_item *item,
	char *path,
	size_t size)
{
	char name[sizeof(item->uid)];
	size_t index;

	/* The UID with the characters a file's name cannot have made "_". */
	(void)snprintf(name, sizeof(name), "%s", item->uid);
	for (index = 0; name[index] != '\0'; index++) {
		if (name[index] == '/' || name[index] == '\\')
			name[index] = '_';
	}

	/* The folder and the name. */
	if (item->memo)
		(void)snprintf(path, size, "%s/%s/%s.ics", store_root, STORE_MEMOS, name);
	else
		(void)snprintf(path, size, "%s/%s/%s.ics", store_root, store_lists[item->list], name);
}

/* Writes a file beside its place and renames it over it. */
static int
store_write_file(
	const char *path,
	const char *text,
	size_t length)
{
	char fresh[STORE_PATH_MAX + 8U];
	size_t written;
	FILE *file;
	int status;

	/* The new file. */
	(void)snprintf(fresh, sizeof(fresh), "%s.new", path);
	file = fopen(fresh, "w");
	if (file == NULL)
		return errno;
	written = fwrite(text, 1U, length, file);
	status = fclose(file);
	if (written != length || status != 0)
		return EIO;

	/* In place. */
	status = rename(fresh, path);
	if (status != 0)
		return errno;

	/* Succeeded: the file is written. */
	return 0;
}
