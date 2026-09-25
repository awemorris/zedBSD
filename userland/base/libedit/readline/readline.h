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

/* Reads a line with editing; NULL at the end of the input. */
char *readline(const char *prompt);

#endif
