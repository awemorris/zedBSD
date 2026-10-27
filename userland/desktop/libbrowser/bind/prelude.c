/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the web platform written in the page's own language
 * (ws074-p230): AbortController, URL and URLSearchParams (over the browser's
 * own URL parser), the session history (History, Location's navigation and
 * the Navigation API, over the natives that change the document's URL and
 * scroll to a fragment), and the smaller objects that scripts probe for
 * (Intl, Reflect, matchMedia, MessageChannel).  The source is
 * prelude-source.inc, made from plan/ws074/phase230/prelude.js.
 */

#include "bind/internal.h"
#include "js/builtin.h"
#include "net/net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bind/prelude-source.inc"
#include "bind/prelude-net.inc"

/* The parts of a URL in the order the prelude reads them (enum net_url_part). */
#define PRELUDE_URL_PARTS	11

static int prelude_set_url(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_scroll_fragment(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_fire(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_set_follower(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_utf8_argument(const vm_value *args, unsigned count, unsigned index, struct wb_buffer *out, int *present);

/* parseURL(input, base): the parts of the URL as an array of strings, or null when it does not parse. */
static int
prelude_parse_url(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct net_url base;
	struct net_url url;
	struct wb_buffer text;
	struct wb_buffer part;
	struct vm_string *string;
	vm_value parts[PRELUDE_URL_PARTS];
	int have_base;
	int index;
	int status;

	UNUSED_PARAMETER(this_value);
	*result = VM_VALUE_NULL;

	/* The input and the optional base as UTF-8. */
	wb_buffer_init(&text);
	wb_buffer_init(&part);
	status = vm_string_to_utf8((const struct vm_string *)vm_value_as_cell(js_argument(args, count, 0)), &text);
	if (status != 0) {
		wb_buffer_release(&text);
		return status;
	}
	have_base = 0;
	if (js_argument(args, count, 1) != VM_VALUE_UNDEFINED) {
		status = vm_string_to_utf8((const struct vm_string *)vm_value_as_cell(js_argument(args, count, 1)), &part);
		if (status == 0)
			status = net_url_parse(wb_buffer_string(&part), part.length, NULL, &base);
		if (status != 0) {
			wb_buffer_release(&text);
			wb_buffer_release(&part);
			return status == ENOMEM ? status : 0;
		}
		have_base = 1;
	}

	/* The parse. */
	status = net_url_parse(wb_buffer_string(&text), text.length, have_base ? &base : NULL, &url);
	if (have_base)
		net_url_release(&base);
	wb_buffer_release(&text);
	wb_buffer_release(&part);
	if (status != 0)
		return status == ENOMEM ? status : 0;

	/* Each part as a string. */
	for (index = 0; index < PRELUDE_URL_PARTS && status == 0; index++) {
		wb_buffer_init(&part);
		status = net_url_component(&url, index, &part);
		if (status == 0) {
			string = vm_string_from_utf8(realm->heap, part.data == NULL ? "" : wb_buffer_string(&part), part.length);
			if (string == NULL)
				status = ENOMEM;
			else
				parts[index] = vm_value_cell(string);
		}
		wb_buffer_release(&part);
	}
	net_url_release(&url);
	if (status != 0)
		return status;
	return js_builtin_array(realm, parts, PRELUDE_URL_PARTS, result);
}

/* setURL(href): changes the document's URL within the document; false when the host cannot. */
static int
prelude_set_url(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_buffer href;
	int present;
	int status;

	UNUSED_PARAMETER(this_value);

	/* A frame's URL belongs to its frame, not to the page's host. */
	window = bind_window_of(realm);
	*result = vm_value_boolean(0);
	if (window->owned || window->host.url_set == NULL)
		return 0;

	/* The href as UTF-8. */
	wb_buffer_init(&href);
	status = prelude_utf8_argument(args, count, 0, &href, &present);
	if (status != 0 || !present) {
		wb_buffer_release(&href);
		return status;
	}

	/* The host takes the new URL; a URL it cannot parse leaves the old one. */
	status = window->host.url_set(window->host.context, wb_buffer_string(&href), href.length);
	wb_buffer_release(&href);
	if (status == ENOMEM)
		return status;
	if (status != 0)
		return 0;

	/* Succeeded: the document has the new URL. */
	*result = vm_value_boolean(1);
	return 0;
}

/* scrollToFragment(fragment): scrolls to the element the fragment names; whether one was found. */
static int
prelude_scroll_fragment(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_buffer fragment;
	int present;
	int found;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The fragment as UTF-8. */
	window = bind_window_of(realm);
	*result = vm_value_boolean(0);
	wb_buffer_init(&fragment);
	status = prelude_utf8_argument(args, count, 0, &fragment, &present);
	if (status != 0 || !present) {
		wb_buffer_release(&fragment);
		return status;
	}

	/* The target, revealed and scrolled to. */
	found = 0;
	if (window->document != NULL)
		status = bind_scroll_to_fragment(window, &fragment, &found);
	wb_buffer_release(&fragment);
	if (status != 0)
		return status;

	/* Succeeded: whether the fragment named an element. */
	*result = vm_value_boolean(found);
	return 0;
}

/* fire(target, event): dispatches an event the browser fires (it is trusted); whether no listener canceled it. */
static int
prelude_fire(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct bind_listeners *listeners;
	struct bind_event *event;
	vm_value target;
	int canceled;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The target must be able to have listeners. */
	window = bind_window_of(realm);
	target = js_argument(args, count, 0);
	status = bind_listeners_of(window, target, 0, &listeners);
	if (status == EINVAL)
		return bind_throw_illegal(realm);
	if (status != 0)
		return status;

	/* The event, which must not be in a dispatch already. */
	event = bind_event_of(js_argument(args, count, 1));
	if (event == NULL)
		return vm_throw_type_error(realm, "fire: parameter 2 is not of type 'Event'.");
	if (event->dispatching || !event->initialized)
		return bind_throw_dom(realm, "InvalidStateError", "The event is uninitialized or already being dispatched.");

	/* The browser's event is trusted. */
	event->trusted = 1;
	status = bind_dispatch(window, target, js_argument(args, count, 1), &canceled);
	if (status != 0)
		return status;

	/* Succeeded: true unless a listener canceled it. */
	*result = vm_value_boolean(!canceled);
	return 0;
}

/* setHyperlinkFollower(function): the function bind_follow_hyperlink calls with a clicked link. */
static int
prelude_set_follower(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	vm_value follower;

	UNUSED_PARAMETER(this_value);

	/* Only a function can follow links. */
	*result = VM_VALUE_UNDEFINED;
	follower = js_argument(args, count, 0);
	if (!vm_value_is_callable(follower))
		return vm_throw_type_error(realm, "setHyperlinkFollower: parameter 1 is not a function.");

	/* Succeeded: the window keeps it (its trace marks it). */
	window = bind_window_of(realm);
	window->follow_hyperlink = vm_value_as_cell(follower);
	return 0;
}

/* A script request under way: its number, the script's callback (a heap root) and the host's handle. */
struct bind_request_pending {
	struct bind_request_pending *next;
	struct bind_window *window;
	unsigned long id;
	struct vm_cell *callback;
	void *handle;
};

/* Reads a string argument as UTF-8 (undefined and null are NULL: *present is 0). */
static int
prelude_utf8_argument(
	const vm_value *args,
	unsigned count,
	unsigned index,
	struct wb_buffer *out,
	int *present)
{
	vm_value value;

	/* Absent values read as no string. */
	value = js_argument(args, count, index);
	*present = vm_value_is_string(value);
	if (!*present)
		return 0;
	return vm_string_to_utf8((const struct vm_string *)vm_value_as_cell(value), out);
}

/* Ends a pending request: out of the window's list, its root dropped, freed. */
static void
prelude_request_end(
	struct bind_request_pending *pending)
{
	struct bind_request_pending **link;

	/* Found by pointer in the window's list. */
	link = &pending->window->requests;
	while (*link != NULL && *link != pending)
		link = &(*link)->next;
	if (*link == pending)
		*link = pending->next;
	vm_heap_remove_root(pending->window->realm->heap, &pending->callback);
	free(pending);
}

/* A request ended: calls the script's callback with (error, status, url, headers, body). */
static void
prelude_request_done(
	void *context,
	int error,
	int status,
	const char *url,
	const char *headers,
	const unsigned char *bytes,
	size_t length)
{
	struct bind_request_pending *pending;
	struct bind_window *window;
	struct vm_realm *realm;
	vm_value arguments[5];
	vm_value callback;
	vm_value returned;
	int result;

	/* The record goes before the script runs, since the script may cancel others (not this one). */
	pending = context;
	window = pending->window;
	realm = window->realm;
	callback = vm_value_cell(pending->callback);
	arguments[0] = vm_value_int32(error);
	arguments[1] = vm_value_int32(status);
	arguments[2] = VM_VALUE_UNDEFINED;
	arguments[3] = VM_VALUE_UNDEFINED;
	arguments[4] = VM_VALUE_UNDEFINED;
	if (error == 0) {
		arguments[2] = vm_value_cell(vm_string_from_utf8(realm->heap, url == NULL ? "" : url, url == NULL ? 0 : strlen(url)));
		arguments[3] = vm_value_cell(vm_string_from_utf8(realm->heap, headers == NULL ? "" : headers, headers == NULL ? 0 : strlen(headers)));
		arguments[4] = vm_value_cell(vm_string_from_utf8(realm->heap, bytes == NULL ? "" : (const char *)bytes, bytes == NULL ? 0 : length));
	}
	result = vm_call(realm, callback, vm_value_cell(realm->global), arguments, 5, &returned);
	if (result == VM_THROWN) {
		bind_report_exception(window, realm->exception);
		realm->exception = VM_VALUE_UNDEFINED;
	} else if (result != 0 && getenv("BROWSER_STACK") != NULL) {
		fprintf(stderr, "request callback stopped by error %d (%s)\n", result, strerror(result));
	}
	prelude_request_end(pending);
}

/* send(method, url, headers, body, callback): starts a request; returns its number. */
static int
prelude_send(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_request request;
	struct bind_request_pending *pending;
	struct bind_window *window;
	struct wb_buffer method;
	struct wb_buffer href;
	struct wb_buffer headers;
	struct wb_buffer body;
	vm_value callback;
	void *handle;
	unsigned long id;
	int has_method;
	int has_href;
	int has_headers;
	int has_body;
	int status;

	UNUSED_PARAMETER(this_value);
	window = bind_window_of(realm);
	callback = js_argument(args, count, 4);
	if (!vm_value_is_callable(callback))
		return vm_throw_type_error(realm, "A request needs a callback.");
	wb_buffer_init(&method);
	wb_buffer_init(&href);
	wb_buffer_init(&headers);
	wb_buffer_init(&body);
	status = prelude_utf8_argument(args, count, 0, &method, &has_method);
	if (status == 0)
		status = prelude_utf8_argument(args, count, 1, &href, &has_href);
	if (status == 0)
		status = prelude_utf8_argument(args, count, 2, &headers, &has_headers);
	if (status == 0)
		status = prelude_utf8_argument(args, count, 3, &body, &has_body);
	pending = NULL;
	if (status == 0 && (!has_href || window->host.request == NULL))
		status = EINVAL;
	if (status == 0) {
		pending = calloc(1, sizeof(*pending));
		if (pending == NULL)
			status = ENOMEM;
	}
	if (status == 0) {
		pending->window = window;
		id = ++window->next_request;
		pending->id = id;
		pending->callback = vm_value_as_cell(callback);
		status = vm_heap_add_root(realm->heap, &pending->callback);
		if (status != 0)
			free(pending);
	}

	/* The window owns the record before the host can complete synchronously. */
	if (status == 0) {
		pending->next = window->requests;
		window->requests = pending;
		memset(&request, 0, sizeof(request));
		request.method = has_method ? wb_buffer_string(&method) : "GET";
		request.href = wb_buffer_string(&href);
		request.headers = has_headers ? wb_buffer_string(&headers) : NULL;
		request.body = has_body ? (const unsigned char *)(body.data == NULL ? "" : wb_buffer_string(&body)) : NULL;
		request.body_length = body.length;
		handle = NULL;
		status = window->host.request(window->host.context, &request, prelude_request_done, pending, &handle);

		/* A request that did not start tells the script it failed (it did not finish yet). */
		if (status != 0) {
			for (pending = window->requests; pending != NULL && pending->id != id; pending = pending->next)
				;
			if (pending != NULL)
				prelude_request_done(pending, status, 0, NULL, NULL, NULL, 0);
			status = 0;
		} else {
			/* Still pending: the host's handle is kept for a cancel. */
			for (pending = window->requests; pending != NULL && pending->id != id; pending = pending->next)
				;
			if (pending != NULL)
				pending->handle = handle;
		}
		*result = vm_value_int32((int32_t)id);
	}
	wb_buffer_release(&method);
	wb_buffer_release(&href);
	wb_buffer_release(&headers);
	wb_buffer_release(&body);
	return status;
}

/* cancel(id): ends a request whose callback has not been called (the callback is dropped). */
static int
prelude_cancel(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_request_pending *pending;
	struct bind_window *window;
	unsigned long id;

	UNUSED_PARAMETER(this_value);
	window = bind_window_of(realm);
	id = (unsigned long)vm_value_as_int32(js_argument(args, count, 0));
	*result = VM_VALUE_UNDEFINED;
	for (pending = window->requests; pending != NULL; pending = pending->next) {
		if (pending->id != id)
			continue;
		if (pending->handle != NULL && window->host.request_cancel != NULL)
			window->host.request_cancel(window->host.context, pending->handle);
		prelude_request_end(pending);
		break;
	}
	return 0;
}

/* syncGet(url): the final URL and text of a blocking GET as [url, text], or null. */
static int
prelude_sync_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_buffer href;
	struct wb_buffer bytes;
	struct wb_buffer final_url;
	vm_value parts[2];
	int present;
	int status;

	UNUSED_PARAMETER(this_value);
	window = bind_window_of(realm);
	*result = VM_VALUE_NULL;
	wb_buffer_init(&href);
	wb_buffer_init(&bytes);
	wb_buffer_init(&final_url);
	status = prelude_utf8_argument(args, count, 0, &href, &present);
	if (status == 0 && present && window->host.fetch_sync != NULL)
		status = window->host.fetch_sync(window->host.context, wb_buffer_string(&href), &bytes, &final_url);
	else if (status == 0)
		status = ENOSYS;
	if (getenv("BROWSER_NET_DUMP") != NULL)
		fprintf(stderr, "xhr: %.200s -> status %d bytes %lu\n", present ? wb_buffer_string(&href) : "", status, (unsigned long)bytes.length);
	if (status == 0) {
		parts[0] = vm_value_cell(vm_string_from_utf8(realm->heap, final_url.data == NULL ? "" : wb_buffer_string(&final_url), final_url.length));
		parts[1] = vm_value_cell(vm_string_from_utf8(realm->heap, bytes.data == NULL ? "" : (const char *)bytes.data, bytes.length));
		status = js_builtin_array(realm, parts, 2, result);
	} else if (status != ENOMEM) {
		status = 0;
	}
	wb_buffer_release(&href);
	wb_buffer_release(&bytes);
	wb_buffer_release(&final_url);
	return status;
}

/*
 * Follows a hyperlink a script clicked (an a or area element, or SVG's a).
 *
 * The prelude's function navigates to the link's URL; an exception it throws
 * is reported like a listener's.
 */
int
bind_follow_hyperlink(
	struct bind_window *window,
	struct dom_element *element)
{
	struct vm_realm *realm;
	vm_value argument;
	vm_value returned;
	int status;

	/* Nothing follows links before the prelude sets its function. */
	realm = window->realm;
	if (window->follow_hyperlink == NULL)
		return 0;

	/* The element's object. */
	status = bind_wrap(window, &element->node, &argument);
	if (status != 0)
		return status;

	/* The function navigates. */
	status = vm_call(realm, vm_value_cell(window->follow_hyperlink), VM_VALUE_UNDEFINED, &argument, 1, &returned);
	if (status == VM_THROWN) {
		bind_report_exception(window, realm->exception);
		realm->exception = VM_VALUE_UNDEFINED;
		return 0;
	}
	if (status != 0)
		return status;

	/* Succeeded: the link is followed. */
	return 0;
}

/* Forgets the window's pending script requests (the page cancelled their host requests). */
void
bind_requests_release(
	struct bind_window *window)
{
	struct bind_request_pending *next;
	struct bind_request_pending *pending;

	/* Each record's root and memory. */
	pending = window->requests;
	while (pending != NULL) {
		next = pending->next;
		vm_heap_remove_root(window->realm->heap, &pending->callback);
		free(pending);
		pending = next;
	}
	window->requests = NULL;
}

/* Runs a prelude source with its natives (the first argument) and the global object. */
static int
prelude_run(
	struct bind_window *window,
	const char *text,
	size_t size,
	vm_value first)
{
	struct vm_realm *realm;
	struct vm_string *source;
	vm_value function;
	vm_value arguments[2];
	vm_value ignored;
	int status;

	/* The source is a function of the natives and the global object. */
	realm = window->realm;
	source = vm_string_from_utf8(realm->heap, text, size);
	if (source == NULL)
		return ENOMEM;
	status = js_builtin_evaluate(realm, source, 0, &function);
	if (status != 0) {
		realm->exception = VM_VALUE_UNDEFINED;
		return 0;
	}
	arguments[0] = first;
	arguments[1] = vm_value_cell(realm->global);
	status = vm_call(realm, function, VM_VALUE_UNDEFINED, arguments, 2, &ignored);
	if (status != 0) {
		/* A failure of the prelude leaves the rest of the page working. */
		if (getenv("BROWSER_NET_DUMP") != NULL && status == VM_THROWN)
			bind_report_exception(window, realm->exception);
		realm->exception = VM_VALUE_UNDEFINED;
		return status == ENOMEM ? status : 0;
	}
	return 0;
}

/* Installs the prelude's objects on the window's global object. */
int
bind_install_prelude(
	struct bind_window *window)
{
	struct vm_realm *realm;
	struct vm_function *function;
	struct vm_object *natives;
	int status;

	/* The page's own objects over the URL parser, the document's URL and scrolling, trusted dispatch and links. */
	realm = window->realm;
	natives = vm_object_create(realm->heap, realm->object_prototype);
	if (natives == NULL)
		return ENOMEM;
	status = js_builtin_function(realm, "parseURL", 2, prelude_parse_url, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "parseURL", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "setURL", 1, prelude_set_url, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "setURL", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "scrollToFragment", 1, prelude_scroll_fragment, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "scrollToFragment", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "fire", 2, prelude_fire, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "fire", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "setHyperlinkFollower", 1, prelude_set_follower, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "setHyperlinkFollower", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status != 0)
		return status;
	status = prelude_run(window, prelude_source, sizeof(prelude_source) - 1U, vm_value_cell(natives));
	if (status != 0)
		return status;

	/* The network objects (fetch, XMLHttpRequest, Headers, ...) over three natives. */
	natives = vm_object_create(realm->heap, realm->object_prototype);
	if (natives == NULL)
		return ENOMEM;
	status = js_builtin_function(realm, "send", 5, prelude_send, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "send", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "cancel", 1, prelude_cancel, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "cancel", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "syncGet", 1, prelude_sync_get, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "syncGet", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status != 0)
		return status;
	return prelude_run(window, prelude_net_source, sizeof(prelude_net_source) - 1U, vm_value_cell(natives));
}
