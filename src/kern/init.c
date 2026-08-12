/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/init.h"
#include "kern/exec.h"

#include <stddef.h>

int
kern_init_start(void)
{
	return process_spawn_init("/bin/sh", NULL);
}
