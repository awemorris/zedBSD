/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Other programs mikuterm runs and reads (program.c on Windows,
 * apps/terminal-linux/program-posix.c on the POSIX systems): where a
 * program named without a folder is (the folders of PATH), and a program
 * run without a console window whose output is collected, as the launch
 * screen asks GitHub's gh for the codespaces.
 */

#ifndef MIKUTERM_PROGRAM_H
#define MIKUTERM_PROGRAM_H

#include <stddef.h>
#include <wchar.h>

/*
 * A unit of a program's path as the system takes it: Windows' UTF-16, a
 * POSIX system's bytes (UTF-8).
 */
#ifdef _WIN32
typedef wchar_t mikuterm_program_char;
#else
typedef char mikuterm_program_char;
#endif

int mikuterm_program_find(const char *name, mikuterm_program_char *path, size_t size);
int mikuterm_program_run(const mikuterm_program_char *program, const char *arguments, unsigned milliseconds, int (*stop)(void *data), void *data, unsigned char *output, size_t capacity, size_t *size, unsigned long *code);

#endif
