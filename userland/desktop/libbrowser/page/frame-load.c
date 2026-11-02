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
	int srcdoc;
};

/* The painted frame of an iframe the page keeps (the last one made), for a frame whose painting is held. */
struct frame_cache_entry {
	const struct dom_element *element;
	struct paint_frame *frame;
	/* The view transition of the frame's document (see page.h's transition), and how many times it changed. */
	struct layout_transition transition;
	int on;
	unsigned generation;
};

static void frame_load_release(struct frame_load *load);
static void frame_load_done(void *context, struct net_request *request);
static int frame_load_drain(struct page *page, int error);
static int frame_load_scan(struct page *page, struct bind_window *window);
static int frame_load_start(struct page *page, struct dom_element *element, struct vm_string *source);
static int frame_load_allowed(struct dom_document *document, const struct wb_buffer *location, int foreign, int *allowed);
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
	wb_vector_init(&page->frame_cache, sizeof(struct frame_cache_entry));
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

	/* The painted frames the page kept. */
	for (index = 0; index < page->frame_cache.count; index++) {
		paint_frame_release(((struct frame_cache_entry *)wb_vector_at(&page->frame_cache, index))->frame);
		layout_transition_release(&((struct frame_cache_entry *)wb_vector_at(&page->frame_cache, index))->transition);
	}
	wb_vector_release(&page->frame_cache);

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
				{
					/* Object data and iframe src are ordinary live native attributes. */
					if (element->tag == DOM_TAG_IFRAME) {
						source = dom_attribute_ascii(element, "srcdoc");
						if (source == NULL)
							source = dom_attribute_ascii(element, "src");
					} else {
						source = dom_attribute_ascii(element, "data");
					}

					/* String equality prevents reloading when an unchanged attribute has a new allocation. */
					same = 0;
					if (source == element->child_attribute) {
						same = 1;
					} else if (source != NULL && element->child_attribute != NULL) {
						same = vm_string_equal(source, element->child_attribute);
					}

					/* A frame a script navigated before its first load starts at that location: its initial about:blank never loads. */
					if (!element->child_load_seen && element->child_navigation != NULL) {
						element->child_attribute = source;
						error = frame_load_start(page, element, element->child_navigation);
						if (error != 0)
							return error;
					} else if (!element->child_load_seen || !same) {
						/* A changed attribute replaces where a script in the child navigated it. */
						element->child_attribute = source;
						element->child_navigation = NULL;
						error = frame_load_start(page, element, source);
						if (error != 0)
							return error;
					} else if (element->child_navigation != NULL && element->child_navigation != element->child_source) {
						/* The child navigated itself: its new location loads, the attribute unchanged. */
						error = frame_load_start(page, element, element->child_navigation);
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
	int srcdoc;
	int allowed;
	int remote;
	int error;

	/* Publish attempted source identity before any callback or recursively observed checkpoint. */
	element->child_source = source;
	element->child_load_seen = 1;
	element->child_epoch++;
	blank = 0;
	srcdoc = element->tag == DOM_TAG_IFRAME && dom_attribute_ascii(element, "srcdoc") != NULL;
	if (srcdoc) {
		/* The document is the attribute's text, given as the frame's own resource. */
		blank = 0;
	} else if (element->tag == DOM_TAG_IFRAME) {
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
	allowed = blank || srcdoc;
	if (!blank && !srcdoc) {
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
		error = frame_load_allowed(element->node.document, &location, element->tag == DOM_TAG_IFRAME, &allowed);
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
	load->srcdoc = srcdoc;
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
	} else if (srcdoc) {
		/* The srcdoc text is the response: HTML at about:srcdoc. */
		load->ready = 1;
		error = vm_string_to_utf8(source, &load->response.body);
		if (error == 0)
			error = wb_buffer_append_string(&load->response.url, "about:srcdoc");
		if (error == 0)
			error = wb_buffer_append_string(&load->response.content_type, "text/html");
		if (error != 0) {
			wb_buffer_release(&location);
			frame_load_release(load);
			return error;
		}
		load->response.status = 200;
	} else if (remote && page->loader != NULL) {
		error = net_loader_fetch(page->loader, wb_buffer_string(&location), frame_load_done, load, &load->request);
		if (error != 0) {
			wb_buffer_release(&location);
			frame_load_release(load);
			return error;
		}
	} else if (!remote) {
		error = page_fetch_response(page_cookies(load->page), "about:blank", wb_buffer_string(&location), &load->response);
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

/*
 * Checks actual native origin before requests and again against their final redirect URL; with foreign
 * (an iframe), a web page may also embed a web page of another origin, whose Window its scripts reach
 * only across origins.
 */
static int
frame_load_allowed(
	struct dom_document *document,
	const struct wb_buffer *location,
	int foreign,
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

		/* A web document of another origin is embedded as its own origin's. */
		if (foreign && net_http_is_web(child.scheme))
			*allowed = 1;
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
	/* Attribute edits during completion events invalidate the remaining work immediately. */
	if (element->tag == DOM_TAG_IFRAME) {
		source = dom_attribute_ascii(element, "srcdoc");
		if (source == NULL)
			source = dom_attribute_ascii(element, "src");
	} else {
		source = dom_attribute_ascii(element, "data");
	}

	/* Absence remains a real distinct source state. */
	if (source == element->child_attribute)
		return 1;
	if (source == NULL || element->child_attribute == NULL)
		return 0;
	same = vm_string_equal(source, element->child_attribute);
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
	struct bind_window *leaving;
	struct bind_window *window;
	struct vm_realm *realm;
	struct vm_cell *roots[2];
	unsigned index;
	unsigned registered;
	int canceled;
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
	if (!load->blank && !load->srcdoc) {
		error = frame_load_allowed(load->element->node.document, &load->response.url, load->element->tag == DOM_TAG_IFRAME, &allowed);
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

		/* The document that is left is told its page is hidden, and then it is replaced if the frame is still this attempt's. */
		realm = (struct vm_realm *)load->element->child_context;
		leaving = NULL;
		if (realm != NULL)
			leaving = realm->host;
		if (leaving != NULL && leaving->document->resource_url != NULL) {
			bind_window_set_unloading(leaving);
			error = bind_fire_event(leaving, NULL, "pagehide", BIND_EVENT_DOCUMENT, &canceled);
			if (error != 0)
				goto release_roots;
			current = frame_load_current(load);
			if (!current)
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
		if (!load->blank && !load->srcdoc) {
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
	struct bind_window *window;
	struct vm_string *source;
	struct net_url base;
	struct net_url parsed;
	struct wb_buffer raw;
	struct wb_buffer location;
	struct wb_buffer bytes;
	struct wb_units units;
	int error;

	/* A frame whose sandbox lacks allow-scripts runs none. */
	window = context;
	if (window->frame != NULL && !bind_frame_scripts_allowed(window->frame)) {
		script->node.flags |= DOM_NODE_SCRIPT_STARTED;
		return 0;
	}

	/* A script with a source is fetched at once, where the parser stands, and run as the page's own are. */
	source = dom_attribute_ascii(script, "src");
	if (source != NULL && source->length != 0 && window->host.context != NULL) {
		struct page *page;

		page = window->host.context;
		script->node.flags |= DOM_NODE_SCRIPT_STARTED;
		wb_buffer_init(&raw);
		wb_buffer_init(&location);
		wb_buffer_init(&bytes);
		wb_units_init(&units);
		memset(&base, 0, sizeof(base));
		memset(&parsed, 0, sizeof(parsed));
		error = vm_string_to_utf8(source, &raw);
		if (error == 0)
			error = bind_reflection_base(script->node.document, &base);
		if (error == 0)
			error = net_url_parse(wb_buffer_string(&raw), raw.length, &base, &parsed);
		if (error == 0)
			error = net_url_serialize(&parsed, 0, &location);
		if (error == 0)
			error = page_fetch(page_cookies(page), page->base, wb_buffer_string(&location), &bytes, NULL);
		if (error == 0)
			error = wb_utf8_to_units(bytes.data, bytes.length, &units);
		if (error == 0)
			error = bind_run_script_source(window, units.data, units.length, wb_buffer_string(&location), 0);
		net_url_release(&base);
		net_url_release(&parsed);
		wb_buffer_release(&raw);
		wb_buffer_release(&location);
		wb_buffer_release(&bytes);
		wb_units_release(&units);

		/* A script that cannot be fetched, or fails, is only that script's failure. */
		if (error == ENOMEM)
			return error;
		return 0;
	}

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
	bind_window_set_load_ended(window);
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

/* Gives each loaded child its iframe's content size for its viewport, from the page's last layout. */
static void
frame_size_walk(
	const struct layout_box *box)
{
	const struct dom_element *element;
	const struct vm_realm *child;
	struct bind_window *window;

	for (; box != NULL; box = box->next) {
		if (box->node != NULL && box->node->type == DOM_ELEMENT) {
			element = (const struct dom_element *)box->node;
			if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_IFRAME && element->child_context != NULL) {
				child = (const struct vm_realm *)element->child_context;
				window = child->host;
				if (window != NULL && !window->detached)
					bind_window_set_viewport(window, (int)(layout_to_px(box->width) / (box->style.zoom_eff > 0.0f ? box->style.zoom_eff : 1.0f)), (int)(layout_to_px(box->height) / (box->style.zoom_eff > 0.0f ? box->style.zoom_eff : 1.0f)));
			}
		}
		frame_size_walk(box->first_child);
	}
}

void
page_frames_size(
	struct page *page)
{
	if (page->laid_out && page->layout.root != NULL)
		frame_size_walk(page->layout.root);
}

/* Finds the entry the page keeps for an iframe (NULL when it has none). */
static struct frame_cache_entry *
frame_cache_find(
	struct page *page,
	const struct dom_element *element)
{
	struct frame_cache_entry *entry;
	size_t index;

	for (index = 0; index < page->frame_cache.count; index++) {
		entry = wb_vector_at(&page->frame_cache, index);
		if (entry->element == element)
			return entry;
	}
	return NULL;
}

/* Finds the entry the page keeps for an iframe, making an empty one when it has none (NULL when memory runs out). */
static struct frame_cache_entry *
frame_cache_get(
	struct page *page,
	const struct dom_element *element)
{
	struct frame_cache_entry *entry;
	struct frame_cache_entry made;

	entry = frame_cache_find(page, element);
	if (entry != NULL)
		return entry;
	memset(&made, 0, sizeof(made));
	made.element = element;
	if (wb_vector_push(&page->frame_cache, &made) != 0)
		return NULL;
	return frame_cache_find(page, element);
}

/*
 * Lays out the document an <iframe> element shows in a viewport of a size in the frame's own pixels device pixels
 * (zoom is the factor the frame's document is under): a new tree the caller owns, or NULL when the frame has no
 * document or cascade yet.  A transition given is the one the tree is built with.
 */
static int
frame_tree(
	struct page *page,
	const struct dom_element *element,
	layout_unit width,
	layout_unit height,
	float zoom,
	const struct layout_transition *transition,
	struct layout_tree **out)
{
	struct bind_window *window;
	struct css_engine *engine;
	struct layout_tree *tree;
	int viewport_width;
	int viewport_height;
	int status;

	*out = NULL;
	window = bind_frame_child(&element->node);
	if (window == NULL || window->detached || window->document == NULL)
		return 0;
	status = bind_window_child_styles(window, &engine, &viewport_width, &viewport_height);
	if (status != 0 || engine == NULL || window->detached)
		return 0;

	/* No size given: the frame's own, in its unzoomed pixels, under the zoom it has. */
	if (zoom <= 0.0f)
		zoom = css_engine_base_zoom(engine);
	if (layout_to_px(width) > 0)
		viewport_width = (int)layout_to_px(width);
	else
		viewport_width = (int)((float)viewport_width * zoom);
	if (layout_to_px(height) > 0)
		viewport_height = (int)layout_to_px(height);
	else
		viewport_height = (int)((float)viewport_height * zoom);
	tree = calloc(1, sizeof(*tree));
	if (tree == NULL)
		return ENOMEM;
	css_engine_set_base_zoom(engine, zoom);
	status = layout_build(tree, engine, &page->text, window->document, page_image_of, page_image_by_url, page,
	    viewport_width, viewport_height, 0, 0, transition);
	if (status != 0) {
		layout_release(tree);
		free(tree);
		return 0;
	}
	tree->image_variant = page_image_variant;
	tree->frame_paint = page_frame_paint;
	*out = tree;
	return 0;
}

/*
 * Lays out and paints the document an <iframe> element shows in a viewport of a size, and keeps the result in the
 * page's cache (NULL out when the frame has no document or cascade yet; otherwise a reference the caller releases).
 */
static int
frame_make(
	struct page *page,
	const struct dom_element *element,
	layout_unit width,
	layout_unit height,
	float zoom,
	struct paint_frame **out)
{
	struct layout_tree *tree;
	struct paint_frame *frame;
	struct frame_cache_entry *entry;
	int status;

	*out = NULL;
	entry = frame_cache_get(page, element);
	status = frame_tree(page, element, width, height, zoom, entry != NULL && entry->on ? &entry->transition : NULL, &tree);
	if (status != 0 || tree == NULL)
		return status;
	frame = calloc(1, sizeof(*frame));
	if (frame == NULL) {
		layout_release(tree);
		free(tree);
		return ENOMEM;
	}
	frame->tree = tree;
	frame->refs = 1;
	status = paint_build(&frame->list, tree);
	if (status != 0) {
		paint_frame_release(frame);
		return status;
	}

	/* The page keeps it too, for as long as the frame's painting may be held. */
	entry = frame_cache_get(page, element);
	if (entry != NULL) {
		frame->refs++;
		paint_frame_release(entry->frame);
		entry->frame = frame;
	}
	*out = frame;
	return 0;
}

/*
 * Gives the painted document an <iframe> element shows, for the painting of the page (a reference the caller
 * releases; NULL when the frame has no document or no cascade yet).  While the frame's document holds its
 * painting (a view transition's update callback is running) the one kept is given again.
 */
int
page_frame_paint(
	void *context,
	const struct dom_element *element,
	layout_unit width,
	layout_unit height,
	float zoom,
	struct paint_frame **out)
{
	struct page *page;
	struct bind_window *window;
	struct frame_cache_entry *entry;

	page = context;
	*out = NULL;
	window = bind_frame_child(&element->node);
	if (window == NULL || window->detached || window->document == NULL)
		return 0;
	entry = frame_cache_find(page, element);
	if (window->frame_hold && entry != NULL && entry->frame != NULL) {
		entry->frame->refs++;
		*out = entry->frame;
		return 0;
	}
	return frame_make(page, element, width, height, zoom, out);
}

/* Paints the document an <iframe> shows as it is now and keeps it (the frame is then asked to hold it). */
int
page_frame_capture(
	void *context,
	struct dom_element *element)
{
	struct page *page;
	struct paint_frame *frame;
	const struct layout_box *box;
	int error;

	page = context;
	if (!page->laid_out)
		return 0;
	box = layout_box_of(&page->layout, &element->node);
	if (box == NULL)
		return 0;
	error = frame_make(page, element, box->width, box->height, box->style.zoom_eff, &frame);
	if (error == 0 && frame != NULL)
		paint_frame_release(frame);
	return error;
}

/*
 * Adds up the generations of the documents the iframes below a document hold (a change in any of them changes the
 * sum), and counts the iframe elements met in *iframes.
 */
static uint64_t
frame_generation_walk(
	const struct dom_node *node,
	int depth,
	size_t *iframes)
{
	const struct dom_node *child;
	const struct dom_element *element;
	const struct vm_realm *realm;
	const struct bind_window *window;
	uint64_t sum;

	sum = 0;
	if (depth > 64)
		return 0;
	if (node->type == DOM_ELEMENT) {
		element = (const struct dom_element *)node;
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_IFRAME)
			(*iframes)++;
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_IFRAME && element->child_context != NULL) {
			realm = (const struct vm_realm *)element->child_context;
			window = realm->host;
			if (window != NULL && !window->detached && window->document != NULL) {
				sum += window->document->generation + 1U + (window->frame_hold ? 1000003U : 0U);
				sum += frame_generation_walk(&window->document->node, depth + 1, iframes);
			}
		}
	}
	for (child = node->first_child; child != NULL; child = child->next)
		sum += frame_generation_walk(child, depth + 1, iframes);
	return sum;
}

/* The sum of the generations of the documents in the page's iframes, which the page's layout notes (the frames are painted into it). */
uint64_t
page_frames_generation(
	const struct page *page)
{
	uint64_t sum;
	size_t iframes;

	/* The documents and the iframes they have. */
	sum = page_frames_generation_count(page, &iframes);

	/* Succeeded: the sum. */
	return sum;
}

/*
 * Computes page_frames_generation and counts the iframe elements of the page's document (and of theirs) in
 * *iframes; a document of no iframe element walked again before it changes gives the same sum without them.
 */
uint64_t
page_frames_generation_count(
	const struct page *page,
	size_t *iframes)
{
	uint64_t sum;
	size_t index;

	*iframes = 0;
	if (page->document == NULL)
		return 0;
	sum = frame_generation_walk(&page->document->node, 0, iframes);
	for (index = 0; index < page->frame_cache.count; index++)
		sum += ((const struct frame_cache_entry *)wb_vector_at(&page->frame_cache, index))->generation;
	return sum;
}

/*
 * Finds the node under a point of the page (document coordinates in pixels), going into the documents of the iframes
 * it is in: the node (text too) and how far the point's document is from the page's top left (a frame's content box).
 * NULL when the page has no box there.
 */
struct dom_node *
page_hit_node(
	struct page *page,
	int x,
	int y,
	int *offset_x,
	int *offset_y)
{
	const struct layout_tree *tree;
	const struct layout_box *box;
	const struct dom_element *element;
	struct frame_cache_entry *entry;
	struct dom_node *node;
	struct dom_node *inner;
	layout_unit px;
	layout_unit py;
	layout_unit left;
	layout_unit top;
	int depth;

	*offset_x = 0;
	*offset_y = 0;
	if (!page->laid_out)
		return NULL;
	tree = &page->layout;
	px = (layout_unit)x * LAYOUT_UNIT;
	py = (layout_unit)y * LAYOUT_UNIT;
	node = layout_hit_node(tree, px, py);
	left = 0;
	top = 0;
	for (depth = 0; node != NULL && depth < 16; depth++) {
		element = (const struct dom_element *)node;
		if (node->type != DOM_ELEMENT || element->ns != DOM_NS_HTML || element->tag != DOM_TAG_IFRAME)
			break;
		entry = frame_cache_find(page, element);
		box = layout_box_of(tree, node);
		if (entry == NULL || entry->frame == NULL || entry->frame->tree == NULL || box == NULL)
			break;

		/* The point in the frame's own document: from its content box. */
		px -= box->x + box->border[CSS_LEFT] + box->padding[CSS_LEFT];
		py -= box->y + box->border[CSS_TOP] + box->padding[CSS_TOP];
		if (px < 0 || py < 0 || px >= box->width || py >= box->height)
			break;
		tree = entry->frame->tree;
		inner = layout_hit_node(tree, px, py);
		if (inner == NULL)
			break;
		left += box->x + box->border[CSS_LEFT] + box->padding[CSS_LEFT];
		top += box->y + box->border[CSS_TOP] + box->padding[CSS_TOP];
		node = inner;
	}
	*offset_x = (int)layout_to_px(left);
	*offset_y = (int)layout_to_px(top);
	return node;
}

/* The view transition of a frame's document, as the page's is (page_view_transition): steps 1 (capture the old state), 2 (the new state, shown), 0 (end). */
int
page_frame_view_transition(
	void *context,
	struct dom_element *element,
	int step)
{
	struct page *page;
	struct frame_cache_entry *entry;
	struct layout_transition_name *name;
	struct layout_tree *tree;
	const struct layout_box *box;
	size_t index;
	int error;

	page = context;
	entry = frame_cache_get(page, element);
	if (entry == NULL)
		return 0;
	if (step == 0 || step == 1) {
		if (entry->on) {
			layout_transition_release(&entry->transition);
			entry->on = 0;
			entry->generation++;
		}
		if (step == 0)
			return 1;
		error = frame_tree(page, element, 0, 0, 0.0f, NULL, &tree);
		if (error != 0 || tree == NULL || tree->root == NULL) {
			if (tree != NULL) {
				layout_release(tree);
				free(tree);
			}
			return 0;
		}
		entry->on = 1;
		entry->transition.heap = page->heap;
		error = layout_transition_measure(tree, &entry->transition, 1);
		for (index = 0; error == 0 && index < entry->transition.count; index++) {
			name = &entry->transition.names[index];
			box = NULL;
			if (!name->is_root) {
				box = layout_transition_box(&entry->transition, tree->root, name->name, 0);
				if (box == NULL)
					continue;
			}
			error = paint_transition_capture(tree, &entry->transition, box, &name->old_image);
		}
		layout_release(tree);
		free(tree);
		if (error != 0) {
			layout_transition_release(&entry->transition);
			entry->on = 0;
			return 0;
		}
		entry->generation++;
		return 1;
	}

	/* The new state: measured without the tree, then shown. */
	if (!entry->on)
		return 0;
	entry->transition.show = 0;
	entry->generation++;
	error = frame_tree(page, element, 0, 0, 0.0f, &entry->transition, &tree);
	if (error != 0 || tree == NULL || tree->root == NULL) {
		if (tree != NULL) {
			layout_release(tree);
			free(tree);
		}
		layout_transition_release(&entry->transition);
		entry->on = 0;
		return 0;
	}
	error = layout_transition_measure(tree, &entry->transition, 0);
	layout_release(tree);
	free(tree);
	if (error != 0) {
		layout_transition_release(&entry->transition);
		entry->on = 0;
		return 0;
	}
	entry->transition.show = 1;
	entry->transition.elapsed = 0.0f;
	entry->generation++;
	return 1;
}

/* The milliseconds until the animations of a frame's shown transition tree have all ended. */
double
page_frame_view_transition_duration(
	void *context,
	struct dom_element *element)
{
	struct page *page;
	struct frame_cache_entry *entry;
	struct layout_tree *tree;
	double longest;

	page = context;
	entry = frame_cache_find(page, element);
	if (entry == NULL || !entry->on)
		return 0.0;
	if (frame_tree(page, element, 0, 0, 0.0f, &entry->transition, &tree) != 0 || tree == NULL)
		return 0.0;
	longest = tree->root != NULL ? page_transition_longest(tree->root, 0) * 1000.0 : 0.0;
	layout_release(tree);
	free(tree);
	return longest;
}
