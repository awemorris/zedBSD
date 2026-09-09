/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_USER_SWAP_FORMAT_H
#define ZEDBSD_USER_SWAP_FORMAT_H

#include <stdint.h>

int swap_format_validate_size(uint64_t);
int swap_format_write(int, uint64_t);
int swap_format_verify(int, uint64_t);
int swap_format_pristine(int, uint64_t);

#endif
