/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A page's scripts and its event loop: the realm and window a page runs
 * its scripts in, the script elements run as the parser reaches them (an
 * inline script's text, or a src file next to the page), the load events,
 * and the timers driven by the page's clock (the user's input becomes DOM
 * events in input.c).
 *
 * The first pass runs classic scripts only, from the page's own files and
 * data: URLs (http arrives with the network, modules later), and runs them
 * while the parser waits, as a browser runs a parser-blocking script.
 */

#include "page/page.h"
#include "net/net.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The longest part of a script's src its errors are named by, in bytes. */
#define SCRIPT_NAME_MAX		200U

/* The task-round limit that detects loops without virtual-clock progress. */
#define SCRIPT_SETTLE_ROUNDS	10000

/* The deepest inserted subtree searched for scripts. */
#define SCRIPT_INSERT_DEPTH	512

/* The most parser-blocking scripts a document's scan names for fetching ahead (BUG-207). */
#define SCRIPT_PREFETCH_MAX	64U

/* An external script prepared by insertion and waiting for its task or fetch. */
struct page_script {
	struct page *page;
	struct dom_element *element;
	char *location;
	struct wb_units source;
	struct net_request *request;
	int ready;
	int failed;
	int done;
	int rooted;
	int ordered;
};

/* One Fetch API request using the page's asynchronous network loader. */
struct page_fetch_request {
	struct page *page;
	struct net_request *request;
	bind_request_done done;
	void *done_context;
};

/*
 * The part of a URL (a NET_URL_*) each part of the window's location
 * reads, in the order of the BIND_LOCATION_* values.  The table is
 * constant for the life of the program.
 */
static const int script_location_parts[] = {
	NET_URL_HREF,
	NET_URL_ORIGIN,
	NET_URL_PROTOCOL,
	NET_URL_HOST,
	NET_URL_HOSTNAME,
	NET_URL_PORT,
	NET_URL_PATHNAME,
	NET_URL_SEARCH,
	NET_URL_HASH
};

/*
 * The MIME types of a classic script's type attribute, in lower case (the
 * standard's JavaScript MIME type essence matches).  The table is constant
 * for the life of the program and ends with NULL.
 */
static const char *const script_types[] = {
	"application/ecmascript",
	"application/javascript",
	"application/x-ecmascript",
	"application/x-javascript",
	"text/ecmascript",
	"text/javascript",
	"text/javascript1.0",
	"text/javascript1.1",
	"text/javascript1.2",
	"text/javascript1.3",
	"text/javascript1.4",
	"text/javascript1.5",
	"text/jscript",
	"text/livescript",
	"text/x-ecmascript",
	"text/x-javascript",
	NULL
};

static int script_settle_network(struct page *page, double wait, double *elapsed);
static void script_console(void *context, int level, const char *text, size_t length);
static int script_location(void *context, int part, struct wb_buffer *out);
static int script_url_set(void *context, const char *href, size_t length);
static int script_cookie_get(void *context, struct wb_buffer *out);
static int script_cookie_set(void *context, const char *text, size_t length);
static int script_type_runs(const struct dom_element *script);
static int script_type_module(const struct dom_element *script);
static int script_attribute(struct page *page, const struct dom_element *element, const char *name, struct dom_attribute **attribute);
static int script_run_file(struct page *page, const struct vm_string *src);
static int script_ascii_equal_folded(const struct vm_string *string, const char *ascii);
static int script_request(void *context, const struct bind_request *request, bind_request_done done, void *done_context, void **handle);
static void script_request_cancel(void *context, void *handle);
static int script_fetch_file(struct page *page, const char *href, struct wb_buffer *bytes);
static const struct page_prefetched *script_prefetched(const struct page *page, const char *location);
static int script_scan_starts(const unsigned char *bytes, size_t length, size_t at, const char *lower);
static size_t script_scan_find(const unsigned char *bytes, size_t length, size_t at, const char *lower);
static int script_scan_tag(const unsigned char *bytes, size_t length, size_t *at, struct wb_buffer *src, int *has_src, int *runs);
static int script_scan_value(const unsigned char *bytes, size_t start, size_t end, struct wb_buffer *out);
static int script_scan_type_runs(const struct wb_buffer *type);
static int script_scan_add(const char *base, const struct wb_buffer *src, struct wb_vector *locations);
static int script_fetch_sync(void *context, const char *href, struct wb_buffer *bytes, struct wb_buffer *final_url);
static void script_fetch_arrived(void *context, struct net_request *request);
static void script_fetch_remove(struct page_fetch_request *entry);
static void script_fetch_unlink(struct page_fetch_request *entry);
static int script_node_inserted(void *context, struct dom_node *node);
static int script_checkpoint(void *context);
static int script_document_write(void *context, const uint16_t *units, size_t length);
static int script_walk_inserted(struct page *page, struct dom_node *node, int depth);
static int script_prepare_dynamic(struct page *page, struct dom_element *script);
static int script_prepare_external(struct page *page, struct dom_element *script, const struct vm_string *src);
static int script_decode(struct page_script *entry, const unsigned char *bytes, size_t length);
static int script_run_ready(struct page *page);
static int script_finish(struct page_script *entry);
static void script_arrived(void *context, struct net_request *request);
static void script_release(struct page_script *entry);

/* Starts the page's list of dynamic external scripts empty. */
void
page_scripts_init(
	struct page *page)
{
	wb_vector_init(&page->scripts, sizeof(struct page_script *));
	wb_vector_init(&page->fetches, sizeof(struct page_fetch_request *));
	wb_vector_init(&page->prefetched, sizeof(struct page_prefetched *));
}

/* Cancels and frees the page's dynamic external scripts. */
void
page_scripts_release(
	struct page *page)
{
	struct page_script *entry;
	struct page_fetch_request *fetch;
	struct page_prefetched *prefetched;
	size_t index;

	/* Each entry owns a request, a root while pending, its source and location. */
	for (index = 0; index < page->scripts.count; index++) {
		entry = *(struct page_script **)wb_vector_at(&page->scripts, index);
		script_release(entry);
	}

	/* Fetch API requests have no callback while cancellation frees them. */
	for (index = 0; index < page->fetches.count; index++) {
		fetch = *(struct page_fetch_request **)wb_vector_at(&page->fetches, index);
		net_request_cancel(fetch->request);
		free(fetch);
	}

	/* The scripts fetched ahead own their location and bytes. */
	for (index = 0; index < page->prefetched.count; index++) {
		prefetched = *(struct page_prefetched **)wb_vector_at(&page->prefetched, index);
		free(prefetched->location);
		wb_buffer_release(&prefetched->bytes);
		free(prefetched);
	}

	/* The tables themselves. */
	wb_vector_release(&page->scripts);
	wb_vector_release(&page->fetches);
	wb_vector_release(&page->prefetched);
}

/*
 * Finds the parser-blocking scripts a document names, so that the view
 * fetches them before the document is parsed (BUG-207): the parser runs a
 * <script src> as it reaches it and would otherwise read it at once, which
 * blocks the window while the network works.
 *
 * The scan is a light reading of the markup, not the parser: the src of
 * each <script> outside comments whose type runs, resolved against base,
 * when it is an http or https URL, each once and at most
 * SCRIPT_PREFETCH_MAX.  A script it misses (one a script writes) is read
 * at once as before; one it finds that does not run is only fetched.
 * locations receives a copy (char *) of each, which the caller frees.
 */
int
page_scan_scripts(
	const char *base,
	const unsigned char *bytes,
	size_t length,
	struct wb_vector *locations)
{
	struct wb_buffer src;
	size_t at;
	size_t end;
	int has_src;
	int runs;
	int found;
	int error;

	/* The markup from its start, with no src read yet. */
	wb_buffer_init(&src);
	at = 0;
	error = 0;

	/* Each tag or comment, up to the end of the document or the most scripts. */
	while (at < length && locations->count < SCRIPT_PREFETCH_MAX) {
		/* A comment's text is not markup: on past its end (an unclosed one ends the document). */
		found = script_scan_starts(bytes, length, at, "<!--");
		if (found) {
			end = script_scan_find(bytes, length, at + 4U, "-->");
			if (end == length)
				break;
			at = end + 3U;
			continue;
		}

		/* Anything but a script's start tag is passed over a byte at a time. */
		found = script_scan_starts(bytes, length, at, "<script");
		if (!found || at + 7U >= length) {
			at++;
			continue;
		}

		/* "<scripts" or "<script-x" is another element. */
		if (bytes[at + 7U] != ' ' &&
		    bytes[at + 7U] != '\t' &&
		    bytes[at + 7U] != '\n' &&
		    bytes[at + 7U] != '\r' &&
		    bytes[at + 7U] != '\f' &&
		    bytes[at + 7U] != '/' &&
		    bytes[at + 7U] != '>') {
			at++;
			continue;
		}

		/* The tag's attributes: its src and whether its type runs (an unclosed tag ends the document). */
		at += 7U;
		wb_buffer_clear(&src);
		error = script_scan_tag(bytes, length, &at, &src, &has_src, &runs);
		if (error == EINVAL) {
			error = 0;
			break;
		}
		if (error != 0)
			break;

		/* A script that runs from a web URL is one to fetch ahead (an empty src loads nothing). */
		if (has_src && runs && src.length != 0) {
			error = script_scan_add(base, &src, locations);
			if (error != 0)
				break;
		}

		/* The script's text is not markup: on past its end tag. */
		end = script_scan_find(bytes, length, at, "</script");
		at = end;
	}

	/* The scratch goes. */
	wb_buffer_release(&src);

	/* Reports a failure (memory) to keep what was found. */
	if (error != 0)
		return error;

	/* Succeeded: locations holds the scripts to fetch ahead. */
	return 0;
}

/*
 * Keeps a parser-blocking script the view fetched before the document was
 * parsed (BUG-207): how its fetch ended and its bytes, under the location
 * it was fetched from; the parser takes it from here instead of reading it
 * at once.
 */
int
page_add_prefetched(
	struct page *page,
	const char *location,
	int error,
	const unsigned char *bytes,
	size_t length)
{
	struct page_prefetched *entry;
	int status;

	/* The entry. */
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return ENOMEM;
	wb_buffer_init(&entry->bytes);

	/* Its location. */
	entry->location = strdup(location);
	if (entry->location == NULL) {
		free(entry);
		return ENOMEM;
	}

	/* How the fetch ended, and the bytes it brought. */
	entry->error = error;
	status = wb_buffer_append(&entry->bytes, bytes, length);
	if (status != 0) {
		free(entry->location);
		free(entry);
		return status;
	}

	/* The page's table keeps it until the page goes. */
	status = wb_vector_push(&page->prefetched, &entry);
	if (status != 0) {
		wb_buffer_release(&entry->bytes);
		free(entry->location);
		free(entry);
		return status;
	}

	/* Succeeded: the parser finds the script here. */
	return 0;
}

/*
 * Parses the page's location as a URL: its URL, or file: for the
 * absolute path of a file.  Returns EINVAL for a page without a location.
 */
int
page_url(
	const struct page *page,
	struct net_url *url)
{
	struct wb_buffer text;
	int error;

	/* A page without a location. */
	if (page->base == NULL)
		return EINVAL;

	/* A path becomes a file: URL. */
	wb_buffer_init(&text);
	if (page->base[0] == '/') {
		error = wb_buffer_printf(&text, "file://%s", page->base);
	} else {
		error = wb_buffer_append_string(&text, page->base);
	}

	/* The URL. */
	if (error == 0)
		error = net_url_parse(wb_buffer_string(&text), text.length, NULL, url);
	wb_buffer_release(&text);
	if (error != 0)
		return error;

	/* Succeeded: the caller releases the URL. */
	return 0;
}

/*
 * Makes the page's realm with the built-ins and makes its global object
 * the document's window.
 */
int
page_start_scripts(
	struct page *page)
{
	struct bind_host host;
	int error;

	/* The realm and the language's built-in objects. */
	error = vm_realm_create(page->heap, &page->realm);
	if (error != 0)
		return error;
	error = js_install_builtins(page->realm);
	if (error != 0)
		return error;

	/* The window, whose console goes to the page's, and whose location and cookies are the page's URL's. */
	memset(&host, 0, sizeof(host));
	host.context = page;
	host.console = script_console;
	host.user_agent = NET_USER_AGENT;
	host.location = script_location;
	host.url_set = script_url_set;
	host.cookie_get = script_cookie_get;
	host.cookie_set = script_cookie_set;
	host.selector_engine = page_selector_engine;
	host.node_box = page_node_box;
	host.grid_tracks = page_grid_tracks;
	host.document_size = page_document_size;
	host.scroll = page_scroll;
	host.storage = page_storage_calls();
	host.computed_style = page_computed_style;
	host.scroll_to = page_scroll_to;
	host.node_inserted = script_node_inserted;
	host.checkpoint = script_checkpoint;
	host.request = script_request;
	host.request_cancel = script_request_cancel;
	host.fetch_sync = script_fetch_sync;
	host.element_at = page_element_at;
	host.document_write = script_document_write;
	host.media = page_media_host;
	error = bind_window_create(page->realm, page->document, &host, &page->window);
	if (error != 0)
		return error;

	/* Succeeded: the page can run scripts. */
	return 0;
}

/* Polls one bounded actual loader step while recording monotonic elapsed waiting time. */
static int
script_settle_network(
	struct page *page,
	double wait,
	double *elapsed)
{
	struct pollfd fds[64];
	struct timespec before;
	struct timespec after;
	size_t count;
	int timeout;
	int ready;
	int status;

	/* Headless network integration uses the same supported monotonic clock as the native view and loader. */
	*elapsed = 0;
	status = clock_gettime(CLOCK_MONOTONIC, &before);
	if (status != 0)
		return errno;

	/* Actual requests supply their own earliest timeout; a missing loader cannot make request progress. */
	if (page->loader == NULL)
		return ENOTSUP;
	timeout = net_loader_timeout(page->loader);
	if (timeout < 0)
		timeout = 0;
	if (timeout > wait) {
		/* Poll uses whole milliseconds; rounding upward prevents submillisecond busy waits. */
		timeout = (int)wait;
		if ((double)timeout < wait)
			timeout++;
	}

	/* Poll the actual descriptor snapshot without advancing any virtual timer first. */
	count = net_loader_poll_fds(page->loader, fds, 64U);
	ready = poll(fds, (nfds_t)count, timeout);
	if (ready < 0 && errno != EINTR)
		return errno;

	/* A borrowed completion callback copies C metadata; outer Page timing drains child scripts and events later. */
	if (ready >= 0)
		net_loader_process(page->loader, fds, count);
	status = clock_gettime(CLOCK_MONOTONIC, &after);
	if (status != 0)
		return errno;

	/* Monotonic elapsed duration, rather than a fabricated fixed tick, advances pending child waits. */
	*elapsed = (double)(after.tv_sec - before.tv_sec) * 1000;
	*elapsed += (double)(after.tv_nsec - before.tv_nsec) / 1000000;
	if (*elapsed < 0)
		return EIO;

	/* Succeeded: one actual network step completed outside child script/event dispatch. */
	return 0;
}

/* Fetches synchronously for legacy synchronous consumers such as the first XHR pass. */
static int
script_fetch_sync(
	void *context,
	const char *href,
	struct wb_buffer *bytes,
	struct wb_buffer *final_url)
{
	struct page *page;

	/* The original blocking page fetcher remains available to synchronous XHR. */
	page = context;
	if (page->base == NULL)
		return EINVAL;
	return page_fetch(page->base, href, bytes, final_url);
}

/* Starts a script's request, resolved against this page; a web resource goes through the page's loader. */
static int
script_request(
	void *context,
	const struct bind_request *request,
	bind_request_done done,
	void *done_context,
	void **handle)
{
	struct page *page;
	struct page_fetch_request *entry;
	struct net_send send;
	struct wb_buffer location;
	struct wb_buffer bytes;
	struct wb_buffer final_url;
	const char *response_url;
	int response_status;
	int kept;
	int remote;
	int plain;
	int error;

	/* Resolve first so remote requests can use the page's non-blocking loader. */
	page = context;
	*handle = NULL;
	if (page->base == NULL)
		return EINVAL;
	wb_buffer_init(&location);
	error = page_resolve_location(page->base, request->href, &location);
	remote = error == 0 && net_loader_takes(wb_buffer_string(&location));
	plain = request->method == NULL || strcmp(request->method, "GET") == 0;

	/* A web resource runs with the same loader as images and inserted scripts. */
	if (error == 0 && page->loader != NULL && remote) {
		kept = 0;
		entry = calloc(1, sizeof(*entry));
		if (entry == NULL) {
			error = ENOMEM;
		} else {
			entry->page = page;
			entry->done = done;
			entry->done_context = done_context;
			send.method = request->method;
			send.headers = request->headers;
			send.body = request->body;
			send.body_length = request->body_length;
			error = wb_vector_push(&page->fetches, &entry);
			if (error == 0) {
				kept = 1;
				*handle = entry;
				error = net_loader_send(page->loader, wb_buffer_string(&location), NET_FETCH_SCRIPT_API, &send,
				    script_fetch_arrived, entry, &entry->request);
			}

			/* A request that did not start leaves no table entry. */
			if (error != 0) {
				if (kept)
					page->fetches.count--;
				*handle = NULL;
				free(entry);
			}
		}

		/* The loader copied the resolved location. */
		wb_buffer_release(&location);
		return error;
	}

	/* Without a loader a web resource is read at once (blocking), with the method, headers and body. */
	if (error == 0 && remote) {
		struct net_response response;

		send.method = request->method;
		send.headers = request->headers;
		send.body = request->body;
		send.body_length = request->body_length;
		error = net_http_fetch_send(wb_buffer_string(&location), &send, &response);
		if (error == 0)
			done(done_context, 0, response.status, wb_buffer_string(&response.url), wb_buffer_string(&response.headers),
			    response.body.data, response.body.length);
		else
			done(done_context, error, 0, NULL, "", NULL, 0);
		if (error == 0)
			net_response_release(&response);
		wb_buffer_release(&location);
		return 0;
	}

	/* Local and data resources complete in this task (only a GET reads them). */
	wb_buffer_init(&bytes);
	wb_buffer_init(&final_url);
	if (error == 0 && !plain)
		error = ENOTSUP;
	if (error == 0)
		error = page_fetch(page->base, request->href, &bytes, &final_url);
	response_status = 0;
	response_url = NULL;
	if (error == 0) {
		response_status = 200;
		response_url = wb_buffer_string(&final_url);
	}

	/* The binding copies the bytes before the temporary buffers go away. */
	done(done_context, error, response_status, response_url, "", bytes.data, bytes.length);
	wb_buffer_release(&bytes);
	wb_buffer_release(&final_url);
	wb_buffer_release(&location);
	return 0;
}

/* Cancels a request the page still holds (its callback is not called). */
static void
script_request_cancel(
	void *context,
	void *handle)
{
	struct page *page;
	struct page_fetch_request *entry;
	size_t index;

	/* The entry must still be in the table; completion removed the others. */
	page = context;
	entry = handle;
	for (index = 0; index < page->fetches.count; index++) {
		if (*(struct page_fetch_request **)wb_vector_at(&page->fetches, index) != entry)
			continue;
		if (entry->request != NULL)
			net_request_cancel(entry->request);
		entry->request = NULL;
		script_fetch_remove(entry);
		return;
	}
}

/* Completes one script request from the asynchronous loader. */
static void
script_fetch_arrived(
	void *context,
	struct net_request *request)
{
	const struct net_response *response;
	struct page_fetch_request *entry;
	const char *response_url;
	const char *headers;
	int response_status;
	int error;

	/* The result remains valid for the duration of this loader callback. */
	entry = context;
	entry->request = NULL;
	error = net_request_error(request);
	response = net_request_response(request);
	response_status = 0;
	response_url = NULL;
	headers = "";
	if (error == 0) {
		response_status = response->status;
		response_url = wb_buffer_string(&response->url);
		headers = wb_buffer_string(&response->headers);
	}

	/* Settlement queues Promise jobs, which this task then runs; the entry is gone before the script can cancel it. */
	script_fetch_unlink(entry);
	entry->done(entry->done_context, error, response_status, response_url, headers,
	    response->body.data, response->body.length);
	(void)bind_checkpoint(entry->page->window);
	free(entry);
}

/* Takes a request out of the page's table (the caller frees it). */
static void
script_fetch_unlink(
	struct page_fetch_request *entry)
{
	struct page_fetch_request **slot;
	size_t index;

	/* Completion may have added more requests; find this one in the table. */
	for (index = 0; index < entry->page->fetches.count; index++) {
		slot = wb_vector_at(&entry->page->fetches, index);
		if (*slot != entry)
			continue;
		*slot = *(struct page_fetch_request **)wb_vector_at(&entry->page->fetches,
		    entry->page->fetches.count - 1U);
		entry->page->fetches.count--;
		break;
	}
}

/* Removes and frees a request. */
static void
script_fetch_remove(
	struct page_fetch_request *entry)
{
	/* The table no longer refers to the entry. */
	script_fetch_unlink(entry);
	free(entry);
}

/*
 * Runs a script element the parser has reached (the parser's script
 * hook; context is the page): its src file, or its text.
 */
int
page_run_script_element(
	void *context,
	struct dom_element *script)
{
	struct page *page;
	struct dom_attribute *src;
	struct dom_attribute *defer;
	struct dom_attribute *async;
	struct dom_node *child;
	const struct dom_character_data *data;
	struct wb_units text;
	const char *name;
	int runs;
	int error;

	/* Modules wait until parsing ends; classic scripts keep parser order. */
	page = context;
	script->node.flags |= DOM_NODE_SCRIPT_STARTED;
	runs = script_type_runs(script);
	if (!runs && script_type_module(script))
		return page_module_enqueue(page, script);
	if (!runs)
		return 0;

	/* A script with a src runs the file. */
	error = script_attribute(page, script, "src", &src);
	if (error != 0)
		return error;
	if (src != NULL) {
		/* A parser-inserted external classic script with defer (and not async) waits until parsing ends. */
		error = script_attribute(page, script, "defer", &defer);
		if (error == 0)
			error = script_attribute(page, script, "async", &async);
		if (error != 0)
			return error;
		if (defer != NULL && async == NULL) {
			error = wb_vector_push(&page->deferred_scripts, &script);
			return error;
		}

		/* A fatal external-script failure must also stop the parser. */
		bind_window_set_current_script(page->window, &script->node);
		error = script_run_file(page, src->value);
		bind_window_set_current_script(page->window, NULL);
		if (error != 0)
			return error;

		/* Succeeded: the parser-blocking file has run. */
		return 0;
	}

	/* Otherwise its text (its text children's) runs, named after the page's file. */
	wb_units_init(&text);
	error = 0;
	for (child = script->node.first_child; child != NULL && error == 0; child = child->next) {
		if (child->type != DOM_TEXT && child->type != DOM_CDATA_SECTION)
			continue;
		data = (const struct dom_character_data *)child;
		error = wb_units_append(&text, data->data.data, data->data.length);
	}

	/* The text runs, named after the page's file. */
	name = "(inline)";
	if (page->base != NULL)
		name = page->base;
	bind_window_set_current_script(page->window, &script->node);
	if (error == 0)
		error = bind_run_script_source(page->window, text.data, text.length, name, 1);
	bind_window_set_current_script(page->window, NULL);
	wb_units_release(&text);
	if (error != 0)
		return error;

	/* Succeeded: the script ran or its ordinary exception was reported. */
	return 0;
}

/* A DOM operation inserted a subtree; connected script elements in it are prepared in tree order. */
static int
script_node_inserted(
	void *context,
	struct dom_node *node)
{
	struct page *page;
	struct dom_node *ancestor;

	/* Only insertion into the page's document prepares scripts. */
	page = context;
	for (ancestor = node; ancestor != NULL; ancestor = ancestor->parent) {
		if (ancestor == &page->document->node)
			return script_walk_inserted(page, node, 0);
	}

	/* A detached subtree does not prepare its scripts until it is connected. */
	return 0;
}

/* Inserts a document write into the parser owned by the current load. */
static int
script_document_write(
	void *context,
	const uint16_t *units,
	size_t length)
{
	struct page *page;
	int error;

	/* Late writes need a script-created parser, outside this implementation. */
	page = context;
	if (page->parser == NULL)
		return ENOTSUP;

	/* The parser keeps the written prefix ahead of its unread source. */
	error = html_parser_write(page->parser, units, length);
	if (error != 0)
		return error;

	/* Succeeded: the inserted text used the production parser. */
	return 0;
}

/* Runs external scripts whose local bytes became ready at the last script checkpoint. */
static int
script_checkpoint(
	void *context)
{
	struct page *page;
	int error;

	/* Finish primary script work before starting native child resource tasks. */
	page = context;
	error = script_run_ready(page);
	if (error != 0)
		return error;

	/* Reentrant child script checkpoints retain the outer task snapshot. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		return error;

	/* Succeeded: primary script work and one native child task snapshot are complete. */
	return 0;
}

/* Prepares each unstarted script in an inserted connected subtree. */
static int
script_walk_inserted(
	struct page *page,
	struct dom_node *node,
	int depth)
{
	struct dom_node *child;
	int script;
	int error;

	/* Stops at the parser's maximum nesting. */
	if (depth > SCRIPT_INSERT_DEPTH)
		return 0;

	/* The node itself, when it is a script. */
	script = dom_element_is(node, DOM_NS_HTML, DOM_TAG_SCRIPT);
	if (script) {
		error = script_prepare_dynamic(page, (struct dom_element *)node);
		if (error != 0)
			return error;
	}

	/* Then its descendants in tree order. */
	for (child = node->first_child; child != NULL; child = child->next) {
		error = script_walk_inserted(page, child, depth + 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the subtree's scripts were prepared. */
	return 0;
}

/* Prepares one script made by DOM operations: inline now, or an external script asynchronously. */
static int
script_prepare_dynamic(
	struct page *page,
	struct dom_element *script)
{
	struct dom_attribute *src;
	struct dom_node *child;
	const struct dom_character_data *data;
	struct wb_units text;
	const char *name;
	int runs;
	int error;

	/* A prepared script is never prepared again, even when it is moved. */
	if ((script->node.flags & DOM_NODE_SCRIPT_STARTED) != 0)
		return 0;
	script->node.flags |= DOM_NODE_SCRIPT_STARTED;

	/* Modules and data blocks are outside this pass. */
	runs = script_type_runs(script);
	if (!runs)
		return 0;

	/* External dynamic scripts are fetched now and run in a later task. */
	error = script_attribute(page, script, "src", &src);
	if (error != 0)
		return error;
	if (src != NULL)
		return script_prepare_external(page, script, src->value);

	/* An inline dynamic script runs synchronously during its insertion. */
	wb_units_init(&text);
	error = 0;
	for (child = script->node.first_child; child != NULL && error == 0; child = child->next) {
		if (child->type != DOM_TEXT && child->type != DOM_CDATA_SECTION)
			continue;
		data = (const struct dom_character_data *)child;
		error = wb_units_append(&text, data->data.data, data->data.length);
	}

	/* The inline source runs under the page's name. */
	name = page->base;
	if (name == NULL)
		name = "(inline)";
	bind_window_set_current_script(page->window, &script->node);
	if (error == 0)
		error = bind_run_script_source(page->window, text.data, text.length, name, 1);
	bind_window_set_current_script(page->window, NULL);
	wb_units_release(&text);

	/* Succeeded, or memory ran out. */
	return error;
}

/* Makes an entry for an external dynamic script and starts or performs its fetch. */
static int
script_prepare_external(
	struct page *page,
	struct dom_element *script,
	const struct vm_string *src)
{
	struct page_script *entry;
	struct wb_buffer href;
	struct wb_buffer location;
	struct wb_buffer bytes;
	struct page_script *last;
	int remote;
	int error;

	/* A stable entry owns the source and roots its element while pending. */
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return ENOMEM;
	entry->page = page;
	entry->element = script;
	entry->ordered = (script->node.flags & DOM_NODE_SCRIPT_ORDERED) != 0;
	wb_units_init(&entry->source);
	wb_buffer_init(&href);
	wb_buffer_init(&location);
	wb_buffer_init(&bytes);

	/* Resolves the source before keeping the entry. */
	error = vm_string_to_utf8(src, &href);
	if (error == 0 && page->base == NULL)
		error = EINVAL;
	if (error == 0)
		error = page_resolve_location(page->base, wb_buffer_string(&href), &location);
	if (error == 0) {
		entry->location = strdup(wb_buffer_string(&location));
		if (entry->location == NULL)
			error = ENOMEM;
	}

	/* The table keeps the entry stable, and its element is a root until completion. */
	if (error == 0)
		error = wb_vector_push(&page->scripts, &entry);
	if (error == 0)
		error = vm_heap_add_root(page->heap, (struct vm_cell **)&entry->element);
	if (error == 0)
		entry->rooted = 1;
	if (error != 0) {
		last = NULL;
		if (page->scripts.count != 0)
			last = *(struct page_script **)wb_vector_at(&page->scripts, page->scripts.count - 1);
		if (last == entry)
			page->scripts.count--;
		wb_buffer_release(&href);
		wb_buffer_release(&location);
		wb_buffer_release(&bytes);
		script_release(entry);
		return error;
	}

	/* Web URLs use the page's asynchronous loader; local bytes are ready for the next checkpoint. */
	remote = net_loader_takes(entry->location);
	if (page->loader != NULL && remote) {
		error = net_loader_fetch(page->loader, entry->location, script_arrived, entry, &entry->request);
	} else {
		error = page_fetch(page->base, entry->location, &bytes, NULL);
		if (error == 0)
			error = script_decode(entry, bytes.data, bytes.length);
		entry->ready = 1;
	}

	/* Fetch failures become an error event; only allocation failure aborts the DOM operation. */
	if (error != 0) {
		entry->failed = 1;
		entry->ready = 1;
		if (error == ENOMEM) {
			wb_buffer_release(&href);
			wb_buffer_release(&location);
			wb_buffer_release(&bytes);
			return error;
		}
	}

	/* The temporary buffers no longer own anything. */
	wb_buffer_release(&href);
	wb_buffer_release(&location);
	wb_buffer_release(&bytes);
	return 0;
}

/* Decodes one external script as UTF-8, dropping its byte order mark. */
static int
script_decode(
	struct page_script *entry,
	const unsigned char *bytes,
	size_t length)
{
	int error;

	/* Drops the UTF-8 byte order mark. */
	if (length >= 3 && bytes[0] == 0xefU && bytes[1] == 0xbbU && bytes[2] == 0xbfU) {
		bytes += 3;
		length -= 3;
	}

	/* The script engine takes UTF-16 units. */
	error = wb_utf8_to_units(bytes, length, &entry->source);
	return error;
}

/* Runs every ready external script in insertion order. */
static int
script_run_ready(
	struct page *page)
{
	struct page_script *entry;
	size_t index;
	int ordered_blocked;
	int error;

	/* A script's checkpoint may recurse while this loop runs. */
	if (page->scripts_running)
		return 0;

	/* Each ready entry runs once, in the table's insertion order. */
	page->scripts_running = 1;
	error = 0;
	ordered_blocked = 0;
	for (index = 0; index < page->scripts.count && error == 0; index++) {
		entry = *(struct page_script **)wb_vector_at(&page->scripts, index);
		if (entry->done)
			continue;
		if (entry->ordered && !entry->ready) {
			ordered_blocked = 1;
			continue;
		}

		/* Async entries may pass an ordered one that is still waiting. */
		if (!entry->ready || (entry->ordered && ordered_blocked))
			continue;
		entry->ready = 0;
		error = script_finish(entry);
	}

	/* Later checkpoints may drain entries that arrive afterwards. */
	page->scripts_running = 0;
	return error;
}

/* Runs a fetched script, fires load or error, and lets its element be collected. */
static int
script_finish(
	struct page_script *entry)
{
	const char *event_type;
	int canceled;
	int error;

	/* Successful bytes run; a fetch failure skips execution. */
	error = 0;
	if (!entry->failed) {
		bind_window_set_current_script(entry->page->window, &entry->element->node);
		error = bind_run_script(entry->page->window, entry->source.data, entry->source.length, entry->location);
		bind_window_set_current_script(entry->page->window, NULL);
	}
	event_type = "load";
	if (entry->failed)
		event_type = "error";
	if (error == 0)
		error = bind_fire_event(entry->page->window, &entry->element->node, event_type, 0, &canceled);

	/* Completion releases the source and the element's temporary root. */
	entry->done = 1;
	vm_heap_remove_root(entry->page->heap, (struct vm_cell **)&entry->element);
	entry->rooted = 0;
	entry->element = NULL;
	wb_units_release(&entry->source);
	return error;
}

/* The asynchronous loader completed an external script. */
static void
script_arrived(
	void *context,
	struct net_request *request)
{
	const struct net_response *response;
	struct page_script *entry;
	char *location;
	int error;

	/* The request ended and its response is valid for this callback. */
	entry = context;
	entry->request = NULL;
	error = net_request_error(request);
	response = net_request_response(request);
	if (error == 0 && (response->status < 200 || response->status > 299))
		error = EINVAL;
	if (error == 0) {
		location = strdup(wb_buffer_string(&response->url));
		if (location == NULL) {
			error = ENOMEM;
		} else {
			free(entry->location);
			entry->location = location;
		}
	}

	/* Decodes a successful response, or records a load failure. */
	if (error == 0)
		error = script_decode(entry, response->body.data, response->body.length);
	if (error != 0)
		entry->failed = 1;
	entry->ready = 1;

	/* Loader callbacks are task boundaries. */
	(void)script_run_ready(entry->page);
}

/* Releases one dynamic script entry. */
static void
script_release(
	struct page_script *entry)
{
	if (entry == NULL)
		return;
	net_request_cancel(entry->request);
	if (entry->rooted)
		vm_heap_remove_root(entry->page->heap, (struct vm_cell **)&entry->element);
	wb_units_release(&entry->source);
	free(entry->location);
	free(entry);
}

/*
 * Tells the scripts the document is parsed: DOMContentLoaded at the
 * document, then load at the window.
 */
int
page_fire_load(
	struct page *page)
{
	int canceled;
	int error;

	/* The document is interactive, and its DOMContentLoaded bubbles to the window. */
	bind_window_set_ready_state(page->window, "interactive");
	error = bind_fire_event(page->window, &page->document->node, "DOMContentLoaded", BIND_EVENT_BUBBLES, &canceled);
	if (error != 0)
		return error;

	/* Then it is complete, and the window's load fires. */
	bind_window_set_ready_state(page->window, "complete");
	error = bind_fire_event(page->window, NULL, "load", BIND_EVENT_DOCUMENT, &canceled);
	if (error != 0)
		return error;

	/* Succeeded: the load events have run. */
	return 0;
}

/*
 * Moves the page's clock to now (milliseconds since the page began) and
 * runs the timers that are due.
 */
int
page_set_time(
	struct page *page,
	double now)
{
	int error;

	/* The clock only moves forward. */
	if (now > page->now)
		page->now = now;
	bind_window_set_time(page->window, page->now);

	/* The timers due by then. */
	error = bind_run_timers(page->window);
	if (error != 0)
		return error;

	/* Drain responses only outside loader callbacks, after the ordinary timers. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		return error;

	/* Succeeded: no timer is overdue. */
	return 0;
}

/*
 * Reports when the page's next timer is due; zero when it has none.
 */
int
page_next_timer(
	const struct page *page,
	double *due)
{
	int found;

	/* The window keeps the timers. */
	found = bind_next_timer(page->window, due);

	/* Whether there is one. */
	return found;
}

/*
 * Settles virtual timers and actual pending child resources within explicit bounds.
 */
int
page_settle(
	struct page *page,
	double budget)
{
	double due;
	double wait;
	double elapsed;
	double network_waited;
	double next;
	int rounds;
	int found;
	int error;

	/* Only actual child work slows virtual jumps; ordinary timer-only settling stays fast. */
	network_waited = 0;
	for (rounds = 0; rounds < SCRIPT_SETTLE_ROUNDS; rounds++) {
		/* Discover newly connected sources and dispatch already copied native response tasks. */
		error = page_frames_checkpoint(page);
		if (error != 0)
			return error;
		found = page_next_timer(page, &due);

		/* Pending real responses need descriptor progress before the virtual clock can leap ahead. */
		if (page->frame_loads.count != 0) {
			/* Unfinished real work cannot be reported as settled at an exhausted bound. */
			if (page->now >= budget || network_waited >= 30000)
				return ETIMEDOUT;

			/* A short poll yields to due timers, including source replacement or cancellation. */
			wait = budget - page->now;
			if (wait > 10)
				wait = 10;
			if (found && due - page->now < wait)
				wait = due - page->now;
			if (wait < 0)
				wait = 0;
			error = script_settle_network(page, wait, &elapsed);
			if (error != 0)
				return error;

			/* Actual monotonic waiting time advances this virtual clock without inventing network completion. */
			network_waited += elapsed;
			next = page->now + elapsed;
			if (next > budget)
				next = budget;
			error = page_set_time(page, next);
			if (error != 0)
				return error;
			continue;
		}

		/* With no pending child response, preserve the original timer-to-timer fast path. */
		if (!found || due > budget)
			return 0;
		error = page_set_time(page, due);
		if (error != 0)
			return error;
	}

	/* A request or eligible timer surviving the task bound is an explicit incomplete run. */
	if (page->frame_loads.count != 0)
		return ETIMEDOUT;
	found = page_next_timer(page, &due);
	if (found && due <= budget)
		return ETIMEDOUT;

	/* Succeeded: no eligible timer or actual pending child work remains. */
	return 0;
}

/*
 * Tells whether the document changed since the page was laid out.
 */
int
page_needs_layout(
	const struct page *page)
{
	int pending;

	/* A page never laid out needs it. */
	if (!page->laid_out)
		return 1;

	/* A change since the layout. */
	if (page->laid_out_generation != page->document->generation)
		return 1;
	if (page->laid_out_visits != page->visits_generation)
		return 1;

	/* Images that arrived since the layout. */
	if (page->laid_out_images != page->images_generation)
		return 1;

	/* Style sheets that arrived since the styling, once no other is on its way (the page is styled again once for them all). */
	if (page->styled_sheets != page->sheets_generation) {
		pending = page_sheets_pending(page);
		if (!pending)
			return 1;
	}

	/* Web fonts that arrived since the layout (ws074-p070). */
	if (page->laid_out_fonts != page->fonts_generation)
		return 1;

	/* The layout is up to date. */
	return 0;
}

/* Writes a console line to the embedder's console, or to standard error. */
static void
script_console(
	void *context,
	int level,
	const char *text,
	size_t length)
{
	struct page *page;

	/* The embedder's console, when it has one. */
	page = context;
	if (page->console != NULL) {
		page->console(page->console_context, level, text, length);
		return;
	}

	/* Otherwise the line goes to standard error. */
	fprintf(stderr, "console: %.*s\n", (int)length, text);
}

/*
 * Writes a part of the page's location (a BIND_LOCATION_*; the window's
 * host callback, context is the page): about:blank's for a page without
 * a location.
 */
static int
script_location(
	void *context,
	int part,
	struct wb_buffer *out)
{
	struct net_url url;
	int error;

	/* A part outside the table writes nothing. */
	if (part < 0 || part > BIND_LOCATION_HASH)
		return 0;

	/* The page's URL. */
	error = page_url(context, &url);
	if (error == ENOMEM)
		return error;
	if (error != 0) {
		error = net_url_parse("about:blank", strlen("about:blank"), NULL, &url);
		if (error != 0)
			return error;
	}

	/* The part, as the URL interface writes it. */
	error = net_url_component(&url, script_location_parts[part], out);
	net_url_release(&url);
	if (error != 0)
		return error;

	/* Succeeded: the part is written. */
	return 0;
}

/*
 * Changes this page's URL to another of the same document (a fragment, a
 * history entry a script pushed); the href is resolved against the URL.
 */
static int
script_url_set(
	void *context,
	const char *href,
	size_t length)
{
	struct page *page;
	struct net_url current;
	struct net_url next;
	struct wb_buffer serialized;
	char *copy;
	int error;

	/* The new URL, resolved against the page's. */
	page = context;
	error = page_url(page, &current);
	if (error != 0)
		return error;
	error = net_url_parse(href, length, &current, &next);
	net_url_release(&current);
	if (error != 0)
		return error;

	/* Its text. */
	wb_buffer_init(&serialized);
	error = net_url_serialize(&next, 0, &serialized);
	net_url_release(&next);
	if (error != 0) {
		wb_buffer_release(&serialized);
		return error;
	}

	/* The page keeps the text as its URL. */
	copy = strdup(wb_buffer_string(&serialized));
	wb_buffer_release(&serialized);
	if (copy == NULL)
		return ENOMEM;
	free(page->base);
	page->base = copy;

	/* Succeeded: the page has the new URL. */
	return 0;
}

/* Writes the cookies a script may see for the page's URL (the window's host callback). */
static int
script_cookie_get(
	void *context,
	struct wb_buffer *out)
{
	struct net_url url;
	int error;

	/* A page without a URL has no cookies. */
	error = page_url(context, &url);
	if (error == ENOMEM)
		return error;
	if (error != 0)
		return 0;

	/* The jar's pairs for the URL. */
	error = net_cookie_string(&url, out);
	net_url_release(&url);
	if (error != 0)
		return error;

	/* Succeeded: the cookies are written. */
	return 0;
}

/* Keeps a cookie a script set on the page (the window's host callback). */
static int
script_cookie_set(
	void *context,
	const char *text,
	size_t length)
{
	struct net_url url;
	int error;

	/* A page without a URL keeps no cookies. */
	error = page_url(context, &url);
	if (error == ENOMEM)
		return error;
	if (error != 0)
		return 0;

	/* The jar keeps it, unless it would touch an HttpOnly cookie. */
	error = net_cookie_store_script(&url, text, length);
	net_url_release(&url);
	if (error != 0)
		return error;

	/* Succeeded: the cookie is kept (or refused). */
	return 0;
}

/* Tells whether a parser-inserted script asks for the module path. */
static int
script_type_module(
	const struct dom_element *script)
{
	const struct dom_attribute *attribute;
	size_t index;
	int same;

	for (index = 0; index < script->attribute_count; index++) {
		attribute = &script->attributes[index];
		if (attribute->ns != DOM_NS_NONE)
			continue;
		same = vm_string_equal_ascii(attribute->name, "type");
		if (!same)
			continue;
		return script_ascii_equal_folded(attribute->value, "module");
	}
	return 0;
}

/* Tells whether a script element's type attribute asks for a classic script. */
static int
script_type_runs(
	const struct dom_element *script)
{
	const struct dom_attribute *attribute;
	const struct vm_string *type;
	size_t index;
	int same;

	/* The type attribute (found by comparing names: the element's atoms are the parser's). */
	type = NULL;
	for (index = 0; index < script->attribute_count; index++) {
		attribute = &script->attributes[index];
		if (attribute->ns != DOM_NS_NONE)
			continue;
		same = vm_string_equal_ascii(attribute->name, "type");
		if (same) {
			type = attribute->value;
			break;
		}
	}

	/* No type, or an empty one, is JavaScript. */
	if (type == NULL || type->length == 0)
		return 1;

	/* Otherwise one of the JavaScript MIME types, in any case. */
	for (index = 0; script_types[index] != NULL; index++) {
		same = script_ascii_equal_folded(type, script_types[index]);
		if (same)
			return 1;
	}

	/* Another type (a module, a template, data) does not run in this pass. */
	return 0;
}

/* Finds an element's attribute of no namespace by its ASCII name (*attribute is NULL when it has none). */
static int
script_attribute(
	struct page *page,
	const struct dom_element *element,
	const char *name,
	struct dom_attribute **attribute)
{
	struct vm_string *atom;

	/* The name's atom. */
	*attribute = NULL;
	atom = vm_atom_from_ascii(page->heap, name);
	if (atom == NULL)
		return ENOMEM;

	/* The attribute. */
	*attribute = dom_element_find_attribute(element, DOM_NS_NONE, atom);

	/* Succeeded: the attribute (or none) is found. */
	return 0;
}

/* Runs the script file a src names, resolved against the page's file; a failure goes to the console. */
static int
script_run_file(
	struct page *page,
	const struct vm_string *src)
{
	struct wb_buffer href;
	struct wb_buffer path;
	struct wb_buffer bytes;
	struct wb_buffer line;
	struct wb_units units;
	const unsigned char *data;
	size_t length;
	size_t name_length;
	int error;

	/* The src as UTF-8, and the bytes of the file or data: URL it names. */
	wb_buffer_init(&href);
	wb_buffer_init(&path);
	wb_buffer_init(&bytes);
	wb_buffer_init(&line);
	wb_units_init(&units);
	error = vm_string_to_utf8(src, &href);
	if (error == 0 && page->base == NULL)
		error = EINVAL;
	if (error == 0)
		error = script_fetch_file(page, wb_buffer_string(&href), &bytes);
	name_length = href.length;
	if (name_length > SCRIPT_NAME_MAX)
		name_length = SCRIPT_NAME_MAX;
	if (error == 0)
		error = wb_buffer_append(&path, href.data, name_length);

	/* The bytes, as UTF-8 text without a byte order mark. */
	data = bytes.data;
	length = bytes.length;
	if (error == 0 && length >= 3 && data[0] == 0xefU && data[1] == 0xbbU && data[2] == 0xbfU) {
		data += 3;
		length -= 3;
	}

	/* The text as UTF-16. */
	if (error == 0)
		error = wb_utf8_to_units(data, length, &units);

	/* The script runs, or the console says why it could not load. */
	if (error == 0) {
		error = bind_run_script(page->window, units.data, units.length, wb_buffer_string(&path));
	} else if (error != ENOMEM) {
		error = wb_buffer_printf(&line, "Failed to load the script %s: %s", wb_buffer_string(&href), strerror(error));
		if (error == 0)
			bind_console(page->window, BIND_CONSOLE_ERROR, wb_buffer_string(&line));
	}

	/* Frees the buffers. */
	wb_buffer_release(&href);
	wb_buffer_release(&path);
	wb_buffer_release(&bytes);
	wb_buffer_release(&line);
	wb_units_release(&units);
	if (error != 0 && getenv("BROWSER_STACK") != NULL)
		fprintf(stderr, "script: stopped by error %d (%s)\n", error, strerror(error));
	if (error != 0)
		return error;

	/* Succeeded: the script ran, or its failure was reported. */
	return 0;
}

/* Tells whether a string equals lower-case ASCII text, ignoring the case of its ASCII letters. */
static int
script_ascii_equal_folded(
	const struct vm_string *string,
	const char *ascii)
{
	uint16_t unit;
	size_t length;
	size_t index;

	/* The lengths must match. */
	length = strlen(ascii);
	if (string->length != length)
		return 0;

	/* Then every unit, folded. */
	for (index = 0; index < string->length; index++) {
		unit = vm_string_at(string, index);
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit + 0x20U);
		if (unit != (unsigned char)ascii[index])
			return 0;
	}

	/* The same text. */
	return 1;
}

/* Reads a parser-blocking script's bytes: the ones fetched before the document was parsed, or else at once (BUG-207). */
static int
script_fetch_file(
	struct page *page,
	const char *href,
	struct wb_buffer *bytes)
{
	const struct page_prefetched *entry;
	struct wb_buffer location;
	int error;

	/* The script's location, resolved as the view resolved it when it fetched the scripts ahead. */
	entry = NULL;
	wb_buffer_init(&location);
	error = page_resolve_location(page->base, href, &location);
	if (error == 0)
		entry = script_prefetched(page, wb_buffer_string(&location));
	wb_buffer_release(&location);

	/* A script fetched ahead ends as its fetch ended, without the network. */
	if (entry != NULL) {
		if (entry->error != 0)
			return entry->error;

		/* Its bytes, copied for the caller. */
		error = wb_buffer_append(bytes, entry->bytes.data, entry->bytes.length);
		if (error != 0)
			return error;

		/* Succeeded: the bytes fetched ahead. */
		return 0;
	}

	/* Any other (a local file, or one the scan did not find) is read at once. */
	error = page_fetch(page->base, href, bytes, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the bytes are read. */
	return 0;
}

/* Finds the script fetched ahead from a location; NULL when there is none. */
static const struct page_prefetched *
script_prefetched(
	const struct page *page,
	const char *location)
{
	const struct page_prefetched *entry;
	size_t index;
	int same;

	/* Each script fetched ahead, by its location. */
	for (index = 0; index < page->prefetched.count; index++) {
		entry = *(const struct page_prefetched **)wb_vector_at(&page->prefetched, index);
		same = strcmp(entry->location, location);
		if (same == 0)
			return entry;
	}

	/* None was fetched from there. */
	return NULL;
}

/* Tells whether the markup at an offset starts with lower-case ASCII text, ignoring the case of its letters. */
static int
script_scan_starts(
	const unsigned char *bytes,
	size_t length,
	size_t at,
	const char *lower)
{
	size_t index;
	unsigned char c;

	/* Each character of the text against the markup's, in lower case. */
	for (index = 0; lower[index] != '\0'; index++) {
		if (at + index >= length)
			return 0;
		c = bytes[at + index];
		if (c >= 'A' && c <= 'Z')
			c = (unsigned char)(c - 'A' + 'a');
		if (c != (unsigned char)lower[index])
			return 0;
	}

	/* Every character matched. */
	return 1;
}

/* Reports the offset of the first place from an offset where lower-case ASCII text starts (ignoring case); length when it does not. */
static size_t
script_scan_find(
	const unsigned char *bytes,
	size_t length,
	size_t at,
	const char *lower)
{
	int found;

	/* Each place in turn. */
	for (; at < length; at++) {
		found = script_scan_starts(bytes, length, at, lower);
		if (found)
			return at;
	}

	/* The text is not there. */
	return length;
}

/*
 * Reads a script start tag's attributes from after its name to past its
 * ">": the value of its first src (with the character references
 * decoded) and whether its type runs.  Returns EINVAL for a tag the
 * document does not close, or ENOMEM.
 */
static int
script_scan_tag(
	const unsigned char *bytes,
	size_t length,
	size_t *at,
	struct wb_buffer *src,
	int *has_src,
	int *runs)
{
	struct wb_buffer type;
	size_t place;
	size_t name;
	size_t name_end;
	size_t value;
	size_t value_end;
	unsigned char quote;
	int has_value;
	int is_src;
	int is_type;
	int has_type;
	int error;

	/* No src or type seen yet. */
	*has_src = 0;
	*runs = 1;
	has_type = 0;
	wb_buffer_init(&type);
	place = *at;
	error = 0;

	/* Each attribute up to the ">" that ends the tag. */
	for (;;) {
		/* The spaces and slashes between attributes. */
		while (place < length &&
		       (bytes[place] == ' ' ||
			bytes[place] == '\t' ||
			bytes[place] == '\n' ||
			bytes[place] == '\r' ||
			bytes[place] == '\f' ||
			bytes[place] == '/'))
			place++;

		/* A tag the document does not close. */
		if (place >= length) {
			error = EINVAL;
			break;
		}

		/* The end of the tag. */
		if (bytes[place] == '>') {
			place++;
			break;
		}

		/* The attribute's name, up to a space, "=", "/" or ">". */
		name = place;
		while (place < length &&
		       bytes[place] != ' ' &&
		       bytes[place] != '\t' &&
		       bytes[place] != '\n' &&
		       bytes[place] != '\r' &&
		       bytes[place] != '\f' &&
		       bytes[place] != '=' &&
		       bytes[place] != '/' &&
		       bytes[place] != '>')
			place++;
		name_end = place;

		/* The spaces before a value. */
		while (place < length &&
		       (bytes[place] == ' ' ||
			bytes[place] == '\t' ||
			bytes[place] == '\n' ||
			bytes[place] == '\r' ||
			bytes[place] == '\f'))
			place++;

		/* A value after "=": quoted, or up to a space or ">". */
		has_value = 0;
		value = place;
		value_end = place;
		if (place < length && bytes[place] == '=') {
			place++;

			/* The spaces after the "=". */
			while (place < length &&
			       (bytes[place] == ' ' ||
				bytes[place] == '\t' ||
				bytes[place] == '\n' ||
				bytes[place] == '\r' ||
				bytes[place] == '\f'))
				place++;

			/* A quoted value ends at its quote; another at a space or ">". */
			has_value = 1;
			if (place < length && (bytes[place] == '"' || bytes[place] == '\'')) {
				quote = bytes[place];
				place++;
				value = place;
				while (place < length && bytes[place] != quote)
					place++;
				value_end = place;
				if (place < length)
					place++;
			} else {
				value = place;
				while (place < length &&
				       bytes[place] != ' ' &&
				       bytes[place] != '\t' &&
				       bytes[place] != '\n' &&
				       bytes[place] != '\r' &&
				       bytes[place] != '\f' &&
				       bytes[place] != '>')
					place++;
				value_end = place;
			}
		}

		/* Which attribute it is: the first src and the first type count, as in the parser. */
		is_src = 0;
		if (name_end - name == 3U)
			is_src = script_scan_starts(bytes, name_end, name, "src");
		is_type = 0;
		if (name_end - name == 4U)
			is_type = script_scan_starts(bytes, name_end, name, "type");

		/* The src's value, its references decoded. */
		if (is_src && !*has_src) {
			*has_src = 1;
			if (has_value)
				error = script_scan_value(bytes, value, value_end, src);
			if (error != 0)
				break;
		}

		/* The type's value, likewise. */
		if (is_type && !has_type) {
			has_type = 1;
			if (has_value)
				error = script_scan_value(bytes, value, value_end, &type);
			if (error != 0)
				break;
		}
	}

	/* Whether the type runs (no type runs). */
	if (error == 0 && has_type)
		*runs = script_scan_type_runs(&type);
	wb_buffer_release(&type);

	/* Reports a tag that could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the tag is read up to past its ">". */
	*at = place;
	return 0;
}

/* Appends an attribute's value with its common character references (&amp; and the like, and numeric ones) decoded. */
static int
script_scan_value(
	const unsigned char *bytes,
	size_t start,
	size_t end,
	struct wb_buffer *out)
{
	unsigned long code;
	unsigned long radix;
	size_t place;
	size_t digits;
	int hexadecimal;
	int error;
	unsigned char c;

	/* Each byte, or each reference. */
	place = start;
	while (place < end) {
		c = bytes[place];

		/* A byte that starts no reference is itself. */
		if (c != '&') {
			error = wb_buffer_append_byte(out, c);
			if (error != 0)
				return error;
			place++;
			continue;
		}

		/* The named references a URL is likely to hold. */
		if (script_scan_starts(bytes, end, place, "&amp;")) {
			error = wb_buffer_append_byte(out, '&');
			place += 5U;
		} else if (script_scan_starts(bytes, end, place, "&quot;")) {
			error = wb_buffer_append_byte(out, '"');
			place += 6U;
		} else if (script_scan_starts(bytes, end, place, "&apos;")) {
			error = wb_buffer_append_byte(out, '\'');
			place += 6U;
		} else if (script_scan_starts(bytes, end, place, "&lt;")) {
			error = wb_buffer_append_byte(out, '<');
			place += 4U;
		} else if (script_scan_starts(bytes, end, place, "&gt;")) {
			error = wb_buffer_append_byte(out, '>');
			place += 4U;
		} else if (script_scan_starts(bytes, end, place, "&#")) {
			/* A numeric reference: decimal, or hexadecimal after "x". */
			place += 2U;
			hexadecimal = 0;
			radix = 10UL;
			if (place < end && (bytes[place] == 'x' || bytes[place] == 'X')) {
				hexadecimal = 1;
				radix = 16UL;
				place++;
			}

			/* Its digits, up to a value no character has. */
			code = 0;
			digits = 0;
			while (place < end && code <= 0x10ffffUL) {
				c = bytes[place];
				if (c >= '0' && c <= '9') {
					code = code * radix + (unsigned long)(c - '0');
				} else if (hexadecimal && c >= 'a' && c <= 'f') {
					code = code * 16UL + (unsigned long)(c - 'a' + 10);
				} else if (hexadecimal && c >= 'A' && c <= 'F') {
					code = code * 16UL + (unsigned long)(c - 'A' + 10);
				} else {
					break;
				}
				place++;
				digits++;
			}

			/* Its ";", and the character (a reference without digits or out of range is a replacement character). */
			if (place < end && bytes[place] == ';')
				place++;
			if (digits == 0 || code == 0 || code > 0x10ffffUL)
				code = 0xfffdUL;
			error = wb_buffer_append_utf8(out, (uint32_t)code);
		} else {
			/* Any other "&" is itself. */
			error = wb_buffer_append_byte(out, '&');
			place++;
		}

		/* Memory ran out. */
		if (error != 0)
			return error;
	}

	/* Succeeded: the value is appended. */
	return 0;
}

/* Tells whether a script tag's type (its value as read) runs as a classic script. */
static int
script_scan_type_runs(
	const struct wb_buffer *type)
{
	size_t index;
	int same;

	/* An empty type is JavaScript. */
	if (type->length == 0)
		return 1;

	/* Otherwise one of the JavaScript MIME types, in any case. */
	for (index = 0; script_types[index] != NULL; index++) {
		if (strlen(script_types[index]) != type->length)
			continue;
		same = script_scan_starts(type->data, type->length, 0, script_types[index]);
		if (same)
			return 1;
	}

	/* Another type (a module, a template, data) does not run. */
	return 0;
}

/* Adds a src's web location, resolved against base, to the scripts to fetch ahead, once. */
static int
script_scan_add(
	const char *base,
	const struct wb_buffer *src,
	struct wb_vector *locations)
{
	struct wb_buffer location;
	const char *known;
	char *copy;
	size_t index;
	int remote;
	int same;
	int error;

	/* The location the parser would fetch (a src that is no URL is the parser's to report). */
	wb_buffer_init(&location);
	error = page_resolve_location(base, wb_buffer_string(src), &location);
	if (error != 0) {
		wb_buffer_release(&location);
		if (error == ENOMEM)
			return error;
		return 0;
	}

	/* Only an http or https script is fetched ahead; a local one is read at once anyway. */
	remote = net_loader_takes(wb_buffer_string(&location));
	if (!remote) {
		wb_buffer_release(&location);
		return 0;
	}

	/* A location already named is fetched once. */
	for (index = 0; index < locations->count; index++) {
		known = *(const char **)wb_vector_at(locations, index);
		same = strcmp(known, wb_buffer_string(&location));
		if (same == 0) {
			wb_buffer_release(&location);
			return 0;
		}
	}

	/* Its copy, for the caller. */
	copy = strdup(wb_buffer_string(&location));
	wb_buffer_release(&location);
	if (copy == NULL)
		return ENOMEM;

	/* The list keeps it. */
	error = wb_vector_push(locations, &copy);
	if (error != 0) {
		free(copy);
		return error;
	}

	/* Succeeded: the script is one to fetch ahead. */
	return 0;
}

/* Runs the parser-inserted deferred scripts, in document order, once parsing has ended. */
int
page_deferred_run(
	struct page *page)
{
	struct dom_element *script;
	struct dom_attribute *src;
	size_t index;
	int error;

	for (index = 0; index < page->deferred_scripts.count; index++) {
		script = *(struct dom_element **)wb_vector_at(&page->deferred_scripts, index);
		error = script_attribute(page, script, "src", &src);
		if (error != 0 || src == NULL)
			return error;
		bind_window_set_current_script(page->window, &script->node);
		error = script_run_file(page, src->value);
		bind_window_set_current_script(page->window, NULL);
		if (error != 0)
			return error;
	}
	page->deferred_scripts.count = 0;
	return 0;
}
