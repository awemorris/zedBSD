/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_COMMON_COMMAND_H
#define ZEDBSD_USERLAND_COMMON_COMMAND_H

#include <stddef.h>
#include <stdio.h>

int command_write_all(int descriptor, const void *data, size_t length);
int command_copy_fd(int input, int output);
int command_parse_ull(const char *text, unsigned long long *result);
int command_parse_mode(const char *text, unsigned *result);
void command_error(const char *command, const char *operand);
long command_read_line(FILE *stream, char **line, size_t *capacity);
int command_exec(const char *name, char *const argv[]);

#endif
