/*
 * zedBSD gettext messages-object host test
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <errno.h>
#include <libintl.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

static int host_errno;

int *
__libc_errno_location(void)
{
	return &host_errno;
}

int
main(int argc, char **argv)
{
	if (argc != 2 || setlocale(LC_ALL, "C.UTF-8") == NULL ||
	    bindtextdomain("messages", argv[1]) == NULL ||
	    strcmp(dgettext("messages", "hello"), "bonjour") != 0 ||
	    strcmp(dngettext("messages", "file", "files", 1), "fichier") != 0 ||
	    strcmp(dngettext("messages", "file", "files", 2), "fichiers") != 0 ||
	    strcmp(dgettext("messages", "absent"), "absent") != 0) {
		fprintf(stderr, "gettext catalog host test failed (errno=%d)\n",
		    errno);
		return 1;
	}
	puts("zedBSD gettext catalog host test: PASS");
	return 0;
}
