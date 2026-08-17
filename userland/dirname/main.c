/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	char *copy, *slash;
	size_t length;
	if (argc != 2) {
		fprintf(stderr, "usage: dirname string\n");
		return 1;
	}
	copy = strdup(argv[1][0] != '\0' ? argv[1] : ".");
	if (copy == NULL) {
		fprintf(stderr, "dirname: out of memory\n");
		return 1;
	}
	length = strlen(copy);
	while (length > 1 && copy[length - 1U] == '/')
		copy[--length] = '\0';
	slash = strrchr(copy, '/');
	if (slash == NULL) {
		strcpy(copy, ".");
	} else {
		while (slash > copy && slash[-1] == '/')
			slash--;
		if (slash == copy)
			copy[1] = '\0';
		else
			*slash = '\0';
	}
	puts(copy);
	free(copy);
	return ferror(stdout) ? 1 : 0;
}
