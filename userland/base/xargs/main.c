/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int
add(char ***values, int *count, int *capacity, const char *word)
{
	if (*count + 2 > *capacity) {
		char **grown;
		*capacity *= 2;
		grown = realloc(*values, (size_t)*capacity * sizeof(**values));
		if (!grown)
			return -1;
		*values = grown;
	}
	(*values)[(*count)++] = strdup(word);
	return (*values)[*count - 1] ? 0 : -1;
}
int
main(int argc, char **argv)
{
	char **values, *word = NULL;
	int base = argc > 1 ? argc - 1 : 1;
	int count = base, capacity = base + 16, c, quote = 0, escape = 0,
	    status;
	size_t length = 0, word_capacity = 0;
	pid_t child;
	values = calloc((size_t)capacity, sizeof(*values));
	if (!values)
		return 1;
	if (argc > 1)
		memcpy(values, &argv[1], (size_t)base * sizeof(*values));
	else
		values[0] = (char *)"echo";
	while ((c = fgetc(stdin)) != EOF) {
		if (escape)
			escape = 0;
		else if (c == '\\') {
			escape = 1;
			continue;
		} else if (quote) {
			if (c == quote) {
				quote = 0;
				continue;
			}
		} else if (c == '\'' || c == '"') {
			quote = c;
			continue;
		} else if (isspace((unsigned char)c)) {
			if (!length)
				continue;
			word[length] = 0;
			if (add(&values, &count, &capacity, word))
				return 1;
			length = 0;
			continue;
		}
		if (length + 1 >= word_capacity) {
			size_t next = word_capacity ? word_capacity * 2 : 32;
			char *grown = realloc(word, next);
			if (!grown)
				return 1;
			word = grown;
			word_capacity = next;
		}
		word[length++] = (char)c;
	}
	if (quote || escape) {
		fprintf(stderr, "xargs: unmatched quote or escape\n");
		return 1;
	}
	if (length) {
		word[length] = 0;
		if (add(&values, &count, &capacity, word))
			return 1;
	}
	values[count] = NULL;
	child = fork();
	if (child < 0) {
		command_error("xargs", NULL);
		return 1;
	}
	if (!child) {
		command_exec(values[0], values);
		_exit(errno == ENOENT ? 127 : 126);
	}
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 125;
}
