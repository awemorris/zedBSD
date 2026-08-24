/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_NL_TYPES_H
#define ZEDBSD_NL_TYPES_H

#include <stdint.h>

struct __nl_catalog;
typedef struct __nl_catalog *nl_catd;

#define NL_SETD 1
#define NL_CAT_LOCALE 1

nl_catd catopen(const char *, int);
char *catgets(nl_catd, int, int, const char *);
int catclose(nl_catd);

#endif
