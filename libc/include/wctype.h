/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_WCTYPE_H
#define ZEDBSD_WCTYPE_H
#include <wchar.h>
#ifdef ZEDBSD_REGEX_HOST_TEST
typedef unsigned long wctype_t;
typedef const int *wctrans_t;
#else
typedef uint32_t wctype_t;
typedef uint32_t wctrans_t;
#endif
int iswalnum(wint_t);
int iswalpha(wint_t);
int iswblank(wint_t);
int iswcntrl(wint_t);
int iswdigit(wint_t);
int iswgraph(wint_t);
int iswlower(wint_t);
int iswprint(wint_t);
int iswpunct(wint_t);
int iswspace(wint_t);
int iswupper(wint_t);
int iswxdigit(wint_t);
wint_t towlower(wint_t);
wint_t towupper(wint_t);
wctype_t wctype(const char *);
int iswctype(wint_t, wctype_t);
wctrans_t wctrans(const char *);
wint_t towctrans(wint_t, wctrans_t);
#endif
