/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A tab's connection for trying the terminal without a network: what the user
 * types is shown back.
 */

#ifndef MIKUTERM_ANDROID_ECHO_CONNECTION_H
#define MIKUTERM_ANDROID_ECHO_CONNECTION_H

#include "userland/desktop/terminal/connection.h"

/*
 * Opens a tab's connection (the type of terminal_connector's open).  It starts
 * with a red bar of eight blanks and the line "echo ready", and shows back
 * every key typed, a Return as a new line and a Backspace as erasing.
 */
int echo_connector_open(void *data, struct kl_app *app, uint32_t id, unsigned columns, unsigned rows,
	unsigned width, unsigned height, char *title, size_t size, struct terminal_connection **connection);

#endif /* MIKUTERM_ANDROID_ECHO_CONNECTION_H */
