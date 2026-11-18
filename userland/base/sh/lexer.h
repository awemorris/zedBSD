/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The words of the shell language as the parser hands them to expansion,
 * and the scanners that find where a quotation or an expansion ends in the
 * text of a word.
 */

#ifndef KERN_USERLAND_SH_LEXER_H
#define KERN_USERLAND_SH_LEXER_H

#include <stddef.h>

/* How a character of a word's quote-removed text was quoted. */
enum sh_quote_type {
	SH_QUOTE_UNQUOTED,
	SH_QUOTE_SINGLE,
	SH_QUOTE_DOUBLE,
	SH_QUOTE_ESCAPED
};

/* What a here-document body holds (struct sh_token heredoc). */
#define SH_HEREDOC_NONE		0
#define SH_HEREDOC_EXPAND	1	/* the delimiter was not quoted */
#define SH_HEREDOC_LITERAL	2	/* the delimiter was quoted */

/*
 * One word, as written and with its quotes removed.
 *
 * raw is what expansion reads: the word as it was written, quotes and
 * substitutions and all, less any line continuation.  text is the word with
 * its quotes removed, with how each character was quoted, which is what the
 * grammar looks at: an assignment's name, a here-document's delimiter.  A
 * here-document's body is a word whose raw text is the body.
 */
struct sh_token {
	char *text;
	unsigned char *quote;
	size_t length;
	char *raw;
	size_t raw_length;
	int heredoc;
	int process;	/* '<' or '>' for <( ) or >( ) (bash), else 0 */
};

/*
 * Find where a construct ends in the text of a word, and return the first
 * character after it, or NULL when the text ends first: a single quotation,
 * a double quotation, or an expansion ($name, ${...}, $(...), $((...)) or a
 * backquoted command), all at their opening character.  in_double is set
 * inside a double quotation, where a single quote is ordinary.
 */
const char *sh_skip_single(const char *text);
const char *sh_skip_double(const char *text);
const char *sh_skip_expansion(const char *text, int in_double);

#endif
