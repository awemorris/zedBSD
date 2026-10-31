/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A small program over libbrowser's script core and its operating-system
 * layer, for a check under Wine: "script-tool FILE" runs a JavaScript file
 * with print, as the browser's --js does; "script-tool random" prints 16
 * bytes from the system's random generator the engine's
 * crypto.getRandomValues reads, in hexadecimal (the script core alone has
 * no crypto object).
 */

#include "base/os-system.h"

#include <browser/browser.h>

#include <stdio.h>
#include <string.h>

int
main(
	int argc,
	char **argv)
{
	unsigned char bytes[16];
	unsigned index;
	int differs;
	int error;

	/* One argument: a file, or "random". */
	if (argc != 2) {
		fprintf(stderr, "usage: script-tool FILE | random\n");
		return 2;
	}

	/* The random bytes. */
	differs = strcmp(argv[1], "random");
	if (differs == 0) {
		error = wb_os_random(bytes, sizeof(bytes));
		if (error != 0) {
			fprintf(stderr, "script-tool: random: error %d\n", error);
			return 1;
		}

		/* Each byte in hexadecimal. */
		for (index = 0; index < sizeof(bytes); index++)
			printf("%02x", bytes[index]);
		printf("\n");
		return 0;
	}

	/* The script, run in a realm of its own. */
	return browser_script_tool(BROWSER_SCRIPT_RUN, argv[1], 0, __builtin_frame_address(0));
}
