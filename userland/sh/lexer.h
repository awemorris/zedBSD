/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_SH_LEXER_H
#define ZEDBSD_USERLAND_SH_LEXER_H

#include <stddef.h>

enum sh_token_type {
	SH_TOKEN_WORD,
	SH_TOKEN_SEMI,
	SH_TOKEN_AMP,
	SH_TOKEN_AND_IF,
	SH_TOKEN_OR_IF,
	SH_TOKEN_PIPE,
	SH_TOKEN_INPUT,
	SH_TOKEN_OUTPUT,
	SH_TOKEN_APPEND,
	SH_TOKEN_END
};

struct sh_token {
	enum sh_token_type type;
	char *text;
};

struct sh_token_list {
	struct sh_token *tokens;
	size_t count;
};

/* Returns zero and leaves error_text pointing at a static diagnostic on error. */
int sh_lex(const char *, struct sh_token_list *, const char **error_text);
void sh_tokens_free(struct sh_token_list *);

#endif
