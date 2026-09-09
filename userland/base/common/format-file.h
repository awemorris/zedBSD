/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_USER_FORMAT_FILE_H
#define ZEDBSD_USER_FORMAT_FILE_H

#include <stdint.h>

struct format_file_ops {
	int (*validate_size)(uint64_t);
	int (*write)(int, uint64_t);
	int (*verify)(int, uint64_t);
};

int format_file_run(const char *, const struct format_file_ops *, uint64_t *);
int format_file_verify(const char *, int (*)(uint64_t), int (*)(int, uint64_t), uint64_t *);

#endif
