/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares shared userland lzw support.
 */

#ifndef ZEDBSD_USERLAND_LZW_H
#define ZEDBSD_USERLAND_LZW_H

int lzw_compress(int input, int output, unsigned max_bits, int block_mode);
int lzw_decompress(int input, int output);

#endif
