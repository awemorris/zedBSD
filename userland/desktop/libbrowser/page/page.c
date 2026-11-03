/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Loading a page: bytes to text, text to a DOM (running the scripts as the
 * parser reaches them), the DOM's <style> elements to the style engine;
 * and the text dumps the tests read.
 */

#include "page/page.h"
#include "base/os-file.h"
#include "bind/internal.h"
#include "net/net.h"
#include "xml/dom.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most bytes of live cells a page's heap may hold. */
#define PAGE_HEAP_LIMIT		((size_t)1024U * 1024U * 1024U)

/* How many times more a layout is made at most for the query containers' sizes. */
#define PAGE_CONTAINER_PASSES	2

/* The deepest element nesting the style dump descends (the parser caps nesting too). */
#define PAGE_DUMP_DEPTH		512

/* The number of the last display list any page painted (page->paint_generation), changed atomically: pages of several views paint on their own threads. */
static uint64_t page_paint_next;

/*
 * A work lent to the style thread with its arguments and what it gives
 * back: the page laid out at a size (page_layout_work), a tree laid out
 * (page_lend_layout), a list painted (page_lend_paint) or the sticky boxes
 * moved for a scroll (page_sticky_sync).
 */
/* The most bytes of its arena the styles of custom properties may take in an engine kept across changes, and the most styles it may keep, before it is made again. */
#define PAGE_STYLE_ARENA_MAX	(32ULL * 1024ULL * 1024ULL)
#define PAGE_STYLE_KEPT_MAX	100000ULL

struct page_layout_args {
	struct page *page;
	int width;
	int height;
};

struct page_tree_args {
	struct page *page;
	struct layout_tree *tree;
	struct css_engine *engine;
	struct dom_document *document;
	int width;
	int height;
	layout_unit scroll_x;
	layout_unit scroll_y;
	const struct layout_transition *transition;
};

struct page_paint_args {
	struct paint_list *list;
	const struct layout_tree *tree;
};

struct page_sticky_args {
	struct page *page;
	layout_unit scroll_y;
};

static int page_gather_styles(struct page *page);
static int page_layout_work(void *argument);
static int page_layout_verify(struct page *page, const struct page_layout_args *args);
static struct layout_tree *page_layout_previous(struct page *page);
static void page_layout_count(struct page *page);
static unsigned page_change_scope(void *context, struct dom_element *element, int kind, const struct vm_string *attribute);
static void page_change_removed(void *context, struct dom_node *node);
static int page_style_visit(void *context, struct dom_node *node, unsigned bits);
static int page_styles_reusable(const struct page *page);
static int page_engine_fresh(struct page *page, struct css_engine **made);
static void page_style_changed(void *context, struct dom_element *element);
static int page_tree_work(void *argument);
static int page_list_work(void *argument);
static int page_paint_work(void *argument);
static int page_sticky_work(void *argument);
static void page_animations_sync(struct page *page);
static int page_load_xhtml(struct page *page, const unsigned char *bytes, size_t length);
static int page_xhtml_inline_scripts(struct page *page);
static int page_xhtml_script_eligible(const struct dom_node *node, const struct dom_document *document);
static int page_load_document(struct page *page, const unsigned char *bytes, size_t length, const char *mime, size_t mime_length);
static int page_parse_html(struct page *page, const unsigned char *bytes, size_t length);
static int page_finish_html(struct page *page, int started);
static int page_file_xhtml(const char *path);
static int page_link_visited(void *context, struct dom_element *element);
static int page_container_size(void *context, const struct dom_element *container, float *width, float *height);
static int page_containers_moved(const struct page *page);
static int page_text_of(const struct dom_node *node, struct wb_units *units);
static const struct dom_node *page_find_title(const struct dom_node *node, int depth);
static int page_dump_node(const struct dom_node *node, int depth, struct wb_buffer *out);
static int page_dump_style_node(struct page *page, struct dom_element *element, const struct css_style *parent, int depth, struct wb_buffer *out);
static int page_dump_style_line(const struct dom_element *element, const struct css_style *style, int depth, struct wb_buffer *out);
static int page_indent(struct wb_buffer *out, int depth);
static int page_append_string(struct wb_buffer *out, const struct vm_string *string);
static int page_append_length(struct wb_buffer *out, const struct css_length *length);
static int page_attribute_styled(void *context, const struct vm_string *name);

/*
 * Makes an empty page: a heap whose C stack ends at stack_base, and an
 * empty document.
 */
int
page_create(
	struct page **page,
	const void *stack_base)
{
	struct page *created;
	int error;

	/* Allocates the page and its heap. */
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	error = vm_heap_create(&created->heap, PAGE_HEAP_LIMIT);
	if (error != 0) {
		free(created);
		return error;
	}

	/* Its stack ends where the caller says, and it has no images yet. */
	vm_heap_set_stack_base(created->heap, stack_base);
	page_images_init(created);
	page_media_init(created);
	page_sheets_init(created);
	page_fonts_init(created);
	page_scripts_init(created);
	page_modules_init(created);
	page_frames_init(created);
	wb_vector_init(&created->visited_urls, sizeof(char *));

	/* Makes the document and keeps it alive as a root. */
	created->document = dom_document_create(created->heap);
	if (created->document == NULL) {
		vm_heap_destroy(created->heap);
		free(created);
		return ENOMEM;
	}

	/* The document asks the page whether a style sheet names a data-* attribute it changes. */
	created->document->attribute_styled = page_attribute_styled;
	created->document->attribute_styled_context = created;

	/* Keeps the document alive as a root. */
	error = vm_heap_add_root(created->heap, (struct vm_cell **)&created->document);
	if (error != 0) {
		vm_heap_destroy(created->heap);
		free(created);
		return error;
	}

	/* Keeps the focused element alive as a root (none yet). */
	error = vm_heap_add_root(created->heap, (struct vm_cell **)&created->focused);
	if (error != 0) {
		vm_heap_destroy(created->heap);
		free(created);
		return error;
	}

	/* A page starts in a view that has its program's focus (the view says otherwise). */
	created->window_focused = 1;

	/* The realm whose global object is the document's window. */
	error = page_start_scripts(created);
	if (error != 0) {
		page_destroy(created);
		return error;
	}

	/* Succeeded: the page is empty. */
	*page = created;
	return 0;
}

/*
 * Destroys a page, its styles and its heap.
 */
void
page_destroy(
	struct page *page)
{
	char **url;
	size_t index;

	/* A NULL page is nothing to destroy. */
	if (page == NULL)
		return;

	/* Frees the display list, the layout, the fonts and the style engine, then every cell with the heap. */
	if (page->painted)
		paint_release(&page->paint);
	if (page->laid_out)
		layout_release(&page->layout);
	if (page->previous_laid_out)
		layout_release(&page->previous_layout);
	if (page->text_open)
		text_system_close(&page->text);
	page_media_release(page);
	page_images_release(page);
	page_cache_report(page);
	if (page->document != NULL) {
		page->document->change_scope = NULL;
		page->document->change_removed = NULL;
	}
	css_engine_destroy(page->css);
	css_engine_destroy(page->query_css);
	page_box_index_release(page);
	page_sheets_release(page);
	page_fonts_release(page);
	page_frames_release(page);

	/* The tab's observed URLs are owned separately from VM cells. */
	for (index = 0; index < page->visited_urls.count; index++) {
		url = wb_vector_at(&page->visited_urls, index);
		free(*url);
	}
	wb_vector_release(&page->visited_urls);
	page_scripts_release(page);
	page_modules_release(page);
	bind_window_destroy(page->window);
	vm_realm_destroy(page->realm);
	vm_heap_destroy(page->heap);
	free(page->base);
	free(page);
}

/*
 * Loads an HTML document from its bytes (UTF-8 in this pass) and gathers
 * its style sheets.
 */
int
page_load_html(
	struct page *page,
	const unsigned char *bytes,
	size_t length)
{
	int error;

	/* The parse, then what follows it once the sheets came. */
	error = page_parse_html(page, bytes, length);
	if (error != 0)
		return error;
	error = page_finish_html(page, 0);
	if (error != 0)
		return error;

	/* Succeeded: the page holds the document and its styles. */
	return 0;
}

/* Parses an HTML document's bytes into the page's document, running the scripts the parser reaches. */
static int
page_parse_html(
	struct page *page,
	const unsigned char *bytes,
	size_t length)
{
	struct html_parser *parser;
	struct wb_units units;
	int error;

	/* Decodes the bytes, dropping a UTF-8 byte order mark. */
	if (length >= 3 && bytes[0] == 0xefU && bytes[1] == 0xbbU && bytes[2] == 0xbfU) {
		bytes += 3;
		length -= 3;
	}

	/* Decodes the text. */
	wb_units_init(&units);
	error = wb_utf8_to_units(bytes, length, &units);
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}

	/* Parses the text into the document, with scripting: each script runs when the parser reaches its end. */
	error = html_parser_create(&parser, page->document, 1);
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}

	/* The page runs the scripts the parser reaches. */
	html_parser_set_script_hook(parser, page_run_script_element, page);
	page->parser = parser;

	/* Feeds it all and finishes. */
	error = html_parser_feed(parser, units.data, units.length);
	if (error == 0)
		error = html_parser_finish(parser);
	page->parser = NULL;
	html_parser_destroy(parser);
	wb_units_release(&units);
	if (error != 0)
		return error;

	/* Succeeded: the document is parsed. */
	return 0;
}

/*
 * Does what follows page_load_begin for an HTML document, once
 * page_sheets_blocking says no sheet that blocks its scripts is on its way (or the caller waited
 * long enough: one that comes later is styled in when it comes).
 */
int
page_load_finish(
	struct page *page)
{
	return page_finish_html(page, 1);
}

/*
 * Does what follows the parse of an HTML document: waits for the sheets it
 * links (unless started says the caller waited for them), which block its deferred scripts,
 * runs them and its modules, fires DOMContentLoaded and the window's load,
 * and gathers its style sheets.
 */
static int
page_finish_html(
	struct page *page,
	int started)
{
	int error;

	/* The document is parsed: DOMContentLoaded, then the window's load.  The sheets it links block its deferred scripts. */
	if (started)
		error = page_update_styles(page);
	else
		error = page_wait_sheets(page);
	if (error != 0)
		return error;
	error = page_deferred_run(page);
	if (error != 0)
		return error;
	error = page_modules_run(page);
	if (error != 0)
		return error;
	error = page_fire_load(page);
	if (error != 0)
		return error;

	/* The style sheets of the document as the scripts left it. */
	error = page_gather_styles(page);
	if (error != 0)
		return error;

	/* Succeeded: the page holds the document and its styles. */
	return 0;
}

/*
 * Loads an HTML document from its bytes, as fetched from a location (the
 * page's location for its links, scripts and images).
 */
int
page_load_bytes(
	struct page *page,
	const unsigned char *bytes,
	size_t length,
	const char *location,
	const char *mime,
	size_t mime_length)
{
	int error;

	/* The location. */
	free(page->base);
	page->base = strdup(location);
	if (page->base == NULL)
		return ENOMEM;

	/* The document. */
	error = page_load_document(page, bytes, length, mime, mime_length);
	if (error != 0)
		return error;

	/* Succeeded: the page is loaded. */
	return 0;
}

/*
 * Starts loading a document from its bytes as page_load_bytes does, up to
 * where an HTML document would wait for its style sheets: *finish says
 * that page_load_finish is left to do (when page_sheets_blocking says none
 * is on its way), or that the page is loaded (another kind of document).
 */
int
page_load_begin(
	struct page *page,
	const unsigned char *bytes,
	size_t length,
	const char *location,
	const char *mime,
	size_t mime_length,
	int *finish)
{
	enum dom_document_content content;
	int error;

	/* The location. */
	*finish = 0;
	free(page->base);
	page->base = strdup(location);
	if (page->base == NULL)
		return ENOMEM;

	/* An XHTML document is loaded at once. */
	if (mime != NULL) {
		error = page_document_content(mime, mime_length, &content);
		if (error == 0 && content == DOM_CONTENT_XHTML)
			return page_load_xhtml(page, bytes, length);
	}

	/* An HTML document is parsed, and the sheets it links so far start loading; the rest waits for them. */
	error = page_parse_html(page, bytes, length);
	if (error == 0)
		error = page_update_styles(page);
	if (error != 0)
		return error;

	/* Succeeded: the rest is the caller's to finish. */
	*finish = 1;
	return 0;
}

/*
 * Loads an HTML document from a file.
 */
int
page_load_file(
	struct page *page,
	const char *path)
{
	struct wb_buffer buffer;
	struct wb_buffer full;
	int error;

	/* The page's scripts and images find their files from the page's, by its absolute path. */
	free(page->base);
	wb_buffer_init(&full);
	error = wb_os_full_path(path, &full);
	if (error == 0) {
		page->base = strdup(wb_buffer_string(&full));
	} else {
		/* A path the system cannot resolve stays as it was given. */
		page->base = strdup(path);
	}
	wb_buffer_release(&full);
	if (page->base == NULL)
		return ENOMEM;

	/* Reads the file. */
	wb_buffer_init(&buffer);
	error = wb_file_read(path, &buffer);
	if (error != 0) {
		wb_buffer_release(&buffer);
		return error;
	}

	/* Loads its bytes. */
	if (page_file_xhtml(path))
		error = page_load_xhtml(page, buffer.data, buffer.length);
	else
		error = page_load_html(page, buffer.data, buffer.length);
	wb_buffer_release(&buffer);
	if (error != 0)
		return error;

	/* Succeeded: the page is loaded. */
	return 0;
}

/*
 * Loads an HTML document from a location: a file's path (relative to the
 * working directory, or absolute), or a URL (file:, data: or http:).
 * The page's location becomes the file's path, or the URL after its
 * redirects.
 */
int
page_load_location(
	struct page *page,
	const char *location)
{
	struct net_url url;
	struct net_response response;
	struct wb_buffer where;
	int local;
	int error;

	/* An absolute path of the system's is a file's (a drive's path, C:\x, would parse as a URL). */
	local = wb_os_path_is_absolute(location);
	if (local) {
		error = page_load_file(page, location);
		return error;
	}

	/* What is not a URL is a file's path. */
	error = net_url_parse(location, strlen(location), NULL, &url);
	if (error == EINVAL) {
		error = page_load_file(page, location);
		return error;
	}

	/* A URL that cannot be parsed for another reason (memory). */
	if (error != 0)
		return error;
	net_url_release(&url);

	/* The document's bytes and its final URL. */
	wb_buffer_init(&where);
	error = page_fetch_response_now(page, location, NET_FETCH_DOCUMENT, &response);

	/* The page's location: a file's path, or the final URL. */
	if (error == 0)
		error = page_resolve_location(wb_buffer_string(&response.url), wb_buffer_string(&response.url), &where);
	free(page->base);
	page->base = NULL;
	if (error == 0) {
		page->base = strdup(wb_buffer_string(&where));
		if (page->base == NULL)
			error = ENOMEM;
	}

	/* Why the fetch failed over TLS, for the load callback. */
	snprintf(page->failure_reason, sizeof(page->failure_reason), "%s", response.reason);

	/* The document (and whether it may be kept for Back and Forward). */
	page->no_store = response.no_store;
	if (error == 0)
		error = page_load_document(page, response.body.data, response.body.length,
		wb_buffer_string(&response.content_type), response.content_type.length);
	net_response_release(&response);
	wb_buffer_release(&where);
	if (error != 0)
		return error;

	/* Succeeded: the page is loaded. */
	return 0;
}

/*
 * Tells the page's scripts the size of the viewport the page is shown in
 * (before the page is laid out at it).
 */
void
page_set_viewport(
	struct page *page,
	int width,
	int height)
{
	/* The size a script's question lays the page out at, and the window object's. */
	page->viewport_width = width;
	page->viewport_height = height;
	bind_window_set_viewport(page->window, width, height);
}

/*
 * Tells the page its view's color scheme (dark nonzero for the dark one):
 * its media queries and its scripts' matchMedia see it, and the styles
 * computed in the other scheme are forgotten.
 */
void
page_set_color_scheme(
	struct page *page,
	int dark)
{
	/* The scheme as 0 or 1, the window object's too. */
	page->dark = 0;
	if (dark)
		page->dark = 1;
	bind_window_set_color_scheme(page->window, page->dark);

	/* The engines made already test the new scheme. */
	if (page->css != NULL)
		css_engine_set_color_scheme(page->css, page->dark);
	if (page->query_css != NULL)
		css_engine_set_color_scheme(page->query_css, page->dark);
}

/*
 * Tells the page the fonts its view draws with, which a script's question
 * about the geometry lays the page out with (the paths must outlive the
 * page).
 */
void
page_set_fonts(
	struct page *page,
	const struct text_font_paths *paths)
{
	/* The view's paths. */
	page->font_paths = paths;
}

/*
 * Gives the page the companions of its sans font (the view's, which
 * outlive the page; NULL for the defaults of the data directory).
 */
void
page_set_font_companions(
	struct page *page,
	const struct text_font_companions *companions)
{
	/* The view's companions. */
	page->font_companions = companions;
}

/*
 * Makes the page's animation frames wait for the presents of a drawing
 * thread (page_frame_presented) instead of coming by the clock.
 */
void
page_set_frames_presented(
	struct page *page,
	int presented)
{
	/* The presents decide from now on. */
	page->frames_presented = presented;
}

/* Counts a present of the drawing thread: the animation frames asked for before it are due. */
void
page_frame_presented(
	struct page *page)
{
	/* One chance more. */
	page->frames++;
}

/*
 * Reports how many presents the page (the context) had, when its
 * animation frames wait for them (bind_host.frame_count): 1 with the
 * count, or 0 when they come by the clock.
 */
int
page_frame_count(
	void *context,
	uint64_t *count)
{
	struct page *page;

	/* Frames by the clock have no count. */
	page = context;
	*count = page->frames;
	if (!page->frames_presented)
		return 0;

	/* Succeeded: the frames are the presents. */
	return 1;
}

/*
 * Tells the page how far its view is scrolled, in CSS pixels (the
 * scripts' scrollX, scrollY and client rectangles).
 */
void
page_set_scroll(
	struct page *page,
	double x,
	double y)
{
	/* The two distances. */
	page->scroll_x = x;
	page->scroll_y = y;
}

/*
 * Sends the page's console (its scripts' console.log and the like) to a
 * function of the caller's.
 */
void
page_set_console(
	struct page *page,
	page_console console,
	void *context)
{
	/* The function and what it gets back. */
	page->console = console;
	page->console_context = context;
}

/*
 * Describes why the page's last load of a location failed beyond its error
 * number (the TLS verification's reason, for an https page); empty when
 * there is nothing more to say.
 */
const char *
page_failure_reason(
	const struct page *page)
{
	/* The reason the fetch kept. */
	return page->failure_reason;
}

/*
 * Starts the fetches of the style sheets (and, once they have come, the web
 * fonts) and the images the document names, ahead of the layout that would.
 */
int
page_load_resources(
	struct page *page)
{
	int error;

	/* The sheets the document links, and the images it shows. */
	error = page_gather_styles(page);
	if (error != 0)
		return error;
	error = page_load_images(page);
	if (error != 0)
		return error;

	/* Succeeded: every resource the document names is fetched or on its way. */
	return 0;
}

/*
 * Tells whether a frame, a style sheet, a web font or an image of the
 * document is still on its way (the window's load waits for them).
 */
int
page_load_waiting(
	const struct page *page)
{
	/* The frames, of the page's agent and of others. */
	if (page->frame_loads.count != 0 || page->remote_loads != 0)
		return 1;

	/* The sheets, the fonts and the images. */
	if (page_sheets_pending(page))
		return 1;
	if (page_fonts_pending(page))
		return 1;
	if (page_images_pending(page))
		return 1;

	/* None. */
	return 0;
}

/*
 * Opens the fonts the page's text is drawn with.
 */
int
page_open_fonts(
	struct page *page,
	const struct text_font_paths *paths)
{
	int error;

	/* Opens them once, with the view's companions of the sans font (or the defaults). */
	if (page->text_open)
		return 0;
	if (page->font_companions != NULL) {
		error = text_system_open_with(&page->text, paths, page->font_companions);
	} else {
		error = text_system_open(&page->text, paths);
	}
	if (error != 0)
		return error;

	/* Succeeded: the fonts are open. */
	page->text_open = 1;
	return 0;
}

/*
 * Lays the page out for a viewport of width by height pixels (the fonts
 * must be open).  The styles, the fetches the document names and the
 * events are the page's thread's; the box tree is built on the style
 * thread (page_layout_work, lent).
 */
int
page_layout(
	struct page *page,
	int width,
	int height)
{
	struct page_layout_args args;
	int error;
	uint32_t old_laid_out_images;

	/* A failed rebuild must leave the prior complete layout's image generation intact. */
	old_laid_out_images = page->laid_out_images;

	/* Throws the old display list and layout away. */
	if (page->painted) {
		paint_release(&page->paint);
		page->painted = 0;
	}

	/*
	 * The layout: the old one is kept while the new one is made, for the
	 * sizes of the query containers of the container-relative units
	 *.
	 */
	if (page->laid_out) {
		page->previous_layout = page->layout;
		page->previous_laid_out = 1;
		memset(&page->layout, 0, sizeof(page->layout));
		page->laid_out = 0;
	}

	/* The style sheets again when the document or its sheets changed since they were gathered. */
	error = page_update_styles(page);
	if (error != 0)
		goto cleanup;

	/* The window's size for the scripts, and the size their questions lay the page out at. */
	page->viewport_width = width;
	page->viewport_height = height;
	bind_window_set_viewport(page->window, width, height);

	/* The images the document names, fetched and decoded when they are new. */
	error = page_load_images(page);
	if (error != 0)
		goto cleanup;

	/* The media the document names, started when they are new. */
	error = page_load_media(page);
	if (error != 0)
		goto cleanup;

	/* The box tree, built on the style thread with the fonts that arrived, the images there are now and the styles gathered last. */
	args.page = page;
	args.width = width;
	args.height = height;
	error = page_lend(page, page_layout_work, &args);
	if (error != 0)
		goto cleanup;

	/* The ready promise may allocate; retain the prior layout if it fails. */
	if (!page_sheets_pending(page) && !page_fonts_pending(page)) {
		error = bind_document_fonts_settle(page->document);
		if (error != 0)
			goto cleanup;
	}

	/* The old layout is no longer needed. */
	if (page->previous_laid_out) {
		layout_release(&page->previous_layout);
		page->previous_laid_out = 0;
	}

	/* Only a complete layout advances observable generation and geometry caches. */
	page->laid_out_generation = page_generation(page);
	page->laid_out_frames = page_frames_generation_count(page, &page->laid_out_iframes);
	page_animations_sync(page);
	page->laid_out_transition = page->transition_generation;
	page->laid_out_visits = page->visits_generation;
	page->layout_serial++;

	/* The changes recorded so far are in this layout (a style pass has taken the style ones already). */
	dom_changes_clear_layout(page->document);

	/* What it was made with, for the next one to take content from it. */
	page->reuse_engine = page->css;
	page->reuse_gathered = page->gathered;
	page->reuse_images = page->images_generation;
	page->reuse_fonts = page->fonts_generation;
	page->reuse_visits = page->visits_generation;
	page->reuse_transition = page->transition_generation;
	page->reuse_valid = !page->transition_on;

	/* Succeeded: the page is laid out. */
	return 0;

cleanup:
	/* A layout that took boxes of the last one leaves neither of them whole: both go. */
	if (page->laid_out && page->layout.reuse_boxes != 0) {
		layout_release(&page->layout);
		page->laid_out = 0;
		if (page->previous_laid_out) {
			layout_release(&page->previous_layout);
			page->previous_laid_out = 0;
		}
	}

	/* Discard a partly built layout and restore the last complete one, if any. */
	if (page->laid_out) {
		layout_release(&page->layout);
		page->laid_out = 0;
	}

	/* The last complete layout remains available after a failed rebuild. */
	if (page->previous_laid_out) {
		page->layout = page->previous_layout;
		page->laid_out = 1;
		page->previous_laid_out = 0;
		memset(&page->previous_layout, 0, sizeof(page->previous_layout));
	}

	/* Failed layout attempts do not consume a new image generation. */
	page->laid_out_images = old_laid_out_images;
	return error;
}

/*
 * Runs a work that runs no script with the page's heap and document lent
 * to the style thread of its view (on this thread for a view of the
 * caller's), then fetches the images the work met new.  Reports the work's
 * outcome.
 */
int
page_lend(
	struct page *page,
	vm_lend_work work,
	void *argument)
{
	int borrower;
	int error;

	/* The work, on the style thread while this one waits. */
	error = vm_heap_lend(page->heap, work, argument);

	/* The page's thread starts the fetches the style thread did not (a lend inside a lend leaves them to the outer one). */
	borrower = vm_thread_is_borrower();
	if (!borrower)
		(void)page_images_fetch_deferred(page);
	if (error != 0)
		return error;

	/* Succeeded: the work ran. */
	return 0;
}

/*
 * Lays a document out into a tree of the caller's with an engine at a
 * size and a scroll (a transition given is the one the tree is built
 * with), on the style thread.  Reports 0 or layout_build's error (the tree
 * is the caller's to release either way).
 */
int
page_lend_layout(
	struct page *page,
	struct layout_tree *tree,
	struct css_engine *engine,
	struct dom_document *document,
	int width,
	int height,
	layout_unit scroll_x,
	layout_unit scroll_y,
	const struct layout_transition *transition)
{
	struct page_tree_args args;
	int error;

	/* The tree's arguments, lent. */
	args.page = page;
	args.tree = tree;
	args.engine = engine;
	args.document = document;
	args.width = width;
	args.height = height;
	args.scroll_x = scroll_x;
	args.scroll_y = scroll_y;
	args.transition = transition;
	error = page_lend(page, page_tree_work, &args);
	if (error != 0)
		return error;

	/* Succeeded: the tree is laid out. */
	return 0;
}

/* Paints a laid out tree into a list of the caller's, on the style thread; reports 0 or paint_build's error. */
int
page_lend_paint(
	struct page *page,
	struct paint_list *list,
	const struct layout_tree *tree)
{
	struct page_paint_args args;
	int error;

	/* The list's arguments, lent. */
	args.list = list;
	args.tree = tree;
	error = page_lend(page, page_list_work, &args);
	if (error != 0)
		return error;

	/* Succeeded: the list is painted. */
	return 0;
}

/*
 * Builds the page's box tree at a size, on the style thread (a work lent
 * by page_layout): the web fonts that arrived join the text system, the
 * tree is built and built again while the query containers' sizes move,
 * and the loaded frames' windows get their iframes' sizes.  page->laid_out
 * says whether a tree was built (page_layout's cleanup releases it on a
 * failure).
 */
static int
page_layout_work(
	void *argument)
{
	struct page_layout_args *args;
	struct layout_tree *previous;
	struct page *page;
	int missed;
	int pass;
	int error;

	/* The web fonts that arrived join the text system. */
	args = argument;
	page = args->page;
	page_fonts_install(page);

	/* Builds and lays out the box tree, the images found by their elements (the ones there are now), with the styles gathered last. */
	previous = page_layout_previous(page);
	page->laid_out_images = page->images_generation;
	page->laid_out_gathered = page->gathered;
	css_engine_set_container_lookup(page->css, page_container_size, page);
	page->cache.layouts++;
	error = layout_build_reusing(&page->layout, previous, page_cache_mode() != PAGE_CACHE_OFF, page->css, &page->text, page->document, page_image_of, page_image_by_url, page, args->width, args->height, layout_from_px((float)page->scroll_x), layout_from_px((float)page->scroll_y), page->transition_on ? &page->transition : NULL);
	page->laid_out = 1;
	page->layout.image_variant = page_image_variant;
	page->layout.frame_paint = page_frame_paint;
	page->layout.frame_remote = page_frame_remote;
	page_layout_count(page);
	if (error != 0)
		return error;

	/*
	 * A query container the old layout did not have (the first layout), or
	 * one this layout gave another size than the styles used: the page is
	 * laid out again with the sizes it has now (twice more at most).
	 */
	for (pass = 0; pass < PAGE_CONTAINER_PASSES; pass++) {
		missed = css_engine_container_missed(page->css);
		if (!missed)
			missed = page_containers_moved(page);
		if (!missed)
			break;

		/* This layout is the one the sizes come from now. */
		if (page->previous_laid_out)
			layout_release(&page->previous_layout);
		page->previous_layout = page->layout;
		page->previous_laid_out = 1;
		memset(&page->layout, 0, sizeof(page->layout));
		css_engine_forget_styles(page->css);
		page->cache.layouts++;
		error = layout_build_reusing(&page->layout, NULL, page_cache_mode() != PAGE_CACHE_OFF, page->css, &page->text, page->document, page_image_of, page_image_by_url, page, args->width, args->height, layout_from_px((float)page->scroll_x), layout_from_px((float)page->scroll_y), page->transition_on ? &page->transition : NULL);
		page->layout.image_variant = page_image_variant;
		page->layout.frame_paint = page_frame_paint;
		page->layout.frame_remote = page_frame_remote;
		page_layout_count(page);
		if (error != 0)
			return error;
	}

	/* With the cache being verified, the tree is compared with one made whole, and the whole one is used. */
	if (page_cache_mode() == PAGE_CACHE_VERIFY) {
		error = page_layout_verify(page, args);
		if (error != 0)
			return error;
	}

	/* The loaded frames' windows have their iframes' sizes. */
	page_frames_size(page);

	/* Succeeded: the tree is built. */
	return 0;
}

/*
 * Gives the tree a layout may take the boxes of its blocks from: the last
 * complete one, when it was laid out for the style engine, sheets, fonts,
 * images, links and view transition the page has now, and the cache is on;
 * otherwise NULL (the page is laid out whole).
 */
static struct layout_tree *
page_layout_previous(
	struct page *page)
{
	if (page_cache_mode() == PAGE_CACHE_OFF || !page->reuse_valid || !page->previous_laid_out)
		return NULL;
	if (page->css != page->reuse_engine || page->gathered != page->reuse_gathered)
		return NULL;
	if (page->images_generation != page->reuse_images || page->fonts_generation != page->reuse_fonts)
		return NULL;
	if (page->visits_generation != page->reuse_visits || page->transition_on || page->transition_generation != page->reuse_transition)
		return NULL;

	/* A page whose styles ask for the sizes of query containers is laid out whole (the sizes are read from the previous tree). */
	if (css_engine_container_lookups(page->css) != 0U)
		return NULL;
	return &page->previous_layout;
}

/* Adds what the layout just built took from the previous tree to the page's counts. */
static void
page_layout_count(
	struct page *page)
{
	page->cache.roots += page->layout.reuse_roots;
	page->cache.grafts += page->layout.reuse_grafts;
	page->cache.graft_boxes += page->layout.reuse_boxes;
}

/*
 * Makes an engine for the page as the sheets and the document are now,
 * with nothing kept: what page_gather_styles gives page->css, made apart
 * (page->css stays what it was).
 */
static int
page_engine_fresh(
	struct page *page,
	struct css_engine **made)
{
	struct css_engine *saved;
	int error;

	/* The sheets are added to page->css, so it stands in the new engine for the time they are. */
	saved = page->css;
	page->css = NULL;
	error = css_engine_create(&page->css, page->heap);
	if (error == 0) {
		if (page->visited_urls.count != 0)
			css_engine_set_visited_lookup(page->css, page_link_visited, page);
		css_engine_set_view_transition(page->css, page->vt_active, page->vt_types, page->vt_type_count);
		css_engine_set_color_scheme(page->css, page->dark);
		css_engine_set_container_lookup(page->css, page_container_size, page);
		error = page_sheets_add(page);
	}
	*made = page->css;
	page->css = saved;
	return error;
}

/*
 * Lays the document out again whole, with an engine that kept nothing, into a
 * tree of its own, and compares its dump with the page's tree; a difference
 * is told on standard error (the first line that differs, and the two
 * lines), counted, and the whole tree (with its engine) becomes the page's.
 */
static int
page_layout_verify(
	struct page *page,
	const struct page_layout_args *args)
{
	struct layout_tree whole;
	struct css_engine *fresh;
	struct css_engine *live;
	struct wb_buffer ours;
	struct wb_buffer theirs;
	const char *a;
	const char *b;
	const char *a_end;
	const char *b_end;
	unsigned line;
	int differs;
	int error;

	/* The engine that kept nothing, and the whole tree built with it as the page's was. */
	fresh = NULL;
	error = page_engine_fresh(page, &fresh);
	if (error != 0) {
		css_engine_destroy(fresh);
		return error;
	}
	live = page->css;
	page->css = fresh;
	memset(&whole, 0, sizeof(whole));
	error = layout_build(&whole, page->css, &page->text, page->document, page_image_of, page_image_by_url, page, args->width, args->height,
	    layout_from_px((float)page->scroll_x), layout_from_px((float)page->scroll_y), page->transition_on ? &page->transition : NULL);
	page->css = live;
	if (error != 0) {
		layout_release(&whole);
		css_engine_destroy(fresh);
		return error;
	}

	/* The two dumps. */
	wb_buffer_init(&ours);
	wb_buffer_init(&theirs);
	error = layout_dump(&page->layout, &ours);
	if (error == 0)
		error = layout_dump(&whole, &theirs);
	if (error != 0) {
		wb_buffer_release(&ours);
		wb_buffer_release(&theirs);
		layout_release(&whole);
		css_engine_destroy(fresh);
		return error;
	}

	/* The first line that differs. */
	a = ours.data == NULL ? "" : wb_buffer_string(&ours);
	b = theirs.data == NULL ? "" : wb_buffer_string(&theirs);
	differs = 0;
	for (line = 1; ; line++) {
		a_end = strchr(a, '\n');
		b_end = strchr(b, '\n');
		if (a_end == NULL)
			a_end = a + strlen(a);
		if (b_end == NULL)
			b_end = b + strlen(b);
		if (a_end - a != b_end - b || memcmp(a, b, (size_t)(a_end - a)) != 0) {
			differs = 1;
			break;
		}
		if (*a_end == '\0')
			break;
		a = a_end + 1;
		b = b_end + 1;
	}
	if (differs) {
		page->cache.mismatches++;
		fprintf(stderr, "layout-cache: layout mismatch line=%u\n  cached: %.*s\n  whole:  %.*s\n", line, (int)(a_end - a), a, (int)(b_end - b), b);
	}
	wb_buffer_release(&ours);
	wb_buffer_release(&theirs);

	/* The whole tree and its engine are the page's from now on (a difference), or go (the same). */
	if (differs) {
		page_cache_fold(page);
		layout_release(&page->layout);
		page->layout = whole;
		page->layout.image_variant = page_image_variant;
		page->layout.frame_paint = page_frame_paint;
		page->layout.frame_remote = page_frame_remote;
		css_engine_destroy(page->css);
		page->css = fresh;
	} else {
		layout_release(&whole);
		css_engine_destroy(fresh);
	}

	/* Succeeded: the page's tree is the whole one's equal. */
	return 0;
}

/* Lays a document out into a tree (a work lent by page_lend_layout). */
static int
page_tree_work(
	void *argument)
{
	struct page_tree_args *args;
	int error;

	/* The tree, with the page's text, images and frames. */
	args = argument;
	error = layout_build(args->tree, args->engine, &args->page->text, args->document, page_image_of, page_image_by_url, args->page, args->width, args->height,
	    args->scroll_x, args->scroll_y, args->transition);
	if (error != 0)
		return error;

	/* Succeeded: the tree is laid out. */
	return 0;
}

/* Paints a tree into a list (a work lent by page_lend_paint). */
static int
page_list_work(
	void *argument)
{
	struct page_paint_args *args;
	int error;

	/* The list. */
	args = argument;
	error = paint_build(args->list, args->tree);
	if (error != 0)
		return error;

	/* Succeeded: the list is painted. */
	return 0;
}

/* Tells the prelude which boxes have a running animation (a name with @keyframes), for the animation events. */
static void
page_animations_walk(
	struct page *page,
	const struct layout_box *box,
	int depth)
{
	const struct layout_box *child;

	if (depth > LAYOUT_DEPTH_MAX)
		return;
	for (child = box; child != NULL; child = child->next) {
		if (child->node != NULL && child->node->type == DOM_ELEMENT && !child->vt_tree && child->style.animation_name != NULL &&
		    child->continuation == 0 && child->style.animation_duration > 0.0f &&
		    css_engine_has_keyframes(page->css, child->style.animation_name))
			bind_animation_sync(page->window, 1, (struct dom_element *)child->node, child->style.animation_name,
			    (double)child->style.animation_delay * 1000.0, (double)child->style.animation_duration * 1000.0,
			    (double)child->style.animation_iterations);
		page_animations_walk(page, child->first_child, depth + 1);
	}
}

/* Starts and stops the animation events for the elements the layout shows animated. */
static void
page_animations_sync(
	struct page *page)
{
	if (page->css == NULL || page->layout.root == NULL)
		return;
	bind_animation_sync(page->window, 0, NULL, NULL, 0.0, 0.0, 0.0);
	page_animations_walk(page, page->layout.root, 0);
	bind_animation_sync(page->window, 2, NULL, NULL, 0.0, 0.0, 0.0);
}

/*
 * Moves the sticky boxes of the layout for the page's scroll; reports
 * whether any moved (the display list is then built again).
 */
int
page_sticky_sync(
	struct page *page,
	layout_unit scroll_y)
{
	struct page_sticky_args args;
	layout_unit scroll_x;
	int moved;

	/* Only a laid out page with sticky boxes has any. */
	if (!page->laid_out)
		return 0;
	if (page->layout.sticky_count == 0)
		return 0;

	/* The same scroll leaves them where they are (read here: the style thread writes the tree only while it is lent). */
	scroll_x = layout_from_px((float)page->scroll_x);
	if (page->layout.sticky_scroll == scroll_y && page->layout.sticky_scroll_x == scroll_x)
		return 0;

	/* The boxes moved on the style thread, which owns the tree. */
	args.page = page;
	args.scroll_y = scroll_y;
	moved = page_lend(page, page_sticky_work, &args);

	/* Whether any moved. */
	return moved;
}

/* Moves the page's sticky boxes for a scroll (a work lent by page_sticky_sync); reports whether any moved. */
static int
page_sticky_work(
	void *argument)
{
	struct page_sticky_args *args;
	struct page *page;
	int moved;

	/* The boxes, for the page's scroll across and the one given down. */
	args = argument;
	page = args->page;
	moved = layout_sticky_apply(&page->layout, layout_from_px((float)page->scroll_x), args->scroll_y);

	/* Whether any moved. */
	return moved;
}

/*
 * Builds the display list of the laid out page.
 */
int
page_paint(
	struct page *page)
{
	int error;

	/* A page that is not laid out has nothing to paint. */
	if (!page->laid_out)
		return EINVAL;

	/* Throws the old display list away. */
	if (page->painted) {
		paint_release(&page->paint);
		page->painted = 0;
	}

	/* The new one, made on the style thread. */
	error = page_lend(page, page_paint_work, page);
	if (error != 0)
		return error;

	/* Succeeded: the page has its display list. */
	return 0;
}

/*
 * Builds the display list of the laid out page with its focus ring and
 * caret (a work lent by page_paint).
 */
static int
page_paint_work(
	void *argument)
{
	struct page *page;
	int error;

	/* Walks the layout into a new list. */
	page = argument;
	error = paint_build(&page->paint, &page->layout);
	if (error != 0)
		return error;
	page->painted = 1;
	page->paint_generation = __atomic_add_fetch(&page_paint_next, 1U, __ATOMIC_RELAXED);
	page->paint_taken = 0;
	page->painted_media = page->media_generation;

	/* The focus ring over it, when the keyboard put the focus somewhere. */
	error = page_paint_focus(page);
	if (error != 0)
		return error;

	/* The caret of a focused text control. */
	error = page_paint_caret(page);
	if (error != 0)
		return error;

	/* Succeeded: the page has its display list. */
	return 0;
}

/*
 * Writes the document's title (its first <title> element's text, with
 * whitespace collapsed and trimmed) as UTF-8; empty when there is none.
 */
int
page_title(
	const struct page *page,
	struct wb_buffer *out)
{
	const struct dom_node *title;
	struct wb_units text;
	struct wb_units collapsed;
	uint16_t unit;
	size_t index;
	int space;
	int error;

	/* The first <title> in the document. */
	title = page_find_title(&page->document->node, 0);
	if (title == NULL)
		return 0;

	/* Its text. */
	wb_units_init(&text);
	wb_units_init(&collapsed);
	error = page_text_of(title, &text);

	/* Each run of whitespace becomes one space, and none at either end. */
	space = 0;
	for (index = 0; error == 0 && index < text.length; index++) {
		unit = text.data[index];

		/* A whitespace character is remembered until something follows it. */
		if (unit == 0x20U ||
		    unit == 0x09U ||
		    unit == 0x0aU ||
		    unit == 0x0cU ||
		    unit == 0x0dU) {
			space = 1;
			continue;
		}

		/* The space before a character, when there is text before it. */
		if (space && collapsed.length != 0)
			error = wb_units_append_code_point(&collapsed, 0x20U);
		space = 0;

		/* The character itself. */
		if (error == 0)
			error = wb_units_append(&collapsed, &text.data[index], 1);
	}

	/* The UTF-8 of the collapsed text. */
	if (error == 0)
		error = wb_units_to_utf8(collapsed.data, collapsed.length, out);
	wb_units_release(&text);
	wb_units_release(&collapsed);
	if (error != 0)
		return error;

	/* Succeeded: the title is written. */
	return 0;
}

/*
 * Writes the document tree in html5lib's test format.
 */
int
page_dump_dom(
	const struct page *page,
	struct wb_buffer *out)
{
	int error;

	/* Dumps the document's children. */
	error = page_dump_node(&page->document->node, 0, out);
	if (error != 0)
		return error;

	/* Succeeded: the tree is in the buffer. */
	return 0;
}

/*
 * Writes every element's computed style, one line each, indented by depth.
 */
int
page_dump_style(
	struct page *page,
	struct wb_buffer *out)
{
	struct dom_node *node;
	int error;

	/* The style sheets as the document and its fetched sheets are now. */
	error = page_update_styles(page);
	if (error != 0)
		return error;

	/* Styles from each element child of the document. */
	for (node = page->document->node.first_child; node != NULL; node = node->next) {
		if (node->type != DOM_ELEMENT)
			continue;
		error = page_dump_style_node(page, (struct dom_element *)node, NULL, 0, out);
		if (error != 0)
			return error;
	}

	/* Succeeded: the styles are in the buffer. */
	return 0;
}

/*
 * Tells whether a query container the styles used has another size (by
 * more than half a pixel) in the page's layout, or none.
 */
static int
page_containers_moved(
	const struct page *page)
{
	const struct dom_element *container;
	const struct layout_box *box;
	float width;
	float height;
	float difference;
	size_t count;
	size_t index;

	/* Each container the engine used. */
	count = css_engine_container_uses(page->css);
	for (index = 0; index < count; index++) {
		css_engine_container_use(page->css, index, &container, &width, &height);
		box = layout_box_of(&page->layout, &container->node);
		if (box == NULL)
			return 1;

		/* Its width or its height moved. */
		difference = layout_to_px(box->width) - width;
		if (difference > 0.5f || difference < -0.5f)
			return 1;
		difference = layout_to_px(box->height) - height;
		if (difference > 0.5f || difference < -0.5f)
			return 1;
	}

	/* Every container is as the styles used it. */
	return 0;
}

/*
 * Finds a query container's content box size in the page's previous
 * layout, for the container-relative units of the style
 * engine; 0 when that layout did not have the container.
 */
static int
page_container_size(
	void *context,
	const struct dom_element *container,
	float *width,
	float *height)
{
	const struct layout_box *box;
	struct page *page;

	/* The previous layout's box of the container. */
	page = context;
	if (!page->previous_laid_out)
		return 0;
	box = layout_box_of(&page->previous_layout, &container->node);
	if (box == NULL)
		return 0;

	/* Its content box. */
	*width = layout_to_px(box->width);
	*height = layout_to_px(box->height);
	return 1;
}

/*
 * Tells how the cache of style and layout works for this process: the
 * value of BROWSER_LAYOUT_CACHE ("off", "on" or "verify"), read once; any
 * other value, and none, is on.
 */
enum page_cache_mode
page_cache_mode(
	void)
{
	static int mode = -1;
	const char *value;
	int made;

	/* Read once (a race writes the same value). */
	made = __atomic_load_n(&mode, __ATOMIC_RELAXED);
	if (made >= 0)
		return (enum page_cache_mode)made;
	made = PAGE_CACHE_ON;
	value = getenv("BROWSER_LAYOUT_CACHE");
	if (value != NULL && strcmp(value, "off") == 0)
		made = PAGE_CACHE_OFF;
	else if (value != NULL && strcmp(value, "verify") == 0)
		made = PAGE_CACHE_VERIFY;
	__atomic_store_n(&mode, made, __ATOMIC_RELAXED);
	return (enum page_cache_mode)made;
}

/* Adds what the page's style engine counted to the page's counts (before the engine goes). */
void
page_cache_fold(
	struct page *page)
{
	uint64_t computed;
	uint64_t hits;
	uint64_t dropped;

	/* Without an engine there is nothing to add. */
	if (page->css == NULL)
		return;
	css_engine_counts(page->css, &computed, &hits, &dropped);
	page->cache.styles += computed;
	page->cache.style_hits += hits;
	page->cache.invalidated += dropped;
}

/* Tells on standard error what the cache did for the page, in one line, when BROWSER_LAYOUT_CACHE_STATS is 1. */
void
page_cache_report(
	struct page *page)
{
	static const char *const names[] = { "off", "on", "verify" };
	const char *value;

	/* Asked for only. */
	value = getenv("BROWSER_LAYOUT_CACHE_STATS");
	if (value == NULL || strcmp(value, "1") != 0)
		return;

	/* The styles of the engine that is still there too. */
	page_cache_fold(page);
	fprintf(stderr, "layout-cache: mode=%s engines=%llu styles=%llu style_hits=%llu invalidated=%llu layouts=%llu roots=%llu grafts=%llu graft_boxes=%llu intrinsic=%llu mismatches=%llu\n",
	    names[page_cache_mode()], (unsigned long long)page->cache.engines, (unsigned long long)page->cache.styles,
	    (unsigned long long)page->cache.style_hits, (unsigned long long)page->cache.invalidated, (unsigned long long)page->cache.layouts,
	    (unsigned long long)page->cache.roots, (unsigned long long)page->cache.grafts, (unsigned long long)page->cache.graft_boxes,
	    (unsigned long long)page->cache.intrinsic, (unsigned long long)page->cache.mismatches);
}

/*
 * Reports the document's generation without the changes no style can see
 * (data-* attributes that no style sheet names), which tells whether the
 * page's style and layout are out of date.
 */
uint32_t
page_generation(
	const struct page *page)
{
	/* The difference wraps with the two counts. */
	return page->document->generation - page->document->quiet_generation;
}

/*
 * Tells the document whether one of the page's style sheets may name an
 * attribute (a page not styled yet counts every attribute as named).
 */
static int
page_attribute_styled(
	void *context,
	const struct vm_string *name)
{
	struct page *page;
	int named;

	/* A page without its styles cannot tell. */
	page = context;
	if (page->css == NULL)
		return 1;

	/* The sheets of its engine. */
	named = css_engine_names_attribute(page->css, name);
	return named;
}

/*
 * Gathers the style sheets again when a script changed the document, or
 * a fetched sheet arrived, since they were last gathered.
 */
int
page_update_styles(
	struct page *page)
{
	int pending;
	int error;

	/* Styles that are up to date stay. */
	if (page->css != NULL &&
	    page->styled_generation == page_generation(page) &&
	    page->styled_visits == page->visits_generation &&
	    page->styled_sheets == page->sheets_generation)
		return 0;

	/*
	 * So do styles of the same document while more sheets are on their way:
	 * the page is styled again once they are all here.
	 */
	pending = page_sheets_pending(page);
	if (page->css != NULL && page->styled_generation == page_generation(page) &&
	    page->styled_visits == page->visits_generation && pending)
		return 0;

	/* The styles the engine kept, less what the changes since reach (the cache of style and layout). */
	if (page_styles_reusable(page)) {
		(void)dom_changes_take_style(page->document, page_style_visit, page);
		page->styled_generation = page_generation(page);
		return 0;
	}

	/* A new engine with the sheets as they are now. */
	error = page_gather_styles(page);
	if (error != 0)
		return error;

	/* Succeeded: the styles match the document. */
	return 0;
}

/*
 * Tells whether the engine's kept styles can be used after the changes since
 * they were made, the ones the changes reach dropped (the cache is on, no
 * sheet may have changed, nothing asks for everything, and the engine has not
 * kept too much).
 */
static int
page_styles_reusable(
	const struct page *page)
{
	/* Only with the cache on, and an engine that was made for this document's sheets. */
	if (page_cache_mode() == PAGE_CACHE_OFF || page->css == NULL)
		return 0;
	if (page->styled_visits != page->visits_generation || page->styled_sheets != page->sheets_generation)
		return 0;

	/* The changes since: no sheet may be new or gone, and none may ask for every style. */
	if (dom_change_needs_sheets(page->document) || page->document->change_full_style != 0U)
		return 0;

	/* An engine that kept a lot is made again (what custom properties took of its arena stays until it goes). */
	if (css_engine_arena_bytes(page->css) > PAGE_STYLE_ARENA_MAX || css_engine_kept_styles(page->css) > PAGE_STYLE_KEPT_MAX)
		return 0;
	return 1;
}

/* Drops the styles the engine kept for what a changed node reaches (a style bit of the node means its subtree: the children inherit). */
static int
page_style_visit(
	void *context,
	struct dom_node *node,
	unsigned bits)
{
	struct page *page;
	unsigned scope;

	/* The document itself changed (its children): every style. */
	page = context;
	if (node->type == DOM_DOCUMENT) {
		css_engine_invalidate(page->css, NULL, CSS_RESTYLE_ALL);
		return 0;
	}

	/* Only elements have styles. */
	if (node->type != DOM_ELEMENT)
		return 0;
	scope = CSS_RESTYLE_SUBTREE;
	if ((bits & DOM_CHANGED_STYLE_SIBLINGS) != 0U)
		scope |= CSS_RESTYLE_SIBLINGS;
	css_engine_invalidate(page->css, (struct dom_element *)node, scope);
	return 0;
}

/* Marks an element whose style came out different (or new) for a new layout of its box. */
static void
page_style_changed(
	void *context,
	struct dom_element *element)
{
	struct page *page;

	page = context;
	dom_changes_mark_layout(&element->node);
	(void)page;
}

/* Tells the document's recorder what a change of an element's attribute or state restyles, by the engine's selectors. */
static unsigned
page_change_scope(
	void *context,
	struct dom_element *element,
	int kind,
	const struct vm_string *attribute)
{
	struct page *page;

	/* Without an engine, what could be reached is everything below the element and its siblings. */
	page = context;
	if (page->css == NULL)
		return DOM_RESTYLE_SUBTREE | DOM_RESTYLE_SIBLINGS;
	return css_engine_change_scope(page->css, element, kind, attribute);
}

/* Drops the styles the engine kept for a subtree that went out of the document (its elements may be made again at the same addresses). */
static void
page_change_removed(
	void *context,
	struct dom_node *node)
{
	struct page *page;

	page = context;
	if (page->css != NULL && node->type == DOM_ELEMENT)
		css_engine_invalidate(page->css, (struct dom_element *)node, CSS_RESTYLE_SUBTREE | CSS_RESTYLE_DROP);
}

/*
 * Records a successful same-origin child navigation observed by this page.
 * The caller verifies current ownership and origin before recording the URL.
 */
int
page_record_visited(
	struct page *page,
	const struct wb_buffer *url)
{
	struct wb_buffer location;
	char **existing;
	char *copy;
	size_t index;
	int error;

	/* An empty page has no URL against which to resolve a child location. */
	if (page->base == NULL)
		return 0;

	/* Canonicalize the final response URL in the same way as an anchor's href. */
	wb_buffer_init(&location);
	error = page_resolve_location(page->base, wb_buffer_string(url), &location);
	if (error != 0) {
		wb_buffer_release(&location);
		return error;
	}

	/* A repeated navigation leaves the paint generation unchanged. */
	for (index = 0; index < page->visited_urls.count; index++) {
		existing = wb_vector_at(&page->visited_urls, index);
		if (strcmp(*existing, wb_buffer_string(&location)) == 0) {
			wb_buffer_release(&location);
			return 0;
		}
	}

	/* Keep the new URL until the page closes and require a fresh style pass. */
	copy = strdup(wb_buffer_string(&location));
	wb_buffer_release(&location);
	if (copy == NULL)
		return ENOMEM;
	error = wb_vector_push(&page->visited_urls, &copy);
	if (error != 0) {
		free(copy);
		return error;
	}
	page->visits_generation++;
	return 0;
}

/* Parses a complete XHTML resource before adding any of its nodes to the active Document. */
static int
page_load_xhtml(
	struct page *page,
	const unsigned char *bytes,
	size_t length)
{
	struct xml_document *model;
	struct dom_document *parsed;
	struct dom_node *child;
	struct vm_cell *root;
	struct xml_error failure;
	int error;

	/* Strict parsing cannot publish a partial source tree. */
	model = NULL;
	memset(&failure, 0, sizeof(failure));
	error = xml_document_parse(&model, bytes, length, &failure);
	if (error != 0)
		return error;

	/* The existing XML projection retains all namespaces and self-closing tags. */
	parsed = NULL;
	error = xml_document_project(page->heap, model, DOM_CONTENT_XHTML, &parsed);
	xml_document_destroy(model);
	if (error != 0)
		return error;

	/* The projected owner must survive adoption allocations until its last child moves. */
	root = &parsed->node.cell;
	error = vm_heap_add_root(page->heap, &root);
	if (error != 0)
		return error;

	/* Preserve the original Window and Document identity already installed on this Page. */
	page->document->content = DOM_CONTENT_XHTML;
	while (parsed->node.first_child != NULL) {
		child = parsed->node.first_child;
		error = dom_adopt(page->document, child);
		if (error != 0)
			break;
		dom_append_child(&page->document->node, child);
	}

	/* The active Document now owns every adopted child or retains the precise adoption failure. */
	vm_heap_remove_root(page->heap, &root);
	if (error != 0)
		return error;

	/* Validated connected classic inline scripts run before document completion events. */
	error = page_xhtml_inline_scripts(page);
	if (error != 0)
		return error;

	/* Only the still-active owning Window can publish completion after script callbacks. */
	if (page->window->detached)
		return 0;

	/* Document readiness follows the supported inline source, as in the child XML path. */
	error = page_fire_load(page);
	if (error != 0)
		return error;

	/* Inline mutations determine the source order seen by the ordinary style gatherer. */
	error = page_gather_styles(page);
	if (error != 0)
		return error;

	/* Succeeded: the validated XHTML graph and supported inline lifecycle are installed. */
	return 0;
}

/* Run a stable snapshot so self-removal, later removal and collection cannot corrupt traversal. */
static int
page_xhtml_inline_scripts(
	struct page *page)
{
	struct wb_vector scripts;
	struct dom_node *walk;
	struct dom_node **entry;
	struct dom_node *saved;
	struct vm_cell **roots;
	struct bind_window *window;
	size_t index;
	size_t registered;
	size_t total;
	int eligible;
	int error;

	/* Snapshot collection uses only native allocation while the active Document owns its tree. */
	wb_vector_init(&scripts, sizeof(struct dom_node *));
	walk = page->document->node.first_child;
	error = 0;
	while (walk != NULL) {
		eligible = page_xhtml_script_eligible(walk, page->document);
		if (eligible) {
			error = wb_vector_push(&scripts, &walk);
			if (error != 0)
				break;
		}

		/* No callbacks run until every original candidate and saved context can be rooted. */
		walk = bind_following(walk, &page->document->node);
	}

	/* Native snapshot failure never starts even the first script. */
	if (error != 0) {
		wb_vector_release(&scripts);
		return error;
	}

	/* Script-free documents retain the old load path without any new root allocation. */
	if (scripts.count == 0U) {
		wb_vector_release(&scripts);
		return 0;
	}

	/* Reserve one extra stable slot for a currentScript replaced during nested evaluation. */
	if (scripts.count == SIZE_MAX) {
		wb_vector_release(&scripts);
		return ENOMEM;
	}

	/* calloc rejects any slot-table multiplication overflow before publication. */
	total = scripts.count + 1U;
	roots = calloc(total, sizeof(*roots));
	if (roots == NULL) {
		wb_vector_release(&scripts);
		return ENOMEM;
	}

	/* A removed original candidate remains alive until the last earlier callback has finished. */
	window = page->window;
	saved = window->current_script;
	for (index = 0; index < scripts.count; index++) {
		entry = wb_vector_at(&scripts, index);
		roots[index] = &(*entry)->cell;
	}

	/* Replacing the Window's traced currentScript must not lose an outer script's only edge. */
	if (saved != NULL)
		roots[scripts.count] = &saved->cell;

	/* Each successful registration becomes an obligation for the shared cleanup path. */
	registered = 0;
	for (index = 0; index < total; index++) {
		error = vm_heap_add_root(page->heap, &roots[index]);
		if (error != 0)
			break;
		registered++;
	}

	/* All registered storage remains fixed throughout arbitrary inline DOM mutation and GC. */
	if (error == 0) {
		for (index = 0; index < scripts.count; index++) {
			/* A script that retired this owner cannot trigger further old-document execution. */
			if (window->detached)
				break;

			/* Prior callbacks may remove, adopt or dynamically prepare a saved candidate. */
			entry = wb_vector_at(&scripts, index);
			eligible = page_xhtml_script_eligible(*entry, page->document);
			if (!eligible)
				continue;

			/* The shared inline backend excludes external/module/data source and accepts text or CDATA. */
			bind_window_set_current_script(window, *entry);
			error = bind_run_inline_script(window, (struct dom_element *)*entry);
			bind_window_set_current_script(window, saved);
			if (error != 0)
				break;
		}
	}

	/* Drop every successfully registered slot before releasing its native table. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(page->heap, &roots[registered]);
	}

	/* Temporary ownership ends after execution or any root/evaluation failure. */
	free(roots);
	wb_vector_release(&scripts);
	if (error != 0)
		return error;

	/* Succeeded: only still-connected supported original inline candidates were prepared once. */
	return 0;
}

/* Recognize exact XHTML script names while rejecting inert, detached and already prepared nodes. */
static int
page_xhtml_script_eligible(
	const struct dom_node *node,
	const struct dom_document *document)
{
	const struct dom_node *ancestor;
	const struct dom_element *element;
	int same;
	int connected;

	/* Only an unprepared element belonging to this current Document is a candidate. */
	if (node->type != DOM_ELEMENT || node->document != document)
		return 0;

	/* Dynamic insertion may already have prepared a snapshotted original node. */
	if ((node->flags & DOM_NODE_SCRIPT_STARTED) != 0U)
		return 0;

	/* XML preserves local-name case and actual namespace identity. */
	element = (const struct dom_element *)node;
	if (element->ns != DOM_NS_HTML)
		return 0;

	/* Uppercase or foreign script-looking tags are not HTML script elements. */
	same = vm_string_equal_ascii(element->local_name, "script");
	if (!same)
		return 0;

	/* A saved node must still reach the active Document without crossing an inert template. */
	connected = 0;
	for (ancestor = node->parent; ancestor != NULL; ancestor = ancestor->parent) {
		/* Reaching this root establishes the still-active ownership path. */
		if (ancestor == &document->node) {
			connected = 1;
			break;
		}

		/* Non-element ancestors carry no HTML-template inertness of their own. */
		if (ancestor->type != DOM_ELEMENT)
			continue;

		/* XML's exact lowercase HTML template name keeps its descendants inert. */
		element = (const struct dom_element *)ancestor;
		if (element->ns != DOM_NS_HTML)
			continue;

		/* Foreign or differently cased template-looking tags do not create this HTML boundary. */
		same = vm_string_equal_ascii(element->local_name, "template");
		if (same)
			return 0;
	}

	/* A removed or adopted snapshot has no connected execution path in this Document. */
	if (!connected)
		return 0;

	/* Succeeded: this exact unprepared HTML script remains connected outside inert templates. */
	return 1;
}

/* Selects the actual declared top-level XHTML resource without body sniffing. */
static int
page_load_document(
	struct page *page,
	const unsigned char *bytes,
	size_t length,
	const char *mime,
	size_t mime_length)
{
	enum dom_document_content content;
	int error;

	if (mime != NULL) {
		error = page_document_content(mime, mime_length, &content);
		if (error == 0 && content == DOM_CONTENT_XHTML)
			return page_load_xhtml(page, bytes, length);
	}
	return page_load_html(page, bytes, length);
}

/* Local top-level XHTML has extension-derived MIME just as file URL fetches do. */
static int
page_file_xhtml(
	const char *path)
{
	const char *dot;
	const char *slash;

	dot = strrchr(path, '.');
	if (dot == NULL)
		return 0;
	slash = strrchr(path, '/');
	if (slash != NULL && dot < slash)
		return 0;
	if (strcmp(dot, ".xhtml") == 0 || strcmp(dot, ".xht") == 0)
		return 1;
	return 0;
}

static int
page_gather_styles(
	struct page *page)
{
	int error;

	/* A new engine with the user agent's sheet. */
	page_cache_fold(page);
	css_engine_destroy(page->css);
	page->css = NULL;
	error = css_engine_create(&page->css, page->heap);
	if (error != 0)
		return error;
	page->cache.engines++;
	if (page->visited_urls.count != 0)
		css_engine_set_visited_lookup(page->css, page_link_visited, page);
	css_engine_set_view_transition(page->css, page->vt_active, page->vt_types, page->vt_type_count);
	css_engine_set_color_scheme(page->css, page->dark);

	/* The document's sheets, parsed once and lent to the engine. */
	error = page_sheets_add(page);
	if (error != 0)
		return error;

	/* Succeeded: the sheets match the document of this generation (a layout made before them is stale). */
	page->gathered++;
	page->styled_generation = page_generation(page);
	page->styled_visits = page->visits_generation;

	/* The changes recorded so far are in this engine's styles (it computed them whole); with the cache on, the later ones tell what to drop. */
	(void)dom_changes_take_style(page->document, NULL, NULL);
	if (page_cache_mode() != PAGE_CACHE_OFF) {
		page->document->change_scope = page_change_scope;
		page->document->change_removed = page_change_removed;
		page->document->change_scope_context = page;
		css_engine_set_changed(page->css, page_style_changed, page);
	}

	/* The links whose sheets have arrived tell their scripts (a handler may change the document). */
	return page_sheets_fire_links(page);
}

/* A visited paint lookup cannot consult any history outside this page. */
static int
page_link_visited(
	void *context,
	struct dom_element *element)
{
	struct page *page;
	struct dom_attribute *attribute;
	struct vm_string *name;
	struct wb_buffer href;
	struct wb_buffer location;
	char **existing;
	size_t index;
	int visited;
	int error;

	/* Read only the HTML link's href; an absent or invalid URL is unvisited. */
	page = context;
	name = vm_atom_from_ascii(page->heap, "href");
	if (name == NULL)
		return 0;
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, name);
	if (attribute == NULL)
		return 0;
	if (page->base == NULL)
		return 0;

	/* Resolve link and navigation through the same URL normalization. */
	wb_buffer_init(&href);
	wb_buffer_init(&location);
	error = vm_string_to_utf8(attribute->value, &href);
	if (error == 0)
		error = page_resolve_location(page->base, wb_buffer_string(&href), &location);

	/* Search only the successful child navigations this page observed. */
	visited = 0;
	if (error == 0) {
		for (index = 0; index < page->visited_urls.count; index++) {
			existing = wb_vector_at(&page->visited_urls, index);
			if (strcmp(*existing, wb_buffer_string(&location)) == 0) {
				visited = 1;
				break;
			}
		}
	}
	wb_buffer_release(&href);
	wb_buffer_release(&location);
	return visited;
}

/* Appends the text of a node's text children. */
static int
page_text_of(
	const struct dom_node *node,
	struct wb_units *units)
{
	const struct dom_node *child;
	const struct dom_character_data *text;
	int error;

	/* Joins the text children. */
	for (child = node->first_child; child != NULL; child = child->next) {
		if (child->type != DOM_TEXT)
			continue;
		text = (const struct dom_character_data *)child;
		error = wb_units_append(units, text->data.data, text->data.length);
		if (error != 0)
			return error;
	}

	/* Succeeded: the text is appended. */
	return 0;
}

/* Dumps a node's children at a depth. */
static int
page_dump_node(
	const struct dom_node *node,
	int depth,
	struct wb_buffer *out)
{
	const struct dom_node *child;
	const struct dom_element *element;
	const struct dom_character_data *text;
	size_t index;
	int error;

	/* One line per child, elements followed by their attributes and children. */
	for (child = node->first_child; child != NULL; child = child->next) {
		error = page_indent(out, depth);
		if (error != 0)
			return error;
		switch (child->type) {
		case DOM_ELEMENT:
			element = (const struct dom_element *)child;
			error = wb_buffer_append_string(out, "<");
			if (error != 0)
				return error;
			if (element->ns == DOM_NS_SVG) {
				error = wb_buffer_append_string(out, "svg ");
				if (error != 0)
					return error;
			}

			/* Foreign MathML elements keep their namespace marker. */
			if (element->ns == DOM_NS_MATHML) {
				error = wb_buffer_append_string(out, "math ");
				if (error != 0)
					return error;
			}

			/* The local name and closing delimiter finish the element line. */
			error = page_append_string(out, element->local_name);
			if (error != 0)
				return error;
			error = wb_buffer_append_string(out, ">\n");
			if (error != 0)
				return error;
			for (index = 0; index < element->attribute_count; index++) {
				error = page_indent(out, depth + 1);
				if (error != 0)
					return error;
				error = page_append_string(out, element->attributes[index].name);
				if (error != 0)
					return error;
				error = wb_buffer_append_string(out, "=\"");
				if (error != 0)
					return error;
				error = page_append_string(out, element->attributes[index].value);
				if (error != 0)
					return error;
				error = wb_buffer_append_string(out, "\"\n");
				if (error != 0)
					return error;
			}

			/* Then its children. */
			error = page_dump_node(child, depth + 1, out);
			if (error != 0)
				return error;
			break;
		case DOM_TEXT:
			text = (const struct dom_character_data *)child;
			error = wb_buffer_append_string(out, "\"");
			if (error != 0)
				return error;
			error = wb_units_to_utf8(text->data.data, text->data.length, out);
			if (error != 0)
				return error;
			error = wb_buffer_append_string(out, "\"\n");
			if (error != 0)
				return error;
			break;
		case DOM_COMMENT:
			text = (const struct dom_character_data *)child;
			error = wb_buffer_append_string(out, "<!-- ");
			if (error != 0)
				return error;
			error = wb_units_to_utf8(text->data.data, text->data.length, out);
			if (error != 0)
				return error;
			error = wb_buffer_append_string(out, " -->\n");
			if (error != 0)
				return error;
			break;
		case DOM_DOCUMENT_TYPE:
			error = wb_buffer_append_string(out, "<!DOCTYPE ");
			if (error != 0)
				return error;
			error = page_append_string(out, ((const struct dom_doctype *)child)->name);
			if (error != 0)
				return error;
			error = wb_buffer_append_string(out, ">\n");
			if (error != 0)
				return error;
			break;
		default:
			error = wb_buffer_append_string(out, "?\n");
			if (error != 0)
				return error;
			break;
		}
	}

	/* Succeeded: the children are dumped. */
	return 0;
}

/* Computes and dumps an element's style, then its element children's. */
static int
page_dump_style_node(
	struct page *page,
	struct dom_element *element,
	const struct css_style *parent,
	int depth,
	struct wb_buffer *out)
{
	struct css_style *style;
	struct dom_node *child;
	int error;

	/* Computes the style (on the heap: the recursion would otherwise use much stack). */
	style = malloc(sizeof(*style));
	if (style == NULL)
		return ENOMEM;
	error = css_engine_compute(page->css, element, parent, style);
	if (error != 0) {
		free(style);
		return error;
	}

	/* Emits one complete style line before recursively visiting children. */
	error = page_dump_style_line(element, style, depth, out);
	if (error != 0) {
		free(style);
		return error;
	}

	/* The element children, below the depth limit. */
	error = 0;
	if (depth < PAGE_DUMP_DEPTH) {
		for (child = element->node.first_child; child != NULL && error == 0; child = child->next) {
			if (child->type == DOM_ELEMENT)
				error = page_dump_style_node(page, (struct dom_element *)child, style, depth + 1, out);
		}
	}

	/* The style is no longer needed. */
	free(style);
	if (error != 0)
		return error;

	/* Succeeded: the subtree's styles are dumped. */
	return 0;
}

/* Appends one computed-style line, checking every buffer expansion. */
static int
page_dump_style_line(
	const struct dom_element *element,
	const struct css_style *style,
	int depth,
	struct wb_buffer *out)
{
	static const char *const displays[] = {
		"inline", "block", "inline-block", "list-item", "none", "table", "table-row", "table-cell", "flex", "contents", "grid",
		"table-row-group", "table-caption", "inline-table", "table-column"
	};
	int side;
	int error;

	/* Appends the element name and main computed properties. */
	error = page_indent(out, depth);
	if (error != 0)
		return error;
	error = page_append_string(out, element->local_name);
	if (error != 0)
		return error;
	error = wb_buffer_printf(out, " display=%s font-size=%.2f weight=%d italic=%d color=#%08x background=#%08x",
	    displays[style->display], (double)style->font_size, style->font_weight, style->font_italic,
	    (unsigned)style->color, (unsigned)style->background_color);
	if (error != 0)
		return error;

	/* Appends all four margins in established order. */
	error = wb_buffer_append_string(out, " margin=");
	if (error != 0)
		return error;
	for (side = 0; side < 4; side++) {
		if (side > 0) {
			error = wb_buffer_append_string(out, ",");
			if (error != 0)
				return error;
		}

		/* The current margin follows its separator. */
		error = page_append_length(out, &style->margin[side]);
		if (error != 0)
			return error;
	}

	/* Appends all four paddings in established order. */
	error = wb_buffer_append_string(out, " padding=");
	if (error != 0)
		return error;
	for (side = 0; side < 4; side++) {
		if (side > 0) {
			error = wb_buffer_append_string(out, ",");
			if (error != 0)
				return error;
		}

		/* The current padding follows its separator. */
		error = page_append_length(out, &style->padding[side]);
		if (error != 0)
			return error;
	}

	/* Appends the border widths and final computed width. */
	error = wb_buffer_printf(out, " border=%.1f,%.1f,%.1f,%.1f width=", (double)style->border_width[0], (double)style->border_width[1],
	    (double)style->border_width[2], (double)style->border_width[3]);
	if (error != 0)
		return error;
	error = page_append_length(out, &style->width);
	if (error != 0)
		return error;
	error = wb_buffer_append_string(out, "\n");
	if (error != 0)
		return error;

	/* Succeeded: the line is complete and its caller may visit children. */
	return 0;
}

/* Appends two spaces per level. */
static int
page_indent(
	struct wb_buffer *out,
	int depth)
{
	int level;
	int error;

	/* The html5lib format's prefix and the indentation. */
	error = wb_buffer_append_string(out, "| ");
	if (error != 0)
		return error;
	for (level = 0; level < depth; level++) {
		error = wb_buffer_append_string(out, "  ");
		if (error != 0)
			return error;
	}

	/* Succeeded: the next field starts at the requested tree depth. */
	return 0;
}

/* Appends a VM string as UTF-8. */
static int
page_append_string(
	struct wb_buffer *out,
	const struct vm_string *string)
{
	int error;

	/* Converts the string. */
	error = vm_string_to_utf8(string, out);
	if (error != 0)
		return error;

	/* Succeeded: all string bytes are in the dump. */
	return 0;
}

/* Appends a computed length as text. */
static int
page_append_length(
	struct wb_buffer *out,
	const struct css_length *length)
{
	int error;

	/* The unit decides the form. */
	switch (length->unit) {
	case CSS_UNIT_PX:
		error = wb_buffer_printf(out, "%.2f", (double)length->value);
		break;
	case CSS_UNIT_PERCENT:
		error = wb_buffer_printf(out, "%.2f%%", (double)length->value);
		if (error != 0)
			return error;
		if (length->offset != 0) {
			error = wb_buffer_printf(out, "%+.2f", (double)length->offset);
			if (error != 0)
				return error;
		}

		break;
	case CSS_UNIT_AUTO:
		error = wb_buffer_append_string(out, "auto");
		break;
	default:
		error = wb_buffer_append_string(out, "?");
		break;
	}

	/* A failed append cannot be reported as a complete length. */
	if (error != 0)
		return error;

	/* Succeeded: the complete length representation was appended. */
	return 0;
}

/* Finds the first HTML <title> element under a node, in document order. */
static const struct dom_node *
page_find_title(
	const struct dom_node *node,
	int depth)
{
	const struct dom_node *child;
	const struct dom_node *found;
	int is_title;

	/* The search stops at the depth the dumps stop at. */
	if (depth > PAGE_DUMP_DEPTH)
		return NULL;

	/* The node itself. */
	is_title = dom_element_is(node, DOM_NS_HTML, DOM_TAG_TITLE);
	if (is_title)
		return node;

	/* Otherwise its children, in order. */
	for (child = node->first_child; child != NULL; child = child->next) {
		found = page_find_title(child, depth + 1);
		if (found != NULL)
			return found;
	}

	/* No title under the node. */
	return NULL;
}
