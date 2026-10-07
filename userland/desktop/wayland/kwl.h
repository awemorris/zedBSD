/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shared state for the Wayland compositor.
 *
 * zdesktop draws in window mode (WS035 compositing design, D0): a background
 * and every window, bottom to top, fullscreen ones too, with Vulkan into a
 * VK_KHR_display swapchain (compose.c); a window's image is imported once
 * per wl_buffer (import.c).  The direct scanout of a fullscreen window's
 * image (fullscreen mode) was removed in ws099-p015.  The WS014/WS029 scope had
 * no input; WS031 p013 extends it with one seat ("seat0", wl_seat v5):
 *
 * - Every /dev/input/eventN reporting REL_X+REL_Y or ABS_X+ABS_Y is a
 *   pointer and every one reporting KEY_A and KEY_Z is a keyboard.  Nodes are
 *   scanned at start-up and again every KWL_INPUT_SCAN_MS; a node that fails
 *   a read is closed.  Capabilities follow the open nodes.
 * - Focus is the surface currently on the display; its client's pointer and
 *   keyboard objects get enter when it is shown and leave when it is replaced,
 *   unmapped or destroyed.  Clients without seat objects get nothing.
 * - Pointer positions are integer surface pixels (0..width-1, 0..height-1) sent
 *   as wl_fixed.  An absolute device's range is mapped linearly onto the
 *   surface; relative motion is added and clamped; the start is the centre.
 * - Buttons are Linux BTN_* codes.  A wheel notch is axis value 15.0 (negative
 *   is up/left) with axis_source wheel and axis_discrete +-1 for v5 pointers.
 *   Each evdev report ends with wl_pointer.frame for v5 pointers.
 * - The keyboard sends keymap format no_keymap with a /dev/null descriptor of
 *   size 0, evdev key codes, depressed modifiers (shift 0x1, ctrl 0x4,
 *   alt 0x8, meta 0x40), and repeat_info rate 0 (no client repeat).
 *   Kernel autorepeat events are not forwarded.
 * - wl_pointer.set_cursor is accepted and ignored; nothing draws a cursor and
 *   a cursor surface cannot be committed.
 * - A touch screen (multitouch protocol B) is offered as wl_touch (touch.c,
 *   WS079 p013); the compositor's own gestures see its fingers first.
 */
#ifndef KWL_H
#define KWL_H

#include "userland/desktop/libkeiland-backend/keiland-backend-gpu.h"
#include "userland/desktop/libkeiland-backend/keiland-backend-evdev.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "touchpad.h"
#include "pointer-accel.h"
#include "apps.h"
#include "switcher.h"
#include "swipe.h"
#include "power-layout.h"
#include "lid.h"
#include "sleep-rules.h"
#include "super-tap.h"
#include "plane.h"
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* Bound each connection's wire, descriptor, object and queued-event storage. */
#define KWL_WIRE_MAX		65532U
#define KWL_RIGHTS_MAX		32U
#define KWL_OBJECT_MAX		4096U
#define KWL_OUTPUT_MAX		1048576U

/* The first ID of the range the compositor gives the objects it makes for a client. */
#define KWL_SERVER_ID_FIRST	0xff000000U

/* How many cursor images zdesktop draws for the shapes clients ask for (cursor.c). */
#define KWL_CURSOR_IMAGES	10U

/* Bound the evdev nodes the seat reads and the events one report may carry. */
#define KWL_INPUT_MAX		16U
#define KWL_INPUT_FRAME_MAX	64U
#define KWL_INPUT_PATH_MAX	KL_BACKEND_INPUT_PATH_MAX

/* Rescan period for evdev nodes that appear after start-up, in milliseconds. */
#define KWL_INPUT_SCAN_MS	2000U

/* A window's place when the client chooses its size: cascaded from the centre by this step. */
#define KWL_CASCADE_STEP	32

/*
 * The glass look (glass.c): a title bar's height, the system bar's height,
 * the gap between a title bar and its body, the margin at the output's edges,
 * and so the highest a body may be.  BTN_LEFT is the left mouse button.
 *
 * The system bar is as high as a window's title bar (ws099-p031, the
 * 2026-10-04 user: the two are one height), so it is defined from it.
 */
#define KWL_GLASS_TITLE		44
#define KWL_GLASS_BAR		KWL_GLASS_TITLE
#define KWL_GLASS_GAP		8
#define KWL_GLASS_MARGIN	12
#define KWL_GLASS_TOP		(KWL_GLASS_BAR + KWL_GLASS_MARGIN + KWL_GLASS_TITLE + KWL_GLASS_GAP)

/*
 * The system bar's middle line.  What is drawn in the bar (its icons, its
 * text's baseline, its separators and pills) keeps its size and is placed
 * from this line, so it stays centred whatever the bar's height.
 */
#define KWL_GLASS_BAR_MIDDLE	(KWL_GLASS_BAR / 2)

/*
 * How far a docked (maximized) window's body keeps from the screen's edges
 * and the system bar on every side (ws099-p038, the 2026-10-06 user
 * decision), and where its body starts: just under the system bar.  In
 * logical pixels at the default DPI; DPI scaling multiplies it here.
 */
#define KWL_GLASS_DOCK_PAD	8
#define KWL_GLASS_DOCK_TOP	(KWL_GLASS_BAR + KWL_GLASS_DOCK_PAD)
#define KWL_BUTTON_LEFT		0x110U
#define KWL_TITLE_MAX		64U

struct kwl_server;
struct kwl_client;
struct kwl_object;
struct kwl_notify_model;
struct kwl_compose;
struct kl_backend;
struct kwl_import;
struct kwl_panels;
struct kwl_ime;

/* Each live protocol identity has one immutable interface and negotiated version. */
enum kwl_kind {
	KWL_DISPLAY,
	KWL_REGISTRY,
	KWL_COMPOSITOR,
	KWL_SURFACE,
	KWL_REGION,
	KWL_CALLBACK,
	KWL_BUFFER,
	KWL_OUTPUT,
	KWL_WM,
	KWL_XDG_SURFACE,
	KWL_TOPLEVEL,
	KWL_FACTORY,
	KWL_GPU_OBJECT,
	KWL_SEAT,
	KWL_POINTER,
	KWL_KEYBOARD,
	KWL_SHM,
	KWL_SHM_POOL,
	KWL_MENU_MANAGER,
	KWL_MENU,
	KWL_TOPLEVEL_MENU,
	KWL_POSITIONER,
	KWL_POPUP,
	KWL_SUBCOMPOSITOR,
	KWL_SUBSURFACE,
	KWL_TITLEBAR_MANAGER,
	KWL_TITLEBAR,
	KWL_DATA_MANAGER,
	KWL_DATA_SOURCE,
	KWL_DATA_DEVICE,
	KWL_DATA_OFFER,
	KWL_PRIMARY_MANAGER,
	KWL_PRIMARY_SOURCE,
	KWL_PRIMARY_DEVICE,
	KWL_PRIMARY_OFFER,
	KWL_DECORATION_MANAGER,
	KWL_DECORATION,
	KWL_CURSOR_SHAPE_MANAGER,
	KWL_CURSOR_SHAPE_DEVICE,
	KWL_VIEWPORTER,
	KWL_VIEWPORT,
	KWL_CONTENT_TYPE_MANAGER,
	KWL_CONTENT_TYPE,
	KWL_GLASS_MANAGER,
	KWL_GLASS,
	KWL_CONTEXT_MENU,
	KWL_TABLET_MANAGER,
	KWL_TABLET_SEAT,
	KWL_TABLET,
	KWL_TABLET_TOOL,
	KWL_TOUCH,
	/* The text input and input method protocols (text-input.c, input-method.c, ws095-p004). */
	KWL_TEXT_INPUT_MANAGER,
	KWL_TEXT_INPUT,
	KWL_INPUT_METHOD_MANAGER,
	KWL_INPUT_METHOD,
	KWL_INPUT_POPUP,
	KWL_KEYBOARD_GRAB,
	KWL_VIRTUAL_KEYBOARD_MANAGER,
	KWL_VIRTUAL_KEYBOARD,
	KWL_IME_STATUS_MANAGER,
	KWL_IME_STATUS,
	/* The desktop surface (desktop.c, ws094-p002). */
	KWL_DESKTOP_MANAGER,
	KWL_DESKTOP_SURFACE,
	/* The keyboard inset (inset.c, ws102-p015). */
	KWL_KEYBOARD_INSET_MANAGER,
	KWL_KEYBOARD_INSET,
	/* The editing operations (edit.c, ws102-p017). */
	KWL_EDIT_MANAGER,
	KWL_EDIT,
	/* KDE's server decoration, which GTK declares its decoration with (decoration.c, ws114-p008). */
	KWL_KDE_DECORATION_MANAGER,
	KWL_KDE_DECORATION,
	/* Keiland's system extension: the manager and the settings (settings.c, WS135), the network, the sound, the power and the devices (system.c, WS131 p010). */
	KWL_SYSTEM_MANAGER,
	KWL_SYSTEM_SETTINGS,
	KWL_SYSTEM_NETWORK,
	KWL_SYSTEM_AUDIO,
	KWL_SYSTEM_POWER,
	KWL_SYSTEM_DEVICES,
	/* The system extension's account (system.c, ws160-p002). */
	KWL_SYSTEM_ACCOUNT,
	/* The system extension's Remote Login (system.c, ws089-p025). */
	KWL_SYSTEM_SHARING,
	/* The system extension's monitor (sysmon.c, WS134 p012). */
	KWL_SYSTEM_MONITOR,
	/* xdg_activation_v1 and its tokens (activation.c, ws089-p016). */
	KWL_ACTIVATION_MANAGER,
	KWL_ACTIVATION_TOKEN,
	/* kl_theme_v1, the desktop's appearance (theme.c, ws089-p017). */
	KWL_THEME,
	/* The system extension's notifications (notify-shell.c, ws156-p002). */
	KWL_SYSTEM_NOTIFY,
	/* The system extension's arrivals of mail (mail-shell.c, ws169-p002). */
	KWL_SYSTEM_MAIL,
	/* The system extension's phone (phone-shell.c, ws170-p004). */
	KWL_SYSTEM_PHONE,
	/* The system extension's printers (printers-shell.c, ws145-p003). */
	KWL_SYSTEM_PRINTERS,
	/* The system extension's displays (displays-shell.c, ws113-p005). */
	KWL_SYSTEM_DISPLAYS,
};

/*
 * The editing operations (edit.c, ws102-p017; kl_edit_v1's actions,
 * in its order) and a window's state's bits (kl_edit_v1.set_state).
 */
#define KWL_EDIT_COPY			0U
#define KWL_EDIT_CUT			1U
#define KWL_EDIT_PASTE			2U
#define KWL_EDIT_UNDO			3U
#define KWL_EDIT_REDO			4U
#define KWL_EDIT_SELECT_ALL		5U
#define KWL_EDIT_SELECT_BEGIN		6U
#define KWL_EDIT_SELECT_END		7U
#define KWL_EDIT_ACTIONS		8U
#define KWL_EDIT_HAS_SELECTION		1U
#define KWL_EDIT_CAN_PASTE		2U
#define KWL_EDIT_CAN_UNDO		4U
#define KWL_EDIT_CAN_REDO		8U
#define KWL_EDIT_SELECTING		16U
#define KWL_EDIT_FLAGS_ALL		31U

/*
 * Where a contact the edge gestures hear comes from (ws079-p010): the
 * pointer's left button, the tip of a pen, or a finger.
 */
enum kwl_contact_source {
	KWL_CONTACT_POINTER,
	KWL_CONTACT_PEN,
	KWL_CONTACT_TOUCH
};

/* The wl_shm formats (ARGB8888 has alpha; XRGB8888's top byte is unused). */
#define KWL_SHM_ARGB8888	0U
#define KWL_SHM_XRGB8888	1U

/*
 * A wl_shm_pool's memory: the client's fd, mapped read-only once at
 * creation and again at resize.  The pool object and each buffer made from
 * it hold a reference; the mapping goes with the last.
 */
struct kwl_pool {
	int fd;
	void *map;
	size_t size;
	unsigned references;
};

/* Where a wl_shm buffer's pixels are in its pool. */
struct kwl_shm_buffer {
	struct kwl_pool *pool;
	uint32_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
};

/*
 * The output queue retains unsent bytes through short writes and EAGAIN.
 *
 * A packet may own one descriptor, which travels as SCM_RIGHTS with the
 * packet's first byte and is closed once that byte has been sent; -1 means
 * the event carries no descriptor.
 */
struct kwl_packet {
	struct kwl_packet *next;
	size_t size;
	size_t sent;
	int descriptor;
	unsigned char bytes[1];
};

/*
 * One open evdev node the seat reads.
 *
 * A slot is in use while live is set; the server's fixed table keeps a slot's
 * address stable while the event loop holds it in a poll snapshot.  Events
 * are gathered into frame[] until SYN_REPORT, then applied together.
 */
struct kwl_input_device {
	int fd;
	unsigned live;
	unsigned pointer;
	unsigned keyboard;
	unsigned absolute;
	/* A pen tablet (tablet.c, WS079 p003): its reports go to the tablet, not to apply_frame. */
	unsigned tablet;
	/* A touch screen (touch.c, WS079 p013): its reports go to the touch screen, not to apply_frame. */
	unsigned touch;
	/*
	 * A touch pad (touchpad.c, ws159-p004): its reports go to its touch
	 * pad layer, whose actions move the pointer, press its buttons and
	 * scroll; pad is that layer's state while the device is attached.
	 */
	unsigned touchpad;
	struct kwl_touchpad pad;
	/* A relative mouse's acceleration (pointer-accel.c, ws089-p024): its fractions and its last report's time. */
	struct kwl_pointer_accel accel;
	unsigned discarding;
	int32_t abs_x_minimum;
	int32_t abs_x_maximum;
	int32_t abs_y_minimum;
	int32_t abs_y_maximum;
	int32_t abs_x;
	int32_t abs_y;
	unsigned frame_count;
	struct input_event frame[KWL_INPUT_FRAME_MAX];
	/* The evdev time of the report being applied (its SYN_REPORT), in microseconds (WS081). */
	uint64_t frame_time_us;
	char path[KWL_INPUT_PATH_MAX];
};

/* One acquire fence: a fence fd and the payload generation the image waits for. */
#define KWL_FENCE_MAX 4U
struct kwl_fence {
	int fd;
	uint64_t generation;
};

/*
 * One client-owned protocol object; destroyed buffers remain until all pending,
 * current and scanout holds are gone. Surface state is double-buffered.
 */
struct kwl_object {
	struct kwl_object *next;
	struct kwl_client *client;
	uint32_t id;
	enum kwl_kind kind;
	uint32_t version;
	unsigned dead;
	unsigned holds;
	/*
	 * A wl_output binding's display (ws113-p004b): 0 the output, n the
	 * n-th head (heads.c), KWL_OUTPUT_GONE for a head that closed, whose
	 * binding is told nothing more.
	 */
	uint32_t output_head;
	unsigned busy;
	struct kwl_object *surface;
	struct kwl_object *role;
	struct kwl_object *top;
	/*
	 * An xdg_surface's xdg_wm_base, the binding get_xdg_surface was asked
	 * of (ws035-p132, BUG-112): only its own live xdg_surfaces keep that
	 * binding from being destroyed.  Compared, never followed: a binding
	 * outlives every live xdg_surface made from it.
	 */
	struct kwl_object *wm_base;
	struct kwl_object *pending;
	struct kwl_object *queued;
	struct kwl_object *current;
	struct kwl_object *callbacks;
	struct kwl_object *committed_callbacks;
	struct kwl_object *callback_next;
	unsigned attached;
	unsigned ready;
	unsigned configured;
	unsigned acknowledged;
	uint32_t configure_serial;
	uint64_t commit_order;
	/* A buffer's Vulkan image for window mode. */
	struct kwl_import *import;
	/* The OS module retains buffer descriptors or protocol params until final object retirement. */
	void *gpu_private;
	/* A surface's window: place, stacking (map order, lowest at the bottom), virtual desktop and fullscreen state. */
	unsigned mapped;
	uint64_t map_order;
	/* When the window was first shown (the map order then), which orders the bar's applications (apps.c, ws142-p004). */
	uint64_t open_order;
	unsigned desktop;
	unsigned minimized;
	int32_t x;
	int32_t y;
	/* The output a window is shown on in the extended mode (plane.h's slot, 0 the anchor; ws113-p007); its place is in the plane. */
	unsigned output;
	unsigned fullscreen;
	/*
	 * A window that was docked when it went fullscreen (BUG-208): while it
	 * is fullscreen it is not docked (maximized is 0, so it is placed and
	 * drawn as any fullscreen window), and its restore place is its place
	 * as a floating window.  Leaving fullscreen follows the session's layout
	 * mode (ws142-p008), which docks it again or brings it back there.
	 */
	unsigned fullscreen_docked;
	uint32_t window_width;
	uint32_t window_height;
	int32_t window_x;
	int32_t window_y;
	/*
	 * A window that was placed as a window (so window_x and window_y hold a
	 * place to come back to from fullscreen), and one to be centred at its
	 * next image of a new size (it left a fullscreen it started in, ws035-p138).
	 */
	unsigned placed;
	unsigned place_pending;
	/* A surface whose current image has not been shown yet. */
	unsigned fresh;
	/*
	 * A window's parent (xdg_toplevel.set_parent: the parent's surface,
	 * NULL for none or once it has gone), and for a sheet (ws090-p014,
	 * sheet.c) when it began to show under the parent (0: not shown yet).
	 */
	struct kwl_object *parent_window;
	uint64_t sheet_ms;
	/* The glass look: the toplevel's title and application ID, and a maximized window's place and size to go back to. */
	char title[KWL_TITLE_MAX];
	char app_id[64];
	/* A kl_edit_v1's operations (bit 1 << KWL_EDIT_*) and state (KWL_EDIT_HAS_SELECTION ...; edit.c). */
	uint32_t edit_actions;
	uint32_t edit_flags;
	unsigned maximized;
	int32_t restore_x;
	int32_t restore_y;
	uint32_t restore_width;
	uint32_t restore_height;
	/*
	 * A restore place the shell made up (dock_restore_default, shell.c) for
	 * a window docked before it ever floated: when the docked mode ends
	 * without an animation, such a window is placed as a new window is
	 * (kwl_glass_place), so that several are not left on one spot (WS181).
	 */
	unsigned restore_default;
	/*
	 * A window the shell docked or brought back whose client has not drawn
	 * the new size yet (shell.c, BUG-179 and BUG-180): when the size was
	 * sent (ms; 0 once an image drawn after it came), and the serial of
	 * the configure that carried it.  Until then a window brought back is
	 * drawn at the size it was sent, not at its old docked image's.
	 */
	uint64_t resized_ms;
	uint32_t resized_serial;
	/* When the client acknowledged that configure, and when its latest commit came (ms; 0 before), which split the wait in the log (BUG-179). */
	uint64_t resized_acked_ms;
	uint64_t resized_commit_ms;
	/* The size of the image the window had when the size was sent (0 without one): an image of it is an old one. */
	uint32_t resized_from_width;
	uint32_t resized_from_height;
	/* A wl_shm buffer's place in its pool (NULL for a GPU buffer), and a pool object's memory. */
	struct kwl_shm_buffer *shm;
	struct kwl_pool *pool;
	/* A surface's damage in buffer pixels, pending and committed (x0, y0, x1, y1), and whether any was given. */
	int32_t damage[4];
	unsigned damaged;
	int32_t committed_damage[4];
	unsigned committed_damaged;
	/* A surface's copy of its wl_shm image that window mode samples, and whether it must be copied again. */
	struct kwl_import *shm_image;
	unsigned shm_upload;
	/* A surface used as the pointer's cursor (wl_pointer.set_cursor). */
	unsigned cursor_role;
	/* A window told its frame is done whose next commit the next frame waits for a moment. */
	unsigned awaited;
	/*
	 * Acquire fences (kl_gpu_buffer_v1 revision two): those for the next
	 * commit, and the committed ones the queued image waits for.
	 */
	struct kwl_fence acquire[KWL_FENCE_MAX];
	unsigned acquire_count;
	struct kwl_fence fences[KWL_FENCE_MAX];
	unsigned fence_count;
	/* When the queued fences were committed, and whether a pass found one still pending. */
	uint64_t fence_ms;
	unsigned fence_waited;
	/*
	 * The System Menu (menu.c): an xdg_menu_v1's model; a toplevel's
	 * xdg_toplevel_menu_v1 (whose own top names the toplevel back); and the
	 * xdg_menu_v1 an xdg_toplevel_menu_v1 shows.  Each link is cleared from
	 * both ends when either object goes.
	 */
	struct kwl_menu_model *menu_model;
	struct kwl_object *toplevel_menu;
	struct kwl_object *shown_menu;
	/*
	 * The Titlebar Presentation (titlebar.c, WS070 p008): a
	 * kl_titlebar_v1's model, and a toplevel's kl_titlebar_v1 (whose own
	 * top names the toplevel back).  Each link is cleared from both ends
	 * when either object goes.
	 */
	struct kwl_titlebar_model *titlebar_model;
	struct kwl_object *titlebar;
	/*
	 * xdg_popup (popup.c, ws035-p076): an xdg_positioner's rules; a popup's
	 * parent surface (NULL once the parent has gone), its place relative to
	 * the parent's window geometry and its size, the order it was made in
	 * (popups are drawn in that order), whether it asked for the seat's
	 * grab, and whether it was closed (popup_done: it is not drawn or hit
	 * any more, and waits for its client to destroy it).  A surface's window
	 * geometry (xdg_surface.set_window_geometry: x, y, width, height),
	 * pending and committed, and whether one was set.
	 */
	struct kwl_positioner *positioner;
	struct kwl_object *popup_parent;
	int32_t popup_x;
	int32_t popup_y;
	int32_t popup_width;
	int32_t popup_height;
	uint64_t popup_order;
	unsigned popup_grab;
	unsigned popup_closed;
	int32_t pending_geometry[4];
	unsigned pending_geometry_set;
	int32_t geometry[4];
	unsigned geometry_set;
	/*
	 * A toplevel's requests (toplevel.c, ws035-p076): the smallest and
	 * largest size the client can draw (0 for no limit); the edges a resize
	 * drags and the right and bottom edges on the output that stay while
	 * the left or top edge is dragged (the anchor; no edges when there is
	 * none); the serial of the configure sent when the resize ended, and
	 * when it ended (ms, ws035-p128: an image drawn before the client read
	 * that configure may still come after it acknowledged it), and the last
	 * serial the client acknowledged (xdg_surface.ack_configure).
	 */
	int32_t min_width;
	int32_t min_height;
	int32_t max_width;
	int32_t max_height;
	uint32_t resize_edges;
	int32_t resize_right;
	int32_t resize_bottom;
	uint32_t resize_final_serial;
	uint64_t resize_end_ms;
	uint32_t acked_serial;
	/*
	 * Sub-surfaces (subsurface.c, ws035-p077).  A surface with the role has
	 * its wl_subsurface (whose own surface field names the surface back)
	 * and its parent; a parent has its children, bottom to top, linked by
	 * sub_next, each below or above the parent.  The position applied, the
	 * one set for the parent's next commit and whether one was set, and the
	 * synchronized mode.  A synchronized sub-surface's commit is cached
	 * (the buffer and whether one was attached, the frame callbacks) until
	 * its parent's state is applied.  Each link is cleared from both ends
	 * when either object goes.
	 */
	struct kwl_object *sub_role;
	struct kwl_object *sub_parent;
	struct kwl_object *sub_children;
	struct kwl_object *sub_next;
	unsigned sub_above;
	int32_t sub_x;
	int32_t sub_y;
	int32_t sub_pending_x;
	int32_t sub_pending_y;
	unsigned sub_moved;
	unsigned sub_sync;
	unsigned sub_cached;
	struct kwl_object *sub_cached_buffer;
	unsigned sub_cached_attached;
	struct kwl_object *sub_cached_callbacks;
	/*
	 * The clipboard (data.c, ws035-p079): a wl_data_source's MIME types
	 * (allocated strings, freed with it); a wl_data_offer's source (NULL
	 * once the source has gone).
	 */
	char **mime_types;
	unsigned mime_count;
	struct kwl_object *data_source;
	/* An offer of zdesktop's own selection, an item of the clipboard's history (clipboard.c). */
	unsigned data_offered;
	/*
	 * Drag and drop (data.c, ws035-p084): a source's actions (set_actions),
	 * and for an offer made for a drag: the actions its target takes and
	 * the one it prefers, the action last told, whether it accepted a type,
	 * and whether it was dropped on (its finish is then awaited).
	 */
	uint32_t dnd_actions;
	uint32_t dnd_preferred;
	uint32_t dnd_action;
	unsigned dnd_offer;
	unsigned dnd_accepted;
	unsigned dnd_dropped;
	/*
	 * ws035-p080: a toplevel's zxdg_toplevel_decoration_v1 and the
	 * decoration's toplevel (each cleared from both ends when either
	 * goes); a wp_cursor_shape_device_v1's wl_pointer (NULL once it has
	 * gone).  A surface's wp_viewport (whose own surface field names it
	 * back), and the viewport's state, pending and applied by the commit:
	 * the source rectangle in 24.8 fixed point (x, y, width, height; a
	 * width of 0 for none) and the destination size (0 for none), and
	 * whether the pending state changed since the last commit.
	 */
	struct kwl_object *decoration;
	struct kwl_object *decoration_toplevel;

	/*
	 * The toplevel owns its decoration negotiation and outstanding configure
	 * snapshots until teardown. Preferred zero means no explicit xdg choice;
	 * configured is the offered mode and committed is the visible mode.
	 * An acknowledged snapshot waits for the next surface commit. Generation
	 * changes invalidate mode proposals from a destroyed/replaced decoration;
	 * reset withdraws SSD at the next commit even without a new configure.
	 */
	uint32_t decoration_preferred;
	uint32_t decoration_configured;
	uint32_t decoration_committed;
	uint32_t decoration_acked_mode;
	uint64_t decoration_generation;
	unsigned decoration_acked;
	unsigned decoration_reset;
	struct kwl_decoration_configure *decoration_configures;
	/*
	 * ws114-p008: a toplevel whose xdg-decoration object was destroyed keeps
	 * the client's decoration (withdrawn, until a new object is made).  A
	 * surface's org_kde_kwin_server_decoration, and on that object the
	 * surface it decorates and the mode the client asked for (KDE_MODE_*,
	 * decoration.c); each is cleared from both ends when either goes.
	 */
	unsigned decoration_withdrawn;
	struct kwl_object *kde_decoration;
	struct kwl_object *kde_surface;
	uint32_t kde_mode;
	struct kwl_object *shape_pointer;
	struct kwl_object *viewport;
	int32_t pending_source[4];
	int32_t source[4];
	int32_t pending_destination[2];
	int32_t destination[2];
	unsigned viewport_changed;
	/*
	 * ws122-p005b (content-type.c): a surface's wp_content_type_v1 (whose
	 * own surface field names it back), and its type (0 none, 1 photo, 2
	 * video, 3 game), pending and applied by the commit.
	 */
	struct kwl_object *content_type_object;
	uint32_t pending_content_type;
	uint32_t content_type;
	unsigned content_type_changed;
	/*
	 * ws035-p083 (panels.c): a surface's kl_glass_v1 (whose own surface
	 * field names it back; each cleared from both ends when either goes),
	 * and the record of its glass panels, pending and applied by the
	 * commit, which the surface owns from its first glass to its end.
	 */
	struct kwl_object *glass;
	struct kwl_panels *panels;
	/*
	 * The tablet protocol (tablet.c, WS079 p003): the number of the
	 * zwp_tablet_seat_v2 a tablet or a tool object was announced on (a
	 * seat's own number; unique for the compositor's life), and the slot of
	 * the tablet device and of the tool the object stands for
	 * (KWL_TABLET_SLOT_NONE once the device or the tool has gone).
	 */
	uint64_t tablet_seat_number;
	unsigned tablet_slot;
	unsigned tool_slot;
	/*
	 * A monitor object of the system extension (sysmon.c, WS134 p012): the
	 * period it asked for in milliseconds, the serial of the sample it has
	 * not acked yet (0: none, the next may come), and the serial of the
	 * info it heard last (0: none yet).
	 */
	uint32_t monitor_period;
	uint32_t monitor_waiting;
	uint32_t monitor_info;
	/*
	 * A network object of the system extension (system.c, ws089-p021):
	 * 1 while its client shows the networks around and asked for scans
	 * (set_scanning), counted once in network.c's holders until it asks
	 * no longer, goes, or lets its asking run out: network_scanning_until
	 * is when an asking not asked again ends (kwl_milliseconds' clock,
	 * KWL_SYSTEM_SCAN_MS after the last set_scanning(1)).
	 */
	unsigned network_scanning;
	uint64_t network_scanning_until;
	/*
	 * An xdg_activation_token_v1 (activation.c, ws089-p016): the ID of the
	 * surface set_surface named (0: none; the application's ID is kept in
	 * app_id), and whether it was committed (it then takes nothing but
	 * destroy).
	 */
	uint32_t activation_surface;
	unsigned activation_committed;
};

/* One stream has independent byte and fd FIFOs, plus its own protocol namespace. */
struct kwl_client {
	struct kwl_client *next;
	struct kwl_server *server;
	int fd;
	uint64_t number;
	unsigned fatal;
	uint64_t fatal_time;
	struct kwl_object *objects;
	unsigned object_count;
	unsigned char input[KWL_WIRE_MAX];
	size_t input_size;
	int rights[KWL_RIGHTS_MAX];
	unsigned right_count;
	struct kwl_packet *output_head;
	struct kwl_packet *output_tail;
	size_t output_bytes;
	/*
	 * The ping (toplevel.c): the serial of the ping waiting for its answer
	 * (0 for none) and when it was sent, and whether the client has left a
	 * ping unanswered too long (its title bars say it is not responding).
	 */
	uint32_t ping_serial;
	uint64_t ping_ms;
	unsigned unresponsive;
	/*
	 * The next ID from the server's range (0xff000000 and up) for an object
	 * the compositor makes for this client (a wl_data_offer, data.c).
	 */
	uint32_t server_id_next;
	/*
	 * Nonzero for the connection of the system's input method, which
	 * zdesktop made itself (input-method.c, ws095-p004): only it sees and
	 * binds the input method's globals.
	 */
	unsigned ime;
	/*
	 * Nonzero once the client bound KDE's server decoration manager
	 * (decoration.c, ws114-p008).  Under KDE's protocol a window is the
	 * compositor's to decorate only through a decoration object, so such a
	 * client's windows without one keep their own decoration: GTK4 binds
	 * the manager and makes an object only for a window it wants
	 * decorated.
	 */
	unsigned kde_bound;
	/*
	 * Whether the peer's user was looked at, and whether it is the
	 * compositor's own (settings.c, WS135: only the compositor's user sees
	 * the system extension).  Looked at once, at the first registry.
	 */
	unsigned peer_checked;
	unsigned peer_same;
	/*
	 * Nonzero once the client bound kl_theme_v1 (theme.c,
	 * ws089-p017): it draws in the desktop's appearance, so its windows'
	 * glass takes the dark appearance's colour too; a client that does
	 * not know the appearance keeps light glass under its light drawing.
	 */
	unsigned theme_bound;
	/*
	 * When the client connected (kwl_milliseconds' clock): a program that
	 * has just started may hand its right to show a window on top to
	 * another program's window (activation.c, ws089-p016).
	 */
	uint64_t connected_ms;
};

/* Cycle counts of the event loop, reported every few seconds (KWL PERF). */
struct kwl_perf {
	uint64_t window_start_ms;
	uint64_t window_start_cycles;
	uint64_t poll_cycles;
	uint64_t work_cycles;
	uint32_t passes;
	uint32_t timeouts;
	/* Window mode: frames completed, and their time from the start of drawing to the fence. */
	uint32_t compose_frames;
	uint64_t compose_cycles;
	uint64_t compose_draw_cycles;
	uint64_t compose_acquire_cycles;
	uint64_t compose_present_cycles;
	/* wl_shm: images copied, and the CPU time of the copies. */
	uint32_t shm_copies;
	uint64_t shm_copy_cycles;
};

uint64_t kwl_cycles(void);

/*
 * The compositor: one per process, alive from start to exit.
 *
 * It alone owns its Vulkan device and output and the connections of its
 * clients.
 */
/*
 * The virtual desktops that keep a bar order of their own (shell.c has as
 * many): three, the middle one where a session starts, the others to its
 * left and right (the 2026-10-07 UAT, ws181-p006: "go left or right of the
 * middle", not "the n-th").
 */
#define KWL_APPS_DESKTOPS	3U
#define KWL_DESKTOP_START	1U

/* Whether the previews of an application's icon show: not, waiting on the pointer's rest, or shown (by the rest or a click). */
#define KWL_APPS_IDLE		0U
#define KWL_APPS_ARMED		1U
#define KWL_APPS_SHOWN		2U
#define KWL_APPS_VIA_HOVER	0U
#define KWL_APPS_VIA_CLICK	1U
#define KWL_APPS_VIA_SWITCH	2U

/*
 * The bars' applications (apps-bar.c): each desktop's bar order (one for
 * every output's bar); the output whose bar the previews' state is about
 * (the system bar's or a head's, ws113-p015), that state, the application
 * it is about, when the wait began (or when the pointer left), and whether
 * it has left the icons and the panel; a press on an icon (its
 * application, where it began, whether it became the icon's drag); and
 * each output's bar as last logged.
 */
struct kwl_apps_bar {
	struct kwl_apps_order orders[KWL_APPS_DESKTOPS];
	unsigned output;
	unsigned state;
	unsigned via;
	char key[KWL_APPS_KEY];
	uint64_t since_ms;
	unsigned left;
	unsigned pressed;
	char press_key[KWL_APPS_KEY];
	int32_t press_x;
	unsigned dragging;
	char logged[KWL_PLANE_SLOTS][512];
};

struct kwl_server {
	struct kwl_perf perf;
	int listener;
	char socket_path[108];
	/* An explicit socket overrides the OS module's runtime-directory default. */
	unsigned socket_given;
	dev_t socket_device;
	ino_t socket_inode;
	unsigned socket_owned;
	struct kwl_client *clients;
	struct kwl_object *front_surface;
	uint64_t frame;
	uint64_t commit_order;
	uint64_t client_serial;
	uint32_t serial;
	uint32_t width;
	uint32_t height;
	/* The display mode's refresh in millihertz (from Vulkan, compose.c), told to clients by wl_output. */
	uint32_t refresh;
	/*
	 * The Vulkan device as libkeiland-backend's GPU buffers import into it,
	 * and what it can take (compose.c fills it once the device is made).
	 */
	struct kl_backend_gpu_device gpu_device;
	/*
	 * The role (role.h: KWL_ROLE_NORMAL, _TESTING or _GREETER, WS110), and
	 * the deadline it gives: none for a desktop and the login screen,
	 * --timeout or 150 s for a test run, which --max-frames can end sooner.
	 */
	unsigned role;
	uint64_t timeout_ms;
	uint64_t max_frames;
	/* Nonzero with --log-frames: every presentation and buffer release is printed (for the tests that read them). */
	unsigned log_frames;
	/*
	 * The graphical login (ws035-p095): with --greeter zdesktop draws the
	 * login screen (greeter.c), opens no socket and asks sessiond on
	 * auth_fd; session: it is a login session (the default role, or
	 * --session, which says the same), which has no deadline and ends with
	 * App Home's Log Out.  size_given: --width or --height
	 * was given, so the display's preferred size is not used.  control_fd:
	 * the session's descriptor to sessiond (--control-fd, -1 for none);
	 * auth_fd and control_fd are handed to libkeiland-backend, which speaks
	 * on them (ws131-p006);
	 * handed_over: the display's hand-over (handoff.c, ws035-p101) is done;
	 * logout_ms: when Log Out asked sessiond for a greeter (0: it did not).
	 */
	unsigned greeter;
	unsigned session;
	int auth_fd;
	int control_fd;
	unsigned handed_over;
	uint64_t logout_ms;
	/*
	 * The lock screen (ws035-p102): whether it shows, how long without
	 * input locks the session (--lock-idle, 0: never), and when the last
	 * input came.
	 */
	unsigned locked;
	uint64_t lock_idle_ms;
	unsigned lock_idle_given;
	uint64_t lock_input_ms;
	unsigned size_given;
	unsigned failed;
	struct kwl_input_device inputs[KWL_INPUT_MAX];
	uint64_t input_scan_time;
	uint64_t input_events;
	uint64_t seat_events;
	unsigned capabilities;
	struct kwl_object *focus;
	int32_t pointer_x;
	int32_t pointer_y;
	/*
	 * The output the pointer is on (plane.h's slot, 0 the anchor; the
	 * pointer's place is in the plane, ws113-p007), and the output a pass
	 * draws now: its slot and its rectangle of the plane, which the quads
	 * are placed in (no width: the anchor's).
	 */
	unsigned pointer_output;
	unsigned view_output;
	int32_t view_x;
	int32_t view_y;
	uint32_t view_width;
	uint32_t view_height;
	unsigned modifier_keys;
	uint32_t modifiers;
	/* The Windows key pressed alone (super-tap.c, ws142-p002): armed from its press until something else happens. */
	struct kwl_super_tap super_tap;
	/* The locked modifiers (Caps Lock 0x2, Num Lock 0x10), each toggled by a press of its key (ws035-p078). */
	uint32_t locked_modifiers;
	/* Window mode: the Vulkan output, whether a frame is due, and the fence fd of the frame in flight. */
	struct kwl_compose *compose;
	/* The operating system's side (libkeiland-backend, WS131): opened before the OS resources, closed after them; NULL before. */
	struct kl_backend *backend;
	/* The power as last read (ws132-p003): at start-up and at each power_changed; the bar shows the battery when percent >= 0. */
	struct kl_backend_power_state power;
	/*
	 * The lid (backend-host.c, ws132-p008): its state and whether the lock
	 * standing is its own (lid.c); whether the screen is out (drawn black);
	 * the built-in panel's backlight while the compositor has it open (NULL
	 * on a machine without one, or before the first closing), the
	 * brightness to give back at the opening, and whether it was put out.
	 */
	struct kwl_lid lid;
	unsigned screen_off;
	struct kl_backend_backlight *backlight;
	unsigned backlight_saved;
	unsigned backlight_out;
	/*
	 * The sleep (sleep.c, ws052-p012): its rules' state (sleep-rules.h);
	 * sleep_hold: a sleep's request waits for its answer, and no frame is
	 * drawn (the GPU driver parks the display); the times without input
	 * before a sleep on the adapter and on battery (minutes, 0 never: the
	 * settings power.sleep.ac and power.sleep.battery); screen_idle_off:
	 * the screen is out because of the time without input (half the sleep's
	 * time), and the next input lights it; sleep_lid_synced: the lid's
	 * level was taken from the backend once.
	 */
	struct kwl_sleep sleep;
	unsigned sleep_hold;
	int32_t sleep_ac_minutes;
	int32_t sleep_battery_minutes;
	unsigned screen_idle_off;
	unsigned sleep_lid_synced;
	/* The lid's closing moved the desktop to an external display; its opening brings it back (N8, output-switch.c). */
	unsigned output_lid_moved;
	/* OS device authority can pause composition; zedBSD always leaves this zero. */
	unsigned os_paused;
	unsigned windowed;
	unsigned dirty;
	/*
	 * The damage (damage.c, ws035-p055): whether only a part of the output
	 * changed since the last frame, and that part (left, top, right,
	 * bottom).  dirty, set by any other change, draws the whole output.
	 */
	unsigned damaged;
	int32_t damage[4];
	int frame_fd;
	uint64_t map_order;
	uint32_t windows;
	uint64_t mode_switch_ms;
	/*
	 * An opening or closing of App Home or Wiseview waiting for its first
	 * frame (ws099-p002, C5): what it is (NULL for none) and when it was
	 * asked for.  The next frame is drawn without the frame pacing's wait,
	 * and its submission is logged once (KWL FIRST_FRAME).
	 */
	const char *transition;
	uint64_t transition_ms;
	/*
	 * Frame pacing: after a frame, the windows it told are waited for, until
	 * all have committed or half the last frame's time (at most 50 ms) has
	 * passed, so that a quick client does not start the next frame without
	 * a slower one.
	 */
	unsigned awaiting;
	uint64_t frame_done_ms;
	uint64_t frame_wait_ms;
	/*
	 * Nonzero while a move of the pointer waits for the frame that shows the
	 * cursor at its new place: that frame does not wait for the windows
	 * (WS099's C6, the pointer's move to its display; ws075-p026).
	 */
	unsigned pointer_moved;
	/* The on-screen keyboard's glass shows the scene under it blurred (--keyboard-blur, ws075-p029). */
	unsigned keyboard_blur;
	/* The glass look: on, its font, the window being moved and where it was taken, the clock's minute. */
	unsigned glass;
	const char *font_path;
	const char *fallback_font_path;
	const char *wallpaper_path;
	float window_opacity;
	/*
	 * The glass panels of the windows solid (BUG-171, decision B): only
	 * while window.opacity is chosen at 100, not at the default; the title
	 * bars stay glass.
	 */
	unsigned panels_opaque;
	/*
	 * The settings the session holds (settings.c and settings-store.c,
	 * WS135): NULL for the login screen.  Made before the look, freed at
	 * the compositor's end after the session's settings are written.
	 */
	struct kwl_settings_store *settings;
	/*
	 * The settings' effects (settings.c, WS135): window_opacity_started and
	 * wallpaper_started are what the command line gave, which a setting
	 * reset returns to; wallpaper_path above is the picture shown, and
	 * wallpaper_chosen the settings' (empty for the command line's).  The
	 * pointer is set for each kind of device (ws089-p024): a mouse's speed
	 * (a percentage of its counts), its acceleration's level
	 * (pointer-accel.h) and whether its wheel turns round (natural), and the
	 * same for the touch pads, whose layer (touchpad.c) takes the level and
	 * the scrolling's direction and whose motion is scaled by the speed with
	 * the hundredths of a pixel carried over (pointer_remainder).  The
	 * keyboards' repeat is what wl_keyboard.repeat_info tells a keyboard
	 * bound from then on.
	 */
	float window_opacity_started;
	const char *wallpaper_started;
	char wallpaper_chosen[256];
	int32_t mouse_speed;
	int32_t mouse_acceleration;
	int32_t mouse_natural;
	int32_t touchpad_speed;
	int32_t touchpad_acceleration;
	int32_t touchpad_natural;
	int64_t pointer_remainder_x;
	int64_t pointer_remainder_y;
	int32_t repeat_rate;
	int32_t repeat_delay_ms;
	/*
	 * The desktop's appearance (appearance.dark, ws089-p017): 0 light, 1
	 * dark.  The glass's drawing maps its colours by it (glass.c) and
	 * kl_theme_v1 tells the clients (theme.c).
	 */
	int32_t dark;
	/*
	 * The accent the user chose (appearance.accent, ws179-p001): 0 blue to
	 * 7 graphite (artwork/accent.h).  The compositor's own controls draw
	 * with it (kwl_accent) and kl_theme_v1 version 2 tells the clients.
	 * The event loop's thread alone changes it.
	 */
	int32_t accent;
	/*
	 * Nonzero while the system bar is drawn (shell.c, ws099-p034): the bar
	 * is dark glass with light ink in both appearances, so the glass's
	 * drawing keeps the colours it is given instead of mapping them for the
	 * dark appearance.  Set and cleared around the bar by the event loop's
	 * thread only.
	 */
	unsigned keep_colours;
	/* The input method the Languages page chose (ime.method, WS154): 0 none, 1 Japanese, 2 SKK. */
	int32_t ime_method;
	/* Whether the language of the compositor's text was read once (language.c, WS158); its catalogs are libkeiland's. */
	unsigned language_set;
	struct kwl_object *drag;
	int32_t drag_dx;
	int32_t drag_dy;
	int64_t clock_minute;
	/*
	 * The glass look's shell (shell.c): where a move started, the first press
	 * of a double click, a docked window whose title is being pulled down,
	 * and the dock animation (the window, when it started, which way, and the
	 * body's rectangles at its start and end: x, y, width, height).
	 */
	int32_t drag_start_x;
	int32_t drag_start_y;
	struct kwl_object *click_surface;
	uint64_t click_ms;
	/* When the left button was last let go (BUG-247: a touch pad's tap holds its press until the next tap lifts). */
	uint64_t click_release_ms;
	/*
	 * The presses of the latest run of quick clicks on click_surface's
	 * floating title bar (1, 2 or 3), and the window a double click docked
	 * at once (BUG-179) with the place of that second press: a third
	 * press near it before click_docked_due_ms takes the dock back and
	 * sends the window to the back instead (ws079-p013).
	 */
	unsigned click_count;
	struct kwl_object *click_docked;
	uint64_t click_docked_due_ms;
	int32_t click_docked_x;
	int32_t click_docked_y;
	struct kwl_object *pull;
	int32_t pull_start_y;
	int32_t pull_distance;
	/*
	 * Where a pull started across (WS181: a pull comes off by its distance
	 * from the press in any direction), and whether the move it became has
	 * left the system bar once (only then does a release in the bar dock
	 * the window again; a move from a title bar starts out of the bar).
	 */
	int32_t pull_start_x;
	unsigned drag_left_bar;
	struct kwl_object *anim;
	uint64_t anim_start_ms;
	unsigned anim_docking;
	int32_t anim_from[4];
	int32_t anim_to[4];
	/*
	 * Whether the last frame left the system bar out to keep a fullscreen
	 * window whole (ws035-p119, shell.c); only the change is logged.
	 */
	unsigned bar_hidden;
	/*
	 * The game mode (scanout.c, ws122-p005b): the backend's direct scanout
	 * while a fullscreen video or game shows alone (NULL in window mode),
	 * the window it shows and the image the display holds, the frames
	 * presented, the last reason it was left or not entered (logged once),
	 * a window the display refused, the window last logged, and the
	 * pointer's place and when it last moved.
	 */
	struct kl_backend_scanout *scanout;
	struct kwl_object *scanout_surface;
	struct kwl_object *scanout_front;
	uint64_t scanout_frames;
	unsigned scanout_reason;
	struct kwl_object *scanout_refused;
	struct kwl_object *scanout_noted;
	int32_t scanout_pointer[2];
	uint64_t scanout_motion_ms;
	/*
	 * Wiseview (shell.c): how far it is open (0 closed, 1 open) when settled,
	 * a gesture from the bottom edge and where it started, the animation to
	 * a settled value (from, to, when it started), and the window that was
	 * on top when it opened.
	 */
	float wiseview;
	unsigned wiseview_gesture;
	int32_t wiseview_start_y;
	/*
	 * Whether the gesture is the swipe down from the top edge's band
	 * (WS181): it opens as the pointer goes down from wiseview_start_y, not
	 * up.  Cleared at the gesture's end.
	 */
	unsigned wiseview_top;
	/*
	 * A touch's press in the top edge's band (WS181, edge.h), held until it
	 * is known to be the swipe down to Wiseview or a press of what is under
	 * it (then given again at its point, band_replay set while it is), and
	 * where it was.
	 */
	unsigned band_press;
	unsigned band_replay;
	int32_t band_start_x;
	int32_t band_start_y;
	/* Whether the gesture is the touch pad's (ws142-p003), and how far it has opened Wiseview by the fingers' travel. */
	unsigned wiseview_pad;
	float wiseview_pad_progress;
	unsigned wiseview_moving;
	float wiseview_from;
	float wiseview_to;
	uint64_t wiseview_start_ms;
	struct kwl_object *wiseview_current;
	/* A press on a Wiseview tile that may become its drag to a desktop (ws035-p072): the window, where it started, whether it moved. */
	struct kwl_object *wiseview_press;
	int32_t wiseview_press_x;
	int32_t wiseview_press_y;
	unsigned wiseview_dragging;
	/*
	 * A layer's place: every glass shape drawn with layer_on is moved to
	 * layer_x, layer_y, scaled by layer_scale and faded by layer_opacity
	 * (glass.c; none is drawn at 0).  shell.c turns it on around the desktop
	 * while App Home opens or closes (the desktop going back into the
	 * distance, ws181-p008) or the desktops slide, and home.c around Home's
	 * content coming forward.  Whoever turns it on sets all four.
	 */
	unsigned layer_on;
	float layer_x;
	float layer_y;
	float layer_scale;
	float layer_opacity;
	/*
	 * App Home (home.c): how far it is open (0 closed, 1 open) when settled;
	 * a press in the top-left corner that may become the gesture, where it
	 * started and whether it has moved far enough to be one, and how far it
	 * is open by it; the animation to a settled value (from, to, when it
	 * started); what has been typed, and the selected application.
	 */
	float home;
	unsigned home_press;
	unsigned home_dragging;
	int32_t home_start_x;
	int32_t home_start_y;
	float home_drag;
	unsigned home_moving;
	float home_from;
	float home_to;
	uint64_t home_start_ms;
	/*
	 * App Home's two layers (ws099-p035c, BUG-225): when its content (the
	 * icons, rising one after another) began to come in (0: shown at once,
	 * after a drag), when it was asked to open, and whether its first frame
	 * of the stage and of the content have been logged since.
	 */
	uint64_t home_content_ms;
	uint64_t home_asked_ms;
	unsigned home_cover_logged;
	unsigned home_content_logged;
	char home_query[96];
	unsigned home_query_length;
	char home_preedit[96];
	int home_selected;
	/*
	 * A press at the bottom edge of the desktop, which may become the swipe
	 * up that opens Home (WS181, the 2026-10-07 UAT): whether it has moved
	 * far enough up to be one, and where it started.  And a press on Home
	 * that went mostly down, which pulls the desktop back over Home and
	 * closes it (home_page_closing, while it follows the pointer).
	 */
	unsigned home_rise_press;
	unsigned home_rise_dragging;
	int32_t home_rise_start_y;
	unsigned home_page_closing;
	/*
	 * A press on the launcher (or the top-left corner) that moved away is
	 * no click (ws181-p008: the drag from the corner is no way into Home,
	 * WS184 takes it): its release does nothing.
	 */
	unsigned home_press_moved;
	/*
	 * The touch pad's two fingers down from its top edge (TOP2, ws181-p008)
	 * open Home following them: whether such a gesture is under way, from
	 * its beginning until its end (the gestures' phases go to Home).
	 */
	unsigned home_pad;
	/*
	 * The virtual desktops (ws035-p065): the one shown; a press at the
	 * left or right edge that may become the swipe (where it started,
	 * whether it has moved enough) and the swipe's offset in pixels; the
	 * slide to a desktop (from and to as desktop positions, when it
	 * started).
	 */
	unsigned desktop;
	unsigned desktop_press;
	unsigned desktop_dragging;
	int32_t desktop_start_x;
	int32_t desktop_offset;
	/* Whether the swipe is the touch pad's gesture (ws142-p003). */
	unsigned desktop_pad;
	unsigned desktop_moving;
	float desktop_from;
	float desktop_to;
	uint64_t desktop_start_ms;
	/* The applications' icons in the system bar and their previews (apps-bar.c, ws142-p004). */
	struct kwl_apps_bar apps_bar;
	/* The application switcher (switcher-shell.c, ws142-p005), and a button whose press it took (its release is kept from the windows). */
	struct kwl_switcher switcher;
	uint32_t switch_swallow;
	/*
	 * The session's layout mode (KWL_LAYOUT_WINDOWED or KWL_LAYOUT_DOCKED of
	 * layout.h, ws142-p008, BUG-217): set by the person docking a window or
	 * bringing one back, followed by every window switched to, opened or
	 * leaving fullscreen.  Windowed from the session's start.  Each output
	 * has its own (plane.h's slot, ws113-p015): a head docks its windows
	 * under its own bar.
	 */
	unsigned layout_mode[KWL_PLANE_SLOTS];
	/*
	 * Each desktop's docked owner in the docked mode of each output (WS181,
	 * shell.c's layout_follow; the output's slot, ws113-p015): the docked
	 * window in front of it, seen every frame
	 * once it is mapped.  When the owner of the desktop shown is closed,
	 * minimized or sent away, the docked mode ends and every window floats
	 * again (the 2026-10-07 UAT), instead of the next one being docked.
	 * owner_gone marks an owner destroyed since (kwl_glass_forget), whose
	 * pointer is NULL already.  Both are cleared when the docked mode ends.
	 */
	struct kwl_object *dock_owner[KWL_APPS_DESKTOPS][KWL_PLANE_SLOTS];
	unsigned dock_owner_gone[KWL_APPS_DESKTOPS][KWL_PLANE_SLOTS];
	/*
	 * The touch pad's swipe of two fingers while Wiseview or the switcher
	 * shows (swipe.h, ws142-p009): one swipe a step, from the fingers
	 * landing until their lifting (the gesture SWIPE2's end).
	 */
	struct kwl_swipe pad_swipe;
	/* App Home's Power Off dialog (power-dialog.c, ws099-p037): shown over everything while open. */
	struct kwl_power_dialog power_dialog;
	/*
	 * The system bar's docked layout (ws099-p034b): how far it is (0, a
	 * floating window's: the status and the clock at the right end; 1, a
	 * docked window's: the window's buttons there, the status and the clock
	 * left of them), the animation's ends and when it began (ms).
	 */
	float bar_dock;
	float bar_dock_from;
	float bar_dock_to;
	uint64_t bar_dock_ms;
	/*
	 * App Home's pages (ws035-p071): the page shown; a press on Home that
	 * may become a page drag (where it started, the application under it,
	 * whether it has moved enough) and the drag's offset in pixels; the
	 * snap to a page (from and to as page positions, when it started).
	 */
	unsigned home_page;
	unsigned home_page_press;
	unsigned home_page_dragging;
	int32_t home_page_start_x;
	int32_t home_page_start_y;
	int home_page_app;
	int32_t home_page_offset;
	unsigned home_page_moving;
	float home_page_from;
	float home_page_to;
	uint64_t home_page_start_ms;
	/*
	 * The last launch: the application (its icon grows as Home closes),
	 * and whether its first window is still to grow from the icon, since
	 * when, and the icon's rectangle.
	 */
	int home_launch_app;
	unsigned home_launching;
	uint64_t home_launch_ms;
	int32_t home_launch_rect[4];
	/* The cursor: a client's surface, zdesktop's arrow when there is none, or hidden. */
	struct kwl_object *cursor_surface;
	/*
	 * The client whose request (a cursor surface, none to hide it, or a
	 * shape) the cursor state is; NULL for zdesktop's own.  Its state is
	 * shown only while the pointer is over that client's window (BUG-118);
	 * cleared when the state goes back to the arrow or the client goes.
	 */
	struct kwl_client *cursor_client;
	/* What the log last said about showing that client's cursor (0 none said, 1 not shown, 2 shown), for the tests. */
	unsigned cursor_client_logged;
	int32_t cursor_hotspot_x;
	int32_t cursor_hotspot_y;
	unsigned cursor_hidden;
	struct kwl_import *arrow;
	/*
	 * Nonzero from the start until the pointer first moves (input.c): the
	 * cursor is not drawn before, so a touch screen shows no arrow resting
	 * in the middle of the greeter or the desktop (ws035-p116).
	 */
	unsigned pointer_unmoved;
	/*
	 * The surface whose client was told the pointer entered it (seat.c): it
	 * hears the pointer's events.  It is the focused window, or the
	 * sub-surface of it under the pointer (ws035-p077); while a popup's grab
	 * has the pointer, the surface of the grab's chain under it, or none.
	 * It is cleared before that surface is freed.
	 */
	struct kwl_object *pointer_surface;
	/*
	 * Popups (popup.c): the topmost popup holding the seat's grab (NULL for
	 * none); whether the grab has the pointer (from its first popup shown
	 * until the grab ends); a button whose press dismissed the popups (its
	 * release is eaten too); the order the next popup is made in.
	 */
	struct kwl_object *popup_grab;
	unsigned pointer_grabbed;
	uint32_t popup_eaten_button;
	uint64_t popup_order;
	/*
	 * The pointer's buttons held now (bit n for BTN_LEFT + n), and the serial
	 * of the last press sent to a client, which a move or a resize must name
	 * (toplevel.c).  The window being resized (NULL for none), where the
	 * pointer was and the window's size when the resize started.
	 */
	uint32_t buttons_down;
	uint32_t press_serial;
	/*
	 * Borrowed identity and actual button of the last press delivered to a
	 * client. Surface teardown and that button's release clear this origin.
	 * Accepted xdg move/resize alone copies the origin into the interactive
	 * state; server-owned SSD gestures never establish client ownership.
	 * The window and origin may differ for a subsurface. Both are cleared
	 * before either identity is freed, or when the operation ends.
	 */
	struct kwl_object *press_surface;
	uint32_t press_button;
	/*
	 * Where the pointer was on the output when that press was delivered.  A
	 * client's move or resize names the press and may arrive after the
	 * pointer has gone on; it is anchored here, so the motion made while the
	 * client was answering is not lost (BUG-125).
	 */
	int32_t press_x;
	int32_t press_y;
	struct kwl_object *interactive_window;
	struct kwl_object *interactive_surface;
	uint32_t interactive_button;
	/*
	 * The time of the pointer event being handled (evdev's, in the wrapping
	 * milliseconds Wayland carries), set by kwl_seat_motion and
	 * kwl_seat_button before anything hears the event; the corner's swipe
	 * measures its speed with it (corner.c).
	 */
	uint32_t input_time;
	/*
	 * Where the contact the shell's pointer path carries comes from: the
	 * pointer, except while touch.c passes a finger through the shell as
	 * the pointer's left button (it sets KWL_CONTACT_TOUCH around each call
	 * and puts KWL_CONTACT_POINTER back), so the edge gestures know the
	 * finger's contact from the mouse's (corner.c).
	 */
	enum kwl_contact_source shell_source;
	/*
	 * The clipboard (data.c): the wl_data_source set as the selection (NULL
	 * for an empty clipboard), and the number of the client last told it
	 * (the keyboard's client; 0 for none), so a focus change tells the new
	 * one.
	 */
	struct kwl_object *selection;
	uint64_t selection_client;
	/* Whether the selection is zdesktop's own, an item of the clipboard's history (clipboard.c, ws102-p018; selection is NULL then). */
	unsigned selection_offered;
	/*
	 * The primary selection (primary.c, ws035-p100): the source set as it
	 * (NULL for none), and the number of the client last told it (0 for
	 * none), kept like the clipboard's.
	 */
	struct kwl_object *primary;
	uint64_t primary_client;
	/*
	 * The bounds last sent to windows (xdg_toplevel.configure_bounds,
	 * protocol.c): when the space for bodies no longer matches, the windows
	 * hear the new one (0 before any was sent).
	 */
	int32_t bounds_width;
	int32_t bounds_height;
	/*
	 * Drag and drop (data.c, ws035-p084), while dnd_active: the drag's
	 * wl_data_source (NULL for a drag inside its own client), the surface
	 * it started from, its icon surface (NULL for zdesktop's badge), the
	 * surface under the pointer that heard enter and the data device it
	 * heard it on, the offer made for it, and the titlebar told the part
	 * of a breadcrumb the drag is over (NULL for none) with that part.
	 * Each is cleared when its object goes.
	 */
	unsigned dnd_active;
	struct kwl_object *dnd_source;
	struct kwl_object *dnd_origin;
	struct kwl_object *dnd_icon;
	struct kwl_object *dnd_target;
	struct kwl_object *dnd_target_device;
	struct kwl_object *dnd_offer;
	struct kwl_object *dnd_titlebar;
	uint32_t dnd_part_id;
	uint32_t dnd_part_detail;
	/*
	 * The serial of the enter the drag's target heard, and after a drop the
	 * dropped-on client's number and that serial: a context menu answering
	 * it (the "ask" action's choice, ws035-p088) is taken like one
	 * answering a press (menu.c).
	 */
	uint32_t dnd_enter_serial;
	uint64_t dnd_drop_client;
	uint32_t dnd_drop_serial;
	/*
	 * The cursor shape the pointer's client asked for (wp_cursor_shape_v1,
	 * cursor.c, ws035-p080; 0 for zdesktop's arrow), and the images of the
	 * shapes zdesktop draws, by the index cursor.c gives them (NULL until
	 * made).
	 */
	uint32_t cursor_shape;
	struct kwl_import *cursor_images[KWL_CURSOR_IMAGES];
	/*
	 * The edges of the window frame under the pointer, or of the resize it
	 * started (the glass look's frames, shell.c; KWL_EDGE_* bits of
	 * toplevel.h, 0 over no frame): while not 0 the cursor is that frame's
	 * resize arrow, over the client's own cursor (cursor.c).
	 */
	uint32_t frame_edges;
	struct kwl_object *resize;
	int32_t resize_pointer_x;
	int32_t resize_pointer_y;
	int32_t resize_width;
	int32_t resize_height;
	/*
	 * The system's input method and the text inputs it serves
	 * (input-method.c, text-input.c, ws095-p004); NULL until
	 * kwl_ime_start makes it.
	 */
	struct kwl_ime *ime;
};

uint64_t kwl_milliseconds(void);
uint64_t kwl_microseconds(void);
void kwl_request_stop(void);
int kwl_greeter_open(struct kwl_server *server);
int kwl_greeter_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_greeter_key(struct kwl_server *server, uint32_t key, uint32_t state);
void kwl_greeter_motion(struct kwl_server *server);
void kwl_greeter_wheel(struct kwl_server *server, int32_t vertical);
void kwl_greeter_pad_scroll(struct kwl_server *server, int64_t down_um);
void kwl_greeter_pad_end(struct kwl_server *server, int64_t gesture_up_um);
void kwl_greeter_tick(struct kwl_server *server);
void kwl_handoff_wait(struct kwl_server *server);
int kwl_handoff_logout(struct kwl_server *server);
void kwl_handoff_tick(struct kwl_server *server);
void kwl_handoff_stop(void *data, unsigned reason);
void kwl_handoff_answer(void *data, unsigned request, int error);
void kwl_backend_session_paused(void *data);
void kwl_backend_session_resumed(void *data);
void kwl_backend_input_paused(void *data, const char *path);
void kwl_backend_input_resumed(void *data, const char *path, int descriptor);
void kwl_backend_input_gone(void *data, const char *path);
int kwl_backend_input_known(void *data, const char *path);
int kwl_backend_input_found(void *data, int descriptor, const char *path, const struct kl_backend_input_caps *caps);
void kwl_backend_input_changed(void *data);
void kwl_backend_power_changed(void *data);
void kwl_backend_power_button(void *data, unsigned button);
void kwl_backend_lid_changed(void *data, unsigned open);
void kwl_power_read(struct kwl_server *server);
int kwl_lock(struct kwl_server *server, const char *reason);
void kwl_lock_release(struct kwl_server *server, const char *reason);
void kwl_lid_screen_restore(struct kwl_server *server);
void kwl_lid_follow(struct kwl_server *server, unsigned open);
void kwl_screen_off(struct kwl_server *server, const char *why);
int kwl_output_lid_matters(struct kwl_server *server);
void kwl_outputs_changed(struct kwl_server *server);
int kwl_glass_resize(struct kwl_server *server);
void kwl_glass_output_resized(struct kwl_server *server);
void kwl_output_tick(struct kwl_server *server);
int kwl_output_external_available(struct kwl_server *server);
int kwl_output_use_external(struct kwl_server *server);
int kwl_output_use_internal(struct kwl_server *server);
int kwl_output_display_internal(struct kwl_server *server, VkDisplayKHR display);

/* A wl_output binding of a head that closed (ws113-p004b). */
#define KWL_OUTPUT_GONE	0xffffffffU

/* What a display shows, as a client's wl_output tells it (heads.c, ws113-p004b): its place in the logical plane and its mode. */
struct kwl_output_view {
	uint32_t index;
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
};

int kwl_output_view(struct kwl_server *server, uint32_t head, struct kwl_output_view *view);
uint32_t kwl_output_head_of_global(struct kwl_server *server, uint32_t name);
unsigned kwl_output_head_globals(struct kwl_server *server, uint32_t *names, unsigned room);
void kwl_output_global_add(struct kwl_server *server, uint32_t name);
void kwl_output_global_remove(struct kwl_server *server, uint32_t name);
void kwl_sleep_tick(struct kwl_server *server);
void kwl_sleep_button(struct kwl_server *server);
int kwl_sleep_request(struct kwl_server *server, enum kwl_sleep_via via);
int kwl_sleep_answers(struct kwl_server *server);
int kwl_sleep_waiting(struct kwl_server *server);
void kwl_sleep_answer(struct kwl_server *server, int error);
int kwl_sleep_keys_held_now(struct kwl_server *server);
int kwl_sleep_lid_opened(struct kwl_server *server);
int kwl_greeter_starting(void);
void kwl_greeter_say(struct kwl_server *server, const char *text);
void kwl_greeter_answer(struct kwl_server *server, unsigned request, int error);
int kwl_emit(struct kwl_client *client, uint32_t object, uint32_t opcode, const void *payload, size_t size);
int kwl_emit_fd(struct kwl_client *client, uint32_t object, uint32_t opcode, const void *payload, size_t size, int descriptor);
void kwl_packet_free(struct kwl_packet *packet);
int kwl_flush(struct kwl_client *client);
int kwl_read(struct kwl_client *client);
int kwl_dispatch(struct kwl_client *client, uint32_t id, uint32_t opcode, const unsigned char *payload, size_t size);
int kwl_error(struct kwl_client *client, uint32_t object, const char *reason);
int kwl_error_code(struct kwl_client *client, uint32_t object, uint32_t code, const char *reason);
int kwl_take_fd(struct kwl_client *client);
void kwl_delete_id(struct kwl_client *client, uint32_t id);
void kwl_client_destroy(struct kwl_client *client);
struct kwl_object *kwl_find(struct kwl_client *client, uint32_t id);
struct kwl_object *kwl_create(struct kwl_client *client, uint32_t id, enum kwl_kind kind, uint32_t version);
struct kwl_object *kwl_create_server(struct kwl_client *client, enum kwl_kind kind, uint32_t version);
void kwl_object_destroy(struct kwl_object *object);
void kwl_buffer_get(struct kwl_object *buffer);
void kwl_buffer_put(struct kwl_object *buffer);
void kwl_buffer_size(const struct kwl_object *buffer, uint32_t *width, uint32_t *height);
void kwl_callbacks_done(struct kwl_object **callbacks);
void kwl_schedule(struct kwl_server *server);
void kwl_transition_request(struct kwl_server *server, const char *what);
void kwl_frame_done(struct kwl_server *server);
int kwl_compose_open(struct kwl_server *server);
int kwl_compose_output_prepare(struct kwl_server *server);
int kwl_compose_output_open(struct kwl_server *server);
void kwl_compose_output_close(struct kwl_server *server);
int kwl_compose_draw(struct kwl_server *server);
int kwl_compose_complete(struct kwl_server *server);

/* The test images' screen capture (shot.c, or shot-none.c elsewhere; ws173-p002). */
int kwl_shot_enabled(void);
int kwl_shot_waiting(void);

/* The game mode: a fullscreen video or game shown without composing (scanout.c, ws122-p005b). */
struct kl_backend_scanout;
int kwl_scanout_pass(struct kwl_server *server, struct kwl_object *top);
void kwl_scanout_leave(struct kwl_server *server, unsigned reason);
void kwl_scanout_surface_gone(struct kwl_server *server, struct kwl_object *surface);
void kwl_shot_open(struct kwl_server *server);
void kwl_shot_close(struct kwl_server *server);
void kwl_shot_tick(struct kwl_server *server);
void kwl_shot_complete(struct kwl_server *server);
void kwl_compose_quiesce(struct kwl_server *server);
void kwl_compose_close(struct kwl_server *server);
VkResult kwl_import_adopt(struct kwl_object *buffer, VkImage image, VkDeviceMemory memory, uint32_t width, uint32_t height, VkFormat format);
void kwl_import_destroy(struct kwl_object *buffer);
void kwl_import_set_alpha(struct kwl_object *buffer, uint32_t alpha);
int kwl_shm_upload(struct kwl_server *server);
void kwl_shm_image_destroy(struct kwl_server *server, struct kwl_object *surface);
void kwl_pool_put(struct kwl_pool *pool);
int kwl_shm_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
int kwl_shm_bind(struct kwl_object *shm);
void kwl_cursor_default(struct kwl_server *server);
int kwl_cursor_client_shown(struct kwl_server *server);
int kwl_arrow_create(struct kwl_server *server);
void kwl_arrow_destroy(struct kwl_server *server);
struct kwl_object *kwl_top_window(struct kwl_server *server);
struct kwl_object *kwl_output_top_window(struct kwl_server *server, unsigned slot);
int kwl_glass_still(struct kwl_server *server);
int kwl_glass_body_damage(struct kwl_server *server, struct kwl_object *surface, int32_t *rect);
int kwl_glass_pointer_calm(struct kwl_server *server, int32_t x, int32_t y);
void kwl_damage_pointer(struct kwl_server *server, int32_t old_x, int32_t old_y);
void kwl_damage_commit(struct kwl_server *server, struct kwl_object *surface, struct kwl_object *previous);
void kwl_window_bounds_refresh(struct kwl_server *server);
int kwl_window_send_configure(struct kwl_object *surface);
int kwl_window_enter_fullscreen(struct kwl_object *surface);
int kwl_window_leave_fullscreen(struct kwl_object *surface);
void kwl_window_centre(struct kwl_server *server, struct kwl_object *surface);
int kwl_fence_ready(struct kwl_server *server, struct kwl_object *surface);
int kwl_compose_waiting(struct kwl_server *server);
void kwl_compose_poll(struct kwl_server *server);
int kwl_glass_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_glass_motion(struct kwl_server *server);
void kwl_glass_gesture(struct kwl_server *server, uint32_t gesture, uint32_t phase, int32_t travel_um, int32_t speed);
int kwl_glass_apps_room(struct kwl_server *server, unsigned slot, int32_t *left, int32_t *right, int32_t *top);
void kwl_glass_switch_to(struct kwl_server *server, struct kwl_object *surface, const char *via);
int kwl_glass_unfullscreen_docks(struct kwl_server *server, struct kwl_object *surface);
void kwl_glass_activate(struct kwl_server *server, struct kwl_object *surface, const char *via);

/* The desktop's appearance (theme.c, ws089-p017). */
int kwl_theme_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
int kwl_theme_bind(struct kwl_object *theme);
void kwl_theme_changed(struct kwl_server *server);

/*
 * The accent the user chose (theme.c, ws179-p001): a part of its colours
 * for a light or a dark ground with an opacity, as 0 to 1 RGBA, and the
 * drawing of shapes in it as they are (the dark appearance's mapping of
 * the colours left out, which would turn a grey accent or the ink over).
 */
#define KWL_ACCENT_FILL		0U
#define KWL_ACCENT_INK		1U
#define KWL_ACCENT_TEXT		2U
void kwl_accent_colour(const struct kwl_server *server, int dark_ground, unsigned part, float alpha, float *out);
unsigned kwl_accent_as_is(struct kwl_server *server);
void kwl_accent_done(struct kwl_server *server, unsigned previous);
int kwl_glass_open_docked(struct kwl_server *server, struct kwl_object *surface);
void kwl_glass_open_wiseview(struct kwl_server *server, const char *via);

/* The applications' icons in the system bar and their previews (apps-bar.c, ws142-p004; the drawing is in glass.h). */
int kwl_apps_bar_motion(struct kwl_server *server);
int kwl_apps_bar_button(struct kwl_server *server, uint32_t button, uint32_t state_value);
int kwl_apps_bar_key(struct kwl_server *server, uint32_t key, uint32_t state_value);
void kwl_apps_bar_tick(struct kwl_server *server);

/* The application switcher (switcher-shell.c, ws142-p005; the drawing is in glass.h). */
int kwl_glass_switch_place(struct kwl_server *server, unsigned *placement);
int kwl_switch_on(struct kwl_server *server);
int kwl_switch_open(struct kwl_server *server, unsigned via);
void kwl_switch_step(struct kwl_server *server, int delta, const char *how);
void kwl_switch_commit(struct kwl_server *server, const char *how);
void kwl_switch_cancel(struct kwl_server *server, const char *why);
int kwl_switch_key(struct kwl_server *server, uint32_t key, uint32_t state);
int kwl_switch_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_switch_pad_swipe(struct kwl_server *server, unsigned direction);
int kwl_glass_pad_scroll(struct kwl_server *server, int32_t vertical, int32_t horizontal, int natural);
void kwl_power_dialog_open(struct kwl_server *server, const char *source);
int kwl_power_dialog_showing(struct kwl_server *server);
int kwl_power_dialog_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_power_dialog_key(struct kwl_server *server, uint32_t key, uint32_t state);
int kwl_power_dialog_motion(struct kwl_server *server);
int kwl_power_dialog_swipe(struct kwl_server *server, int down);
void kwl_power_dialog_tick(struct kwl_server *server);
void kwl_switch_tick(struct kwl_server *server);
void kwl_glass_place(struct kwl_server *server, struct kwl_object *surface, int32_t width, int32_t height, int32_t step);
void kwl_glass_space(struct kwl_server *server, int32_t *width, int32_t *height);
void kwl_glass_fit(struct kwl_server *server, int32_t width, int32_t height, int32_t *x, int32_t *y);
void kwl_glass_tick(struct kwl_server *server);
void kwl_glass_prefetch(struct kwl_server *server);
int kwl_glass_landscape(struct kwl_server *server);
const struct kl_backend_protocol_host *kwl_gpu_host(void);
struct kl_backend_resource *kwl_gpu_resource(struct kwl_object *object);
int kwl_glass_wallpaper_begin(struct kwl_server *server, const char *path);
int kwl_glass_wallpaper_poll(struct kwl_server *server, int *error);
void kwl_settings_open(struct kwl_server *server);
void kwl_settings_tick(struct kwl_server *server);
void kwl_settings_logout(struct kwl_server *server);
void kwl_settings_close(struct kwl_server *server);
int kwl_settings_kept(struct kwl_server *server, const char *name, int *number);
int kwl_settings_number(struct kwl_server *server, const char *name, int *number);
int kwl_settings_global_visible(struct kwl_client *client, enum kwl_kind kind);
int kwl_settings_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
int kwl_settings_home(char *home, size_t size);
int kwl_system_bind(struct kwl_object *manager);
int kwl_system_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_system_tick(struct kwl_server *server);
void kwl_system_power_changed(struct kwl_server *server);
void kwl_system_sharing_answer(struct kwl_server *server, int error);
int kwl_system_pin_answer(struct kwl_server *server, int error);
int kwl_system_key_touch(struct kwl_server *server);
void kwl_system_enrolled_answer(struct kwl_server *server, int error);
void kwl_system_network_changed(struct kwl_server *server, unsigned changed);
int kwl_system_network_done(struct kwl_server *server, unsigned request, int error);
int kwl_system_bar_save_key(struct kwl_server *server, const char *ssid, const char *key);
void kwl_system_bar_saved(struct kwl_server *server);
void kwl_system_network_gone(struct kwl_object *object);
void kwl_system_close(struct kwl_server *server);
int kwl_sysmon_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
int kwl_sysmon_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_sysmon_tick(struct kwl_server *server);
void kwl_sysmon_close(struct kwl_server *server);
int kwl_notify_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
int kwl_notify_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
int kwl_mail_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
int kwl_mail_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
int kwl_phone_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
int kwl_phone_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
int kwl_printers_available(void);
int kwl_printers_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
int kwl_printers_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_printers_tick(struct kwl_server *server);

/* The system extension's displays and the panel's light (displays-shell.c, ws113-p005). */
int kwl_displays_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
int kwl_displays_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_displays_tell(struct kwl_server *server);
int kwl_displays_key(struct kwl_server *server, uint32_t key, uint32_t state);
void kwl_displays_tick(struct kwl_server *server);

/* The outputs of the plane, the windows and the pointer on them (heads.c, ws113-p007). */
unsigned kwl_outputs(struct kwl_server *server, struct kwl_plane_rect *outputs);
int kwl_output_rect(struct kwl_server *server, unsigned slot, struct kwl_plane_rect *rect);
unsigned kwl_output_at(struct kwl_server *server, int32_t x, int32_t y);
int32_t kwl_output_top(struct kwl_server *server, unsigned slot);
unsigned kwl_window_output(struct kwl_object *surface);
void kwl_window_to_output(struct kwl_server *server, struct kwl_object *window, unsigned slot, const char *why);
void kwl_window_set_output(struct kwl_server *server, struct kwl_object *window, unsigned slot, const char *why);
void kwl_pointer_relative(struct kwl_server *server, int32_t dx, int32_t dy, int32_t *x, int32_t *y);
void kwl_pointer_absolute(struct kwl_server *server);
uint32_t kwl_notify_post_system(struct kwl_server *server, const char *title, const char *body, unsigned flags);
struct kwl_notify_model *kwl_notify_model(void);
/* The notifications' popup (notify-popup.c) and what it does to them (notify-shell.c), ws156-p003. */
void kwl_notify_hide_shown(struct kwl_server *server);
size_t kwl_notify_clear_log(struct kwl_server *server);
int kwl_notify_log_key(struct kwl_server *server, uint32_t key, uint32_t state);
int kwl_notify_dismiss_id(struct kwl_server *server, uint32_t id);
int kwl_notify_activate(struct kwl_server *server, uint32_t id);
uint32_t kwl_notify_system_post(struct kwl_server *server, const char *title, const char *body, unsigned flags, const char *command);
void kwl_notify_system_activated(struct kwl_server *server, uint32_t id);
void kwl_notify_battery(struct kwl_server *server);
void kwl_notify_popup_tick(struct kwl_server *server);
int kwl_notify_popup_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_notify_popup_showing(void);
float kwl_home_progress(struct kwl_server *server);
void kwl_home_layer(struct kwl_server *server, float progress, float *x, float *y, float *scale, float *opacity);
void kwl_home_pad(struct kwl_server *server, uint32_t phase, int32_t travel_um, int32_t speed);
int kwl_home_edge_press(struct kwl_server *server);
void kwl_home_close_now(struct kwl_server *server, const char *via);
int kwl_home_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_home_motion(struct kwl_server *server);
int kwl_home_key(struct kwl_server *server, uint32_t key, uint32_t state);
int kwl_home_field_state(struct kwl_server *server, char *text, size_t size, int32_t *cursor, int32_t *anchor, int32_t *rectangle);
void kwl_home_field_input(struct kwl_server *server, const char *preedit, const char *commit, uint32_t before);
void kwl_home_tick(struct kwl_server *server);
int kwl_home_axis(struct kwl_server *server, int32_t vertical, int32_t horizontal);
int kwl_home_launched(struct kwl_server *server, int32_t *rect);
int kwl_home_open_app(struct kwl_server *server, const char *name, const char *via, int *running);
void kwl_home_dismiss(struct kwl_server *server, const char *via);
void kwl_home_toggle(struct kwl_server *server, const char *via);
pid_t kwl_spawn(struct kwl_server *server, const char *command);

/* The top-right corner's swipe that brings Notes (corner.c; the drawing is in glass.h). */
int kwl_corner_contact_begin(struct kwl_server *server, enum kwl_contact_source source, int32_t x, int32_t y, uint32_t time);
int kwl_corner_contact_move(struct kwl_server *server, int32_t x, int32_t y, uint32_t time);
int kwl_corner_contact_end(struct kwl_server *server, int32_t x, int32_t y, uint32_t time);
int kwl_corner_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_corner_motion(struct kwl_server *server);
void kwl_corner_tick(struct kwl_server *server);
int kwl_corner_showing(void);

/*
 * The on-screen keyboard (keyboard.c, ws102; the drawing is in glass.h):
 * its bottom corners' swipe, its panel, and the side of the square in each
 * bottom corner where the swipe starts.
 */
#define KWL_KEYBOARD_ZONE	28
int kwl_keyboard_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_keyboard_motion(struct kwl_server *server);
void kwl_keyboard_tick(struct kwl_server *server);
int kwl_keyboard_showing(void);
int kwl_keyboard_at(int32_t x, int32_t y);
void kwl_keyboard_close(struct kwl_server *server, const char *reason);
void kwl_keyboard_reserved(int32_t *right, int32_t *bottom);
void kwl_keyboard_reserved_now(int32_t *right, int32_t *bottom);
/* A window hung under its parent's title bar (sheet.c, ws090-p014). */
struct kwl_object *kwl_sheet_parent(const struct kwl_object *surface);
struct kwl_object *kwl_sheet_of(const struct kwl_object *parent);
void kwl_sheet_set_parent(struct kwl_object *surface, struct kwl_object *parent);
void kwl_sheet_surface_gone(struct kwl_object *surface);
int kwl_keyboard_touch_down(struct kwl_server *server, uint32_t id, int32_t x, int32_t y, uint32_t time);
int kwl_keyboard_touch_motion(struct kwl_server *server, uint32_t id, int32_t x, int32_t y, uint32_t time);
int kwl_keyboard_touch_up(struct kwl_server *server, uint32_t id, int32_t x, int32_t y, uint32_t time);
void kwl_keyboard_touch_cancel(struct kwl_server *server, uint32_t id);
void kwl_keyboard_inset_notify(struct kwl_server *server, const int32_t *panel);
void kwl_keyboard_predictions(struct kwl_server *server, uint32_t serial, const char *list);

/* The editing operations and the previous application, for the keyboard's tool face (edit.c, ws102-p017). */
int kwl_edit_action(struct kwl_server *server, unsigned action);
int kwl_edit_state(struct kwl_server *server, uint32_t *enabled);
int kwl_focus_previous(struct kwl_server *server);

/*
 * The clipboard's history (clipboard.c, ws102-p018): the last
 * KWL_CLIPBOARD_HISTORY selections' text, newest first, in memory only, for
 * the keyboard's history tab; an item is pasted by making it the
 * selection and sending the paste operation.  The lock screen and Log Out
 * empty it.
 */
#define KWL_CLIPBOARD_HISTORY		10U
#define KWL_CLIPBOARD_TEXT_MAX		(64U * 1024U)
unsigned kwl_clipboard_history_count(struct kwl_server *server);
const char *kwl_clipboard_history_get(struct kwl_server *server, unsigned index, size_t *length);
int kwl_clipboard_history_paste(struct kwl_server *server, unsigned index);
void kwl_clipboard_history_clear(struct kwl_server *server, const char *reason);

/* The edge gestures over a fullscreen window, whether the input is theirs, and whether one shows something (shell.c). */
int kwl_glass_fullscreen_input(struct kwl_server *server);
int kwl_glass_edge_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_glass_edge_motion(struct kwl_server *server);
int kwl_glass_overlay(struct kwl_server *server);
struct kwl_object *kwl_glass_title_at(struct kwl_server *server, int32_t x, int32_t y);
void kwl_glass_lower(struct kwl_server *server, struct kwl_object *surface, const char *via);
void kwl_glass_mapped(struct kwl_server *server, struct kwl_object *surface);
void kwl_glass_forget(struct kwl_server *server, struct kwl_object *surface);
struct kwl_arrange_rect;
void kwl_glass_desktops_pill(struct kwl_server *server, unsigned slot, int32_t *x, int32_t *top, int32_t *width);
int kwl_glass_bar_control_at(struct kwl_server *server, int32_t x, int32_t y);
void kwl_glass_work_area(struct kwl_server *server, unsigned slot, struct kwl_arrange_rect *area);
void kwl_glass_leave_quiet(struct kwl_server *server, unsigned slot, const char *via);
void kwl_glass_place_body(struct kwl_server *server, struct kwl_object *surface, int32_t x, int32_t y, int32_t width, int32_t height);
void kwl_glass_body(struct kwl_server *server, const struct kwl_object *surface, int32_t body[4]);
void kwl_glass_dock_window(struct kwl_server *server, struct kwl_object *surface, const char *via);
int kwl_arrange_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_arrange_key(struct kwl_server *server, uint32_t key, uint32_t state);
int kwl_arrange_motion(struct kwl_server *server);
int kwl_arrange_move_start(struct kwl_server *server, struct kwl_object *surface, int32_t x, int32_t y);
int kwl_arrange_move_end(struct kwl_server *server);
int kwl_arrange_glide(struct kwl_server *server, const struct kwl_object *surface, int32_t body[4]);
void kwl_arrange_tick(struct kwl_server *server);
void kwl_arrange_forget(struct kwl_server *server, struct kwl_object *surface);
void kwl_arrange_end_all(struct kwl_server *server, unsigned output, const char *reason);
void kwl_arrange_mapped(struct kwl_server *server, struct kwl_object *surface);
void kwl_arrange_join_prepare(struct kwl_server *server);
void kwl_arrange_join_opened(struct kwl_server *server, int error, int running);
void kwl_arrange_moved(struct kwl_server *server, struct kwl_object *surface, unsigned from);
void kwl_glass_committed(struct kwl_server *server, struct kwl_object *surface);
int kwl_glass_key(struct kwl_server *server, uint32_t key, uint32_t state);

/* The network's icon in the system bar and its menu (network.c, ws035-p013; the drawing is in glass.h). */
void kwl_network_tick(struct kwl_server *server);
int kwl_network_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_network_key(struct kwl_server *server, uint32_t key, uint32_t state);
int kwl_network_motion(struct kwl_server *server);
int kwl_network_is_open(void);
struct kl_backend_network *kwl_network_watch(void);
void kwl_network_state(struct kl_backend_network_state *state);
size_t kwl_network_scan(struct kl_backend_network_ap *aps, size_t capacity);
void kwl_network_key_failed(struct kwl_server *server, const char *ssid, int error);
void kwl_network_saved(struct kwl_server *server, char (*ssids)[KL_BACKEND_NETWORK_SSID_MAX], size_t count);
void kwl_network_details(struct kwl_server *server, const struct kl_backend_network_link *links, size_t link_count, const char (*dns)[KL_BACKEND_NETWORK_ADDRESS_MAX], size_t dns_count);
void kwl_network_scan_hold(unsigned on);
void kwl_volume_tick(struct kwl_server *server);
void kwl_volume_keep(struct kwl_server *server, const char *why);
void kwl_volume_report(unsigned *restored, unsigned *available, unsigned *value, unsigned *muted);
int kwl_volume_request(struct kwl_server *server, unsigned value, unsigned muted);
int kwl_volume_request_channels(struct kwl_server *server, unsigned left, unsigned right, unsigned muted);
int kwl_volume_feedback(void);
void kwl_volume_audio_state(struct kl_backend_audio_state *state);
int kwl_volume_button(struct kwl_server *server, uint32_t button, uint32_t state);
int kwl_volume_key(struct kwl_server *server, uint32_t key, uint32_t state);
int kwl_volume_motion(struct kwl_server *server);
int kwl_volume_axis(struct kwl_server *server, int32_t vertical, int32_t horizontal);
int kwl_volume_is_open(void);

/* What a toplevel asks the glass look's shell to do (xdg_toplevel requests, ws035-p076). */
#define KWL_TOPLEVEL_MOVE		1
#define KWL_TOPLEVEL_MAXIMIZE		2
#define KWL_TOPLEVEL_UNMAXIMIZE		3
#define KWL_TOPLEVEL_MINIMIZE		4
void kwl_glass_toplevel_request(struct kwl_server *server, struct kwl_object *surface, int request);
void kwl_glass_toplevel_move_end(struct kwl_server *server, struct kwl_object *surface);
uint32_t kwl_next_serial(struct kwl_server *server);
int kwl_seat_bind(struct kwl_object *seat);
int kwl_seat_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_seat_focus(struct kwl_server *server);
void kwl_seat_surface_gone(struct kwl_object *surface);
void kwl_seat_capabilities(struct kwl_server *server);
void kwl_seat_repeat_changed(struct kwl_server *server);
void kwl_seat_motion(struct kwl_server *server, uint32_t time);
int kwl_seat_motion_shell(struct kwl_server *server, uint32_t time);
void kwl_seat_motion_deliver(struct kwl_server *server, uint32_t time);
void kwl_seat_button(struct kwl_server *server, uint32_t time, uint32_t button, uint32_t state);
int kwl_seat_button_shell(struct kwl_server *server, uint32_t time, uint32_t button, uint32_t state);
void kwl_seat_button_deliver(struct kwl_server *server, uint32_t time, uint32_t button, uint32_t state);
void kwl_seat_axis(struct kwl_server *server, uint32_t time, int32_t vertical, int32_t horizontal);
void kwl_seat_axis_finger(struct kwl_server *server, uint32_t time, int32_t vertical, int32_t horizontal, int32_t vertical_units, int32_t horizontal_units);
void kwl_seat_axis_stop(struct kwl_server *server, uint32_t time);
void kwl_seat_frame(struct kwl_server *server);
void kwl_seat_key(struct kwl_server *server, uint32_t time, uint32_t key, uint32_t state);
void kwl_seat_key_deliver(struct kwl_server *server, uint32_t time, uint32_t key, uint32_t state);
void kwl_seat_modifiers(struct kwl_server *server);
/* The operating system through libkeiland-backend (os.c, ws131-p008; the display's two in compose.h). */
struct pollfd;
int kwl_os_open(struct kwl_server *server);
void kwl_os_close(struct kwl_server *server);
size_t kwl_os_poll_count(const struct kwl_server *server);
void kwl_os_poll_fill(struct kwl_server *server, struct pollfd *descriptors);
void kwl_os_poll_done(struct kwl_server *server, const struct pollfd *descriptors);
void kwl_input_scan(struct kwl_server *server);
int kwl_input_probe(struct kwl_server *server, int descriptor, const char *path, const struct kl_backend_input_caps *capabilities);
int kwl_input_alt_held(const struct kwl_server *server);
int kwl_input_attach(struct kwl_server *server, int descriptor, const char *path, unsigned pointer, unsigned keyboard, const struct input_absinfo *x, const struct input_absinfo *y);
void kwl_input_read_devices(struct kwl_server *server, struct kwl_input_device **devices, size_t count);
void kwl_input_tick(struct kwl_server *server, uint64_t now);
void kwl_input_touchpads_changed(struct kwl_server *server);
void kwl_input_close(struct kwl_server *server, struct kwl_input_device *device);
void kwl_input_forget(struct kwl_server *server, struct kwl_input_device *device);
void kwl_input_cleanup(struct kwl_server *server);

#endif
