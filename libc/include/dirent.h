/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DIRENT_H
#define ZEDBSD_DIRENT_H
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
typedef struct __dir_stream DIR;
DIR *opendir(const char *);
DIR *fdopendir(int);
struct dirent *readdir(DIR *);
int readdir_r(DIR *, struct dirent *, struct dirent **);
int alphasort(const struct dirent **, const struct dirent **);
int scandir(const char *, struct dirent ***,
	int (*)(const struct dirent *),
	int (*)(const struct dirent **, const struct dirent **));
int closedir(DIR *);
void rewinddir(DIR *);
void seekdir(DIR *, long);
long telldir(DIR *);
int dirfd(DIR *);
#endif
