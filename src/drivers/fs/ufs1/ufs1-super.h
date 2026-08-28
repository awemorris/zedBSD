/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UFS1_SUPER_H
#define ZEDBSD_UFS1_SUPER_H
#include "ufs1-disk.h"
int ufs1_super_decode(const void *, size_t, uint64_t, struct ufs1_super *);
#endif
