/*
 * zedBSD C/POSIX and C.UTF-8 locale core
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "libc/stdio-internal.h"
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

struct __locale {
	const char *name;
	unsigned utf8;
};

static struct __locale locale_c = { "C", 0 };
static struct __locale locale_utf8 = { "C.UTF-8", 1 };
static struct __locale *global_category[6] = {
	&locale_c, &locale_c, &locale_c, &locale_c, &locale_c, &locale_c
};
static volatile uint32_t locale_lock_word;
static mbstate_t bootstrap_states[2];

extern const void *__pthread_locale_exchange(const void *, int)
	__attribute__((weak));
extern void *__pthread_mbstate(unsigned) __attribute__((weak));
extern char *getenv(const char *) __attribute__((weak));

static void
locale_lock(void)
{
	while (__atomic_exchange_n(&locale_lock_word, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		;
}

static void
locale_unlock(void)
{
	__atomic_store_n(&locale_lock_word, 0U, __ATOMIC_RELEASE);
}

static struct __locale *
locale_named(const char *name)
{
	if (name == NULL)
		return NULL;
	if (name[0] == '\0') {
		const char *environment = NULL;
		if (getenv != NULL) {
			environment = getenv("LC_ALL");
			if (environment == NULL || environment[0] == '\0')
				environment = getenv("LANG");
		}
		name = environment != NULL && environment[0] != '\0' ?
		    environment : "C";
	}
	if (!strcmp(name, "C") || !strcmp(name, "POSIX"))
		return &locale_c;
	if (!strcmp(name, "C.UTF-8") || !strcmp(name, "C.utf8") ||
	    !strcmp(name, "UTF-8"))
		return &locale_utf8;
	return NULL;
}

static struct __locale *
effective_locale(void)
{
	const void *local = NULL;
	if (__pthread_locale_exchange != NULL)
		local = __pthread_locale_exchange(NULL, 0);
	return local != NULL ? (struct __locale *)local :
	    __atomic_load_n(&global_category[LC_CTYPE], __ATOMIC_ACQUIRE);
}

char *
setlocale(int category, const char *name)
{
	struct __locale *locale;
	unsigned first, last, i;

	if (category < LC_CTYPE || category > LC_ALL) {
		errno = EINVAL;
		return NULL;
	}
	first = category == LC_ALL ? 0U : (unsigned)category;
	last = category == LC_ALL ? 6U : first + 1U;
	if (name == NULL) {
		locale = __atomic_load_n(&global_category[first],
		    __ATOMIC_ACQUIRE);
		for (i = first + 1U; i < last; i++)
			if (__atomic_load_n(&global_category[i],
			    __ATOMIC_ACQUIRE) != locale)
				return (char *)"C";
		return (char *)locale->name;
	}
	locale = locale_named(name);
	if (locale == NULL) {
		errno = ENOENT;
		return NULL;
	}
	locale_lock();
	for (i = first; i < last; i++)
		global_category[i] = locale;
	locale_unlock();
	return (char *)locale->name;
}

locale_t
newlocale(int mask, const char *name, locale_t base)
{
	struct __locale *wanted;
	if (mask < 0 || ((unsigned)mask & ~LC_ALL_MASK) != 0 ||
	    name == NULL || base == LC_GLOBAL_LOCALE) {
		errno = EINVAL;
		return NULL;
	}
	wanted = locale_named(name);
	if (wanted == NULL) {
		errno = ENOENT;
		return NULL;
	}
	if (mask == 0)
		return base != NULL ? base : &locale_c;
	/*
	 * The initial database has internally uniform locales. A later database
	 * may replace this with immutable per-category composite objects.
	 */
	return wanted;
}

locale_t
duplocale(locale_t locale)
{
	if (locale == NULL) {
		errno = EINVAL;
		return NULL;
	}
	if (locale == LC_GLOBAL_LOCALE)
		return effective_locale();
	return locale;
}

void
freelocale(locale_t locale)
{
	(void)locale;
}

locale_t
uselocale(locale_t locale)
{
	const void *previous;
	if (__pthread_locale_exchange == NULL) {
		if (locale != NULL && locale != LC_GLOBAL_LOCALE) {
			errno = ENOSYS;
			return NULL;
		}
		return LC_GLOBAL_LOCALE;
	}
	previous = __pthread_locale_exchange(
	    locale == LC_GLOBAL_LOCALE ? NULL : locale, locale != NULL);
	return previous != NULL ? (locale_t)previous : LC_GLOBAL_LOCALE;
}

size_t
__libc_mb_cur_max(void)
{
	return effective_locale()->utf8 ? 4U : 1U;
}

struct lconv *
localeconv(void)
{
	static char empty[] = "";
	static char decimal[] = ".";
	static struct lconv value = {
		decimal, empty, empty, empty, empty, empty, empty, empty,
		empty, empty, 127, 127, 127, 127, 127, 127, 127, 127
	};
	return &value;
}

char *
nl_langinfo(nl_item item)
{
	static char empty[] = "";
	static char decimal[] = ".";
	static char ascii[] = "US-ASCII";
	static char utf8[] = "UTF-8";
	if (item == CODESET)
		return effective_locale()->utf8 ? utf8 : ascii;
	if (item == RADIXCHAR)
		return decimal;
	return empty;
}

int strcoll(const char *a, const char *b) { return strcmp(a, b); }

size_t
strxfrm(char *destination, const char *source, size_t count)
{
	size_t length = strlen(source);
	if (destination != NULL && count != 0) {
		size_t copied = length < count - 1U ? length : count - 1U;
		memcpy(destination, source, copied);
		destination[copied] = '\0';
	}
	return length;
}

static mbstate_t *
internal_state(unsigned which)
{
	void *state = __pthread_mbstate != NULL ?
	    __pthread_mbstate(which) : NULL;
	return state != NULL ? (mbstate_t *)state : &bootstrap_states[which];
}

int
mbsinit(const mbstate_t *state)
{
	return state == NULL || state->needed == 0;
}

size_t
mbrtowc(wchar_t *result, const char *bytes, size_t count, mbstate_t *state)
{
	mbstate_t *s = state != NULL ? state : internal_state(0);
	size_t used = 0;
	uint32_t value;
	unsigned total;

	if (bytes == NULL) {
		memset(s, 0, sizeof(*s));
		return 0;
	}
	if (!effective_locale()->utf8) {
		unsigned char byte;
		memset(s, 0, sizeof(*s));
		if (count == 0)
			return (size_t)-2;
		byte = (unsigned char)bytes[0];
		if (byte > 0x7fU) {
			errno = EILSEQ;
			return (size_t)-1;
		}
		if (result != NULL)
			*result = (wchar_t)byte;
		return byte == 0 ? 0 : 1;
	}
	if (s->needed == 0) {
		unsigned char first;
		if (count == 0)
			return (size_t)-2;
		first = (unsigned char)bytes[used++];
		if (first < 0x80U) {
			if (result != NULL)
				*result = (wchar_t)first;
			return first == 0 ? 0 : 1;
		}
		if (first >= 0xc2U && first <= 0xdfU) {
			s->needed = 2; s->value = first & 0x1fU;
		} else if (first >= 0xe0U && first <= 0xefU) {
			s->needed = 3; s->value = first & 0x0fU;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			s->needed = 4; s->value = first & 0x07U;
		} else {
			memset(s, 0, sizeof(*s));
			errno = EILSEQ;
			return (size_t)-1;
		}
		s->seen = 1;
	}
	total = s->needed;
	while (s->seen < s->needed && used < count) {
		unsigned char byte = (unsigned char)bytes[used];
		if ((byte & 0xc0U) != 0x80U) {
			memset(s, 0, sizeof(*s));
			errno = EILSEQ;
			return (size_t)-1;
		}
		s->value = (s->value << 6) | (byte & 0x3fU);
		s->seen++;
		used++;
	}
	if (s->seen != s->needed)
		return (size_t)-2;
	value = s->value;
	if ((total == 2 && value < 0x80U) ||
	    (total == 3 && value < 0x800U) ||
	    (total == 4 && value < 0x10000U) ||
	    value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
		memset(s, 0, sizeof(*s));
		errno = EILSEQ;
		return (size_t)-1;
	}
	memset(s, 0, sizeof(*s));
	if (result != NULL)
		*result = (wchar_t)value;
	return used;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *st)
{ return mbrtowc(NULL, s, n, st); }

size_t
wcrtomb(char *bytes, wchar_t character, mbstate_t *state)
{
	mbstate_t *s = state != NULL ? state : internal_state(1);
	uint32_t value = (uint32_t)character;
	memset(s, 0, sizeof(*s));
	if (bytes == NULL)
		return 1;
	if (value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU) ||
	    (!effective_locale()->utf8 && value > 0x7fU)) {
		errno = EILSEQ;
		return (size_t)-1;
	}
	if (value < 0x80U) { bytes[0] = (char)value; return 1; }
	if (value < 0x800U) {
		bytes[0] = (char)(0xc0U | (value >> 6));
		bytes[1] = (char)(0x80U | (value & 0x3fU));
		return 2;
	}
	if (value < 0x10000U) {
		bytes[0] = (char)(0xe0U | (value >> 12));
		bytes[1] = (char)(0x80U | ((value >> 6) & 0x3fU));
		bytes[2] = (char)(0x80U | (value & 0x3fU));
		return 3;
	}
	bytes[0] = (char)(0xf0U | (value >> 18));
	bytes[1] = (char)(0x80U | ((value >> 12) & 0x3fU));
	bytes[2] = (char)(0x80U | ((value >> 6) & 0x3fU));
	bytes[3] = (char)(0x80U | (value & 0x3fU));
	return 4;
}

wint_t
btowc(int byte)
{
	if (byte == EOF || (unsigned)byte > 0x7fU)
		return WEOF;
	return (wint_t)(unsigned char)byte;
}

int wctob(wint_t value) { return value <= 0x7fU ? (int)value : EOF; }

size_t
mbsrtowcs(wchar_t *destination, const char **source, size_t count,
	mbstate_t *state)
{
	const char *input;
	size_t output = 0;
	mbstate_t local;
	if (source == NULL || *source == NULL) {
		errno = EINVAL;
		return (size_t)-1;
	}
	input = *source;
	if (state == NULL) {
		memset(&local, 0, sizeof(local));
		state = &local;
	}
	for (;;) {
		wchar_t value;
		size_t available = strlen(input) + 1U;
		size_t used = mbrtowc(&value, input, available, state);
		if (used == (size_t)-1 || used == (size_t)-2) {
			*source = input;
			return (size_t)-1;
		}
		if (value == 0) {
			if (destination != NULL && output < count)
				destination[output] = 0;
			*source = NULL;
			return output;
		}
		if (destination != NULL) {
			if (output == count) {
				*source = input;
				return output;
			}
			destination[output] = value;
		}
		output++;
		input += used;
	}
}

size_t
wcsrtombs(char *destination, const wchar_t **source, size_t count,
	mbstate_t *state)
{
	const wchar_t *input;
	size_t output = 0;
	char encoded[4];
	mbstate_t local;
	if (source == NULL || *source == NULL) {
		errno = EINVAL;
		return (size_t)-1;
	}
	input = *source;
	if (state == NULL) {
		memset(&local, 0, sizeof(local));
		state = &local;
	}
	while (*input != 0) {
		size_t bytes = wcrtomb(encoded, *input, state);
		if (bytes == (size_t)-1) {
			*source = input;
			return (size_t)-1;
		}
		if (destination != NULL) {
			if (bytes > count - output) {
				*source = input;
				return output;
			}
			memcpy(destination + output, encoded, bytes);
		}
		output += bytes;
		input++;
	}
	if (destination != NULL && output < count)
		destination[output] = '\0';
	*source = NULL;
	return output;
}
