/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_STDDEF_H
#define BOOTS_STDDEF_H

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
