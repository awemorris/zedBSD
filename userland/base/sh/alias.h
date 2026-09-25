/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Aliases (POSIX XCU 2.3.1): the table the alias and unalias builtins keep,
 * and the parser reads when a word stands where a command name may.
 */

#ifndef KERN_USERLAND_SH_ALIAS_H
#define KERN_USERLAND_SH_ALIAS_H

/* Returns an alias's text, or NULL. */
const char *sh_alias_get(const char *);

/* Defines an alias.  Returns -1 for a name an alias may not have. */
int sh_alias_set(const char *, const char *);

/* Removes an alias.  Returns -1 when there was none. */
int sh_alias_unset(const char *);

/* Removes every alias. */
void sh_alias_clear(void);

/* Prints one alias, or every alias with NULL, as name='text'. */
void sh_alias_print(const char *);

#endif
