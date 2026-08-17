/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_LANGINFO_H
#define ZEDBSD_LANGINFO_H
typedef int nl_item;
#define CODESET 1
#define RADIXCHAR 2
#define THOUSEP 3
#define CRNCYSTR 4
char *nl_langinfo(nl_item);
#endif
