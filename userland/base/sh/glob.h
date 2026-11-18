/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland glob interface.
 */

#ifndef KERN_USERLAND_SH_GLOB_H
#define KERN_USERLAND_SH_GLOB_H

#include "userland/base/sh/expand.h"

int sh_glob_fields(struct sh_field_list *, const char **);

/*
 * Matches a whole string against a pattern, for the arms of a case command.
 * The mask says which characters of the pattern were quoted and so stand
 * for themselves; a null mask means none of them were.
 */
int sh_glob_match(const char *, const unsigned char *, const char *);

#endif
