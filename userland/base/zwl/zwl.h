/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shared state for the Wayland compositor.
 *
 * zwl has two modes (WS035 compositing design, D0).  In window mode it draws
 * a background and every window, bottom to top, with Vulkan into a
 * VK_KHR_display swapchain (compose.c); a window's image is imported once
 * per wl_buffer (import.c).  When the topmost window is fullscreen and its
 * image can be scanned out as the whole output, it enters fullscreen mode:
 * the swapchain is destroyed and that image is presented directly with
 * GPU_DISPLAY_PRESENT (display.c).  The WS014/WS029 scope had
 * no input; WS031 p013 extends it with one seat ("seat0", wl_seat v5):
 *
 * - Every /dev/input/eventN reporting REL_X+REL_Y or ABS_X+ABS_Y is a
 *   pointer and every one reporting KEY_A and KEY_Z is a keyboard.  Nodes are
 *   scanned at start-up and again every ZWL_INPUT_SCAN_MS; a node that fails
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
 *   a cursor surface cannot be committed.  wl_touch is not offered.
 */
#ifndef ZWL_H
#define ZWL_H

#include <uapi/gpu.h>
#include <uapi/gpu-display.h>
#include <uapi/gpu-fence.h>
#include <uapi/input.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* Bound each connection's wire, descriptor, object and queued-event storage. */
#define ZWL_WIRE_MAX		65532U
#define ZWL_RIGHTS_MAX		32U
#define ZWL_OBJECT_MAX		4096U
#define ZWL_OUTPUT_MAX		1048576U

/* Bound the evdev nodes the seat reads and the events one report may carry. */
#define ZWL_INPUT_MAX		16U
#define ZWL_INPUT_FRAME_MAX	64U
#define ZWL_INPUT_PATH_MAX	64U

/* Rescan period for evdev nodes that appear after start-up, in milliseconds. */
#define ZWL_INPUT_SCAN_MS	2000U

/* A window's place when the client chooses its size: cascaded from the centre by this step. */
#define ZWL_CASCADE_STEP	32

/*
 * The glass look (glass.c): the system bar's height, a title bar's height,
 * the gap between a title bar and its body, the margin at the output's edges,
 * and so the highest a body may be.  BTN_LEFT is the left mouse button.
 */
#define ZWL_GLASS_BAR		34
#define ZWL_GLASS_TITLE		44
#define ZWL_GLASS_GAP		8
#define ZWL_GLASS_MARGIN	12
#define ZWL_GLASS_TOP		(ZWL_GLASS_BAR + ZWL_GLASS_MARGIN + ZWL_GLASS_TITLE + ZWL_GLASS_GAP)
#define ZWL_BUTTON_LEFT		0x110U
#define ZWL_TITLE_MAX		64U

struct zwl_server;
struct zwl_client;
struct zwl_object;
struct zwl_compose;
struct zwl_import;

/* Each live protocol identity has one immutable interface and negotiated version. */
enum zwl_kind {
	ZWL_DISPLAY,
	ZWL_REGISTRY,
	ZWL_COMPOSITOR,
	ZWL_SURFACE,
	ZWL_REGION,
	ZWL_CALLBACK,
	ZWL_BUFFER,
	ZWL_OUTPUT,
	ZWL_WM,
	ZWL_XDG_SURFACE,
	ZWL_TOPLEVEL,
	ZWL_FACTORY,
	ZWL_SEAT,
	ZWL_POINTER,
	ZWL_KEYBOARD,
	ZWL_SHM,
	ZWL_SHM_POOL,
};

/* The wl_shm formats (ARGB8888 has alpha; XRGB8888's top byte is unused). */
#define ZWL_SHM_ARGB8888	0U
#define ZWL_SHM_XRGB8888	1U

/*
 * A wl_shm_pool's memory: the client's fd, mapped read-only once at
 * creation and again at resize.  The pool object and each buffer made from
 * it hold a reference; the mapping goes with the last.
 */
struct zwl_pool {
	int fd;
	void *map;
	size_t size;
	unsigned references;
};

/* Where a wl_shm buffer's pixels are in its pool. */
struct zwl_shm_buffer {
	struct zwl_pool *pool;
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
struct zwl_packet {
	struct zwl_packet *next;
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
struct zwl_input_device {
	int fd;
	unsigned live;
	unsigned pointer;
	unsigned keyboard;
	unsigned absolute;
	unsigned discarding;
	int32_t abs_x_minimum;
	int32_t abs_x_maximum;
	int32_t abs_y_minimum;
	int32_t abs_y_maximum;
	int32_t abs_x;
	int32_t abs_y;
	unsigned frame_count;
	struct input_event frame[ZWL_INPUT_FRAME_MAX];
	char path[ZWL_INPUT_PATH_MAX];
};

/* One acquire fence: a fence fd and the payload generation the image waits for. */
#define ZWL_FENCE_MAX 4U
struct zwl_fence {
	int fd;
	uint64_t generation;
};

/*
 * One client-owned protocol object; destroyed buffers remain until all pending,
 * current and scanout holds are gone. Surface state is double-buffered.
 */
struct zwl_object {
	struct zwl_object *next;
	struct zwl_client *client;
	uint32_t id;
	enum zwl_kind kind;
	uint32_t version;
	unsigned dead;
	unsigned holds;
	unsigned busy;
	struct gpu_resource_import image;
	struct zwl_object *surface;
	struct zwl_object *role;
	struct zwl_object *top;
	struct zwl_object *pending;
	struct zwl_object *queued;
	struct zwl_object *current;
	struct zwl_object *callbacks;
	struct zwl_object *committed_callbacks;
	struct zwl_object *callback_next;
	unsigned attached;
	unsigned ready;
	unsigned configured;
	unsigned acknowledged;
	uint32_t configure_serial;
	uint64_t commit_order;
	/* A buffer's Vulkan image for window mode, and whether it can be the whole output. */
	struct zwl_import *import;
	unsigned scanout;
	/* A surface's window: place, stacking (map order, lowest at the bottom) and fullscreen state. */
	unsigned mapped;
	uint64_t map_order;
	int32_t x;
	int32_t y;
	unsigned fullscreen;
	uint32_t window_width;
	uint32_t window_height;
	int32_t window_x;
	int32_t window_y;
	/* A surface whose current image has not been shown yet. */
	unsigned fresh;
	/* The glass look: the toplevel's title, and a maximized window's place and size to go back to. */
	char title[ZWL_TITLE_MAX];
	unsigned maximized;
	int32_t restore_x;
	int32_t restore_y;
	uint32_t restore_width;
	uint32_t restore_height;
	/* A wl_shm buffer's place in its pool (NULL for a GPU buffer), and a pool object's memory. */
	struct zwl_shm_buffer *shm;
	struct zwl_pool *pool;
	/* A surface's damage in buffer pixels, pending and committed (x0, y0, x1, y1), and whether any was given. */
	int32_t damage[4];
	unsigned damaged;
	int32_t committed_damage[4];
	unsigned committed_damaged;
	/* A surface's copy of its wl_shm image that window mode samples, and whether it must be copied again. */
	struct zwl_import *shm_image;
	unsigned shm_upload;
	/* A surface used as the pointer's cursor (wl_pointer.set_cursor). */
	unsigned cursor_role;
	/* A window told its frame is done whose next commit the next frame waits for a moment. */
	unsigned awaited;
	/*
	 * Acquire fences (zed_gpu_buffer_v1 revision two): those for the next
	 * commit, and the committed ones the queued image waits for.
	 */
	struct zwl_fence acquire[ZWL_FENCE_MAX];
	unsigned acquire_count;
	struct zwl_fence fences[ZWL_FENCE_MAX];
	unsigned fence_count;
	/* When the queued fences were committed, and whether a pass found one still pending. */
	uint64_t fence_ms;
	unsigned fence_waited;
};

/* One stream has independent byte and fd FIFOs, plus its own protocol namespace. */
struct zwl_client {
	struct zwl_client *next;
	struct zwl_server *server;
	int fd;
	uint64_t number;
	unsigned fatal;
	uint64_t fatal_time;
	struct zwl_object *objects;
	unsigned object_count;
	unsigned char input[ZWL_WIRE_MAX];
	size_t input_size;
	int rights[ZWL_RIGHTS_MAX];
	unsigned right_count;
	struct zwl_packet *output_head;
	struct zwl_packet *output_tail;
	size_t output_bytes;
};

/* The compositor alone owns the GPU context and the currently scanned-out image. */
/* Cycle counts of the event loop, reported every few seconds (ZWL PERF). */
struct zwl_perf {
	uint64_t window_start_ms;
	uint64_t window_start_cycles;
	uint64_t poll_cycles;
	uint64_t work_cycles;
	uint64_t present_cycles;
	uint64_t present_to_flush_cycles;
	uint32_t passes;
	uint32_t timeouts;
	uint32_t presents;
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

uint64_t zwl_cycles(void);

struct zwl_server {
	struct zwl_perf perf;
	int listener;
	int gpu;
	const char *gpu_path;
	char socket_path[108];
	dev_t socket_device;
	ino_t socket_inode;
	unsigned socket_owned;
	struct zwl_client *clients;
	struct zwl_object *front;
	struct zwl_object *front_surface;
	struct gpu_display_info display;
	uint64_t lease;
	uint64_t frame;
	uint64_t commit_order;
	uint64_t client_serial;
	uint32_t serial;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	uint64_t timeout_ms;
	uint64_t max_frames;
	/* Nonzero with --log-frames: every presentation and buffer release is printed (for the tests that read them). */
	unsigned log_frames;
	/* Nonzero with --direct: no window mode; one surface is shown directly, as before WS035. */
	unsigned direct;
	unsigned failed;
	struct zwl_input_device inputs[ZWL_INPUT_MAX];
	uint64_t input_scan_time;
	uint64_t input_events;
	uint64_t seat_events;
	unsigned capabilities;
	struct zwl_object *focus;
	int32_t pointer_x;
	int32_t pointer_y;
	unsigned modifier_keys;
	uint32_t modifiers;
	/* Window mode: the Vulkan output, whether a frame is due, and the fence fd of the frame in flight. */
	struct zwl_compose *compose;
	unsigned windowed;
	unsigned dirty;
	int frame_fd;
	uint64_t map_order;
	uint32_t windows;
	uint64_t mode_switch_ms;
	/*
	 * Frame pacing: after a frame, the windows it told are waited for, until
	 * all have committed or half the last frame's time (at most 50 ms) has
	 * passed, so that a quick client does not start the next frame without
	 * a slower one.
	 */
	unsigned awaiting;
	uint64_t frame_done_ms;
	uint64_t frame_wait_ms;
	/* The glass look: on, its font, the window being moved and where it was taken, the clock's minute. */
	unsigned glass;
	const char *font_path;
	const char *wallpaper_path;
	float window_opacity;
	struct zwl_object *drag;
	int32_t drag_dx;
	int32_t drag_dy;
	int64_t clock_minute;
	/* The cursor: a client's surface, zdesktop's arrow when there is none, or hidden. */
	struct zwl_object *cursor_surface;
	int32_t cursor_hotspot_x;
	int32_t cursor_hotspot_y;
	unsigned cursor_hidden;
	struct zwl_import *arrow;
};

uint64_t zwl_milliseconds(void);
int zwl_emit(struct zwl_client *client, uint32_t object, uint32_t opcode, const void *payload, size_t size);
int zwl_emit_fd(struct zwl_client *client, uint32_t object, uint32_t opcode, const void *payload, size_t size, int descriptor);
void zwl_packet_free(struct zwl_packet *packet);
int zwl_flush(struct zwl_client *client);
int zwl_read(struct zwl_client *client);
int zwl_dispatch(struct zwl_client *client, uint32_t id, uint32_t opcode, const unsigned char *payload, size_t size);
int zwl_error(struct zwl_client *client, uint32_t object, const char *reason);
int zwl_take_fd(struct zwl_client *client);
void zwl_delete_id(struct zwl_client *client, uint32_t id);
void zwl_client_destroy(struct zwl_client *client);
struct zwl_object *zwl_find(struct zwl_client *client, uint32_t id);
struct zwl_object *zwl_create(struct zwl_client *client, uint32_t id, enum zwl_kind kind, uint32_t version);
void zwl_object_destroy(struct zwl_object *object);
void zwl_buffer_get(struct zwl_object *buffer);
void zwl_buffer_put(struct zwl_object *buffer);
void zwl_buffer_size(const struct zwl_object *buffer, uint32_t *width, uint32_t *height);
void zwl_callbacks_done(struct zwl_object **callbacks);
int zwl_gpu_open(struct zwl_server *server);
int zwl_gpu_import(struct zwl_object *buffer, int descriptor, const struct gpu_image_descriptor *image);
int zwl_present(struct zwl_object *surface);
int zwl_unscan(struct zwl_server *server);
void zwl_schedule(struct zwl_server *server);
void zwl_frame_done(struct zwl_server *server);
int zwl_compose_open(struct zwl_server *server);
int zwl_compose_output_open(struct zwl_server *server);
void zwl_compose_output_close(struct zwl_server *server);
int zwl_compose_draw(struct zwl_server *server);
int zwl_compose_complete(struct zwl_server *server);
void zwl_compose_quiesce(struct zwl_server *server);
void zwl_compose_close(struct zwl_server *server);
int zwl_import_create(struct zwl_object *buffer, int descriptor);
void zwl_import_destroy(struct zwl_object *buffer);
int zwl_shm_upload(struct zwl_server *server);
void zwl_shm_image_destroy(struct zwl_server *server, struct zwl_object *surface);
void zwl_pool_put(struct zwl_pool *pool);
int zwl_shm_request(struct zwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
int zwl_shm_bind(struct zwl_object *shm);
void zwl_cursor_default(struct zwl_server *server);
int zwl_arrow_create(struct zwl_server *server);
void zwl_arrow_destroy(struct zwl_server *server);
struct zwl_object *zwl_top_window(struct zwl_server *server);
int zwl_window_send_configure(struct zwl_object *surface);
int zwl_fence_ready(struct zwl_server *server, struct zwl_object *surface);
int zwl_glass_button(struct zwl_server *server, uint32_t button, uint32_t state);
int zwl_glass_motion(struct zwl_server *server);
void zwl_glass_place(struct zwl_server *server, struct zwl_object *surface, int32_t width, int32_t height, int32_t step);
void zwl_glass_tick(struct zwl_server *server);
uint32_t zwl_next_serial(struct zwl_server *server);
int zwl_seat_bind(struct zwl_object *seat);
int zwl_seat_request(struct zwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void zwl_seat_focus(struct zwl_server *server);
void zwl_seat_surface_gone(struct zwl_object *surface);
void zwl_seat_capabilities(struct zwl_server *server);
void zwl_seat_motion(struct zwl_server *server, uint32_t time);
void zwl_seat_button(struct zwl_server *server, uint32_t time, uint32_t button, uint32_t state);
void zwl_seat_axis(struct zwl_server *server, uint32_t time, int32_t vertical, int32_t horizontal);
void zwl_seat_frame(struct zwl_server *server);
void zwl_seat_key(struct zwl_server *server, uint32_t time, uint32_t key, uint32_t state);
void zwl_seat_modifiers(struct zwl_server *server);
void zwl_input_scan(struct zwl_server *server);
int zwl_input_attach(struct zwl_server *server, int descriptor, const char *path, unsigned pointer, unsigned keyboard, const struct input_absinfo *x, const struct input_absinfo *y);
void zwl_input_read(struct zwl_server *server, struct zwl_input_device *device);
void zwl_input_close(struct zwl_server *server, struct zwl_input_device *device);
void zwl_input_cleanup(struct zwl_server *server);

#endif
