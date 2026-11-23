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
 * (their bytes come and go through the transport), protocol.c dispatches
 * their requests and answers the font, pixmap, drawing and keyboard ones,
 * resource.c keeps the resource ids and the memory each client holds,
 * window.c keeps the window tree and answers its requests, event.c
 * chooses who hears an event and sends it, property.c keeps atoms and
 * properties, selection.c keeps selections and the clipboard's bridge,
 * rootless.c shows each top-level window as a surface of the backend
 * (whose pixels present-vulkan.c shows through a swapchain when it can),
 * and keymap.c, glyphs.c and glx.c are the keyboard's table, the core font
 * and the GLX extension.
 */

#ifndef KEILAND_X11SERVER_INTERNAL_H
#define KEILAND_X11SERVER_INTERNAL_H

#include "userland/desktop/xserver/x11backend.h"

#include <stddef.h>
#include <stdint.h>

/*
 * The clients a server takes unless its options say otherwise, and at
 * most: a client's resource ids are its slot plus one above 22 bits of its
 * own, and an X resource id has 29 bits.
 */
#define X11_CLIENTS_DEFAULT	64U
#define X11_CLIENTS_LIMIT	127U

/* The bits of a resource id a client chooses, and where its slot's base starts. */
#define X11_ID_MASK		0x003fffffU
#define X11_ID_SHIFT		22U

/*
 * The memory one client may hold (its windows, pixmaps, properties and the
 * pixels behind them) and the whole server, unless the options say
 * otherwise.
 */
#define X11_CLIENT_MEMORY_DEFAULT	((size_t)256U * 1024U * 1024U)
#define X11_SERVER_MEMORY_DEFAULT	((size_t)512U * 1024U * 1024U)

/* No client: the owner of the root window and of what the server holds itself. */
#define X11_NO_CLIENT		0xffffffffU

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

/*
 * The core's extensions' major opcodes, numbered from 129 (128 is
 * zedBSD's PutImageRGB24): BIG-REQUESTS and XC-MISC.
 */
#define X11_BIGREQ_MAJOR	129U
#define X11_XCMISC_MAJOR	130U

/* How many of a client's last requests the stuck-client report shows. */
#define X11_HISTORY		16U

/*
 * The server's own resources, in the ids no client chooses: the root
 * window, its colormap and its one visual.  None of them is 0 or 1, which
 * some requests take for None, PointerRoot or InputFocus.
 */
#define X11_ROOT_XID		0x00000100U
#define X11_COLORMAP_XID	0x00000101U
#define X11_VISUAL_XID		3U

/* The focus that is no window: None (keys are dropped) and PointerRoot (keys go to the window under the pointer). */
#define X11_FOCUS_NONE		0U
#define X11_FOCUS_POINTER_ROOT	1U

/* How many windows the backend may ask to close before a tick, and how many surfaces may be open. */
#define X11_CLOSING_MAX		64U
#define X11_SURFACES_MAX	256U

/* The kinds of resources an id names, and the mark of a slot whose resource went. */
#define X11_RESOURCE_WINDOW	1U
#define X11_RESOURCE_PIXMAP	2U
#define X11_RESOURCE_GC		3U
#define X11_RESOURCE_FONT	4U
#define X11_RESOURCE_GONE	0xffU

/* How many selections are kept, and how many of the backend's requests for an X client's text may wait. */
#define X11_MAX_SELECTIONS	8U
#define X11_PENDING_SENDS	4U

/*
 * Atoms: the last of the core's predefined atoms (the next name interned
 * gets the number after it), some of them, and zedBSD's icon path, an atom
 * outside the interned range that the tree's programs use by number.
 */
#define X11_ATOM_LAST_PREDEFINED	68U
#define X11_ATOM_PRIMARY	1U
#define X11_ATOM_ATOM		4U
#define X11_ATOM_STRING		31U
#define X11_ATOM_WM_NAME	39U
#define X11_ATOM_WM_CLASS	67U
#define X11_ATOM_ICON_PATH	0x5a000001U

/* The event masks the core speaks of by name (the bits of a window's event mask and of SendEvent's). */
#define X11_MASK_KEY_PRESS		(1U << 0)
#define X11_MASK_KEY_RELEASE		(1U << 1)
#define X11_MASK_BUTTON_PRESS		(1U << 2)
#define X11_MASK_BUTTON_RELEASE		(1U << 3)
#define X11_MASK_POINTER_MOTION		(1U << 6)
#define X11_MASK_BUTTON1_MOTION		(1U << 8)
#define X11_MASK_BUTTON_MOTION		(1U << 13)
#define X11_MASK_EXPOSURE		(1U << 15)
#define X11_MASK_STRUCTURE_NOTIFY	(1U << 17)
#define X11_MASK_RESIZE_REDIRECT	(1U << 18)
#define X11_MASK_SUBSTRUCTURE_NOTIFY	(1U << 19)
#define X11_MASK_SUBSTRUCTURE_REDIRECT	(1U << 20)
#define X11_MASK_PROPERTY_CHANGE	(1U << 22)

/* Every bit an event mask may have, and those a window may keep from propagating (the device events'). */
#define X11_MASK_ALL			0x01ffffffU
#define X11_MASK_DEVICE			0x00003f4fU

/* The masks only one client at a time may select on a window. */
#define X11_MASK_EXCLUSIVE		(X11_MASK_BUTTON_PRESS | X11_MASK_RESIZE_REDIRECT | X11_MASK_SUBSTRUCTURE_REDIRECT)

/* The core's events. */
#define X11_EVENT_KEY_PRESS		2U
#define X11_EVENT_KEY_RELEASE		3U
#define X11_EVENT_BUTTON_PRESS		4U
#define X11_EVENT_BUTTON_RELEASE	5U
#define X11_EVENT_MOTION_NOTIFY		6U
#define X11_EVENT_EXPOSE		12U
#define X11_EVENT_CREATE_NOTIFY		16U
#define X11_EVENT_DESTROY_NOTIFY	17U
#define X11_EVENT_UNMAP_NOTIFY		18U
#define X11_EVENT_MAP_NOTIFY		19U
#define X11_EVENT_MAP_REQUEST		20U
#define X11_EVENT_REPARENT_NOTIFY	21U
#define X11_EVENT_CONFIGURE_NOTIFY	22U
#define X11_EVENT_CONFIGURE_REQUEST	23U
#define X11_EVENT_GRAVITY_NOTIFY	24U
#define X11_EVENT_RESIZE_REQUEST	25U
#define X11_EVENT_CIRCULATE_NOTIFY	26U
#define X11_EVENT_CIRCULATE_REQUEST	27U
#define X11_EVENT_PROPERTY_NOTIFY	28U
#define X11_EVENT_LAST			34U

/* The bit an event sent with SendEvent carries. */
#define X11_EVENT_SENT			0x80U

/* The core's errors. */
#define X11_BAD_REQUEST		1U
#define X11_BAD_VALUE		2U
#define X11_BAD_WINDOW		3U
#define X11_BAD_PIXMAP		4U
#define X11_BAD_ATOM		5U
#define X11_BAD_CURSOR		6U
#define X11_BAD_FONT		7U
#define X11_BAD_MATCH		8U
#define X11_BAD_DRAWABLE	9U
#define X11_BAD_ACCESS		10U
#define X11_BAD_ALLOC		11U
#define X11_BAD_COLORMAP	12U
#define X11_BAD_GC		13U
#define X11_BAD_ID_CHOICE	14U
#define X11_BAD_NAME		15U
#define X11_BAD_LENGTH		16U
#define X11_BAD_IMPLEMENTATION	17U

/* The classes of a window. */
#define X11_INPUT_OUTPUT	1U
#define X11_INPUT_ONLY		2U

/* The depth of the screen's windows and pixmaps. */
#define X11_DEPTH		24U

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

	/* The memory the client's resources hold (x11_memory_charge): beyond its share, it is refused with BadAlloc. */
	size_t memory;

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
 * One slot of the resource ids' table: the id, its kind and the object (a
 * window, pixmap, graphics context or font).  An empty slot has id 0 and
 * type 0; a slot whose resource went has id 0 and type X11_RESOURCE_GONE,
 * so that a later probe goes on past it.
 */
struct x11_resource {
	uint32_t id;
	unsigned type;
	void *object;
};

/*
 * The events one client asked for on a window.
 */
struct x11_event_select {
	unsigned client;
	uint32_t mask;
};

/*
 * One property of a window (property.c): its name, type and format, its
 * data (16- and 32-bit units kept least significant byte first, whatever
 * the order of the client that wrote them), and the client whose memory
 * holds it (X11_NO_CLIENT: the server's).
 */
struct x11_property {
	uint32_t atom;
	uint32_t type;
	uint8_t format;
	uint8_t *data;
	size_t length;
	unsigned charged;
};

/*
 * One window: its place in the tree, its attributes, the events clients
 * asked for on it, its properties, its own pixels, and, for a top-level
 * window, the backend's surface that shows it.
 *
 * Each window's children are kept in stacking order, bottom first.  The
 * window's place is its origin (inside its border) on the root window.
 */
struct x11_window {
	uint32_t id;

	/* The client that created it, or X11_NO_CLIENT for the root window. */
	unsigned owner;

	/* Its parent, its children bottom to top, and its siblings below and above it. */
	struct x11_window *parent;
	struct x11_window *first_child;
	struct x11_window *last_child;
	struct x11_window *below;
	struct x11_window *above;

	/* InputOutput or InputOnly. */
	unsigned window_class;

	/*
	 * Its attributes: the colour it is cleared to and the background
	 * pixmap named (None, ParentRelative or a pixmap, kept for the
	 * answer), the border, the gravities, backing store, save under,
	 * override-redirect, the colormap and the cursor.
	 */
	uint32_t background;
	uint32_t background_pixmap;
	uint32_t border_pixel;
	uint32_t border_pixmap;
	uint8_t bit_gravity;
	uint8_t win_gravity;
	uint8_t backing_store;
	uint8_t save_under;
	uint8_t override_redirect;
	uint32_t backing_planes;
	uint32_t backing_pixel;
	uint32_t colormap;
	uint32_t cursor;

	/* The events each client asked for, and those kept from reaching its ancestors. */
	struct x11_event_select *selects;
	unsigned select_count;
	unsigned select_capacity;
	uint32_t do_not_propagate;

	/* Its origin on the root window, its size and border, and whether it is mapped. */
	int x;
	int y;
	uint16_t width;
	uint16_t height;
	uint16_t border;
	int mapped;

	/* Its pixels (0x00RRGGBB, rows of its width; none for InputOnly), kept so it can be shown without its client. */
	uint32_t *pixels;

	/* Its properties. */
	struct x11_property *properties;
	unsigned property_count;
	unsigned property_capacity;

	/* WM_NAME, kept as a string for the surface's title. */
	char name[64];

	/*
	 * A mapped top-level window's surface, the swapchain its pixels are
	 * shown through (NULL: the backend's present shows them), the size,
	 * place and title the surface was last given, whether it must be
	 * drawn whole next time, and the window with its children drawn over
	 * it (with the bytes its owner is charged for it).
	 */
	struct x11_surface *surface;
	struct x11_vulkan_window *swapchain;
	uint16_t surface_width;
	uint16_t surface_height;
	int32_t surface_x;
	int32_t surface_y;
	int surface_fresh;
	char surface_title[64];
	uint32_t *composite;
	size_t composite_bytes;
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
	uint8_t depth;
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
	uint32_t closing[X11_CLOSING_MAX];
	unsigned closing_count;

	/* When the stuck-client report last looked (the server's milliseconds). */
	uint32_t report_ms;

	/* The client slots. */
	struct x11_client *clients;
	unsigned clients_max;

	/* The memory one client and the whole server may hold, and what the server holds now (every client's and its own). */
	size_t client_memory_max;
	size_t server_memory_max;
	size_t memory;

	/* The client that grabbed the server (GrabServer), whose requests alone are handled; X11_NO_CLIENT: none. */
	unsigned grabbed;

	/*
	 * The resource ids (resource.c): an open-addressing table of a power
	 * of two slots, the slots taken (with those whose resource went) and
	 * the resources in it.
	 */
	struct x11_resource *resources;
	size_t resource_capacity;
	size_t resource_used;
	size_t resource_count;

	/* The root window, and how many surfaces are open. */
	struct x11_window *root;
	unsigned surface_count;

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

	/* The window with the keyboard's focus (or X11_FOCUS_NONE, X11_FOCUS_POINTER_ROOT). */
	uint32_t focus;

	/*
	 * Atoms (property.c): the names (atom n is entry n - 1, the first
	 * X11_ATOM_LAST_PREDEFINED the core's), their lengths, and an
	 * open-addressing table from a name to its atom (0: an empty slot).
	 */
	char **atom_names;
	uint16_t *atom_lengths;
	uint32_t atom_count;
	uint32_t atom_capacity;
	uint32_t *atom_table;
	size_t atom_table_capacity;

	/*
	 * Selections (selection.c): the selections, the backend's requests
	 * for an X owner's text waiting for the owner's answer (the backend's
	 * numbers, oldest first), and the atoms the server uses itself.
	 */
	struct x11_selection selections[X11_MAX_SELECTIONS];
	unsigned selection_count;
	uint32_t pending_sends[X11_PENDING_SENDS];
	unsigned pending_count;
	uint32_t atom_clipboard;
	uint32_t atom_utf8;
	uint32_t atom_targets;
	uint32_t atom_text;
	uint32_t atom_bridge;

	/* The implicit grab of a button press: the client it is for (X11_NO_CLIENT: none) and its window. */
	unsigned grab_owner;
	uint32_t grab_window;

	/* A pointer motion held back so that only the last of a burst is sent: when, where, and the buttons held. */
	int motion_pending;
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

/* protocol.c: requests, replies and errors. */
int x11_setup_reply(struct x11server *server, struct x11_client *client);
void x11_request(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
uint16_t x11_read16(const uint8_t *bytes, int msb);
uint32_t x11_read32(const uint8_t *bytes, int msb);
void x11_write16(uint8_t *bytes, uint16_t value, int msb);
void x11_write32(uint8_t *bytes, uint32_t value, int msb);
void x11_reply(struct x11server *server, struct x11_client *client, uint8_t *reply, size_t length);
int x11_id_chosen(const struct x11_client *client, uint32_t id);
int x11_values_fit(uint32_t mask, size_t fixed, size_t length);

/* resource.c: resource ids, memory, and XC-MISC. */
int x11_resource_add(struct x11server *server, uint32_t id, unsigned type, void *object);
void *x11_resource_find(struct x11server *server, uint32_t id, unsigned type);
unsigned x11_resource_type(struct x11server *server, uint32_t id);
void x11_resource_remove(struct x11server *server, uint32_t id);
int x11_resource_owned(struct x11server *server, unsigned owner, unsigned type, uint32_t **ids, size_t *count);
void x11_resource_fini(struct x11server *server);
int x11_memory_charge(struct x11server *server, unsigned owner, size_t bytes);
void x11_memory_release(struct x11server *server, unsigned owner, size_t bytes);
unsigned x11_xcmisc_request(struct x11server *server, unsigned index, const uint8_t *request, size_t length);

/* window.c: the window tree, its requests, and drawing into windows and pixmaps. */
int x11_window_root(struct x11server *server, uint32_t background);
struct x11_window *x11_window_find(struct x11server *server, uint32_t id);
struct x11_pixmap *x11_pixmap_find(struct x11server *server, uint32_t id);
struct x11_gc *x11_gc_find(struct x11server *server, uint32_t id);
unsigned x11_request_create_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_change_attributes(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_get_attributes(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_destroy_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_change_save_set(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_reparent_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_map_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_unmap_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_configure_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_circulate_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_get_geometry(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_query_tree(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_translate_coordinates(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
void x11_window_destroy(struct x11server *server, struct x11_window *window);
void x11_window_raise(struct x11server *server, struct x11_window *window);
void x11_window_backend_configure(struct x11server *server, struct x11_window *window, int x, int y, uint16_t width, uint16_t height);
int x11_window_viewable(const struct x11_window *window);
struct x11_window *x11_window_next(struct x11_window *window, const struct x11_window *top);
int x11_window_inferior(const struct x11_window *window, const struct x11_window *ancestor);
struct x11_window *x11_window_top_level(struct x11_window *window);
struct x11_window *x11_window_at(struct x11server *server, int x, int y);
struct x11_window *x11_window_child_at(struct x11_window *parent, int x, int y);
uint32_t *x11_pixels_alloc(uint16_t width, uint16_t height, uint32_t color);
void x11_window_fill(struct x11server *server, struct x11_window *window, int x, int y, int width, int height, uint32_t color);
void x11_pixmap_fill(struct x11_pixmap *pixmap, int x, int y, int width, int height, uint32_t color);
void x11_draw_text(struct x11server *server, struct x11_window *window, const struct x11_gc *gc, int x, int y, const uint8_t *text, size_t count, int wide);
void x11_mark_dirty(struct x11server *server, int x, int y, int width, int height);
void x11_resources_release(struct x11server *server, unsigned owner);

/* event.c: who hears an event, and sending it. */
unsigned x11_event_select(struct x11server *server, struct x11_window *window, unsigned client, uint32_t mask);
uint32_t x11_event_mask_of(const struct x11_window *window, unsigned client);
uint32_t x11_event_masks(const struct x11_window *window);
unsigned x11_event_selector(const struct x11_window *window, uint32_t mask);
void x11_event_forget_client(struct x11_window *window, unsigned client);
void x11_event_send(struct x11server *server, struct x11_client *client, const uint8_t *event);
unsigned x11_event_deliver(struct x11server *server, const struct x11_window *window, uint32_t mask, const uint8_t *event);
void x11_event_structure(struct x11server *server, const struct x11_window *window, uint8_t *event);
void x11_event_expose(struct x11server *server, const struct x11_window *window);
void x11_event_property(struct x11server *server, struct x11_window *window, uint32_t atom, int deleted);
void x11_event_key(struct x11server *server, uint8_t keycode, int pressed, uint32_t time, uint16_t state);
void x11_event_button(struct x11server *server, uint8_t button, int pressed, uint32_t time, uint16_t state);
void x11_event_motion(struct x11server *server, uint32_t time, uint16_t state);
void x11_event_swap(uint8_t *event);
unsigned x11_request_send_event(struct x11server *server, unsigned index, const uint8_t *request, size_t length);

/* property.c: atoms and properties. */
int x11_atoms_init(struct x11server *server);
void x11_atoms_fini(struct x11server *server);
uint32_t x11_atom_intern(struct x11server *server, const char *name, size_t length, int only_if_exists);
int x11_atom_valid(struct x11server *server, uint32_t atom);
const char *x11_atom_name(struct x11server *server, uint32_t atom, size_t *length);
struct x11_property *x11_property_find(struct x11_window *window, uint32_t atom);
unsigned x11_property_set(struct x11server *server, struct x11_window *window, uint32_t atom, uint32_t type, uint8_t format, unsigned mode, const uint8_t *data, size_t length, unsigned writer);
void x11_property_delete(struct x11server *server, struct x11_window *window, uint32_t atom);
void x11_property_forget_window(struct x11server *server, struct x11_window *window);
void x11_property_forget_client(struct x11server *server, unsigned client);
int x11_window_class(struct x11server *server, uint32_t window, char *name, size_t size);
unsigned x11_request_intern_atom(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_get_atom_name(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_change_property(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_delete_property(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_get_property(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_list_properties(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_rotate_properties(struct x11server *server, unsigned index, const uint8_t *request, size_t length);

/* selection.c: selections and the clipboard's bridge. */
void x11_selection_init(struct x11server *server);
unsigned x11_request_set_selection_owner(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_get_selection_owner(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_convert_selection(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
void x11_selection_bridge_done(struct x11server *server, unsigned slot, const uint8_t *event);
void x11_selection_forget_window(struct x11server *server, uint32_t window);
void x11_selection_forget_client(struct x11server *server, unsigned client);
void x11_selection_property_event(struct x11server *server, struct x11_window *window, uint32_t atom, int deleted);

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
