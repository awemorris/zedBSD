/*
 * zedBSD wide-character utility and stream support
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "libc/stdio-internal.h"
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

size_t
wcslen(const wchar_t *string)
{
	const wchar_t *end = string;
	while (*end != 0)
		end++;
	return (size_t)(end - string);
}

int
wcscmp(const wchar_t *left, const wchar_t *right)
{
	while (*left != 0 && *left == *right) {
		left++;
		right++;
	}
	return *left < *right ? -1 : *left > *right;
}

int
wcsncmp(const wchar_t *left, const wchar_t *right, size_t count)
{
	while (count != 0 && *left != 0 && *left == *right) {
		left++;
		right++;
		count--;
	}
	return count == 0 ? 0 : *left < *right ? -1 : *left > *right;
}

wchar_t *
wcscpy(wchar_t *destination, const wchar_t *source)
{
	wchar_t *result = destination;
	while ((*destination++ = *source++) != 0)
		;
	return result;
}

wchar_t *
wcsncpy(wchar_t *destination, const wchar_t *source, size_t count)
{
	wchar_t *result = destination;
	while (count != 0 && *source != 0) {
		*destination++ = *source++;
		count--;
	}
	while (count-- != 0)
		*destination++ = 0;
	return result;
}

wchar_t *
wcschr(const wchar_t *string, wchar_t character)
{
	for (;;) {
		if (*string == character)
			return (wchar_t *)string;
		if (*string++ == 0)
			return NULL;
	}
}

wchar_t *
wcsrchr(const wchar_t *string, wchar_t character)
{
	const wchar_t *last = NULL;
	for (;;) {
		if (*string == character)
			last = string;
		if (*string++ == 0)
			return (wchar_t *)last;
	}
}

wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n)
{ return memcpy(d, s, n * sizeof(*d)); }
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n)
{ return memmove(d, s, n * sizeof(*d)); }

wchar_t *
wmemset(wchar_t *destination, wchar_t character, size_t count)
{
	wchar_t *result = destination;
	while (count-- != 0)
		*destination++ = character;
	return result;
}

int
wmemcmp(const wchar_t *left, const wchar_t *right, size_t count)
{
	while (count-- != 0) {
		if (*left != *right)
			return *left < *right ? -1 : 1;
		left++;
		right++;
	}
	return 0;
}

int wcscoll(const wchar_t *left, const wchar_t *right)
{ return wcscmp(left, right); }

size_t
wcsxfrm(wchar_t *destination, const wchar_t *source, size_t count)
{
	size_t length = wcslen(source);
	if (destination != NULL && count != 0) {
		size_t copied = length < count - 1U ? length : count - 1U;
		wmemcpy(destination, source, copied);
		destination[copied] = 0;
	}
	return length;
}

static int
unicode_scalar(wint_t value)
{
	return value <= 0x10ffffU &&
	    !(value >= 0xd800U && value <= 0xdfffU);
}

static int
combining(wint_t value)
{
	return (value >= 0x0300U && value <= 0x036fU) ||
	    (value >= 0x1ab0U && value <= 0x1affU) ||
	    (value >= 0x1dc0U && value <= 0x1dffU) ||
	    (value >= 0x20d0U && value <= 0x20ffU) ||
	    (value >= 0xfe00U && value <= 0xfe0fU) ||
	    (value >= 0xfe20U && value <= 0xfe2fU);
}

int
wcwidth(wchar_t character)
{
	wint_t value = (wint_t)character;
	if (value == 0)
		return 0;
	if (!unicode_scalar(value) || value < 0x20U ||
	    (value >= 0x7fU && value < 0xa0U))
		return -1;
	if (combining(value))
		return 0;
	if ((value >= 0x1100U && value <= 0x115fU) ||
	    (value >= 0x2e80U && value <= 0xa4cfU) ||
	    (value >= 0xac00U && value <= 0xd7a3U) ||
	    (value >= 0xf900U && value <= 0xfaffU) ||
	    (value >= 0xfe10U && value <= 0xfe6fU) ||
	    (value >= 0xff01U && value <= 0xff60U) ||
	    (value >= 0xffe0U && value <= 0xffe6U) ||
	    (value >= 0x1f300U && value <= 0x1faffU) ||
	    (value >= 0x20000U && value <= 0x3fffdU))
		return 2;
	return 1;
}

int
wcswidth(const wchar_t *string, size_t count)
{
	int total = 0;
	while (count-- != 0 && *string != 0) {
		int width = wcwidth(*string++);
		if (width < 0)
			return -1;
		total += width;
	}
	return total;
}

enum {
	WC_ALPHA = 1, WC_DIGIT, WC_SPACE, WC_UPPER, WC_LOWER, WC_XDIGIT,
	WC_ALNUM, WC_BLANK, WC_CNTRL, WC_GRAPH, WC_PRINT, WC_PUNCT
};

int iswcntrl(wint_t c)
{ return c < 0x20U || (c >= 0x7fU && c < 0xa0U); }
int iswspace(wint_t c)
{ return c == ' ' || (c >= '\t' && c <= '\r') ||
    (MB_CUR_MAX > 1 && (c == 0x85U || c == 0xa0U || c == 0x1680U ||
    (c >= 0x2000U && c <= 0x200aU) || c == 0x2028U || c == 0x2029U ||
    c == 0x202fU || c == 0x205fU || c == 0x3000U)); }
int iswdigit(wint_t c) { return c >= '0' && c <= '9'; }
int iswupper(wint_t c)
{ return (c >= 'A' && c <= 'Z') ||
    (MB_CUR_MAX > 1 && c >= 0xc0U && c <= 0xdeU && c != 0xd7U); }
int iswlower(wint_t c)
{ return (c >= 'a' && c <= 'z') ||
    (MB_CUR_MAX > 1 && c >= 0xdfU && c <= 0xffU && c != 0xf7U); }
int iswalpha(wint_t c)
{ return iswupper(c) || iswlower(c) || (MB_CUR_MAX > 1 &&
    unicode_scalar(c) && c >= 0x100U && !iswspace(c) && !iswcntrl(c) &&
    !combining(c)); }
int iswalnum(wint_t c) { return iswalpha(c) || iswdigit(c); }
int iswblank(wint_t c)
{ return c == ' ' || c == '\t' ||
    (MB_CUR_MAX > 1 && (c == 0xa0U || c == 0x3000U)); }
int iswprint(wint_t c) { return unicode_scalar(c) && !iswcntrl(c); }
int iswgraph(wint_t c) { return iswprint(c) && !iswspace(c); }
int iswxdigit(wint_t c)
{ return iswdigit(c) || (c >= 'a' && c <= 'f') ||
    (c >= 'A' && c <= 'F'); }
int iswpunct(wint_t c) { return iswgraph(c) && !iswalnum(c); }

wint_t
towlower(wint_t c)
{
	if (c >= 'A' && c <= 'Z')
		return c + 32U;
	if (c >= 0xc0U && c <= 0xdeU && c != 0xd7U)
		return c + 32U;
	return c;
}

wint_t
towupper(wint_t c)
{
	if (c >= 'a' && c <= 'z')
		return c - 32U;
	if (c >= 0xe0U && c <= 0xfeU && c != 0xf7U)
		return c - 32U;
	return c;
}

wctype_t
wctype(const char *name)
{
	static const char *names[] = {
		"", "alpha", "digit", "space", "upper", "lower", "xdigit",
		"alnum", "blank", "cntrl", "graph", "print", "punct"
	};
	unsigned i;
	if (name == NULL)
		return 0;
	for (i = 1; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(name, names[i]))
			return i;
	return 0;
}

int
iswctype(wint_t c, wctype_t type)
{
	switch (type) {
	case WC_ALPHA: return iswalpha(c);
	case WC_DIGIT: return iswdigit(c);
	case WC_SPACE: return iswspace(c);
	case WC_UPPER: return iswupper(c);
	case WC_LOWER: return iswlower(c);
	case WC_XDIGIT: return iswxdigit(c);
	case WC_ALNUM: return iswalnum(c);
	case WC_BLANK: return iswblank(c);
	case WC_CNTRL: return iswcntrl(c);
	case WC_GRAPH: return iswgraph(c);
	case WC_PRINT: return iswprint(c);
	case WC_PUNCT: return iswpunct(c);
	default: return 0;
	}
}

wctrans_t
wctrans(const char *name)
{
	if (name != NULL && !strcmp(name, "tolower"))
		return 1;
	if (name != NULL && !strcmp(name, "toupper"))
		return 2;
	return 0;
}

wint_t
towctrans(wint_t c, wctrans_t transform)
{
	return transform == 1 ? towlower(c) :
	    transform == 2 ? towupper(c) : c;
}

int
fwide(FILE *stream, int mode)
{
	int result;
	if (stream == NULL)
		return 0;
	flockfile(stream);
	if (stream->orientation == 0 && mode != 0)
		stream->orientation = mode > 0 ? 1 : -1;
	result = stream->orientation;
	funlockfile(stream);
	return result;
}

wint_t
fputwc(wchar_t character, FILE *stream)
{
	char bytes[4];
	size_t count;
	if (stream == NULL || fwide(stream, 1) < 0)
		return WEOF;
	count = wcrtomb(bytes, character, NULL);
	if (count == (size_t)-1 || fwrite(bytes, 1, count, stream) != count)
		return WEOF;
	return (wint_t)character;
}

wint_t putwc(wchar_t c, FILE *s) { return fputwc(c, s); }
wint_t putwchar(wchar_t c) { return fputwc(c, stdout); }

wint_t
fgetwc(FILE *stream)
{
	mbstate_t state;
	wchar_t result;
	memset(&state, 0, sizeof(state));
	if (stream == NULL || fwide(stream, 1) < 0)
		return WEOF;
	for (;;) {
		char byte;
		int value = fgetc(stream);
		size_t status;
		if (value == EOF) {
			if (!mbsinit(&state))
				errno = EILSEQ;
			return WEOF;
		}
		byte = (char)value;
		status = mbrtowc(&result, &byte, 1, &state);
		if (status == (size_t)-1)
			return WEOF;
		if (status != (size_t)-2)
			return (wint_t)result;
	}
}

wint_t getwc(FILE *stream) { return fgetwc(stream); }
wint_t getwchar(void) { return fgetwc(stdin); }
