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


/*
 * The least-width and fastest types.  zedBSD's compilers make each of them
 * the exact-width type of the same width (int_least16_t and int_fast16_t are
 * short, as int16_t is), so their modifiers are the exact-width ones.
 */
#define PRIdLEAST8 PRId8
#define PRIdFAST8 PRId8
#define PRIiLEAST8 PRIi8
#define PRIiFAST8 PRIi8
#define PRIoLEAST8 PRIo8
#define PRIoFAST8 PRIo8
#define PRIuLEAST8 PRIu8
#define PRIuFAST8 PRIu8
#define PRIxLEAST8 PRIx8
#define PRIxFAST8 PRIx8
#define PRIXLEAST8 PRIX8
#define PRIXFAST8 PRIX8
#define PRIdLEAST16 PRId16
#define PRIdFAST16 PRId16
#define PRIiLEAST16 PRIi16
#define PRIiFAST16 PRIi16
#define PRIoLEAST16 PRIo16
#define PRIoFAST16 PRIo16
#define PRIuLEAST16 PRIu16
#define PRIuFAST16 PRIu16
#define PRIxLEAST16 PRIx16
#define PRIxFAST16 PRIx16
#define PRIXLEAST16 PRIX16
#define PRIXFAST16 PRIX16
#define PRIdLEAST32 PRId32
#define PRIdFAST32 PRId32
#define PRIiLEAST32 PRIi32
#define PRIiFAST32 PRIi32
#define PRIoLEAST32 PRIo32
#define PRIoFAST32 PRIo32
#define PRIuLEAST32 PRIu32
#define PRIuFAST32 PRIu32
#define PRIxLEAST32 PRIx32
#define PRIxFAST32 PRIx32
#define PRIXLEAST32 PRIX32
#define PRIXFAST32 PRIX32
#define PRIdLEAST64 PRId64
#define PRIdFAST64 PRId64
#define PRIiLEAST64 PRIi64
#define PRIiFAST64 PRIi64
#define PRIoLEAST64 PRIo64
#define PRIoFAST64 PRIo64
#define PRIuLEAST64 PRIu64
#define PRIuFAST64 PRIu64
#define PRIxLEAST64 PRIx64
#define PRIxFAST64 PRIx64
#define PRIXLEAST64 PRIX64
#define PRIXFAST64 PRIX64
#define SCNdLEAST8 SCNd8
#define SCNdFAST8 SCNd8
#define SCNiLEAST8 SCNi8
#define SCNiFAST8 SCNi8
#define SCNoLEAST8 SCNo8
#define SCNoFAST8 SCNo8
#define SCNuLEAST8 SCNu8
#define SCNuFAST8 SCNu8
#define SCNxLEAST8 SCNx8
#define SCNxFAST8 SCNx8
#define SCNdLEAST16 SCNd16
#define SCNdFAST16 SCNd16
#define SCNiLEAST16 SCNi16
#define SCNiFAST16 SCNi16
#define SCNoLEAST16 SCNo16
#define SCNoFAST16 SCNo16
#define SCNuLEAST16 SCNu16
#define SCNuFAST16 SCNu16
#define SCNxLEAST16 SCNx16
#define SCNxFAST16 SCNx16
#define SCNdLEAST32 SCNd32
#define SCNdFAST32 SCNd32
#define SCNiLEAST32 SCNi32
#define SCNiFAST32 SCNi32
#define SCNoLEAST32 SCNo32
#define SCNoFAST32 SCNo32
#define SCNuLEAST32 SCNu32
#define SCNuFAST32 SCNu32
#define SCNxLEAST32 SCNx32
#define SCNxFAST32 SCNx32
#define SCNdLEAST64 SCNd64
#define SCNdFAST64 SCNd64
#define SCNiLEAST64 SCNi64
#define SCNiFAST64 SCNi64
#define SCNoLEAST64 SCNo64
#define SCNoFAST64 SCNo64
#define SCNuLEAST64 SCNu64
#define SCNuFAST64 SCNu64
#define SCNxLEAST64 SCNx64
#define SCNxFAST64 SCNx64

#ifdef __cplusplus
}
#endif

#endif
