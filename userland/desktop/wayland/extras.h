/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The protocols toolkits use besides the core and xdg-shell (ws035-p080):
 * xdg-decoration (decoration.c), cursor-shape (cursor.c) and viewporter
 * (viewport.c).
 */

#ifndef KWL_EXTRAS_H
#define KWL_EXTRAS_H

#include "compose.h"

int kwl_decoration_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_decoration_object_gone(struct kwl_object *object);
int kwl_decoration_configure(struct kwl_object *surface, uint32_t serial);
int kwl_decoration_ack(struct kwl_object *surface, uint32_t serial);
void kwl_decoration_commit(struct kwl_object *surface);
int kwl_decoration_server(const struct kwl_object *surface);
int kwl_decoration_native_changed(struct kwl_object *toplevel);
int kwl_decoration_kde_bind(struct kwl_object *manager);
void kwl_decoration_geometry(const struct kwl_object *surface, uint32_t *width, uint32_t *height);

int kwl_cursor_shape_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_cursor_shape_object_gone(struct kwl_object *object);
void kwl_cursor_images_destroy(struct kwl_server *server);
int kwl_cursor_images_create(struct kwl_server *server);
const struct kwl_import *kwl_cursor_image(const struct kwl_server *server, int32_t *hotspot_x, int32_t *hotspot_y);
void kwl_cursor_frame(struct kwl_server *server, uint32_t edges);

int kwl_viewport_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_viewport_commit(struct kwl_object *surface);
void kwl_viewport_object_gone(struct kwl_object *object);
void kwl_surface_size(const struct kwl_object *surface, uint32_t *width, uint32_t *height);
void kwl_viewport_source(const struct kwl_object *surface, float *uv);

int kwl_content_type_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_content_type_commit(struct kwl_object *surface);
void kwl_content_type_object_gone(struct kwl_object *object);

#endif
