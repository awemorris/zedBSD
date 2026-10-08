/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A page: one document loaded into a tab, with its heap, its DOM, its
 * style sheets and, as the engine grows, its layout and display list.
 *
 * The headless modes and the window both load pages through here, so a
 * page looks the same whether it is drawn on the host for a test or in a
 * the compositor window.
 */

#ifndef KEILAND_BROWSER_PAGE_H
#define KEILAND_BROWSER_PAGE_H

#include "bind/bind.h"
#include "css/css.h"
#include "dom/dom.h"
#include "html/html.h"
#include "image/image.h"
#include "layout/layout.h"
#include "paint/paint.h"
#include "text/text.h"

/*
 * Where a page's console lines go: a function of the embedder's with its
 * context (NULL writes them to standard error).
 */
typedef void (*page_console)(void *context, int level, const char *text, size_t length);

/* The network's loader and its requests (net/net.h), which the embedder runs through page_net_*. */
struct net_loader;
struct net_request;
struct net_response;
struct pollfd;
struct page_box_slot;
struct net_url;

/* A request's callback, as the loader calls it (net_request_done). */
typedef void (*page_request_done)(void *context, struct net_request *request);

/*
 * A parser-blocking script fetched before its document was parsed
 * (BUG-207, script.c): its location (resolved against the document's
 * URL), how the fetch ended (0, or an errno value) and the bytes it
 * brought.  The page owns it from page_add_prefetched to page_destroy.
 */
struct page_prefetched {
	char *location;
	int error;
	struct wb_buffer bytes;
};

/*
 * A loaded page.
 *
 * The page owns its heap; the document is a root of it for as long as the
 * page lives.  Its realm's global object is the document's window, which
 * runs the page's scripts.  base is the page's location: the absolute path
 * of its file, or its URL (what relative URLs resolve against); now the
 * page's clock in milliseconds, and the two
 * generations the document's generation when the style sheets were
 * gathered and when the page was last laid out.  images is the table of
 * the page's decoded images (page/images.c), by location; loader (the
 * embedder's, or NULL) fetches http and https images without blocking,
 * and images_generation counts the images that arrived, which
 * laid_out_images is compared with to lay the page out again.
 *
 * The style sheets (sheets.c): sheets is the table of the page's parsed
 * sheets (<style> texts and fetched sheets, by location);
 * sheets_generation counts the sheets that arrived, styled_sheets is its
 * value when the page was last styled, and styling numbers the stylings.
 *
 * The focus (input.c): focused is the element the keys go to (NULL for
 * none: the body gets them), a root of the heap; focus_visible says the
 * keyboard moved it there, so its ring is drawn while window_focused says
 * the view has the focus of its program.  focus_generation counts the
 * changes of the three, and painted_focus is its value when the display
 * list was made, so a change paints the page again without a new layout.
 *
 * The geometry scripts ask for (geometry.c, ws074-p031): query_css is the
 * style engine their selectors are matched with (made when first needed,
 * apart from the styling's engine, which is made anew at each change);
 * font_paths are the fonts the view gives (NULL until it does), and
 * viewport_width and viewport_height its size, with which a script's
 * question lays the page out when it changed; scroll_x and scroll_y are
 * how far the view is scrolled, in CSS pixels.  scroll_requested says a
 * script moved them (ws074-p082), which the view takes as its scroll.
 * layout_serial counts the layouts; box_index (box_index_capacity slots,
 * open addressing by the node) finds each node's first box of the layout
 * whose serial box_index_serial is, made when a script first asks after a
 * layout (geometry.c).
 */
struct page {
	struct vm_heap *heap;
	struct dom_document *document;
	struct vm_realm *realm;
	struct bind_window *window;
	/* Borrowed while page_load_html owns the live parser; NULL afterward. */
	struct html_parser *parser;
	struct css_engine *css;
	struct text_system text;
	int text_open;
	struct layout_tree layout;
	int laid_out;
	struct layout_tree previous_layout;
	int previous_laid_out;
	struct paint_list paint;
	int painted;
	char *base;
	double now;
	uint32_t styled_generation;
	uint32_t laid_out_generation;
	struct wb_vector images;
	struct net_loader *loader;
	uint32_t images_generation;
	uint32_t laid_out_images;
	struct wb_vector sheets;
	uint32_t sheets_generation;
	uint32_t styled_sheets;
	struct wb_vector scripts;
	struct wb_vector fetches;
	/* The parser-blocking scripts the view fetched before the document was parsed (struct page_prefetched *, BUG-207). */
	struct wb_vector prefetched;
	/* Pending child responses own temporary native roots until task completion. */
	struct wb_vector frame_loads;
	/* Defer nested child-response checkpoints while the outer snapshot drains. */
	int frames_running;
	/* Defer nested ready-script drains while the outer insertion-order loop runs. */
	int scripts_running;
	struct wb_vector fonts;
	uint32_t fonts_generation;
	uint32_t laid_out_fonts;
	uint32_t styling;
	page_console console;
	void *console_context;
	struct dom_element *focused;
	int focus_visible;
	int window_focused;
	uint32_t focus_generation;
	uint32_t painted_focus;
	/* The media (media.c, ws121-p004): struct page_media *, the pictures that came, and their count when painted. */
	struct wb_vector media;
	uint32_t media_generation;
	uint32_t painted_media;
	struct css_engine *query_css;
	const struct text_font_paths *font_paths;
	int viewport_width;
	int viewport_height;
	double scroll_x;
	double scroll_y;
	int scroll_requested;
	uint32_t layout_serial;
	struct page_box_slot *box_index;
	size_t box_index_capacity;
	uint32_t box_index_serial;
};

/*
 * One input of the pointer for the page: its place in the document and in
 * the viewport (pixels), the DOM's button number, the modifiers held
 * (BIND_MOD_*), and a wheel's distances in pixels.
 */
struct page_pointer {
	int x;
	int y;
	int client_x;
	int client_y;
	int button;
	unsigned modifiers;
	double delta_x;
	double delta_y;
};

/* Pages (page.c). */
int page_create(struct page **page, const void *stack_base);
void page_destroy(struct page *page);
int page_load_html(struct page *page, const unsigned char *bytes, size_t length);
int page_load_file(struct page *page, const char *path);
int page_load_location(struct page *page, const char *location);
int page_dump_dom(const struct page *page, struct wb_buffer *out);
int page_dump_style(struct page *page, struct wb_buffer *out);
int page_open_fonts(struct page *page, const struct text_font_paths *paths);
int page_layout(struct page *page, int width, int height);
int page_update_styles(struct page *page);
int page_paint(struct page *page);
int page_title(const struct page *page, struct wb_buffer *out);

/* Child response tasks (frame-load.c). */
void page_frames_init(struct page *page);
void page_frames_release(struct page *page);
int page_frames_checkpoint(struct page *page);

/* Dynamic scripts (script.c). */
void page_scripts_init(struct page *page);
void page_scripts_release(struct page *page);

/* The parser-blocking scripts fetched before a document is parsed (script.c, BUG-207). */
int page_scan_scripts(const char *base, const unsigned char *bytes, size_t length, struct wb_vector *locations);
int page_add_prefetched(struct page *page, const char *location, int error, const unsigned char *bytes, size_t length);

/* Style sheets (sheets.c). */
void page_sheets_init(struct page *page);
int page_sheets_add(struct page *page);
int page_sheets_pending(const struct page *page);
void page_sheets_release(struct page *page);

/* Web fonts (fonts.c, ws074-p070). */
void page_fonts_init(struct page *page);
int page_fonts_add_sheet(struct page *page, const struct css_sheet *sheet);
void page_fonts_install(struct page *page);
void page_fonts_release(struct page *page);

/* Scripts, events and time (script.c). */
int page_start_scripts(struct page *page);
int page_url(const struct page *page, struct net_url *url);
int page_run_script_element(void *context, struct dom_element *script);
int page_fire_load(struct page *page);
int page_set_time(struct page *page, double now);
int page_next_timer(const struct page *page, double *due);
int page_settle(struct page *page, double budget);
int page_needs_layout(const struct page *page);

/* The user's input and the focus (input.c). */
int page_mouse_event(struct page *page, const char *type, const struct page_pointer *pointer, int *canceled);
int page_wheel_event(struct page *page, const struct page_pointer *pointer, int *canceled);
int page_key_event(struct page *page, const char *type, const struct bind_key *key, int *canceled);
int page_focus_at(struct page *page, int x, int y);
int page_focus_move(struct page *page, int backward);
int page_focus_field(struct page *page, const char *token);
int page_window_focus(struct page *page, int focused);
int page_activate_focused(struct page *page, struct wb_buffer *href, int *found);
int page_click_control(struct page *page, int x, int y, struct wb_buffer *href, int *found);
int page_focus_pressable(struct page *page);
int page_focus_rect(struct page *page, struct layout_rect *rect);
int page_needs_paint(const struct page *page);
int page_paint_focus(struct page *page);

/* Forms (form.c). */
int page_is_editing(struct page *page);
int page_edit_key(struct page *page, const char *key, const char *text, unsigned modifiers, int *handled);
int page_activate_control(struct page *page, struct dom_element *element, int implicit, struct wb_buffer *href, int *found, int *handled);
int page_submit_form(struct page *page, struct dom_element *form, struct dom_element *submitter, struct wb_buffer *href, int *found);
int page_place_caret(struct page *page, int x);
int page_caret_to_end(struct page *page);
int page_paint_caret(struct page *page);
struct dom_element *page_compose_element(struct page *page);
int page_compose(struct page *page, const char *text, int cursor);
int page_commit_text(struct page *page, const char *text, uint32_t before, uint32_t after);
void page_compose_end(struct page *page, struct dom_element *element);

/* Links (link.c). */
int page_link_at(struct page *page, int x, int y, struct wb_buffer *href, int *found);
int page_resolve_file(const char *base, const char *href, struct wb_buffer *out);
int page_resolve_location(const char *base, const char *href, struct wb_buffer *out);
void page_images_init(struct page *page);
void page_set_loader(struct page *page, struct net_loader *loader);
int page_load_bytes(struct page *page, const unsigned char *bytes, size_t length, const char *location);
int page_net_create(struct net_loader **loader);
void page_net_destroy(struct net_loader *loader);
size_t page_net_poll_fds(const struct net_loader *loader, struct pollfd *fds, size_t capacity);
int page_net_timeout(const struct net_loader *loader);
void page_net_process(struct net_loader *loader, const struct pollfd *fds, size_t count);
int page_net_is_remote(const char *location);
int page_net_fetch(struct net_loader *loader, const char *location, page_request_done done, void *context, struct net_request **request);
void page_net_cancel(struct net_request *request);
int page_net_result(const struct net_request *request, const unsigned char **bytes, size_t *length, const char **url);
int page_load_images(struct page *page);
void page_set_viewport(struct page *page, int width, int height);
void page_set_fonts(struct page *page, const struct text_font_paths *paths);
void page_set_scroll(struct page *page, double x, double y);

/* The geometry and the selectors of scripts (geometry.c, ws074-p031). */
struct css_engine *page_selector_engine(void *context);
int page_node_box(void *context, struct dom_node *node, struct bind_box *box);
void page_document_size(void *context, double *width, double *height);
void page_scroll(void *context, double *x, double *y);
int page_computed_style(void *context, struct dom_element *element, struct css_style *style);
void page_box_index_release(struct page *page);
void page_scroll_to(void *context, double x, double y);
struct dom_node *page_element_at(void *context, double x, double y);

/* Web Storage (storage.c, ws074-p080). */
const struct bind_storage_calls *page_storage_calls(void);
void page_set_console(struct page *page, page_console console, void *context);
const char *page_failure_reason(void);
const struct img_bitmap *page_image_of(void *context, const struct dom_element *element);
const struct img_bitmap *page_image_by_url(void *context, const struct vm_string *url);
void page_images_release(struct page *page);

/* The media of <video> and <audio> (media.c, ws121-p004). */
struct media_status;
void page_media_init(struct page *page);
void page_media_release(struct page *page);
int page_load_media(struct page *page);
const struct img_bitmap *page_media_bitmap(const struct page *page, const struct dom_element *element);
size_t page_media_poll_fds(const struct page *page, struct pollfd *fds, size_t capacity);
int page_media_timeout(const struct page *page);
void page_media_process(struct page *page);
int page_media_play(struct page *page, const struct dom_element *element, int play);
int page_media_status(const struct page *page, const struct dom_element *element, struct media_status *status);
void page_media_seek(struct page *page, const struct dom_element *element, double seconds);
int page_media_host(void *context, struct dom_element *element, int request, double value, struct bind_media *state);
int page_media_click(struct page *page, struct dom_element *element, int x, int y);
int page_fetch(const char *base, const char *href, struct wb_buffer *bytes, struct wb_buffer *final_url);
/* Fresh response storage receives owned metadata/body, or an empty response on failure. */
int page_fetch_response(const char *base, const char *href, struct net_response *response);
/* Declared MIME essence selects a supported native document kind; failures preserve content. */
int page_document_content(const char *mime, size_t length, enum dom_document_content *content);

#endif
