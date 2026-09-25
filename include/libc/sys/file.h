/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_FILE_H
#define LIBC_SYS_FILE_H

#include <fcntl.h>
#include <uapi/fcntl.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A whole-file advisory lock.
 *
 * The lock belongs to the open file description rather than the process, so
 * a descriptor inherited through fork or made by dup shares the one lock,
 * and it is released when the last of those descriptors is closed.  That is
 * what separates this call from the record locks fcntl places: two opens of
 * the same file in one process hold two independent locks here, and a lock
 * survives the close of an unrelated descriptor onto the same file.
 *
 * LOCK_SH, LOCK_EX, LOCK_NB and LOCK_UN come from <uapi/fcntl.h>.
 */
int flock(int, int);

#ifdef __cplusplus
}
#endif

#endif
