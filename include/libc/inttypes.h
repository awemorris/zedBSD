/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_INTTYPES_H
#define LIBC_INTTYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <wchar.h>

typedef struct { intmax_t quot, rem; } imaxdiv_t;

intmax_t imaxabs(intmax_t);
imaxdiv_t imaxdiv(intmax_t, intmax_t);
intmax_t strtoimax(const char *, char **, int);
uintmax_t strtoumax(const char *, char **, int);
intmax_t wcstoimax(const wchar_t *, wchar_t **, int);
uintmax_t wcstoumax(const wchar_t *, wchar_t **, int);

/*
 * The length modifiers of the 64-bit and pointer-sized integers.  They follow
 * the types <stdint.h> takes from the compiler: on an LP64 target int64_t,
 * intmax_t and intptr_t are long, so the modifier is "l"; on an ILP32 target
 * int64_t and intmax_t are long long ("ll") and intptr_t is int (none).  A
 * modifier of the right size but the wrong type still prints correctly, but
 * the compiler's format check reports it.
 */
#ifdef __LP64__
#define __PRI_64 "l"
#define __PRI_PTR "l"
#else
#define __PRI_64 "ll"
#define __PRI_PTR ""
#endif

#define PRId8 "d"
#define PRIi8 "i"
#define PRIo8 "o"
#define PRIu8 "u"
#define PRIx8 "x"
#define PRIX8 "X"
#define PRId16 "d"
#define PRIi16 "i"
#define PRIo16 "o"
#define PRIu16 "u"
#define PRIx16 "x"
#define PRIX16 "X"
#define PRId32 "d"
#define PRIi32 "i"
#define PRIo32 "o"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 __PRI_64 "d"
#define PRIi64 __PRI_64 "i"
#define PRIo64 __PRI_64 "o"
#define PRIu64 __PRI_64 "u"
#define PRIx64 __PRI_64 "x"
#define PRIX64 __PRI_64 "X"
#define PRIdPTR __PRI_PTR "d"
#define PRIiPTR __PRI_PTR "i"
#define PRIoPTR __PRI_PTR "o"
#define PRIuPTR __PRI_PTR "u"
#define PRIxPTR __PRI_PTR "x"
#define PRIXPTR __PRI_PTR "X"
#define PRIdMAX __PRI_64 "d"
#define PRIiMAX __PRI_64 "i"
#define PRIoMAX __PRI_64 "o"
#define PRIuMAX __PRI_64 "u"
#define PRIxMAX __PRI_64 "x"
#define PRIXMAX __PRI_64 "X"

#define SCNd8 "hhd"
#define SCNi8 "hhi"
#define SCNo8 "hho"
#define SCNu8 "hhu"
#define SCNx8 "hhx"
#define SCNd16 "hd"
#define SCNi16 "hi"
#define SCNo16 "ho"
#define SCNu16 "hu"
#define SCNx16 "hx"
#define SCNd32 "d"
#define SCNi32 "i"
#define SCNo32 "o"
#define SCNu32 "u"
#define SCNx32 "x"
#define SCNd64 __PRI_64 "d"
#define SCNi64 __PRI_64 "i"
#define SCNo64 __PRI_64 "o"
#define SCNu64 __PRI_64 "u"
#define SCNx64 __PRI_64 "x"
#define SCNdPTR __PRI_PTR "d"
#define SCNiPTR __PRI_PTR "i"
#define SCNoPTR __PRI_PTR "o"
#define SCNuPTR __PRI_PTR "u"
#define SCNxPTR __PRI_PTR "x"
#define SCNdMAX __PRI_64 "d"
#define SCNiMAX __PRI_64 "i"
#define SCNoMAX __PRI_64 "o"
#define SCNuMAX __PRI_64 "u"
#define SCNxMAX __PRI_64 "x"

#ifdef __cplusplus
}
#endif

#endif
