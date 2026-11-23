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
 * their requests, input.c
 * keeps the keyboard, the pointer, the focus and the grabs and answers
 * their requests, resource.c keeps the resource ids and the memory each client holds,
 * window.c keeps the window tree and answers its requests, event.c
 * chooses who hears an event and sends it, property.c keeps atoms and
 * properties, selection.c keeps selections and the clipboard's bridge,
 * draw.c keeps the pixels of the top-level windows, the clips, the
 * exposures and the pixmap and drawing requests, gc.c the graphics
 * contexts, shape.c turns lines, arcs and polygons into spans,
 * colormap.c and colornames.c keep the colormaps and the colours' names,
 * render-cpu.c draws (render.h) and region.c keeps regions,
 * rootless.c shows each top-level window as a surface of the backend
 * (whose pixels present-vulkan.c shows through a swapchain when it can),
 * wm.c is the top-level windows' window manager (ICCCM and EWMH), font.c
 * keeps the core fonts and answers their requests and the text ones,
 * glyphs.c draws their glyphs from the TrueType faces, cursor.c keeps the
 * cursors and tells the backend which one to show, and keymap.c and glx.c
 * are the US keyboard map and the GLX extension.
 */

#ifndef KEILAND_X11SERVER_INTERNAL_H
#define KEILAND_X11SERVER_INTERNAL_H

#include "userland/desktop/xserver/render.h"
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
 * it), and of replies and events not yet taken from it: as long as the
 * longest request and a little more, so that one reply as large (a
 * GetImage of a large window, a GetProperty) fits whole.
 */
#define X11_INPUT_CAP		X11SERVER_REQUEST_MAX
#define X11_OUTPUT_CAP		(X11SERVER_REQUEST_MAX + 65536U)

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

/*
 * The window manager's own window (wm.c): the child of the root window
 * that EWMH's _NET_SUPPORTING_WM_CHECK names, in an id no client chooses.
 */
#define X11_WM_CHECK_XID	0x00000102U

/* The longest title a surface is given, in bytes of UTF-8 with its NUL. */
#define X11_TITLE_MAX		256U

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
#define X11_RESOURCE_CURSOR	5U
#define X11_RESOURCE_COLORMAP	6U
#define X11_RESOURCE_GONE	0xffU

/*
 * How many selections are kept, how many of X's selections the clipboard
 * bridges (CLIPBOARD and PRIMARY), and how many large texts may be given
 * to requestors in pieces at once.
 */
#define X11_MAX_SELECTIONS	8U
#define X11_BRIDGES		2U
#define X11_TRANSFERS		4U

/*
 * Atoms: the last of the core's predefined atoms (the next name interned
 * gets the number after it), some of them, and zedBSD's icon path, an atom
 * outside the interned range that the tree's programs use by number.
 */
#define X11_ATOM_LAST_PREDEFINED	68U
#define X11_ATOM_PRIMARY	1U
#define X11_ATOM_ATOM		4U
#define X11_ATOM_CARDINAL	6U
#define X11_ATOM_STRING		31U
#define X11_ATOM_WINDOW		33U
#define X11_ATOM_WM_HINTS	35U
#define X11_ATOM_WM_NAME	39U
#define X11_ATOM_WM_NORMAL_HINTS	40U
#define X11_ATOM_WM_CLASS	67U
#define X11_ATOM_WM_TRANSIENT_FOR	68U
#define X11_ATOM_ICON_PATH	0x5a000001U

/* The event masks the core speaks of by name (the bits of a window's event mask and of SendEvent's). */
#define X11_MASK_KEY_PRESS		(1U << 0)
#define X11_MASK_KEY_RELEASE		(1U << 1)
#define X11_MASK_BUTTON_PRESS		(1U << 2)
#define X11_MASK_BUTTON_RELEASE		(1U << 3)
#define X11_MASK_ENTER_WINDOW		(1U << 4)
#define X11_MASK_LEAVE_WINDOW		(1U << 5)
#define X11_MASK_POINTER_MOTION		(1U << 6)
#define X11_MASK_BUTTON1_MOTION		(1U << 8)
#define X11_MASK_BUTTON_MOTION		(1U << 13)
#define X11_MASK_KEYMAP_STATE		(1U << 14)
#define X11_MASK_EXPOSURE		(1U << 15)
#define X11_MASK_STRUCTURE_NOTIFY	(1U << 17)
#define X11_MASK_RESIZE_REDIRECT	(1U << 18)
#define X11_MASK_SUBSTRUCTURE_NOTIFY	(1U << 19)
#define X11_MASK_SUBSTRUCTURE_REDIRECT	(1U << 20)
#define X11_MASK_FOCUS_CHANGE		(1U << 21)
#define X11_MASK_PROPERTY_CHANGE	(1U << 22)
#define X11_MASK_OWNER_GRAB_BUTTON	(1U << 24)

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
#define X11_EVENT_ENTER_NOTIFY		7U
#define X11_EVENT_LEAVE_NOTIFY		8U
#define X11_EVENT_FOCUS_IN		9U
#define X11_EVENT_FOCUS_OUT		10U
#define X11_EVENT_KEYMAP_NOTIFY		11U
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
#define X11_EVENT_MAPPING_NOTIFY	34U
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

/*
 * What a window's background is: a pixel, None (never painted), its
 * parent's (ParentRelative), or a copy of a pixmap tiled from the
 * window's origin.  A border is a pixel or a pixmap.
 */
#define X11_BACKGROUND_PIXEL		0U
#define X11_BACKGROUND_NONE		1U
#define X11_BACKGROUND_PARENT		2U
#define X11_BACKGROUND_PIXMAP		3U

/* A graphics context's subwindow modes. */
#define X11_CLIP_BY_CHILDREN		0U
#define X11_INCLUDE_INFERIORS		1U

/* The longest side of a top-level window's pixels: a larger window shows its top-left part. */
#define X11_IMAGE_SIDE_MAX		16384

/* How many changes to window trees may be under way at once (one inside another). */
#define X11_DRAW_FRAMES			4U

/*
 * The keyboard (input.c): the most keysyms one keycode and keycodes one
 * modifier keep (ChangeKeyboardMapping and SetModifierMapping ask for no
 * more), the modifiers, and the bytes of the keys' down bits.
 */
#define X11_KEYSYMS_MAX		8U
#define X11_MODIFIER_KEYS_MAX	8U
#define X11_MODIFIERS		8U
#define X11_KEY_BYTES		32U

/* The pointer's buttons (1 to 9: three, the wheels' four and two on the side). */
#define X11_BUTTONS		9U

/* The device events held while a grab freezes their device. */
#define X11_DEVICE_QUEUE	256U

/* The keycodes lent at once to the characters a backend types (an input method's text). */
#define X11_TEXT_KEYS		8U

/* The device events held: a key, a button, a motion, and the pointer leaving every X window. */
#define X11_DEVICE_KEY		1U
#define X11_DEVICE_BUTTON	2U
#define X11_DEVICE_MOTION	3U
#define X11_DEVICE_LEAVE	4U

/* What froze a device: nothing, the pointer's grab, the keyboard's grab. */
#define X11_FREEZE_NONE		0U
#define X11_FREEZE_POINTER	1U
#define X11_FREEZE_KEYBOARD	2U

/*
 * The faces the core fonts are drawn from (glyphs.c), by their roles: the
 * monospaced one and its bold, the proportional one and its bold.  A role
 * without a file of its own takes another's face (drawn bold when it is a
 * bold role).
 */
#define X11_FACE_MONO		0U
#define X11_FACE_MONO_BOLD	1U
#define X11_FACE_SANS		2U
#define X11_FACE_SANS_BOLD	3U
#define X11_FACES		4U

/* The largest pixel size a core font is drawn at, and the largest glyph drawn from a face (pixels each way). */
#define X11_FONT_PIXELS_MAX	128U
#define X11_GLYPH_DRAWN_MAX	256U

/* How many glyphs one font keeps drawn, and the longest name a font answers to. */
#define X11_FONT_CACHE		256U
#define X11_FONT_NAME_MAX	255U

/* The kinds of a core font: one drawn from a face, and the cursor font (the shapes of the standard cursors). */
#define X11_FONT_TEXT		0U
#define X11_FONT_CURSOR		1U

/* The types other modules keep behind pointers: a face, and Vulkan's state and a window's swapchain. */
struct truetype_face;
struct x11_cursor;

/*
 * An active grab of the pointer or of the keyboard: the client that holds
 * it (X11_NO_CLIENT: none) and its window, the pointer events it asked
 * for, the window the pointer is confined to and the cursor (pointer
 * grabs), whether the client's own windows hear their events as usual,
 * the modes each device runs in while it lasts (0 frozen until
 * AllowEvents, 1 as usual), and when it started.  A grab a button press
 * or a key press started (passive) ends with that button's or key's
 * release; an implicit grab is the one every button press starts for the
 * client it reached.
 */
struct x11_grab {
	unsigned client;
	uint32_t window;
	uint32_t mask;
	uint32_t confine_to;
	uint32_t cursor;
	uint32_t time;
	uint8_t owner_events;
	uint8_t pointer_mode;
	uint8_t keyboard_mode;
	uint8_t passive;
	uint8_t implicit;
	uint8_t key;
};

/*
 * A passive grab (GrabButton, GrabKey): the client and window, whether it
 * is a key's, the button or keycode (0: any) and the modifiers
 * (0x8000: any) that start it, and the active grab it starts.
 */
struct x11_passive_grab {
	unsigned client;
	uint32_t window;
	uint8_t keyboard;
	uint8_t detail;
	uint16_t modifiers;
	uint32_t mask;
	uint32_t confine_to;
	uint32_t cursor;
	uint8_t owner_events;
	uint8_t pointer_mode;
	uint8_t keyboard_mode;
};

/*
 * A device event as the backend reported it, held until its device is
 * not frozen: its kind, the key or physical button and whether it went
 * down, when, and where the pointer was.
 */
struct x11_device_event {
	unsigned kind;
	uint8_t detail;
	uint8_t pressed;
	uint32_t time;
	int x;
	int y;
};

/*
 * A keycode lent to a character the backend typed (an input method's
 * text, which no key of the keyboard's map carries): the keycode, the
 * keysym its first two columns carry, and when it was last used (a count
 * the server keeps, so the least recently used one is lent again first).
 * It stays lent while the map keeps that keysym there; a client's
 * ChangeKeyboardMapping or a backend's map that fills the keycode ends it.
 */
struct x11_text_key {
	uint8_t keycode;
	uint32_t keysym;
	uint32_t used;
};
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

	/* The bytes of a request too long to hold, refused, still to be dropped as they come. */
	size_t discard;

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

	/* The cursor named (NULL: None, the parent's is shown), held while the window names it. */
	struct x11_cursor *cursor_held;

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

	/*
	 * How its background and border are drawn: the background's kind and
	 * the copies of the background and border pixmaps (NULL: none, the
	 * pixels are used), with the bytes the owner is charged for them.
	 */
	uint8_t background_kind;
	struct x11_image *background_tile;
	struct x11_image *border_tile;
	size_t tiles_bytes;

	/*
	 * A viewable top-level InputOutput window's pixels (draw.c): its own
	 * and its inferiors', which its surface shows (none otherwise, and
	 * none for the root window); the rows they have room for, and the
	 * bytes the owner is charged for them.
	 */
	struct x11_image image;
	int image_rows;
	size_t image_bytes;

	/* Its properties. */
	struct x11_property *properties;
	unsigned property_count;
	unsigned property_capacity;

	/* WM_NAME, kept as a string for the surface's title. */
	char name[64];

	/*
	 * The window manager's state of a top-level window (wm.c): ICCCM's
	 * state (WM_STATE: X11_WM_WITHDRAWN, NORMAL or ICONIC), the state its
	 * surface is to have (X11_SURFACE_ICONIC, MAXIMIZED, FULLSCREEN, with
	 * wm.c's own bits of a half maximized one), and what of the window
	 * the surface is yet to be told (X11_SURFACE_CHANGED_*).  An Iconic
	 * window is unmapped and keeps its surface, minimized.
	 */
	uint8_t wm_state;
	unsigned wm_flags;
	unsigned wm_changed;

	/*
	 * A mapped top-level window's surface, the swapchain its pixels are
	 * shown through (NULL: the backend's present shows them), the size,
	 * place and title the surface was last given, and whether it must be
	 * drawn whole next time.
	 */
	struct x11_surface *surface;
	struct x11_vulkan_window *swapchain;
	uint16_t surface_width;
	uint16_t surface_height;
	int32_t surface_x;
	int32_t surface_y;
	int surface_fresh;
	char surface_title[X11_TITLE_MAX];
};

/*
 * One selection (selection.c): its atom, the owner window (0 for none; the
 * root window while the server answers for it), the owner's client slot
 * (X11_NO_CLIENT for the server), when it last changed, and whether it
 * has changed at all.
 */
struct x11_selection {
	uint32_t atom;
	uint32_t window;
	unsigned owner;
	uint32_t time;
	int changed;
};

/*
 * The clipboard's bridge of one of X's CLIPBOARD and PRIMARY (selection.c;
 * its index is the backend's X11_SELECTION_* number): the selection's
 * atom and the root window's property an X owner's text is asked into;
 * the target being asked for (0: nothing is; UTF8_STRING, then STRING) of
 * which owner window and client, whether the text comes in pieces (INCR),
 * whether the owner was told to start them and since when it waits to
 * be, and what came so far; and the copy of the last X owner's text (NULL for
 * none), which the root window answers with once that owner is gone.
 */
struct x11_bridge {
	uint32_t atom;
	uint32_t property;
	uint32_t asking;
	uint32_t window;
	unsigned owner;
	int incr;
	int incr_started;
	uint32_t incr_since;
	uint8_t *data;
	size_t length;
	uint8_t *kept;
	size_t kept_length;
};

/*
 * A large text given to a requestor in pieces (selection.c, INCR): the
 * requestor's window and property, the type, the requestor's client, the
 * text (NULL: the slot is free) and how much is given, and whether the
 * empty last piece is.
 */
struct x11_transfer {
	uint32_t window;
	uint32_t property;
	uint32_t type;
	unsigned client;
	uint8_t *data;
	size_t length;
	size_t offset;
	int done;
};

/*
 * One graphics context (gc.c): the values drawing uses, and the depth of
 * the drawables it may draw on.  The tile, the stipple and the clip mask
 * are copied when they are set (the protocol leaves later changes to the
 * pixmaps undefined), so a pixmap may be freed while a context uses it;
 * the clip is a region on the clip origin (clip_set 0: none).  The dashes
 * are one length (dash_one, as ChangeGC gives them) or the list SetDashes
 * gave (dash_list, malloc'd), dash_count lengths in all.
 */
struct x11_gc {
	uint32_t id;
	unsigned owner;
	unsigned depth;
	unsigned function;
	uint32_t plane_mask;
	uint32_t foreground;
	uint32_t background;
	uint16_t line_width;
	uint8_t line_style;
	uint8_t cap_style;
	uint8_t join_style;
	uint8_t fill_style;
	uint8_t fill_rule;
	uint8_t arc_mode;
	uint8_t subwindow_mode;
	uint8_t graphics_exposures;
	struct x11_image *tile;
	struct x11_image *stipple;
	int16_t tile_x;
	int16_t tile_y;
	uint32_t font;
	int16_t clip_x;
	int16_t clip_y;
	int clip_set;
	struct x11_region clip;
	uint16_t dash_offset;
	uint8_t dash_one;
	uint8_t *dash_list;
	size_t dash_count;
	size_t charged;
};

/*
 * The metrics of one character of a core font, as QueryFont answers them:
 * the ink's left and right edges from the origin, how far the origin
 * moves, and the ink above and below the baseline.  All zero: the font has
 * no such character.
 */
struct x11_char_metrics {
	int16_t left;
	int16_t right;
	int16_t width;
	int16_t ascent;
	int16_t descent;
};

/*
 * One glyph a font keeps drawn: its character (used 0: the slot is free),
 * where its bits sit from the origin on the baseline (left, and the rows
 * above it), its size, and the bits (one a pixel, most significant first,
 * rows of stride bytes).
 */
struct x11_font_glyph {
	uint32_t codepoint;
	int used;
	int left;
	int top;
	int width;
	int height;
	size_t stride;
	uint8_t *bits;
};

/*
 * One open core font (font.c): a size of a face, or the cursor font.
 *
 * A text font has its name (an XLFD, or the name it was opened by when it
 * has none), the face's role, the pixel size the face is drawn at, how
 * far a glyph is moved right and raised from the origin, the font's
 * ascent and descent, and its characters: the range of the first and
 * second bytes, the default character, and either one width every
 * character shares (monospaced: cell_width, metrics NULL) or each one's
 * metrics, with the bounds of them all.  A monospaced font draws the
 * line-drawing characters itself, across the whole cell (line_drawing),
 * and the tree's zed-unicode keeps the background under ImageText (the
 * tree's libX11 sends its XDrawString as ImageText).  The glyphs drawn so far are kept, the bytes the owner is
 * charged with the worst they could take.  The server's own default font
 * has the owner X11_NO_CLIENT and no id.
 */
struct x11_font {
	uint32_t id;
	unsigned owner;
	unsigned kind;
	char name[X11_FONT_NAME_MAX + 1U];
	unsigned role;
	unsigned pixels;
	int shift_x;
	int raise;
	int ascent;
	int descent;
	uint8_t min_byte1;
	uint8_t max_byte1;
	uint8_t min_byte2;
	uint8_t max_byte2;
	uint16_t default_char;
	int cell_width;
	int box_width;
	int line_drawing;
	int keeps_background;
	struct x11_char_metrics min_bounds;
	struct x11_char_metrics max_bounds;
	struct x11_char_metrics *metrics;
	size_t metric_count;
	struct x11_font_glyph cache[X11_FONT_CACHE];
	size_t charged;
};

/*
 * One cursor (cursor.c): its shape, a mask of the pixels it shows and its
 * two colours, its hot spot, and, when it is one of the cursor font's,
 * which (shape -1: none).  It lives while its id does or a window or a
 * grab holds it (holders).
 */
struct x11_cursor {
	uint32_t id;
	unsigned owner;
	unsigned holders;
	int shape;
	int width;
	int height;
	int hot_x;
	int hot_y;
	uint8_t *source;
	uint8_t *mask;
	size_t stride;
	uint16_t foreground[3];
	uint16_t background[3];
	uint32_t version;
	size_t charged;
};

/*
 * One colormap a client made.  The screen's one visual is TrueColor, so a
 * colormap holds no cells: every one gives each colour the pixel of its
 * own bits, and only its id and its owner are kept.
 */
struct x11_colormap {
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
	struct x11_image image;
};

/*
 * One window's state before a change to its tree (draw.c): its place on
 * the top-level window's pixels and its size, and the parts of it that
 * could be seen (inside its border, without its children, and its
 * border).
 */
struct x11_draw_record {
	struct x11_window *window;
	int x;
	int y;
	uint16_t width;
	uint16_t height;
	uint16_t border;
	struct x11_region inner;
	struct x11_region outline;
};

/*
 * A change to a top-level window's tree under way (draw.c): the top-level
 * window, the window that moves or changes size (NULL: none), how many
 * changes nested inside one another it holds, where the top-level was,
 * whether it had pixels and whether they were made anew at the end (what
 * they showed then gone), each viewable window as it was, and the pixels
 * of the changing window as they were (with where they were taken).
 */
struct x11_draw_frame {
	struct x11_window *top;
	struct x11_window *changing;
	unsigned depth;
	int top_x;
	int top_y;
	int top_had_image;
	int fresh;
	struct x11_draw_record *records;
	size_t count;
	struct x11_image saved;
	int saved_x;
	int saved_y;
};

/*
 * The faces the core fonts are drawn from (glyphs.c): one over each font
 * file the caller keeps in memory (NULL: none of that role), the size and
 * weight each was last set to, and the coverage a glyph is drawn into
 * before it becomes bits (NULL when there is no face).
 */
struct x11_faces {
	struct truetype_face *faces[X11_FACES];
	unsigned sizes[X11_FACES];
	int bolds[X11_FACES];
	uint8_t *coverage;
};

/*
 * A glyph as a face draws it: its coverage (rows of stride bytes, 0 to
 * 255), where it sits from the origin (left, and the rows above the
 * baseline), its size and how far the origin moves.
 */
struct x11_face_glyph {
	const uint8_t *coverage;
	size_t stride;
	int left;
	int top;
	int width;
	int height;
	int advance;
};

/*
 * One glyph of a text request (font.c to draw.c): where its bits go on the
 * drawable (its top-left corner), their size, and where they start in the
 * request's bits (one a pixel, most significant first, rows of stride
 * bytes).
 */
struct x11_text_glyph {
	int x;
	int y;
	int width;
	int height;
	size_t stride;
	size_t offset;
};

/*
 * The text a request draws (draw.c): the drawable and graphics context,
 * whether the glyphs go on a box filled with the background first
 * (ImageText) and that box, the box that covers all that is drawn (both
 * on the drawable), and the glyphs and their bits.
 */
struct x11_text {
	uint32_t drawable;
	uint32_t gc;
	int image;
	struct x11_box background;
	struct x11_box extent;
	const struct x11_text_glyph *glyphs;
	size_t count;
	const uint8_t *bits;
};

/* ICCCM's states of a top-level window (WM_STATE's first field). */
#define X11_WM_WITHDRAWN	0U
#define X11_WM_NORMAL		1U
#define X11_WM_ICONIC		3U

/*
 * The atoms the window manager (wm.c) speaks of, interned when the server
 * starts: ICCCM's protocols and state, and EWMH's hints, the states and
 * the kinds of window that are shown as menus.
 */
struct x11_wm_atoms {
	uint32_t protocols;
	uint32_t delete_window;
	uint32_t take_focus;
	uint32_t state;
	uint32_t change_state;
	uint32_t net_supported;
	uint32_t net_supporting_wm_check;
	uint32_t net_wm_name;
	uint32_t net_wm_state;
	uint32_t net_wm_state_fullscreen;
	uint32_t net_wm_state_maximized_vert;
	uint32_t net_wm_state_maximized_horz;
	uint32_t net_wm_state_hidden;
	uint32_t net_wm_window_type;
	uint32_t net_wm_window_type_menu;
	uint32_t net_wm_window_type_dropdown_menu;
	uint32_t net_wm_window_type_popup_menu;
	uint32_t net_wm_window_type_tooltip;
	uint32_t net_wm_window_type_combo;
	uint32_t net_wm_window_type_dnd;
	uint32_t net_wm_window_type_notification;
	uint32_t net_active_window;
	uint32_t net_close_window;
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

	/*
	 * The faces the core fonts are drawn from, the font a graphics context
	 * without one of its own draws with (NULL until first needed), and the
	 * names ListFonts answers (NUL after each; NULL until first asked).
	 */
	struct x11_faces faces;
	struct x11_font *default_font;
	char *font_names;
	size_t font_names_size;

	/*
	 * The cursor shown (cursor.c): the top-level window and the surface it
	 * was last given to (the pair, so that a surface made anew at the same
	 * place is not taken for the old one), the version of the cursor (0:
	 * the system's own), and whether any was given.
	 */
	uint32_t cursor_window;
	const struct x11_surface *cursor_surface;
	uint32_t cursor_shown_version;
	int cursor_known;

	/* The version the last cursor made or recoloured was given (cursor.c), never 0. */
	uint32_t cursor_serial;

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

	/* The changes to window trees under way (draw.c), innermost last. */
	struct x11_draw_frame frames[X11_DRAW_FRAMES];
	unsigned frame_count;

	/* What has changed since it was last shown: one box on the root window, half-open. */
	int dirty;
	int dirty_x0;
	int dirty_y0;
	int dirty_x1;
	int dirty_y1;

	/*
	 * The pointer (input.c): its place on the root window, the window it
	 * was last found in (kept for the crossing events; the parent takes
	 * the place of a window destroyed), whether it is outside every X
	 * window (on the system's own windows), the logical buttons held (bit
	 * n - 1 for button n), the physical buttons held and the logical
	 * button each press was taken as, and the buttons' map.
	 */
	int pointer_x;
	int pointer_y;
	uint32_t pointer_window;
	int pointer_outside;
	uint16_t buttons;
	uint16_t physical_buttons;
	uint8_t pressed_as[X11_BUTTONS];
	uint8_t button_map[X11_BUTTONS];

	/* The pointer's control: the acceleration and its threshold (kept for the answer; the system moves the pointer). */
	int16_t acceleration_numerator;
	int16_t acceleration_denominator;
	int16_t acceleration_threshold;

	/*
	 * The keyboard (input.c): the keysyms of each keycode and how many a
	 * keycode has, the modifiers' keycodes and how many a modifier has,
	 * the keys down (bit k of the array for keycode k), and the modifiers
	 * a locking key (Caps Lock, Num Lock) holds on.
	 */
	uint32_t keysyms[X11_KEYMAP_KEYCODES][X11_KEYSYMS_MAX];
	unsigned keysyms_per_keycode;
	uint8_t modifier_keys[X11_MODIFIERS][X11_MODIFIER_KEYS_MAX];
	unsigned keys_per_modifier;
	uint8_t keys_down[X11_KEY_BYTES];
	uint16_t locks;

	/*
	 * The keycodes lent to the characters the backend typed (input.c):
	 * how many are lent, and the count their use is stamped with.
	 */
	struct x11_text_key text_keys[X11_TEXT_KEYS];
	unsigned text_key_count;
	uint32_t text_stamp;

	/* The keyboard's control, kept for GetKeyboardControl: the click and the bell, the LEDs and auto-repeat. */
	int8_t key_click_percent;
	int8_t bell_percent;
	int16_t bell_pitch;
	int16_t bell_duration;
	uint32_t leds;
	uint8_t global_auto_repeat;
	uint8_t auto_repeats[X11_KEY_BYTES];

	/*
	 * The window with the keyboard's focus (or X11_FOCUS_NONE,
	 * X11_FOCUS_POINTER_ROOT), where it goes when that window is no
	 * longer viewable (None, PointerRoot or Parent), and when it last
	 * changed.
	 */
	uint32_t focus;
	uint8_t focus_revert;
	uint32_t focus_time;

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
	 * Selections (selection.c): the selections, the clipboard's bridges,
	 * the texts being given in pieces, and the atoms the server uses
	 * itself.
	 */
	struct x11_selection selections[X11_MAX_SELECTIONS];
	unsigned selection_count;
	struct x11_bridge bridges[X11_BRIDGES];
	struct x11_transfer transfers[X11_TRANSFERS];
	uint32_t atom_clipboard;
	uint32_t atom_utf8;
	uint32_t atom_targets;
	uint32_t atom_text;
	uint32_t atom_incr;

	/*
	 * The grabs (input.c): the pointer's and the keyboard's active grabs
	 * and when each device was last grabbed, and the passive grabs.
	 */
	struct x11_grab pointer_grab;
	struct x11_grab keyboard_grab;
	uint32_t pointer_grab_time;
	uint32_t keyboard_grab_time;
	struct x11_passive_grab *passive_grabs;
	unsigned passive_count;
	unsigned passive_capacity;

	/*
	 * The devices' freezes (X11_FREEZE_*: which grab froze each), whether
	 * AllowEvents let a device go until its next event reaches the
	 * grabbing client (Sync modes; both: either device's event freezes
	 * both again), the event each device froze on (for the Replay modes)
	 * and the window of the grab it started, and the device events held
	 * meanwhile (a ring; draining says they are being handled, so that a
	 * device event that arrives then joins the ring).
	 */
	unsigned pointer_freeze;
	unsigned keyboard_freeze;
	int pointer_sync;
	int keyboard_sync;
	int both_sync;
	struct x11_device_event pointer_replay;
	struct x11_device_event keyboard_replay;
	int pointer_replay_valid;
	int keyboard_replay_valid;
	struct x11_device_event device_queue[X11_DEVICE_QUEUE];
	unsigned device_head;
	unsigned device_count;
	int draining;

	/* A pointer motion held back so that only the last of a burst is sent: when and where. */
	int motion_pending;
	uint32_t motion_time;
	int motion_x;
	int motion_y;

	/*
	 * The window manager (wm.c): its atoms, and the top-level window the
	 * root window's _NET_ACTIVE_WINDOW names (0: none), kept so that it
	 * is written only when it changes.
	 */
	struct x11_wm_atoms wm;
	uint32_t wm_active;
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
void x11_request_refuse(struct x11server *server, unsigned index, uint8_t opcode, uint8_t minor, unsigned code);
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
int x11_window_server_child(struct x11server *server, uint32_t id);
void x11_window_map(struct x11server *server, struct x11_window *window);
void x11_window_unmap(struct x11server *server, struct x11_window *window);
int x11_window_viewable(const struct x11_window *window);
struct x11_window *x11_window_next(struct x11_window *window, const struct x11_window *top);
int x11_window_inferior(const struct x11_window *window, const struct x11_window *ancestor);
struct x11_window *x11_window_top_level(struct x11_window *window);
struct x11_window *x11_window_at(struct x11server *server, int x, int y);
struct x11_window *x11_window_child_at(struct x11_window *parent, int x, int y);
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
void x11_event_expose(struct x11server *server, const struct x11_window *window, const struct x11_region *region);
void x11_event_property(struct x11server *server, struct x11_window *window, uint32_t atom, int deleted);
void x11_event_device(uint8_t *event, struct x11server *server, uint8_t type, uint8_t detail, uint32_t time, const struct x11_window *window, uint32_t child, uint16_t state);
struct x11_window *x11_event_propagate(struct x11server *server, struct x11_window *source, const struct x11_window *stop, uint32_t mask, uint8_t type, uint8_t detail, uint32_t time, uint16_t state, unsigned only);
uint32_t x11_event_child_toward(const struct x11_window *window, const struct x11_window *inferior);
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
void x11_selection_destroy(struct x11server *server);
int x11_selection_tick(struct x11server *server);

/* draw.c: the top-level windows' pixels, clips, exposures, and the drawing requests. */
void x11_draw_begin(struct x11server *server, struct x11_window *window, struct x11_window *changing);
void x11_draw_end(struct x11server *server);
void x11_draw_forget(struct x11server *server, struct x11_window *window);
void x11_draw_border(struct x11server *server, struct x11_window *window);
unsigned x11_draw_set_tile(struct x11server *server, unsigned owner, uint32_t pixmap_id, struct x11_image **tile, size_t *charged);
int x11_image_copy(const struct x11_image *source, struct x11_image **copy);
void x11_image_free(struct x11_image *image);
size_t x11_image_bytes(int width, int height);
unsigned x11_request_clear_area(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_copy_area(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_copy_plane(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_poly_point(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_poly_line(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_poly_segment(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_poly_rectangle(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_poly_arc(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_fill_poly(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_poly_fill_arc(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_poly_fill_rectangle(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_put_image(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_get_image(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_draw_text(struct x11server *server, const struct x11_text *text);
unsigned x11_request_put_image_rgb24(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_create_pixmap(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_free_pixmap(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
void x11_pixmap_free(struct x11server *server, struct x11_pixmap *pixmap);

/* gc.c: graphics contexts. */
unsigned x11_request_create_gc(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_change_gc(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_free_gc(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_copy_gc(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_set_dashes(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
unsigned x11_request_set_clip_rectangles(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
const uint8_t *x11_gc_dashes(const struct x11_gc *gc, size_t *count);
void x11_gc_free(struct x11server *server, struct x11_gc *gc);

/* colormap.c: colormaps and colours. */
unsigned x11_colormap_request(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
void x11_colormap_free(struct x11server *server, struct x11_colormap *colormap);
int x11_color_lookup(const char *name, size_t length, uint16_t *red, uint16_t *green, uint16_t *blue);

/* rootless.c: the top-level windows as the backend's surfaces, and the backend's input. */
void x11_rootless_present(struct x11server *server);
void x11_rootless_closing(struct x11server *server);
void x11_rootless_flush_motion(struct x11server *server);
void x11_rootless_forget(struct x11server *server, struct x11_window *window);

/* present-vulkan.c: a surface shown through a Vulkan swapchain. */
struct x11_vulkan *x11_vulkan_open(const char *wsi);
void x11_vulkan_close(struct x11_vulkan *vulkan);
struct x11_vulkan_window *x11_vulkan_window_open(struct x11server *server, struct x11_surface *surface, unsigned width, unsigned height);
int x11_vulkan_window_present(struct x11_vulkan_window *window, const uint32_t *pixels, size_t stride);
int x11_vulkan_window_resize(struct x11_vulkan_window *window, unsigned width, unsigned height);
void x11_vulkan_window_close(struct x11_vulkan_window *window);

/* input.c: the keyboard, the pointer, the focus and the grabs. */
void x11_input_init(struct x11server *server);
void x11_input_fini(struct x11server *server);
void x11_input_tick(struct x11server *server);
void x11_input_key(struct x11server *server, uint8_t keycode, int pressed, uint32_t time);
void x11_input_button(struct x11server *server, uint8_t button, int pressed, uint32_t time, int x, int y);
void x11_input_motion(struct x11server *server, uint32_t time, int x, int y);
void x11_input_leave(struct x11server *server, uint32_t time);
void x11_input_keymap(struct x11server *server, const struct x11_keymap *keymap);
void x11_input_text(struct x11server *server, uint32_t character, uint32_t time);
void x11_input_focus_top_level(struct x11server *server, struct x11_window *window, uint32_t time);
void x11_input_focus_lost(struct x11server *server, struct x11_window *window, uint32_t time);
struct x11_window *x11_input_window_under(struct x11server *server);
uint16_t x11_input_state(const struct x11server *server);
void x11_input_window_hidden(struct x11server *server, struct x11_window *window);
void x11_input_window_destroyed(struct x11server *server, struct x11_window *window);
void x11_input_forget_client(struct x11server *server, unsigned client);
unsigned x11_input_request(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
void x11_input_capture_lost(struct x11server *server, uint32_t time);

/* wm.c: the top-level windows' window manager (ICCCM and EWMH). */
int x11_wm_init(struct x11server *server);
void x11_wm_fini(struct x11server *server);
int x11_wm_map(struct x11server *server, struct x11_window *window);
void x11_wm_unmapped(struct x11server *server, struct x11_window *window);
void x11_wm_close(struct x11server *server, struct x11_window *window);
void x11_wm_backend_state(struct x11server *server, struct x11_window *window, unsigned state);
void x11_wm_root_event(struct x11server *server, const uint8_t *event);
void x11_wm_property(struct x11server *server, struct x11_window *window, uint32_t atom);
void x11_wm_focus_changed(struct x11server *server);
int x11_wm_protocol(struct x11server *server, struct x11_window *window, uint32_t protocol);
void x11_wm_take_focus(struct x11server *server, struct x11_window *window, uint32_t time);
int x11_wm_keeps_surface(const struct x11_window *window);
void x11_wm_info(struct x11server *server, struct x11_window *window, struct x11_surface_info *info);
void x11_wm_title(struct x11server *server, struct x11_window *window, char *title, size_t size);
void x11_wm_constrain(struct x11server *server, struct x11_window *window, int *width, int *height);

/* glyphs.c: the faces the core fonts are drawn from. */
int x11_faces_open(struct x11_faces *faces, const struct x11server_font_file *files, unsigned count, const char *fallback_path);
void x11_faces_close(struct x11_faces *faces);
int x11_face_present(const struct x11_faces *faces, unsigned role);
int x11_face_line(struct x11_faces *faces, unsigned role, unsigned pixels, int *ascent, int *descent, int *advance);
int x11_face_char(struct x11_faces *faces, unsigned role, unsigned pixels, uint32_t codepoint, struct x11_char_metrics *metrics);
int x11_face_draw(struct x11_faces *faces, unsigned role, unsigned pixels, uint32_t codepoint, struct x11_face_glyph *glyph);

/* font.c: the core fonts and the requests of fonts and text. */
void x11_fonts_fini(struct x11server *server);
struct x11_font *x11_font_find(struct x11server *server, uint32_t id);
void x11_font_free(struct x11server *server, struct x11_font *font);
int x11_font_has_char(const struct x11_font *font, uint32_t character);
const struct x11_font_glyph *x11_font_glyph(struct x11server *server, struct x11_font *font, uint32_t character);
unsigned x11_font_request(struct x11server *server, unsigned index, const uint8_t *request, size_t length);

/* cursor.c: cursors, and the one the backend shows. */
struct x11_cursor *x11_cursor_find(struct x11server *server, uint32_t id);
void x11_cursor_hold(struct x11_cursor *cursor);
void x11_cursor_release(struct x11server *server, struct x11_cursor *cursor);
void x11_cursor_free(struct x11server *server, struct x11_cursor *cursor);
void x11_cursor_update(struct x11server *server);
unsigned x11_cursor_request(struct x11server *server, unsigned index, const uint8_t *request, size_t length);

/*
 * glx.c: the GLX extension.  A program whose server does not name GLX links
 * an implementation of these that answers GLX absent instead.
 */
void x11_glx_query_extension(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
size_t x11_glx_list_name(uint8_t *list, size_t room);
unsigned x11_glx_request(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);

/* The GLX extension's major opcode. */
#define X11_GLX_MAJOR		144U

/*
 * GLX's first event and first error.  A client's Xlib names the 17 events
 * and the 14 errors from them as GLX's (glxproto.h's __GLX_NUMBER_EVENTS and
 * __GLX_NUMBER_ERRORS), so they follow those of the other extensions and
 * overlap none of them.
 */
#define X11_GLX_FIRST_EVENT	89U
#define X11_GLX_FIRST_ERROR	149U

#endif
