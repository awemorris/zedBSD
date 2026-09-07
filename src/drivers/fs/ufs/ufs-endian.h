/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UFS_ENDIAN_H
#define ZEDBSD_UFS_ENDIAN_H
#include <stddef.h>
#include <stdint.h>
uint16_t ufs_get16(const void *, size_t, int);
uint32_t ufs_get32(const void *, size_t, int);
uint64_t ufs_get64(const void *, size_t, int);
void ufs_put16(void *, size_t, uint16_t, int);
void ufs_put32(void *, size_t, uint32_t, int);
void ufs_put64(void *, size_t, uint64_t, int);
#endif
