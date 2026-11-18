/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_NL_TYPES_H
#define LIBC_NL_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct __nl_catalog;
typedef struct __nl_catalog *nl_catd;

/* An item nl_langinfo() is asked for (<langinfo.h> takes it from here). */
typedef int nl_item;

#define NL_SETD 1
#define NL_CAT_LOCALE 1

nl_catd catopen(const char *, int);
char *catgets(nl_catd, int, int, const char *);
int catclose(nl_catd);

#ifdef __cplusplus
}
#endif

#endif
