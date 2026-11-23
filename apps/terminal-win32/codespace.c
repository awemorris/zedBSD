/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GitHub Codespaces on the launch screen (codespace.h).
 *
 * gh lists the codespaces with "gh codespace list", and writes one's ssh
 * settings with "gh codespace ssh -c NAME --config": a Host block
 * (cs.NAME.BRANCH) with the User, the key gh made (IdentityFile) and a
 * ProxyCommand that runs "gh cs ssh -c NAME --stdio -- -i KEY", which
 * carries the connection to the codespace's sshd through GitHub.  gh
 * writes the paths without quotes, so here IdentityFile's path is put in
 * quotes (a user's folder may have a space in its name) and the
 * ProxyCommand is made again with its paths in quotes (a path such as
 * C:\Program Files\GitHub CLI\gh.exe has spaces).
 *
 * Procedure referenced: cli/cli pkg/cmd/codespace/ssh.go (printOpenSSHConfig:
 * the Host block's lines; SSH with --stdio: the connection on the
 * standard input and output) and pkg/cmd/codespace/list.go (the --json
 * fields).
 */

#include "codespace.h"

#include "userland/base/ssh/ssh-config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The fields of a line of the list, in their order. */
#define CODESPACE_FIELD_NAME		0U
#define CODESPACE_FIELD_STATE		1U
#define CODESPACE_FIELD_REPOSITORY	2U
#define CODESPACE_FIELD_DISPLAY		3U
#define CODESPACE_FIELDS		4U

/* The keyword whose path is put in quotes. */
#define CODESPACE_IDENTITY_KEYWORD	"IdentityFile"

/* What codespace_host_named answers when it has the first Host. */
#define CODESPACE_HOST_FOUND		1

/* Where codespace_host_named keeps the first Host's name: the room and its size. */
struct codespace_host {
	char *name;
	size_t size;
};

static int codespace_name_valid(const char *name, size_t length);
static void codespace_copy(char *field, size_t field_size, const char *start, size_t length);
static int codespace_host_named(void *data, const char *name, size_t size);
static int codespace_path_valid(const char *path);
static int codespace_put(char *out, size_t size, size_t *used, const char *bytes, size_t length);
static int codespace_identity_line(const char *line, size_t length, size_t *value);
static int codespace_blank(char character);
static size_t codespace_value_end(const char *line, size_t value, size_t length);

/*
 * Reads gh's list (CODESPACE_LIST_ARGUMENTS) into entries, at most so
 * many: a line of four fields separated by tabs, whose name is one gh
 * gives (letters, digits, '-', '_' and '.'); any other line is passed
 * over.  The other fields are cut to fit, a control character in them
 * shown as a space.  Returns how many were read.
 */
unsigned
codespace_list_read(
	const char *text,
	struct codespace_entry *entries,
	unsigned most)
{
	const char *starts[CODESPACE_FIELDS];
	size_t lengths[CODESPACE_FIELDS];
	const char *line;
	const char *end;
	const char *next;
	const char *field;
	const char *tab;
	unsigned count;
	unsigned index;
	int valid;

	/* Each line in turn. */
	count = 0U;
	line = text;
	while (*line != '\0' && count < most) {
		/* The line's end, without its CR, and where the next starts. */
		end = strchr(line, '\n');
		if (end == NULL)
			end = line + strlen(line);
		next = end;
		if (*next == '\n')
			next++;
		if (end > line && end[-1] == '\r')
			end--;

		/* The four fields, each up to a tab or the line's end. */
		field = line;
		for (index = 0U; index < CODESPACE_FIELDS; index++) {
			/* The field's start and length. */
			tab = memchr(field, '\t', (size_t)(end - field));
			if (tab == NULL)
				tab = end;
			starts[index] = field;
			lengths[index] = (size_t)(tab - field);

			/* The next field after the tab; none after the line's end. */
			field = tab;
			if (field < end)
				field++;
		}

		/* A line whose name gh would give. */
		valid = codespace_name_valid(starts[CODESPACE_FIELD_NAME], lengths[CODESPACE_FIELD_NAME]);
		if (valid) {
			memset(&entries[count], 0, sizeof(entries[count]));
			codespace_copy(entries[count].name, sizeof(entries[count].name), starts[CODESPACE_FIELD_NAME], lengths[CODESPACE_FIELD_NAME]);
			codespace_copy(entries[count].state, sizeof(entries[count].state), starts[CODESPACE_FIELD_STATE], lengths[CODESPACE_FIELD_STATE]);
			codespace_copy(entries[count].repository, sizeof(entries[count].repository), starts[CODESPACE_FIELD_REPOSITORY], lengths[CODESPACE_FIELD_REPOSITORY]);
			codespace_copy(entries[count].display, sizeof(entries[count].display), starts[CODESPACE_FIELD_DISPLAY], lengths[CODESPACE_FIELD_DISPLAY]);
			count++;
		}

		/* The next line. */
		line = next;
	}

	/* Succeeded: the codespaces read. */
	return count;
}

/*
 * Writes gh's arguments for a codespace's ssh settings ("codespace ssh
 * -c NAME --config") into arguments (size bytes).  Returns 0, EINVAL for
 * a name gh does not give, or ENAMETOOLONG.
 */
int
codespace_config_arguments(
	const char *name,
	char *arguments,
	size_t size)
{
	int valid;
	int written;

	/* A name gh gives (nothing in it is read as more arguments). */
	valid = codespace_name_valid(name, strlen(name));
	if (!valid)
		return EINVAL;

	/* The arguments. */
	written = snprintf(arguments, size, "codespace ssh -c %s --config", name);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the arguments. */
	return 0;
}

/*
 * Copies gh's ssh settings (text) into out (size bytes with its NUL;
 * *length without it), an IdentityFile path with a blank in it and not
 * in quotes put in quotes, so it is read as one path.  Returns 0 or
 * EMSGSIZE.
 */
int
codespace_config_quote(
	const char *text,
	char *out,
	size_t size,
	size_t *length)
{
	const char *line;
	const char *end;
	const char *next;
	const char *blank;
	size_t line_length;
	size_t value;
	size_t value_end;
	size_t used;
	int identity;
	int status;

	/* Nothing yet. */
	used = 0U;
	*length = 0U;
	if (size == 0U)
		return EMSGSIZE;
	out[0] = '\0';

	/* Each line in turn, with its line break. */
	line = text;
	while (*line != '\0') {
		/* The line's end and where the next starts. */
		end = strchr(line, '\n');
		if (end == NULL)
			end = line + strlen(line);
		next = end;
		if (*next == '\n')
			next++;
		line_length = (size_t)(end - line);

		/* An IdentityFile line whose path has a space and no quote: the path in quotes. */
		identity = codespace_identity_line(line, line_length, &value);
		blank = NULL;
		value_end = value;
		if (identity) {
			value_end = codespace_value_end(line, value, line_length);
			if (value_end > value && line[value] != '"')
				blank = memchr(line + value, ' ', value_end - value);
		}

		/* The line, the path in quotes when it needs them. */
		if (blank != NULL) {
			status = codespace_put(out, size, &used, line, value);
			if (status == 0)
				status = codespace_put(out, size, &used, "\"", 1U);
			if (status == 0)
				status = codespace_put(out, size, &used, line + value, value_end - value);
			if (status == 0)
				status = codespace_put(out, size, &used, "\"", 1U);
			if (status == 0)
				status = codespace_put(out, size, &used, line + value_end, (size_t)(next - line) - value_end);
		} else {
			status = codespace_put(out, size, &used, line, (size_t)(next - line));
		}
		if (status != 0)
			return status;

		/* The next line. */
		line = next;
	}

	/* Succeeded: the settings. */
	*length = used;
	return 0;
}

/*
 * Writes the name of the first Host of gh's ssh settings (size bytes of
 * text) into host (host_size bytes).  Returns 0, ENOENT when the text has
 * no Host, EINVAL when it is not ssh_config text, or ENAMETOOLONG.
 */
int
codespace_config_host(
	const char *text,
	size_t size,
	char *host,
	size_t host_size)
{
	struct codespace_host found;
	int answer;

	/* Nothing found yet. */
	if (host_size == 0U)
		return ENAMETOOLONG;
	host[0] = '\0';
	found.name = host;
	found.size = host_size;

	/* The Host lines' names, until the first. */
	answer = ssh_config_hosts(text, size, codespace_host_named, &found, NULL);
	if (answer == CODESPACE_HOST_FOUND)
		return 0;
	if (answer == ENAMETOOLONG)
		return ENAMETOOLONG;
	if (answer != 0)
		return EINVAL;

	/* No Host. */
	return ENOENT;
}

/*
 * Writes the ProxyCommand that reaches a codespace through gh into
 * command (size bytes): gh's path in quotes, "codespace ssh -c NAME
 * --stdio", and, when the settings name a key, "-- -i" and its path in
 * quotes (NULL or empty: none), as gh's own settings have it.  Returns
 * 0, EINVAL for a name gh does not give or a path that cannot be quoted
 * (with a quote in it, or ending with a backslash), or ENAMETOOLONG.
 */
int
codespace_proxy_command(
	const char *gh,
	const char *name,
	const char *identity,
	char *command,
	size_t size)
{
	int valid;
	int written;

	/* A name gh gives, and paths that can be in quotes. */
	valid = codespace_name_valid(name, strlen(name));
	if (!valid)
		return EINVAL;
	valid = codespace_path_valid(gh);
	if (!valid)
		return EINVAL;

	/* Without a key: gh chooses one. */
	if (identity == NULL || identity[0] == '\0') {
		written = snprintf(command, size, "\"%s\" codespace ssh -c %s --stdio", gh, name);
		if (written < 0 || (size_t)written >= size)
			return ENAMETOOLONG;
		return 0;
	}

	/* The key's path must be able to be in quotes too. */
	valid = codespace_path_valid(identity);
	if (!valid)
		return EINVAL;

	/* With the key the settings name. */
	written = snprintf(command, size, "\"%s\" codespace ssh -c %s --stdio -- -i \"%s\"", gh, name, identity);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the command. */
	return 0;
}

/* Tells whether a name is one gh gives a codespace (1) or not (0): letters, digits, '-', '_' and '.', not starting with '-'. */
static int
codespace_name_valid(
	const char *name,
	size_t length)
{
	size_t index;
	char character;

	/* Not empty, fitting, and not read as an option. */
	if (length == 0U || length >= CODESPACE_NAME_MAX)
		return 0;
	if (name[0] == '-')
		return 0;

	/* Each character one a name has. */
	for (index = 0U; index < length; index++) {
		/* A letter, a digit or one of the marks. */
		character = name[index];
		if (character >= 'a' && character <= 'z')
			continue;
		if (character >= 'A' && character <= 'Z')
			continue;
		if (character >= '0' && character <= '9')
			continue;
		if (character == '-' ||
		    character == '_' ||
		    character == '.')
			continue;
		return 0;
	}

	/* A name gh gives. */
	return 1;
}

/* Copies a field (length bytes) into its room, cut to fit, a control character shown as a space. */
static void
codespace_copy(
	char *field,
	size_t field_size,
	const char *start,
	size_t length)
{
	size_t index;

	/* What fits with the NUL. */
	if (length >= field_size)
		length = field_size - 1U;

	/* Each byte, a control character as a space. */
	for (index = 0U; index < length; index++) {
		/* A control character would move the screen's cursor. */
		field[index] = start[index];
		if ((unsigned char)start[index] < 0x20U || start[index] == 0x7f)
			field[index] = ' ';
	}
	field[length] = '\0';
}

/* Keeps the first Host's name (ssh_config_hosts' handler); answers CODESPACE_HOST_FOUND, or ENAMETOOLONG when it does not fit. */
static int
codespace_host_named(
	void *data,
	const char *name,
	size_t size)
{
	struct codespace_host *found;

	/* The room. */
	found = data;
	if (size >= found->size)
		return ENAMETOOLONG;

	/* Succeeded: the name, and the walk stops. */
	memcpy(found->name, name, size);
	found->name[size] = '\0';
	return CODESPACE_HOST_FOUND;
}

/* Tells whether a path can be in double quotes on a command line (1) or not (0): no quote in it, not ending with a backslash. */
static int
codespace_path_valid(
	const char *path)
{
	const char *quote;
	size_t length;

	/* Not empty, and no quote in it. */
	length = strlen(path);
	if (length == 0U)
		return 0;
	quote = strchr(path, '"');
	if (quote != NULL)
		return 0;

	/* A backslash before the closing quote would make it part of the path. */
	if (path[length - 1U] == '\\')
		return 0;

	/* It can. */
	return 1;
}

/* Appends length bytes to out (size bytes with the NUL, *used of them used); returns 0 or EMSGSIZE. */
static int
codespace_put(
	char *out,
	size_t size,
	size_t *used,
	const char *bytes,
	size_t length)
{
	/* The bytes must fit with the NUL. */
	if (*used + length + 1U > size)
		return EMSGSIZE;

	/* Succeeded: the bytes are after what was there. */
	memcpy(out + *used, bytes, length);
	*used += length;
	out[*used] = '\0';
	return 0;
}

/*
 * Tells whether a line (length bytes) is an IdentityFile line (1) or not
 * (0): blanks, the keyword in any case, then blanks or one '=' with
 * blanks round it.  *value is where its value starts.
 */
static int
codespace_identity_line(
	const char *line,
	size_t length,
	size_t *value)
{
	const char *keyword;
	size_t keyword_length;
	size_t index;
	size_t at;
	int blank;
	char character;

	/* The blanks before the keyword. */
	index = 0U;
	for (;;) {
		/* A blank is passed over. */
		if (index == length)
			return 0;
		blank = codespace_blank(line[index]);
		if (!blank)
			break;
		index++;
	}

	/* The keyword, its ASCII letters in any case. */
	keyword = CODESPACE_IDENTITY_KEYWORD;
	keyword_length = sizeof(CODESPACE_IDENTITY_KEYWORD) - 1U;
	if (length - index <= keyword_length)
		return 0;
	for (at = 0U; at < keyword_length; at++) {
		/* The character, small. */
		character = line[index + at];
		if (character >= 'A' && character <= 'Z')
			character = (char)(character - 'A' + 'a');
		if (keyword[at] >= 'A' && keyword[at] <= 'Z') {
			if (character != (char)(keyword[at] - 'A' + 'a'))
				return 0;
		} else if (character != keyword[at]) {
			return 0;
		}
	}
	index += keyword_length;

	/* A blank or '=' must end the keyword. */
	blank = codespace_blank(line[index]);
	if (!blank && line[index] != '=')
		return 0;

	/* Blanks, an '=', and blanks again before the value. */
	for (;;) {
		/* A blank, or the one '=', is passed over. */
		if (index == length)
			break;
		blank = codespace_blank(line[index]);
		if (!blank && line[index] != '=')
			break;
		index++;
	}

	/* An IdentityFile line. */
	*value = index;
	return 1;
}

/* Tells whether a character is a blank of a settings line (1) or not (0). */
static int
codespace_blank(
	char character)
{
	/* A space or a tab. */
	if (character == ' ' || character == '\t')
		return 1;

	/* Any other. */
	return 0;
}

/* Finds where a line's value ends (length bytes, the value from value): before the blanks and the CR at its end. */
static size_t
codespace_value_end(
	const char *line,
	size_t value,
	size_t length)
{
	size_t end;
	int blank;

	/* Back over the CR and the blanks. */
	end = length;
	while (end > value) {
		/* A CR or a blank at the end is not the value's. */
		blank = codespace_blank(line[end - 1U]);
		if (!blank && line[end - 1U] != '\r')
			break;
		end--;
	}

	/* The value's end. */
	return end;
}
