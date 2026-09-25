/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shell arithmetic (POSIX XCU 2.6.4): signed integers of at least 64 bits,
 * the operators of C without ++, --, the comma and the address operators,
 * and assignment to shell variables.
 */

#ifndef KERN_USERLAND_SH_ARITHMETIC_H
#define KERN_USERLAND_SH_ARITHMETIC_H

/*
 * Evaluates an expression.  lookup reads a variable (NULL when unset), assign
 * sets one (nonzero on success).  On failure *error_text names the fault.
 */
int sh_arithmetic_eval(const char *text,
		       const char *(*lookup)(void *, const char *),
		       int (*assign)(void *, const char *, const char *),
		       void *context, long long *result,
		       const char **error_text);

#endif
