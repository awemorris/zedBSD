/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tracing another process.
 */

#ifndef LIBC_SYS_PTRACE_H
#define LIBC_SYS_PTRACE_H

#include <sys/types.h>
#include <uapi/ptrace.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Makes one tracing request.  What addr and data mean depends on the
 * request; a request that reports a word returns it, and -1 with errno
 * set is how failure is told apart from a word whose value is -1 by
 * clearing errno before the call.
 */
int ptrace(int request, pid_t pid, void *addr, int data);

#ifdef __cplusplus
}
#endif

#endif
