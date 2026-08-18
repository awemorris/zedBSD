/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/sh/alias.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	struct sh_token_list list;
	const char *error;
	assert(sh_alias_set("ll", "echo expanded") == 0);
	assert(sh_lex("X=1 ll; 'll'; echo ll", &list, &error));
	assert(sh_alias_expand(&list, &error));
	assert(strcmp(list.tokens[1].text, "echo") == 0);
	assert(strcmp(list.tokens[2].text, "expanded") == 0);
	assert(strcmp(list.tokens[4].text, "ll") == 0);
	assert(strcmp(list.tokens[7].text, "ll") == 0);
	sh_tokens_free(&list);
	assert(sh_alias_set("a", "b") == 0);
	assert(sh_alias_set("b", "a") == 0);
	assert(sh_lex("a", &list, &error));
	assert(!sh_alias_expand(&list, &error));
	sh_tokens_free(&list);
	sh_alias_clear();
	puts("zedBSD shell alias host test: PASS");
	return 0;
}
