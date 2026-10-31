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
static int prelude_navigate_frame(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_load_ended(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_fully_active(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_initial_blank(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_frame_element(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_scroll_fragment(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_fire(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_set_follower(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_node_rect(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_set_popover(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_view_transition(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_view_transition_active(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_view_transition_duration(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_view_transition_time(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_view_transition_names(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_view_transition_style(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_rule_info(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int prelude_rule_replace(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
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

/* navigateFrame(url): points the window's own frame at the URL; whether it is a frame (a top-level window is not). */
static int
prelude_navigate_frame(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_buffer location;
	int present;
	int started;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The URL as UTF-8. */
	window = bind_window_of(realm);
	*result = vm_value_boolean(0);
	wb_buffer_init(&location);
	status = prelude_utf8_argument(args, count, 0, &location, &present);
	if (status != 0 || !present) {
		wb_buffer_release(&location);
		return status;
	}

	/* The frame loads it at its next scan. */
	started = 0;
	status = bind_frame_navigate(window, wb_buffer_string(&location), location.length, &started);
	wb_buffer_release(&location);
	if (status != 0)
		return status;

	/* Succeeded: whether the frame was pointed at the URL. */
	*result = vm_value_boolean(started);
	return 0;
}

/* loadEnded(): whether the window's load event has finished (a navigation before that replaces the current entry). */
static int
prelude_load_ended(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The window's flag. */
	window = bind_window_of(realm);
	*result = vm_value_boolean(window->load_ended);
	return 0;
}

/* fullyActive(): whether the window's document is the active one of a connected frame or of the top-level view (not a removed frame's). */
static int
prelude_fully_active(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A window that was detached with its removed frame is not. */
	window = bind_window_of(realm);
	*result = vm_value_boolean(!window->detached && !window->unloading);
	return 0;
}

/* initialBlank(): whether the window is a frame still on its initial about:blank document (no resource was loaded into it). */
static int
prelude_initial_blank(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int blank;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A top-level window has its page's resource; a frame without a loaded resource is blank. */
	window = bind_window_of(realm);
	blank = window->parent_global != NULL && window->document->resource_url == NULL;
	*result = vm_value_boolean(blank);
	return 0;
}

/* frameElement(): the frame element this window is in, or null; unlike the window's own property it works while the window is being made. */
static int
prelude_frame_element(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A top-level or retired window has none. */
	window = bind_window_of(realm);
	*result = VM_VALUE_NULL;
	if (window->frame == NULL)
		return 0;

	/* The element's wrapper. */
	status = bind_wrap(window, &window->frame->node, result);
	if (status != 0)
		return status;

	/* Succeeded: the frame element. */
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

/* viewTransition(step): 1 captures the old state, 2 makes the new one and shows the tree, 0 ends the transition; true when it could be made. */
static int
prelude_view_transition(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	double step;
	int status;

	UNUSED_PARAMETER(this_value);
	window = bind_window_of(realm);
	*result = VM_VALUE_FALSE;
	status = vm_to_number(realm, js_argument(args, count, 0), &step);
	if (status != 0)
		return status;
	if (window->host.view_transition == NULL)
		return 0;
	if (window->host.view_transition(window->host.context, (int)step) != 0)
		*result = VM_VALUE_TRUE;
	return 0;
}

/* viewTransitionActive(on, types): tells the page whether a view transition is active and the strings of its types. */
static int
prelude_view_transition_active(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_string *types[8];
	struct vm_object *list;
	struct vm_string *string;
	vm_value item;
	vm_value length_value;
	double length;
	double on;
	size_t used;
	size_t index;
	int status;

	UNUSED_PARAMETER(this_value);
	window = bind_window_of(realm);
	*result = VM_VALUE_UNDEFINED;
	status = vm_to_number(realm, js_argument(args, count, 0), &on);
	if (status != 0)
		return status;
	used = 0;
	if (vm_value_is_object(js_argument(args, count, 1))) {
		list = (struct vm_object *)vm_value_as_cell(js_argument(args, count, 1));
		status = vm_get(realm, vm_value_cell(list), vm_value_cell(vm_atom_from_ascii(realm->heap, "length")), &length_value);
		if (status != 0)
			return status;
		status = vm_to_number(realm, length_value, &length);
		if (status != 0)
			return status;
		for (index = 0; index < (size_t)length && used < 8U; index++) {
			status = vm_get(realm, vm_value_cell(list), vm_value_int32((int32_t)index), &item);
			if (status != 0)
				return status;
			status = vm_to_string(realm, item, &string);
			if (status != 0)
				return status;
			string = vm_atom(realm->heap, string);
			if (string != NULL)
				types[used++] = string;
		}
	}
	if (window->host.view_transition_active != NULL)
		window->host.view_transition_active(window->host.context, on != 0.0, types, used);
	return 0;
}

/* viewTransitionDuration(): the milliseconds until the shown transition's animations have all ended. */
static int
prelude_view_transition_duration(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	window = bind_window_of(realm);
	*result = vm_value_number(0.0);
	if (window->host.view_transition_duration != NULL)
		*result = vm_value_number(window->host.view_transition_duration(window->host.context));
	return 0;
}

/* viewTransitionTime(ms): sets how long the shown transition's animations have run. */
static int
prelude_view_transition_time(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	double milliseconds;
	int status;

	UNUSED_PARAMETER(this_value);
	*result = VM_VALUE_UNDEFINED;
	if (count < 1U)
		return 0;
	window = bind_window_of(realm);
	status = vm_to_number(realm, args[0], &milliseconds);
	if (status != 0)
		return status;
	if (window->host.view_transition_time != NULL)
		window->host.view_transition_time(window->host.context, milliseconds);
	return 0;
}

/* viewTransitionNames(): the shown transition's names, "name flags" per line. */
static int
prelude_view_transition_names(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_units units;
	struct vm_string *string;
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	window = bind_window_of(realm);
	wb_units_init(&units);
	status = 0;
	if (window->host.view_transition_names != NULL)
		status = window->host.view_transition_names(window->host.context, &units);
	if (status == 0) {
		string = vm_string_from_units(realm->heap, units.data, units.length);
		if (string == NULL)
			status = ENOMEM;
		else
			*result = vm_value_cell(&string->cell);
	}
	wb_units_release(&units);
	return status;
}

/* viewTransitionStyle(pseudo, name, declarations): what a script's animation gives a view transition pseudo-element now. */
static int
prelude_view_transition_style(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_string *name;
	struct wb_buffer text;
	uint32_t pseudo;
	int present;
	int status;

	UNUSED_PARAMETER(this_value);
	*result = VM_VALUE_UNDEFINED;
	if (count < 3U)
		return 0;
	window = bind_window_of(realm);
	if (window->host.view_transition_style == NULL)
		return 0;
	status = vm_to_uint32(realm, args[0], &pseudo);
	if (status != 0)
		return status;
	status = vm_to_string(realm, args[1], &name);
	if (status != 0)
		return status;
	name = vm_atom(realm->heap, name);
	if (name == NULL)
		return ENOMEM;
	wb_buffer_init(&text);
	status = prelude_utf8_argument(args, count, 2U, &text, &present);
	if (status == 0)
		window->host.view_transition_style(window->host.context, (int)pseudo, name, wb_buffer_string(&text));
	wb_buffer_release(&text);
	return status;
}

/* ruleInfo(sheet, index): the rule of a sheet as "id type source", or undefined. */
static int
prelude_rule_info(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(this_value);
	*result = VM_VALUE_UNDEFINED;
	if (count < 2U)
		return 0;
	return bind_cssom_rule_info(realm, args[0], args + 1, count - 1U, result);
}

/* ruleReplace(sheet, index, text): replaces a rule of a sheet and gives its new identity. */
static int
prelude_rule_replace(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(this_value);
	*result = VM_VALUE_UNDEFINED;
	if (count < 3U)
		return 0;
	return bind_cssom_rule_replace(realm, args[0], args + 1, count - 1U, result);
}

/* setPopover(element, open): whether the popover of an element is showing (the page is styled again). */
static int
prelude_set_popover(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_element *element;
	int open;

	UNUSED_PARAMETER(this_value);
	*result = VM_VALUE_UNDEFINED;
	node = bind_node_of(js_argument(args, count, 0));
	if (node == NULL || node->type != DOM_ELEMENT)
		return 0;
	element = (struct dom_element *)node;
	open = js_argument(args, count, 1) == VM_VALUE_TRUE;
	if ((element->popover_open != 0) != (open != 0)) {
		/* What opens later is above (its number is its place in the top layer). */
		static int top_layer_order;

		top_layer_order++;
		element->popover_open = open ? top_layer_order : 0;
		element->node.document->generation++;
	}
	UNUSED_PARAMETER(realm);
	return 0;
}

/* nodeRect(node): where a node's boxes are in the viewport as [x, y, width, height], or null without a box. */
static int
prelude_node_rect(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_node *node;
	struct vm_object *array;
	struct bind_box box;
	double scroll_x;
	double scroll_y;
	double part[4];
	int index;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Only a node of this window's document laid out by the host has a place. */
	window = bind_window_of(realm);
	*result = VM_VALUE_NULL;
	node = bind_node_of(js_argument(args, count, 0));
	if (node == NULL || window->host.node_box == NULL)
		return 0;
	memset(&box, 0, sizeof(box));
	if (!window->host.node_box(window->host.context, node, &box))
		return 0;

	/* The page's place less the scroll. */
	scroll_x = 0.0;
	scroll_y = 0.0;
	if (window->host.scroll != NULL)
		window->host.scroll(window->host.context, &scroll_x, &scroll_y);
	part[0] = box.x - scroll_x;
	part[1] = box.y - scroll_y;
	part[2] = box.width;
	part[3] = box.height;

	/* Succeeded: the four numbers. */
	status = bind_array_create(realm, &array);
	if (status != 0)
		return status;
	for (index = 0; index < 4; index++) {
		status = vm_object_define(realm->heap, array, vm_value_int32(index), vm_value_number(part[index]), VM_PROPERTY_DEFAULT);
		if (status != 0)
			return status;
	}
	*result = vm_value_cell(array);
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

	/* The page's own objects over the URL parser, the document's URL and scrolling, trusted dispatch, links and node places. */
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
		status = js_builtin_function(realm, "navigateFrame", 1, prelude_navigate_frame, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "navigateFrame", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "loadEnded", 0, prelude_load_ended, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "loadEnded", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "fullyActive", 0, prelude_fully_active, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "fullyActive", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "initialBlank", 0, prelude_initial_blank, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "initialBlank", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "frameElement", 0, prelude_frame_element, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "frameElement", vm_value_cell(function), JS_BUILTIN_METHOD);
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
	if (status == 0)
		status = js_builtin_function(realm, "nodeRect", 1, prelude_node_rect, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "nodeRect", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "setPopover", 2, prelude_set_popover, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "setPopover", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "viewTransitionTime", 1, prelude_view_transition_time, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "viewTransitionTime", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "viewTransitionNames", 0, prelude_view_transition_names, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "viewTransitionNames", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "viewTransitionStyle", 3, prelude_view_transition_style, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "viewTransitionStyle", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "ruleInfo", 2, prelude_rule_info, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "ruleInfo", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "ruleReplace", 3, prelude_rule_replace, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "ruleReplace", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "viewTransition", 2, prelude_view_transition, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "viewTransition", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "viewTransitionActive", 2, prelude_view_transition_active, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "viewTransitionActive", vm_value_cell(function), JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_function(realm, "viewTransitionDuration", 1, prelude_view_transition_duration, NULL, &function);
	if (status == 0)
		status = js_builtin_value(realm, natives, "viewTransitionDuration", vm_value_cell(function), JS_BUILTIN_METHOD);
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
