/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the parts of the vi editor that its source files share.
 */

#ifndef USERLAND_BASE_VI_VI_H
#define USERLAND_BASE_VI_VI_H

#include <regex.h>
#include <signal.h>
#include <stddef.h>
#include <termios.h>

#include "userland/base/common/terminfo.h"

/*
 * The largest file vi opens, in bytes, and the most lines it holds.  A
 * larger file is refused before anything is read, so the editor never
 * runs the machine out of memory on a log or a disk image.
 */
#define VI_FILE_SIZE_LIMIT (64UL * 1024UL * 1024UL)
#define VI_LINE_COUNT_LIMIT 4194304UL

/* Marks a parameter a function must take but does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The number of changes u can take back; older ones are forgotten. */
#define VI_UNDO_GROUP_LIMIT 1000U

/*
 * The most bytes the history of changes keeps.  Older changes are
 * forgotten beyond it, so a few large deletions cannot hold many times
 * the size of the file.
 */
#define VI_UNDO_BYTE_LIMIT VI_FILE_SIZE_LIMIT

/*
 * The largest count a command takes; a longer number is cut to it.  The
 * interrupt key is an ordinary key while vi runs, so a count must stay
 * small enough to finish.
 */
#define VI_COUNT_LIMIT 1000000UL

/* The length of a status message and of a typed : or / line. */
#define VI_MESSAGE_LENGTH 256U
#define VI_INPUT_LENGTH 1024U

/* The size of the output that is collected before one write. */
#define VI_OUTPUT_LENGTH 4096U

/* The most keys one command may take and still be repeated by the . command. */
#define VI_RECORD_LENGTH 4096U

/* The width of a tab stop on the screen. */
#define VI_TAB_WIDTH 8U

/* The smallest screen vi draws on; a smaller one is treated as this. */
#define VI_MINIMUM_ROWS 2
#define VI_MINIMUM_COLUMNS 10

/*
 * The keys that have no byte of their own.  A plain byte is its own key
 * value, from 0 to 255; these come after.
 */
#define VI_KEY_UP 0x101
#define VI_KEY_DOWN 0x102
#define VI_KEY_LEFT 0x103
#define VI_KEY_RIGHT 0x104
#define VI_KEY_HOME 0x105
#define VI_KEY_END 0x106
#define VI_KEY_DELETE 0x107
#define VI_KEY_PAGE_UP 0x108
#define VI_KEY_PAGE_DOWN 0x109

/* A key sequence vi does not know, read whole and then ignored. */
#define VI_KEY_UNKNOWN 0x10a

/* Not a key: the editor was told to stop (a hangup or a termination). */
#define VI_KEY_STOP 0x1ff

/* The control keys vi gives a meaning to. */
#define VI_KEY_CONTROL_B 0x02
#define VI_KEY_CONTROL_C 0x03
#define VI_KEY_CONTROL_D 0x04
#define VI_KEY_CONTROL_F 0x06
#define VI_KEY_CONTROL_G 0x07
#define VI_KEY_BACKSPACE 0x08
#define VI_KEY_TAB 0x09
#define VI_KEY_NEWLINE 0x0a
#define VI_KEY_CONTROL_L 0x0c
#define VI_KEY_RETURN 0x0d
#define VI_KEY_CONTROL_R 0x12
#define VI_KEY_CONTROL_U 0x15
#define VI_KEY_CONTROL_W 0x17
#define VI_KEY_CONTROL_Z 0x1a
#define VI_KEY_ESCAPE 0x1b
#define VI_KEY_ERASE 0x7f

/*
 * What the editor is doing, which decides how the cursor may sit and
 * what the bottom line of the screen shows.
 */
enum vi_mode {
	VI_MODE_COMMAND,
	VI_MODE_INSERT,
	VI_MODE_LINE
};

/*
 * The kinds of change the buffer knows.  Every edit is made of these, and
 * each kind has an opposite that takes it back.
 */
enum vi_change_kind {
	VI_CHANGE_INSERT_TEXT,
	VI_CHANGE_DELETE_TEXT,
	VI_CHANGE_SPLIT_LINE,
	VI_CHANGE_JOIN_LINE,
	VI_CHANGE_INSERT_LINE,
	VI_CHANGE_DELETE_LINE
};

/*
 * One line of the file being edited.
 *
 * The text has no newline.  A NUL byte always follows the text, outside
 * its length, so the regular expression functions can read the line.
 */
struct vi_line {
	char *text;
	size_t length;
	size_t capacity;
};

/*
 * The lines of the file being edited.
 *
 * There is always at least one line: an empty file is one empty line.
 * bytes is the size the lines would have in a file, a newline after
 * each, which edits keep within the file size limit.
 */
struct vi_buffer {
	struct vi_line *lines;
	size_t count;
	size_t capacity;
	size_t bytes;
};

/*
 * A place in the buffer: a line and a byte offset in it.
 *
 * In command mode the offset is the start of a character and lies on the
 * line's text; in insert mode it may also be the end of the line.
 */
struct vi_position {
	size_t line;
	size_t offset;
};

/*
 * One change made to the buffer, kept so that u can take it back and
 * CONTROL-R can make it again.
 *
 * The text is what was inserted or deleted.  For the line kinds it holds
 * count whole lines, each ended by a newline, so that many lines move in
 * one step.  A split or a join keeps no text.
 */
struct vi_change {
	enum vi_change_kind kind;
	size_t line;
	size_t offset;
	char *text;
	size_t length;
	size_t count;
};

/*
 * The changes one command made, taken back together by one u.
 *
 * The cursor is where the command started, which is where u puts it.
 * serial names the state of the buffer after the group; no two groups
 * ever share one.
 */
struct vi_undo_group {
	struct vi_change *changes;
	size_t count;
	size_t capacity;
	struct vi_position cursor;
	unsigned long serial;
};

/*
 * The history of changes.
 *
 * Groups before current are applied to the buffer; groups from current to
 * count were taken back and can be made again.  The state of the buffer
 * is named by the serial of the last applied group, or by base_serial
 * when none is applied (the serial of the last group forgotten, 0 at the
 * start).  saved_serial names the state last read or written, so the
 * buffer is unchanged exactly when its state has that name.  next_serial
 * only grows.  bytes is the text all groups hold, kept within
 * VI_UNDO_BYTE_LIMIT.  open is the group the running command adds to,
 * valid while open_active is set.
 */
struct vi_undo {
	struct vi_undo_group *groups;
	size_t count;
	size_t capacity;
	size_t current;
	unsigned long base_serial;
	unsigned long saved_serial;
	unsigned long next_serial;
	size_t bytes;
	struct vi_undo_group open;
	int open_active;
};

/*
 * The text y, d, c and x leave behind for p and P.
 *
 * A line-wise register holds whole lines, each ended by a newline.  A
 * character-wise register holds text in which a newline joins two lines.
 */
struct vi_register {
	char *text;
	size_t length;
	int linewise;
};

/*
 * The terminal vi draws on.
 *
 * The capability strings are the terminal's, or the VT100 ones when the
 * terminal has no description.  Output is collected in the output buffer
 * and written at the end of each screen update.  A byte read while
 * telling ESC from a key sequence, and found to start the next key, waits
 * in pushed_byte.  wait_mask is the signal mask used only while waiting
 * for a key; the editor's signals are blocked at all other times.
 */
struct vi_terminal {
	int input;
	int output;
	struct termios saved_modes;
	int modes_saved;
	int raw;
	int screen_entered;
	int rows;
	int columns;
	char move_string[TERMINFO_VALUE_LENGTH];
	char clear_line_string[TERMINFO_VALUE_LENGTH];
	char clear_string[TERMINFO_VALUE_LENGTH];
	char enter_string[TERMINFO_VALUE_LENGTH];
	char leave_string[TERMINFO_VALUE_LENGTH];
	char hide_cursor_string[TERMINFO_VALUE_LENGTH];
	char show_cursor_string[TERMINFO_VALUE_LENGTH];
	char normal_string[TERMINFO_VALUE_LENGTH];
	char bell_string[TERMINFO_VALUE_LENGTH];
	char output_buffer[VI_OUTPUT_LENGTH];
	size_t output_length;
	unsigned char pushed_byte;
	int has_pushed_byte;
	sigset_t wait_mask;
	int wait_mask_set;
};

/*
 * One row of the screen as it was last drawn, so that an update rewrites
 * only the rows that changed.  width is how many columns the text fills.
 */
struct vi_screen_row {
	char *text;
	size_t length;
	int width;
	int valid;
};

/*
 * A list of keys: the keys of the running command, the keys of the last
 * change, or the keys . feeds back into the editor.
 *
 * overflow is set when a key did not fit; such a list is never repeated.
 * count_end is the number of leading keys that were the command's count.
 */
struct vi_keys {
	int keys[VI_RECORD_LENGTH];
	size_t length;
	size_t count_end;
	size_t position;
	int overflow;
};

/*
 * The whole state of one editing session.
 *
 * changed is set by any edit of the buffer during the running command, so
 * that the main loop knows to keep the command's keys for the . command.
 */
struct vi_editor {
	struct vi_buffer buffer;
	struct vi_undo undo;
	struct vi_register yank;
	struct vi_terminal terminal;
	char *file_name;
	struct vi_position cursor;
	struct vi_position insert_start;
	size_t want_column;
	size_t top;
	size_t top_skip;
	size_t bottom;
	enum vi_mode mode;
	char message[VI_MESSAGE_LENGTH];
	char input[VI_INPUT_LENGTH];
	size_t input_length;
	char input_prompt;
	regex_t search_regex;
	int search_compiled;
	int search_backward;
	int find_key;
	char find_text[8];
	size_t find_length;
	int pending_key;
	struct vi_keys record;
	struct vi_keys repeat;
	struct vi_keys replay;
	int recording;
	int changed;
	int quit;
	int exit_status;
	struct vi_screen_row *screen;
	int screen_rows;
};

/* buffer.c */
int vi_buffer_init(struct vi_buffer *buffer);
void vi_buffer_free(struct vi_buffer *buffer);
int vi_buffer_append_line(struct vi_buffer *buffer, const char *text, size_t length);
int vi_buffer_apply(struct vi_buffer *buffer, const struct vi_change *change, int reverse);

/* undo.c */
void vi_undo_init(struct vi_undo *undo);
void vi_undo_free(struct vi_undo *undo);
int vi_undo_begin(struct vi_editor *editor);
void vi_undo_end(struct vi_editor *editor);
void vi_undo_mark_saved(struct vi_editor *editor);
int vi_undo_modified(const struct vi_editor *editor);
int vi_undo_back(struct vi_editor *editor);
int vi_undo_forward(struct vi_editor *editor);
int vi_edit(struct vi_editor *editor, enum vi_change_kind kind, size_t line, size_t offset, const char *text, size_t length);

/* file.c */
int vi_file_load(struct vi_editor *editor, const char *path);
int vi_file_write(struct vi_editor *editor, const char *path, int force);
void vi_file_recover(struct vi_editor *editor);

/* terminal.c */
int vi_terminal_open(struct vi_terminal *terminal);
int vi_terminal_enter(struct vi_terminal *terminal);
void vi_terminal_leave(struct vi_terminal *terminal);
void vi_terminal_measure(struct vi_terminal *terminal);
void vi_terminal_put(struct vi_terminal *terminal, const char *bytes, size_t length);
void vi_terminal_move(struct vi_terminal *terminal, int row, int column);
void vi_terminal_command(struct vi_terminal *terminal, const char *capability);
void vi_terminal_flush(struct vi_terminal *terminal);
int vi_terminal_read_key(struct vi_terminal *terminal);
int vi_terminal_input_pending(struct vi_terminal *terminal);

/* screen.c */
size_t vi_text_next(const char *text, size_t length, size_t offset);
size_t vi_text_previous(const char *text, size_t length, size_t offset);
unsigned long vi_text_code(const char *text, size_t length, size_t offset);
size_t vi_text_column(const char *text, size_t length, size_t offset);
size_t vi_text_offset_at_column(const char *text, size_t length, size_t column);
void vi_screen_invalidate(struct vi_editor *editor);
void vi_screen_free(struct vi_editor *editor);
void vi_screen_update(struct vi_editor *editor);
void vi_screen_center(struct vi_editor *editor);

/* command.c */
void vi_command(struct vi_editor *editor, int key);
void vi_command_finish(struct vi_editor *editor);
int vi_editor_key(struct vi_editor *editor);
void vi_message(struct vi_editor *editor, const char *text);
void vi_bell(struct vi_editor *editor);
void vi_cursor_clamp(struct vi_editor *editor);
void vi_cursor_first_nonblank(struct vi_editor *editor);
int vi_read_line(struct vi_editor *editor, char prompt);

/* ex.c */
void vi_ex_command(struct vi_editor *editor, const char *line);
void vi_ex_file_status(struct vi_editor *editor);

/* search.c */
int vi_search_set(struct vi_editor *editor, const char *pattern, int backward);
int vi_search_find(struct vi_editor *editor, struct vi_position from, int backward, struct vi_position *found);

/* main.c */
int vi_signal_pending(struct vi_editor *editor);
void vi_suspend(struct vi_editor *editor);

#endif
