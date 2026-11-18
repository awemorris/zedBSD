/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system's side of keiland-preview's children (each system's
 * spawn.c), for the callers' side (client.c) alone: a child started with
 * the input and the output, looked at without waiting, and ended.  The
 * record of a child (struct preview_child) is the system's; the callers'
 * programs never see it open.
 */

#ifndef KEILAND_PREVIEW_SPAWN_H
#define KEILAND_PREVIEW_SPAWN_H

#include "client.h"

/*
 * Starts a child making a preview of the input into the output (both
 * descriptors stay the caller's).  Returns 0 with *child (NULL when the
 * preview was made in this process, with *status), or an errno value.
 */
int preview_spawn(int input, int output, const struct preview_request *request, struct preview_child **child, int *status);

/*
 * Looks at a child without waiting: 1 when it ended (its record is freed,
 * *status is its exit status or PREVIEW_KILLED plus the signal), 0 while
 * it runs.
 */
int preview_child_ended(struct preview_child *child, int *status);

/*
 * Ends a child that runs and collects it (its record is freed).  Returns
 * its status: PREVIEW_KILLED plus the signal that ended it.
 */
int preview_child_end(struct preview_child *child);

#endif
