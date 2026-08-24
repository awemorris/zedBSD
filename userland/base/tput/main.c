/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/common/command.h"
#include "userland/base/common/terminfo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
parse_parameter(const char *text, long *result)
{
	char *end;
	long value;

	if (text == NULL || *text == '\0')
		return 0;
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || *end != '\0')
		return 0;
	*result = value;
	return 1;
}

int
main(int argc, char **argv)
{
	struct terminfo terminal;
	const struct terminfo_capability *capability;
	const char *type = NULL;
	const char *directory = getenv("TERMINFO");
	long parameters[9] = {0};
	char expanded[1024];
	int index = 1;
	int parameter;

	if (index < argc && strcmp(argv[index], "-T") == 0) {
		if (++index >= argc)
			goto usage;
		type = argv[index++];
	}
	if (index >= argc)
		goto usage;
	if (type == NULL)
		type = getenv("TERM");
	if (type == NULL || *type == '\0') {
		fprintf(stderr, "tput: TERM is not set\n");
		return 2;
	}
	if (terminfo_load(&terminal, type, directory) != 0) {
		fprintf(stderr, "tput: %s: unknown or invalid terminal\n",
			type);
		return 3;
	}
	if (strcmp(argv[index], "longname") == 0) {
		if (index + 1 != argc)
			goto usage;
		puts(terminal.name);
		return 0;
	}
	capability = terminfo_find(&terminal, argv[index++]);
	if (capability == NULL) {
		fprintf(stderr, "tput: unknown capability\n");
		return 4;
	}
	for (parameter = 0; index < argc && parameter < 9; parameter++, index++)
		if (!parse_parameter(argv[index], &parameters[parameter])) {
			fprintf(stderr, "tput: %s: invalid numeric parameter\n",
				argv[index]);
			return 2;
		}
	if (index != argc)
		goto usage;
	if (capability->kind == TERMINFO_BOOLEAN)
		return capability->number ? 0 : 1;
	if (capability->kind == TERMINFO_NUMBER) {
		printf("%ld\n", capability->number);
		return ferror(stdout) ? 1 : 0;
	}
	if (terminfo_expand(capability->string, parameters, expanded,
			    sizeof(expanded)) < 0) {
		fprintf(stderr, "tput: malformed capability expansion\n");
		return 4;
	}
	if (command_write_all(STDOUT_FILENO, expanded, strlen(expanded)) != 0) {
		fprintf(stderr, "tput: %s\n", strerror(errno));
		return 1;
	}
	return 0;

usage:
	fprintf(stderr,
		"usage: tput [-T terminal] capability [parameter ...]\n");
	return 2;
}
