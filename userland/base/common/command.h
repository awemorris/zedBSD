/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_COMMON_COMMAND_H
#define ZEDBSD_USERLAND_COMMON_COMMAND_H

#include <stddef.h>
#include <stdio.h>

int command_write_all(int, const void *, size_t);
int command_copy_fd(int, int);
int command_parse_ull(const char *, unsigned long long *);
int command_parse_mode(const char *, unsigned *);
void command_error(const char *, const char *);
long command_read_line(FILE *, char **, size_t *);
int command_exec(const char *, char *const[]);

#endif
