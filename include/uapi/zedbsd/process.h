/*
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_PROCESS_H
#define ZEDBSD_UAPI_PROCESS_H

#include <stddef.h>
#include <sys/types.h>

#define ZEDBSD_SPAWN_RESULT 0x00000001U
#define ZEDBSD_SPAWN_ARG_MAX 32U
#define ZEDBSD_SPAWN_ENV_MAX 64U
#define ZEDBSD_SPAWN_STRING_MAX (16U * 1024U)

pid_t zedbsd_spawn(const char *, char *const [], char *const [], unsigned);
pid_t zedbsd_wait_result(pid_t, int *, char *, size_t);

#endif
