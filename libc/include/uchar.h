/* ISO C UTF-16/UTF-32 conversion interfaces. SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UCHAR_H
#define ZEDBSD_UCHAR_H
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#ifndef __cplusplus
typedef __CHAR16_TYPE__ char16_t;
typedef __CHAR32_TYPE__ char32_t;
#endif
size_t mbrtoc16(char16_t *, const char *, size_t, mbstate_t *);
size_t c16rtomb(char *, char16_t, mbstate_t *);
size_t mbrtoc32(char32_t *, const char *, size_t, mbstate_t *);
size_t c32rtomb(char *, char32_t, mbstate_t *);
#endif
