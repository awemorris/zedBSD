/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland alias interface.
 */

#ifndef ZEDBSD_USERLAND_SH_ALIAS_H
#define ZEDBSD_USERLAND_SH_ALIAS_H

#include "userland/base/sh/lexer.h"

const char *sh_alias_get(const char *);
int sh_alias_set(const char *, const char *);
int sh_alias_unset(const char *);
void sh_alias_clear(void);
void sh_alias_print(void);
int sh_alias_expand(struct sh_token_list *, const char **);

#endif
