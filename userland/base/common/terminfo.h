/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares shared userland terminfo support.
 */

#ifndef ZEDBSD_USERLAND_COMMON_TERMINFO_H
#define ZEDBSD_USERLAND_COMMON_TERMINFO_H

#include <stddef.h>
#include <stdio.h>

#define TERMINFO_CAPACITY 128U
#define TERMINFO_NAME_LENGTH 32U
#define TERMINFO_VALUE_LENGTH 256U

enum terminfo_kind {
	TERMINFO_BOOLEAN,
	TERMINFO_NUMBER,
	TERMINFO_STRING,
};

struct terminfo_capability {
	char name[TERMINFO_NAME_LENGTH];
	enum terminfo_kind kind;
	long number;
	char string[TERMINFO_VALUE_LENGTH];
};

struct terminfo {
	char name[TERMINFO_VALUE_LENGTH];
	struct terminfo_capability capabilities[TERMINFO_CAPACITY];
	size_t count;
};

int terminfo_load(struct terminfo *, const char *, const char *);
const struct terminfo_capability *terminfo_find(const struct terminfo *,
						const char *);
int terminfo_expand(const char *, const long[9], char *, size_t);
int terminfo_write_source(FILE *, const struct terminfo *, const char *);

#endif
