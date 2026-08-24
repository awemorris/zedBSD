/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <ctype.h>
#include "libc/include/fnmatch.h"
#include <stddef.h>
#include <string.h>

static unsigned char
fold(unsigned char character, int flags)
{
	return flags & FNM_CASEFOLD ? (unsigned char)tolower(character)
				    : character;
}

static int
character_class(const char *name, size_t length, unsigned char character)
{
#define CLASS(test_name, function)                                             \
	if (length == sizeof(test_name) - 1U &&                                \
	    memcmp(name, test_name, sizeof(test_name) - 1U) == 0)              \
	return function(character) != 0
	CLASS("alnum", isalnum);
	CLASS("alpha", isalpha);
	CLASS("blank", isblank);
	CLASS("cntrl", iscntrl);
	CLASS("digit", isdigit);
	CLASS("graph", isgraph);
	CLASS("lower", islower);
	CLASS("print", isprint);
	CLASS("punct", ispunct);
	CLASS("space", isspace);
	CLASS("upper", isupper);
	CLASS("xdigit", isxdigit);
#undef CLASS
	return 0;
}

static int
bracket(const char **pattern_pointer, unsigned char character, int flags)
{
	const char *pattern = *pattern_pointer;
	int negate = 0;
	int matched = 0;
	int first = 1;

	if (*pattern == '!' || *pattern == '^') {
		negate = 1;
		pattern++;
	}
	if (*pattern == ']') {
		matched = character == ']';
		pattern++;
		first = 0;
	}
	while (*pattern != '\0' && (*pattern != ']' || first)) {
		unsigned char low;
		unsigned char high;

		first = 0;
		if (pattern[0] == '[' && pattern[1] == ':') {
			const char *end = strstr(pattern + 2, ":]");

			if (end != NULL) {
				if (character_class(pattern + 2,
						    (size_t)(end - pattern - 2),
						    character))
					matched = 1;
				pattern = end + 2;
				continue;
			}
		}
		if (*pattern == '\\' && !(flags & FNM_NOESCAPE) &&
		    pattern[1] != '\0')
			pattern++;
		low = fold((unsigned char)*pattern++, flags);
		high = low;
		if (*pattern == '-' && pattern[1] != '\0' &&
		    pattern[1] != ']') {
			pattern++;
			if (*pattern == '\\' && !(flags & FNM_NOESCAPE) &&
			    pattern[1] != '\0')
				pattern++;
			high = fold((unsigned char)*pattern++, flags);
		}
		if (fold(character, flags) >= low &&
		    fold(character, flags) <= high)
			matched = 1;
	}
	if (*pattern != ']')
		return -1;
	*pattern_pointer = pattern + 1;
	return matched != negate;
}

static int
match(const char *pattern, const char *string, int flags, int component_start)
{
	for (;;) {
		unsigned char expected;

		switch (*pattern) {
		case '\0':
			return *string == '\0' ||
			       ((flags & FNM_LEADING_DIR) && *string == '/');
		case '?':
			if (*string == '\0' ||
			    ((flags & FNM_PATHNAME) && *string == '/') ||
			    ((flags & FNM_PERIOD) && component_start &&
			     *string == '.'))
				return 0;
			pattern++;
			component_start =
			    (flags & FNM_PATHNAME) && *string == '/';
			string++;
			break;
		case '*':
			if ((flags & FNM_PERIOD) && component_start &&
			    *string == '.')
				return 0;
			while (*pattern == '*')
				pattern++;
			if (*pattern == '\0')
				return !(flags & FNM_PATHNAME) ||
				       strchr(string, '/') == NULL ||
				       (flags & FNM_LEADING_DIR);
			do {
				if (match(pattern, string, flags,
					  component_start))
					return 1;
				if (*string == '\0' ||
				    ((flags & FNM_PATHNAME) && *string == '/'))
					break;
				component_start = 0;
				string++;
			} while (1);
			return 0;
		case '[': {
			int result;

			if (*string == '\0' ||
			    ((flags & FNM_PATHNAME) && *string == '/') ||
			    ((flags & FNM_PERIOD) && component_start &&
			     *string == '.'))
				return 0;
			pattern++;
			result =
			    bracket(&pattern, (unsigned char)*string, flags);
			if (result == 0)
				return 0;
			if (result < 0) {
				pattern--;
				expected = '[';
				goto literal;
			}
			component_start = 0;
			string++;
			break;
		}
		case '\\':
			if (!(flags & FNM_NOESCAPE) && pattern[1] != '\0')
				pattern++;
			/* FALLTHROUGH */
		default:
			expected = (unsigned char)*pattern++;
		literal:
			if (*string == '\0' ||
			    fold(expected, flags) !=
				fold((unsigned char)*string, flags))
				return 0;
			component_start =
			    (flags & FNM_PATHNAME) && *string == '/';
			string++;
			break;
		}
	}
}

int
fnmatch(const char *pattern, const char *string, int flags)
{
	return match(pattern, string, flags, 1) ? 0 : FNM_NOMATCH;
}
