/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's parts the library's files share (window.c, present.c,
 * present-shm.c, clipboard.c, primary.c): the window's record, its
 * presenters, and the clipboard's and the primary selection's calls.
 * Nothing here leaves the library.
 */

#ifndef KEIUI_WINDOW_H
#define KEIUI_WINDOW_H

#include <keiland/keiland.h>

#include "declare.h"

/* The window's surface is a Wayland one, for Vulkan's surface. */
#define VK_USE_PLATFORM_WAYLAND_KHR 1

#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

/* The most tabs a window's titlebar shows, and the longest tab title kept (WS131 p018). */
#define KEIUI_WINDOW_TABS	32U
#define KEIUI_TAB_TITLE		128U

/* The most tools of a pen tablet a window follows (WS131 p018, Notes' tablet.c moved here). */
#define KEIUI_TABLET_TOOLS	4U

/* The most types a drag of the window's own offers (WS131 p020: Files' file names and their text). */
#define KEIUI_DRAG_TYPES	4U

/* How many inputs wait at most (the oldest is dropped past it). */
#define KEIUI_WINDOW_EVENTS	512U

/* The most fingers followed on the program's other surfaces. */
#define KEIUI_WINDOW_FOREIGN	10U

/* The shared-memory buffers a frame is drawn into, one while the compositor reads the other. */
#define KEIUI_SHM_BUFFERS	2

/* The most globals an application keeps, the longest interface name, and the events it queues at most. */
#define KEIUI_APP_GLOBALS	96U
#define KEIUI_APP_INTERFACE	64U
#define KEIUI_APP_EVENTS	512U

struct zwp_primary_selection_device_manager_v1;
struct kl_keyboard_inset;
struct kl_edit;
struct zwp_primary_selection_device_v1;
struct zwp_primary_selection_source_v1;
struct zwp_primary_selection_offer_v1;
struct kl_menu_service;
struct kl_menu;
struct kl_window_menu;
struct kl_context_menu;
struct kl_titlebar;
struct kl_glass;
struct zwp_tablet_manager_v2;
struct zwp_tablet_seat_v2;
struct zwp_tablet_tool_v2;
struct kl_window;

/*
 * One tool of a pen tablet (tablet.c): its window and object, its kind
 * (KL_TABLET_*) and whether it reports pressure; whether it is over the
 * window, touching, in a contact told to the application, lifting and
 * leaving in this frame, and moved in it; its buttons held
 * (KL_TABLET_BUTTON_*); and its place, pressure (0 to 1) and tilt.
 */
struct keiui_tablet_tool {
	struct kl_window *window;
	struct zwp_tablet_tool_v2 *tool;
	unsigned type;
	int has_pressure;
	int near;
	int touching;
	int down;
	int lifting;
	int leaving;
	int moved;
	unsigned buttons;
	double x;
	double y;
	double pressure;
	double tilt_x;
	double tilt_y;
};

/* One tab as the titlebar shows it (WS131 p018): its ID, title and KL_TAB_* flags. */
struct keiui_tab {
	uint32_t id;
	char title[KEIUI_TAB_TITLE];
	unsigned flags;
};

/* One global the compositor announced: its name, version and interface. */
struct keiui_app_global {
	uint32_t name;
	uint32_t version;
	char interface[KEIUI_APP_INTERFACE];
};

/*
 * One application (app.c, WS131 p015), from kl_app_open to kl_app_close:
 * its connection and the one registry, the globals it announced, the
 * app_id its windows get, its windows, the events queued (a ring), the
 * descriptors watched, the system and the menu service (made when first
 * asked for), and the next application open (the library's list, read by
 * libkeiland's objects to bind from an application's registry).
 */
struct kl_app {
	struct wl_display *display;
	struct wl_registry *registry;
	struct keiui_app_global globals[KEIUI_APP_GLOBALS];
	unsigned global_count;
	char *application;

	/* The windows, a list through their app_next. */
	struct kl_window *windows;

	/* The events waiting: the oldest's slot and how many. */
	struct kl_app_event events[KEIUI_APP_EVENTS];
	unsigned event_first;
	unsigned event_count;

	/* The descriptors watched and what for (KL_APP_FD_*). */
	int fds[KL_APP_FDS_MAX];
	unsigned fd_events[KL_APP_FDS_MAX];
	unsigned fd_count;

	/* The system and the menu service, and whether the service was tried (a compositor without it says so once). */
	struct kl_system *system;
	struct kl_menu_service *menu_service;
	int menu_tried;

	/* The desktop's appearance watched (ws089-p017; NULL under a compositor without it). */
	struct kl_appearance *appearance;

	struct kl_app *next;
};

/* One swapchain image and what draws into it. */
struct keiui_present_target {
	VkImage image;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkSemaphore rendered;
};

/*
 * The Vulkan presenter (Text Editor's present.c): the swapchain over the
 * window's surface, and the quad that copies the CPU's frame onto an
 * image of it.  It lives from the window's opening to its closing.
 */
struct keiui_present {
	/* The instance, the surface of the window, the device and its queue. */
	VkInstance instance;
	VkSurfaceKHR surface;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;

	/* The swapchain, its format and extent, whether it is see-through, and one target per image. */
	VkSwapchainKHR swapchain;
	VkFormat format;
	int premultiplied;
	VkExtent2D extent;
	struct keiui_present_target *targets;
	uint32_t count;

	/* The pass and the pipeline that draw the canvas, and what the pipeline binds. */
	VkRenderPass pass;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet set;
	VkSampler sampler;

	/* The canvas image (linear, host-written), its memory, view and map, and whether it was made ready. */
	VkImage canvas;
	VkDeviceMemory canvas_memory;
	VkImageView canvas_view;
	unsigned char *canvas_map;
	size_t canvas_pitch;
	int canvas_ready;

	/*
	 * Whether the canvas holds the last frame whole, so that a frame may
	 * be copied into it by its changed part alone (0 for a new canvas),
	 * and whether the device names that part to the window system
	 * (VK_KHR_incremental_present; BUG-221).
	 */
	int canvas_whole;
	int incremental;

	/* The quad's vertices, in host-visible memory. */
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;

	/* One command buffer, the fence that says it finished and the acquire semaphore. */
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkSemaphore acquired;

	/* The device's name (kl_window_device_name, WS131 p019). */
	char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];

	/* The Vulkan call that failed last, and the last frame's copy, acquire, present and wait times (milliseconds). */
	const char *operation;
	unsigned copy_ms;
	unsigned acquire_ms;
	unsigned present_ms;
	unsigned wait_ms;
};

/* One wl_shm buffer: its window, the pixels mapped, their size, and whether the compositor still reads it. */
struct keiui_shm_buffer {
	struct kl_window *window;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	size_t size;
	int width;
	int height;
	int busy;
};

/*
 * One window, from kl_window_open to kl_window_close: the connection
 * and its globals, the surface and its roles, the size, the input's state
 * and queue, the presenter, and the clipboard's and the primary
 * selection's objects and copies.
 */
struct kl_window {
	/* The connection and the globals bound from it. */
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *shell;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_touch *touch;

	/* The window: its surface and roles. */
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;

	/* The desktop's surface role instead of a toplevel (KL_VERSION 46, Files' desktop; NULL for a window), and its place on the screen. */
	struct kl_desktop *desktop;
	int32_t desktop_x;
	int32_t desktop_y;

	/* The size the compositor asked for, the largest the window may choose (0 when not known), and the size it would like. */
	uint32_t width;
	uint32_t height;
	uint32_t bounds_width;
	uint32_t bounds_height;
	uint32_t preferred_width;
	uint32_t preferred_height;

	/* Whether the first configure arrived, and whether the last one made the window fullscreen (libkeiui's version 10) or maximized (KL_VERSION 45). */
	int configured;
	int fullscreen;
	int maximized;

	/* The first screen and its current mode (KL_VERSION 45; 0 while unknown; the refresh in millihertz). */
	struct wl_output *output;
	int32_t output_width;
	int32_t output_height;
	int32_t output_refresh;

	/*
	 * The pointer's place, the modifiers held (KL_MOD_*), the serial of
	 * the last input (for the selections), and the serial of the last press
	 * of a button or a finger (a context menu opens for a press).
	 */
	double pointer_x;
	double pointer_y;
	unsigned modifiers;
	uint32_t serial;
	uint32_t press_serial;

	/*
	 * The scrolling of the pointer's frame being received (KL_VERSION 40,
	 * BUG-211): what the axis events come from (axis_source comes first in
	 * a frame; a wheel's until told otherwise, and again after the frame),
	 * and whether the frame already told the end of the fingers' scrolling.
	 */
	unsigned axis_source;
	int axis_stopped;

	/*
	 * Whether the pointer and the keyboard are on the window's own surface,
	 * and the fingers down on other surfaces of the program (a file
	 * chooser's window): their input is not the window's.
	 */
	int pointer_ours;
	int keyboard_ours;
	int32_t foreign[KEIUI_WINDOW_FOREIGN];
	unsigned foreign_count;

	/* The key held for repeating (0 when none), when it repeats next (milliseconds), and the repeat's delay and interval. */
	uint32_t repeat_key;
	uint64_t repeat_at;
	uint32_t repeat_delay;
	uint32_t repeat_interval;

	/* The inputs waiting, a ring: the oldest's slot and how many. */
	struct kl_window_event events[KEIUI_WINDOW_EVENTS];
	unsigned event_first;
	unsigned event_count;

	/* How the frames are shown, and the presenters. */
	unsigned present;
	struct keiui_present vulkan;
	struct keiui_shm_buffer buffers[KEIUI_SHM_BUFFERS];
	uint32_t shm_width;
	uint32_t shm_height;

	/*
	 * The clipboard (clipboard.c): the data device manager and the seat's
	 * device, the window's source while its text is the selection, the
	 * selection's offer and whether it has text, whether the offer being
	 * described has text, and the window's own copied text (which the
	 * source sends).
	 */
	struct wl_data_device_manager *data_manager;
	struct wl_data_device *data_device;
	struct wl_data_source *data_source;
	struct wl_data_offer *data_offer;
	int offer_text;
	int pending_text;
	char *clipboard;
	size_t clipboard_length;

	/*
	 * Drops (clipboard.c, WS131 p018, Terminal's ws035-p088 moved here):
	 * the types the window takes (KL_DROP_*, 0 refuses every drag),
	 * whether the offer being described has file names, the drag over the
	 * window (NULL for none) with its enter's serial and the types it has,
	 * and whether it was dropped and waits for kl_window_take_drop.
	 */
	unsigned drop_types;
	int pending_uris;
	struct wl_data_offer *drop_offer;
	uint32_t drop_serial;
	unsigned drop_offered;
	int drop_pending;

	/*
	 * A pen tablet (tablet.c, WS131 p018): the manager and the seat's
	 * tablets (NULL until kl_window_accept_tablet), and the tools.
	 */
	struct zwp_tablet_manager_v2 *tablet_manager;
	struct zwp_tablet_seat_v2 *tablet_seat;
	struct keiui_tablet_tool *tools[KEIUI_TABLET_TOOLS];
	unsigned tool_count;

	/* Whether a held key repeats (kl_window_set_repeat; on unless the application turned it off). */
	int repeat_off;

	/* Where the drag over the window is (surface pixels, from its enter and motions). */
	double drop_x;
	double drop_y;

	/*
	 * A drag of the window's own (Terminal's ws035-p093, Files'
	 * ws035-p084): its source (NULL for none), the types it offers with a
	 * copy of each one's data, and the action the compositor chose last.
	 */
	struct wl_data_source *drag_source;
	char *drag_types[KEIUI_DRAG_TYPES];
	char *drag_data[KEIUI_DRAG_TYPES];
	size_t drag_lengths[KEIUI_DRAG_TYPES];
	unsigned drag_count;
	unsigned drag_action;

	/* The compositor's content type manager and the surface's content type object (ws122-p005b), NULL until bound and asked. */
	struct wp_content_type_manager_v1 *content_manager;
	struct wp_content_type_v1 *content_type;

	/* The primary selection (primary.c), the same parts as the clipboard's. */
	struct zwp_primary_selection_device_manager_v1 *primary_manager;
	struct zwp_primary_selection_device_v1 *primary_device;
	struct zwp_primary_selection_source_v1 *primary_source;
	struct zwp_primary_selection_offer_v1 *primary_offer;
	int primary_offer_text;
	int primary_pending_text;
	char *primary_text;
	size_t primary_length;

	/*
	 * The text input (text-input.c, libkeiui's version 6): the compositor's
	 * manager and the seat's text input, whether the application asks for
	 * it and whether the text input is on the window's surface (enabled
	 * when both), the commits made (the done's serial), the caret's
	 * rectangle, and what the events before a done carried.
	 */
	struct zwp_text_input_manager_v3 *text_manager;
	struct zwp_text_input_v3 *text_input;
	int text_wanted;
	int text_entered;
	int text_enabled;
	uint32_t text_commits;
	int32_t text_cursor[4];
	char text_commit[KL_WINDOW_TEXT_MAX];
	char text_preedit[KL_WINDOW_TEXT_MAX];
	int32_t text_preedit_begin;
	int32_t text_preedit_end;
	int text_preedit_set;
	int text_preedit_shown;
	uint32_t text_before;
	uint32_t text_after;

	/*
	 * A window on another's connection (a file chooser's, ws090-p006): the
	 * connection is the application's and stays open, and the owner hears
	 * of queued input and of a buffer given back from a wl_display.sync
	 * after the events that queued it (notify, its data, and the sync
	 * asked for).
	 */
	int shared;
	void (*notify)(void *data);
	void *notify_data;
	struct wl_callback *notify_sync;

	/*
	 * The on-screen keyboard's inset (libkeiui's version 7, ws102-p015): libkeiland's
	 * object (NULL with a compositor without it), the covered widths from
	 * the right and bottom edges last heard, and the application's callback.
	 */
	struct kl_keyboard_inset *inset;
	int inset_right;
	int inset_bottom;
	kl_window_keyboard_inset_fn inset_callback;
	void *inset_data;

	/*
	 * The editing operations (libkeiui's version 8, ws102-p017, edit.c):
	 * libkeiland's object (NULL with a compositor without it), whether a
	 * selection is being made, the state the application told and whether
	 * it did, and the application's callback.
	 */
	struct kl_edit *edit;
	int selecting;
	int edit_told;
	unsigned edit_state;
	kl_window_edit_fn edit_callback;
	void *edit_data;

	/*
	 * A window of an application (KL_VERSION 26): the application, the
	 * next of its windows, and whether its input is kept from the queue
	 * (while it is made: its first configure is not news).
	 */
	struct kl_app *app;
	struct kl_window *app_next;
	int app_quiet;

	/*
	 * The declarative menus, controls and glass (window-declare.c): the
	 * action states, the menu's, the controls' and the popup's models and
	 * the objects that show them (a menu service of the window's own when
	 * it is not an application's), and the glass with the panels sent.
	 */
	struct keiui_declare_states action_states;
	struct keiui_declare_model menu_model;
	struct keiui_declare_model control_model;
	struct keiui_declare_model popup_model;
	struct kl_menu_service *menu_service;
	struct kl_menu *menu;
	struct kl_window_menu *window_menu;
	struct kl_menu *popup_menu;
	struct kl_context_menu *popup;
	struct kl_titlebar *titlebar;

	/* The tabs in the titlebar (window-declare.c, WS131 p018): the ones shown, how many, the options and whether any were ever sent. */
	struct keiui_tab tabs[KEIUI_WINDOW_TABS];
	size_t tab_count;
	unsigned tab_options;
	int tabs_sent;

	struct kl_glass *glass;
	struct kl_glass_panel glass_panels[KL_GLASS_PANELS_MAX];
	size_t glass_count;
	int glass_sent;
};

/* An application's part of its windows (app.c): an event queued for a window (NULL when full), a window gone, and the menu service. */
struct kl_window_event *keiui_app_push(struct kl_app *app, struct kl_window *window);
void keiui_app_forget(struct kl_app *app, struct kl_window *window);
struct kl_menu_service *keiui_app_menu_service(struct kl_app *app);

/* A window of an application (window.c). */
struct kl_window *keiui_window_open_app(struct kl_app *app, const struct kl_window_options *options);

/* The declarative parts of a window (window-declare.c): their models made, and their objects taken away before the window. */
void keiui_declare_window_init(struct kl_window *window);
void keiui_declare_window_close(struct kl_window *window);

/* A window on the application's connection, and its owner's wake-up (window.c). */
struct kl_window *keiui_window_open_shared(struct wl_display *display, struct xdg_toplevel *parent, const struct kl_window_options *options, uint32_t min_width, uint32_t min_height);
void keiui_window_set_notify(struct kl_window *window, void (*notify)(void *data), void *data);
void keiui_window_wake(struct kl_window *window);

/* The window's clock in milliseconds (window.c). */
uint64_t keiui_clock_ms(void);

/* The Vulkan presenter (present.c). */
VkResult keiui_present_open(struct keiui_present *present, struct kl_window *window);
VkResult keiui_present_resize(struct keiui_present *present, uint32_t width, uint32_t height);
VkResult keiui_present_frame(struct keiui_present *present, const uint32_t *pixels, size_t stride, const struct kl_rect *part);
void keiui_present_close(struct keiui_present *present);

/* The shared-memory presenter (present-shm.c). */
int keiui_shm_present(struct kl_window *window, const uint32_t *pixels, size_t stride);
void keiui_shm_close(struct kl_window *window);

/* The clipboard (clipboard.c) and the primary selection (primary.c). */
void keiui_clipboard_bind(struct kl_window *window, struct wl_registry *registry, uint32_t name, uint32_t version);
void keiui_clipboard_start(struct kl_window *window);
void keiui_clipboard_close(struct kl_window *window);
void keiui_primary_bind(struct kl_window *window, struct wl_registry *registry, uint32_t name);
void keiui_primary_start(struct kl_window *window);
void keiui_primary_close(struct kl_window *window);

/* A pen tablet (tablet.c), and an input's time from the compositor's (window.c). */
void keiui_tablet_bind(struct kl_window *window, struct wl_registry *registry, uint32_t name);
void keiui_tablet_close(struct kl_window *window);
void keiui_window_stamp(struct kl_window_event *event, uint32_t time);

/* The text input (text-input.c). */
void keiui_text_input_bind(struct kl_window *window, struct wl_registry *registry, uint32_t name);
void keiui_text_input_start(struct kl_window *window);
void keiui_text_input_close(struct kl_window *window);
struct kl_window_event *keiui_window_push(struct kl_window *window, unsigned kind);

/* The editing operations (edit.c): made with the window, the state sent before each wait, a key moved with Shift while selecting, and the end. */
void keiui_edit_start(struct kl_window *window);
void keiui_edit_update(struct kl_window *window);
void keiui_edit_key(struct kl_window *window, struct kl_window_event *event);
void keiui_edit_close(struct kl_window *window);

#endif
