/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Same-origin child responses become native tasks, never script inside loader callbacks. */

#include "page/page.h"
#include "bind/internal.h"
#include "net/net.h"
#include "xml/dom.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One pending response owns a stable precise root and copied metadata until its task ends. */
struct frame_load {
	struct page *page;
	struct dom_element *element;
	struct net_request *request;
	struct net_response response;
	uint64_t epoch;
	int ready;
	int error;
	int blank;
};

static void frame_load_release(struct frame_load *load);
static void frame_load_done(void *context, struct net_request *request);
static int frame_load_drain(struct page *page, int error);
static int frame_load_scan(struct page *page, struct bind_window *window);
static int frame_load_start(struct page *page, struct dom_element *element, struct vm_string *source);
static int frame_load_allowed(struct dom_document *document, const struct wb_buffer *location, int *allowed);
static int frame_load_current(struct frame_load *load);
static int frame_load_activate(struct frame_load *load);
static int frame_load_parse(struct frame_load *load, struct dom_document **document);
static int frame_load_error_document(struct vm_heap *heap, const struct xml_error *failure, struct dom_document **document);
static int frame_load_metadata(struct frame_load *load, struct dom_document *document);
static int frame_load_html(struct bind_window *window, const struct wb_buffer *body);
static int frame_load_inline(void *context, struct dom_element *script);
static int frame_load_xml_scripts(struct bind_window *window);
static int frame_load_events(struct frame_load *load, struct bind_window *window);

/*
 * Initializes the Page's empty child response task ownership.
 */
void
page_frames_init(
	struct page *page)
{
	/* Stable allocated records, rather than movable vector slots, own VM root addresses. */
	wb_vector_init(&page->frame_loads, sizeof(struct frame_load *));
	page->frames_running = 0;

	/* Succeeded: no resource task or temporary root exists yet. */
	return;
}

/*
 * Cancels every child request before the Page heap or its callbacks are released.
 */
void
page_frames_release(
	struct page *page)
{
	struct frame_load **slot;
	size_t index;

	/* Requests lose their callbacks before any native root or Page address is freed. */
	for (index = 0; index < page->frame_loads.count; index++) {
		slot = wb_vector_at(&page->frame_loads, index);
		if (*slot != NULL)
			frame_load_release(*slot);
	}

	/* Release task storage after every stable record is gone. */
	wb_vector_release(&page->frame_loads);

	/* Succeeded: loader completion cannot call this Page again. */
	return;
}

/*
 * Starts changed connected sources and drains one finite completed-task snapshot.
 */
int
page_frames_checkpoint(
	struct page *page)
{
	int status;
	int error;

	/* Parser-time primary checkpoints and reentrant child callbacks defer to a stable outer task. */
	if (page->frames_running || page->parser != NULL)
		return 0;

	/* Scanning starts actual requests; their borrowed completion callbacks only copy C bytes. */
	page->frames_running = 1;
	error = frame_load_scan(page, page->window);
	if (error != 0) {
		/* A failed scan still cancels stale/ready tasks and restores outer execution. */
		status = frame_load_drain(page, error);
		if (status != 0)
			return status;

		/* Succeeded: the failed snapshot has released its terminal records. */
		return 0;
	}

	/* Activates only current ready tasks after a successful scan. */
	error = frame_load_drain(page, 0);
	if (error != 0)
		return error;

	/* A window load that waited for these frames fires when the last is done. */
	error = page_fire_load_resume(page);
	if (error != 0)
		return error;

	/* Succeeded: the finite completed-task snapshot has a terminal outcome. */
	return 0;
}

/* Drains a finite task snapshot and restores the outer checkpoint state on either outcome. */
static int
frame_load_drain(
	struct page *page,
	int error)
{
	struct frame_load **slot;
	struct frame_load *load;
	size_t index;
	size_t count;
	size_t kept;
	int current;

	/* A prior scan failure suppresses activation but still releases terminal records. */
	count = page->frame_loads.count;
	for (index = 0; index < count; index++) {
		/* Re-fetch the vector slot after any script can append and move vector storage. */
		slot = wb_vector_at(&page->frame_loads, index);
		load = *slot;
		if (load == NULL)
			continue;

		/* Removed, adopted, reinserted and superseded sources cannot activate a stale response. */
		current = frame_load_current(load);
		if (!current || load->ready) {
			/* Remove the task's public slot before execution can cause another checkpoint. */
			*slot = NULL;
			if (current && error == 0) {
				error = frame_load_activate(load);
				if (error != 0) {
					frame_load_release(load);
					continue;
				}
			}

			/* Releases the terminal record after its callbacks have unwound. */
			frame_load_release(load);
		}
	}

	/* Compact only after the snapshot so callbacks never change an outstanding loop index. */
	kept = 0;
	for (index = 0; index < page->frame_loads.count; index++) {
		slot = wb_vector_at(&page->frame_loads, index);
		if (*slot != NULL) {
			load = *slot;
			slot = wb_vector_at(&page->frame_loads, kept);
			*slot = load;
			kept++;
		}
	}

	/* Restore ordinary task execution even when allocation or interpretation failed. */
	page->frame_loads.count = kept;
	page->frames_running = 0;
	if (error != 0)
		return error;

	/* Succeeded: every ready task in this snapshot has a terminal outcome. */
	return 0;
}

/* Releases one owning C task after suppressing its still-pending callback. */
static void
frame_load_release(
	struct frame_load *load)
{
	/* Cancellation never runs a callback and precedes removal of its native owner root. */
	if (load->request != NULL)
		net_request_cancel(load->request);

	/* Drops the stable native root before releasing copied response storage. */
	vm_heap_remove_root(load->page->heap, (struct vm_cell **)&load->element);
	net_response_release(&load->response);
	free(load);

	/* Succeeded: neither loader nor heap retains this task record. */
	return;
}

/* Copies a borrowed response without invoking VM allocation, script or events. */
static void
frame_load_done(
	void *context,
	struct net_request *request)
{
	struct frame_load *load;
	const struct net_response *response;
	int error;

	/* Loader request storage ceases to belong to this task on completion. */
	load = context;
	load->request = NULL;
	error = net_request_error(request);
	if (error != 0) {
		load->error = error;
		load->ready = 1;
		return;
	}

	/* Copies actual response metadata while the loader still owns the borrowed bytes. */
	response = net_request_response(request);
	load->response.status = response->status;
	error = wb_buffer_append(&load->response.url, response->url.data, response->url.length);
	if (error != 0) {
		load->error = error;
		load->ready = 1;
		return;
	}

	/* Copies the declaration independently of the final URL. */
	error = wb_buffer_append(&load->response.content_type, response->content_type.data, response->content_type.length);
	if (error != 0) {
		load->error = error;
		load->ready = 1;
		return;
	}

	/* Copies the complete body before marking the response ready. */
	error = wb_buffer_append(&load->response.body, response->body.data, response->body.length);
	if (error != 0) {
		load->error = error;
		load->ready = 1;
		return;
	}

	/* Ready means the next outer Page checkpoint may inspect the real response outcome. */
	load->error = error;
	load->ready = 1;

	/* Succeeded: the callback has queued only native C work. */
	return;
}

/* Walks each actual active Document iteratively, descending only bounded child contexts. */
static int
frame_load_scan(
	struct page *page,
	struct bind_window *window)
{
	struct dom_node *walk;
	struct dom_element *element;
	struct vm_string *source;
	struct vm_string *sandbox;
	struct vm_realm *child;
	struct bind_window *nested;
	int same;
	int error;

	/* Retired owners and maximum-depth contexts create no further child tasks. */
	if (window->detached || window->context_depth >= 64U)
		return 0;

	/* No script executes during source scanning, so native traversal links remain stable. */
	walk = window->document->node.first_child;
	while (walk != NULL) {
		/* Only real HTML embedding interfaces participate in navigation. */
		if (walk->type == DOM_ELEMENT) {
			element = (struct dom_element *)walk;
			if (element->ns == DOM_NS_HTML &&
			    (element->tag == DOM_TAG_IFRAME || element->tag == DOM_TAG_OBJECT)) {
				/* Unsupported sandbox contexts never publish a new accessible resource owner. */
				sandbox = dom_attribute_ascii(element, "sandbox");
				if (sandbox == NULL) {
					/* Object data and iframe src are ordinary live native attributes. */
					if (element->tag == DOM_TAG_IFRAME) {
						source = dom_attribute_ascii(element, "src");
					} else {
						source = dom_attribute_ascii(element, "data");
					}

					/* String equality prevents reloading when an unchanged attribute has a new allocation. */
					same = 0;
					if (source == element->child_source) {
						same = 1;
					} else if (source != NULL && element->child_source != NULL) {
						same = vm_string_equal(source, element->child_source);
					}

					/* First observation and changed source each establish one actual attempt. */
					if (!element->child_load_seen || !same) {
						error = frame_load_start(page, element, source);
						if (error != 0)
							return error;
					}
				}

				/* Descend through the current real child, never an unverified URL alias. */
				child = (struct vm_realm *)element->child_context;
				if (child != NULL) {
					nested = child->host;
					if (nested != NULL) {
						error = frame_load_scan(page, nested);
						if (error != 0)
							return error;
					}
				}
			}
		}

		/* Advance within the current Document without recursive native DOM traversal. */
		walk = bind_following(walk, &window->document->node);
	}

	/* Succeeded: connected changed sources now have native pending records. */
	return 0;
}

/* Starts one real source attempt with a stable native owner and copied complete URL. */
static int
frame_load_start(
	struct page *page,
	struct dom_element *element,
	struct vm_string *source)
{
	struct frame_load *load;
	struct net_url base;
	struct net_url parsed;
	struct wb_buffer raw;
	struct wb_buffer location;
	int blank;
	int allowed;
	int remote;
	int error;

	/* Publish attempted source identity before any callback or recursively observed checkpoint. */
	element->child_source = source;
	element->child_load_seen = 1;
	element->child_epoch++;
	blank = 0;
	if (element->tag == DOM_TAG_IFRAME) {
		/* Absence, empty source and about:blank use the genuine initial blank owner. */
		if (source == NULL) {
			blank = 1;
		} else if (source->length == 0) {
			blank = 1;
		} else {
			blank = vm_string_equal_ascii(source, "about:blank");
		}
	}

	/* Objects without a usable data source have no resource to navigate. */
	if (!blank && source == NULL)
		return 0;

	/* Resolve actual source text using this Document's base, before allocating request storage. */
	wb_buffer_init(&raw);
	wb_buffer_init(&location);
	memset(&base, 0, sizeof(base));
	memset(&parsed, 0, sizeof(parsed));
	error = 0;
	allowed = blank;
	if (!blank) {
		error = vm_string_to_utf8(source, &raw);
		if (error != 0)
			goto release_urls;

		/* Uses this Document's actual base before resolving source text. */
		error = bind_reflection_base(element->node.document, &base);
		if (error != 0)
			goto release_urls;

		/* Parses the source against the validated native base. */
		error = net_url_parse(wb_buffer_string(&raw), raw.length, &base, &parsed);
		if (error != 0)
			goto release_urls;

		/* Copies the complete request location before checking origin. */
		error = net_url_serialize(&parsed, 0, &location);
		if (error != 0)
			goto release_urls;

		/* Allows only the established native same-origin resource profile. */
		error = frame_load_allowed(element->node.document, &location, &allowed);
		if (error != 0)
			goto release_urls;
	}

release_urls:

	/* Syntax and unsupported origin refusals do not expose a normal foreign Document. */
	net_url_release(&base);
	net_url_release(&parsed);
	wb_buffer_release(&raw);
	if (error != 0 || !allowed) {
		wb_buffer_release(&location);
		if (error == ENOMEM)
			return error;
		return 0;
	}

	/* A stable allocated slot owns the temporary embedding element root. */
	load = calloc(1, sizeof(*load));
	if (load == NULL) {
		wb_buffer_release(&location);
		return ENOMEM;
	}

	/* Initialize owning response buffers before any request can complete synchronously. */
	load->page = page;
	load->element = element;
	load->epoch = element->child_epoch;
	load->blank = blank;
	net_response_init(&load->response);
	error = vm_heap_add_root(page->heap, (struct vm_cell **)&load->element);
	if (error != 0) {
		wb_buffer_release(&location);
		net_response_release(&load->response);
		free(load);
		return error;
	}

	/* Real asynchronous web loading is required; no synchronous HTTP fallback blocks the Page. */
	remote = page_net_is_remote(wb_buffer_string(&location));
	if (blank) {
		load->ready = 1;
	} else if (remote && page->loader != NULL) {
		error = net_loader_fetch(page->loader, wb_buffer_string(&location), frame_load_done, load, &load->request);
		if (error != 0) {
			wb_buffer_release(&location);
			frame_load_release(load);
			return error;
		}
	} else if (!remote) {
		error = page_fetch_response("about:blank", wb_buffer_string(&location), &load->response);
		if (error != 0) {
			/* A local resource refusal is the task outcome, not a setup failure. */
			load->error = error;
			error = 0;
		}

		/* Local completion is consumed by the same outer task as network completion. */
		load->ready = 1;
	} else {
		load->error = ENOTSUP;
		load->ready = 1;
	}

	/* Store one task only after request setup has an explicit owning outcome. */
	wb_buffer_release(&location);

	/* Publishes the stable owning task after all request setup has succeeded. */
	error = wb_vector_push(&page->frame_loads, &load);
	if (error != 0) {
		frame_load_release(load);
		return error;
	}

	/* Succeeded: the actual response will be consumed at an outer checkpoint. */
	return 0;
}

/* Checks actual native origin before requests and again against their final redirect URL. */
static int
frame_load_allowed(
	struct dom_document *document,
	const struct wb_buffer *location,
	int *allowed)
{
	struct wb_buffer owner;
	struct wb_buffer parent_origin;
	struct wb_buffer child_origin;
	struct net_url parent;
	struct net_url child;
	int parent_file;
	int child_file;
	int web;
	int same;
	int error;

	/* Origin comes from the owning Document fallback, independently of a foreign first base. */
	*allowed = 0;
	wb_buffer_init(&owner);
	wb_buffer_init(&parent_origin);
	wb_buffer_init(&child_origin);
	memset(&parent, 0, sizeof(parent));
	memset(&child, 0, sizeof(child));
	error = bind_document_url(document, 1, &owner);
	if (error != 0)
		goto release_origins;

	/* Parses the owner without using the possibly foreign first base. */
	error = net_url_parse(wb_buffer_string(&owner), owner.length, NULL, &parent);
	if (error != 0)
		goto release_origins;

	/* Parses the complete requested or redirected location. */
	error = net_url_parse(wb_buffer_string(location), location->length, NULL, &child);
	if (error != 0)
		goto release_origins;

	/* Local file support remains the explicit same-tab native file profile. */
	parent_file = strcmp(parent.scheme, "file");
	child_file = strcmp(child.scheme, "file");
	web = net_http_is_web(parent.scheme);
	if (parent_file == 0 && child_file == 0) {
		*allowed = 1;
	} else if (web) {
		/* A web owner may expose only its actual tuple origin. */
		error = net_url_component(&parent, NET_URL_ORIGIN, &parent_origin);
		if (error != 0)
			goto release_origins;

		/* Compares an independently copied child origin, including default-port normalization. */
		error = net_url_component(&child, NET_URL_ORIGIN, &child_origin);
		if (error != 0)
			goto release_origins;

		/* Equal complete origin bytes establish accessibility. */
		if (parent_origin.length == child_origin.length) {
			same = memcmp(parent_origin.data, child_origin.data, parent_origin.length);
			if (same == 0)
				*allowed = 1;
		}
	}

release_origins:

	/* No parsed native URL or copied origin survives this verification. */
	net_url_release(&parent);
	net_url_release(&child);
	wb_buffer_release(&owner);
	wb_buffer_release(&parent_origin);
	wb_buffer_release(&child_origin);
	if (error != 0)
		return error;

	/* Succeeded: allowed records only a verified native origin match. */
	return 0;
}

/* Validates generation, actual source, active owner and connection without running script. */
static int
frame_load_current(
	struct frame_load *load)
{
	struct dom_element *element;
	struct bind_window *window;
	struct vm_string *source;
	struct vm_string *sandbox;
	int connected;
	int same;

	/* Removal increments generation even when reinsertion happens before completion. */
	element = load->element;
	if (element->child_epoch != load->epoch)
		return 0;
	window = element->node.document->view;
	if (window == NULL || window->detached)
		return 0;
	connected = dom_is_inclusive_ancestor(&window->document->node, &element->node);
	if (!connected)
		return 0;
	sandbox = dom_attribute_ascii(element, "sandbox");
	if (sandbox != NULL)
		return 0;

	/* Attribute edits during completion events invalidate the remaining work immediately. */
	if (element->tag == DOM_TAG_IFRAME) {
		source = dom_attribute_ascii(element, "src");
	} else {
		source = dom_attribute_ascii(element, "data");
	}

	/* Absence remains a real distinct source state. */
	if (source == element->child_source)
		return 1;
	if (source == NULL || element->child_source == NULL)
		return 0;
	same = vm_string_equal(source, element->child_source);
	if (!same)
		return 0;

	/* Succeeded: the pending task still belongs to the current connected attempt. */
	return 1;
}

/* Publishes a verified supported parse before inline scripts and actual completion events. */
static int
frame_load_activate(
	struct frame_load *load)
{
	struct dom_document *document;
	struct bind_window *parent;
	struct bind_window *window;
	struct vm_realm *realm;
	struct vm_cell *roots[2];
	unsigned index;
	unsigned registered;
	int allowed;
	int current;
	int error;

	/* Network failures remain failed resources rather than forged successful child loads. */
	if (load->error != 0) {
		if (load->error == ENOMEM)
			return ENOMEM;
		return 0;
	}
	if (load->element->tag == DOM_TAG_OBJECT && !load->blank &&
	    (load->response.status < 200 || load->response.status > 299))
		return 0;

	/* Redirects must retain the verified actual origin before any parsed DOM is exposed. */
	if (!load->blank) {
		error = frame_load_allowed(load->element->node.document, &load->response.url, &allowed);
		if (error != 0)
			return error;
		if (!allowed)
			return 0;
	}

	/* Protect a not-yet-published Document and the executing realm across every VM allocation. */
	roots[0] = NULL;
	roots[1] = NULL;
	registered = 0;
	error = 0;
	for (index = 0; index < 2U; index++) {
		error = vm_heap_add_root(load->page->heap, &roots[index]);
		if (error != 0)
			goto release_roots;

		/* Keeps partial publication recoverable if the next registration fails. */
		registered++;
	}

	/* A previously observed initial blank pair completes in place once, preserving synchronous DOM edits. */
	document = NULL;
	window = NULL;
	parent = load->element->node.document->view;
	realm = (struct vm_realm *)load->element->child_context;
	if (realm != NULL) {
		window = realm->host;
		if (window->document->resource_url != NULL)
			window = NULL;
	}

	/* Existing initial blank ownership is distinct from replacement of a loaded resource. */
	if (load->blank && window != NULL) {
		realm = (struct vm_realm *)load->element->child_context;
		window = realm->host;
		document = window->document;
		roots[0] = &document->node.cell;
		roots[1] = &realm->cell;
	} else {
		/* No earlier blank context can stand in for a refused response. */
		window = NULL;

		/* Strict XML projection finishes before realm publication or script execution. */
		error = frame_load_parse(load, &document);
		if (error != 0) {
			/* Unsupported MIME/ordinary refusal still creates no accessible child. */
			if (error == ENOTSUP || error == EINVAL)
				error = 0;
			goto release_roots;
		}

		/* Publishes the parsed Document root before metadata allocates VM strings. */
		roots[0] = &document->node.cell;
		if (!load->blank) {
			error = frame_load_metadata(load, document);
			if (error != 0)
				goto release_roots;
		}

		/* Installs the actual parsed graph, never an earlier blank substitute. */
		error = bind_frame_install(parent, load->element, document, &window);
		if (error != 0)
			goto release_roots;

		/* Roots the executing child realm before the parser can invoke scripts. */
		roots[1] = &window->realm->cell;
		if (document->content == DOM_CONTENT_HTML) {
			error = frame_load_html(window, &load->response.body);
		} else {
			error = frame_load_xml_scripts(window);
		}

		/* A parser or interpreter failure suppresses completion events. */
		if (error != 0)
			goto release_roots;
	}

	/* Actual completion events require the same current native attempt after all script callbacks. */
	current = frame_load_current(load);
	if (window != NULL && current) {
		/* The parent can observe this verified child navigation before its load event. */
		if (!load->blank) {
			error = page_record_visited(load->page, &load->response.url);
			if (error != 0)
				goto release_roots;
		}
		error = frame_load_events(load, window);
		if (error != 0)
			goto release_roots;
	}

release_roots:

	/* Temporary owners end after scripts and callbacks have unwound. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(load->page->heap, &roots[registered]);
	}

	/* Propagate fatal native failure without declaring a successful load. */
	if (error != 0)
		return error;

	/* Succeeded: only an actual supported current resource could produce a load event. */
	return 0;
}

/* Selects HTML or strictly parses the full XML input before publishing any native graph. */
static int
frame_load_parse(
	struct frame_load *load,
	struct dom_document **document)
{
	struct xml_document *model;
	struct xml_error failure;
	enum dom_document_content content;
	int error;

	/* Missing or unsupported declaration is a real refusal, not content sniffing. */
	*document = NULL;
	content = DOM_CONTENT_HTML;
	if (!load->blank) {
		error = page_document_content(wb_buffer_string(&load->response.content_type), load->response.content_type.length, &content);
		if (error != 0)
			return error;
	}

	/* HTML owns an initially empty native Document before production parser activation. */
	if (content == DOM_CONTENT_HTML) {
		*document = dom_document_create(load->page->heap);
		if (*document == NULL)
			return ENOMEM;

		/* Succeeded: the empty HTML owner is ready for actual parser activation. */
		return 0;
	}

	/* The entire XML input must be well-formed before scripts or partial nodes become accessible. */
	model = NULL;
	memset(&failure, 0, sizeof(failure));
	error = xml_document_parse(&model, load->response.body.data, load->response.body.length, &failure);
	if (error == 0) {
		error = xml_document_project(load->page->heap, model, content, document);
		if (error != 0) {
			xml_document_destroy(model);
			return error;
		}

		/* Releases the C parse model after its native graph is complete. */
		xml_document_destroy(model);
	} else if (error != ENOMEM) {
		/* Ordinary parse errors produce a real private error Document without recovered script nodes. */
		error = frame_load_error_document(load->page->heap, &failure, document);
	}

	/* Preserve allocation failure as a failed native task. */
	if (error != 0)
		return error;

	/* Succeeded: the actual native graph reflects full parse or explicit parse failure. */
	return 0;
}

/* Builds an actual error graph whose text records the strict parser's original offset and status. */
static int
frame_load_error_document(
	struct vm_heap *heap,
	const struct xml_error *failure,
	struct dom_document **document)
{
	struct dom_document *made;
	struct dom_element *element;
	struct dom_node *text;
	struct vm_string *name;
	struct vm_cell *root;
	struct wb_units units;
	char message[96];
	int length;
	int error;

	/* Prepares nullable ownership before any native construction. */
	*document = NULL;
	root = NULL;
	wb_units_init(&units);
	error = vm_heap_add_root(heap, &root);
	if (error != 0)
		return error;

	/* Creates an XML error owner without any recovered original child. */
	made = dom_document_create(heap);
	if (made == NULL) {
		error = ENOMEM;
		goto release_graph;
	}

	/* Roots the owner before allocating its error element. */
	root = &made->node.cell;
	made->content = DOM_CONTENT_XML;
	name = vm_atom_from_ascii(heap, "parsererror");
	if (name == NULL) {
		error = ENOMEM;
		goto release_graph;
	}

	/* Constructs the private error element in the actual native graph. */
	element = dom_element_create(made, DOM_NS_OTHER, name, NULL);
	if (element == NULL) {
		error = ENOMEM;
		goto release_graph;
	}

	/* The rooted Document owns the element before its exact URI is allocated. */
	dom_append_child(&made->node, &element->node);
	element->namespace_uri = vm_string_from_utf8(heap, "urn:zedbsd:xml-parser-error", 27);
	if (element->namespace_uri == NULL) {
		error = ENOMEM;
		goto release_graph;
	}

	/* Embeds the strict parser's status and offset independently of resource identity. */
	length = snprintf(message, sizeof(message), "XML parse error %d at UTF16 offset %lu", failure->status, (unsigned long)failure->offset);
	if (length < 0) {
		error = EIO;
		goto release_graph;
	}

	/* Refuses any truncation before the reported length can exceed stored bytes. */
	if ((size_t)length >= sizeof(message)) {
		error = EOVERFLOW;
		goto release_graph;
	}

	/* Copies only the actual formatted bytes into temporary UTF-16 storage. */
	if (length > 0) {
		error = wb_utf8_to_units((const unsigned char *)message, (size_t)length, &units);
		if (error != 0)
			goto release_graph;
	}

	/* Creates the native text before publishing the complete error graph. */
	text = dom_text_create(made, units.data, units.length);
	if (text == NULL) {
		error = ENOMEM;
		goto release_graph;
	}

	/* Publishes no original script or recovered partial tree. */
	dom_append_child(&element->node, text);
	*document = made;

release_graph:
	/* Releases temporary representation and root ownership on either outcome. */
	wb_units_release(&units);
	vm_heap_remove_root(heap, &root);
	if (error != 0)
		return error;

	/* Succeeded: the caller receives only the complete private error graph. */
	return 0;
}

/* Stores the actual final URL and lowercase declared MIME essence as traced native metadata. */
static int
frame_load_metadata(
	struct frame_load *load,
	struct dom_document *document)
{
	struct wb_buffer essence;
	const unsigned char *mime;
	size_t first;
	size_t end;
	size_t index;
	unsigned char byte;
	int error;

	/* The protected Document retains each metadata allocation before the next one. */
	document->resource_url = vm_string_from_utf8(document->heap, wb_buffer_string(&load->response.url), load->response.url.length);
	if (document->resource_url == NULL)
		return ENOMEM;

	/* MIME validation already succeeded; retain its exact declared essence rather than an enum alias. */
	mime = load->response.content_type.data;
	first = 0;
	end = load->response.content_type.length;
	while (first < end) {
		byte = mime[first];
		if (byte != ' ' && byte != '\t')
			break;
		first++;
	}

	/* Parameters do not belong to Document.contentType. */
	for (index = first; index < end; index++) {
		if (mime[index] == ';') {
			end = index;
			break;
		}
	}

	/* Remove HTTP trailing whitespace before lowercasing the validated essence. */
	while (end > first) {
		byte = mime[end - 1U];
		if (byte != ' ' && byte != '\t')
			break;
		end--;
	}

	/* C storage owns the normalized declaration before conversion into a traced VM string. */
	wb_buffer_init(&essence);
	error = 0;
	for (index = first; index < end; index++) {
		byte = mime[index];
		if (byte >= 'A' && byte <= 'Z')
			byte += 'a' - 'A';
		error = wb_buffer_append(&essence, &byte, 1U);
		if (error != 0) {
			wb_buffer_release(&essence);
			return error;
		}
	}

	/* The actual declared text/xml remains text/xml even when the native content kind is XML. */
	document->resource_mime = vm_string_from_utf8(document->heap, wb_buffer_string(&essence), essence.length);
	if (document->resource_mime == NULL) {
		wb_buffer_release(&essence);
		return ENOMEM;
	}

	/* Release normalization storage before returning either outcome. */
	wb_buffer_release(&essence);

	/* Succeeded: metadata is owned by the actual Document graph. */
	return 0;
}

/* Feeds real HTML input under traced active-parser ownership in the actual child realm. */
static int
frame_load_html(
	struct bind_window *window,
	const struct wb_buffer *body)
{
	struct wb_units units;
	struct html_parser *parser;
	const unsigned char *bytes;
	size_t length;
	int error;
	int active;

	/* UTF8 and its ordinary BOM handling match the primary production HTML input path. */
	bytes = body->data;
	length = body->length;
	if (length >= 3U &&
	    bytes[0] == 0xefU &&
	    bytes[1] == 0xbbU &&
	    bytes[2] == 0xbfU) {
		bytes += 3U;
		length -= 3U;
	}

	/* Prepares both temporary input and nullable parser ownership. */
	wb_units_init(&units);
	parser = NULL;
	active = 0;
	error = wb_utf8_to_units(bytes, length, &units);
	if (error != 0)
		goto release_parser;

	/* Creates the actual child parser only after complete byte decoding. */
	error = html_parser_create(&parser, window->document, 1);
	if (error != 0)
		goto release_parser;

	/* Transfers parser tracing to the Window before script-triggered allocation. */
	window->document_parser = parser;
	html_parser_transfer_ownership(parser);
	html_parser_set_script_hook(parser, frame_load_inline, window);
	window->document_parser_depth++;
	active = 1;
	error = html_parser_feed(parser, units.data, units.length);
	if (error != 0)
		goto release_parser;

	/* Processes actual EOF after all fed input succeeds. */
	error = html_parser_finish(parser);
	if (error != 0)
		goto release_parser;

release_parser:
	/* Removes only ownership that this call actually published. */
	if (active) {
		window->document_parser_depth--;
		window->document_parser = NULL;
		window->document_parser_close = 0;
	}

	/* No parser source or insertion stack survives the completed call. */
	if (parser != NULL)
		html_parser_destroy(parser);
	wb_units_release(&units);
	if (error != 0)
		return error;

	/* Succeeded: the real HTML parser has reached EOF in the actual child realm. */
	return 0;
}

/* Invokes the existing classic inline preparation from the production HTML parser. */
static int
frame_load_inline(
	void *context,
	struct dom_element *script)
{
	int error;

	/* The shared hook preserves document.write insertion and ordinary child globals. */
	error = bind_run_inline_script(context, script);
	if (error != 0)
		return error;

	/* Succeeded: this inline script completed through the actual interpreter. */
	return 0;
}

/* Runs only actual lowercase HTML-namespace scripts from the fully validated native XML tree. */
static int
frame_load_xml_scripts(
	struct bind_window *window)
{
	struct dom_node *walk;
	struct dom_element *element;
	struct vm_cell *root;
	int script;
	int error;

	/* A saved traversal node remains precise while a preceding script mutates the tree. */
	root = NULL;
	error = vm_heap_add_root(window->realm->heap, &root);
	if (error != 0)
		return error;

	/* Native namespace and exact local spelling determine eligibility, never test identity or tag folding. */
	walk = window->document->node.first_child;
	while (walk != NULL && !window->detached) {
		root = &walk->cell;
		script = 0;
		if (walk->type == DOM_ELEMENT) {
			element = (struct dom_element *)walk;
			if (element->ns == DOM_NS_HTML)
				script = vm_string_equal_ascii(element->local_name, "script");
		}

		/* Compute and retain the following native node before actual script callbacks. */
		walk = bind_following(walk, &window->document->node);
		if (walk != NULL)
			root = &walk->cell;
		if (script) {
			error = bind_run_inline_script(window, element);
			if (error != 0) {
				vm_heap_remove_root(window->realm->heap, &root);
				return error;
			}
		}
	}

	/* The active realm root protects the current script; release traversal ownership after callbacks. */
	vm_heap_remove_root(window->realm->heap, &root);

	/* Succeeded: only validated namespace-eligible supported inline code executed. */
	return 0;
}

/* Dispatches actual child completion and then its still-connected embedding load event. */
static int
frame_load_events(
	struct frame_load *load,
	struct bind_window *window)
{
	struct bind_window *parent;
	int canceled;
	int current;
	int error;

	/* Document readiness becomes interactive only after actual supported parsing. */
	bind_window_set_ready_state(window, "interactive");
	error = bind_fire_event(window, &window->document->node, "DOMContentLoaded", BIND_EVENT_BUBBLES, &canceled);
	if (error != 0)
		return error;
	current = frame_load_current(load);
	if (!current || window->detached)
		return 0;

	/* A callback may have removed or superseded the native attempt before Window completion. */
	bind_window_set_ready_state(window, "complete");
	error = bind_fire_event(window, NULL, "load", BIND_EVENT_DOCUMENT, &canceled);
	if (error != 0)
		return error;
	current = frame_load_current(load);
	if (!current || window->detached)
		return 0;

	/* The actual owning parent dispatches the embedding element event after child completion. */
	parent = load->element->node.document->view;
	error = bind_fire_event(parent, &load->element->node, "load", 0, &canceled);
	if (error != 0)
		return error;

	/* Succeeded: the real current resource completed exactly one native load task. */
	return 0;
}
