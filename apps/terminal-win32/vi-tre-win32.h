/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * UTF-8 compatibility shim for building TRE regular expressions on Windows.
 *
 * Windows defines wchar_t as 16-bit and uses the active C locale in mbtowc,
 * which treats single bytes as characters in the default "C" locale.
 * Overriding mbtowc and MB_CUR_MAX for TRE lets pattern compilation and
 * matching advance by full UTF-8 sequences without changing TRE itself.
 */

#ifndef APPS_TERMINAL_WIN32_VI_TRE_WIN32_H
#define APPS_TERMINAL_WIN32_VI_TRE_WIN32_H

#include <stddef.h>
#include <stdlib.h>
#include <wchar.h>

int vi_tre_mbtowc(wchar_t *pwc, const char *s, size_t n);

#ifdef VI_TRE_BUILD
#undef NDEBUG
#ifndef CHARCLASS_NAME_MAX
#define CHARCLASS_NAME_MAX 2048
#endif
#ifndef RE_DUP_MAX
#define RE_DUP_MAX 255
#endif
#undef MB_CUR_MAX
#define MB_CUR_MAX 4
#undef mbtowc
#define mbtowc(pwc, s, n) vi_tre_mbtowc((pwc), (s), (n))
#endif

#endif
