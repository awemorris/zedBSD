/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_SH_EXPAND_H
#define ZEDBSD_USERLAND_SH_EXPAND_H

#include "userland/base/sh/lexer.h"

struct sh_expand_context {
	int status;
	long shell_pid;
	long last_job;
	const char *(*lookup)(void *, const char *);
	int (*assign)(void *, const char *, const char *);
	int (*command_substitute)(void *, const char *, char **);
	void *lookup_context;
	const char *shell_name;
	int positional_count;
	char **positional;
};

struct sh_field_list {
	char **fields;
	unsigned char **quoted;
	size_t count;
};

int sh_expand_word(const struct sh_token *, const struct sh_expand_context *,
    char **, const char **);
int sh_expand_fields(const struct sh_token *, const struct sh_expand_context *,
    struct sh_field_list *, const char **);
void sh_fields_free(struct sh_field_list *);

#endif
