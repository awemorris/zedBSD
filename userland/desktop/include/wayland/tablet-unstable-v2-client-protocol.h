/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the tablet protocol's objects, listeners and typed requests
 * (tablet_unstable_v2, version 1; WS079 p003): the manager, a seat's tablets
 * and their tools.  The pads (zwp_tablet_pad_v2 and its groups, rings and
 * strips) are described only as far as zwp_tablet_seat_v2.pad_added needs;
 * the compositor never announces a pad.  The names follow the upstream
 * client header; the wire descriptions were checked against the pinned
 * description (API-PROVENANCE.md).
 */

#ifndef KERN_TABLET_UNSTABLE_V2_CLIENT_PROTOCOL_H
#define KERN_TABLET_UNSTABLE_V2_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_seat;
struct wl_surface;
struct zwp_tablet_manager_v2;
struct zwp_tablet_seat_v2;
struct zwp_tablet_tool_v2;
struct zwp_tablet_v2;
struct zwp_tablet_pad_v2;

/* The interfaces' descriptions (userland/desktop/libwayland/tablet-protocol.c). */
extern const struct wl_interface zwp_tablet_manager_v2_interface;
extern const struct wl_interface zwp_tablet_seat_v2_interface;
extern const struct wl_interface zwp_tablet_tool_v2_interface;
extern const struct wl_interface zwp_tablet_v2_interface;
extern const struct wl_interface zwp_tablet_pad_v2_interface;

/* zwp_tablet_manager_v2: the global that gives a seat's tablets. */
#define ZWP_TABLET_MANAGER_V2_GET_TABLET_SEAT 0U
#define ZWP_TABLET_MANAGER_V2_DESTROY 1U
struct zwp_tablet_seat_v2 *zwp_tablet_manager_v2_get_tablet_seat(struct zwp_tablet_manager_v2 *zwp_tablet_manager_v2, struct wl_seat *seat);
void zwp_tablet_manager_v2_destroy(struct zwp_tablet_manager_v2 *zwp_tablet_manager_v2);

/* zwp_tablet_seat_v2: a seat's tablets, tools and pads, announced as they come. */
struct zwp_tablet_seat_v2_listener {
	void (*tablet_added)(void *data, struct zwp_tablet_seat_v2 *zwp_tablet_seat_v2, struct zwp_tablet_v2 *id);
	void (*tool_added)(void *data, struct zwp_tablet_seat_v2 *zwp_tablet_seat_v2, struct zwp_tablet_tool_v2 *id);
	void (*pad_added)(void *data, struct zwp_tablet_seat_v2 *zwp_tablet_seat_v2, struct zwp_tablet_pad_v2 *id);
};
int zwp_tablet_seat_v2_add_listener(struct zwp_tablet_seat_v2 *zwp_tablet_seat_v2, const struct zwp_tablet_seat_v2_listener *listener, void *data);
#define ZWP_TABLET_SEAT_V2_DESTROY 0U
void zwp_tablet_seat_v2_destroy(struct zwp_tablet_seat_v2 *zwp_tablet_seat_v2);

/* zwp_tablet_tool_v2: one physical tool (a pen's tip, its eraser end). */
#define ZWP_TABLET_TOOL_V2_TYPE_PEN 0x140U
#define ZWP_TABLET_TOOL_V2_TYPE_ERASER 0x141U
#define ZWP_TABLET_TOOL_V2_TYPE_BRUSH 0x142U
#define ZWP_TABLET_TOOL_V2_TYPE_PENCIL 0x143U
#define ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH 0x144U
#define ZWP_TABLET_TOOL_V2_TYPE_FINGER 0x145U
#define ZWP_TABLET_TOOL_V2_TYPE_MOUSE 0x146U
#define ZWP_TABLET_TOOL_V2_TYPE_LENS 0x147U
#define ZWP_TABLET_TOOL_V2_CAPABILITY_TILT 1U
#define ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE 2U
#define ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE 3U
#define ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION 4U
#define ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER 5U
#define ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL 6U
#define ZWP_TABLET_TOOL_V2_BUTTON_STATE_RELEASED 0U
#define ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED 1U
#define ZWP_TABLET_TOOL_V2_ERROR_ROLE 0U
struct zwp_tablet_tool_v2_listener {
	void (*type)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, uint32_t tool_type);
	void (*hardware_serial)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, uint32_t hardware_serial_hi, uint32_t hardware_serial_lo);
	void (*hardware_id_wacom)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, uint32_t hardware_id_hi, uint32_t hardware_id_lo);
	void (*capability)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, uint32_t capability);
	void (*done)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2);
	void (*removed)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2);
	void (*proximity_in)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, uint32_t serial, struct zwp_tablet_v2 *tablet, struct wl_surface *surface);
	void (*proximity_out)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2);
	void (*down)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, uint32_t serial);
	void (*up)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2);
	void (*motion)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, wl_fixed_t x, wl_fixed_t y);
	void (*pressure)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, uint32_t pressure);
	void (*distance)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, uint32_t distance);
	void (*tilt)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, wl_fixed_t tilt_x, wl_fixed_t tilt_y);
	void (*rotation)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, wl_fixed_t degrees);
	void (*slider)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, int32_t position);
	void (*wheel)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, wl_fixed_t degrees, int32_t clicks);
	void (*button)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, uint32_t serial, uint32_t button, uint32_t state);
	void (*frame)(void *data, struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, uint32_t time);
};
int zwp_tablet_tool_v2_add_listener(struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, const struct zwp_tablet_tool_v2_listener *listener, void *data);
#define ZWP_TABLET_TOOL_V2_SET_CURSOR 0U
#define ZWP_TABLET_TOOL_V2_DESTROY 1U
void zwp_tablet_tool_v2_set_cursor(struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2, uint32_t serial, struct wl_surface *surface, int32_t hotspot_x, int32_t hotspot_y);
void zwp_tablet_tool_v2_destroy(struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2);

/* zwp_tablet_v2: one graphics tablet (a device the tools are used on). */
struct zwp_tablet_v2_listener {
	void (*name)(void *data, struct zwp_tablet_v2 *zwp_tablet_v2, const char *name);
	void (*id)(void *data, struct zwp_tablet_v2 *zwp_tablet_v2, uint32_t vid, uint32_t pid);
	void (*path)(void *data, struct zwp_tablet_v2 *zwp_tablet_v2, const char *path);
	void (*done)(void *data, struct zwp_tablet_v2 *zwp_tablet_v2);
	void (*removed)(void *data, struct zwp_tablet_v2 *zwp_tablet_v2);
};
int zwp_tablet_v2_add_listener(struct zwp_tablet_v2 *zwp_tablet_v2, const struct zwp_tablet_v2_listener *listener, void *data);
#define ZWP_TABLET_V2_DESTROY 0U
void zwp_tablet_v2_destroy(struct zwp_tablet_v2 *zwp_tablet_v2);

/* zwp_tablet_pad_v2: a tablet's buttons and rings (never announced by the compositor). */
#define ZWP_TABLET_PAD_V2_SET_FEEDBACK 0U
#define ZWP_TABLET_PAD_V2_DESTROY 1U
void zwp_tablet_pad_v2_destroy(struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2);

#ifdef __cplusplus
}
#endif

#endif
