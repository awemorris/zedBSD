/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ssh_config the settings tab's Hosts pane edits a host at a time
 * (settings-hosts.c): its text read into lines and blocks, and the
 * smallest changes of the text a host's form makes.
 *
 * The text is the file's bytes as they are.  A change rewrites only the
 * lines it is about -- a value in place, a line added or one taken out --
 * and keeps every other byte: the comments, the keywords the page does not
 * know, Match and Include, the blanks, the tabs and the line breaks (a
 * line added ends as the line it follows does).  Each change gives a new
 * text, which is read again for the next.
 *
 * Nothing here knows the SSH library: whether a text is right is the
 * program's check (struct terminal_page_program), and this file only says
 * whether a check's problem is one the text had before.
 */

#ifndef TERMINAL_SETTINGS_HOSTS_H
#define TERMINAL_SETTINGS_HOSTS_H

#include <stddef.h>

/* What a line of the text is. */
#define TERMINAL_HOSTS_LINE_EMPTY	0U
#define TERMINAL_HOSTS_LINE_COMMENT	1U
#define TERMINAL_HOSTS_LINE_HOST	2U
#define TERMINAL_HOSTS_LINE_MATCH	3U
#define TERMINAL_HOSTS_LINE_KEYWORD	4U
#define TERMINAL_HOSTS_LINE_BAD		5U

/* What a block of the text is: the lines before the first Host or Match, a Host's, or a Match's. */
#define TERMINAL_HOSTS_BLOCK_BEFORE	0U
#define TERMINAL_HOSTS_BLOCK_HOST	1U
#define TERMINAL_HOSTS_BLOCK_MATCH	2U

/* The largest text the pane edits (as large as the SSH library reads), and the longest value of one line with its NUL. */
#define TERMINAL_HOSTS_TEXT_MAX		(256U * 1024U)
#define TERMINAL_HOSTS_VALUE_MAX	4100U

/* How a line that matches no other is answered by terminal_hosts_find and terminal_hosts_overrides: no line. */
#define TERMINAL_HOSTS_NONE		((unsigned)-1)

/*
 * One line of the text, as byte offsets into it: where it starts, where
 * its words end (before the CR LF or LF that ends it), and where the next
 * line starts.  A keyword's line also has its keyword's place (after the
 * blanks it is indented by) and its value's: after the separator (blanks,
 * or an '=' with blanks round it), up to a comment that ends the line and
 * the blanks before it (RemoteCommand's value runs to the line's end, as
 * ssh reads it).  number counts the lines from 1; block is the block the
 * line is in.
 */
struct terminal_hosts_line {
	size_t start;
	size_t end;
	size_t next;
	size_t keyword;
	size_t keyword_end;
	size_t value;
	size_t value_end;
	unsigned kind;
	unsigned block;
	unsigned number;
};

/*
 * One block of the text: its kind, and its lines from first to one before
 * end.  A Host's or a Match's first line is its Host or Match line; the
 * block before them may have no line at all (first equals end).
 */
struct terminal_hosts_block {
	unsigned kind;
	unsigned first;
	unsigned end;
};

/*
 * A text read into lines and blocks.  It points into the text it was read
 * from, which must outlive it; terminal_hosts_release frees the tables.
 * Block 0 is always the part before the first Host or Match.
 */
struct terminal_hosts {
	const char *text;
	size_t size;
	struct terminal_hosts_line *lines;
	unsigned line_count;
	struct terminal_hosts_block *blocks;
	unsigned block_count;
};

/*
 * A program's check of an ssh_config's text: writes what is wrong with it
 * ("line N: what", or another text without a line), empty when nothing is.
 */
typedef int (*terminal_hosts_checker)(void *data, const char *text, size_t size, char *report, size_t capacity);

/* Reading the text. */
int terminal_hosts_read(struct terminal_hosts *hosts, const char *text, size_t size);
void terminal_hosts_release(struct terminal_hosts *hosts);
int terminal_hosts_keyword_is(const struct terminal_hosts *hosts, unsigned line, const char *keyword);
unsigned terminal_hosts_find(const struct terminal_hosts *hosts, unsigned block, const char *keyword, unsigned after);
size_t terminal_hosts_value(const struct terminal_hosts *hosts, unsigned line, char *value, size_t size);
size_t terminal_hosts_word(const struct terminal_hosts *hosts, unsigned line, unsigned index, char *word, size_t size);
int terminal_hosts_has_name(const struct terminal_hosts *hosts, unsigned block);

/* Which blocks apply to which host. */
int terminal_hosts_match(const char *pattern, size_t length, const char *name);
int terminal_hosts_applies(const struct terminal_hosts *hosts, unsigned block, const char *name);
unsigned terminal_hosts_overrides(const struct terminal_hosts *hosts, unsigned block, const char *keyword);
unsigned terminal_hosts_other(const struct terminal_hosts *hosts, unsigned block, const char *keyword, unsigned after);

/* The values a change writes. */
int terminal_hosts_put_word(const char *value, char *written, size_t size);
int terminal_hosts_put_names(const char *value, char *written, size_t size);
int terminal_hosts_put_command(const char *value, char *written, size_t size);
int terminal_hosts_put_forward(int remote, const char *listen, const char *destination, char *written, size_t size, const char **why);

/* The changes of the text (each gives a new text, malloc'd, with a NUL after it). */
int terminal_hosts_set(const struct terminal_hosts *hosts, unsigned block, const char *keyword, const char *value, char **out, size_t *out_size);
int terminal_hosts_add(const struct terminal_hosts *hosts, unsigned block, const char *keyword, const char *value, char **out, size_t *out_size);
int terminal_hosts_remove(const struct terminal_hosts *hosts, unsigned line, char **out, size_t *out_size);
int terminal_hosts_add_host(const struct terminal_hosts *hosts, const char *names, char **out, size_t *out_size, unsigned *block);
int terminal_hosts_remove_host(const struct terminal_hosts *hosts, unsigned block, char **out, size_t *out_size, unsigned *removed);

/* Whether a text has a problem its earlier text did not have. */
int terminal_hosts_new_problem(terminal_hosts_checker check, void *data, const char *before, size_t before_size, const char *text, size_t size, char *report, size_t capacity);

#endif
