/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	char *copy, *base;
	size_t length, suffix_length;
	int status = 0;

	if (argc > 1 && strcmp(argv[1], "--") == 0) {
		argc--;
		argv++;
	}
	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: basename string [suffix]\n");
		return 1;
	}
	copy = strdup(argv[1]);
	if (copy == NULL) {
		fprintf(stderr, "basename: out of memory\n");
		return 1;
	}
	length = strlen(copy);
	while (length > 1 && copy[length - 1U] == '/')
		copy[--length] = '\0';
	base = strrchr(copy, '/');
	if (base == NULL)
		base = copy;
	else if (base[1] != '\0')
		base++;
	length = strlen(base);
	if (argc == 3 && (suffix_length = strlen(argv[2])) != 0 &&
	    suffix_length < length &&
	    !memcmp(base + length - suffix_length, argv[2], suffix_length))
		base[length - suffix_length] = '\0';
	if (printf("%s\n", base) < 0 || fflush(stdout) == EOF)
		status = 1;
	free(copy);
	return status;
}
