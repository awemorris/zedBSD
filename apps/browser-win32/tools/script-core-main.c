/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A program over libbrowser's script core alone (libbrowser-script.a: the
 * base library, the virtual machine, JavaScript and the operating-system
 * layer), linked with nothing else of libbrowser, so that it links only
 * when the core needs nothing of the engine.  "script-core --js FILE" (or
 * "script-core FILE") runs a JavaScript file with print, as the browser's
 * --js does.  On Windows standard output and standard error are put in
 * binary mode first, so that the program writes the same bytes as on a
 * POSIX system.
 */

#include <browser/browser.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int
main(
	int argc,
	char **argv)
{
	const char *path;
	int differs;

#ifdef _WIN32
	/* The bytes as written, without CR before each LF. */
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);
#endif

	/* "--js FILE", as the test runners call the browser, or the file alone. */
	path = NULL;
	if (argc == 2) {
		path = argv[1];
	} else if (argc == 3) {
		differs = strcmp(argv[1], "--js");
		if (differs == 0)
			path = argv[2];
	}

	/* The file is required. */
	if (path == NULL) {
		fprintf(stderr, "usage: script-core [--js] FILE\n");
		return 2;
	}

	/* The script, run in a realm of its own. */
	return browser_script_tool(BROWSER_SCRIPT_RUN, path, 0, __builtin_frame_address(0));
}
