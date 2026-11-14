/*
 * Standard output at the end of a program: writes a whole line and a part
 * of one, registers an atexit handler that writes more, then ends with
 * exit(3) ("exit") or by returning 4 from main ("return").  Everything,
 * "bug432 MODE line\ntail-handler", is expected on standard output whether
 * it is a file, a pipe or a terminal (C 7.22.4.4, POSIX exit: the handlers
 * run, then every open stream is flushed).
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
handler(void)
{
	printf("handler");
}

int
main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	if (atexit(handler) != 0)
		return 5;
	printf("bug432 %s line\n", argv[1]);
	printf("tail-");
	if (strcmp(argv[1], "exit") == 0)
		exit(3);
	return 4;
}
