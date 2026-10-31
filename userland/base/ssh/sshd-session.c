/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The session process of sshd: the connection protocol as the user.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "sshd.h"

/*
 * Runs the session process (sshd started with SSHD_EXEC_SESSION): reads
 * the settings, the user and the exported transport from the monitor,
 * takes the user's ids, imports the transport and runs the connection
 * protocol.
 */
void
sshd_session_run(void)
{
}

