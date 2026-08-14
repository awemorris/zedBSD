/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/init.h"
#include "kern/exec.h"

#include <stddef.h>

#ifndef ZEDBSD_INIT_PATH
#define ZEDBSD_INIT_PATH "/bin/sh"
#endif

int
kern_init_start(void)
{
	return process_spawn_init(ZEDBSD_INIT_PATH, NULL);
}
