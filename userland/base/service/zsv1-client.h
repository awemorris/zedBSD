/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_ZSV1_CLIENT_H
#define ZEDBSD_ZSV1_CLIENT_H

#include "userland/base/service/zsv1-protocol.h"

#include <time.h>

#define ZSV1_INIT_SOCKET "/run/init.sock"
#define ZSV1_CLIENT_TIMEOUT_SECONDS 310U

/* The caller retains ownership of descriptor.  The write side is closed. */
int zsv1_client_exchange_fd(int, const struct zsv1_request *,
			    struct zsv1_response *, const struct timespec *);
int zsv1_client_call(const char *, const struct zsv1_request *,
		     struct zsv1_response *);

#endif
