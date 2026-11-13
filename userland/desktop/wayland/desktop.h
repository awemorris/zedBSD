/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop surface (kl_desktop_v1, desktop.c, ws094-p002): one
 * client's surface over the wallpaper and under every window, which shows
 * the icons of ~/Desktop; shared with the drawing, the input and the
 * display pass.
 */

#ifndef KWL_DESKTOP_H
#define KWL_DESKTOP_H

#include "glass.h"

int kwl_desktop_option(struct kwl_server *server, const char *argument);
int kwl_desktop_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_desktop_object_gone(struct kwl_object *object);
void kwl_desktop_tick(struct kwl_server *server);
int kwl_desktop_child_ended(struct kwl_server *server, long child);
void kwl_desktop_draw(struct kwl_server *server, struct kwl_list *command);
void kwl_desktop_commit(struct kwl_object *surface);
struct kwl_object *kwl_desktop_surface(struct kwl_server *server);
int kwl_desktop_is(const struct kwl_object *surface);
struct kwl_object *kwl_desktop_front(struct kwl_server *server, struct kwl_object *top);
int kwl_desktop_press(struct kwl_server *server);
void kwl_desktop_unfocus(struct kwl_server *server);
struct kwl_object *kwl_desktop_at(struct kwl_server *server, int32_t x, int32_t y);

#endif
