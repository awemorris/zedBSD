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
#include "net/net.h"
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
struct net_cookie_jar;
struct net_cache;
struct browser_instance;
struct net_response;
struct page_box_slot;
struct net_url;

/* A request's callback, as the loader calls it (net_request_done). */
typedef void (*page_request_done)(void *context, struct net_request *request);

/*
 * A parser-blocking script or a style sheet fetched before its document
 * was parsed (script.c): its location (resolved against the document's
 * URL), how the fetch ended (0, or an errno value), the response's status
 * and Content-Type, and the bytes it brought.  The page owns it from
 * page_add_prefetched to page_destroy.
 */
struct page_prefetched {
	char *location;
	int error;
	int status;
	struct wb_buffer mime;
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
 * generations the document's generation (page_generation: without the
 * changes no style can see) when the style sheets were gathered and when
 * the page was last laid out.  images is the table of
 * the page's decoded images (page/images.c), by location; loader (the
 * embedder's, or NULL) fetches http and https images without blocking,
 * and images_generation counts the images that arrived, which
 * laid_out_images is compared with to lay the page out again.  instance
 * (the view's, borrowed; NULL until the view gives it, when the thread's
 * default instance is used, page_instance) holds the cookies and the Web
 * Storage areas the page sees.
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
 * font_paths are the fonts the view gives (NULL until it does), with the
 * sans font's companions (font_companions; NULL for the defaults), and
 * viewport_width and viewport_height its size, with which a script's
 * question lays the page out when it changed, and dark its color scheme
 * (nonzero for the dark one, prefers-color-scheme); scroll_x and scroll_y are
 * how far the view is scrolled, in CSS pixels.  scroll_requested says a
 * script moved them (ws074-p082), which the view takes as its scroll.
 * layout_serial counts the layouts; box_index (box_index_capacity slots,
 * open addressing by the node) finds each node's first box of the layout
 * whose serial box_index_serial is, made when a script first asks after a
 * layout (geometry.c).
 */
/*
 * How an embedder gives a page's iframes threads of their own: attach
 * gives an iframe's browsing context its thread before its first document
 * (0, or why not: the iframe then has no browsing context), detach ends it
 * (the iframe left its document, or the page is put away), and ring wakes
 * a runner's thread (NULL: the page's own) for work it has to do.  The
 * calls are made with the page's tasks held (on any of its threads).
 */
struct page;

/* A response an iframe shows in a browsing context of another agent: its final URL, its Content-Type and its bytes. */
struct page_remote_response {
	const char *url;
	const char *mime;
	size_t mime_length;
	const unsigned char *body;
	size_t length;
};

/*
 * With the threads, how an embedder gives an iframe whose document is of
 * another origin a browsing context in an agent of its own (NULL calls:
 * the page keeps every document in its own heap): remote_load shows a
 * response there (in the context the iframe has, or a new one), which
 * runs on a thread and in a heap of its own, and tells the page when its
 * document loaded (page_remote_loaded); remote_close ends it (the iframe
 * left its document, shows a document of the page's agent again, or the
 * page is put away); remote_frame gives the number of its frame item,
 * told the size the page lays it out at (on any of the page's threads, the
 * style thread's too; 0: none).  The calls that change it are made with the
 * page's tasks held.
 */
struct page_frame_calls {
	int (*attach)(void *context, struct page *page, struct dom_element *element);
	void (*detach)(void *context, struct dom_element *element);
	void (*ring)(void *context, void *runner);
	int (*remote_load)(void *context, struct page *page, struct dom_element *element, const struct page_remote_response *response);
	void (*remote_close)(void *context, struct dom_element *element);
	uint64_t (*remote_frame)(void *context, const struct dom_element *element, int width, int height);
	const struct bind_remote_calls *remote;
};

/* A link element whose stylesheet arrived (or failed), waiting for its load or error event. */
struct page_link_event {
	struct dom_element *element;
	int failed;
};

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
	/* The number of the display list painted last, which no other list of any page has (a view of its own threads takes each into a scene). */
	uint64_t paint_generation;
	/* Whether the display list painted last was taken into a scene (the page's list is empty until it paints again). */
	int paint_taken;
	/* Whether its animation frames wait for the presents of a drawing thread (frames_presented), and how many came. */
	int frames_presented;
	uint64_t frames;
	char *base;
	double now;
	uint32_t styled_generation;
	uint32_t laid_out_generation;
	/* The sum of the generations of the documents in the iframes when the page was laid out, and how many iframe elements there were. */
	uint64_t laid_out_frames;
	size_t laid_out_iframes;
	/* Only successful same-origin child navigations observed by this page. */
	struct wb_vector visited_urls;
	uint32_t visits_generation;
	uint32_t styled_visits;
	uint32_t laid_out_visits;
	struct wb_vector images;
	/* The <img> elements (heap roots) whose load or error event is still to be fired (struct page_image_watch *). */
	struct wb_vector image_watches;
	/* The locations (char *) of the http and https images a work lent to the style thread met new, fetched by the page's thread when the work came back. */
	struct wb_vector images_deferred;
	struct net_loader *loader;
	/* The embedder's threads for the iframes (NULL: they run on the page's thread), with their context. */
	const struct page_frame_calls *frame_calls;
	void *frame_calls_context;
	struct browser_instance *instance;
	uint32_t images_generation;
	uint32_t laid_out_images;
	struct wb_vector sheets;
	struct wb_vector link_events;
	uint32_t sheets_generation;
	uint32_t styled_sheets;
	/* How many times the sheets were gathered, and the count the layout was made with (a gathering outside a layout makes it stale). */
	uint32_t gathered;
	uint32_t laid_out_gathered;
	struct wb_vector scripts;
	/* Parser-inserted inline module scripts and their retained URL graph. */
	struct wb_vector module_scripts;
	struct wb_vector deferred_scripts;
	struct js_module_graph *module_graph;
	struct wb_vector fetches;
	/* The parser-blocking scripts the view fetched before the document was parsed (struct page_prefetched *, BUG-207). */
	struct wb_vector prefetched;
	/* Pending child responses own temporary native roots until task completion. */
	struct wb_vector frame_loads;
	/* The painted documents of the page's iframes, the last ones made (struct frame_cache_entry). */
	struct wb_vector frame_cache;
	/* Defer nested child-response checkpoints while the outer snapshot drains. */
	int frames_running;
	/* The window's load event waits for the frames the document embeds (those of other agents too: how many have not loaded). */
	int load_pending;
	unsigned remote_loads;
	/* The document came with Cache-Control: no-store (the view keeps no copy of it for Back and Forward). */
	int no_store;
	/* The document's page is showing: set by its pageshow, cleared by its pagehide (HTML's page showing). */
	int showing;
	/* Why the last load of a location failed over TLS ("" when it did not), for the load callback. */
	char failure_reason[NET_TLS_REASON_MAX];
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
	const struct text_font_companions *font_companions;
	int viewport_width;
	int viewport_height;
	int dark;
	double scroll_x;
	double scroll_y;
	int scroll_requested;
	/* Set while the page is laid out for a rendering update (the position options that last fit are remembered then). */
	int rendering;
	uint32_t layout_serial;
	struct page_box_slot *box_index;
	size_t box_index_capacity;
	uint32_t box_index_serial;
	/* The view transition (transition.c): its state, whether there is one, and the counts that tell the layout it changed. */
	struct layout_transition transition;
	int transition_on;
	uint32_t transition_generation;
	uint32_t laid_out_transition;
	/* Whether a view transition is active and its types (for :active-view-transition; the style engines are told). */
	int vt_active;
	const struct vm_string *vt_types[8];
	int vt_type_count;
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
int page_lend(struct page *page, vm_lend_work work, void *argument);
int page_lend_layout(struct page *page, struct layout_tree *tree, struct css_engine *engine, struct dom_document *document, int width, int height,
    layout_unit scroll_x, layout_unit scroll_y, const struct layout_transition *transition);
int page_lend_paint(struct page *page, struct paint_list *list, const struct layout_tree *tree);
int page_update_styles(struct page *page);
uint32_t page_generation(const struct page *page);
int page_load_resources(struct page *page);
int page_wait_sheets(struct page *page);
int page_load_waiting(const struct page *page);
int page_paint(struct page *page);
int page_title(const struct page *page, struct wb_buffer *out);

/* Child response tasks (frame-load.c). */
void page_frames_init(struct page *page);
void page_frames_release(struct page *page);
int page_frames_checkpoint(struct page *page);
void page_frames_size(struct page *page);
void page_set_frame_calls(struct page *page, const struct page_frame_calls *calls, void *context);
int page_frame_attach(void *context, struct dom_element *element);
void page_frame_detach(void *context, struct dom_element *element);
void page_frames_suspend(struct page *page);
int page_frames_resume(struct page *page);
uint64_t page_frame_remote(void *context, const struct dom_element *element, layout_unit width, layout_unit height, float zoom);
void page_remote_loading(struct page *page);
int page_remote_loaded(struct page *page, struct dom_element *element, int fire);
int page_remote_navigate(struct page *page, struct dom_element *element, const char *url);

/* Dynamic scripts (script.c). */
void page_scripts_init(struct page *page);
void page_scripts_release(struct page *page);

/* Parser-inserted inline modules (module.c). */
void page_modules_init(struct page *page);
void page_modules_release(struct page *page);
int page_module_enqueue(struct page *page, struct dom_element *script);
int page_modules_run(struct page *page);
int page_integrity_matches(const char *metadata, const unsigned char *data, size_t length);
int page_import_module(void *context, const char *url, int json, vm_value *namespace_object);
int page_deferred_run(struct page *page);
int page_sheets_fire_links(struct page *page);

/* The parser-blocking scripts and the style sheets fetched before a document is parsed (script.c). */
int page_scan_scripts(const char *base, const unsigned char *bytes, size_t length, struct wb_vector *locations);
int page_add_prefetched(struct page *page, const char *location, int error, int response_status, const char *mime, const unsigned char *bytes, size_t length);
const struct page_prefetched *page_prefetched_find(const struct page *page, const char *location);

/* Style sheets (sheets.c). */
void page_sheets_init(struct page *page);
int page_sheets_add(struct page *page);
int page_sheets_pending(const struct page *page);
int page_sheets_blocking(const struct page *page);
int page_sheets_blocking_for(const struct page *page, const struct net_loader_client *client);
void page_sheets_release(struct page *page);

/* Web fonts (fonts.c, ws074-p070). */
void page_fonts_init(struct page *page);
int page_fonts_add_sheet(struct page *page, const struct css_sheet *sheet);
void page_fonts_install(struct page *page);
int page_fonts_pending(const struct page *page);
void page_fonts_release(struct page *page);

/* Scripts, events and time (script.c). */
int page_start_scripts(struct page *page);
int page_url(const struct page *page, struct net_url *url);
int page_run_script_element(void *context, struct dom_element *script);
int page_fire_load(struct page *page);
int page_fire_load_resume(struct page *page);
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
int page_page_transition(struct page *page, const char *type, int persisted);
int page_unload(struct page *page);
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
int page_compose(struct page *page, const char *text, int begin, int cursor);

/* What the focused control is for, for an input method (ws177-p019; the numbers are text-input-v3's purposes and hints). */
#define PAGE_PURPOSE_NORMAL	0
#define PAGE_PURPOSE_DIGITS	2
#define PAGE_PURPOSE_NUMBER	3
#define PAGE_PURPOSE_PHONE	4
#define PAGE_PURPOSE_URL	5
#define PAGE_PURPOSE_EMAIL	6
#define PAGE_HINT_MULTILINE	0x200U
int page_compose_context(struct page *page, struct wb_buffer *text, size_t *caret, int *purpose, unsigned *hints);
int page_commit_text(struct page *page, const char *text, uint32_t before, uint32_t after);
void page_compose_end(struct page *page, struct dom_element *element);
int page_compose_commit(struct page *page, struct dom_element *element);

/* Links (link.c). */
int page_link_at(struct page *page, int x, int y, struct wb_buffer *href, int *found);
int page_image_at(struct page *page, int x, int y, const struct img_bitmap **bitmap, struct wb_buffer *source);
int page_resolve_file(const char *base, const char *href, struct wb_buffer *out);
int page_resolve_location(const char *base, const char *href, struct wb_buffer *out);
int page_record_visited(struct page *page, const struct wb_buffer *url);
void page_images_init(struct page *page);
int page_images_fetch_deferred(struct page *page);
void page_set_loader(struct page *page, struct net_loader *loader);
void page_set_thread_client(struct net_loader_client *client);
struct net_loader_client *page_client(const struct page *page);
void page_set_instance(struct page *page, struct browser_instance *instance);
struct browser_instance *page_instance(struct page *page);
struct net_cookie_jar *page_cookies(struct page *page);
int page_load_bytes(struct page *page, const unsigned char *bytes, size_t length, const char *location, const char *mime, size_t mime_length);
int page_load_begin(struct page *page, const unsigned char *bytes, size_t length, const char *location, const char *mime, size_t mime_length, int *finish);
int page_load_finish(struct page *page);
int page_net_create(struct net_cookie_jar *jar, struct net_cache *cache, void (*notify)(void *context), void *context, struct net_loader **loader);
void page_net_destroy(struct net_loader *loader);
size_t page_net_pending(const struct net_loader *loader);
void page_net_wait(struct net_loader *loader, int timeout);
void page_net_process(struct net_loader *loader);
int page_net_is_remote(const char *location);
int page_net_fetch(struct net_loader *loader, const char *location, page_request_done done, void *context, struct net_request **request);
void page_net_cancel(struct net_request *request);
int page_net_result(const struct net_request *request, const unsigned char **bytes, size_t *length, const char **url);
const char *page_net_reason(const struct net_request *request);
int page_load_images(struct page *page);
void page_set_viewport(struct page *page, int width, int height);
void page_set_color_scheme(struct page *page, int dark);
void page_set_fonts(struct page *page, const struct text_font_paths *paths);
void page_set_font_companions(struct page *page, const struct text_font_companions *companions);
void page_set_frames_presented(struct page *page, int presented);
void page_frame_presented(struct page *page);
int page_frame_count(void *context, uint64_t *count);
int page_sticky_sync(struct page *page, layout_unit scroll_y);
void page_set_scroll(struct page *page, double x, double y);

/* The geometry and the selectors of scripts (geometry.c, ws074-p031). */
struct css_engine *page_selector_engine(void *context);
int page_grid_tracks(void *context, struct dom_node *node, int columns, struct wb_vector *sizes, int *leading, int *repeats, int *found);
int page_node_box(void *context, struct dom_node *node, struct bind_box *box);
int page_pseudo_box(void *context, struct dom_node *node, int pseudo, struct bind_box *box);
struct dom_node *page_hit_node(struct page *page, int x, int y, int *offset_x, int *offset_y);
int page_frame_view_transition(void *context, struct dom_element *element, int step);
double page_frame_view_transition_duration(void *context, struct dom_element *element);
double page_transition_longest(const struct layout_box *box, int depth);
int page_frame_capture(void *context, struct dom_element *element);
uint64_t page_frames_generation(const struct page *page);
uint64_t page_frames_generation_count(const struct page *page, size_t *iframes);
int page_frame_paint(void *context, const struct dom_element *element, layout_unit width, layout_unit height, float zoom, struct paint_frame **out);
int page_node_rects(void *context, struct dom_node *node, struct bind_rect *rects, size_t max, size_t *count);
int page_view_transition(void *context, int step);
int page_vt_pseudo_style(void *context, struct dom_element *element, int pseudo, const struct vm_string *name, struct css_style *style);
double page_view_transition_duration(void *context);
void page_view_transition_time(void *context, double milliseconds);
void page_view_transition_time_of(void *context, double milliseconds, int pseudo, const struct vm_string *name);
int page_view_transition_names(void *context, struct wb_units *units);
void page_view_transition_style(void *context, int pseudo, const struct vm_string *name, const char *text);
void page_view_transition_active(void *context, int active, struct vm_string **types, size_t count);
void page_document_size(void *context, double *width, double *height);
void page_scroll(void *context, double *x, double *y);
int page_computed_style(void *context, struct dom_element *element, struct css_style *style);
int page_cascaded_style(void *context, struct dom_element *element, struct css_style *style);
void page_box_index_release(struct page *page);
void page_scroll_to(void *context, double x, double y);
void page_rendering(void *context);
struct dom_node *page_element_at(void *context, double x, double y);
struct dom_node *page_child_element_at(void *context, struct bind_window *window, double x, double y);

/* Web Storage (storage.c): the window's calls, and the release of an instance's areas. */
const struct bind_storage_calls *page_storage_calls(void);
void page_storage_release(struct wb_vector *areas);
void page_set_console(struct page *page, page_console console, void *context);
const char *page_failure_reason(const struct page *page);
const struct img_bitmap *page_image_of(void *context, const struct dom_element *element);
int page_svg_render(const struct dom_element *svg, struct img_bitmap *bitmap, int width, int height);
int page_svg_decode(struct page *page, const unsigned char *bytes, size_t length, struct img_bitmap *bitmap, int width, int height);
const struct img_bitmap *page_image_variant(void *context, const struct img_bitmap *image, int width, int height);
const struct img_bitmap *page_image_by_url(void *context, const struct vm_string *url);
void page_images_release(struct page *page);
int page_image_host(void *context, struct dom_element *element, int restart, struct bind_image *state);
int page_images_checkpoint(struct page *page);
int page_images_waiting(const struct page *page);
int page_images_pending(const struct page *page);
int page_images_ready(const struct page *page);

/* The media of <video> and <audio> (media.c, ws121-p004). */
struct media_status;
void page_media_init(struct page *page);
void page_media_release(struct page *page);
int page_load_media(struct page *page);
const struct img_bitmap *page_media_bitmap(const struct page *page, const struct dom_element *element);
int page_media_timeout(const struct page *page);
void page_media_process(struct page *page);
int page_media_play(struct page *page, const struct dom_element *element, int play);
int page_media_status(const struct page *page, const struct dom_element *element, struct media_status *status);
void page_media_seek(struct page *page, const struct dom_element *element, double seconds);
int page_media_host(void *context, struct dom_element *element, int request, double value, struct bind_media *state);
int page_media_click(struct page *page, struct dom_element *element, int x, int y);
/* The cookies of an http or https fetch go to and come from a jar (page_cookies; NULL for none). */
int page_fetch(struct net_cookie_jar *jar, const char *base, const char *href, struct wb_buffer *bytes, struct wb_buffer *final_url);
/* Fresh response storage receives owned metadata/body, or an empty response on failure. */
int page_fetch_response(struct net_cookie_jar *jar, const char *base, const char *href, struct net_response *response);
int page_fetch_now(struct page *page, const char *href, int kind, struct wb_buffer *bytes, struct wb_buffer *final_url);
int page_fetch_response_now(struct page *page, const char *location, int kind, struct net_response *response);
/* Declared MIME essence selects a supported native document kind; failures preserve content. */
int page_document_content(const char *mime, size_t length, enum dom_document_content *content);

#endif
