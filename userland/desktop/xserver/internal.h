/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The types and functions the modules of xserver share.
 *
 * These are the core's modules, none of which does input or output of its
 * own.  client.c keeps the server's life and its clients' connections
 * (their bytes come and go through the transport), protocol.c answers
 * their requests, window.c keeps the windows, pixmaps and graphics
 * contexts and draws into them, rootless.c shows each top-level window as
 * a surface of the backend (whose pixels present-vulkan.c shows through a
 * swapchain when it can), selection.c keeps atoms, properties and
 * selections, and keymap.c, glyphs.c and glx.c are the keyboard's table,
 * the core font and the GLX extension.
 */

#ifndef KEILAND_X11SERVER_INTERNAL_H
#define KEILAND_X11SERVER_INTERNAL_H

#include "userland/desktop/xserver/x11backend.h"

#include <stddef.h>
#include <stdint.h>

/* The fixed tables' sizes: clients, windows, graphics contexts, fonts and pixmaps. */
#define X11_MAX_CLIENTS		8U
#define X11_MAX_WINDOWS		64U
#define X11_MAX_GCS		64U
#define X11_MAX_FONTS		32U
#define X11_MAX_PIXMAPS		16U

/*
 * The most bytes of input a client may have waiting (its transport holds
 * back more than that, so only a client that breaks the protocol reaches
 * it), and of replies and events not yet taken from it.
 */
#define X11_INPUT_CAP		(16U * 1024U * 1024U)
#define X11_OUTPUT_CAP		(4U * 1024U * 1024U)

/* The most bytes of one client's requests one tick handles before it lets the caller run again. */
#define X11_TICK_BYTES		(256U * 1024U)

/* The longest authorization data a connection may be asked for, and the one name checked. */
#define X11_AUTH_MAX		64U
#define X11_AUTH_COOKIE		"MIT-MAGIC-COOKIE-1"

/* The major opcode of BIG-REQUESTS (the core's extensions are numbered from 129; 128 is zedBSD's PutImageRGB24). */
#define X11_BIGREQ_MAJOR	129U

/* How many of a client's last requests the stuck-client report shows. */
#define X11_HISTORY		16U

/* The server's own resources: the root window, its colormap and its one visual. */
#define X11_ROOT_XID		1U
#define X11_COLORMAP_XID	2U
#define X11_VISUAL_XID		3U

/* The owner of the root window: no client. */
#define X11_NO_CLIENT		X11_MAX_CLIENTS

/*
 * Atoms, properties and selections (selection.c, ws035-p087): how many
 * names are given numbers and the longest, the first number given, how
 * many properties are kept, how many selections, and how many of the
 * backend's requests for an X client's text may wait.
 */
#define X11_MAX_ATOMS		64U
#define X11_ATOM_NAME		64U
#define X11_ATOM_FIRST_DYNAMIC	69U
#define X11_MAX_PROPERTIES	32U
#define X11_MAX_SELECTIONS	8U
#define X11_PENDING_SENDS	4U

/* The atoms of the properties kept: WM_NAME, STRING, and zedBSD's icon path. */
#define X11_ATOM_WM_NAME	39U
#define X11_ATOM_WM_CLASS	67U
#define X11_ATOM_STRING		31U
#define X11_ATOM_ICON_PATH	0x5a000001U

/* The event masks the server honours. */
#define X11_MASK_POINTER_MOTION		(1U << 6)
#define X11_MASK_EXPOSURE		(1U << 15)
#define X11_MASK_STRUCTURE_NOTIFY	(1U << 17)
#define X11_MASK_SUBSTRUCTURE_NOTIFY	(1U << 19)
#define X11_MASK_SUBSTRUCTURE_REDIRECT	(1U << 20)

/* The events sent. */
#define X11_EVENT_KEY_PRESS		2U
#define X11_EVENT_KEY_RELEASE		3U
#define X11_EVENT_BUTTON_PRESS		4U
#define X11_EVENT_BUTTON_RELEASE	5U
#define X11_EVENT_MOTION_NOTIFY		6U
#define X11_EVENT_EXPOSE		12U
#define X11_EVENT_DESTROY_NOTIFY	17U
#define X11_EVENT_MAP_REQUEST		20U
#define X11_EVENT_CONFIGURE_NOTIFY	22U

/* The errors sent. */
#define X11_BAD_REQUEST		1U
#define X11_BAD_WINDOW		3U
#define X11_BAD_ALLOC		11U
#define X11_BAD_ID_CHOICE	14U
#define X11_BAD_LENGTH		16U

/* The keycodes of the keys that have no character (the ASCII keys are their character plus 8). */
#define X11_KEYCODE_UP		0xe0U
#define X11_KEYCODE_DOWN	0xe1U
#define X11_KEYCODE_LEFT	0xe2U
#define X11_KEYCODE_RIGHT	0xe3U
#define X11_KEYCODE_HOME	0xe4U
#define X11_KEYCODE_END		0xe5U
#define X11_KEYCODE_PAGE_UP	0xe6U
#define X11_KEYCODE_PAGE_DOWN	0xe7U
#define X11_KEYCODE_INSERT	0xe8U
#define X11_KEYCODE_DELETE	0xe9U

/* The glyph cell of the core font, the printable ASCII glyphs kept once drawn, and the largest glyph drawn from the font. */
#define X11_GLYPH_BITMAP	32U
#define X11_GLYPHS_CACHED	128U
#define X11_GLYPH_DRAWN_MAX	48U

/* The types other modules keep behind pointers: the font's face, and Vulkan's state and a window's swapchain. */
struct truetype_face;
struct x11_vulkan;
struct x11_vulkan_window;

/*
 * One client's connection.
 *
 * A slot is free while id is 0.  The input buffer holds what the
 * transport has handed in and not yet been answered (a request can arrive
 * in pieces); the output buffer holds replies and events the transport has
 * not taken yet.
 */
struct x11_client {
	/* The number the transport knows the client by: its slot in the low byte, a generation above. */
	uint32_t id;

	/*
	 * The number of a client of this slot the core closed and has not yet
	 * told the transport about (0: none).  The slot is not given to a new
	 * client until the transport has heard.
	 */
	uint32_t closed;

	/* Bytes of input handled since the transport was last told, and whether it has been told of output waiting. */
	size_t consumed;
	int output_told;

	/*
	 * The authorization the setup must present (x11server_connect): its
	 * name is MIT-MAGIC-COOKIE-1 and its data these bytes.  Without one,
	 * any setup is accepted.
	 */
	int auth_required;
	uint8_t auth_data[X11_AUTH_MAX];
	size_t auth_size;

	/*
	 * Nonzero once the client is to end when its output has been taken
	 * (its setup was refused): no more of its input is handled.
	 */
	int ending;

	/* Nonzero once the client has enabled BIG-REQUESTS (a request of length 0 then carries a 32-bit length). */
	int big_requests;

	/* Nonzero for a most-significant-byte-first client, and once its setup is answered. */
	int order;
	int setup;

	/* The sequence number of the last request, and the base of the resource ids it may choose. */
	uint16_t sequence;
	uint32_t base;

	/* Requests read and not yet handled. */
	uint8_t *input;
	size_t used;
	size_t capacity;

	/* Replies and events not yet sent. */
	uint8_t *output;
	size_t output_used;
	size_t output_capacity;

	/*
	 * Nonzero once the connection cannot take what is sent to it (its
	 * client stopped reading past X11_OUTPUT_CAP): the tick closes it
	 * after its pass, not in the middle of a request.
	 */
	int broken;

	/* The last request handled and when (the server's milliseconds), for the stuck-client report. */
	uint8_t last_opcode;
	uint32_t last_ms;

	/* The last requests handled (opcode, length and sequence number, oldest first from history_next), for the same report. */
	uint8_t history_opcode[X11_HISTORY];
	uint32_t history_length[X11_HISTORY];
	uint16_t history_sequence[X11_HISTORY];
	unsigned history_next;
};

/*
 * One window: its place in the tree, its size, its own pixels, and, for a
 * top-level window, the backend's surface that shows it.
 *
 * Windows live in the server's table in stacking order, bottom first; the
 * root window is the first and has no pixels.
 */
struct x11_window {
	uint32_t id;
	uint32_t parent;

	/* The owning client's slot, or X11_NO_CLIENT for the root window. */
	unsigned owner;

	/* The events its owner asked for, and the colour it is cleared to. */
	uint32_t event_mask;
	uint32_t background;

	/* Its place on the root window and its size. */
	int16_t x;
	int16_t y;
	uint16_t width;
	uint16_t height;
	uint16_t border;
	int mapped;

	/* Its pixels (0x00RRGGBB, rows of its width), kept so it can be shown without its client. */
	uint32_t *pixels;

	/* WM_NAME and the icon path, kept as strings. */
	char name[64];
	char icon_path[160];

	/*
	 * A mapped top-level window's surface, the swapchain its pixels are
	 * shown through (NULL: the backend's present shows them), the size,
	 * place and title the surface was last given, whether it must be
	 * drawn whole next time, and the window with its children drawn over
	 * it.
	 */
	struct x11_surface *surface;
	struct x11_vulkan_window *swapchain;
	uint16_t surface_width;
	uint16_t surface_height;
	int16_t surface_x;
	int16_t surface_y;
	int surface_fresh;
	char surface_title[64];
	uint32_t *composite;
};

/*
 * One property kept with a window (selection.c): the window, its name,
 * type and format, and its data (NULL for a free slot).
 */
struct x11_property {
	uint32_t window;
	uint32_t atom;
	uint32_t type;
	uint8_t format;
	uint8_t *data;
	size_t length;
};

/*
 * One selection (selection.c): its atom, the owner window (0 for none; the
 * root window while the backend's text is the selection's), the owner's
 * client slot (X11_NO_CLIENT for the backend) and when it was taken.
 */
struct x11_selection {
	uint32_t atom;
	uint32_t window;
	unsigned owner;
	uint32_t time;
};

/*
 * One graphics context: the colour and font drawing uses.
 */
struct x11_gc {
	uint32_t id;
	uint32_t foreground;
	uint32_t font;
	unsigned owner;
};

/*
 * One open font (every name opens the one core font).
 */
struct x11_font {
	uint32_t id;
	unsigned owner;
};

/*
 * One pixmap: pixels a client draws into and copies onto its windows.
 */
struct x11_pixmap {
	uint32_t id;
	unsigned owner;
	uint16_t width;
	uint16_t height;
	uint32_t *pixels;
};

/*
 * One glyph of the core font: a cell of one bit a pixel, rows of stride
 * bytes, and how far the pen moves after it.
 */
struct x11_glyph {
	unsigned width;
	unsigned height;
	unsigned stride;
	unsigned advance;
	uint8_t bitmap[X11_GLYPH_BITMAP];
};

/*
 * The core font: the face over the TrueType file the caller keeps in
 * memory, the ASCII glyphs drawn so far, and the coverage a glyph is drawn
 * into before it becomes bits.  The face is NULL when there is no font.
 */
struct x11_glyphs {
	struct truetype_face *face;
	struct x11_glyph cache[X11_GLYPHS_CACHED];
	uint8_t cached[X11_GLYPHS_CACHED];
	uint8_t coverage[X11_GLYPH_DRAWN_MAX * X11_GLYPH_DRAWN_MAX];
};

/*
 * The server: the backend, the clients and their resources, what has
 * changed and not been shown, and the pointer's and keyboard's state.
 */
struct x11server {
	/* The root window's size. */
	unsigned width;
	unsigned height;

	/* The backend's functions and their context. */
	const struct x11_backend_ops *ops;
	void *backend;

	/*
	 * Vulkan's state, made with the first swapchain (NULL until then), the
	 * WSI extension the backend's surfaces need, and whether windows are
	 * shown through the backend's present instead (Vulkan was not asked
	 * for, or failed once).
	 */
	struct x11_vulkan *vulkan;
	const char *vulkan_wsi;
	int cpu_present;

	/* The core font. */
	struct x11_glyphs glyphs;

	/* Where log lines go. */
	void (*log)(void *context, const char *line);
	void *log_context;

	/* The server's time (milliseconds, from the last tick), and the generation the next client's number carries. */
	uint32_t now;
	uint32_t generation;

	/* The top-level windows the backend asked to close, handled at the next tick. */
	uint32_t closing[X11_MAX_WINDOWS];
	unsigned closing_count;

	/* When the stuck-client report last looked (the server's milliseconds). */
	uint32_t report_ms;

	/* The clients and their resources. */
	struct x11_client clients[X11_MAX_CLIENTS];
	struct x11_window windows[X11_MAX_WINDOWS];
	unsigned window_count;
	struct x11_gc gcs[X11_MAX_GCS];
	unsigned gc_count;
	struct x11_font fonts[X11_MAX_FONTS];
	unsigned font_count;
	struct x11_pixmap pixmaps[X11_MAX_PIXMAPS];
	unsigned pixmap_count;

	/* What has changed since it was last shown: one box on the root window, half-open. */
	int dirty;
	int dirty_x0;
	int dirty_y0;
	int dirty_x1;
	int dirty_y1;

	/* Where the pointer is on the root window, the buttons held, and the modifiers held. */
	int pointer_x;
	int pointer_y;
	uint16_t buttons;
	uint16_t key_state;

	/* The window with the keyboard's focus. */
	uint32_t focus;

	/*
	 * Atoms, properties and selections (selection.c): the names given
	 * numbers (X11_ATOM_FIRST_DYNAMIC on), the properties kept, the
	 * selections, the backend's requests for an X owner's text waiting
	 * for the owner's answer (the backend's numbers, oldest first), and
	 * the atoms the server uses itself.
	 */
	char atom_names[X11_MAX_ATOMS][X11_ATOM_NAME];
	unsigned atom_count;
	struct x11_property properties[X11_MAX_PROPERTIES];
	struct x11_selection selections[X11_MAX_SELECTIONS];
	unsigned selection_count;
	uint32_t pending_sends[X11_PENDING_SENDS];
	unsigned pending_count;
	uint32_t atom_clipboard;
	uint32_t atom_utf8;
	uint32_t atom_targets;
	uint32_t atom_text;
	uint32_t atom_bridge;

	/* The implicit grab of a button press: its owner's slot (-1 without one) and window. */
	int grab_owner;
	uint32_t grab_window;

	/* A pointer motion held back so that only the last of a burst is sent. */
	int motion_pending;
	unsigned motion_client;
	uint32_t motion_window;
	uint32_t motion_time;
	int motion_x;
	int motion_y;
	uint16_t motion_buttons;
};

/* client.c: the clients' connections, and the log. */
struct x11_client *x11_client_of(struct x11server *server, unsigned owner);
void x11_client_send(struct x11server *server, struct x11_client *client, const void *bytes, size_t length);
void x11_client_close(struct x11server *server, unsigned index);
void x11_log(struct x11server *server, const char *format, ...);

/* protocol.c: requests, replies and events. */
int x11_setup_reply(struct x11server *server, struct x11_client *client);
void x11_request(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
uint16_t x11_read16(const uint8_t *bytes, int msb);
uint32_t x11_read32(const uint8_t *bytes, int msb);
void x11_write16(uint8_t *bytes, uint16_t value, int msb);
void x11_write32(uint8_t *bytes, uint32_t value, int msb);
void x11_reply(struct x11server *server, struct x11_client *client, uint8_t *reply, size_t length);
void x11_input_event(struct x11server *server, unsigned owner, uint8_t type, uint32_t window, uint8_t detail, uint32_t time, uint16_t state);
void x11_expose(struct x11server *server, struct x11_window *window);
void x11_configure_notify(struct x11server *server, struct x11_window *window);
void x11_destroy_notify(struct x11server *server, unsigned owner, uint32_t event_window, uint32_t window);
void x11_event_swap(uint8_t *event);

/* window.c: windows, pixmaps, graphics contexts and drawing. */
struct x11_window *x11_window_find(struct x11server *server, uint32_t id);
struct x11_pixmap *x11_pixmap_find(struct x11server *server, uint32_t id);
struct x11_gc *x11_gc_find(struct x11server *server, uint32_t id);
uint32_t *x11_pixels_alloc(uint16_t width, uint16_t height, uint32_t color);
int x11_window_resize(struct x11_window *window, uint16_t width, uint16_t height);
void x11_window_destroy(struct x11server *server, uint32_t id);
void x11_window_raise(struct x11server *server, struct x11_window *window);
struct x11_window *x11_window_top_level(struct x11server *server, struct x11_window *window);
struct x11_window *x11_window_at(struct x11server *server, int x, int y);
struct x11_window *x11_window_child_at(struct x11server *server, uint32_t parent, int x, int y);
void x11_window_fill(struct x11server *server, struct x11_window *window, int x, int y, int width, int height, uint32_t color);
void x11_pixmap_fill(struct x11_pixmap *pixmap, int x, int y, int width, int height, uint32_t color);
void x11_draw_text(struct x11server *server, struct x11_window *window, const struct x11_gc *gc, int x, int y, const uint8_t *text, size_t count, int wide);
void x11_mark_dirty(struct x11server *server, int x, int y, int width, int height);
void x11_resources_release(struct x11server *server, unsigned owner);

/* selection.c: atoms, properties, selections and the clipboard's bridge. */
void x11_selection_init(struct x11server *server);
uint32_t x11_atom_intern(struct x11server *server, const char *name, int only_if_exists);
unsigned x11_request_intern_atom(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_get_atom_name(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
int x11_window_class(struct x11server *server, uint32_t window, char *name, size_t size);
unsigned x11_request_change_property(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_get_property(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_delete_property(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_set_selection_owner(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_get_selection_owner(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_convert_selection(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_send_event(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
void x11_selection_forget_window(struct x11server *server, uint32_t window);

/* rootless.c: the top-level windows as the backend's surfaces, and the backend's input. */
void x11_rootless_present(struct x11server *server);
void x11_rootless_closing(struct x11server *server);
void x11_rootless_flush_motion(struct x11server *server);
void x11_rootless_forget(struct x11server *server, struct x11_window *window);

/* present-vulkan.c: a surface shown through a Vulkan swapchain. */
struct x11_vulkan *x11_vulkan_open(const char *wsi);
void x11_vulkan_close(struct x11_vulkan *vulkan);
struct x11_vulkan_window *x11_vulkan_window_open(struct x11server *server, struct x11_surface *surface, unsigned width, unsigned height);
int x11_vulkan_window_present(struct x11_vulkan_window *window, const uint32_t *pixels);
int x11_vulkan_window_resize(struct x11_vulkan_window *window, unsigned width, unsigned height);
void x11_vulkan_window_close(struct x11_vulkan_window *window);

/* keymap.c: the keyboard's table (the evdev side, x11_keymap_keycode, is in x11backend.h). */
uint32_t x11_keymap_keysym(uint32_t keycode);

/* glyphs.c: the core font. */
int x11_glyphs_open(struct x11_glyphs *glyphs, const uint8_t *bytes, size_t size, const char *fallback_path);
int x11_glyph(struct x11_glyphs *glyphs, uint32_t codepoint, struct x11_glyph *glyph);
void x11_glyphs_close(struct x11_glyphs *glyphs);

/* glx.c: the GLX extension. */
void x11_glx_query_extension(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
unsigned x11_glx_request(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);

/* The GLX extension's major opcode. */
#define X11_GLX_MAJOR		144U

#endif
