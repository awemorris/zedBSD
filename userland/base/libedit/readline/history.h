/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland history interface.
 */

#ifndef KERN_READLINE_HISTORY_H
#define KERN_READLINE_HISTORY_H

/*
 * One line of the history, as GNU Readline lays it out.  The history owns
 * the line; no timestamp is kept (it is always NULL).
 */
typedef struct _hist_entry {
	char *line;
	char *timestamp;
} HIST_ENTRY;

/* The number of the oldest entry, and how many entries there are. */
extern int history_base;
extern int history_length;

/* Keeping lines, and walking them from the line being typed. */
void using_history(void);
void add_history(const char *line);
void clear_history(void);
int history_set_pos(int position);
HIST_ENTRY *current_history(void);
HIST_ENTRY *previous_history(void);
HIST_ENTRY *next_history(void);

#endif
