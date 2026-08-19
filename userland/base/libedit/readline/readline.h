/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_READLINE_READLINE_H
#define ZEDBSD_READLINE_READLINE_H

/* Small, source-compatible subset of the GNU Readline interface. */
extern char *rl_line_buffer;
extern int rl_point;
extern int rl_end;

char *readline(const char *prompt);

#endif
