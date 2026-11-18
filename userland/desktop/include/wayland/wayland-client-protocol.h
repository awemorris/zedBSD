/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Declares the selected core protocol objects and typed requests. */

#ifndef KERN_WAYLAND_CLIENT_PROTOCOL_H
#define KERN_WAYLAND_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_registry;
struct wl_callback;
struct wl_compositor;
struct wl_surface;
struct wl_region;
struct wl_buffer;
struct wl_output;
struct wl_seat;
struct wl_pointer;
struct wl_keyboard;
struct wl_touch;
struct xdg_wm_base;
struct xdg_positioner;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_popup;

struct wl_display;
extern const struct wl_interface wl_display_interface;

/* Receives events for one wl_display object; retained by its proxy. */
struct wl_display_listener {
	void (*error)(void *data, struct wl_display *object, struct wl_proxy *object_id, uint32_t code, const char *message);
	void (*delete_id)(void *data, struct wl_display *object, uint32_t id);
};

int wl_display_add_listener(struct wl_display *object, const struct wl_display_listener *listener, void *data);
#define WL_DISPLAY_SYNC 0U
struct wl_callback *wl_display_sync(struct wl_display *object);
#define WL_DISPLAY_GET_REGISTRY 1U
struct wl_registry *wl_display_get_registry(struct wl_display *object);
void wl_display_set_user_data(struct wl_display *object, void *data);
void *wl_display_get_user_data(struct wl_display *object);
uint32_t wl_display_get_version(struct wl_display *object);

struct wl_registry;
extern const struct wl_interface wl_registry_interface;

/* Receives events for one wl_registry object; retained by its proxy. */
struct wl_registry_listener {
	void (*global)(void *data, struct wl_registry *object, uint32_t name, const char *interface_name, uint32_t version);
	void (*global_remove)(void *data, struct wl_registry *object, uint32_t name);
};

int wl_registry_add_listener(struct wl_registry *object, const struct wl_registry_listener *listener, void *data);
void wl_registry_destroy(struct wl_registry *object);
void wl_registry_set_user_data(struct wl_registry *object, void *data);
void *wl_registry_get_user_data(struct wl_registry *object);
uint32_t wl_registry_get_version(struct wl_registry *object);

struct wl_callback;
extern const struct wl_interface wl_callback_interface;

/* Receives events for one wl_callback object; retained by its proxy. */
struct wl_callback_listener {
	void (*done)(void *data, struct wl_callback *object, uint32_t callback_data);
};

int wl_callback_add_listener(struct wl_callback *object, const struct wl_callback_listener *listener, void *data);
void wl_callback_destroy(struct wl_callback *object);
void wl_callback_set_user_data(struct wl_callback *object, void *data);
void *wl_callback_get_user_data(struct wl_callback *object);
uint32_t wl_callback_get_version(struct wl_callback *object);

struct wl_compositor;
extern const struct wl_interface wl_compositor_interface;
#define WL_COMPOSITOR_CREATE_SURFACE 0U
struct wl_surface *wl_compositor_create_surface(struct wl_compositor *object);
#define WL_COMPOSITOR_CREATE_REGION 1U
struct wl_region *wl_compositor_create_region(struct wl_compositor *object);
void wl_compositor_destroy(struct wl_compositor *object);
void wl_compositor_set_user_data(struct wl_compositor *object, void *data);
void *wl_compositor_get_user_data(struct wl_compositor *object);
uint32_t wl_compositor_get_version(struct wl_compositor *object);

struct wl_surface;
extern const struct wl_interface wl_surface_interface;

/* Receives events for one wl_surface object; retained by its proxy. */
struct wl_surface_listener {
	void (*enter)(void *data, struct wl_surface *object, struct wl_output *output);
	void (*leave)(void *data, struct wl_surface *object, struct wl_output *output);
	/* Version 6; called only on a surface of version 6 or later. */
	void (*preferred_buffer_scale)(void *data, struct wl_surface *object, int32_t factor);
	void (*preferred_buffer_transform)(void *data, struct wl_surface *object, uint32_t transform);
};

int wl_surface_add_listener(struct wl_surface *object, const struct wl_surface_listener *listener, void *data);
#define WL_SURFACE_DESTROY 0U
void wl_surface_destroy(struct wl_surface *object);
#define WL_SURFACE_ATTACH 1U
void wl_surface_attach(struct wl_surface *object, struct wl_buffer *buffer, int32_t x, int32_t y);
#define WL_SURFACE_DAMAGE 2U
void wl_surface_damage(struct wl_surface *object, int32_t x, int32_t y, int32_t width, int32_t height);
#define WL_SURFACE_FRAME 3U
struct wl_callback *wl_surface_frame(struct wl_surface *object);
#define WL_SURFACE_SET_OPAQUE_REGION 4U
void wl_surface_set_opaque_region(struct wl_surface *object, struct wl_region *region);
#define WL_SURFACE_SET_INPUT_REGION 5U
void wl_surface_set_input_region(struct wl_surface *object, struct wl_region *region);
#define WL_SURFACE_COMMIT 6U
void wl_surface_commit(struct wl_surface *object);
#define WL_SURFACE_SET_BUFFER_TRANSFORM 7U
void wl_surface_set_buffer_transform(struct wl_surface *object, int32_t transform);
#define WL_SURFACE_SET_BUFFER_SCALE 8U
void wl_surface_set_buffer_scale(struct wl_surface *object, int32_t scale);
#define WL_SURFACE_DAMAGE_BUFFER 9U
void wl_surface_damage_buffer(struct wl_surface *object, int32_t x, int32_t y, int32_t width, int32_t height);
#define WL_SURFACE_OFFSET 10U
void wl_surface_offset(struct wl_surface *object, int32_t x, int32_t y);
#define WL_SURFACE_ENTER_SINCE_VERSION 1
#define WL_SURFACE_LEAVE_SINCE_VERSION 1
#define WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION 6
#define WL_SURFACE_PREFERRED_BUFFER_TRANSFORM_SINCE_VERSION 6
#define WL_SURFACE_DESTROY_SINCE_VERSION 1
#define WL_SURFACE_ATTACH_SINCE_VERSION 1
#define WL_SURFACE_DAMAGE_SINCE_VERSION 1
#define WL_SURFACE_FRAME_SINCE_VERSION 1
#define WL_SURFACE_SET_OPAQUE_REGION_SINCE_VERSION 1
#define WL_SURFACE_SET_INPUT_REGION_SINCE_VERSION 1
#define WL_SURFACE_COMMIT_SINCE_VERSION 1
#define WL_SURFACE_SET_BUFFER_TRANSFORM_SINCE_VERSION 2
#define WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION 3
#define WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION 4
#define WL_SURFACE_OFFSET_SINCE_VERSION 5
void wl_surface_set_user_data(struct wl_surface *object, void *data);
void *wl_surface_get_user_data(struct wl_surface *object);
uint32_t wl_surface_get_version(struct wl_surface *object);

struct wl_region;
extern const struct wl_interface wl_region_interface;
#define WL_REGION_DESTROY 0U
void wl_region_destroy(struct wl_region *object);
#define WL_REGION_ADD 1U
void wl_region_add(struct wl_region *object, int32_t x, int32_t y, int32_t width, int32_t height);
#define WL_REGION_SUBTRACT 2U
void wl_region_subtract(struct wl_region *object, int32_t x, int32_t y, int32_t width, int32_t height);
void wl_region_set_user_data(struct wl_region *object, void *data);
void *wl_region_get_user_data(struct wl_region *object);
uint32_t wl_region_get_version(struct wl_region *object);

/* wl_subcompositor (WS035 p077): gives surfaces the sub-surface role. */
struct wl_subcompositor;
struct wl_subsurface;
extern const struct wl_interface wl_subcompositor_interface;
#define WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE 0U
#define WL_SUBCOMPOSITOR_ERROR_BAD_PARENT 1U
#define WL_SUBCOMPOSITOR_DESTROY 0U
void wl_subcompositor_destroy(struct wl_subcompositor *object);
#define WL_SUBCOMPOSITOR_GET_SUBSURFACE 1U
struct wl_subsurface *wl_subcompositor_get_subsurface(struct wl_subcompositor *object, struct wl_surface *surface, struct wl_surface *parent);
void wl_subcompositor_set_user_data(struct wl_subcompositor *object, void *data);
void *wl_subcompositor_get_user_data(struct wl_subcompositor *object);
uint32_t wl_subcompositor_get_version(struct wl_subcompositor *object);

/* wl_subsurface (WS035 p077): a surface shown with its parent. */
extern const struct wl_interface wl_subsurface_interface;
#define WL_SUBSURFACE_ERROR_BAD_SURFACE 0U
#define WL_SUBSURFACE_DESTROY 0U
void wl_subsurface_destroy(struct wl_subsurface *object);
#define WL_SUBSURFACE_SET_POSITION 1U
void wl_subsurface_set_position(struct wl_subsurface *object, int32_t x, int32_t y);
#define WL_SUBSURFACE_PLACE_ABOVE 2U
void wl_subsurface_place_above(struct wl_subsurface *object, struct wl_surface *sibling);
#define WL_SUBSURFACE_PLACE_BELOW 3U
void wl_subsurface_place_below(struct wl_subsurface *object, struct wl_surface *sibling);
#define WL_SUBSURFACE_SET_SYNC 4U
void wl_subsurface_set_sync(struct wl_subsurface *object);
#define WL_SUBSURFACE_SET_DESYNC 5U
void wl_subsurface_set_desync(struct wl_subsurface *object);
void wl_subsurface_set_user_data(struct wl_subsurface *object, void *data);
void *wl_subsurface_get_user_data(struct wl_subsurface *object);
uint32_t wl_subsurface_get_version(struct wl_subsurface *object);

/* The data-sharing interfaces (WS035 p079): the clipboard and drag and drop between clients. */
struct wl_data_offer;
struct wl_data_source;
struct wl_data_device;
struct wl_data_device_manager;
extern const struct wl_interface wl_data_offer_interface;
extern const struct wl_interface wl_data_source_interface;
extern const struct wl_interface wl_data_device_interface;
extern const struct wl_interface wl_data_device_manager_interface;

/* The drag and drop actions (wl_data_device_manager.dnd_action). */
enum wl_data_device_manager_dnd_action {
	WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE = 0,
	WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY = 1,
	WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE = 2,
	WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK = 4
};

/* Receives events for one wl_data_offer object; retained by its proxy. */
struct wl_data_offer_listener {
	void (*offer)(void *data, struct wl_data_offer *wl_data_offer, const char *mime_type);
	void (*source_actions)(void *data, struct wl_data_offer *wl_data_offer, uint32_t source_actions);
	void (*action)(void *data, struct wl_data_offer *wl_data_offer, uint32_t dnd_action);
};

/* Installs the listener, and the wl_data_offer requests. */
int wl_data_offer_add_listener(struct wl_data_offer *wl_data_offer, const struct wl_data_offer_listener *listener, void *data);
#define WL_DATA_OFFER_ACCEPT 0U
void wl_data_offer_accept(struct wl_data_offer *wl_data_offer, uint32_t serial, const char *mime_type);
#define WL_DATA_OFFER_RECEIVE 1U
void wl_data_offer_receive(struct wl_data_offer *wl_data_offer, const char *mime_type, int32_t fd);
#define WL_DATA_OFFER_DESTROY 2U
void wl_data_offer_destroy(struct wl_data_offer *wl_data_offer);
#define WL_DATA_OFFER_FINISH 3U
#define WL_DATA_OFFER_FINISH_SINCE_VERSION 3U
void wl_data_offer_finish(struct wl_data_offer *wl_data_offer);
#define WL_DATA_OFFER_SET_ACTIONS 4U
#define WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION 3U
void wl_data_offer_set_actions(struct wl_data_offer *wl_data_offer, uint32_t dnd_actions, uint32_t preferred_action);

/* Receives events for one wl_data_source object; retained by its proxy. */
struct wl_data_source_listener {
	void (*target)(void *data, struct wl_data_source *wl_data_source, const char *mime_type);
	void (*send)(void *data, struct wl_data_source *wl_data_source, const char *mime_type, int32_t fd);
	void (*cancelled)(void *data, struct wl_data_source *wl_data_source);
	void (*dnd_drop_performed)(void *data, struct wl_data_source *wl_data_source);
	void (*dnd_finished)(void *data, struct wl_data_source *wl_data_source);
	void (*action)(void *data, struct wl_data_source *wl_data_source, uint32_t dnd_action);
};

/* Installs the listener, and the wl_data_source requests. */
int wl_data_source_add_listener(struct wl_data_source *wl_data_source, const struct wl_data_source_listener *listener, void *data);
#define WL_DATA_SOURCE_OFFER 0U
void wl_data_source_offer(struct wl_data_source *wl_data_source, const char *mime_type);
#define WL_DATA_SOURCE_DESTROY 1U
void wl_data_source_destroy(struct wl_data_source *wl_data_source);
#define WL_DATA_SOURCE_SET_ACTIONS 2U
#define WL_DATA_SOURCE_SET_ACTIONS_SINCE_VERSION 3U
void wl_data_source_set_actions(struct wl_data_source *wl_data_source, uint32_t dnd_actions);

/* Receives events for one wl_data_device object; retained by its proxy. */
struct wl_data_device_listener {
	void (*data_offer)(void *data, struct wl_data_device *wl_data_device, struct wl_data_offer *id);
	void (*enter)(void *data, struct wl_data_device *wl_data_device, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *id);
	void (*leave)(void *data, struct wl_data_device *wl_data_device);
	void (*motion)(void *data, struct wl_data_device *wl_data_device, uint32_t time, wl_fixed_t x, wl_fixed_t y);
	void (*drop)(void *data, struct wl_data_device *wl_data_device);
	void (*selection)(void *data, struct wl_data_device *wl_data_device, struct wl_data_offer *id);
};

/* Installs the listener, and the wl_data_device requests. */
int wl_data_device_add_listener(struct wl_data_device *wl_data_device, const struct wl_data_device_listener *listener, void *data);
#define WL_DATA_DEVICE_START_DRAG 0U
void wl_data_device_start_drag(struct wl_data_device *wl_data_device, struct wl_data_source *source, struct wl_surface *origin, struct wl_surface *icon, uint32_t serial);
#define WL_DATA_DEVICE_SET_SELECTION 1U
void wl_data_device_set_selection(struct wl_data_device *wl_data_device, struct wl_data_source *source, uint32_t serial);
#define WL_DATA_DEVICE_RELEASE 2U
#define WL_DATA_DEVICE_RELEASE_SINCE_VERSION 2U
void wl_data_device_release(struct wl_data_device *wl_data_device);
void wl_data_device_destroy(struct wl_data_device *wl_data_device);

#define WL_DATA_DEVICE_MANAGER_CREATE_DATA_SOURCE 0U
struct wl_data_source *wl_data_device_manager_create_data_source(struct wl_data_device_manager *wl_data_device_manager);
#define WL_DATA_DEVICE_MANAGER_GET_DATA_DEVICE 1U
struct wl_data_device *wl_data_device_manager_get_data_device(struct wl_data_device_manager *wl_data_device_manager, struct wl_seat *seat);
void wl_data_device_manager_destroy(struct wl_data_device_manager *wl_data_device_manager);
uint32_t wl_data_device_manager_get_version(struct wl_data_device_manager *wl_data_device_manager);

struct wl_buffer;
extern const struct wl_interface wl_buffer_interface;

/* Receives events for one wl_buffer object; retained by its proxy. */
struct wl_buffer_listener {
	void (*release)(void *data, struct wl_buffer *object);
};

int wl_buffer_add_listener(struct wl_buffer *object, const struct wl_buffer_listener *listener, void *data);
#define WL_BUFFER_DESTROY 0U
void wl_buffer_destroy(struct wl_buffer *object);
void wl_buffer_set_user_data(struct wl_buffer *object, void *data);
void *wl_buffer_get_user_data(struct wl_buffer *object);
uint32_t wl_buffer_get_version(struct wl_buffer *object);

struct wl_shm;
struct wl_shm_pool;
extern const struct wl_interface wl_shm_interface;
extern const struct wl_interface wl_shm_pool_interface;

/* The pixel formats of wl_shm: 32-bit little-endian words with alpha, or with an unused byte. */
/*
 * The pixel formats of wl_shm buffers: ARGB8888 and XRGB8888 are 0 and 1,
 * and the rest are DRM fourcc codes.  The two first were macros here and keep
 * their values.
 */
enum wl_shm_format {
	WL_SHM_FORMAT_ARGB8888 = 0,
	WL_SHM_FORMAT_XRGB8888 = 1,
	WL_SHM_FORMAT_C8 = 0x20203843,
	WL_SHM_FORMAT_RGB332 = 0x38424752,
	WL_SHM_FORMAT_BGR233 = 0x38524742,
	WL_SHM_FORMAT_XRGB4444 = 0x32315258,
	WL_SHM_FORMAT_XBGR4444 = 0x32314258,
	WL_SHM_FORMAT_RGBX4444 = 0x32315852,
	WL_SHM_FORMAT_BGRX4444 = 0x32315842,
	WL_SHM_FORMAT_ARGB4444 = 0x32315241,
	WL_SHM_FORMAT_ABGR4444 = 0x32314241,
	WL_SHM_FORMAT_RGBA4444 = 0x32314152,
	WL_SHM_FORMAT_BGRA4444 = 0x32314142,
	WL_SHM_FORMAT_XRGB1555 = 0x35315258,
	WL_SHM_FORMAT_XBGR1555 = 0x35314258,
	WL_SHM_FORMAT_RGBX5551 = 0x35315852,
	WL_SHM_FORMAT_BGRX5551 = 0x35315842,
	WL_SHM_FORMAT_ARGB1555 = 0x35315241,
	WL_SHM_FORMAT_ABGR1555 = 0x35314241,
	WL_SHM_FORMAT_RGBA5551 = 0x35314152,
	WL_SHM_FORMAT_BGRA5551 = 0x35314142,
	WL_SHM_FORMAT_RGB565 = 0x36314752,
	WL_SHM_FORMAT_BGR565 = 0x36314742,
	WL_SHM_FORMAT_RGB888 = 0x34324752,
	WL_SHM_FORMAT_BGR888 = 0x34324742,
	WL_SHM_FORMAT_XBGR8888 = 0x34324258,
	WL_SHM_FORMAT_RGBX8888 = 0x34325852,
	WL_SHM_FORMAT_BGRX8888 = 0x34325842,
	WL_SHM_FORMAT_ABGR8888 = 0x34324241,
	WL_SHM_FORMAT_RGBA8888 = 0x34324152,
	WL_SHM_FORMAT_BGRA8888 = 0x34324142,
	WL_SHM_FORMAT_XRGB2101010 = 0x30335258,
	WL_SHM_FORMAT_XBGR2101010 = 0x30334258,
	WL_SHM_FORMAT_RGBX1010102 = 0x30335852,
	WL_SHM_FORMAT_BGRX1010102 = 0x30335842,
	WL_SHM_FORMAT_ARGB2101010 = 0x30335241,
	WL_SHM_FORMAT_ABGR2101010 = 0x30334241,
	WL_SHM_FORMAT_RGBA1010102 = 0x30334152,
	WL_SHM_FORMAT_BGRA1010102 = 0x30334142,
	WL_SHM_FORMAT_YUYV = 0x56595559,
	WL_SHM_FORMAT_YVYU = 0x55595659,
	WL_SHM_FORMAT_UYVY = 0x59565955,
	WL_SHM_FORMAT_VYUY = 0x59555956,
	WL_SHM_FORMAT_AYUV = 0x56555941,
	WL_SHM_FORMAT_NV12 = 0x3231564e,
	WL_SHM_FORMAT_NV21 = 0x3132564e,
	WL_SHM_FORMAT_NV16 = 0x3631564e,
	WL_SHM_FORMAT_NV61 = 0x3136564e,
	WL_SHM_FORMAT_YUV410 = 0x39565559,
	WL_SHM_FORMAT_YVU410 = 0x39555659,
	WL_SHM_FORMAT_YUV411 = 0x31315559,
	WL_SHM_FORMAT_YVU411 = 0x31315659,
	WL_SHM_FORMAT_YUV420 = 0x32315559,
	WL_SHM_FORMAT_YVU420 = 0x32315659,
	WL_SHM_FORMAT_YUV422 = 0x36315559,
	WL_SHM_FORMAT_YVU422 = 0x36315659,
	WL_SHM_FORMAT_YUV444 = 0x34325559,
	WL_SHM_FORMAT_YVU444 = 0x34325659,
	WL_SHM_FORMAT_R8 = 0x20203852,
	WL_SHM_FORMAT_R16 = 0x20363152,
	WL_SHM_FORMAT_RG88 = 0x38384752,
	WL_SHM_FORMAT_GR88 = 0x38385247,
	WL_SHM_FORMAT_RG1616 = 0x32334752,
	WL_SHM_FORMAT_GR1616 = 0x32335247,
	WL_SHM_FORMAT_XRGB16161616F = 0x48345258,
	WL_SHM_FORMAT_XBGR16161616F = 0x48344258,
	WL_SHM_FORMAT_ARGB16161616F = 0x48345241,
	WL_SHM_FORMAT_ABGR16161616F = 0x48344241,
	WL_SHM_FORMAT_XYUV8888 = 0x56555958,
	WL_SHM_FORMAT_VUY888 = 0x34325556,
	WL_SHM_FORMAT_VUY101010 = 0x30335556,
	WL_SHM_FORMAT_Y210 = 0x30313259,
	WL_SHM_FORMAT_Y212 = 0x32313259,
	WL_SHM_FORMAT_Y216 = 0x36313259,
	WL_SHM_FORMAT_Y410 = 0x30313459,
	WL_SHM_FORMAT_Y412 = 0x32313459,
	WL_SHM_FORMAT_Y416 = 0x36313459,
	WL_SHM_FORMAT_XVYU2101010 = 0x30335658,
	WL_SHM_FORMAT_XVYU12_16161616 = 0x36335658,
	WL_SHM_FORMAT_XVYU16161616 = 0x38345658,
	WL_SHM_FORMAT_Y0L0 = 0x304c3059,
	WL_SHM_FORMAT_X0L0 = 0x304c3058,
	WL_SHM_FORMAT_Y0L2 = 0x324c3059,
	WL_SHM_FORMAT_X0L2 = 0x324c3058,
	WL_SHM_FORMAT_YUV420_8BIT = 0x38305559,
	WL_SHM_FORMAT_YUV420_10BIT = 0x30315559,
	WL_SHM_FORMAT_XRGB8888_A8 = 0x38415258,
	WL_SHM_FORMAT_XBGR8888_A8 = 0x38414258,
	WL_SHM_FORMAT_RGBX8888_A8 = 0x38415852,
	WL_SHM_FORMAT_BGRX8888_A8 = 0x38415842,
	WL_SHM_FORMAT_RGB888_A8 = 0x38413852,
	WL_SHM_FORMAT_BGR888_A8 = 0x38413842,
	WL_SHM_FORMAT_RGB565_A8 = 0x38413552,
	WL_SHM_FORMAT_BGR565_A8 = 0x38413542,
	WL_SHM_FORMAT_NV24 = 0x3432564e,
	WL_SHM_FORMAT_NV42 = 0x3234564e,
	WL_SHM_FORMAT_P210 = 0x30313250,
	WL_SHM_FORMAT_P010 = 0x30313050,
	WL_SHM_FORMAT_P012 = 0x32313050,
	WL_SHM_FORMAT_P016 = 0x36313050,
	WL_SHM_FORMAT_AXBXGXRX106106106106 = 0x30314241,
	WL_SHM_FORMAT_NV15 = 0x3531564e,
	WL_SHM_FORMAT_Q410 = 0x30313451,
	WL_SHM_FORMAT_Q401 = 0x31303451,
	WL_SHM_FORMAT_XRGB16161616 = 0x38345258,
	WL_SHM_FORMAT_XBGR16161616 = 0x38344258,
	WL_SHM_FORMAT_ARGB16161616 = 0x38345241,
	WL_SHM_FORMAT_ABGR16161616 = 0x38344241,
	WL_SHM_FORMAT_C1 = 0x20203143,
	WL_SHM_FORMAT_C2 = 0x20203243,
	WL_SHM_FORMAT_C4 = 0x20203443,
	WL_SHM_FORMAT_D1 = 0x20203144,
	WL_SHM_FORMAT_D2 = 0x20203244,
	WL_SHM_FORMAT_D4 = 0x20203444,
	WL_SHM_FORMAT_D8 = 0x20203844,
	WL_SHM_FORMAT_R1 = 0x20203152,
	WL_SHM_FORMAT_R2 = 0x20203252,
	WL_SHM_FORMAT_R4 = 0x20203452,
	WL_SHM_FORMAT_R10 = 0x20303152,
	WL_SHM_FORMAT_R12 = 0x20323152,
	WL_SHM_FORMAT_AVUY8888 = 0x59555641,
	WL_SHM_FORMAT_XVUY8888 = 0x59555658,
	WL_SHM_FORMAT_P030 = 0x30333050
};

/* Receives the formats the compositor accepts for shared-memory buffers. */
struct wl_shm_listener {
	void (*format)(void *data, struct wl_shm *object, uint32_t format);
};

int wl_shm_add_listener(struct wl_shm *object, const struct wl_shm_listener *listener, void *data);
#define WL_SHM_CREATE_POOL 0U
struct wl_shm_pool *wl_shm_create_pool(struct wl_shm *object, int32_t fd, int32_t size);
void wl_shm_destroy(struct wl_shm *object);
#define WL_SHM_POOL_CREATE_BUFFER 0U
struct wl_buffer *wl_shm_pool_create_buffer(struct wl_shm_pool *object, int32_t offset, int32_t width, int32_t height, int32_t stride, uint32_t format);
#define WL_SHM_POOL_DESTROY 1U
void wl_shm_pool_destroy(struct wl_shm_pool *object);
#define WL_SHM_POOL_RESIZE 2U
void wl_shm_pool_resize(struct wl_shm_pool *object, int32_t size);

struct wl_output;
extern const struct wl_interface wl_output_interface;

/* Receives events for one wl_output object; retained by its proxy. */
struct wl_output_listener {
	void (*geometry)(void *data, struct wl_output *object, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform);
	void (*mode)(void *data, struct wl_output *object, uint32_t flags, int32_t width, int32_t height, int32_t refresh);
	void (*done)(void *data, struct wl_output *object);
	void (*scale)(void *data, struct wl_output *object, int32_t factor);
	void (*name)(void *data, struct wl_output *object, const char *name);
	void (*description)(void *data, struct wl_output *object, const char *description);
};

int wl_output_add_listener(struct wl_output *object, const struct wl_output_listener *listener, void *data);
#define WL_OUTPUT_RELEASE 0U
void wl_output_release(struct wl_output *object);
void wl_output_destroy(struct wl_output *object);
#define WL_OUTPUT_GEOMETRY_SINCE_VERSION 1
#define WL_OUTPUT_MODE_SINCE_VERSION 1
#define WL_OUTPUT_DONE_SINCE_VERSION 2
#define WL_OUTPUT_SCALE_SINCE_VERSION 2
#define WL_OUTPUT_NAME_SINCE_VERSION 4
#define WL_OUTPUT_DESCRIPTION_SINCE_VERSION 4
#define WL_OUTPUT_RELEASE_SINCE_VERSION 3
void wl_output_set_user_data(struct wl_output *object, void *data);
void *wl_output_get_user_data(struct wl_output *object);
uint32_t wl_output_get_version(struct wl_output *object);

struct wl_seat;
extern const struct wl_interface wl_seat_interface;

/* Names the input device classes a seat currently offers; values are bits. */
enum wl_seat_capability {
	WL_SEAT_CAPABILITY_POINTER = 1,
	WL_SEAT_CAPABILITY_KEYBOARD = 2,
	WL_SEAT_CAPABILITY_TOUCH = 4
};

/* Names the protocol errors a seat can raise. */
enum wl_seat_error {
	WL_SEAT_ERROR_MISSING_CAPABILITY = 0
};

/* Receives events for one wl_seat object; retained by its proxy. */
struct wl_seat_listener {
	void (*capabilities)(void *data, struct wl_seat *wl_seat, uint32_t capabilities);
	void (*name)(void *data, struct wl_seat *wl_seat, const char *name);
};

int wl_seat_add_listener(struct wl_seat *wl_seat, const struct wl_seat_listener *listener, void *data);
#define WL_SEAT_GET_POINTER 0U
#define WL_SEAT_GET_KEYBOARD 1U
#define WL_SEAT_GET_TOUCH 2U
#define WL_SEAT_RELEASE 3U
#define WL_SEAT_CAPABILITIES_SINCE_VERSION 1
#define WL_SEAT_NAME_SINCE_VERSION 2
#define WL_SEAT_GET_POINTER_SINCE_VERSION 1
#define WL_SEAT_GET_KEYBOARD_SINCE_VERSION 1
#define WL_SEAT_GET_TOUCH_SINCE_VERSION 1
#define WL_SEAT_RELEASE_SINCE_VERSION 5
struct wl_pointer *wl_seat_get_pointer(struct wl_seat *wl_seat);
struct wl_keyboard *wl_seat_get_keyboard(struct wl_seat *wl_seat);
struct wl_touch *wl_seat_get_touch(struct wl_seat *wl_seat);
void wl_seat_release(struct wl_seat *wl_seat);
void wl_seat_destroy(struct wl_seat *wl_seat);
void wl_seat_set_user_data(struct wl_seat *wl_seat, void *data);
void *wl_seat_get_user_data(struct wl_seat *wl_seat);
uint32_t wl_seat_get_version(struct wl_seat *wl_seat);

struct wl_pointer;
extern const struct wl_interface wl_pointer_interface;

/* Names the protocol errors a pointer can raise. */
enum wl_pointer_error {
	WL_POINTER_ERROR_ROLE = 0
};

/* Says whether a button event reports a press or a release. */
enum wl_pointer_button_state {
	WL_POINTER_BUTTON_STATE_RELEASED = 0,
	WL_POINTER_BUTTON_STATE_PRESSED = 1
};

/* Names the scroll axis an axis event moves. */
enum wl_pointer_axis {
	WL_POINTER_AXIS_VERTICAL_SCROLL = 0,
	WL_POINTER_AXIS_HORIZONTAL_SCROLL = 1
};

/* Names the physical source of a group of axis events. */
enum wl_pointer_axis_source {
	WL_POINTER_AXIS_SOURCE_WHEEL = 0,
	WL_POINTER_AXIS_SOURCE_FINGER = 1,
	WL_POINTER_AXIS_SOURCE_CONTINUOUS = 2,
	WL_POINTER_AXIS_SOURCE_WHEEL_TILT = 3
};

/*
 * Receives events for one wl_pointer object; retained by its proxy.
 *
 * The last two members exist for source compatibility with the upstream
 * listener layout.  They belong to pointer versions 8 and 9, which this
 * library does not describe, so they are never invoked.
 */
struct wl_pointer_listener {
	void (*enter)(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y);
	void (*leave)(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface);
	void (*motion)(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y);
	void (*button)(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
	void (*axis)(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value);
	void (*frame)(void *data, struct wl_pointer *wl_pointer);
	void (*axis_source)(void *data, struct wl_pointer *wl_pointer, uint32_t axis_source);
	void (*axis_stop)(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis);
	void (*axis_discrete)(void *data, struct wl_pointer *wl_pointer, uint32_t axis, int32_t discrete);
	void (*axis_value120)(void *data, struct wl_pointer *wl_pointer, uint32_t axis, int32_t value120);
	void (*axis_relative_direction)(void *data, struct wl_pointer *wl_pointer, uint32_t axis, uint32_t direction);
};

int wl_pointer_add_listener(struct wl_pointer *wl_pointer, const struct wl_pointer_listener *listener, void *data);
#define WL_POINTER_SET_CURSOR 0U
#define WL_POINTER_RELEASE 1U
#define WL_POINTER_ENTER_SINCE_VERSION 1
#define WL_POINTER_LEAVE_SINCE_VERSION 1
#define WL_POINTER_MOTION_SINCE_VERSION 1
#define WL_POINTER_BUTTON_SINCE_VERSION 1
#define WL_POINTER_AXIS_SINCE_VERSION 1
#define WL_POINTER_FRAME_SINCE_VERSION 5
#define WL_POINTER_AXIS_SOURCE_SINCE_VERSION 5
#define WL_POINTER_AXIS_STOP_SINCE_VERSION 5
#define WL_POINTER_AXIS_DISCRETE_SINCE_VERSION 5
#define WL_POINTER_AXIS_VALUE120_SINCE_VERSION 8
#define WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION 9
#define WL_POINTER_SET_CURSOR_SINCE_VERSION 1
#define WL_POINTER_RELEASE_SINCE_VERSION 3
void wl_pointer_set_cursor(struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, int32_t hotspot_x, int32_t hotspot_y);
void wl_pointer_release(struct wl_pointer *wl_pointer);
void wl_pointer_destroy(struct wl_pointer *wl_pointer);
void wl_pointer_set_user_data(struct wl_pointer *wl_pointer, void *data);
void *wl_pointer_get_user_data(struct wl_pointer *wl_pointer);
uint32_t wl_pointer_get_version(struct wl_pointer *wl_pointer);

struct wl_keyboard;
extern const struct wl_interface wl_keyboard_interface;

/* Names the format of the keymap file a keyboard supplies. */
enum wl_keyboard_keymap_format {
	WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP = 0,
	WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 = 1
};

/* Says whether a key event reports a press or a release. */
enum wl_keyboard_key_state {
	WL_KEYBOARD_KEY_STATE_RELEASED = 0,
	WL_KEYBOARD_KEY_STATE_PRESSED = 1
};

/* Receives events for one wl_keyboard object; retained by its proxy. */
struct wl_keyboard_listener {
	void (*keymap)(void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size);
	void (*enter)(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys);
	void (*leave)(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface);
	void (*key)(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
	void (*modifiers)(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group);
	void (*repeat_info)(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay);
};

int wl_keyboard_add_listener(struct wl_keyboard *wl_keyboard, const struct wl_keyboard_listener *listener, void *data);
#define WL_KEYBOARD_RELEASE 0U
#define WL_KEYBOARD_KEYMAP_SINCE_VERSION 1
#define WL_KEYBOARD_ENTER_SINCE_VERSION 1
#define WL_KEYBOARD_LEAVE_SINCE_VERSION 1
#define WL_KEYBOARD_KEY_SINCE_VERSION 1
#define WL_KEYBOARD_MODIFIERS_SINCE_VERSION 1
#define WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION 4
#define WL_KEYBOARD_RELEASE_SINCE_VERSION 3
void wl_keyboard_release(struct wl_keyboard *wl_keyboard);
void wl_keyboard_destroy(struct wl_keyboard *wl_keyboard);
void wl_keyboard_set_user_data(struct wl_keyboard *wl_keyboard, void *data);
void *wl_keyboard_get_user_data(struct wl_keyboard *wl_keyboard);
uint32_t wl_keyboard_get_version(struct wl_keyboard *wl_keyboard);

struct wl_touch;
extern const struct wl_interface wl_touch_interface;

/*
 * Receives events for one wl_touch object; retained by its proxy.  shape and
 * orientation are version 6 events, never sent to a version 5 wl_touch.
 */
struct wl_touch_listener {
	void (*down)(void *data, struct wl_touch *wl_touch, uint32_t serial, uint32_t time, struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y);
	void (*up)(void *data, struct wl_touch *wl_touch, uint32_t serial, uint32_t time, int32_t id);
	void (*motion)(void *data, struct wl_touch *wl_touch, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y);
	void (*frame)(void *data, struct wl_touch *wl_touch);
	void (*cancel)(void *data, struct wl_touch *wl_touch);
	void (*shape)(void *data, struct wl_touch *wl_touch, int32_t id, wl_fixed_t major, wl_fixed_t minor);
	void (*orientation)(void *data, struct wl_touch *wl_touch, int32_t id, wl_fixed_t orientation);
};

int wl_touch_add_listener(struct wl_touch *wl_touch, const struct wl_touch_listener *listener, void *data);
#define WL_TOUCH_RELEASE 0U
#define WL_TOUCH_DOWN_SINCE_VERSION 1
#define WL_TOUCH_UP_SINCE_VERSION 1
#define WL_TOUCH_MOTION_SINCE_VERSION 1
#define WL_TOUCH_FRAME_SINCE_VERSION 1
#define WL_TOUCH_CANCEL_SINCE_VERSION 1
#define WL_TOUCH_SHAPE_SINCE_VERSION 6
#define WL_TOUCH_ORIENTATION_SINCE_VERSION 6
#define WL_TOUCH_RELEASE_SINCE_VERSION 3
void wl_touch_release(struct wl_touch *wl_touch);
void wl_touch_destroy(struct wl_touch *wl_touch);
void wl_touch_set_user_data(struct wl_touch *wl_touch, void *data);
void *wl_touch_get_user_data(struct wl_touch *wl_touch);
uint32_t wl_touch_get_version(struct wl_touch *wl_touch);

#define WL_REGISTRY_BIND 0U
void *wl_registry_bind(struct wl_registry *registry, uint32_t name, const struct wl_interface *interface, uint32_t version);

/* Flags of wl_output.mode; the values the macros here had. */
enum wl_output_mode {
	WL_OUTPUT_MODE_CURRENT = 0x1,
	WL_OUTPUT_MODE_PREFERRED = 0x2
};

/* The subpixel layouts wl_output.geometry reports. */
enum wl_output_subpixel {
	WL_OUTPUT_SUBPIXEL_UNKNOWN = 0,
	WL_OUTPUT_SUBPIXEL_NONE = 1,
	WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB = 2,
	WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR = 3,
	WL_OUTPUT_SUBPIXEL_VERTICAL_RGB = 4,
	WL_OUTPUT_SUBPIXEL_VERTICAL_BGR = 5
};

/* The rotations and flips of an output or a buffer, counter-clockwise. */
enum wl_output_transform {
	WL_OUTPUT_TRANSFORM_NORMAL = 0,
	WL_OUTPUT_TRANSFORM_90 = 1,
	WL_OUTPUT_TRANSFORM_180 = 2,
	WL_OUTPUT_TRANSFORM_270 = 3,
	WL_OUTPUT_TRANSFORM_FLIPPED = 4,
	WL_OUTPUT_TRANSFORM_FLIPPED_90 = 5,
	WL_OUTPUT_TRANSFORM_FLIPPED_180 = 6,
	WL_OUTPUT_TRANSFORM_FLIPPED_270 = 7
};

#ifdef __cplusplus
}
#endif

#endif
