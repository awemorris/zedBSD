/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_ASSERT_H
#define ZEDBSD_ASSERT_H

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
void zedbsd_assert_fail(const char *expression, const char *file, int line);
#define assert(expression) \
	((expression) ? (void)0 : zedbsd_assert_fail(#expression, __FILE__, __LINE__))
#endif

#endif
