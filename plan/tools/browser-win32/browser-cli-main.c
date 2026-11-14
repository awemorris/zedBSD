/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A test's console program over the browser's own entry, for comparing
 * the headless modes (--js, --dump=layout) of the Windows build with the
 * host's: wmain turns the command line into UTF-8, the standard output and
 * error are binary (no CR is added before a LF), and browser_main runs.
 * It is not shipped (OmniGrammaton.exe is the program that is).
 */

#include "userland/desktop/browser/browser-main.h"

#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

int
wmain(
	int count,
	wchar_t **wide)
{
	char **arguments;
	int index;
	int bytes;
	int status;

	/* Bytes as they are on the standard output and error. */
	(void)_setmode(_fileno(stdout), _O_BINARY);
	(void)_setmode(_fileno(stderr), _O_BINARY);

	/* The words in UTF-8. */
	arguments = calloc((size_t)count + 1U, sizeof(*arguments));
	if (arguments == NULL)
		return 1;
	for (index = 0; index < count; index++) {
		bytes = WideCharToMultiByte(CP_UTF8, 0U, wide[index], -1, NULL, 0, NULL, NULL);
		arguments[index] = calloc((size_t)bytes + 1U, 1U);
		if (arguments[index] == NULL)
			return 1;
		(void)WideCharToMultiByte(CP_UTF8, 0U, wide[index], -1, arguments[index], bytes, NULL, NULL);
	}

	/* The browser's entry. */
	status = browser_main(count, arguments);
	(void)fflush(stdout);
	(void)fflush(stderr);
	return status;
}
