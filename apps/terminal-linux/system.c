/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the Windows program asks Windows of WSL (wsl.c), answered on
 * Linux, where there is no WSL: wsl.exe is never there, no address is
 * Windows' own, and nothing of WSL listens on a port.  The launch screen
 * of the Linux program offers no WSL row (MIKUTERM_LINUX), so these are
 * reached only by a host of ssh/config named as a distribution.
 */

#include "mikuterm.h"

#include <errno.h>
#include <stdio.h>

/*
 * Runs wsl.exe with the arguments: there is none on Linux.  Returns
 * ENOENT, with nothing written and no exit code.
 */
int
mikuterm_wsl_run(
	const char *arguments,
	unsigned milliseconds,
	int (*stop)(void *data),
	void *data,
	unsigned char *output,
	size_t capacity,
	size_t *size,
	unsigned long *code)
{
	(void)arguments;
	(void)milliseconds;
	(void)stop;
	(void)data;
	(void)output;
	(void)capacity;

	/* Nothing ran. */
	*size = 0U;
	*code = 0UL;
	return ENOENT;
}

/* Tells whether an address is one of Windows' own: none is on Linux (0). */
int
mikuterm_windows_address(
	const char *address)
{
	(void)address;

	/* No address is Windows'. */
	return 0;
}

/* Tells what listens on a port of localhost for WSL: nothing on Linux. */
unsigned
mikuterm_port_listener(
	unsigned port,
	char *name,
	size_t size)
{
	(void)port;

	/* No name, and nothing of WSL's. */
	if (size != 0U)
		name[0] = '\0';
	return MIKUTERM_LISTENER_NONE;
}
