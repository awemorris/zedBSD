/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_CTYPE_H
#define KERN_CTYPE_H

#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

int isalnum(int character);
int isalpha(int character);
int isascii(int character);
int isblank(int character);
int iscntrl(int character);
int isdigit(int character);
int isgraph(int character);
int islower(int character);
int isprint(int character);
int ispunct(int character);
int isspace(int character);
int isupper(int character);
int isxdigit(int character);
int tolower(int character);
int toupper(int character);
int _tolower(int character);
int _toupper(int character);
int toascii(int character);


/* The locale-aware forms POSIX.1-2008 added. */
int toupper_l(int, locale_t);
int tolower_l(int, locale_t);

#ifdef __cplusplus
}
#endif

#endif
