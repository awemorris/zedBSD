/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland zsv1 server interface.
 */

#ifndef ZEDBSD_ZSV1_SERVER_H
#define ZEDBSD_ZSV1_SERVER_H

#include "userland/base/service/zsv1-protocol.h"

#include <time.h>

int zsv1_server_receive_fd(int, struct zsv1_request *, const struct timespec *);
int zsv1_server_send_record_fd(int, const struct zsv1_record *);
int zsv1_server_send_end_fd(int);
int zsv1_server_send_ok_end_fd(int, const char *);
int zsv1_server_send_error_end_fd(int, int, const char *);
int zsv1_server_dependency_lists_validate(const char *, const char *, size_t *,
					  size_t *);

#endif
