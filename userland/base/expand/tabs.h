/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the tab stops of expand and unexpand.
 */

#ifndef USERLAND_BASE_EXPAND_TABS_H
#define USERLAND_BASE_EXPAND_TABS_H

#include <stddef.h>

/* The most tab stops a list holds. */
#define TAB_STOPS_MAX 256

/*
 * The tab stops of a -t tablist.
 *
 * With count 1 the stops repeat every stops[0] columns.  With more, they
 * are the listed columns, ascending, counted from 0 as the columns of a
 * line are; there is no stop after the last one.
 */
struct tab_stops {
	size_t stops[TAB_STOPS_MAX];
	size_t count;
};

int tab_stops_parse(const char *program, const char *text, struct tab_stops *tabs);
void tab_stops_default(struct tab_stops *tabs);
int tab_stops_next(const struct tab_stops *tabs, size_t column, size_t *next);

#endif
