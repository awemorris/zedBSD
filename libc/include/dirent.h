/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_DIRENT_H
#define BOOTS_DIRENT_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DT_UNKNOWN 0U
#define DT_REG 1U
#define DT_DIR 2U
#define DT_CHR 3U
#define DT_BLK 4U

struct dirent {
	ino_t d_ino;
	uint8_t d_type;
	char d_name[256];
};
typedef struct boots_directory DIR;
DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int closedir(DIR *);
#endif
