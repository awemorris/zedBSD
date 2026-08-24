/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <locale.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
	do {                                                                   \
		if (!(condition)) {                                            \
			fprintf(stderr, "regex test failed at line %d\n",      \
				__LINE__);                                     \
			exit(1);                                               \
		}                                                              \
	} while (0)

static void
must_match(const char *pattern, const char *text, const char *capture)
{
	regex_t expression;
	regmatch_t match[2];

	CHECK(regcomp(&expression, pattern, 0) == 0);
	CHECK(regexec(&expression, text, 2, match, 0) == 0);
	if (capture != NULL) {
		CHECK(match[1].rm_so >= 0);
		CHECK((size_t)(match[1].rm_eo - match[1].rm_so) ==
		      strlen(capture));
		CHECK(memcmp(text + match[1].rm_so, capture, strlen(capture)) ==
		      0);
	}
	regfree(&expression);
}

int
main(void)
{
	regex_t expression;
	char error[64];

	(void)setlocale(LC_ALL, "C.UTF-8");
	must_match("^ab*c$", "abbbc", NULL);
	must_match("^\\(ab\\)c\\1$", "abcab", "ab");
	must_match("^[[:alpha:]][[:digit:]]\\{2,3\\}$", "z123", NULL);
	CHECK(regcomp(&expression, "[", 0) == REG_EBRACK);
	CHECK(regerror(REG_EBRACK, &expression, error, sizeof(error)) > 1);
	CHECK(strstr(error, "Missing") != NULL);
	return 0;
}
