/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text of ssh_config's ProxyCommand (proxy-command.h): the tokens
 * OpenSSH's ssh puts in a ProxyCommand (%%, %h, %n, %p and %r; any other
 * token, and a '%' at the end, are refused), and the program the command
 * names.
 *
 * OpenSSH hands the line to the user's shell.  Here no shell runs it: the
 * first word is the program (in double quotes when its path has spaces),
 * and the rest of the line is the program's command line as written,
 * which the program splits into its arguments by Windows' rules.
 */

#include "proxy-command.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int proxy_command_put(char *out, size_t size, size_t *used, const char *text);
static int proxy_command_blank(char character);

/*
 * Writes text into out (size bytes with its NUL) with each %-token
 * replaced by its value.  Returns 0, EINVAL with the reason in error (a
 * token that cannot be put in), or EMSGSIZE for a command longer than
 * out takes.
 *
 * Procedure referenced: openssh-portable fa4b2a82be7df9768272cfabc2d78e4a12c2b3c0 sshconnect.c
 * (ssh_proxy_connect: percent_expand of %%, %h, %n, %p and %r), and
 * ssh_config(5) of OpenSSH, TOKENS (ProxyCommand accepts %%, %h, %n, %p
 * and %r).
 */
int
proxy_command_expand(
	const char *text,
	const struct proxy_command_tokens *tokens,
	char *out,
	size_t size,
	char *error,
	size_t error_size)
{
	char number[24];
	const char *value;
	char single[2];
	size_t used;
	int status;

	/* Nothing written yet. */
	error[0] = '\0';
	used = 0U;
	if (size == 0U)
		return EMSGSIZE;
	out[0] = '\0';

	/* Each character, a token's value in the token's place. */
	for (; *text != '\0'; text++) {
		/* A character that starts no token is copied. */
		if (*text != '%') {
			single[0] = *text;
			single[1] = '\0';
			status = proxy_command_put(out, size, &used, single);
			if (status != 0)
				return status;
			continue;
		}

		/* A '%' at the end names no token. */
		text++;
		if (*text == '\0') {
			(void)snprintf(error, error_size, "ProxyCommand: a %% at the end");
			return EINVAL;
		}

		/* The token's value. */
		value = NULL;
		switch (*text) {
		case '%':
			value = "%";
			break;
		case 'h':
			value = tokens->host_name;
			break;
		case 'n':
			value = tokens->host;
			break;
		case 'p':
			(void)snprintf(number, sizeof(number), "%u", tokens->port);
			value = number;
			break;
		case 'r':
			value = tokens->user;
			break;
		default:
			break;
		}

		/* A token ProxyCommand does not have. */
		if (value == NULL) {
			(void)snprintf(error, error_size, "ProxyCommand: unknown token %%%c", *text);
			return EINVAL;
		}

		/* The value in its place. */
		status = proxy_command_put(out, size, &used, value);
		if (status != 0)
			return status;
	}

	/* Succeeded: the command with its tokens put in. */
	return 0;
}

/*
 * Takes the program from a command: the text in double quotes when the
 * command starts with one (words must then be 1), else its first words
 * blank-separated words as written (more than one for a path with spaces
 * not in quotes).  *rest is the rest of the command after them, and
 * *quoted says whether the program was in quotes.  Returns 0, EINVAL for
 * an empty command or a quote not closed, ENOENT when the command has
 * fewer words, or ENAMETOOLONG when the program does not fit.
 */
int
proxy_command_program(
	const char *command,
	unsigned words,
	char *program,
	size_t size,
	const char **rest,
	int *quoted)
{
	const char *start;
	const char *end;
	const char *closing;
	size_t length;
	unsigned counted;
	int blank;

	/* The blanks before the program. */
	start = command;
	for (;;) {
		/* A blank is passed over. */
		blank = proxy_command_blank(*start);
		if (!blank)
			break;
		start++;
	}

	/* An empty command names no program. */
	if (*start == '\0')
		return EINVAL;

	/* A program in double quotes is one word: the text between them. */
	*quoted = 0;
	if (*start == '"') {
		if (words != 1U)
			return ENOENT;
		closing = strchr(start + 1, '"');
		if (closing == NULL)
			return EINVAL;
		*quoted = 1;
		start++;
		end = closing;
		*rest = closing + 1;
	} else {
		/* So many words, the blanks between them as written. */
		end = start;
		counted = 0U;
		while (counted < words) {
			/* The blanks before a word after the first. */
			if (counted != 0U) {
				for (;;) {
					/* A blank is passed over. */
					blank = proxy_command_blank(*end);
					if (!blank)
						break;
					end++;
				}
			}

			/* A command with fewer words. */
			if (*end == '\0')
				return ENOENT;

			/* The word up to the next blank. */
			for (;;) {
				/* The word ends at a blank or at the end. */
				blank = proxy_command_blank(*end);
				if (blank || *end == '\0')
					break;
				end++;
			}
			counted++;
		}
		*rest = end;
	}

	/* The program, when it fits. */
	length = (size_t)(end - start);
	if (length == 0U)
		return EINVAL;
	if (length >= size)
		return ENAMETOOLONG;
	memcpy(program, start, length);
	program[length] = '\0';

	/* Succeeded: the program and the rest. */
	return 0;
}

/* Appends text to out (size bytes with the NUL, *used of them used); returns 0 or EMSGSIZE. */
static int
proxy_command_put(
	char *out,
	size_t size,
	size_t *used,
	const char *text)
{
	size_t length;

	/* The text must fit with the NUL. */
	length = strlen(text);
	if (*used + length + 1U > size)
		return EMSGSIZE;

	/* Succeeded: the text is after what was there. */
	memcpy(out + *used, text, length + 1U);
	*used += length;
	return 0;
}

/* Tells whether a character separates words of the command (1) or not (0). */
static int
proxy_command_blank(
	char character)
{
	/* A space or a tab. */
	if (character == ' ' || character == '\t')
		return 1;

	/* Any other. */
	return 0;
}
