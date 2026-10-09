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

/* The classifications in a locale: single bytes are classified the same in every locale this C library has. */
int isalnum_l(int, locale_t);
int isalpha_l(int, locale_t);
int isblank_l(int, locale_t);
int iscntrl_l(int, locale_t);
int isdigit_l(int, locale_t);
int isgraph_l(int, locale_t);
int islower_l(int, locale_t);
int isprint_l(int, locale_t);
int ispunct_l(int, locale_t);
int isspace_l(int, locale_t);
int isupper_l(int, locale_t);
int isxdigit_l(int, locale_t);

#ifdef __cplusplus
}
#endif

#endif
