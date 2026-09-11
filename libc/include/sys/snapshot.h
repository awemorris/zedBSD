/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_SNAPSHOT_H
#define LIBC_SYS_SNAPSHOT_H

#include <uapi/snapshot.h>

int snapshotctl(const char *,struct snapshot_control *);

#endif
