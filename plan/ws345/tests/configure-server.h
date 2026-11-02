/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The small Wayland server of configure-test.c (configure-server.c): one
 * toplevel, its configures and acknowledgements, and a check of every
 * frame committed against the size of the configure acknowledged last.
 */

#ifndef CONFIGURE_SERVER_H
#define CONFIGURE_SERVER_H

#include <stdint.h>

/* What the server saw so far. */
struct server_state {
	unsigned acks;
	uint32_t last_ack;
	unsigned bad_acks;
	unsigned commits;
	uint32_t commit_width;
	uint32_t commit_height;
	unsigned violations;
	char violation[200];
};

const char *server_start(uint32_t width, uint32_t height);
void server_stop(void);
uint32_t server_configure(uint32_t width, uint32_t height, int maximized);
void server_state(struct server_state *state);

#endif
