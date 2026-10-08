/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Links: the <a href> under a point of the laid out page, and a link's
 * target resolved against the file the page came from.
 *
 * A target is resolved as a URL against the page's location (the
 * absolute path of its file, or its URL) with the WHATWG URL parser
 * (net/url.c): a file: target becomes a path again; data: and http:
 * targets stay URLs, and page_fetch reads what any of them names.
 */

#include "page/page.h"
#include "net/net.h"

#include <errno.h>
#include <string.h>

/* The deepest element nesting searched for a link (the parser caps nesting too). */
#define LINK_DEPTH 512

static const struct layout_box *link_image_box(const struct layout_tree *tree, const struct layout_box *box, layout_unit x, layout_unit y, int depth);

static int link_resolve(const char *base, const char *href, struct net_url *target);
static int link_named(const char *scheme, const char *name);
static const char *link_file_mime(const char *path);

/*
 * Finds the link under a point of the page (pixels from the top left of
 * the document) and writes its href as UTF-8; *found says whether there
 * was one.
 */
int
page_link_at(
	struct page *page,
	int x,
	int y,
	struct wb_buffer *href,
	int *found)
{
	const struct layout_box *box;
	const struct dom_node *node;
	const struct dom_attribute *attribute;
	struct vm_string *name;
	int is_link;
	int depth;
	int error;

	/* Nothing is found until an <a href> is. */
	*found = 0;

	/* An unlaid page has no hit-tested link geometry. */
	if (!page->laid_out)
		return 0;

	/* The text under the point. */
	box = layout_hit(&page->layout, (layout_unit)x * LAYOUT_UNIT, (layout_unit)y * LAYOUT_UNIT);
	if (box == NULL || box->node == NULL)
		return 0;

	/* The attribute's name, as the atom the elements keep. */
	name = vm_atom_from_ascii(page->heap, "href");
	if (name == NULL)
		return ENOMEM;

	/* The nearest <a> with an href among the text's ancestors. */
	node = box->node;
	for (depth = 0; node != NULL && depth < LINK_DEPTH; depth++) {
		/* An HTML <a> element with an href is the link. */
		is_link = dom_element_is(node, DOM_NS_HTML, DOM_TAG_A);
		if (is_link) {
			attribute = dom_element_find_attribute((const struct dom_element *)node, DOM_NS_NONE, name);
			if (attribute != NULL)
				break;
		}

		/* Otherwise its parent. */
		node = node->parent;
	}

	/* No link around the text. */
	if (node == NULL || depth == LINK_DEPTH)
		return 0;

	/* The href's value. */
	error = vm_string_to_utf8(attribute->value, href);
	if (error != 0)
		return error;

	/* Publishes the copied href as a genuine hit-tested link. */
	*found = 1;

	/* Succeeded: the caller receives the actual ancestor link's href. */
	return 0;
}

/*
 * Resolves a link's target (a URL, usually relative) against the absolute
 * path of the page's file and writes the absolute path of the file it
 * names.
 *
 * Returns EINVAL for a target that is not a URL, and EPROTONOSUPPORT for a
 * URL of another scheme than file.
 */
int
page_resolve_file(
	const char *base,
	const char *href,
	struct wb_buffer *out)
{
	struct net_url target;
	int error;

	/* The target against the page's file. */
	error = link_resolve(base, href, &target);
	if (error != 0)
		return error;

	/* The file a file: URL names. */
	error = net_url_file_path(&target, out);
	if (error != 0) {
		net_url_release(&target);
		return error;
	}

	/* The copied file path no longer borrows the resolved URL. */
	net_url_release(&target);

	/* Succeeded: the target's path is written. */
	return 0;
}

/*
 * Resolves a link's target against a page's location (the absolute path
 * of its file, or its URL) and writes the target's location: the path of
 * a file: URL, or the URL itself for other schemes.
 */
int
page_resolve_location(
	const char *base,
	const char *href,
	struct wb_buffer *out)
{
	struct net_url target;
	int error;

	/* The target against the page's location. */
	error = link_resolve(base, href, &target);
	if (error != 0)
		return error;

	/* A local file's path, or the URL. */
	error = net_url_file_path(&target, out);
	if (error == EPROTONOSUPPORT || error == ENOENT) {
		/* Non-file schemes preserve their complete URL instead of a filesystem path. */
		wb_buffer_clear(out);
		error = net_url_serialize(&target, 0, out);
		if (error != 0) {
			net_url_release(&target);
			return error;
		}
	} else if (error != 0) {
		/* Other file-path failures retain their actual error. */
		net_url_release(&target);
		return error;
	}

	/* The target is written. */
	net_url_release(&target);

	/* Succeeded: the location is written. */
	return 0;
}

/*
 * Reads what a URL (resolved against a page's location) names, as a
 * script, a stylesheet or an image would: a data: URL's body, a file:
 * URL's file, or an http: URL's response body.  The final URL (after
 * redirects) goes to final_url when it is not NULL.  Returns
 * EPROTONOSUPPORT for a URL of another scheme.
 */
int
page_fetch(
	const char *base,
	const char *href,
	struct wb_buffer *bytes,
	struct wb_buffer *final_url)
{
	struct wb_buffer path;
	struct wb_buffer text;
	struct net_url target;
	struct net_data data;
	struct net_response response;
	int is_data;
	int is_http;
	int error;

	/* The target against the page's location. */
	error = link_resolve(base, href, &target);
	if (error != 0)
		return error;

	/* Publishes the initially resolved URL when the caller requested it. */
	if (final_url != NULL) {
		error = net_url_serialize(&target, 0, final_url);
		if (error != 0) {
			net_url_release(&target);
			return error;
		}
	}

	/* A data: URL carries its bytes. */
	is_data = link_named(target.scheme, "data");

	/* Tests HTTPS only when the actual scheme was not already HTTP. */
	is_http = link_named(target.scheme, "http");
	if (!is_http)
		is_http = link_named(target.scheme, "https");

	/* Decoded data fields are independently owned after successful parsing. */
	if (is_data) {
		error = net_data_parse(&target, &data);
		if (error != 0) {
			net_url_release(&target);
			return error;
		}

		/* Copies the genuine decoded body and releases both data fields on failure. */
		error = wb_buffer_append(bytes, data.body.data, data.body.length);
		if (error != 0) {
			net_data_release(&data);
			net_url_release(&target);
			return error;
		}

		/* The caller's copied bytes no longer borrow decoded fields or URL storage. */
		net_data_release(&data);
		net_url_release(&target);

		/* Succeeded: the caller owns the decoded data resource bytes. */
		return 0;
	}

	/* An http: URL is fetched; its final URL replaces the one asked for. */
	if (is_http) {
		wb_buffer_init(&text);
		error = net_url_serialize(&target, 0, &text);
		if (error != 0) {
			net_url_release(&target);
			wb_buffer_release(&text);
			return error;
		}

		/* Fetches the serialized target after its C URL storage is no longer needed. */
		net_url_release(&target);
		error = net_http_fetch(wb_buffer_string(&text), &response);
		if (error != 0) {
			wb_buffer_release(&text);
			return error;
		}

		/* Copies the owned HTTP body after releasing its completed request text. */
		wb_buffer_release(&text);
		error = wb_buffer_append(bytes, response.body.data, response.body.length);
		if (error != 0) {
			net_response_release(&response);
			return error;
		}

		/* Replaces the original location with the actual redirect result when requested. */
		if (final_url != NULL) {
			wb_buffer_clear(final_url);
			error = wb_buffer_append(final_url, response.url.data, response.url.length);
			if (error != 0) {
				net_response_release(&response);
				return error;
			}
		}

		/* The response is copied. */
		net_response_release(&response);

		/* Succeeded: bytes and optional final URL reflect the actual HTTP response. */
		return 0;
	}

	/* A file: URL names a file. */
	wb_buffer_init(&path);
	error = net_url_file_path(&target, &path);
	if (error != 0) {
		net_url_release(&target);
		wb_buffer_release(&path);
		return error;
	}

	/* Reads the resolved file after its URL representation is no longer needed. */
	net_url_release(&target);
	error = wb_file_read(wb_buffer_string(&path), bytes);
	if (error != 0) {
		wb_buffer_release(&path);
		return error;
	}

	/* Releases the native path after the file operation no longer borrows it. */
	wb_buffer_release(&path);

	/* Succeeded: the bytes are read. */
	return 0;
}

/*
 * Fetches an owned response with its actual resolved URL, status, declared MIME and body.
 *
 * Fresh output storage is initialized here and remains empty on every failure.
 * Existing byte-only callers retain their separate page_fetch contract.
 */
int
page_fetch_response(
	const char *base,
	const char *href,
	struct net_response *response)
{
	struct net_url target;
	struct net_data data;
	struct wb_buffer location;
	struct wb_buffer path;
	const char *mime;
	int is_data;
	int is_http;
	int is_file;
	int status;

	/* Fresh output has a single cleanup owner even when embedding arguments are invalid. */
	if (response == NULL)
		return EINVAL;

	/* Gives the caller a fresh empty response before validating its input locations. */
	net_response_init(response);

	/* Absent input cannot authorize URL parsing or resource acquisition. */
	if (base == NULL || href == NULL)
		return EINVAL;

	/* Resolves the target before any local or network resource work begins. */
	status = link_resolve(base, href, &target);
	if (status != 0)
		return status;

	/* Both scratch buffers share the single forward cleanup boundary below. */
	wb_buffer_init(&location);
	wb_buffer_init(&path);

	/* Classifies the actual resolved scheme without inspecting any document body. */
	is_data = link_named(target.scheme, "data");
	is_http = net_http_is_web(target.scheme);
	is_file = link_named(target.scheme, "file");

	/* HTTP metadata and final redirect URL come from the existing real network implementation. */
	if (is_http) {
		/* Serializes the exact HTTP target before the real network implementation uses it. */
		status = net_url_serialize(&target, 0, &location);
		if (status != 0)
			goto cleanup;

		/* The network response owns its redirect URL, declared MIME and body. */
		status = net_http_fetch(wb_buffer_string(&location), response);
		if (status != 0)
			goto cleanup;
	} else if (is_data) {
		/* A real data URL supplies its MIME and decoded body independently of any DOM root. */
		status = net_data_parse(&target, &data);
		if (status != 0)
			goto cleanup;

		/* Copies the decoded body before its MIME fields can be published. */
		status = wb_buffer_append(&response->body, data.body.data, data.body.length);
		if (status != 0) {
			net_data_release(&data);
			goto cleanup;
		}

		/* Copies actual decoded MIME while both independent data fields remain owned. */
		status = wb_buffer_append(&response->content_type, data.mime.data, data.mime.length);
		if (status != 0) {
			net_data_release(&data);
			goto cleanup;
		}

		/* The response copies no longer borrow either decoded data field. */
		net_data_release(&data);
	} else if (is_file) {
		/* Local resource metadata uses generic decoded extensions, never test names or XML contents. */
		status = net_url_file_path(&target, &path);
		if (status != 0)
			goto cleanup;

		/* Reads the actual file before publishing any extension-derived metadata. */
		status = wb_file_read(wb_buffer_string(&path), &response->body);
		if (status != 0)
			goto cleanup;

		/* Publishes only generic metadata belonging to the real decoded extension. */
		mime = link_file_mime(wb_buffer_string(&path));
		status = wb_buffer_append_string(&response->content_type, mime);
		if (status != 0)
			goto cleanup;
	} else {
		/* Unsupported resource schemes cannot publish a normal response body or metadata. */
		status = EPROTONOSUPPORT;
		goto cleanup;
	}

	/* Successful local and data responses retain their actual URL and ordinary successful status. */
	if (!is_http) {
		status = net_url_serialize(&target, 0, &response->url);
		if (status != 0)
			goto cleanup;

		/* Only complete local/data response fields can carry successful status. */
		response->status = 200;
	}

cleanup:
	/* All independent C URL and working buffers are released before response publication. */
	net_url_release(&target);
	wb_buffer_release(&location);
	wb_buffer_release(&path);

	/* A failed operation leaves the caller the same fresh empty response contract. */
	if (status != 0) {
		net_response_release(response);
		net_response_init(response);
		return status;
	}

	/* Succeeded: the caller owns every actual response field until normal response release. */
	return 0;
}

/* Parses a target against a location (an absolute path is a file: URL; anything else a URL). */
static int
link_resolve(
	const char *base,
	const char *href,
	struct net_url *target)
{
	struct wb_buffer base_text;
	struct net_url base_url;
	int error;

	/* The base as a URL. */
	wb_buffer_init(&base_text);
	error = 0;
	if (base[0] == '/') {
		error = net_url_from_file_path(base, &base_text);
		if (error != 0) {
			wb_buffer_release(&base_text);
			return error;
		}
	} else {
		error = wb_buffer_append_string(&base_text, base);
		if (error != 0) {
			wb_buffer_release(&base_text);
			return error;
		}
	}

	/* The base parsed. */
	error = net_url_parse(wb_buffer_string(&base_text), base_text.length, NULL, &base_url);
	if (error != 0) {
		wb_buffer_release(&base_text);
		return error;
	}

	/* The parsed base owns its representation independently of the scratch text. */
	wb_buffer_release(&base_text);

	/* The target against it. */
	error = net_url_parse(href, strlen(href), &base_url, target);
	if (error != 0) {
		net_url_release(&base_url);
		return error;
	}

	/* The resolved target no longer borrows the independent base URL. */
	net_url_release(&base_url);

	/* Succeeded: the target is parsed. */
	return 0;
}

/* Tells whether a scheme is a name. */
static int
link_named(
	const char *scheme,
	const char *name)
{
	int differs;

	/* The two strings. */
	differs = strcmp(scheme, name);
	if (differs != 0)
		return 0;

	/* Succeeded: the actual resolved scheme is this exact name. */
	return 1;
}

/* Supplies generic local document metadata from a real decoded path extension. */
static const char *
link_file_mime(
	const char *path)
{
	const char *extension;
	const char *cursor;
	char lower[8];
	size_t index;
	int character;
	int same;

	/* A later directory separator invalidates a dot belonging to an enclosing directory. */
	extension = NULL;
	for (cursor = path; *cursor != '\0'; cursor++) {
		/* A directory boundary discards an earlier extension; a dot starts a new one. */
		if (*cursor == '/')
			extension = NULL;
		else if (*cursor == '.')
			extension = cursor + 1;
	}

	/* Missing and long unknown extensions remain an ordinary binary resource. */
	if (extension == NULL)
		return "application/octet-stream";

	/* Folds only a complete short ASCII path extension, bounded by local storage. */
	index = 0;
	while (extension[index] != '\0' && index < sizeof(lower) - 1) {
		/* ASCII uppercase extensions share their ordinary lowercase MIME mapping. */
		character = (unsigned char)extension[index];
		if (character >= 'A' && character <= 'Z')
			character += 'a' - 'A';
		lower[index] = (char)character;
		index++;
	}

	/* Only complete ordinary extensions can supply local document metadata. */
	if (extension[index] != '\0')
		return "application/octet-stream";

	/* HTML's two ordinary decoded path suffixes share its declared local MIME. */
	lower[index] = '\0';
	same = strcmp(lower, "html");
	if (same == 0)
		return "text/html";

	/* The abbreviated HTML suffix has the same processing declaration. */
	same = strcmp(lower, "htm");
	if (same == 0)
		return "text/html";

	/* XHTML's complete suffix declares XML-based HTML processing. */
	same = strcmp(lower, "xhtml");
	if (same == 0)
		return "application/xhtml+xml";

	/* The abbreviated XHTML suffix retains the same XML-based HTML declaration. */
	same = strcmp(lower, "xht");
	if (same == 0)
		return "application/xhtml+xml";

	/* An SVG path declares scalable vector content independently of its root markup. */
	same = strcmp(lower, "svg");
	if (same == 0)
		return "image/svg+xml";

	/* Generic XML paths declare ordinary XML document processing. */
	same = strcmp(lower, "xml");
	if (same == 0)
		return "application/xml";

	/* Unrecognized local formats cannot silently become HTML or XML documents. */
	return "application/octet-stream";
}

/*
 * Finds the image shown at a point of the document (whole pixels, the
 * scroll included; ws189-p003, a picture dragged out of the browser): its
 * decoded bitmap (the page's, borrowed while the page lives) and the
 * absolute URL of its source (empty when its element has no src).
 * Returns 0, ENOENT when no image is there, or an errno value.
 */
int
page_image_at(
	struct page *page,
	int x,
	int y,
	const struct img_bitmap **bitmap,
	struct wb_buffer *source)
{
	const struct layout_box *box;
	const struct dom_attribute *attribute;
	struct wb_buffer written;
	struct net_url target;
	struct vm_string *name;
	int is_element;
	int error;

	/* Nothing yet; an unlaid page has no image geometry. */
	*bitmap = NULL;
	if (!page->laid_out || page->layout.root == NULL)
		return ENOENT;

	/* The last replaced box with an image under the point (the one drawn on top among those of the flow). */
	box = link_image_box(&page->layout, page->layout.root, (layout_unit)x * LAYOUT_UNIT, (layout_unit)y * LAYOUT_UNIT, 0);
	if (box == NULL)
		return ENOENT;
	*bitmap = box->image;

	/* Its element's src, as written. */
	is_element = 0;
	if (box->node != NULL && box->node->type == DOM_ELEMENT)
		is_element = 1;
	if (!is_element)
		return 0;
	name = vm_atom_from_ascii(page->heap, "src");
	if (name == NULL)
		return ENOMEM;
	attribute = dom_element_find_attribute((const struct dom_element *)box->node, DOM_NS_NONE, name);
	if (attribute == NULL)
		return 0;
	wb_buffer_init(&written);
	error = vm_string_to_utf8(attribute->value, &written);
	if (error != 0) {
		wb_buffer_release(&written);
		return error;
	}

	/*
	 * Made an absolute URL against the page's location, a local file's a
	 * file: URL (ws189-p002 F3: another program takes the text as a URL,
	 * not a path); as written when it cannot be.
	 */
	error = EINVAL;
	if (page->base != NULL)
		error = link_resolve(page->base, wb_buffer_string(&written), &target);
	if (error == 0) {
		error = net_url_serialize(&target, 0, source);
		net_url_release(&target);
	}

	/* As written, when it could not be made a URL. */
	if (error != 0) {
		wb_buffer_clear(source);
		error = wb_buffer_append(source, wb_buffer_string(&written), strlen(wb_buffer_string(&written)));
	}

	/* The text as written is not needed any more. */
	wb_buffer_release(&written);
	if (error != 0)
		return error;

	/* Succeeded: the image and its source. */
	return 0;
}

/* Searches a box and its descendants for the last replaced box with an image whose border box holds a point. */
static const struct layout_box *
link_image_box(
	const struct layout_tree *tree,
	const struct layout_box *box,
	layout_unit x,
	layout_unit y,
	int depth)
{
	const struct layout_box *child;
	const struct layout_box *found;
	const struct layout_box *deeper;
	struct layout_rect rect;
	int bounded;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return NULL;

	/* The box itself, when it is an image with its node's bounds around the point. */
	found = NULL;
	if (box->replaced && box->image != NULL && box->node != NULL) {
		bounded = layout_node_bounds(tree, box->node, &rect);
		if (bounded &&
		    x >= rect.x &&
		    y >= rect.y &&
		    x < rect.x + rect.width &&
		    y < rect.y + rect.height)
			found = box;
	}

	/* Its children, a later one over an earlier one. */
	for (child = box->first_child; child != NULL; child = child->next) {
		deeper = link_image_box(tree, child, x, y, depth + 1);
		if (deeper != NULL)
			found = deeper;
	}

	/* Succeeded: the image found, or NULL. */
	return found;
}
