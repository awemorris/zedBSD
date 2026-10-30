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
#include "bind/internal.h"
#include "net/net.h"
#include "xml/dom.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most bytes of live cells a page's heap may hold. */
#define PAGE_HEAP_LIMIT		((size_t)1024U * 1024U * 1024U)

/* How many times more a layout is made at most for the query containers' sizes. */
#define PAGE_CONTAINER_PASSES	2

/* The deepest element nesting the style dump descends (the parser caps nesting too). */
#define PAGE_DUMP_DEPTH		512

static int page_gather_styles(struct page *page);
static int page_load_xhtml(struct page *page, const unsigned char *bytes, size_t length);
static int page_xhtml_inline_scripts(struct page *page);
static int page_xhtml_script_eligible(const struct dom_node *node, const struct dom_document *document);
static int page_load_document(struct page *page, const unsigned char *bytes, size_t length, const char *mime, size_t mime_length);
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

	/* The document is parsed: DOMContentLoaded, then the window's load.  The sheets it links block its deferred scripts. */
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
 * Loads an HTML document from a file.
 */
int
page_load_file(
	struct page *page,
	const char *path)
{
	struct wb_buffer buffer;
	int error;

	/* The page's scripts and images find their files from the page's, by its absolute path. */
	free(page->base);
	page->base = realpath(path, NULL);
	if (page->base == NULL)
		page->base = strdup(path);
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
	int error;

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
	error = page_fetch_response(location, location, &response);

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

	/* The document. */
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
 * Describes why the last load failed beyond its error number (the TLS
 * verification's reason, for an https page); empty when there is nothing
 * more to say.
 */
const char *
page_failure_reason(void)
{
	const char *reason;

	/* The network's reason. */
	reason = net_tls_error();
	return reason;
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
	/* The frames. */
	if (page->frame_loads.count != 0)
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

	/* Opens them once. */
	if (page->text_open)
		return 0;
	error = text_system_open(&page->text, paths);
	if (error != 0)
		return error;

	/* Succeeded: the fonts are open. */
	page->text_open = 1;
	return 0;
}

/*
 * Lays the page out for a viewport of width by height pixels (the fonts
 * must be open).
 */
int
page_layout(
	struct page *page,
	int width,
	int height)
{
	int missed;
	int pass;
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

	/* The web fonts that arrived join the text system. */
	page_fonts_install(page);

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

	/* Builds and lays out the box tree, the images found by their elements (the ones there are now). */
	page->laid_out_images = page->images_generation;
	css_engine_set_container_lookup(page->css, page_container_size, page);
	error = layout_build(&page->layout, page->css, &page->text, page->document, page_image_of, page_image_by_url, page, width, height);
	page->laid_out = 1;
	if (error != 0)
		goto cleanup;

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
		error = layout_build(&page->layout, page->css, &page->text, page->document, page_image_of, page_image_by_url, page, width, height);
		if (error != 0)
			goto cleanup;
	}

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
	page->laid_out_generation = page->document->generation;
	page->laid_out_visits = page->visits_generation;
	page->layout_serial++;

	/* Succeeded: the page is laid out. */
	return 0;

cleanup:
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
 * Moves the sticky boxes of the layout for the page's scroll; reports
 * whether any moved (the display list is then built again).
 */
int
page_sticky_sync(
	struct page *page,
	layout_unit scroll_y)
{
	/* Only a laid out page with sticky boxes has any. */
	if (!page->laid_out)
		return 0;
	return layout_sticky_apply(&page->layout, scroll_y);
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

	/* Walks the layout into a new one. */
	error = paint_build(&page->paint, &page->layout);
	if (error != 0)
		return error;
	page->painted = 1;
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
	    page->styled_generation == page->document->generation &&
	    page->styled_visits == page->visits_generation &&
	    page->styled_sheets == page->sheets_generation)
		return 0;

	/*
	 * So do styles of the same document while more sheets are on their way:
	 * the page is styled again once they are all here.
	 */
	pending = page_sheets_pending(page);
	if (page->css != NULL && page->styled_generation == page->document->generation &&
	    page->styled_visits == page->visits_generation && pending)
		return 0;

	/* A new engine with the sheets as they are now. */
	error = page_gather_styles(page);
	if (error != 0)
		return error;

	/* Succeeded: the styles match the document. */
	return 0;
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
	css_engine_destroy(page->css);
	page->css = NULL;
	error = css_engine_create(&page->css, page->heap);
	if (error != 0)
		return error;
	if (page->visited_urls.count != 0)
		css_engine_set_visited_lookup(page->css, page_link_visited, page);

	/* The document's sheets, parsed once and lent to the engine. */
	error = page_sheets_add(page);
	if (error != 0)
		return error;

	/* Succeeded: the sheets match the document of this generation. */
	page->styled_generation = page->document->generation;
	page->styled_visits = page->visits_generation;

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
