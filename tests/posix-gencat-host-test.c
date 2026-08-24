/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <errno.h>
#include "libc/include/nl_types.h"
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
	nl_catd catalog;
	const char *fallback = "fallback";

	if (argc != 2)
		return 2;
	catalog = catopen(argv[1], NL_CAT_LOCALE);
	if (catalog == (nl_catd)-1) {
		perror("catopen");
		return 1;
	}
	if (strcmp(catgets(catalog, 2, 1, fallback), "hello") != 0 ||
	    strcmp(catgets(catalog, 2, 2, fallback), "line\nnext") != 0 ||
	    strcmp(catgets(catalog, 3, 1, fallback), "old") != 0)
		return 1;
	errno = 0;
	if (catgets(catalog, 9, 9, fallback) != fallback || errno != ENOMSG)
		return 1;
	return catclose(catalog) != 0;
}
