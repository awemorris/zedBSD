/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland readline interface.
 */

#ifndef KERN_READLINE_READLINE_H
#define KERN_READLINE_READLINE_H

/*
 * A small, source-compatible subset of the GNU Readline interface.  The
 * line being edited, its cursor and its length.
 */
extern char *rl_line_buffer;
extern int rl_point;
extern int rl_end;

/*
 * The editing mode, as GNU Readline names it: 1 for emacs (the default),
 * 0 for vi.  The caller sets it before readline().
 */
extern int rl_editing_mode;

/*
 * Completion, as GNU Readline names it.
 *
 * Tab (in emacs mode and in vi insert mode) finds the word before the
 * cursor: it runs back to a character of rl_completer_word_break_characters
 * that rl_char_is_quoted_p (when set) does not call quoted.  The word's
 * text, its start and its end (the cursor) go to
 * rl_attempted_completion_function, which sees the whole line in
 * rl_line_buffer.
 *
 * The function returns NULL for no match, or an array it allocated with
 * malloc, ended by NULL: [0] replaces the word (the one match, or what all
 * the matches have in common), [1] and after are the matches as a list of
 * them shows them (none when there is one match).  The editor frees the
 * array and its strings.  One match is followed by
 * rl_completion_append_character (unless it is '\0'), which the editor sets
 * back to ' ' before each call.  A Tab that does not change the line right
 * after another Tab lists the matches, asking first when there are more
 * than rl_completion_query_items of them.  Without a function, Tab does
 * nothing.
 */
typedef char **rl_completion_func_t(const char *text, int start, int end);
typedef int rl_linebuf_func_t(char *line, int index);
extern rl_completion_func_t *rl_attempted_completion_function;
extern char *rl_completer_word_break_characters;
extern rl_linebuf_func_t *rl_char_is_quoted_p;
extern int rl_completion_append_character;
extern int rl_completion_query_items;

/* Reads a line with editing; NULL at the end of the input. */
char *readline(const char *prompt);

#endif
