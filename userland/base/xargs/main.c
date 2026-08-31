/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD xargs userland command.
 */

#include "userland/base/common/command.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int add(char ***values, int *count, int *capacity, const char *word);

/*
 * Runs the xargs command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	size_t next;
	char *grown;
	char **values, *word;
	int base;
	int count, capacity, c, quote, escape, status;
	size_t length, word_capacity;
	pid_t child;

	word = NULL;
	base = argc > 1 ? argc - 1 : 1;
	count = base;
	capacity = base + 16;
	quote = 0;
	escape = 0;
	length = 0;
	word_capacity = 0;
	values = calloc((size_t)capacity, sizeof(*values));

	/* Handles the values condition. */
	if (!values)
		return 1;

	/* Validates the command-line arguments. */
	if (argc > 1)
		memcpy(values, &argv[1], (size_t)base * sizeof(*values));
	else

	/* Process input until it is exhausted. */
		values[0] = (char *)"echo";
	while ((c = fgetc(stdin)) != EOF) {
		/* Handles the escape condition. */
		if (escape)
			escape = 0;
		else if (c == '\\') {
			escape = 1;
			continue;
		} else if (quote) {
			/* Classifies the current input character. */
			if (c == quote) {
				quote = 0;
				continue;
			}
		} else if (c == '\'' || c == '"') {
			quote = c;
			continue;
		} else if (isspace((unsigned char)c)) {
			/* Checks the current data length. */
			if (!length)
				continue;
			word[length] = 0;

			/* Handles the add condition. */
			if (add(&values, &count, &capacity, word))
				return 1;
			length = 0;
			continue;
		}

		/* Checks the current data length. */
		if (length + 1 >= word_capacity) {
			next = word_capacity ? word_capacity * 2 : 32;
			grown = realloc(word, next);

			/* Handles the grown condition. */
			if (!grown)
				return 1;
			word = grown;
			word_capacity = next;
		}
		word[length++] = (char)c;
	}

	/* Handles the quote condition. */
	if (quote || escape) {
		fprintf(stderr, "xargs: unmatched quote or escape\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the current data length. */
	if (length) {
		word[length] = 0;

		/* Handles the add condition. */
		if (add(&values, &count, &capacity, word))
			return 1;
	}
	values[count] = NULL;
	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
		command_error("xargs", NULL);

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the child process state. */
	if (!child) {
		command_exec(values[0], values);
		_exit(errno == ENOENT ? 127 : 126);
	}
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}

	/* Computes the function result. */
	function_result = WIFEXITED(status) ? WEXITSTATUS(status) : 125;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the add operation. */
static int
add(
	char ***values,
	int *count,
	int *capacity,
	const char *word)
{
	char **grown;

	/* Checks the remaining item count. */
	if (*count + 2 > *capacity) {
		*capacity *= 2;
		grown = realloc(*values, (size_t)*capacity * sizeof(**values));

		/* Handles the grown condition. */
		if (!grown)
			return -1;
		*values = grown;
	}
	(*values)[(*count)++] = strdup(word);

	/* Returns the computed result. */
	return (*values)[*count - 1] ? 0 : -1;
}
