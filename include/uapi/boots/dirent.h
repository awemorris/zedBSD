/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_UAPI_DIRENT_H
#define BOOTS_UAPI_DIRENT_H

#include <stdint.h>

#define BOOTS_DT_UNKNOWN 0U
#define BOOTS_DT_REG 1U
#define BOOTS_DT_DIR 2U
#define BOOTS_DT_BLK 3U
#define BOOTS_DT_CHR 4U

struct boots_dirent {
	uint64_t d_ino;
	uint32_t d_type;
	char d_name[256];
};

#endif
