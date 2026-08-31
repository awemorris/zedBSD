/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares shared userland uucodec support.
 */

#ifndef ZEDBSD_USERLAND_UUCODEC_H
#define ZEDBSD_USERLAND_UUCODEC_H

int uu_encode_fd(int input_fd, int base64, unsigned mode,
		 const char *decode_path);
int uu_decode_fd(int input_fd, const char *output_override);

#endif
