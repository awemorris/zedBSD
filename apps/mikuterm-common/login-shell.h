/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The launch screen's Login Shell (MIKUTERM_HAVE_LOGIN_SHELL): which
 * program is the user's login shell, and that shell started as a login
 * shell on a terminal of its own, which is the tab's connection from then
 * on (connection-ssh.c hands the tab's keys, size and end to it).
 *
 * The POSIX systems' is apps/terminal-linux/login-shell-posix.c.
 */

#ifndef MIKUTERM_LOGIN_SHELL_H
#define MIKUTERM_LOGIN_SHELL_H

#include <stddef.h>

struct kl_app;
struct kl_process;

int login_shell_find(char *path, size_t size);
int login_shell_start(struct kl_app *app, const char *path, const char *term, unsigned columns, unsigned rows, struct kl_process **process);

#endif
