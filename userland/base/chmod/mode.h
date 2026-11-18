/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the mode operand of chmod, which mkdir -m and mkfifo -m share.
 */

#ifndef USERLAND_BASE_CHMOD_MODE_H
#define USERLAND_BASE_CHMOD_MODE_H

#include <sys/types.h>

int mode_valid(const char *text);
int mode_apply(const char *text, mode_t original, mode_t mask, int directory, mode_t *result);

#endif
