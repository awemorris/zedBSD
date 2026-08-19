/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/sh/lexer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
word(const struct sh_token_list *list, size_t index, const char *expected)
{
	assert(index < list->count);
	assert(list->tokens[index].type == SH_TOKEN_WORD);
	assert(strcmp(list->tokens[index].text, expected) == 0);
}

static void
operators(void)
{
	static const enum sh_token_type expected[] = {
		SH_TOKEN_WORD, SH_TOKEN_AND_IF, SH_TOKEN_WORD, SH_TOKEN_OR_IF,
		SH_TOKEN_WORD, SH_TOKEN_SEMI, SH_TOKEN_WORD, SH_TOKEN_AMP,
		SH_TOKEN_WORD, SH_TOKEN_PIPE, SH_TOKEN_WORD, SH_TOKEN_INPUT,
		SH_TOKEN_WORD, SH_TOKEN_OUTPUT, SH_TOKEN_WORD, SH_TOKEN_APPEND,
		SH_TOKEN_WORD, SH_TOKEN_END
	};
	struct sh_token_list list;
	const char *error;
	size_t index;
	assert(sh_lex("a&&b||c;d&e|f<g>h>>i", &list, &error));
	assert(list.count == sizeof(expected) / sizeof(expected[0]));
	for (index = 0; index < list.count; index++)
		assert(list.tokens[index].type == expected[index]);
	sh_tokens_free(&list);
}

int
main(void)
{
	struct sh_token_list list;
	const char *error;

	assert(sh_lex("echo 'a b' \"c d\" e\\ f \"\" ab'cd'ef # ignored",
	    &list, &error));
	assert(list.count == 7);
	word(&list, 0, "echo");
	word(&list, 1, "a b");
	word(&list, 2, "c d");
	word(&list, 3, "e f");
	word(&list, 4, "");
	word(&list, 5, "abcdef");
	assert(list.tokens[6].type == SH_TOKEN_END);
	sh_tokens_free(&list);

	assert(sh_lex("value#suffix # comment", &list, &error));
	assert(list.count == 2);
	word(&list, 0, "value#suffix");
	sh_tokens_free(&list);
	assert(sh_lex("\"a\\n\"", &list, &error));
	word(&list, 0, "a\\n");
	sh_tokens_free(&list);

	operators();
	assert(!sh_lex("echo 'unterminated", &list, &error));
	assert(strcmp(error, "unterminated quote") == 0);
	assert(!sh_lex("echo trailing\\", &list, &error));
	assert(strcmp(error, "trailing backslash") == 0);
	puts("zedBSD shell lexer host test: PASS");
	return 0;
}
