/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pen tablets (tablet.c, WS079 p003): zwp_tablet_manager_v2 version 1
 * and the pen as the pointer for clients and screens that do not take it.
 */

#ifndef KWL_TABLET_H
#define KWL_TABLET_H

#include "kwl.h"

/* The slot a tablet or tool object names once its device or tool has gone. */
#define KWL_TABLET_SLOT_NONE	0xffffffffU

int kwl_tablet_add(struct kwl_server *server, struct kwl_input_device *device);
void kwl_tablet_remove(struct kwl_server *server, struct kwl_input_device *device, int notify);
void kwl_tablet_cancel(struct kwl_server *server);
void kwl_tablet_frame(struct kwl_server *server, struct kwl_input_device *device, uint32_t time);
int kwl_tablet_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_tablet_object_gone(struct kwl_object *object);

#endif
