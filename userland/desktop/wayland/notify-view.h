/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the notifications' popup (notify-popup.c) and the log's board
 * (notify-log.c, ws156-p004) share: the board's place and size, a
 * notification's card and the bare glass, and the log's board's part of
 * the popup's tick, drawing and pointer.
 */

#ifndef KWL_NOTIFY_VIEW_H
#define KWL_NOTIFY_VIEW_H

#include "draw-list.h"
#include "kwl.h"
#include "notify.h"

#include <vulkan/vulkan.h>

/* A board's height in pixels. */
#define KWL_NOTIFY_BOARD_HEIGHT	76

int32_t kwl_notify_board_width(const struct kwl_server *server);
int32_t kwl_notify_board_top(const struct kwl_server *server);
void kwl_notify_draw_card(struct kwl_server *server, struct kwl_list *command, const struct kwl_notification *item, int32_t left, int32_t top, int32_t width, float opacity);
void kwl_notify_draw_glass(struct kwl_server *server, struct kwl_list *command, int32_t left, int32_t top, int32_t width, float opacity);
int kwl_notify_card_close_at(int32_t left, int32_t top, int32_t width, int32_t x, int32_t y);

void kwl_notify_log_tick(struct kwl_server *server);
void kwl_notify_log_draw(struct kwl_server *server, struct kwl_list *command);
int kwl_notify_log_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_notify_log_showing(void);

#endif
