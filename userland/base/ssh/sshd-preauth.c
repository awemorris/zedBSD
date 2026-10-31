/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pre-authentication process of sshd: transport and user authentication without privileges.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "sshd.h"

/*
 * Runs the pre-authentication process (sshd started with
 * SSHD_EXEC_PREAUTH): reads the settings from the monitor, warms the
 * crypto up, confines itself, then runs the transport and the user
 * authentication.
 */
void
sshd_preauth_run(void)
{
}

