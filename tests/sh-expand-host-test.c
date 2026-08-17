/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200809L
#include "userland/sh/expand.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
expect(const char *source, const struct sh_expand_context *context,
    const char *expected)
{
	struct sh_token_list list;
	const char *error;
	char *value;
	assert(sh_lex(source, &list, &error));
	assert(list.count == 2);
	assert(sh_expand_word(&list.tokens[0], context, &value, &error));
	assert(strcmp(value, expected) == 0);
	free(value);
	sh_tokens_free(&list);
}

int
main(void)
{
	struct sh_expand_context context = { 7, 123, 456 };
	struct sh_token_list list;
	const char *error;
	char *value;

	assert(setenv("SH_EXPAND_TEST", "value with spaces", 1) == 0);
	expect("'$SH_EXPAND_TEST'", &context, "$SH_EXPAND_TEST");
	expect("\"$SH_EXPAND_TEST\"", &context, "value with spaces");
	expect("pre${SH_EXPAND_TEST}post", &context,
	    "prevalue with spacespost");
	expect("\\$SH_EXPAND_TEST", &context, "$SH_EXPAND_TEST");
	expect("$?", &context, "7");
	expect("$$", &context, "123");
	expect("$!", &context, "456");
	expect("${SH_EXPAND_UNDEFINED}", &context, "");

	assert(sh_lex("${SH_EXPAND_TEST:-fallback}", &list, &error));
	assert(!sh_expand_word(&list.tokens[0], &context, &value, &error));
	assert(strcmp(error, "unsupported parameter expansion") == 0);
	sh_tokens_free(&list);
	puts("zedBSD shell expansion host test: PASS");
	return 0;
}
