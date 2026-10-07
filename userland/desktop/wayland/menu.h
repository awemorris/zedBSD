/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Menu (WS070, plan/ws070/design.md): the menu models clients
 * give with xdg_menu_v1 (menu.c), and the menus the compositor draws and operates
 * in the title bars and the system bar (menu-shell.c).
 */

#ifndef KWL_MENU_H
#define KWL_MENU_H

#include "glass.h"

/* The item types of xdg_menu_v1. */
#define KWL_MENU_NORMAL		0U
#define KWL_MENU_SEPARATOR	1U
#define KWL_MENU_CHECKBOX	2U
#define KWL_MENU_RADIO		3U
#define KWL_MENU_SUBMENU	4U

/* The modifier bits of a shortcut (xdg_menu_v1.modifier). */
#define KWL_MENU_SHIFT		1U
#define KWL_MENU_CTRL		2U
#define KWL_MENU_ALT		4U
#define KWL_MENU_SUPER		8U

/* The parent of the top-level items. */
#define KWL_MENU_ROOT		0U

/*
 * One item of a menu model, as the client last committed it.
 *
 * The label and the icon name are owned by the item (never NULL, possibly
 * empty).  A child follows its parent's other children in the model's
 * array order.
 */
struct kwl_menu_item {
	uint32_t id;
	uint32_t parent;
	uint32_t type;
	uint32_t action;
	uint32_t enabled;
	uint32_t visible;
	uint32_t checked;
	uint32_t role;
	uint32_t modifiers;
	uint32_t keysym;
	char *label;
	char *icon_name;
};

/*
 * The model of one xdg_menu_v1 object.
 *
 * items is what is shown.  Between begin_update and commit the client's
 * changes go to pending, a deep copy made at begin_update; commit makes it
 * the shown model at once and moves generation on.  The model lives as
 * long as its xdg_menu_v1 object.
 */
struct kwl_menu_model {
	struct kwl_menu_item *items;
	unsigned count;
	unsigned capacity;
	struct kwl_menu_item *pending;
	unsigned pending_count;
	unsigned pending_capacity;
	unsigned updating;
	uint32_t update_serial;
	uint64_t generation;
};

/*
 * Where a window's top-level items go in a title bar or the system bar: the
 * first item's left edge, the bar's top and height, the right edge the items
 * must end before, and the bar's left edge (the log gives the items'
 * places from it).
 */
struct kwl_menu_area {
	int32_t x;
	int32_t top;
	int32_t right;
	int32_t height;
	int32_t origin;
};

/* The model and the protocol (menu.c). */
int kwl_menu_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_menu_object_gone(struct kwl_object *object);
struct kwl_menu_model *kwl_menu_of_surface(struct kwl_object *surface, struct kwl_object **place);
const struct kwl_menu_item *kwl_menu_item(const struct kwl_menu_model *model, uint32_t id);
unsigned kwl_menu_children(const struct kwl_menu_model *model, uint32_t parent, const struct kwl_menu_item **children, unsigned capacity);
void kwl_menu_send_activated(struct kwl_object *place, const struct kwl_menu_item *item, const char *via);
void kwl_menu_send_popup(struct kwl_object *place, uint32_t item, unsigned opened);
void kwl_menu_send_context_activated(struct kwl_object *context, const struct kwl_menu_item *item, const char *via);
void kwl_menu_send_context_done(struct kwl_object *context);
int kwl_menu_open_context(struct kwl_server *server, struct kwl_object *context, struct kwl_object *surface, int32_t x, int32_t y);
int kwl_menu_is_open(void);

/* The menus the compositor draws and operates (menu-shell.c). */
void kwl_menu_frame(struct kwl_server *server);
int32_t kwl_menu_title_limit(struct kwl_server *server, struct kwl_object *surface, int32_t available);
void kwl_menu_draw_bar(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface, unsigned docked, const struct kwl_menu_area *area, const float *ink, float fade);
void kwl_menu_draw_popups(struct kwl_server *server, VkCommandBuffer command);
int kwl_menu_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_menu_motion(struct kwl_server *server);
int kwl_menu_grab_key(struct kwl_server *server, uint32_t key, uint32_t state);
int kwl_menu_key(struct kwl_server *server, uint32_t key, uint32_t state);
void kwl_menu_tick(struct kwl_server *server);
void kwl_menu_forget(struct kwl_server *server, struct kwl_object *object);
void kwl_menu_add_overflow(struct kwl_object *surface, unsigned docked, const struct kwl_menu_area *area, int32_t x, int32_t width, const uint32_t *ids, const char *const *labels, unsigned count);
int kwl_menu_keysym(uint32_t key, uint32_t seat_modifiers, uint32_t *keysym);

/* The IDs of the rows "..." holds for a titlebar's hidden controls: the control's ID with these bits. */
#define KWL_MENU_EXTRA_BASE	0xf0000000U

/* The glass look's shell, for the menus (shell.c). */
void kwl_glass_raise(struct kwl_server *server, struct kwl_object *surface);
struct kwl_object *kwl_glass_window_at(struct kwl_server *server, int32_t x, int32_t y);

/* A press on a menu item or a search field that moves its window when it goes far enough (shell.c, ws099-p030). */
int kwl_glass_press_moved(struct kwl_server *server, int32_t x, int32_t y, enum kwl_contact_source source);
void kwl_glass_press_move(struct kwl_server *server, struct kwl_object *surface, unsigned docked, int32_t x, int32_t y);

#endif
