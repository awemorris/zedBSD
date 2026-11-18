/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * sandbox_spawn (ws168-p002): starts a child that runs a static image in
 * a sandbox from its first instruction (<uapi/sandbox.h> says what the
 * request holds and what the child may do).
 */

#ifndef LIBC_SANDBOX_H
#define LIBC_SANDBOX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <uapi/sandbox.h>

/*
 * Starts the child a request describes.  Returns its process ID, or -1
 * with errno: EINVAL (a malformed request, an unknown flag or allow bit,
 * a file's number out of range or used twice), E2BIG (a request larger
 * than this kernel knows with bytes it does not know set), EBADF (the
 * image or a file not open), ENOEXEC (not a static image), EACCES (not
 * executable by the caller), ENOMEM, or EPERM (called from a sandbox).
 */
pid_t sandbox_spawn(const struct sandbox_spawn *request);

#ifdef __cplusplus
}
#endif

#endif
