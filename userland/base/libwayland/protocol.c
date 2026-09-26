/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Describes and marshals the selected Wayland wire interfaces. */

#include "internal.h"

/* Identifies object arguments in wl_display.sync for validation. */
static const struct wl_interface *wl_display_requests_0_types[] = {
	&wl_callback_interface,
};

/* Identifies object arguments in wl_display.get_registry for validation. */
static const struct wl_interface *wl_display_requests_1_types[] = {
	&wl_registry_interface,
};

/* Preserves the wire opcode order for wl_display requests. */
static const struct wl_message wl_display_requests[] = {
	{ "sync", "n", wl_display_requests_0_types },
	{ "get_registry", "n", wl_display_requests_1_types },
};

/* Identifies object arguments in wl_display.error for validation. */
static const struct wl_interface *wl_display_events_0_types[] = {
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_display.delete_id for validation. */
static const struct wl_interface *wl_display_events_1_types[] = {
	NULL,
};

/* Preserves the wire opcode order for wl_display events. */
static const struct wl_message wl_display_events[] = {
	{ "error", "ous", wl_display_events_0_types },
	{ "delete_id", "u", wl_display_events_1_types },
};

/* Exposes the immutable selected wl_display protocol description. */
const struct wl_interface wl_display_interface = {
	"wl_display", 1, 2, wl_display_requests,
	2, wl_display_events
};

/* Identifies object arguments in wl_registry.bind for validation. */
static const struct wl_interface *wl_registry_requests_0_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Preserves the wire opcode order for wl_registry requests. */
static const struct wl_message wl_registry_requests[] = {
	{ "bind", "usun", wl_registry_requests_0_types },
};

/* Identifies object arguments in wl_registry.global for validation. */
static const struct wl_interface *wl_registry_events_0_types[] = {
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_registry.global_remove for validation. */
static const struct wl_interface *wl_registry_events_1_types[] = {
	NULL,
};

/* Preserves the wire opcode order for wl_registry events. */
static const struct wl_message wl_registry_events[] = {
	{ "global", "usu", wl_registry_events_0_types },
	{ "global_remove", "u", wl_registry_events_1_types },
};

/* Exposes the immutable selected wl_registry protocol description. */
const struct wl_interface wl_registry_interface = {
	"wl_registry", 1, 1, wl_registry_requests,
	2, wl_registry_events
};

/* Identifies object arguments in wl_callback.done for validation. */
static const struct wl_interface *wl_callback_events_0_types[] = {
	NULL,
};

/* Preserves the wire opcode order for wl_callback events. */
static const struct wl_message wl_callback_events[] = {
	{ "done", "u", wl_callback_events_0_types },
};

/* Exposes the immutable selected wl_callback protocol description. */
const struct wl_interface wl_callback_interface = {
	"wl_callback", 1, 0, NULL,
	1, wl_callback_events
};

/* Identifies object arguments in wl_compositor.create_surface for validation. */
static const struct wl_interface *wl_compositor_requests_0_types[] = {
	&wl_surface_interface,
};

/* Identifies object arguments in wl_compositor.create_region for validation. */
static const struct wl_interface *wl_compositor_requests_1_types[] = {
	&wl_region_interface,
};

/* Preserves the wire opcode order for wl_compositor requests. */
static const struct wl_message wl_compositor_requests[] = {
	{ "create_surface", "n", wl_compositor_requests_0_types },
	{ "create_region", "n", wl_compositor_requests_1_types },
};

/* Exposes the immutable selected wl_compositor protocol description. */
const struct wl_interface wl_compositor_interface = {
	"wl_compositor", 4, 2, wl_compositor_requests,
	0, NULL
};

/* Identifies object arguments in wl_surface.attach for validation. */
static const struct wl_interface *wl_surface_requests_1_types[] = {
	&wl_buffer_interface,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_surface.damage for validation. */
static const struct wl_interface *wl_surface_requests_2_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_surface.frame for validation. */
static const struct wl_interface *wl_surface_requests_3_types[] = {
	&wl_callback_interface,
};

/* Identifies object arguments in wl_surface.set_opaque_region for validation. */
static const struct wl_interface *wl_surface_requests_4_types[] = {
	&wl_region_interface,
};

/* Identifies object arguments in wl_surface.set_input_region for validation. */
static const struct wl_interface *wl_surface_requests_5_types[] = {
	&wl_region_interface,
};

/* Identifies object arguments in wl_surface.set_buffer_transform for validation. */
static const struct wl_interface *wl_surface_requests_7_types[] = {
	NULL,
};

/* Identifies object arguments in wl_surface.set_buffer_scale for validation. */
static const struct wl_interface *wl_surface_requests_8_types[] = {
	NULL,
};

/* Identifies object arguments in wl_surface.damage_buffer for validation. */
static const struct wl_interface *wl_surface_requests_9_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Preserves the wire opcode order for wl_surface requests. */
static const struct wl_message wl_surface_requests[] = {
	{ "destroy", "", NULL },
	{ "attach", "?oii", wl_surface_requests_1_types },
	{ "damage", "iiii", wl_surface_requests_2_types },
	{ "frame", "n", wl_surface_requests_3_types },
	{ "set_opaque_region", "?o", wl_surface_requests_4_types },
	{ "set_input_region", "?o", wl_surface_requests_5_types },
	{ "commit", "", NULL },
	{ "set_buffer_transform", "2i", wl_surface_requests_7_types },
	{ "set_buffer_scale", "3i", wl_surface_requests_8_types },
	{ "damage_buffer", "4iiii", wl_surface_requests_9_types },
};

/* Identifies object arguments in wl_surface.enter for validation. */
static const struct wl_interface *wl_surface_events_0_types[] = {
	&wl_output_interface,
};

/* Identifies object arguments in wl_surface.leave for validation. */
static const struct wl_interface *wl_surface_events_1_types[] = {
	&wl_output_interface,
};

/* Preserves the wire opcode order for wl_surface events. */
static const struct wl_message wl_surface_events[] = {
	{ "enter", "o", wl_surface_events_0_types },
	{ "leave", "o", wl_surface_events_1_types },
};

/* Exposes the immutable selected wl_surface protocol description. */
const struct wl_interface wl_surface_interface = {
	"wl_surface", 4, 10, wl_surface_requests,
	2, wl_surface_events
};

/* Identifies object arguments in wl_region.add for validation. */
static const struct wl_interface *wl_region_requests_1_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_region.subtract for validation. */
static const struct wl_interface *wl_region_requests_2_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Preserves the wire opcode order for wl_region requests. */
static const struct wl_message wl_region_requests[] = {
	{ "destroy", "", NULL },
	{ "add", "iiii", wl_region_requests_1_types },
	{ "subtract", "iiii", wl_region_requests_2_types },
};

/* Exposes the immutable selected wl_region protocol description. */
const struct wl_interface wl_region_interface = {
	"wl_region", 1, 3, wl_region_requests,
	0, NULL
};

/* Preserves the wire opcode order for wl_buffer requests. */
static const struct wl_message wl_buffer_requests[] = {
	{ "destroy", "", NULL },
};

/* Preserves the wire opcode order for wl_buffer events. */
static const struct wl_message wl_buffer_events[] = {
	{ "release", "", NULL },
};

/* Exposes the immutable selected wl_buffer protocol description. */
const struct wl_interface wl_buffer_interface = {
	"wl_buffer", 1, 1, wl_buffer_requests,
	1, wl_buffer_events
};

/* Identifies the new pool of wl_shm.create_pool. */
static const struct wl_interface *wl_shm_requests_0_types[] = {
	&wl_shm_pool_interface,
	NULL,
	NULL,
};

/* Preserves the wire opcode order for wl_shm requests. */
static const struct wl_message wl_shm_requests[] = {
	{ "create_pool", "nhi", wl_shm_requests_0_types },
};

/* Preserves the wire opcode order for wl_shm events. */
static const struct wl_message wl_shm_events[] = {
	{ "format", "u", NULL },
};

/* Exposes the immutable selected wl_shm protocol description. */
const struct wl_interface wl_shm_interface = {
	"wl_shm", 1, 1, wl_shm_requests,
	1, wl_shm_events
};

/* Identifies the new buffer of wl_shm_pool.create_buffer. */
static const struct wl_interface *wl_shm_pool_requests_0_types[] = {
	&wl_buffer_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Preserves the wire opcode order for wl_shm_pool requests. */
static const struct wl_message wl_shm_pool_requests[] = {
	{ "create_buffer", "niiiiu", wl_shm_pool_requests_0_types },
	{ "destroy", "", NULL },
	{ "resize", "i", NULL },
};

/* Exposes the immutable selected wl_shm_pool protocol description. */
const struct wl_interface wl_shm_pool_interface = {
	"wl_shm_pool", 1, 3, wl_shm_pool_requests,
	0, NULL
};

/* Preserves the wire opcode order for wl_output requests. */
static const struct wl_message wl_output_requests[] = {
	{ "release", "3", NULL },
};

/* Identifies object arguments in wl_output.geometry for validation. */
static const struct wl_interface *wl_output_events_0_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_output.mode for validation. */
static const struct wl_interface *wl_output_events_1_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_output.scale for validation. */
static const struct wl_interface *wl_output_events_3_types[] = {
	NULL,
};

/* Identifies object arguments in wl_output.name for validation. */
static const struct wl_interface *wl_output_events_4_types[] = {
	NULL,
};

/* Identifies object arguments in wl_output.description for validation. */
static const struct wl_interface *wl_output_events_5_types[] = {
	NULL,
};

/* Preserves the wire opcode order for wl_output events. */
static const struct wl_message wl_output_events[] = {
	{ "geometry", "iiiiissi", wl_output_events_0_types },
	{ "mode", "uiii", wl_output_events_1_types },
	{ "done", "2", NULL },
	{ "scale", "2i", wl_output_events_3_types },
	{ "name", "4s", wl_output_events_4_types },
	{ "description", "4s", wl_output_events_5_types },
};

/* Exposes the immutable selected wl_output protocol description. */
const struct wl_interface wl_output_interface = {
	"wl_output", 4, 1, wl_output_requests,
	6, wl_output_events
};

/* Identifies object arguments in wl_seat.get_pointer for validation. */
static const struct wl_interface *wl_seat_requests_0_types[] = {
	&wl_pointer_interface,
};

/* Identifies object arguments in wl_seat.get_keyboard for validation. */
static const struct wl_interface *wl_seat_requests_1_types[] = {
	&wl_keyboard_interface,
};

/*
 * Identifies object arguments in wl_seat.get_touch for validation.
 *
 * wl_touch is not described by this library, so the entry only keeps the
 * request opcodes in their protocol order; no wrapper sends it.
 */
static const struct wl_interface *wl_seat_requests_2_types[] = {
	NULL,
};

/* Preserves the wire opcode order for wl_seat requests. */
static const struct wl_message wl_seat_requests[] = {
	{ "get_pointer", "n", wl_seat_requests_0_types },
	{ "get_keyboard", "n", wl_seat_requests_1_types },
	{ "get_touch", "n", wl_seat_requests_2_types },
	{ "release", "5", NULL },
};

/* Identifies object arguments in wl_seat.capabilities for validation. */
static const struct wl_interface *wl_seat_events_0_types[] = {
	NULL,
};

/* Identifies object arguments in wl_seat.name for validation. */
static const struct wl_interface *wl_seat_events_1_types[] = {
	NULL,
};

/* Preserves the wire opcode order for wl_seat events. */
static const struct wl_message wl_seat_events[] = {
	{ "capabilities", "u", wl_seat_events_0_types },
	{ "name", "2s", wl_seat_events_1_types },
};

/* Exposes the immutable selected wl_seat protocol description. */
const struct wl_interface wl_seat_interface = {
	"wl_seat", 5, 4, wl_seat_requests,
	2, wl_seat_events
};

/* Identifies object arguments in wl_pointer.set_cursor for validation. */
static const struct wl_interface *wl_pointer_requests_0_types[] = {
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
};

/* Preserves the wire opcode order for wl_pointer requests. */
static const struct wl_message wl_pointer_requests[] = {
	{ "set_cursor", "u?oii", wl_pointer_requests_0_types },
	{ "release", "3", NULL },
};

/* Identifies object arguments in wl_pointer.enter for validation. */
static const struct wl_interface *wl_pointer_events_0_types[] = {
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_pointer.leave for validation. */
static const struct wl_interface *wl_pointer_events_1_types[] = {
	NULL,
	&wl_surface_interface,
};

/* Identifies object arguments in wl_pointer.motion for validation. */
static const struct wl_interface *wl_pointer_events_2_types[] = {
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_pointer.button for validation. */
static const struct wl_interface *wl_pointer_events_3_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_pointer.axis for validation. */
static const struct wl_interface *wl_pointer_events_4_types[] = {
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_pointer.axis_source for validation. */
static const struct wl_interface *wl_pointer_events_6_types[] = {
	NULL,
};

/* Identifies object arguments in wl_pointer.axis_stop for validation. */
static const struct wl_interface *wl_pointer_events_7_types[] = {
	NULL,
	NULL,
};

/* Identifies object arguments in wl_pointer.axis_discrete for validation. */
static const struct wl_interface *wl_pointer_events_8_types[] = {
	NULL,
	NULL,
};

/* Preserves the wire opcode order for wl_pointer events. */
static const struct wl_message wl_pointer_events[] = {
	{ "enter", "uoff", wl_pointer_events_0_types },
	{ "leave", "uo", wl_pointer_events_1_types },
	{ "motion", "uff", wl_pointer_events_2_types },
	{ "button", "uuuu", wl_pointer_events_3_types },
	{ "axis", "uuf", wl_pointer_events_4_types },
	{ "frame", "5", NULL },
	{ "axis_source", "5u", wl_pointer_events_6_types },
	{ "axis_stop", "5uu", wl_pointer_events_7_types },
	{ "axis_discrete", "5ui", wl_pointer_events_8_types },
};

/* Exposes the immutable selected wl_pointer protocol description. */
const struct wl_interface wl_pointer_interface = {
	"wl_pointer", 5, 2, wl_pointer_requests,
	9, wl_pointer_events
};

/* Preserves the wire opcode order for wl_keyboard requests. */
static const struct wl_message wl_keyboard_requests[] = {
	{ "release", "3", NULL },
};

/* Identifies object arguments in wl_keyboard.keymap for validation. */
static const struct wl_interface *wl_keyboard_events_0_types[] = {
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_keyboard.enter for validation. */
static const struct wl_interface *wl_keyboard_events_1_types[] = {
	NULL,
	&wl_surface_interface,
	NULL,
};

/* Identifies object arguments in wl_keyboard.leave for validation. */
static const struct wl_interface *wl_keyboard_events_2_types[] = {
	NULL,
	&wl_surface_interface,
};

/* Identifies object arguments in wl_keyboard.key for validation. */
static const struct wl_interface *wl_keyboard_events_3_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_keyboard.modifiers for validation. */
static const struct wl_interface *wl_keyboard_events_4_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in wl_keyboard.repeat_info for validation. */
static const struct wl_interface *wl_keyboard_events_5_types[] = {
	NULL,
	NULL,
};

/* Preserves the wire opcode order for wl_keyboard events. */
static const struct wl_message wl_keyboard_events[] = {
	{ "keymap", "uhu", wl_keyboard_events_0_types },
	{ "enter", "uoa", wl_keyboard_events_1_types },
	{ "leave", "uo", wl_keyboard_events_2_types },
	{ "key", "uuuu", wl_keyboard_events_3_types },
	{ "modifiers", "uuuuu", wl_keyboard_events_4_types },
	{ "repeat_info", "4ii", wl_keyboard_events_5_types },
};

/* Exposes the immutable selected wl_keyboard protocol description. */
const struct wl_interface wl_keyboard_interface = {
	"wl_keyboard", 5, 1, wl_keyboard_requests,
	6, wl_keyboard_events
};

/* Identifies object arguments in xdg_wm_base.create_positioner for validation. */
static const struct wl_interface *xdg_wm_base_requests_1_types[] = {
	&xdg_positioner_interface,
};

/* Identifies object arguments in xdg_wm_base.get_xdg_surface for validation. */
static const struct wl_interface *xdg_wm_base_requests_2_types[] = {
	&xdg_surface_interface,
	&wl_surface_interface,
};

/* Identifies object arguments in xdg_wm_base.pong for validation. */
static const struct wl_interface *xdg_wm_base_requests_3_types[] = {
	NULL,
};

/* Preserves the wire opcode order for xdg_wm_base requests. */
static const struct wl_message xdg_wm_base_requests[] = {
	{ "destroy", "", NULL },
	{ "create_positioner", "n", xdg_wm_base_requests_1_types },
	{ "get_xdg_surface", "no", xdg_wm_base_requests_2_types },
	{ "pong", "u", xdg_wm_base_requests_3_types },
};

/* Identifies object arguments in xdg_wm_base.ping for validation. */
static const struct wl_interface *xdg_wm_base_events_0_types[] = {
	NULL,
};

/* Preserves the wire opcode order for xdg_wm_base events. */
static const struct wl_message xdg_wm_base_events[] = {
	{ "ping", "u", xdg_wm_base_events_0_types },
};

/* Exposes the immutable selected xdg_wm_base protocol description. */
const struct wl_interface xdg_wm_base_interface = {
	"xdg_wm_base", 1, 4, xdg_wm_base_requests,
	1, xdg_wm_base_events
};

/* Identifies object arguments in xdg_positioner.set_size for validation. */
static const struct wl_interface *xdg_positioner_requests_1_types[] = {
	NULL,
	NULL,
};

/* Identifies object arguments in xdg_positioner.set_anchor_rect for validation. */
static const struct wl_interface *xdg_positioner_requests_2_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in xdg_positioner.set_anchor for validation. */
static const struct wl_interface *xdg_positioner_requests_3_types[] = {
	NULL,
};

/* Identifies object arguments in xdg_positioner.set_gravity for validation. */
static const struct wl_interface *xdg_positioner_requests_4_types[] = {
	NULL,
};

/* Identifies object arguments in xdg_positioner.set_constraint_adjustment for validation. */
static const struct wl_interface *xdg_positioner_requests_5_types[] = {
	NULL,
};

/* Identifies object arguments in xdg_positioner.set_offset for validation. */
static const struct wl_interface *xdg_positioner_requests_6_types[] = {
	NULL,
	NULL,
};

/* Preserves the wire opcode order for xdg_positioner requests. */
static const struct wl_message xdg_positioner_requests[] = {
	{ "destroy", "", NULL },
	{ "set_size", "ii", xdg_positioner_requests_1_types },
	{ "set_anchor_rect", "iiii", xdg_positioner_requests_2_types },
	{ "set_anchor", "u", xdg_positioner_requests_3_types },
	{ "set_gravity", "u", xdg_positioner_requests_4_types },
	{ "set_constraint_adjustment", "u", xdg_positioner_requests_5_types },
	{ "set_offset", "ii", xdg_positioner_requests_6_types },
};

/* Exposes the immutable selected xdg_positioner protocol description. */
const struct wl_interface xdg_positioner_interface = {
	"xdg_positioner", 1, 7, xdg_positioner_requests,
	0, NULL
};

/* Identifies object arguments in xdg_surface.get_toplevel for validation. */
static const struct wl_interface *xdg_surface_requests_1_types[] = {
	&xdg_toplevel_interface,
};

/* Identifies object arguments in xdg_surface.get_popup for validation. */
static const struct wl_interface *xdg_surface_requests_2_types[] = {
	&xdg_popup_interface,
	&xdg_surface_interface,
	&xdg_positioner_interface,
};

/* Identifies object arguments in xdg_surface.set_window_geometry for validation. */
static const struct wl_interface *xdg_surface_requests_3_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in xdg_surface.ack_configure for validation. */
static const struct wl_interface *xdg_surface_requests_4_types[] = {
	NULL,
};

/* Preserves the wire opcode order for xdg_surface requests. */
static const struct wl_message xdg_surface_requests[] = {
	{ "destroy", "", NULL },
	{ "get_toplevel", "n", xdg_surface_requests_1_types },
	{ "get_popup", "n?oo", xdg_surface_requests_2_types },
	{ "set_window_geometry", "iiii", xdg_surface_requests_3_types },
	{ "ack_configure", "u", xdg_surface_requests_4_types },
};

/* Identifies object arguments in xdg_surface.configure for validation. */
static const struct wl_interface *xdg_surface_events_0_types[] = {
	NULL,
};

/* Preserves the wire opcode order for xdg_surface events. */
static const struct wl_message xdg_surface_events[] = {
	{ "configure", "u", xdg_surface_events_0_types },
};

/* Exposes the immutable selected xdg_surface protocol description. */
const struct wl_interface xdg_surface_interface = {
	"xdg_surface", 1, 5, xdg_surface_requests,
	1, xdg_surface_events
};

/* Identifies object arguments in xdg_toplevel.set_parent for validation. */
static const struct wl_interface *xdg_toplevel_requests_1_types[] = {
	&xdg_toplevel_interface,
};

/* Identifies object arguments in xdg_toplevel.set_title for validation. */
static const struct wl_interface *xdg_toplevel_requests_2_types[] = {
	NULL,
};

/* Identifies object arguments in xdg_toplevel.set_app_id for validation. */
static const struct wl_interface *xdg_toplevel_requests_3_types[] = {
	NULL,
};

/* Identifies object arguments in xdg_toplevel.show_window_menu for validation. */
static const struct wl_interface *xdg_toplevel_requests_4_types[] = {
	&wl_seat_interface,
	NULL,
	NULL,
	NULL,
};

/* Identifies object arguments in xdg_toplevel.move for validation. */
static const struct wl_interface *xdg_toplevel_requests_5_types[] = {
	&wl_seat_interface,
	NULL,
};

/* Identifies object arguments in xdg_toplevel.resize for validation. */
static const struct wl_interface *xdg_toplevel_requests_6_types[] = {
	&wl_seat_interface,
	NULL,
	NULL,
};

/* Identifies object arguments in xdg_toplevel.set_max_size for validation. */
static const struct wl_interface *xdg_toplevel_requests_7_types[] = {
	NULL,
	NULL,
};

/* Identifies object arguments in xdg_toplevel.set_min_size for validation. */
static const struct wl_interface *xdg_toplevel_requests_8_types[] = {
	NULL,
	NULL,
};

/* Identifies object arguments in xdg_toplevel.set_fullscreen for validation. */
static const struct wl_interface *xdg_toplevel_requests_11_types[] = {
	&wl_output_interface,
};

/* Preserves the wire opcode order for xdg_toplevel requests. */
static const struct wl_message xdg_toplevel_requests[] = {
	{ "destroy", "", NULL },
	{ "set_parent", "?o", xdg_toplevel_requests_1_types },
	{ "set_title", "s", xdg_toplevel_requests_2_types },
	{ "set_app_id", "s", xdg_toplevel_requests_3_types },
	{ "show_window_menu", "ouii", xdg_toplevel_requests_4_types },
	{ "move", "ou", xdg_toplevel_requests_5_types },
	{ "resize", "ouu", xdg_toplevel_requests_6_types },
	{ "set_max_size", "ii", xdg_toplevel_requests_7_types },
	{ "set_min_size", "ii", xdg_toplevel_requests_8_types },
	{ "set_maximized", "", NULL },
	{ "unset_maximized", "", NULL },
	{ "set_fullscreen", "?o", xdg_toplevel_requests_11_types },
	{ "unset_fullscreen", "", NULL },
	{ "set_minimized", "", NULL },
};

/* Identifies object arguments in xdg_toplevel.configure for validation. */
static const struct wl_interface *xdg_toplevel_events_0_types[] = {
	NULL,
	NULL,
	NULL,
};

/* Preserves the wire opcode order for xdg_toplevel events. */
static const struct wl_message xdg_toplevel_events[] = {
	{ "configure", "iia", xdg_toplevel_events_0_types },
	{ "close", "", NULL },
};

/* Exposes the immutable selected xdg_toplevel protocol description. */
const struct wl_interface xdg_toplevel_interface = {
	"xdg_toplevel", 1, 14, xdg_toplevel_requests,
	2, xdg_toplevel_events
};

/* Identifies object arguments in xdg_popup.grab for validation. */
static const struct wl_interface *xdg_popup_requests_1_types[] = {
	&wl_seat_interface,
	NULL,
};

/* Preserves the wire opcode order for xdg_popup requests. */
static const struct wl_message xdg_popup_requests[] = {
	{ "destroy", "", NULL },
	{ "grab", "ou", xdg_popup_requests_1_types },
};

/* Identifies object arguments in xdg_popup.configure for validation. */
static const struct wl_interface *xdg_popup_events_0_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* Preserves the wire opcode order for xdg_popup events. */
static const struct wl_message xdg_popup_events[] = {
	{ "configure", "iiii", xdg_popup_events_0_types },
	{ "popup_done", "", NULL },
};

/* Exposes the immutable selected xdg_popup protocol description. */
const struct wl_interface xdg_popup_interface = {
	"xdg_popup", 1, 2, xdg_popup_requests,
	2, xdg_popup_events
};

/* Identifies object arguments in zed_gpu_buffer_v1.create_buffer for validation. */
static const struct wl_interface *zed_gpu_buffer_v1_requests_1_types[] = {
	&wl_buffer_interface,
	NULL,
	NULL,
};

/* Identifies the surface argument of zed_gpu_buffer_v1.set_acquire_fence (version 2). */
static const struct wl_interface *zed_gpu_buffer_v1_requests_2_types[] = {
	&wl_surface_interface,
	NULL,
	NULL,
	NULL,
};

/* Preserves the wire opcode order for zed_gpu_buffer_v1 requests. */
static const struct wl_message zed_gpu_buffer_v1_requests[] = {
	{ "destroy", "", NULL },
	{ "create_buffer", "nha", zed_gpu_buffer_v1_requests_1_types },
	{ "set_acquire_fence", "2ohuu", zed_gpu_buffer_v1_requests_2_types },
};

/* Exposes the immutable selected zed_gpu_buffer_v1 protocol description. */
const struct wl_interface zed_gpu_buffer_v1_interface = {
	"zed_gpu_buffer_v1", 2, 3, zed_gpu_buffer_v1_requests,
	0, NULL
};

/*
 * Installs the listener for wl_display events.
 */
int
wl_display_add_listener(
	struct wl_display *object,
	const struct wl_display_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the wl_display.sync request.
 */
struct wl_callback *
wl_display_sync(
	struct wl_display *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].n = 0;

	/* Queues the wire request atomically with any newly allocated object. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, &wl_callback_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct wl_callback *)created;
}

/*
 * Sends the wl_display.get_registry request.
 */
struct wl_registry *
wl_display_get_registry(
	struct wl_display *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].n = 0;

	/* Queues the wire request atomically with any newly allocated object. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, &wl_registry_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct wl_registry *)created;
}

/*
 * Associates client state with the wl_display proxy.
 */
void
wl_display_set_user_data(
	struct wl_display *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the wl_display proxy.
 */
void *
wl_display_get_user_data(
	struct wl_display *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_display proxy.
 */
uint32_t
wl_display_get_version(
	struct wl_display *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for wl_registry events.
 */
int
wl_registry_add_listener(
	struct wl_registry *object,
	const struct wl_registry_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Drops the local wl_registry proxy.
 */
void
wl_registry_destroy(
	struct wl_registry *object)
{
	/* Suppresses future callbacks while the server retires the object ID. */
	wl_proxy_destroy((struct wl_proxy *)object);

	/* Succeeded: caller ownership has ended. */
	return;
}

/*
 * Associates client state with the wl_registry proxy.
 */
void
wl_registry_set_user_data(
	struct wl_registry *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the wl_registry proxy.
 */
void *
wl_registry_get_user_data(
	struct wl_registry *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_registry proxy.
 */
uint32_t
wl_registry_get_version(
	struct wl_registry *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for wl_callback events.
 */
int
wl_callback_add_listener(
	struct wl_callback *object,
	const struct wl_callback_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Drops the local wl_callback proxy.
 */
void
wl_callback_destroy(
	struct wl_callback *object)
{
	/* Suppresses future callbacks while the server retires the object ID. */
	wl_proxy_destroy((struct wl_proxy *)object);

	/* Succeeded: caller ownership has ended. */
	return;
}

/*
 * Associates client state with the wl_callback proxy.
 */
void
wl_callback_set_user_data(
	struct wl_callback *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the wl_callback proxy.
 */
void *
wl_callback_get_user_data(
	struct wl_callback *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_callback proxy.
 */
uint32_t
wl_callback_get_version(
	struct wl_callback *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Sends the wl_compositor.create_surface request.
 */
struct wl_surface *
wl_compositor_create_surface(
	struct wl_compositor *object)
{
	uint32_t version;
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].n = 0;

	/* Inherits the compositor version negotiated at registry bind. */
	version = wl_proxy_get_version((struct wl_proxy *)object);

	/* Queues the wire request atomically with any newly allocated object. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, &wl_surface_interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct wl_surface *)created;
}

/*
 * Sends the wl_compositor.create_region request.
 */
struct wl_region *
wl_compositor_create_region(
	struct wl_compositor *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].n = 0;

	/* Queues the wire request atomically with any newly allocated object. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, &wl_region_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct wl_region *)created;
}

/*
 * Drops the local wl_compositor proxy.
 */
void
wl_compositor_destroy(
	struct wl_compositor *object)
{
	/* Suppresses future callbacks while the server retires the object ID. */
	wl_proxy_destroy((struct wl_proxy *)object);

	/* Succeeded: caller ownership has ended. */
	return;
}

/*
 * Associates client state with the wl_compositor proxy.
 */
void
wl_compositor_set_user_data(
	struct wl_compositor *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the wl_compositor proxy.
 */
void *
wl_compositor_get_user_data(
	struct wl_compositor *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_compositor proxy.
 */
uint32_t
wl_compositor_get_version(
	struct wl_compositor *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for wl_surface events.
 */
int
wl_surface_add_listener(
	struct wl_surface *object,
	const struct wl_surface_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the wl_surface.destroy request.
 */
void
wl_surface_destroy(
	struct wl_surface *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the wl_surface.attach request.
 */
void
wl_surface_attach(
	struct wl_surface *object,
	struct wl_buffer *buffer,
	int32_t x,
	int32_t y)
{
	union wl_argument arguments[3];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].o = (struct wl_object *)buffer;
	arguments[1].i = x;
	arguments[2].i = y;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the wl_surface.damage request.
 */
void
wl_surface_damage(
	struct wl_surface *object,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	union wl_argument arguments[4];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = x;
	arguments[1].i = y;
	arguments[2].i = width;
	arguments[3].i = height;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the wl_surface.frame request.
 */
struct wl_callback *
wl_surface_frame(
	struct wl_surface *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].n = 0;

	/* Queues the wire request atomically with any newly allocated object. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 3U, &wl_callback_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct wl_callback *)created;
}

/*
 * Sends the wl_surface.set_opaque_region request.
 */
void
wl_surface_set_opaque_region(
	struct wl_surface *object,
	struct wl_region *region)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].o = (struct wl_object *)region;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 4U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the wl_surface.set_input_region request.
 */
void
wl_surface_set_input_region(
	struct wl_surface *object,
	struct wl_region *region)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].o = (struct wl_object *)region;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 5U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the wl_surface.commit request.
 */
void
wl_surface_commit(
	struct wl_surface *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 6U, NULL, 0, 0, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the wl_surface.set_buffer_transform request.
 */
void
wl_surface_set_buffer_transform(
	struct wl_surface *object,
	int32_t transform)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = transform;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 7U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the wl_surface.set_buffer_scale request.
 */
void
wl_surface_set_buffer_scale(
	struct wl_surface *object,
	int32_t scale)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = scale;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 8U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the wl_surface.damage_buffer request.
 */
void
wl_surface_damage_buffer(
	struct wl_surface *object,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	union wl_argument arguments[4];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = x;
	arguments[1].i = y;
	arguments[2].i = width;
	arguments[3].i = height;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 9U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Associates client state with the wl_surface proxy.
 */
void
wl_surface_set_user_data(
	struct wl_surface *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the wl_surface proxy.
 */
void *
wl_surface_get_user_data(
	struct wl_surface *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_surface proxy.
 */
uint32_t
wl_surface_get_version(
	struct wl_surface *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Sends the wl_region.destroy request.
 */
void
wl_region_destroy(
	struct wl_region *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the wl_region.add request.
 */
void
wl_region_add(
	struct wl_region *object,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	union wl_argument arguments[4];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = x;
	arguments[1].i = y;
	arguments[2].i = width;
	arguments[3].i = height;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the wl_region.subtract request.
 */
void
wl_region_subtract(
	struct wl_region *object,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	union wl_argument arguments[4];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = x;
	arguments[1].i = y;
	arguments[2].i = width;
	arguments[3].i = height;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Associates client state with the wl_region proxy.
 */
void
wl_region_set_user_data(
	struct wl_region *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the wl_region proxy.
 */
void *
wl_region_get_user_data(
	struct wl_region *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_region proxy.
 */
uint32_t
wl_region_get_version(
	struct wl_region *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for wl_buffer events.
 */
int
wl_buffer_add_listener(
	struct wl_buffer *object,
	const struct wl_buffer_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Associates typed callbacks with a wl_shm proxy.
 */
int
wl_shm_add_listener(
	struct wl_shm *object,
	const struct wl_shm_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the wl_shm.create_pool request; the fd stays the caller's.
 */
struct wl_shm_pool *
wl_shm_create_pool(
	struct wl_shm *object,
	int32_t fd,
	int32_t size)
{
	union wl_argument arguments[3];
	struct wl_proxy *created;

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].n = 0;
	arguments[1].h = fd;
	arguments[2].i = size;

	/* Queues the wire request atomically with the newly allocated pool. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, &wl_shm_pool_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct wl_shm_pool *)created;
}

/*
 * Drops the local wl_shm proxy (wl_shm version 1 has no destroy request).
 */
void
wl_shm_destroy(
	struct wl_shm *object)
{
	/* Suppresses future callbacks while the server keeps the binding. */
	wl_proxy_destroy((struct wl_proxy *)object);
}

/*
 * Sends the wl_shm_pool.create_buffer request.
 */
struct wl_buffer *
wl_shm_pool_create_buffer(
	struct wl_shm_pool *object,
	int32_t offset,
	int32_t width,
	int32_t height,
	int32_t stride,
	uint32_t format)
{
	union wl_argument arguments[6];
	struct wl_proxy *created;

	/* Preserves argument order. */
	arguments[0].n = 0;
	arguments[1].i = offset;
	arguments[2].i = width;
	arguments[3].i = height;
	arguments[4].i = stride;
	arguments[5].u = format;

	/* Queues the wire request atomically with the newly allocated buffer. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, &wl_buffer_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct wl_buffer *)created;
}

/*
 * Sends the wl_shm_pool.destroy request.
 */
void
wl_shm_pool_destroy(
	struct wl_shm_pool *object)
{
	/* Queues the wire request and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends the wl_shm_pool.resize request.
 */
void
wl_shm_pool_resize(
	struct wl_shm_pool *object,
	int32_t size)
{
	union wl_argument arguments[1];

	/* The new size of the pool. */
	arguments[0].i = size;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, NULL, 0, 0, arguments);
}

/*
 * Sends the wl_buffer.destroy request.
 */
void
wl_buffer_destroy(
	struct wl_buffer *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Associates client state with the wl_buffer proxy.
 */
void
wl_buffer_set_user_data(
	struct wl_buffer *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the wl_buffer proxy.
 */
void *
wl_buffer_get_user_data(
	struct wl_buffer *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_buffer proxy.
 */
uint32_t
wl_buffer_get_version(
	struct wl_buffer *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for wl_output events.
 */
int
wl_output_add_listener(
	struct wl_output *object,
	const struct wl_output_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the wl_output.release request.
 */
void
wl_output_release(
	struct wl_output *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Associates client state with the wl_output proxy.
 */
void
wl_output_set_user_data(
	struct wl_output *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the wl_output proxy.
 */
void *
wl_output_get_user_data(
	struct wl_output *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_output proxy.
 */
uint32_t
wl_output_get_version(
	struct wl_output *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for wl_seat events.
 */
int
wl_seat_add_listener(
	struct wl_seat *wl_seat,
	const struct wl_seat_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)wl_seat, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the wl_seat.get_pointer request.
 */
struct wl_pointer *
wl_seat_get_pointer(
	struct wl_seat *wl_seat)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;
	uint32_t version;

	/* The child inherits the seat's negotiated version, as the protocol requires. */
	version = wl_proxy_get_version((struct wl_proxy *)wl_seat);

	/* The new_id slot is filled with the identity allocated for the child. */
	arguments[0].n = 0;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)wl_seat, 0U, &wl_pointer_interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct wl_pointer *)created;
}

/*
 * Sends the wl_seat.get_keyboard request.
 */
struct wl_keyboard *
wl_seat_get_keyboard(
	struct wl_seat *wl_seat)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;
	uint32_t version;

	/* The child inherits the seat's negotiated version, as the protocol requires. */
	version = wl_proxy_get_version((struct wl_proxy *)wl_seat);

	/* The new_id slot is filled with the identity allocated for the child. */
	arguments[0].n = 0;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)wl_seat, 1U, &wl_keyboard_interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct wl_keyboard *)created;
}

/*
 * Sends the wl_seat.release request and drops the local proxy.
 */
void
wl_seat_release(
	struct wl_seat *wl_seat)
{
	/* Queues the destructor request; the proxy is destroyed with it. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)wl_seat, 3U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Drops the local wl_seat proxy without telling the compositor.
 */
void
wl_seat_destroy(
	struct wl_seat *wl_seat)
{
	/* Suppresses future callbacks; the compositor keeps its object alive. */
	wl_proxy_destroy((struct wl_proxy *)wl_seat);

	/* Succeeded: caller ownership has ended. */
	return;
}

/*
 * Associates client state with the wl_seat proxy.
 */
void
wl_seat_set_user_data(
	struct wl_seat *wl_seat,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)wl_seat, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the wl_seat proxy.
 */
void *
wl_seat_get_user_data(
	struct wl_seat *wl_seat)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)wl_seat);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_seat proxy.
 */
uint32_t
wl_seat_get_version(
	struct wl_seat *wl_seat)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)wl_seat);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for wl_pointer events.
 */
int
wl_pointer_add_listener(
	struct wl_pointer *wl_pointer,
	const struct wl_pointer_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)wl_pointer, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the wl_pointer.set_cursor request.
 *
 * zwl accepts the request and draws no cursor; other compositors may.
 */
void
wl_pointer_set_cursor(
	struct wl_pointer *wl_pointer,
	uint32_t serial,
	struct wl_surface *surface,
	int32_t hotspot_x,
	int32_t hotspot_y)
{
	union wl_argument arguments[4];

	/* Preserves argument order; a null surface hides the cursor. */
	arguments[0].u = serial;
	arguments[1].o = (struct wl_object *)surface;
	arguments[2].i = hotspot_x;
	arguments[3].i = hotspot_y;

	/* Queues the wire request atomically. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)wl_pointer, 0U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the wl_pointer.release request and drops the local proxy.
 */
void
wl_pointer_release(
	struct wl_pointer *wl_pointer)
{
	/* Queues the destructor request; the proxy is destroyed with it. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)wl_pointer, 1U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Drops the local wl_pointer proxy without telling the compositor.
 */
void
wl_pointer_destroy(
	struct wl_pointer *wl_pointer)
{
	/* Suppresses future callbacks; the compositor keeps its object alive. */
	wl_proxy_destroy((struct wl_proxy *)wl_pointer);

	/* Succeeded: caller ownership has ended. */
	return;
}

/*
 * Associates client state with the wl_pointer proxy.
 */
void
wl_pointer_set_user_data(
	struct wl_pointer *wl_pointer,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)wl_pointer, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the wl_pointer proxy.
 */
void *
wl_pointer_get_user_data(
	struct wl_pointer *wl_pointer)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)wl_pointer);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_pointer proxy.
 */
uint32_t
wl_pointer_get_version(
	struct wl_pointer *wl_pointer)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)wl_pointer);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for wl_keyboard events.
 */
int
wl_keyboard_add_listener(
	struct wl_keyboard *wl_keyboard,
	const struct wl_keyboard_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)wl_keyboard, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the wl_keyboard.release request and drops the local proxy.
 */
void
wl_keyboard_release(
	struct wl_keyboard *wl_keyboard)
{
	/* Queues the destructor request; the proxy is destroyed with it. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)wl_keyboard, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Drops the local wl_keyboard proxy without telling the compositor.
 */
void
wl_keyboard_destroy(
	struct wl_keyboard *wl_keyboard)
{
	/* Suppresses future callbacks; the compositor keeps its object alive. */
	wl_proxy_destroy((struct wl_proxy *)wl_keyboard);

	/* Succeeded: caller ownership has ended. */
	return;
}

/*
 * Associates client state with the wl_keyboard proxy.
 */
void
wl_keyboard_set_user_data(
	struct wl_keyboard *wl_keyboard,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)wl_keyboard, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the wl_keyboard proxy.
 */
void *
wl_keyboard_get_user_data(
	struct wl_keyboard *wl_keyboard)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)wl_keyboard);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_keyboard proxy.
 */
uint32_t
wl_keyboard_get_version(
	struct wl_keyboard *wl_keyboard)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)wl_keyboard);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for xdg_wm_base events.
 */
int
xdg_wm_base_add_listener(
	struct xdg_wm_base *object,
	const struct xdg_wm_base_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the xdg_wm_base.destroy request.
 */
void
xdg_wm_base_destroy(
	struct xdg_wm_base *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_wm_base.create_positioner request.
 */
struct xdg_positioner *
xdg_wm_base_create_positioner(
	struct xdg_wm_base *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].n = 0;

	/* Queues the wire request atomically with any newly allocated object. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, &xdg_positioner_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct xdg_positioner *)created;
}

/*
 * Sends the xdg_wm_base.get_xdg_surface request.
 */
struct xdg_surface *
xdg_wm_base_get_xdg_surface(
	struct xdg_wm_base *object,
	struct wl_surface *surface)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)surface;

	/* Queues the wire request atomically with any newly allocated object. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, &xdg_surface_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct xdg_surface *)created;
}

/*
 * Sends the xdg_wm_base.pong request.
 */
void
xdg_wm_base_pong(
	struct xdg_wm_base *object,
	uint32_t serial)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].u = serial;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 3U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Associates client state with the xdg_wm_base proxy.
 */
void
xdg_wm_base_set_user_data(
	struct xdg_wm_base *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the xdg_wm_base proxy.
 */
void *
xdg_wm_base_get_user_data(
	struct xdg_wm_base *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the xdg_wm_base proxy.
 */
uint32_t
xdg_wm_base_get_version(
	struct xdg_wm_base *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Sends the xdg_positioner.destroy request.
 */
void
xdg_positioner_destroy(
	struct xdg_positioner *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_positioner.set_size request.
 */
void
xdg_positioner_set_size(
	struct xdg_positioner *object,
	int32_t width,
	int32_t height)
{
	union wl_argument arguments[2];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = width;
	arguments[1].i = height;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_positioner.set_anchor_rect request.
 */
void
xdg_positioner_set_anchor_rect(
	struct xdg_positioner *object,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	union wl_argument arguments[4];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = x;
	arguments[1].i = y;
	arguments[2].i = width;
	arguments[3].i = height;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_positioner.set_anchor request.
 */
void
xdg_positioner_set_anchor(
	struct xdg_positioner *object,
	uint32_t anchor)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].u = anchor;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 3U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_positioner.set_gravity request.
 */
void
xdg_positioner_set_gravity(
	struct xdg_positioner *object,
	uint32_t gravity)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].u = gravity;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 4U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_positioner.set_constraint_adjustment request.
 */
void
xdg_positioner_set_constraint_adjustment(
	struct xdg_positioner *object,
	uint32_t constraint_adjustment)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].u = constraint_adjustment;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 5U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_positioner.set_offset request.
 */
void
xdg_positioner_set_offset(
	struct xdg_positioner *object,
	int32_t x,
	int32_t y)
{
	union wl_argument arguments[2];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = x;
	arguments[1].i = y;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 6U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Associates client state with the xdg_positioner proxy.
 */
void
xdg_positioner_set_user_data(
	struct xdg_positioner *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the xdg_positioner proxy.
 */
void *
xdg_positioner_get_user_data(
	struct xdg_positioner *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the xdg_positioner proxy.
 */
uint32_t
xdg_positioner_get_version(
	struct xdg_positioner *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for xdg_surface events.
 */
int
xdg_surface_add_listener(
	struct xdg_surface *object,
	const struct xdg_surface_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the xdg_surface.destroy request.
 */
void
xdg_surface_destroy(
	struct xdg_surface *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_surface.get_toplevel request.
 */
struct xdg_toplevel *
xdg_surface_get_toplevel(
	struct xdg_surface *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].n = 0;

	/* Queues the wire request atomically with any newly allocated object. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, &xdg_toplevel_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct xdg_toplevel *)created;
}

/*
 * Sends the xdg_surface.get_popup request.
 */
struct xdg_popup *
xdg_surface_get_popup(
	struct xdg_surface *object,
	struct xdg_surface *parent,
	struct xdg_positioner *positioner)
{
	union wl_argument arguments[3];
	struct wl_proxy *created;

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)parent;
	arguments[2].o = (struct wl_object *)positioner;

	/* Queues the wire request atomically with any newly allocated object. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, &xdg_popup_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct xdg_popup *)created;
}

/*
 * Sends the xdg_surface.set_window_geometry request.
 */
void
xdg_surface_set_window_geometry(
	struct xdg_surface *object,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	union wl_argument arguments[4];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = x;
	arguments[1].i = y;
	arguments[2].i = width;
	arguments[3].i = height;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 3U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_surface.ack_configure request.
 */
void
xdg_surface_ack_configure(
	struct xdg_surface *object,
	uint32_t serial)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].u = serial;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 4U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Associates client state with the xdg_surface proxy.
 */
void
xdg_surface_set_user_data(
	struct xdg_surface *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the xdg_surface proxy.
 */
void *
xdg_surface_get_user_data(
	struct xdg_surface *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the xdg_surface proxy.
 */
uint32_t
xdg_surface_get_version(
	struct xdg_surface *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for xdg_toplevel events.
 */
int
xdg_toplevel_add_listener(
	struct xdg_toplevel *object,
	const struct xdg_toplevel_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the xdg_toplevel.destroy request.
 */
void
xdg_toplevel_destroy(
	struct xdg_toplevel *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.set_parent request.
 */
void
xdg_toplevel_set_parent(
	struct xdg_toplevel *object,
	struct xdg_toplevel *parent)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].o = (struct wl_object *)parent;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.set_title request.
 */
void
xdg_toplevel_set_title(
	struct xdg_toplevel *object,
	const char *title)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].s = title;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.set_app_id request.
 */
void
xdg_toplevel_set_app_id(
	struct xdg_toplevel *object,
	const char *app_id)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].s = app_id;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 3U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.show_window_menu request.
 */
void
xdg_toplevel_show_window_menu(
	struct xdg_toplevel *object,
	struct wl_seat *seat,
	uint32_t serial,
	int32_t x,
	int32_t y)
{
	union wl_argument arguments[4];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].o = (struct wl_object *)seat;
	arguments[1].u = serial;
	arguments[2].i = x;
	arguments[3].i = y;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 4U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.move request.
 */
void
xdg_toplevel_move(
	struct xdg_toplevel *object,
	struct wl_seat *seat,
	uint32_t serial)
{
	union wl_argument arguments[2];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].o = (struct wl_object *)seat;
	arguments[1].u = serial;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 5U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.resize request.
 */
void
xdg_toplevel_resize(
	struct xdg_toplevel *object,
	struct wl_seat *seat,
	uint32_t serial,
	uint32_t edges)
{
	union wl_argument arguments[3];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].o = (struct wl_object *)seat;
	arguments[1].u = serial;
	arguments[2].u = edges;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 6U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.set_max_size request.
 */
void
xdg_toplevel_set_max_size(
	struct xdg_toplevel *object,
	int32_t width,
	int32_t height)
{
	union wl_argument arguments[2];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = width;
	arguments[1].i = height;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 7U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.set_min_size request.
 */
void
xdg_toplevel_set_min_size(
	struct xdg_toplevel *object,
	int32_t width,
	int32_t height)
{
	union wl_argument arguments[2];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].i = width;
	arguments[1].i = height;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 8U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.set_maximized request.
 */
void
xdg_toplevel_set_maximized(
	struct xdg_toplevel *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 9U, NULL, 0, 0, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.unset_maximized request.
 */
void
xdg_toplevel_unset_maximized(
	struct xdg_toplevel *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 10U, NULL, 0, 0, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.set_fullscreen request.
 */
void
xdg_toplevel_set_fullscreen(
	struct xdg_toplevel *object,
	struct wl_output *output)
{
	union wl_argument arguments[1];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].o = (struct wl_object *)output;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 11U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.unset_fullscreen request.
 */
void
xdg_toplevel_unset_fullscreen(
	struct xdg_toplevel *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 12U, NULL, 0, 0, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_toplevel.set_minimized request.
 */
void
xdg_toplevel_set_minimized(
	struct xdg_toplevel *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 13U, NULL, 0, 0, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Associates client state with the xdg_toplevel proxy.
 */
void
xdg_toplevel_set_user_data(
	struct xdg_toplevel *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the xdg_toplevel proxy.
 */
void *
xdg_toplevel_get_user_data(
	struct xdg_toplevel *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the xdg_toplevel proxy.
 */
uint32_t
xdg_toplevel_get_version(
	struct xdg_toplevel *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Installs the listener for xdg_popup events.
 */
int
xdg_popup_add_listener(
	struct xdg_popup *object,
	const struct xdg_popup_listener *listener,
	void *data)
{
	int error;

	/* Associates typed callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the xdg_popup.destroy request.
 */
void
xdg_popup_destroy(
	struct xdg_popup *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the xdg_popup.grab request.
 */
void
xdg_popup_grab(
	struct xdg_popup *object,
	struct wl_seat *seat,
	uint32_t serial)
{
	union wl_argument arguments[2];

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].o = (struct wl_object *)seat;
	arguments[1].u = serial;

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, NULL, 0, 0, arguments);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Associates client state with the xdg_popup proxy.
 */
void
xdg_popup_set_user_data(
	struct xdg_popup *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the xdg_popup proxy.
 */
void *
xdg_popup_get_user_data(
	struct xdg_popup *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the xdg_popup proxy.
 */
uint32_t
xdg_popup_get_version(
	struct xdg_popup *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Sends the zed_gpu_buffer_v1.destroy request.
 */
void
zed_gpu_buffer_v1_destroy(
	struct zed_gpu_buffer_v1 *object)
{

	/* Queues the wire request atomically with any newly allocated object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the display owns the queued request or its fatal error. */
	return;
}

/*
 * Sends the zed_gpu_buffer_v1.create_buffer request.
 */
struct wl_buffer *
zed_gpu_buffer_v1_create_buffer(
	struct zed_gpu_buffer_v1 *object,
	int fd,
	struct wl_array *metadata)
{
	union wl_argument arguments[3];
	struct wl_proxy *created;

	/* Preserves argument order and keeps descriptor ownership with the caller. */
	arguments[0].n = 0;
	arguments[1].h = fd;
	arguments[2].a = metadata;

	/* Queues the wire request atomically with any newly allocated object. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, &wl_buffer_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct wl_buffer *)created;
}

/*
 * Sends the zed_gpu_buffer_v1.set_acquire_fence request (version 2): the
 * surface's next commit is used once the fence's payload generation is done.
 * The fd stays the caller's.
 */
void
zed_gpu_buffer_v1_set_acquire_fence(
	struct zed_gpu_buffer_v1 *object,
	struct wl_surface *surface,
	int fd,
	uint64_t generation)
{
	union wl_argument arguments[4];

	/* The surface, the fence and its generation in two words (high, then low). */
	arguments[0].o = (struct wl_object *)surface;
	arguments[1].h = fd;
	arguments[2].u = (uint32_t)(generation >> 32);
	arguments[3].u = (uint32_t)generation;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, NULL, 0, 0, arguments);
}

/*
 * Associates client state with the zed_gpu_buffer_v1 proxy.
 */
void
zed_gpu_buffer_v1_set_user_data(
	struct zed_gpu_buffer_v1 *object,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);

	/* Succeeded: the association is updated. */
	return;
}

/*
 * Obtains client state from the zed_gpu_buffer_v1 proxy.
 */
void *
zed_gpu_buffer_v1_get_user_data(
	struct zed_gpu_buffer_v1 *object)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Obtains the negotiated version of the zed_gpu_buffer_v1 proxy.
 */
uint32_t
zed_gpu_buffer_v1_get_version(
	struct zed_gpu_buffer_v1 *object)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the requested proxy property. */
	return answer;
}

/*
 * Binds a registry global at the requested supported interface version.
 */
void *
wl_registry_bind(
	struct wl_registry *registry,
	uint32_t name,
	const struct wl_interface *interface,
	uint32_t version)
{
	union wl_argument arguments[4];
	struct wl_proxy *created;

	/* Rejects a local interface/version mismatch before writing the wire. */
	if (interface == NULL || version == 0) {
		errno = EINVAL;
		return NULL;
	}

	/* Rejects a version the supplied description cannot decode. */
	if (version > (uint32_t)interface->version) {
		errno = EINVAL;
		return NULL;
	}

	/* Expands the untyped new_id into its standard name/version/id triple. */
	arguments[0].u = name;
	arguments[1].s = interface->name;
	arguments[2].u = version;
	arguments[3].n = 0;

	/* Associates the created proxy with the constructor's event queue. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)registry, 0, interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the bound proxy. */
	return created;
}
