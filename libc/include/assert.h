/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_ASSERT_H
#define ZEDBSD_ASSERT_H

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
void __libc_assert_fail(const char *expression, const char *file, int line);
#define assert(expression) \
	((expression) ? (void)0 : __libc_assert_fail(#expression, __FILE__, __LINE__))
#endif

#endif
