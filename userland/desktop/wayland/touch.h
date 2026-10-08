/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Touch screens (touch.c, WS079 p013): wl_touch for the clients and the
 * compositor's own gestures of the fingers.
 */

#ifndef KWL_TOUCH_H
#define KWL_TOUCH_H

#include "kwl.h"

int kwl_touch_add(struct kwl_input_device *device);
void kwl_touch_remove(struct kwl_server *server, struct kwl_input_device *device, int notify);
void kwl_touch_frame(struct kwl_server *server, struct kwl_input_device *device, uint32_t time);
void kwl_touch_tick(struct kwl_server *server);
void kwl_touch_object_gone(struct kwl_object *object);
int kwl_touch_drag_start(struct kwl_server *server, struct kwl_client *client, uint32_t serial);
int kwl_touch_shell_handback(struct kwl_server *server, int32_t x, int32_t y, uint32_t time, int lifted);

#endif
