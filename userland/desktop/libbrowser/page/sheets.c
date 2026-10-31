/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A page's style sheets (ws074-p068): the <style> elements' text, the
 * sheets <link rel=stylesheet> elements name, and the sheets their @import
 * rules name, each parsed once and kept for the life of the page, then
 * lent to every style engine the page makes, in document order with each
 * sheet's imports before its own rules.
 *
 * An external sheet is kept by its location (one that could not be
 * fetched or parsed is remembered as failed, and not fetched again).
 * With the embedder's loader an http or https sheet is fetched without
 * blocking: the page is styled without it until the loader's callback
 * parses it and counts it in the page's sheets_generation, which styles
 * and lays the page out again.  A <style> element's sheet is kept by its
 * text, so a script that changes the document does not make the page
 * parse its sheets again; one no element has any more is dropped when the
 * page is next styled.
 */

#include "page/page.h"
#include "net/net.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The deepest element nesting the walk descends (the parser caps nesting too). */
#define SHEETS_DEPTH		512

/* How deep @import rules are followed (a sheet importing itself stops here); less than CSS_MEDIA_CHAIN_MAX. */
#define SHEETS_IMPORT_DEPTH	8U

/*
 * One sheet of the page: an external one's location (NULL for a <style>
 * element's), or a <style> element's text and its hash; the parsed sheet
 * (NULL while it is fetched, or when it failed); whether it failed; the
 * number of the styling that last used it; and while it is fetched its
 * request and the page it is for.
 */
struct page_sheet {
	char *location;
	uint16_t *text;
	size_t length;
	uint32_t hash;
	struct css_sheet *sheet;
	int failed;
	uint32_t used;
	struct page *page;
	struct net_request *request;
};

/*
 * What resolving a sheet's URLs needs: the page (its heap) and the
 * sheet's own location.
 */
struct sheets_resolving {
	struct page *page;
	const char *location;
};

static int sheets_add_adopted(struct page *page, struct dom_node *root);
static int sheets_walk(struct page *page, struct dom_node *node, int depth);
static int sheets_add_style(struct page *page, struct dom_element *element);
static int sheets_add_link(struct page *page, struct dom_element *element);
static int sheets_is_stylesheet_link(const struct dom_element *element);
static int sheets_word_in(const struct vm_string *list, const char *word);
static int sheets_add_loaded(struct page *page, struct page_sheet *entry, const struct css_media **chain, size_t depth);
static int sheets_element_media(struct page *page, const struct dom_element *element, const struct css_media **chain);
static int sheets_external(struct page *page, const char *location, struct page_sheet **found);
static struct page_sheet *sheets_find_location(const struct page *page, const char *location);
static int sheets_parse(struct page *page, struct page_sheet *entry, const unsigned char *bytes, size_t length);
static int sheets_resolve_url(void *context, const struct vm_string *url, struct vm_string **resolved);
static void sheets_arrived(void *context, struct net_request *request);
static int sheets_css_mime(const struct wb_buffer *content_type);
static void sheets_fetch_imports(struct page *page, struct page_sheet *entry);
static void sheets_drop_unused(struct page *page);
static void sheets_free(struct page_sheet *entry);

/*
 * Starts a page's table of sheets empty.
 */
void
page_sheets_init(
	struct page *page)
{
	/* No sheet yet. */
	wb_vector_init(&page->sheets, sizeof(struct page_sheet *));
	wb_vector_init(&page->link_events, sizeof(struct page_link_event));
	page->sheets_generation = 0;
	page->styled_sheets = 0;
	page->styling = 0;
}

/*
 * Lends the page's sheets to its style engine in cascade order: every
 * <style> and <link rel=stylesheet> of the document in tree order, each
 * after the sheets its @import rules name.  Sheets not fetched yet start
 * fetching (the page is styled again when they arrive).
 */
int
page_sheets_add(
	struct page *page)
{
	int error;

	/* A new styling: the sheets it uses are marked with its number. */
	page->styling++;
	page->styled_sheets = page->sheets_generation;

	/* The document's sheets in tree order. */
	error = sheets_walk(page, &page->document->node, 0);
	if (error != 0)
		return error;

	/* The constructed sheets the document adopted come after them. */
	error = sheets_add_adopted(page, &page->document->node);
	if (error != 0)
		return error;

	/* The <style> texts no element has any more. */
	sheets_drop_unused(page);

	/* Succeeded: the engine has the page's sheets. */
	return 0;
}

/*
 * Tells whether a sheet of the page is still on its way (ws074-p071): the
 * page, once styled, waits for the lot before it is styled again.
 */
int
page_sheets_pending(
	const struct page *page)
{
	const struct page_sheet *entry;
	size_t index;

	/* Any entry with a request under way. */
	for (index = 0; index < page->sheets.count; index++) {
		entry = *(struct page_sheet *const *)wb_vector_at(&page->sheets, index);
		if (entry->request != NULL)
			return 1;
	}

	/* Every sheet is here, or failed. */
	return 0;
}

/*
 * Frees the page's sheets, cancelling the fetches under way.
 */
void
page_sheets_release(
	struct page *page)
{
	struct page_sheet *entry;
	size_t index;

	/* Each entry. */
	for (index = 0; index < page->sheets.count; index++) {
		entry = *(struct page_sheet **)wb_vector_at(&page->sheets, index);
		sheets_free(entry);
	}

	/* The table itself. */
	wb_vector_release(&page->sheets);
	wb_vector_release(&page->link_events);
}

/* Adds the sheets of the constructed style sheets a document or shadow root adopted, in the tree they belong to. */
static int
sheets_add_adopted(
	struct page *page,
	struct dom_node *root)
{
	struct dom_node *holder;
	struct dom_node *child;
	int error;

	holder = dom_adopted_sheets(root, 0);
	if (holder == NULL)
		return 0;
	for (child = holder->first_child; child != NULL; child = child->next) {
		error = sheets_add_style(page, (struct dom_element *)child);
		if (error != 0)
			return error;
		css_engine_set_scope(page->css, dom_shadow_of(root) != NULL ? root : NULL);
	}
	return 0;
}

/* Adds the sheets of a node and its descendants in tree order. */
static int
sheets_walk(
	struct page *page,
	struct dom_node *node,
	int depth)
{
	struct dom_element *element;
	struct dom_node *child;
	int is_style;
	int is_link;
	int error;

	/* Stops at the depth the parser stops at. */
	if (depth > SHEETS_DEPTH)
		return 0;

	/* A <style> element's text is a sheet; its children are not searched. */
	is_style = dom_element_is(node, DOM_NS_HTML, DOM_TAG_STYLE);
	if (is_style) {
		element = (struct dom_element *)node;
		error = sheets_add_style(page, element);
		return error;
	}

	/* A <link> may name a sheet. */
	is_link = dom_element_is(node, DOM_NS_HTML, DOM_TAG_LINK);
	if (is_link) {
		element = (struct dom_element *)node;
		error = sheets_add_link(page, element);
		if (error != 0)
			return error;
	}

	/* The children, in document order. */
	for (child = node->first_child; child != NULL; child = child->next) {
		error = sheets_walk(page, child, depth + 1);
		if (error != 0)
			return error;
	}

	/* An element's shadow tree has its own sheets. */
	if (node->type == DOM_ELEMENT && ((struct dom_element *)node)->shadow_root != NULL) {
		css_engine_set_shadow_scopes(page->css);
		for (child = ((struct dom_element *)node)->shadow_root->first_child; child != NULL; child = child->next) {
			error = sheets_walk(page, child, depth + 1);
			if (error != 0)
				return error;
		}
		error = sheets_add_adopted(page, ((struct dom_element *)node)->shadow_root);
		if (error != 0)
			return error;
	}

	/* Succeeded: the subtree's sheets are added. */
	return 0;
}

/*
 * Adds a <style> element's sheet: the one parsed before from the same
 * text, or a new one.
 */
static int
sheets_add_style(
	struct page *page,
	struct dom_element *element)
{
	struct page_sheet *entry;
	struct page_sheet *candidate;
	struct wb_units units;
	const struct css_media *chain[CSS_MEDIA_CHAIN_MAX];
	uint32_t hash;
	size_t index;
	int differs;
	int error;

	/* Inline source ownership is shared with child styling and later native CSSOM handles. */
	wb_units_init(&units);
	error = bind_style_sheet_source(element, &units);
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}

	/* A sheet parsed before from the same text. */
	hash = wb_hash_units(units.data, units.length);
	entry = NULL;
	for (index = 0; index < page->sheets.count; index++) {
		candidate = *(struct page_sheet **)wb_vector_at(&page->sheets, index);
		if (candidate->location != NULL || candidate->hash != hash || candidate->length != units.length)
			continue;

		/* The same hash and length: the texts themselves decide. */
		differs = 0;
		if (units.length != 0)
			differs = memcmp(candidate->text, units.data, units.length * sizeof(uint16_t));
		if (differs != 0)
			continue;
		entry = candidate;
		break;
	}

	/* Otherwise a new entry with the text, parsed now. */
	if (entry == NULL) {
		entry = calloc(1, sizeof(*entry));
		if (entry == NULL) {
			wb_units_release(&units);
			return ENOMEM;
		}

		/* The entry keeps the text (the units buffer is handed over) and its hash. */
		entry->text = units.data;
		entry->length = units.length;
		entry->hash = hash;
		entry->page = page;
		wb_units_init(&units);

		/* The parsed sheet; a sheet that does not parse is an empty one. */
		error = css_sheet_create(&entry->sheet, page->heap, entry->text, entry->length);
		if (error == ENOMEM) {
			sheets_free(entry);
			return error;
		}

		/* Any other failure leaves the entry without a sheet. */
		if (error != 0)
			entry->failed = 1;

		/* The entry goes into the table. */
		error = wb_vector_push(&page->sheets, &entry);
		if (error != 0) {
			sheets_free(entry);
			return error;
		}
	}

	/* The text, when a known entry matched it, is no longer needed. */
	wb_units_release(&units);

	/* The element's media list, then the sheet after its imports. */
	error = sheets_element_media(page, element, chain);
	if (error != 0)
		return error;
	error = sheets_add_loaded(page, entry, chain, 1);
	if (error != 0)
		return error;

	/* The sheet's implicit @scope is under the element's parent. */
	css_engine_set_owner(page->css, element);

	/* Succeeded: the element's sheet is in the engine. */
	return 0;
}

/* Adds the sheet a <link rel=stylesheet href> names, fetching it the first time. */
static int
sheets_add_link(
	struct page *page,
	struct dom_element *element)
{
	struct page_sheet *entry;
	struct dom_attribute *attribute;
	struct vm_string *href_name;
	struct wb_buffer href;
	struct wb_buffer location;
	const struct css_media *chain[CSS_MEDIA_CHAIN_MAX];
	struct page_link_event event;
	int is_sheet;
	int error;

	/* Only a link to a style sheet counts. */
	is_sheet = sheets_is_stylesheet_link(element);
	if (!is_sheet || page->base == NULL)
		return 0;

	/* Its href. */
	href_name = vm_atom_from_ascii(page->heap, "href");
	if (href_name == NULL)
		return ENOMEM;
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, href_name);
	if (attribute == NULL)
		return 0;

	/* The location the href names against the page's. */
	wb_buffer_init(&href);
	wb_buffer_init(&location);
	error = vm_string_to_utf8(attribute->value, &href);
	if (error == 0)
		error = page_resolve_location(page->base, wb_buffer_string(&href), &location);
	wb_buffer_release(&href);
	if (error == EINVAL) {
		wb_buffer_release(&location);
		return 0;
	}

	/* The location's sheet, fetched when it is new. */
	if (error == 0)
		error = sheets_external(page, wb_buffer_string(&location), &entry);
	wb_buffer_release(&location);
	if (error != 0)
		return error;

	/* The link's media list, then the sheet after its imports, once it is there. */
	error = sheets_element_media(page, element, chain);
	if (error != 0)
		return error;
	error = sheets_add_loaded(page, entry, chain, 1);
	if (error != 0)
		return error;

	/* A link whose sheet is in (or failed) has its load or error event queued, once. */
	if ((element->node.flags & DOM_NODE_LINK_LOADED) == 0U && (entry->sheet != NULL || entry->failed)) {
		event.element = element;
		event.failed = entry->failed;
		element->node.flags |= DOM_NODE_LINK_LOADED;
		error = wb_vector_push(&page->link_events, &event);
		if (error != 0)
			return error;
	}

	/* Succeeded: the link's sheet is in the engine, or on its way. */
	return 0;
}

/*
 * Tells whether a <link> names a style sheet in use: its rel has the word
 * stylesheet and not alternate, and it is not disabled.
 */
static int
sheets_is_stylesheet_link(
	const struct dom_element *element)
{
	struct vm_string *rel;
	struct vm_string *disabled;
	int has_word;

	/* The rel attribute's words. */
	rel = dom_attribute_ascii(element, "rel");
	if (rel == NULL)
		return 0;

	/* It must say stylesheet. */
	has_word = sheets_word_in(rel, "stylesheet");
	if (!has_word)
		return 0;

	/* An alternate sheet is not used until the user picks it. */
	has_word = sheets_word_in(rel, "alternate");
	if (has_word)
		return 0;

	/* A disabled link names no sheet in use. */
	disabled = dom_attribute_ascii(element, "disabled");
	if (disabled != NULL)
		return 0;

	/* The link names a style sheet. */
	return 1;
}

/* Tells whether a space-separated list has an ASCII word, ignoring ASCII case. */
static int
sheets_word_in(
	const struct vm_string *list,
	const char *word)
{
	size_t start;
	size_t end;
	size_t index;
	size_t length;
	uint16_t unit;
	uint16_t wanted;

	/* Walks the words. */
	start = 0;
	while (start < list->length) {
		/* Skips whitespace. */
		unit = vm_string_at(list, start);
		if (unit == ' ' || unit == '\t' || unit == '\n' || unit == '\f' || unit == '\r') {
			start++;
			continue;
		}

		/* Finds the end of the word. */
		end = start;
		while (end < list->length) {
			unit = vm_string_at(list, end);
			if (unit == ' ' || unit == '\t' || unit == '\n' || unit == '\f' || unit == '\r')
				break;
			end++;
		}

		/* Compares it with the word wanted, folding ASCII upper case. */
		length = strlen(word);
		if (end - start == length) {
			for (index = 0; index < end - start; index++) {
				unit = vm_string_at(list, start + index);
				if (unit >= 'A' && unit <= 'Z')
					unit = (uint16_t)(unit + 0x20U);
				wanted = (unsigned char)word[index];
				if (unit != wanted)
					break;
			}

			/* The whole word matched. */
			if (index == end - start)
				return 1;
		}

		/* On to the next word. */
		start = end;
	}

	/* The word is not in the list. */
	return 0;
}

/*
 * Reads the media attribute of a <style> or <link> into chain[0] (NULL
 * without one: the sheet applies to every medium).
 */
static int
sheets_element_media(
	struct page *page,
	const struct dom_element *element,
	const struct css_media **chain)
{
	struct vm_string *media;
	struct wb_units units;
	size_t index;
	int error;

	/* No attribute is no list. */
	chain[0] = NULL;
	media = dom_attribute_ascii(element, "media");
	if (media == NULL)
		return 0;

	/* The attribute's characters. */
	wb_units_init(&units);
	error = 0;
	for (index = 0; index < media->length && error == 0; index++)
		error = wb_units_append_code_point(&units, vm_string_at(media, index));
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}

	/* The list, kept by the engine. */
	error = css_engine_parse_media(page->css, units.data, units.length, &chain[0]);
	wb_units_release(&units);
	if (error != 0)
		return error;

	/* Succeeded: the element's list is read. */
	return 0;
}

/*
 * Lends a loaded sheet to the engine after the sheets its @import rules
 * name (a sheet still fetched, or one that failed, adds nothing); it
 * applies under the depth media lists of chain (its element's and those
 * of the imports that led to it).
 */
static int
sheets_add_loaded(
	struct page *page,
	struct page_sheet *entry,
	const struct css_media **chain,
	size_t depth)
{
	struct page_sheet *imported;
	struct wb_buffer text;
	struct wb_buffer location;
	const char *base;
	size_t count;
	size_t index;
	int error;

	/* The styling uses the entry, loaded or not (a fetch under way is kept). */
	entry->used = page->styling;
	if (entry->sheet == NULL)
		return 0;

	/* The imported sheets first, resolved against this sheet's location, not too deep. */
	count = css_sheet_import_count(entry->sheet);
	base = entry->location;
	if (base == NULL)
		base = page->base;
	for (index = 0; index < count && depth < SHEETS_IMPORT_DEPTH && base != NULL; index++) {
		wb_buffer_init(&text);
		wb_buffer_init(&location);
		error = vm_string_to_utf8(css_sheet_import(entry->sheet, index), &text);
		if (error == 0)
			error = page_resolve_location(base, wb_buffer_string(&text), &location);
		wb_buffer_release(&text);

		/* The import's sheet, fetched when it is new. */
		imported = NULL;
		if (error == 0)
			error = sheets_external(page, wb_buffer_string(&location), &imported);
		wb_buffer_release(&location);
		if (error == EINVAL)
			continue;
		if (error != 0)
			return error;

		/* Its own imports and rules, under the import's media list too. */
		chain[depth] = css_sheet_import_media(entry->sheet, index);
		error = sheets_add_loaded(page, imported, chain, depth + 1U);
		if (error != 0)
			return error;
	}

	/* Then the sheet's own rules. */
	error = css_engine_add_parsed(page->css, entry->sheet, chain, depth);
	if (error != 0)
		return error;

	/* And its web fonts, loaded when they are new (ws074-p070). */
	error = page_fonts_add_sheet(page, entry->sheet);
	if (error != 0)
		return error;

	/* Succeeded: the sheet and its imports are in the engine. */
	return 0;
}

/*
 * Finds an external sheet by its location, or starts loading it into a
 * new entry: through the loader for an http or https location, read at
 * once otherwise.
 */
static int
sheets_external(
	struct page *page,
	const char *location,
	struct page_sheet **found)
{
	struct page_sheet *entry;
	struct wb_buffer bytes;
	int remote;
	int error;

	/* A location loaded before, well or not, is not fetched again. */
	entry = sheets_find_location(page, location);
	if (entry != NULL) {
		*found = entry;
		return 0;
	}

	/* The table's entry for the location. */
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return ENOMEM;
	entry->page = page;
	entry->location = strdup(location);
	if (entry->location == NULL) {
		free(entry);
		return ENOMEM;
	}

	/* An http or https sheet with a loader is fetched without blocking. */
	remote = net_loader_takes(entry->location);
	if (page->loader != NULL && remote) {
		error = net_loader_fetch(page->loader, entry->location, sheets_arrived, entry, &entry->request);
		if (error != 0)
			entry->failed = 1;
	} else {
		/* The bytes, then the parse; either failing marks the sheet failed. */
		wb_buffer_init(&bytes);
		error = page_fetch(page->base, entry->location, &bytes, NULL);
		if (error == 0)
			error = sheets_parse(page, entry, bytes.data, bytes.length);
		wb_buffer_release(&bytes);
		if (error == ENOMEM) {
			sheets_free(entry);
			return error;
		}

		/* Any other failure is remembered. */
		if (error != 0)
			entry->failed = 1;
	}

	/* The entry goes into the table. */
	error = wb_vector_push(&page->sheets, &entry);
	if (error != 0) {
		sheets_free(entry);
		return error;
	}

	/* Succeeded: the sheet is in the table. */
	*found = entry;
	return 0;
}

/* Finds an external sheet of the table by its location. */
static struct page_sheet *
sheets_find_location(
	const struct page *page,
	const char *location)
{
	struct page_sheet *entry;
	size_t index;
	int differs;

	/* Each external entry of the table. */
	for (index = 0; index < page->sheets.count; index++) {
		entry = *(struct page_sheet **)wb_vector_at(&page->sheets, index);
		if (entry->location == NULL)
			continue;
		differs = strcmp(entry->location, location);
		if (differs == 0)
			return entry;
	}

	/* The location is not in the table. */
	return NULL;
}

/*
 * Parses an external sheet's bytes (UTF-8, a byte order mark dropped),
 * with its URLs resolved against its own location.
 */
static int
sheets_parse(
	struct page *page,
	struct page_sheet *entry,
	const unsigned char *bytes,
	size_t length)
{
	struct sheets_resolving resolving;
	struct wb_units units;
	int error;

	/* Drops a UTF-8 byte order mark. */
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

	/* Parses it. */
	error = css_sheet_create(&entry->sheet, page->heap, units.data, units.length);
	wb_units_release(&units);
	if (error != 0)
		return error;

	/* Its URLs no longer depend on where it came from. */
	resolving.page = page;
	resolving.location = entry->location;
	error = css_sheet_resolve_urls(entry->sheet, sheets_resolve_url, &resolving);
	if (error != 0) {
		/* A partly resolved sheet must not be published after an asynchronous failure. */
		css_sheet_destroy(entry->sheet);
		entry->sheet = NULL;
		return error;
	}

	/* Succeeded: the sheet is parsed. */
	return 0;
}

/* Resolves a URL of an external sheet against the sheet's location, as an atom. */
static int
sheets_resolve_url(
	void *context,
	const struct vm_string *url,
	struct vm_string **resolved)
{
	struct sheets_resolving *resolving;
	struct wb_buffer text;
	struct wb_buffer location;
	struct wb_units units;
	int error;

	/* The URL's text against the sheet's location. */
	resolving = context;
	wb_buffer_init(&text);
	wb_buffer_init(&location);
	wb_units_init(&units);
	error = vm_string_to_utf8(url, &text);
	if (error == 0)
		error = page_resolve_location(resolving->location, wb_buffer_string(&text), &location);

	/* The resolved location as an atom. */
	if (error == 0)
		error = wb_utf8_to_units((const unsigned char *)location.data, location.length, &units);
	*resolved = NULL;
	if (error == 0) {
		*resolved = vm_atom_from_units(resolving->page->heap, units.data, units.length);
		if (*resolved == NULL)
			error = ENOMEM;
	}

	/* The buffers are no longer needed. */
	wb_buffer_release(&text);
	wb_buffer_release(&location);
	wb_units_release(&units);
	if (error != 0)
		return error;

	/* Succeeded: the URL is absolute. */
	return 0;
}

/*
 * The loader's callback for a sheet: its body parsed when it came (a
 * failure, or a status other than 2xx, marks the sheet failed), and the
 * page told to style itself again.
 */
static void
sheets_arrived(
	void *context,
	struct net_request *request)
{
	const struct net_response *response;
	struct page_sheet *entry;
	int error;

	/* The sheet, which waits no longer. */
	entry = context;
	entry->request = NULL;

	/* A response with the sheet's text, parsed. */
	error = net_request_error(request);
	response = net_request_response(request);
	if (error == 0 && (response->status < 200 || response->status > 299))
		error = EINVAL;
	if (error == 0 && entry->page->document->content == DOM_CONTENT_HTML &&
	    entry->page->document->quirks != DOM_QUIRKS &&
	    response->content_type.length != 0 &&
	    !sheets_css_mime(&response->content_type))
		error = EINVAL;
	if (error == 0)
		error = sheets_parse(entry->page, entry, response->body.data, response->body.length);
	if (error != 0)
		entry->failed = 1;

	/* The sheets it imports start on their way now, not at the next styling. */
	if (error == 0)
		sheets_fetch_imports(entry->page, entry);

	/* The page is styled and laid out again with it. */
	entry->page->sheets_generation++;
}

/* Checks the MIME essence of an HTTP stylesheet response. */
static int
sheets_css_mime(
	const struct wb_buffer *content_type)
{
	static const char css_mime[] = "text/css";
	const unsigned char *bytes;
	size_t start;
	size_t end;
	size_t index;
	unsigned char byte;

	bytes = content_type->data;
	start = 0;
	end = content_type->length;
	while (start < end && (bytes[start] == ' ' || bytes[start] == '\t'))
		start++;
	for (index = start; index < end; index++) {
		if (bytes[index] == ';') {
			end = index;
			break;
		}
	}
	while (end > start && (bytes[end - 1] == ' ' || bytes[end - 1] == '\t'))
		end--;
	if (end - start != sizeof(css_mime) - 1)
		return 0;
	for (index = 0; index < sizeof(css_mime) - 1; index++) {
		byte = bytes[start + index];
		if (byte >= 'A' && byte <= 'Z')
			byte += 'a' - 'A';
		if (byte != css_mime[index])
			return 0;
	}
	return 1;
}

/*
 * Starts fetching the sheets a sheet that arrived imports (its URLs are
 * resolved already), so that a chain of imports arrives in one wait; a
 * failure only leaves an import for the next styling to try.
 */
static void
sheets_fetch_imports(
	struct page *page,
	struct page_sheet *entry)
{
	struct page_sheet *imported;
	struct wb_buffer location;
	size_t count;
	size_t index;
	int error;

	/* Each import's location, fetched when it is new. */
	count = css_sheet_import_count(entry->sheet);
	for (index = 0; index < count; index++) {
		wb_buffer_init(&location);
		error = vm_string_to_utf8(css_sheet_import(entry->sheet, index), &location);
		if (error == 0)
			error = sheets_external(page, wb_buffer_string(&location), &imported);
		wb_buffer_release(&location);
		if (error != 0)
			break;
	}
}

/* Frees the <style> sheets the last styling did not use (external ones are kept). */
static void
sheets_drop_unused(
	struct page *page)
{
	struct page_sheet **entries;
	size_t index;
	size_t kept;

	/* Keeps the used entries and the external ones, in order. */
	entries = page->sheets.items;
	kept = 0;
	for (index = 0; index < page->sheets.count; index++) {
		if (entries[index]->location == NULL && entries[index]->used != page->styling) {
			sheets_free(entries[index]);
			continue;
		}

		/* The entry stays. */
		entries[kept] = entries[index];
		kept++;
	}

	/* The table holds the kept entries. */
	page->sheets.count = kept;
}

/* Frees one entry: its fetch, sheet, text and location. */
static void
sheets_free(
	struct page_sheet *entry)
{
	/* The fetch under way is cancelled (its callback is not called). */
	net_request_cancel(entry->request);
	css_sheet_destroy(entry->sheet);
	free(entry->text);
	free(entry->location);
	free(entry);
}

/*
 * Fires the load (or error) event of the links whose sheets arrived since
 * the last styling; a handler may change the document, which styles it again.
 */
int
page_sheets_fire_links(
	struct page *page)
{
	struct page_link_event *list;
	size_t count;
	size_t index;
	int canceled;
	int error;

	/* The events are taken out of the page first: a handler may style the page again. */
	count = page->link_events.count;
	if (count == 0)
		return 0;
	list = malloc(count * sizeof(*list));
	if (list == NULL)
		return ENOMEM;
	memcpy(list, page->link_events.items, count * sizeof(*list));
	page->link_events.count = 0;

	/* Each element's event. */
	error = 0;
	for (index = 0; index < count && error == 0; index++)
		error = bind_fire_event(page->window, &list[index].element->node, list[index].failed ? "error" : "load", 0, &canceled);
	free(list);
	return error;
}
