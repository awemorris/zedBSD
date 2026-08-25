/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	char *copy, *end;
	size_t length;
	int status = 0;

	if (argc > 1 && strcmp(argv[1], "--") == 0) {
		argc--;
		argv++;
	}
	if (argc != 2) {
		fprintf(stderr, "usage: dirname string\n");
		return 1;
	}
	copy = strdup(argv[1]);
	if (copy == NULL) {
		fprintf(stderr, "dirname: out of memory\n");
		return 1;
	}
	length = strlen(copy);
	while (length != 0 && copy[length - 1U] == '/')
		copy[--length] = '\0';
	if (length == 0) {
		strcpy(copy, argv[1][0] == '/' ? "/" : ".");
	} else if ((end = strrchr(copy, '/')) == NULL) {
		strcpy(copy, ".");
	} else {
		while (end > copy && end[-1] == '/')
			end--;
		if (end == copy)
			end++;
		*end = '\0';
	}
	if (printf("%s\n", copy) < 0 || fflush(stdout) == EOF)
		status = 1;
	free(copy);
	return status;
}
