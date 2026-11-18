/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The removable media (media.c, ws132-p004): the volumes libkeiland-backend
 * follows (volumed on zedBSD), for the system extension's devices objects
 * (system.c), and the notification of a volume that comes not mounted
 * (WS156 H7, which replaced the bar's media icon of ws132-p004): its click
 * starts Files on its devices.  The bar's icon functions remain and draw
 * nothing.
 */

#ifndef KWL_MEDIA_H
#define KWL_MEDIA_H

#include "draw-list.h"
#include "kwl.h"

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <vulkan/vulkan.h>

/* What kwl_media_tick found changed (the backend's KL_BACKEND_VOLUMES_CHANGED_* bits). */
unsigned kwl_media_tick(struct kwl_server *server);
void kwl_media_close(void);
size_t kwl_media_volumes(struct kl_backend_volume *list, size_t capacity);
int kwl_media_ask(int mount, const char *id, uint32_t *request);
int kwl_media_take_result(uint32_t *request, int *error, char *user, size_t size);
int32_t kwl_media_width(void);
void kwl_media_draw_icon(struct kwl_server *server, struct kwl_list *command, int32_t x, const float *ink);
int kwl_media_button(struct kwl_server *server, uint32_t button, uint32_t state);

#endif
