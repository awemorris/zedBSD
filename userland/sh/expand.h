/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_SH_EXPAND_H
#define ZEDBSD_USERLAND_SH_EXPAND_H

#include "userland/sh/lexer.h"

struct sh_expand_context {
	int status;
	long shell_pid;
	long last_job;
};

int sh_expand_word(const struct sh_token *, const struct sh_expand_context *,
    char **, const char **);

#endif
