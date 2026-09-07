/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fixed UFS data-image formatter interface.
 */

#ifndef ZEDBSD_MKFS_UFS_FORMAT_H
#define ZEDBSD_MKFS_UFS_FORMAT_H

#include <stdint.h>

#define UFS_FORMAT_MIN_BYTES UINT64_C(4194304)
#define UFS_FORMAT_MAX_BYTES UINT64_C(2147482624)

int ufs_format_validate_size(uint64_t bytes);
int ufs_format_write(int fd, uint64_t bytes);
int ufs_format_verify(int fd, uint64_t bytes);

int ufs_format_feature_write(int fd, uint64_t bytes);
int ufs_format_feature_verify(int fd, uint64_t bytes);

#endif
