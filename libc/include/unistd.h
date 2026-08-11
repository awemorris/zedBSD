/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UNISTD_H
#define ZEDBSD_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define F_OK 0

int access(const char *path, int mode);
char *getcwd(char *buffer, size_t size);
int chdir(const char *path);
int isatty(int descriptor);
int fileno(void *stream);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
int close(int);
off_t lseek(int, off_t, int);
void _exit(int) __attribute__((noreturn));

#endif
