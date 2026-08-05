/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_ASSERT_H
#define BOOTS_ASSERT_H

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
void boots_assert_fail(const char *expression, const char *file, int line);
#define assert(expression) \
	((expression) ? (void)0 : boots_assert_fail(#expression, __FILE__, __LINE__))
#endif

#endif
