/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Word expansion (POSIX XCU 2.6).
 *
 * A word is expanded from the text it was written as (struct sh_token raw):
 * tilde expansion, parameter expansion, command substitution and arithmetic
 * expansion in one left-to-right pass, then field splitting, then quote
 * removal.  Pathname expansion is the caller's (sh_glob_fields), on the
 * fields and the marks that say which of their characters were quoted.
 */

#ifndef KERN_USERLAND_SH_EXPAND_H
#define KERN_USERLAND_SH_EXPAND_H

#include "userland/base/sh/lexer.h"

struct sh_expand_context {
	int status;
	long shell_pid;
	long last_job;
	const char *(*lookup)(void *, const char *);
	int (*assign)(void *, const char *, const char *);
	int (*command_substitute)(void *, const char *, char **);
	void *lookup_context;
	const char *shell_name;
	int positional_count;
	char **positional;

	/* The letters of the set options, which is what $- expands to. */
	const char *options;

	/*
	 * Set when a parameter that was never set is a fault rather than an
	 * empty word, which is what the u option asks for.  A parameter
	 * written with a word to fall back on is never a fault, because the
	 * word is what it is for.
	 */
	int unset_is_error;
};

struct sh_field_list {
	char **fields;
	unsigned char **quoted;
	size_t count;
};

/* Every expansion and field splitting: the arguments of a simple command. */
int sh_expand_fields(const struct sh_token *, const struct sh_expand_context *,
		     struct sh_field_list *, const char **);

/*
 * One word without field splitting and with the quotes removed: the value of
 * an assignment (with the tilde rules of assignments), a redirection's file,
 * the word a case matches, a here-document body.
 */
int sh_expand_word(const struct sh_token *, const struct sh_expand_context *,
		   char **, const char **);

/*
 * One word without field splitting, keeping which characters were quoted so
 * that a quoted * stands for itself: a case pattern.
 */
int sh_expand_pattern(const struct sh_token *,
		      const struct sh_expand_context *, char **,
		      unsigned char **, const char **);

/* Expands a string as an arithmetic expression and evaluates it. */
int sh_expand_arithmetic(const char *, const struct sh_expand_context *,
			 long *, const char **);

void sh_fields_free(struct sh_field_list *);

/*
 * Set when the last failed expansion was one the shell must stop for: an
 * unset parameter under set -u, or ${name?word}.  A shell that is not
 * interactive exits.
 */
extern int sh_expand_fatal;

#endif
