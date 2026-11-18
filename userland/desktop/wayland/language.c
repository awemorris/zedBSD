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
#include <stdlib.h>
#include <string.h>

/* A language libkeiland names fits the name the event loop keeps. */
_Static_assert(KL_TR_NAME_MAX + 1U <= KWL_LANGUAGE_NAME_SIZE, "language name");

/* The longest line of the system's language file read. */
#define LANGUAGE_LINE_MAX	32U

/*
 * A language's catalogs read on the file I/O thread: the language (one of
 * libkeiland's names, which live as long as the program), and the
 * catalogs read or the error.
 */
struct language_job {
	struct kwl_lane_job lane;
	const char *language;
	int error;
	struct kl_tr_catalogs *catalogs;
};

static void language_open(struct kwl_server *server, const char *language, const char *why);
static void language_applied(struct kwl_server *server, int error, const char *why, const char *by);
static void language_read_run(struct kwl_lane_job *job);
static void language_read_done(void *context, struct kwl_lane_job *job);
static void language_read_drop(struct kwl_lane_job *job);
static const char *language_weekday(int day, int form);
static const char *language_month(int month, int form);

/* The operations of a read of the catalogs. */
static const struct kwl_lane_ops language_read_ops = {
	language_read_run,
	language_read_done,
	language_read_drop
};

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
 * Takes the session's language from the setting.  The first is read here
 * (the session's start, before the screen is shown); a change while the
 * session runs is read on the file I/O thread and used when it is done.
 */
void
kwl_language_set(
	struct kwl_server *server,
	int setting)
{
	struct language_job *job;
	const char *language;
	int differs;
	int error;

	/* A value no language has is English. */
	language = kl_tr_language_code(setting);
	if (language == NULL)
		language = "en";

	/* The language asked for last, once read or being read, changes nothing. */
	differs = strcmp(language, server->language_asked);
	if (differs == 0 && server->language_set)
		return;
	(void)snprintf(server->language_asked, sizeof(server->language_asked), "%s", language);

	/* The start reads it at once. */
	if (!server->language_set) {
		language_open(server, language, "setting");
		return;
	}

	/* A change is read on the file I/O thread, which replaces a read not started. */
	job = calloc(1U, sizeof(*job));
	if (job != NULL) {
		job->language = language;
		error = kwl_io_file_submit(server, &job->lane, &language_read_ops, KWL_IO_KEY_LANGUAGE);
		if (error == 0)
			return;
		free(job);
	}

	/* Without the thread, or without memory for the job, it is read here. */
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

/* Reads the catalogs of a language here and draws the screen again in it. */
static void
language_open(
	struct kwl_server *server,
	const char *language,
	const char *why)
{
	int error;

	/* The compositor's catalogs (and the shared one); a failure leaves English. */
	error = kl_tr_open(KWL_LANGUAGE_DOMAIN, language);
	language_applied(server, error, why, "event-loop");
}

/*
 * Draws the screen again in the language the catalogs now give, App
 * Home's picture with its names (layer-cache.c).
 */
static void
language_applied(
	struct kwl_server *server,
	int error,
	const char *why,
	const char *by)
{
	/* What is in use is what is asked for now (English after a failure, which a new ask tries again). */
	(void)snprintf(server->language_asked, sizeof(server->language_asked), "%s", kl_tr_language());

	/* The screen and App Home's picture. */
	server->language_set = 1;
	server->dirty = 1;
	server->home_picture_stale = 1U;

	/* The log line the tests read. */
	printf("KWL LANGUAGE language=%s from=%s error=%d by=%s\n", kl_tr_language(), why, error, by);
}

/* Reads a language's catalogs on the file I/O thread; the texts in use do not change. */
static void
language_read_run(
	struct kwl_lane_job *lane_job)
{
	struct language_job *job;

	/* The catalogs, into the job's record. */
	job = (struct language_job *)lane_job;
	job->error = kl_tr_read(KWL_LANGUAGE_DOMAIN, job->language, &job->catalogs);
}

/*
 * Uses the catalogs read, on the event loop's thread, when they are still
 * the language asked for (a newer one being read replaces them).
 */
static void
language_read_done(
	void *context,
	struct kwl_lane_job *lane_job)
{
	struct kwl_server *server;
	struct language_job *job;
	int differs;

	/* The server, and the job the thread has run. */
	server = context;
	job = (struct language_job *)lane_job;

	/* A language asked for since comes with its own read. */
	differs = strcmp(job->language, server->language_asked);
	if (differs != 0) {
		language_read_drop(lane_job);
		return;
	}

	/* The catalogs read, or English after a failure, as kl_tr_open leaves them. */
	if (job->error == 0)
		kl_tr_use(job->catalogs);
	else
		kl_tr_close();
	job->catalogs = NULL;
	language_applied(server, job->error, "setting", "io-file");

	/* The record. */
	free(job);
}

/* Frees a read that is not used (replaced, or not run). */
static void
language_read_drop(
	struct kwl_lane_job *lane_job)
{
	struct language_job *job;

	/* The catalogs read, if any, then the record. */
	job = (struct language_job *)lane_job;
	kl_tr_discard(job->catalogs);
	free(job);
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
