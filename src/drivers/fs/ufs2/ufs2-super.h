/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UFS2_SUPER_H
#define ZEDBSD_UFS2_SUPER_H
#include "ufs2-disk.h"
int ufs2_super_decode(const void *, size_t, uint64_t, struct ufs2_super *);
#endif
