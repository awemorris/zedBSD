/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * wl_subcompositor and wl_subsurface (ws035-p077, subsurface.c): surfaces
 * shown with a parent surface, at a place relative to it, below or above it.
 */

#ifndef KWL_SUBSURFACE_H
#define KWL_SUBSURFACE_H

#include "compose.h"

int kwl_subcompositor_request(struct kwl_object *subcompositor, uint32_t opcode, const unsigned char *bytes, size_t size);
int kwl_subsurface_request(struct kwl_object *subsurface, uint32_t opcode, const unsigned char *bytes, size_t size);
int kwl_subsurface_commit(struct kwl_object *surface);
void kwl_subsurface_applied(struct kwl_object *surface);
void kwl_subsurface_object_gone(struct kwl_object *object);
void kwl_subsurface_draw(struct kwl_server *server, struct kwl_list *command, struct kwl_object *parent, float x, float y, float scale_x, float scale_y, unsigned above);
unsigned kwl_subsurface_collect(struct kwl_server *server, struct kwl_object **surfaces, unsigned capacity);
struct kwl_object *kwl_subsurface_at(struct kwl_object *root, int32_t x, int32_t y);

/* What subsurface.c needs from protocol.c: a commit of a surface without a shell role, applied now. */
int kwl_surface_queue(struct kwl_object *surface);

#endif
