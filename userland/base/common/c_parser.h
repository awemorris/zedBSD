/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares shared userland c parser support.
 */

#ifndef ZEDBSD_USERLAND_C_PARSER_H
#define ZEDBSD_USERLAND_C_PARSER_H

#include <stddef.h>

enum c_symbol_kind {
	C_SYMBOL_REFERENCE,
	C_SYMBOL_DECLARATION,
	C_SYMBOL_FUNCTION,
	C_SYMBOL_CALL
};

struct c_symbol_event {
	char *name;
	char *function;
	char *file;
	size_t line;
	enum c_symbol_kind kind;
};

struct c_parse_result {
	struct c_symbol_event *events;
	size_t count;
};

int c_parse_path(const char *path, struct c_parse_result *result);
int c_parse_stream(const char *name, int fd, struct c_parse_result *result);
void c_parse_free(struct c_parse_result *result);

#endif
