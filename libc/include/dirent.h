/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DIRENT_H
#define ZEDBSD_DIRENT_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DT_UNKNOWN 0U
#define DT_REG 1U
#define DT_DIR 2U
#define DT_BLK 3U
#define DT_CHR 4U
#define DT_FIFO 5U
#define DT_LNK 6U
#define DT_SOCK 7U
#define DT_MQ 8U
#define DT_SEM 9U
#define DT_SHM 10U
#define DT_TMO 11U

struct dirent {
	ino_t d_ino;
	uint8_t d_type;
	char d_name[256];
};

struct posix_dent {
	ino_t d_ino;
	reclen_t d_reclen;
	unsigned char d_type;
	char d_name[];
};

typedef struct __dir_stream DIR;
DIR *opendir(const char *);
DIR *fdopendir(int);
ssize_t posix_getdents(int, void *, size_t, int);
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
