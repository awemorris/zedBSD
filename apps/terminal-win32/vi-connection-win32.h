/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Embedded vi child-process connection for mikuterm.
 */

#ifndef APPS_TERMINAL_WIN32_VI_CONNECTION_WIN32_H
#define APPS_TERMINAL_WIN32_VI_CONNECTION_WIN32_H

#include "userland/desktop/terminal/connection.h"

#include <stddef.h>

/*
 * Opens a file in vi.exe --mikuterm over Win32 anonymous pipes and returns
 * a terminal_connection for a new terminal grid tab.
 *
 * When path is NULL, displays a modal Windows file-open dialog first.
 * Returns 0 on success, ECANCELED when the dialog is cancelled, or an errno
 * value.
 */
int mikuterm_vi_open(
	struct kl_app *app,
	struct terminal_window *window,
	unsigned columns,
	unsigned rows,
	int ambiguous_wide,
	const char *path,
	char *title,
	size_t title_size,
	struct terminal_connection **connection);

#endif
