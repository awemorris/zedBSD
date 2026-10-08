/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clipboard between clients and drag and drop: wl_data_device_manager,
 * wl_data_source, wl_data_device and wl_data_offer (ws035-p079, ws035-p084,
 * data.c).
 */

#ifndef KWL_DATA_H
#define KWL_DATA_H

#include "kwl.h"

int kwl_data_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_data_focus(struct kwl_server *server, struct kwl_object *focus);
void kwl_data_object_gone(struct kwl_object *object);
void kwl_data_drag_motion(struct kwl_server *server, uint32_t time);
void kwl_data_drag_release(struct kwl_server *server);
void kwl_data_drag_cancel(struct kwl_server *server);
void kwl_data_drag_mark(struct kwl_server *server);
void kwl_data_tick(struct kwl_server *server);
int kwl_data_emit_string(struct kwl_client *client, uint32_t id, uint32_t opcode, const char *text, int descriptor);
int kwl_data_read_string(const unsigned char *bytes, size_t size, size_t offset, const char **text, size_t *next);

/* The clipboard's history (clipboard.c, ws102-p018): a source asked for a type, and the compositor's own offered text made the selection (data.c). */
int kwl_data_send(struct kwl_object *source, const char *type, int descriptor);
void kwl_data_select_offered(struct kwl_server *server);

/* The history (clipboard.c): a new selection heard, the reading of its pipe, the compositor's text written to a reader, the list logged. */
void kwl_clipboard_selected(struct kwl_server *server, struct kwl_object *source);
void kwl_clipboard_poll(struct kwl_server *server);
void kwl_clipboard_offer_write(int descriptor);
void kwl_clipboard_history_log(struct kwl_server *server);

/* The primary selection: zwp_primary_selection_device_manager_v1 and its objects (ws035-p100, primary.c). */
int kwl_primary_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_primary_focus(struct kwl_server *server, struct kwl_object *focus);
void kwl_primary_object_gone(struct kwl_object *object);

#endif
