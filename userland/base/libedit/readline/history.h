/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_READLINE_HISTORY_H
#define ZEDBSD_READLINE_HISTORY_H

typedef struct _hist_entry {
	char *line;
	char *timestamp;
} HIST_ENTRY;

extern int history_base;
extern int history_length;

void using_history(void);
void add_history(const char *);
void clear_history(void);
int history_set_pos(int);
HIST_ENTRY *current_history(void);
HIST_ENTRY *previous_history(void);
HIST_ENTRY *next_history(void);

#endif
