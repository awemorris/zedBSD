/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The xdg_toplevel requests a toolkit makes of the window manager, and the
 * xdg_wm_base ping (ws035-p076, toplevel.c).
 */

#ifndef KWL_TOPLEVEL_H
#define KWL_TOPLEVEL_H

#include "kwl.h"

/*
 * The edges a resize drags: xdg_toplevel.resize_edge is made of these bits,
 * one side or two neighbouring sides (a corner).  The glass look's window
 * frames (shell.c) and the cursor they show (cursor.c) use the same bits.
 */
#define KWL_EDGE_TOP		1U
#define KWL_EDGE_BOTTOM		2U
#define KWL_EDGE_LEFT		4U
#define KWL_EDGE_RIGHT		8U

int kwl_toplevel_request(struct kwl_object *toplevel, struct kwl_object *surface, uint32_t opcode, const unsigned char *bytes, size_t size);
unsigned kwl_toplevel_resizing(const struct kwl_server *server, const struct kwl_object *surface);
void kwl_toplevel_committed(struct kwl_server *server, struct kwl_object *surface);
void kwl_toplevel_surface_gone(struct kwl_object *surface);
int kwl_toplevel_motion(struct kwl_server *server);
int kwl_toplevel_button(struct kwl_server *server, uint32_t state);
int kwl_toplevel_resize_start(struct kwl_server *server, struct kwl_object *surface, uint32_t edges);
void kwl_ping_send(struct kwl_client *client);
void kwl_ping_pong(struct kwl_client *client, uint32_t serial);

#endif
