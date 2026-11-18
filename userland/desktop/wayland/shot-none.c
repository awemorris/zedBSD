/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor without the screen capture (ws173-p002): every image but
 * a test image built with ZEDBSD_TEST_SCREEN_CAPTURE=y links this file in
 * place of shot.c, so the compositor opens no capture socket and copies
 * no frame.
 */

#include "compose.h"

/* Tells whether this compositor was built with the capture (it was not). */
int
kwl_shot_enabled(
	void)
{
	return 0;
}

/* Has no capture to wait for. */
int
kwl_shot_waiting(
	void)
{
	return 0;
}

/* Takes no test option: --rotate-policy is a test image's alone (WS217). */
int
kwl_shot_option(
	struct kwl_server *server,
	const char *argument)
{
	(void)server;
	(void)argument;
	return 0;
}

/* No socket. */
void
kwl_shot_open(
	struct kwl_server *server)
{
	(void)server;
}

/* Nothing to close. */
void
kwl_shot_close(
	struct kwl_server *server)
{
	(void)server;
}

/* Nothing to serve. */
void
kwl_shot_tick(
	struct kwl_server *server)
{
	(void)server;
}

/* No copy. */
void
kwl_shot_record(
	struct kwl_server *server,
	struct kwl_list *command,
	VkImage image)
{
	(void)server;
	(void)command;
	(void)image;
}

/* Nothing to send. */
void
kwl_shot_complete(
	struct kwl_server *server)
{
	(void)server;
}
