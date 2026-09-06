/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Starting the first user process.
 */

#include "kern/init.h"
#include "kern/exec.h"

#include <stddef.h>

/*
 * Spawns the init process from the given executable path.
 */
int
kern_init_start(
	const char *path)
{
	int error;

	/* Spawns init without an inherited argument vector. */
	error = process_spawn_init(path, NULL);

	/* Reports the spawn result. */
	return error;
}
