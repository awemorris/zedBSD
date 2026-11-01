/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Linked into the Windows build of the host tests (plan/ws334/tests/host-tests-windows.sh): the C
 * runtime reads and writes the standard streams and every file in binary mode, so that a test writes the same
 * bytes (no CR before a LF) and reads the same bytes as on the host.
 */

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

/* Every file the program opens afterwards. */
int _CRT_fmode = _O_BINARY;

/* The standard streams, which the runtime has opened before this runs. */
static void __attribute__((constructor))
binary_streams(void)
{
	(void)_setmode(_fileno(stdin), _O_BINARY);
	(void)_setmode(_fileno(stdout), _O_BINARY);
	(void)_setmode(_fileno(stderr), _O_BINARY);
}
