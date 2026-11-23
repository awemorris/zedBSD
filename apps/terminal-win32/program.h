/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Other programs mikuterm runs and reads (program.c): where a program
 * named without a folder is (the folders of PATH), and a program run
 * without a console window whose output is collected, as the launch
 * screen asks GitHub's gh for the codespaces.
 */

#ifndef MIKUTERM_PROGRAM_H
#define MIKUTERM_PROGRAM_H

#include <stddef.h>
#include <wchar.h>

int mikuterm_program_find(const char *name, wchar_t *path, size_t size);
int mikuterm_program_run(const wchar_t *program, const char *arguments, unsigned milliseconds, int (*stop)(void *data), void *data, unsigned char *output, size_t capacity, size_t *size, unsigned long *code);

#endif
