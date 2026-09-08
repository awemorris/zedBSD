/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UFS_SUPER_H
#define ZEDBSD_UFS_SUPER_H
#include "ufs-disk.h"
int ufs_super_decode(const void *, size_t, uint64_t, struct ufs_super *);
#endif
