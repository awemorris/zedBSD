/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DOM binding: what a page's scripts see
 * of the page.  The realm's global object becomes the window, with the
 * document, the console, the timers and queueMicrotask; each DOM node a
 * script reaches gets one object whose prototype chain follows the DOM's
 * interfaces (EventTarget, Node, Element, HTMLElement, Document, Text...);
 * and events are dispatched along the node tree with the capture, target
 * and bubble phases.
 *
 * The page drives time: it tells the window what time it is and asks it
 * to run the timers that are due, and a microtask checkpoint follows every
 * script, callback and event.
 */

#ifndef KEILAND_BROWSER_BIND_H
#define KEILAND_BROWSER_BIND_H

#include "dom/dom.h"
#include "js/js.h"

/*
 * The flags of bind_fire_event: the event bubbles; it can be canceled; its
 * target is the document although it is dispatched at the window (the
 * window's load event).
 */
#define BIND_EVENT_BUBBLES 0x1U
#define BIND_EVENT_CANCELABLE 0x2U
#define BIND_EVENT_DOCUMENT 0x4U

/*
 * The modifier keys an input event says were held (shiftKey, ctrlKey,
 * altKey and metaKey of MouseEvent and KeyboardEvent).
 */
#define BIND_MOD_SHIFT 0x01U
#define BIND_MOD_CTRL 0x02U
#define BIND_MOD_ALT 0x04U
#define BIND_MOD_META 0x08U

/*
 * The levels of the console's methods, which the host may show apart.
 */
enum bind_console_level {
	BIND_CONSOLE_LOG,
	BIND_CONSOLE_INFO,
	BIND_CONSOLE_WARN,
	BIND_CONSOLE_ERROR,
	BIND_CONSOLE_DEBUG
};

/*
 * The two storage areas of a window: sessionStorage's and
 * localStorage's.
 */
enum bind_storage_area {
	BIND_STORAGE_SESSION,
	BIND_STORAGE_LOCAL
};

/*
 * The parts of the document's location the host's location callback
 * writes (the parts of the URL interface).
 */
enum bind_location_part_index {
	BIND_LOCATION_HREF,
	BIND_LOCATION_ORIGIN,
	BIND_LOCATION_PROTOCOL,
	BIND_LOCATION_HOST,
	BIND_LOCATION_HOSTNAME,
	BIND_LOCATION_PORT,
	BIND_LOCATION_PATHNAME,
	BIND_LOCATION_SEARCH,
	BIND_LOCATION_HASH
};

struct css_engine;
struct css_style;

/*
 * Where a node is on the laid out page, for the geometry scripts ask for
 * (getBoundingClientRect, clientWidth and the like), in CSS
 * pixels from the document's top left: the union of the node's boxes,
 * whether its first box is a block (an inline element has no client area
 * of its own), and that box's border widths, together with that
 * box's used margins and paddings (getComputedStyle's resolved values).
 */
struct bind_box {
	double x;
	double y;
	double width;
	double height;
	int block;
	double border_top;
	double border_right;
	double border_bottom;
	double border_left;
	double margin_top;
	double margin_right;
	double margin_bottom;
	double margin_left;
	double padding_top;
	double padding_right;
	double padding_bottom;
	double padding_left;
	/* Successful layout proved this node generates no box; zero also covers unavailable geometry. */
	int no_generated_box;
	/* A box out of the flow's used insets (top, right, bottom, left) from its containing block, when inset_known. */
	int inset_known;
	double inset[4];
	/* What a block scrolls over: its padding box and the overflow of its content (0 when not known). */
	double scroll_width;
	double scroll_height;
};

/*
 * What the window asks of its host for Web Storage: the
 * items of a storage area of the page's origin, keys and values in UTF-16.
 * key reports the index-th key in the host's order (found is 0 past the
 * last); get reports a key's value (found is 0 for a key not there); set
 * reports ENOSPC when the item would take the origin past its quota; each
 * reports ENOMEM when memory runs out. The Window borrows this immutable
 * callback table for its lifetime; returned units are appended to caller storage.
 */
struct bind_storage_calls {
	int (*length)(void *context, int area, size_t *count);
	int (*key)(void *context, int area, size_t index, struct wb_units *key, int *found);
	int (*get)(void *context, int area, const uint16_t *key, size_t key_length, struct wb_units *value, int *found);
	int (*set)(void *context, int area, const uint16_t *key, size_t key_length, const uint16_t *value, size_t value_length);
	int (*remove)(void *context, int area, const uint16_t *key, size_t key_length);
	int (*clear)(void *context, int area);
};

/*
 * A media element's state as the host reports it (ws121-p005): whether it
 * has media, whether it is paused or ended, where it is and how long it is
 * (seconds), how ready it is (HTMLMediaElement's HAVE_* number), the
 * video's size, and whether it failed.
 */
struct bind_media {
	int has;
	int paused;
	int ended;
	double current_time;
	double duration;
	int ready_state;
	int width;
	int height;
	int error;
};

/* What a script asks of a media element's host (bind_host.media). */
#define BIND_MEDIA_STATE	0
#define BIND_MEDIA_PLAY		1
#define BIND_MEDIA_PAUSE	2
#define BIND_MEDIA_SEEK		3

/* An image element's state for scripts (bind_host.image): no source, loading, decoded at a natural size, or broken. */
#define BIND_IMAGE_NONE		0
#define BIND_IMAGE_LOADING	1
#define BIND_IMAGE_LOADED	2
#define BIND_IMAGE_BROKEN	3

struct bind_image {
	int state;
	int width;
	int height;
};

/*
 * A request a script makes (fetch and XMLHttpRequest): the
 * method, the URL (resolved by the host against the page), the header lines
 * (each "Name: value" ending in CR LF, or NULL) and the body (NULL for none).
 */
struct bind_request {
	const char *method;
	const char *href;
	const char *headers;
	const unsigned char *body;
	size_t body_length;
};

/*
 * Completion of a request: the error (0 when a response came), its status,
 * final URL, header lines ("Name: value" each ending in LF) and body.
 */
typedef void (*bind_request_done)(void *context, int error, int status, const char *url,
				  const char *headers, const unsigned char *bytes, size_t length);

/*
 * What the window asks of its host: where the console's lines go (one
 * line of UTF-8 text at a level, without its line feed), the User-Agent
 * navigator reports, the parts of the document's location (a
 * BIND_LOCATION_*, written as UTF-8), and document.cookie's reading and
 * writing (the cookies a script may see, and one cookie set in the form
 * of a Set-Cookie header).  It also gives the style engine
 * a script's selectors are matched with (querySelector and the like),
 * where a node is on the page laid out as it is now (node_box lays the
 * page out first when it changed, and reports whether the node has a box;
 * document_size gives the laid out document's width and height), and how
 * far the viewport is scrolled; the storage areas (NULL:
 * scripts get storage that holds nothing).  It computes an
 * element's style (computed_style: the style of its box when the page is
 * laid out and the element has one, otherwise the cascade's; 0, ENOENT
 * for an element not in the document, or ENOMEM), and takes a scroll a
 * script asks for (scroll_to, in CSS pixels; the host keeps it inside the
 * document, and scroll reports where it went).  A NULL callback reads as nothing and
 * writes nothing: no engine matches selectors, and missing layout supplies no geometry proof.
 * node_inserted is told after a script changes a DOM tree; checkpoint is
 * called after the outermost script task and its microtasks have finished.
 *
 * The Window copies this record. Its context, user_agent and optional storage
 * table remain borrowed from the host for the Window lifetime. These callbacks
 * are internal engine collaborators; the public view callback contract is in
 * browser.h and does not expose these private records.
 */
struct bind_host {
	void *context;
	void (*console)(void *context, int level, const char *text, size_t length);
	const char *user_agent;
	int (*location)(void *context, int part, struct wb_buffer *out);
	/* Changes this document's URL (resolved against it) without loading another document. */
	int (*url_set)(void *context, const char *href, size_t length);
	int (*cookie_get)(void *context, struct wb_buffer *out);
	int (*cookie_set)(void *context, const char *text, size_t length);
	struct css_engine *(*selector_engine)(void *context);
	/* May copy no_generated_box proof even when the ordinary bounds query returns zero. */
	int (*node_box)(void *context, struct dom_node *node, struct bind_box *box);
	/*
	 * Appends used CSS-pixel Grid tracks to a caller-owned double vector and
	 * reports how many implicit ones come before the explicit grid and how
	 * many times an automatic repeat was put in it; found=0 requests
	 * computed fallback.
	 */
	int (*grid_tracks)(void *context, struct dom_node *node, int columns, struct wb_vector *sizes, int *leading, int *repeats, int *found);
	void (*document_size)(void *context, double *width, double *height);
	void (*scroll)(void *context, double *x, double *y);
	const struct bind_storage_calls *storage;
	int (*computed_style)(void *context, struct dom_element *element, struct css_style *style);
	void (*scroll_to)(void *context, double x, double y);
	int (*node_inserted)(void *context, struct dom_node *node);
	int (*checkpoint)(void *context);
	/* Starts a request; done is called once, from here or later, and *handle names it for request_cancel. */
	int (*request)(void *context, const struct bind_request *request, bind_request_done done, void *done_context, void **handle);
	void (*request_cancel)(void *context, void *handle);
	int (*fetch_sync)(void *context, const char *href, struct wb_buffer *bytes, struct wb_buffer *final_url);
	struct dom_node *(*element_at)(void *context, double x, double y);
	/* Inserts into the active parser; ENOTSUP when no insertion point exists. */
	int (*document_write)(void *context, const uint16_t *units, size_t length);
	/* A media element's state, play, pause or seek to a time (BIND_MEDIA_*, ws121-p005); fills state. */
	int (*media)(void *context, struct dom_element *element, int request, double value, struct bind_media *state);
	/* An <img>'s state (BIND_IMAGE_*); restart loads its source again, a load or error event to follow. */
	int (*image)(void *context, struct dom_element *element, int restart, struct bind_image *state);
	/* A view transition's step: 1 captures the old state, 2 makes the new one and shows the tree, 0 ends it; returns whether it could be made. */
	int (*view_transition)(void *context, int step);
	/* The milliseconds until the shown transition's animations all end (a very large number when one never does). */
	double (*view_transition_duration)(void *context);
	/* The frame element of a child window, known from its creation (its scripts may read it at once). */
	struct dom_element *frame;
};

/*
 * A pointer event's place and button, for bind_fire_mouse_event and
 * bind_fire_wheel_event: the point in the viewport and in the document,
 * in CSS pixels, the DOM's button number (0 is the main button), the
 * modifiers held (BIND_MOD_*), and a wheel event's distances in pixels.
 */
struct bind_mouse {
	double client_x;
	double client_y;
	double page_x;
	double page_y;
	int button;
	unsigned modifiers;
	double delta_x;
	double delta_y;
};

/*
 * A key event's key, for bind_fire_key_event: the key's value and its
 * physical code as the DOM names them (UTF-8, "a" and "KeyA", "Enter"
 * and "Enter"), whether the key is held and repeating, and the modifiers
 * held (BIND_MOD_*).
 */
struct bind_key {
	const char *key;
	const char *code;
	int repeat;
	unsigned modifiers;
};

struct bind_window;

/* The window (window.c). */
int bind_window_create(struct vm_realm *realm, struct dom_document *document, const struct bind_host *host, struct bind_window **window);
int bind_document_fonts_settle(struct dom_document *document);
/* Explicit primary windows are destroyed here; managed windows belong to GC. */
void bind_window_destroy(struct bind_window *window);
/* Cancels detached context tasks while saved script references remain valid. */
void bind_window_detach(struct bind_window *window);
void bind_window_set_time(struct bind_window *window, double now);
void bind_window_set_viewport(struct bind_window *window, int width, int height);
void bind_window_set_ready_state(struct bind_window *window, const char *state);
void bind_window_set_load_ended(struct bind_window *window);
void bind_window_set_unloading(struct bind_window *window);
void bind_window_set_current_script(struct bind_window *window, struct dom_node *script);
/* Borrows active child cascade/viewport until its next DOM mutation; caller roots its Document. */
int bind_window_child_styles(struct bind_window *window, struct css_engine **engine, int *width, int *height);
/* Appends actual inline source for primary or managed-child rendering without DOM mutation. */
int bind_style_sheet_source(struct dom_element *element, struct wb_units *units);
int bind_run_script(struct bind_window *window, const uint16_t *source, size_t length, const char *name);
int bind_run_script_source(struct bind_window *window, const uint16_t *source, size_t length, const char *name, int known_inline);
int bind_checkpoint(struct bind_window *window);
void bind_report_exception(struct bind_window *window, vm_value exception);
void bind_report_exception_at(struct bind_window *window, vm_value exception, unsigned line, unsigned column);
void bind_console(struct bind_window *window, int level, const char *text);

/* The timers (timer.c). */
int bind_next_timer(const struct bind_window *window, double *due);
int bind_run_timers(struct bind_window *window);

/* Events (event.c). */
int bind_fire_event(struct bind_window *window, struct dom_node *target, const char *type, unsigned flags, int *canceled);
int bind_fire_mouse_event(struct bind_window *window, struct dom_node *target, const char *type, const struct bind_mouse *mouse, int *canceled);

/* The events of the keyboard, the wheel and the focus (input.c). */
int bind_fire_key_event(struct bind_window *window, struct dom_node *target, const char *type, const struct bind_key *key, int *canceled);
int bind_fire_wheel_event(struct bind_window *window, struct dom_node *target, const struct bind_mouse *mouse, int *canceled);
int bind_fire_focus_event(struct bind_window *window, struct dom_node *target, const char *type, int bubbles);

#endif
