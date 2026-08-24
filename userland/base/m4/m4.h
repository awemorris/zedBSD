/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_M4_H
#define ZEDBSD_M4_H

#include <stddef.h>

struct m4_context;

struct m4_context *m4_context_create(void);
void m4_context_destroy(struct m4_context *);
int m4_define(struct m4_context *, const char *, const char *);
int m4_undefine(struct m4_context *, const char *);
int m4_process(struct m4_context *, const char *, const char *, size_t);
int m4_finish(struct m4_context *, int);
const char *m4_error(const struct m4_context *);

#endif
