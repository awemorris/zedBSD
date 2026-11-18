/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * xdg_positioner and xdg_popup (ws035-p076): popups placed by a positioner
 * relative to their parent, drawn over the windows, and the seat's grab a
 * popup menu takes (popup.c).
 */

#ifndef KWL_POPUP_H
#define KWL_POPUP_H

#include "compose.h"

/*
 * The rules of one xdg_positioner: the popup's size, the anchor rectangle
 * in the parent's window geometry, the anchor edge, the gravity, the
 * adjustments allowed when the popup would leave the output, and the offset.
 * A positioner is complete once its size and its anchor rectangle are set.
 */
struct kwl_positioner {
	int32_t width;
	int32_t height;
	int32_t anchor_x;
	int32_t anchor_y;
	int32_t anchor_width;
	int32_t anchor_height;
	uint32_t anchor;
	uint32_t gravity;
	uint32_t constraint;
	int32_t offset_x;
	int32_t offset_y;
	unsigned reactive;
	unsigned size_set;
	unsigned anchor_set;
};

int kwl_positioner_create(struct kwl_object *wm, uint32_t id);
int kwl_popup_create(struct kwl_object *role, uint32_t id, uint32_t parent_id, uint32_t positioner_id);
int kwl_popup_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
int kwl_popup_send_configure(struct kwl_object *surface);
void kwl_popup_mapped(struct kwl_server *server, struct kwl_object *surface);
void kwl_popup_object_gone(struct kwl_object *object);
int kwl_popup_origin(struct kwl_server *server, struct kwl_object *surface, int32_t *x, int32_t *y);
unsigned kwl_popup_collect(struct kwl_server *server, struct kwl_object **popups, unsigned capacity);
void kwl_popup_draw(struct kwl_server *server, struct kwl_list *command);
struct kwl_object *kwl_popup_focus(struct kwl_server *server, struct kwl_object *target);
struct kwl_object *kwl_popup_chain_at(struct kwl_server *server);
struct kwl_object *kwl_popup_root(struct kwl_object *surface);
int kwl_popup_button(struct kwl_server *server, uint32_t button, uint32_t state);

/* What popup.c needs from the compositor and the shell. */
void kwl_compose_surface_quad(struct kwl_server *server, struct kwl_list *command, const struct kwl_object *surface, const struct kwl_import *import, int32_t x, int32_t y);
int kwl_glass_body_origin(struct kwl_server *server, struct kwl_object *surface, int32_t *x, int32_t *y);
struct kwl_object *kwl_glass_body_at(struct kwl_server *server, int32_t x, int32_t y);
void kwl_seat_pointer_move(struct kwl_server *server, struct kwl_object *from, struct kwl_object *to);
void kwl_seat_pointer_update(struct kwl_server *server);

#endif
