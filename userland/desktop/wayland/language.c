/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's language (WS158): which catalogs its text is read
 * from, and the dates it draws (language.h).
 */

#include "userland/desktop/paths.h"

#include "language.h"
#include "kwl.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The longest line of the system's language file read. */
#define LANGUAGE_LINE_MAX	32U

static void language_open(struct kwl_server *server, const char *language, const char *why);
static const char *language_weekday(int day, int form);
static const char *language_month(int month, int form);

/*
 * Reads the system's language for the login screen.
 */
void
kwl_language_system(
	struct kwl_server *server)
{
	char word[LANGUAGE_LINE_MAX];
	int error;

	/* The file's first line; no file, or an empty one, is English. */
	error = kwl_language_system_word(word, sizeof(word));
	if (error != 0) {
		language_open(server, "en", "system");
		return;
	}

	/* Succeeded: the language it names (a name the catalogs refuse is English). */
	language_open(server, word, "system");
}

/*
 * Reads the first line of the system's language file without its end, as
 * it is (ws188-p002: the machine's answer and the login screen share it).
 * Returns 0 with the line, or ENOENT when the file cannot be read or has
 * no line (the word is then empty).  It only reads a file, so the
 * machine's thread may call it.
 */
int
kwl_language_system_word(
	char *word,
	size_t size)
{
	char line[LANGUAGE_LINE_MAX];
	FILE *file;
	char *read;
	size_t length;

	/* Nothing yet. */
	word[0] = '\0';

	/* The file. */
	file = fopen(KWL_LANGUAGE_SYSTEM_PATH, "r");
	if (file == NULL)
		return ENOENT;

	/* Its first line. */
	read = fgets(line, sizeof(line), file);
	(void)fclose(file);
	if (read == NULL)
		return ENOENT;

	/* The line without its end, cut to the room. */
	length = strcspn(line, "\r\n");
	if (length >= size)
		length = size - 1U;
	memcpy(word, line, length);
	word[length] = '\0';

	/* Succeeded: the line as it is. */
	return 0;
}

/*
 * Takes the session's language from the setting.
 */
void
kwl_language_set(
	struct kwl_server *server,
	int setting)
{
	const char *language;
	int differs;

	/* A value no language has is English. */
	language = kl_tr_language_code(setting);
	if (language == NULL)
		language = "en";

	/* The same language, once read, changes nothing. */
	differs = strcmp(language, kl_tr_language());
	if (differs == 0 && server->language_set)
		return;

	/* Succeeded: the catalogs of the language. */
	language_open(server, language, "setting");
}

/*
 * Writes a date in the language.
 */
void
kwl_language_date(
	const struct tm *local,
	int form,
	char *out,
	size_t size)
{
	char day[8];
	char time_of_day[8];
	const char *weekday;
	const char *month;
	const char *pattern;

	/* The parts: the weekday's and the month's names, the day of the month, and the time. */
	weekday = language_weekday(local->tm_wday, form);
	month = language_month(local->tm_mon, form);
	(void)snprintf(day, sizeof(day), "%d", local->tm_mday);
	(void)snprintf(time_of_day, sizeof(time_of_day), "%02d:%02d", local->tm_hour, local->tm_min);

	/* The order the language puts them in. */
	if (form == KWL_LANGUAGE_DATE_LONG) {
		pattern = kl_trc("long date", "{1}, {2} {3}");
	} else {
		pattern = kl_trc("short date", "{1} {2} {3}  {4}");
	}

	/* Succeeded: the date. */
	(void)kl_tr_format(out, size, pattern, weekday, month, day, time_of_day, (const char *)NULL);
}

/* Reads the catalogs of a language and draws the screen again in it. */
static void
language_open(
	struct kwl_server *server,
	const char *language,
	const char *why)
{
	int error;

	/* The compositor's catalogs (and the shared one); a failure leaves English. */
	error = kl_tr_open(KWL_LANGUAGE_DOMAIN, language);
	server->language_set = 1;
	server->dirty = 1;

	/* The log line the tests read. */
	printf("KWL LANGUAGE language=%s from=%s error=%d\n", kl_tr_language(), why, error);
}

/* Names a day of the week (0 Sunday) in the language, short or long. */
static const char *
language_weekday(
	int day,
	int form)
{
	/* Each day. */
	switch (day) {
	case 0:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("weekday", "Sunday");
		return kl_trc("weekday", "Sun");
	case 1:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("weekday", "Monday");
		return kl_trc("weekday", "Mon");
	case 2:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("weekday", "Tuesday");
		return kl_trc("weekday", "Tue");
	case 3:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("weekday", "Wednesday");
		return kl_trc("weekday", "Wed");
	case 4:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("weekday", "Thursday");
		return kl_trc("weekday", "Thu");
	case 5:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("weekday", "Friday");
		return kl_trc("weekday", "Fri");
	case 6:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("weekday", "Saturday");
		return kl_trc("weekday", "Sat");
	default:
		break;
	}

	/* A day out of range has no name. */
	return "";
}

/* Names a month (0 January) in the language, short or long. */
static const char *
language_month(
	int month,
	int form)
{
	/* Each month. */
	switch (month) {
	case 0:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("month", "January");
		return kl_trc("month", "Jan");
	case 1:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("month", "February");
		return kl_trc("month", "Feb");
	case 2:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("month", "March");
		return kl_trc("month", "Mar");
	case 3:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("month", "April");
		return kl_trc("month", "Apr");
	case 4:
		/* May is the same word in both forms. */
		return kl_trc("month", "May");
	case 5:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("month", "June");
		return kl_trc("month", "Jun");
	case 6:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("month", "July");
		return kl_trc("month", "Jul");
	case 7:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("month", "August");
		return kl_trc("month", "Aug");
	case 8:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("month", "September");
		return kl_trc("month", "Sep");
	case 9:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("month", "October");
		return kl_trc("month", "Oct");
	case 10:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("month", "November");
		return kl_trc("month", "Nov");
	case 11:
		if (form == KWL_LANGUAGE_DATE_LONG)
			return kl_trc("month", "December");
		return kl_trc("month", "Dec");
	default:
		break;
	}

	/* A month out of range has no name. */
	return "";
}
