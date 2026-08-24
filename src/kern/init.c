/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/init.h"
#include "kern/exec.h"

#include <stddef.h>

#ifndef ZEDBSD_INIT_PATH
#define ZEDBSD_INIT_PATH "/sbin/init"
#endif

int
kern_init_start(void)
{
	int error = process_spawn_init(ZEDBSD_INIT_PATH, NULL);

	/* A missing or corrupt init must remain diagnosable from the console.
	 */
	if (error != 0)
		return process_spawn_init("/bin/sh", NULL);
	return 0;
}
