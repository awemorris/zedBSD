/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Who may end the machine through sessiond (power-rules.c, ws131-p027): the
 * words a POWER request may say and the program each runs, and the rule of
 * a session's request.  Nothing here reads a file or starts a program, so
 * the host tests run it alone.
 */

#ifndef SESSIOND_POWER_RULES_H
#define SESSIOND_POWER_RULES_H

#include <sys/types.h>

/* The programs that end the machine (they ask init, which stops sessiond among the rest). */
#define SESSIOND_POWEROFF	"/sbin/poweroff"
#define SESSIOND_REBOOT		"/sbin/reboot"

const char *sessiond_power_program(const char *what);
int sessiond_power_decide(const char *what, uid_t uid, int in_wheel, const char **program);

#endif
