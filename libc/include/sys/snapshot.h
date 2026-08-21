/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_SNAPSHOT_H
#define ZEDBSD_SYS_SNAPSHOT_H
#include <zedbsd/snapshot.h>
int snapshotctl(const char *,struct snapshot_control *);
#endif
