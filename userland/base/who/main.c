/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD who userland command.
 */

#include <utmpx.h>

#include <stdio.h>
#include <stdint.h>

static void utc_fields(int64_t seconds, int *year, int *month, int *day, int *hour, int *minute);
static int leap(int y);

/*
 * Runs the who command.
 */
int
main(
	void)
{
	int y, m, d, h, n;
	struct utmpx *entry;

	setutxent();

	/* Continue while the operation condition remains true. */
	while ((entry = getutxent()) != NULL)

		/* Handles the entry condition. */
		if (entry->ut_type == USER_PROCESS) {

			utc_fields(entry->ut_tv_sec, &y, &m, &d, &h, &n);
			printf("%-16s %-16s %04d-%02d-%02d %02d:%02d\n",
			       entry->ut_user, entry->ut_line, y, m, d, h, n);
		}
	endutxent();

	/* Reports successful completion. */
	return 0;
}

/* Supports the utc fields operation. */
static void
utc_fields(
	int64_t seconds,
	int *year,
	int *month,
	int *day,
	int *hour,
	int *minute)
{
	static const int mdays[12] = {31, 28, 31, 30, 31, 30,
				      31, 31, 30, 31, 30, 31};
	int64_t days;
	int y, m, d;

	days = seconds / 86400;
	y = 1970;
	m = 0;

	/* Handles the seconds condition. */
	if (seconds < 0) {
		*year = 1970;
		*month = *day = *hour = *minute = 0;
		/* Returns the computed result. */
		return;
	}

	/* Continue while the operation condition remains true. */
	*hour = (int)((seconds % 86400) / 3600);
	*minute = (int)((seconds % 3600) / 60);
	while (days >= 365 + leap(y)) {
		days -= 365 + leap(y);
		y++;
	}
	while (m < 12) {
		d = mdays[m] + (m == 1 && leap(y));

		/* Handles the days condition. */
		if (days < d)
			break;
		days -= d;
		m++;
	}
	*year = y;
	*month = m + 1;
	*day = (int)days + 1;
}

/* Supports the leap operation. */
static int
leap(
	int y)
{
	/* Returns the computed result. */
	return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}
