/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_WAIT_H
#define ZEDBSD_SYS_WAIT_H

#include <sys/types.h>

#define WIFEXITED(status) 1
#define WEXITSTATUS(status) ((status) & 0xff)

pid_t waitpid(pid_t, int *, int);

#endif
