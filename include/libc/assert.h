/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_ASSERT_H
#define LIBC_ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
void __libc_assert_fail(const char *expression, const char *file, int line);
#define assert(expression) \
	((expression) ? (void)0 : __libc_assert_fail(#expression, __FILE__, __LINE__))
#endif

#ifdef __cplusplus
}
#endif

/*
 * C11 and C17 name the static assertion static_assert in this header
 * (C23 makes it a keyword, C++ has its own).
 */
#if !defined(__cplusplus) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && __STDC_VERSION__ < 202311L
#ifndef static_assert
#define static_assert _Static_assert
#endif
#endif

#endif
