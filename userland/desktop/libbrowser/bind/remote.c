/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Windows of other agents: the windows of a frame whose document is of
 * another origin than its parent's, which runs in a heap of its own on a
 * thread of its own (the embedder's, struct bind_remote_calls).
 *
 * A window of another agent is seen through a proxy of the caller's heap
 * (HTML's WindowProxy of a cross-origin window): postMessage, closed,
 * length, window, self, frames, parent, top, opener, location (whose href
 * may be set and which may be replaced), focus, blur and close; any other
 * access throws a SecurityError.  An iframe's proxy is kept by its element;
 * the top-level window of such a frame keeps those of its parent and top.
 *
 * A message is written out of the sender's heap as a tape of plain items
 * (the structured clone's serialization: primitives, Arrays, plain
 * objects, Map, Set, Date, RegExp, the wrappers of primitives, Errors,
 * ArrayBuffers and their views, references to an object met before), an
 * ArrayBuffer of the transfer list is copied and its sender's detached,
 * and anything else (a MessagePort among them) is a DataCloneError.  The
 * receiver's heap reads the tape back as a task of its own thread.
 *
 * The proxies and the tape's writer and reader are written in the page's
 * own language (remote_source), run in a window the first time it needs
 * them.
 */

#include "bind/internal.h"
#include "js/builtin.h"
#include "net/net.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most items one message's tape may have. */
#define REMOTE_TAPE_MAX		(1U << 22)

static int remote_ready(struct bind_window *window);
static int remote_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int remote_post(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int remote_navigate(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int remote_info(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int remote_detach(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int remote_detached(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int remote_make(struct bind_window *window, int kind, uint64_t frame, vm_value owner, vm_value *result);
static int remote_tape_write(struct vm_realm *realm, vm_value array, struct bind_tape *tape);
static int remote_tape_read(struct vm_realm *realm, const struct bind_tape *tape, vm_value *array);
static int remote_origin(struct bind_window *window, struct wb_buffer *origin);
static int remote_utf8(struct vm_realm *realm, vm_value value, struct wb_buffer *out);

/*
 * The remote prelude: a function of its natives and the global object,
 * which gives setRemote the proxy maker, the tape's writer and its reader.
 */
static const char remote_source[] =
	"(function (native, g) {\n"
	"  'use strict';\n"
	"  function blocked() { return new DOMException('Blocked a frame from accessing a cross-origin frame.', 'SecurityError'); }\n"
	"  function fail(what) { return new DOMException(what + ' could not be cloned.', 'DataCloneError'); }\n"
	"  var CHILD = 0;\n"
	"  function write(value, transfer) {\n"
	"    var tape = [], seen = new Map(), next = 0, list = [], i, t;\n"
	"    if (transfer !== undefined && transfer !== null) {\n"
	"      if (typeof transfer !== 'object' || typeof transfer[Symbol.iterator] !== 'function')\n"
	"        throw new TypeError(\"Failed to execute 'postMessage' on 'Window': The provided value cannot be converted to a sequence.\");\n"
	"      list = Array.from(transfer);\n"
	"    }\n"
	"    for (i = 0; i < list.length; i++) {\n"
	"      t = list[i];\n"
	"      if (!(t instanceof g.ArrayBuffer)) throw new DOMException('Value at index ' + i + ' does not have a transferable type.', 'DataCloneError');\n"
	"      if (list.indexOf(t) !== i) throw new DOMException('ArrayBuffer at index ' + i + ' is a duplicate of an earlier ArrayBuffer.', 'DataCloneError');\n"
	"      if (native.detached(t)) throw new DOMException('ArrayBuffer at index ' + i + ' is already detached.', 'DataCloneError');\n"
	"    }\n"
	"    function bytes(b) {\n"
	"      var a = new Uint8Array(b), s = '', i, n = 4096;\n"
	"      for (i = 0; i < a.length; i += n) s += String.fromCharCode.apply(null, Array.prototype.slice.call(a, i, i + n));\n"
	"      return s;\n"
	"    }\n"
	"    function put(x) {\n"
	"      var tag, keys, i;\n"
	"      if (x === undefined) { tape.push('u'); return; }\n"
	"      if (x === null) { tape.push('n'); return; }\n"
	"      switch (typeof x) {\n"
	"      case 'boolean': tape.push(x ? 't' : 'f'); return;\n"
	"      case 'number': tape.push('d', x); return;\n"
	"      case 'string': tape.push('s', x); return;\n"
	"      case 'bigint': tape.push('i', String(x)); return;\n"
	"      case 'function': throw fail(String(x));\n"
	"      case 'symbol': throw fail('Symbol()');\n"
	"      }\n"
	"      if (seen.has(x)) { tape.push('r', seen.get(x)); return; }\n"
	"      tag = Object.prototype.toString.call(x);\n"
	"      if ((typeof x.addEventListener === 'function' && typeof x.dispatchEvent === 'function') || tag === '[object Window]' ||\n"
	"          (typeof x.preventDefault === 'function' && typeof x.stopPropagation === 'function'))\n"
	"        throw fail('#<' + (tag.slice(8, -1) || 'Object') + '>');\n"
	"      seen.set(x, next++);\n"
	"      if (Array.isArray(x)) {\n"
	"        tape.push('a', x.length);\n"
	"        for (i = 0; i < x.length; i++) { if (i in x) put(x[i]); else tape.push('h'); }\n"
	"        return;\n"
	"      }\n"
	"      if (tag === '[object Date]') { tape.push('D', x.getTime()); return; }\n"
	"      if (tag === '[object RegExp]') { tape.push('R', x.source, x.flags); return; }\n"
	"      if (tag === '[object Boolean]') { tape.push('B', x.valueOf()); return; }\n"
	"      if (tag === '[object Number]') { tape.push('N', x.valueOf()); return; }\n"
	"      if (tag === '[object String]') { tape.push('S', x.valueOf()); return; }\n"
	"      if (x instanceof g.ArrayBuffer) { tape.push('X', bytes(x)); return; }\n"
	"      if (g.ArrayBuffer.isView(x)) {\n"
	"        if (x instanceof g.DataView) { tape.push('W'); put(x.buffer); tape.push(x.byteOffset, x.byteLength); return; }\n"
	"        tape.push('V', x.constructor.name); put(x.buffer); tape.push(x.byteOffset, x.length); return;\n"
	"      }\n"
	"      if (x instanceof g.Map) {\n"
	"        keys = []; x.forEach(function (v, k) { keys.push(k, v); });\n"
	"        tape.push('M', keys.length / 2);\n"
	"        for (i = 0; i < keys.length; i++) put(keys[i]);\n"
	"        return;\n"
	"      }\n"
	"      if (x instanceof g.Set) {\n"
	"        keys = []; x.forEach(function (v) { keys.push(v); });\n"
	"        tape.push('T', keys.length);\n"
	"        for (i = 0; i < keys.length; i++) put(keys[i]);\n"
	"        return;\n"
	"      }\n"
	"      if (x instanceof g.Error) { tape.push('E', String(x.name), String(x.message)); return; }\n"
	"      if (tag !== '[object Object]') throw fail('#<' + tag.slice(8, -1) + '>');\n"
	"      keys = Object.keys(x);\n"
	"      tape.push('o', keys.length);\n"
	"      for (i = 0; i < keys.length; i++) { tape.push(keys[i]); put(x[keys[i]]); }\n"
	"    }\n"
	"    put(value);\n"
	"    for (i = 0; i < list.length; i++) native.detach(list[i]);\n"
	"    return tape;\n"
	"  }\n"
	"  var HOLE = {};\n"
	"  var ERRORS = { Error: g.Error, EvalError: g.EvalError, RangeError: g.RangeError, ReferenceError: g.ReferenceError,\n"
	"    SyntaxError: g.SyntaxError, TypeError: g.TypeError, URIError: g.URIError };\n"
	"  var VIEWS = ['Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array',\n"
	"    'Float32Array', 'Float64Array', 'BigInt64Array', 'BigUint64Array'];\n"
	"  function read(tape) {\n"
	"    var at = 0, refs = [];\n"
	"    function own(o, k, v) { Object.defineProperty(o, k, { value: v, writable: true, enumerable: true, configurable: true }); }\n"
	"    function get() {\n"
	"      var tag = tape[at++], r, n, i, id, k, v, s, b, u, name, off;\n"
	"      switch (tag) {\n"
	"      case 'u': return undefined;\n"
	"      case 'n': return null;\n"
	"      case 't': return true;\n"
	"      case 'f': return false;\n"
	"      case 'd': return tape[at++];\n"
	"      case 's': return tape[at++];\n"
	"      case 'i': return g.BigInt(tape[at++]);\n"
	"      case 'r': return refs[tape[at++]];\n"
	"      case 'h': return HOLE;\n"
	"      case 'a':\n"
	"        n = tape[at++]; r = new Array(n); refs.push(r);\n"
	"        for (i = 0; i < n; i++) { v = get(); if (v !== HOLE) r[i] = v; }\n"
	"        return r;\n"
	"      case 'D': r = new g.Date(tape[at++]); refs.push(r); return r;\n"
	"      case 'R': r = new g.RegExp(tape[at], tape[at + 1]); at += 2; refs.push(r); return r;\n"
	"      case 'B': r = new g.Boolean(tape[at++]); refs.push(r); return r;\n"
	"      case 'N': r = new g.Number(tape[at++]); refs.push(r); return r;\n"
	"      case 'S': r = new g.String(tape[at++]); refs.push(r); return r;\n"
	"      case 'X':\n"
	"        s = tape[at++]; b = new g.ArrayBuffer(s.length); u = new g.Uint8Array(b);\n"
	"        for (i = 0; i < s.length; i++) u[i] = s.charCodeAt(i);\n"
	"        refs.push(b);\n"
	"        return b;\n"
	"      case 'V': case 'W':\n"
	"        name = tag === 'W' ? 'DataView' : tape[at++];\n"
	"        if (name !== 'DataView' && VIEWS.indexOf(name) < 0) throw fail(name);\n"
	"        id = refs.length; refs.push(null);\n"
	"        b = get(); off = tape[at++]; n = tape[at++];\n"
	"        r = new g[name](b, off, n); refs[id] = r;\n"
	"        return r;\n"
	"      case 'M':\n"
	"        n = tape[at++]; r = new g.Map(); refs.push(r);\n"
	"        for (i = 0; i < n; i++) { k = get(); v = get(); r.set(k, v); }\n"
	"        return r;\n"
	"      case 'T':\n"
	"        n = tape[at++]; r = new g.Set(); refs.push(r);\n"
	"        for (i = 0; i < n; i++) r.add(get());\n"
	"        return r;\n"
	"      case 'E':\n"
	"        name = tape[at++]; s = tape[at++];\n"
	"        r = new (Object.prototype.hasOwnProperty.call(ERRORS, name) ? ERRORS[name] : g.Error)(s); refs.push(r);\n"
	"        return r;\n"
	"      case 'o':\n"
	"        n = tape[at++]; r = {}; refs.push(r);\n"
	"        for (i = 0; i < n; i++) { k = tape[at++]; own(r, k, get()); }\n"
	"        return r;\n"
	"      }\n"
	"      throw fail('A message');\n"
	"    }\n"
	"    return get();\n"
	"  }\n"
	"  function make(kind, frame, owner) {\n"
	"    var proxy, location, post, replace, noop;\n"
	"    function closed() { return native.info(kind, frame, 0) !== 0; }\n"
	"    function go(url, replacing) { native.navigate(kind, frame, String(url), replacing); }\n"
	"    function relative(k) {\n"
	"      if (kind === CHILD) return k === 'top' ? owner.top : owner;\n"
	"      return g.top;\n"
	"    }\n"
	"    post = { postMessage(message, options, transfer) {\n"
	"      var target = '/', list;\n"
	"      if (arguments.length === 0) throw new TypeError(\"Failed to execute 'postMessage' on 'Window': 1 argument required, but only 0 present.\");\n"
	"      if (options !== null && typeof options === 'object' && arguments.length < 3) {\n"
	"        if (options.targetOrigin !== undefined) target = String(options.targetOrigin);\n"
	"        list = options.transfer;\n"
	"      } else {\n"
	"        if (options !== undefined) target = String(options);\n"
	"        list = transfer;\n"
	"      }\n"
	"      if (target !== '*' && target !== '/') {\n"
	"        try { target = new URL(target).origin; } catch (e) {\n"
	"          throw new DOMException(\"Failed to execute 'postMessage' on 'Window': Invalid target origin '\" + target + \"' in a call to 'postMessage'.\", 'SyntaxError');\n"
	"        }\n"
	"      }\n"
	"      native.post(kind, frame, write(message, list), target);\n"
	"    } }.postMessage;\n"
	"    replace = { replace(url) { go(url, true); } }.replace;\n"
	"    noop = { focus() {} }.focus;\n"
	"    location = new Proxy(Object.create(null), {\n"
	"      get: function (t, k) {\n"
	"        if (k === 'replace') return replace;\n"
	"        if (k === 'then' || k === Symbol.toPrimitive) return undefined;\n"
	"        if (k === Symbol.toStringTag) return 'Location';\n"
	"        throw blocked();\n"
	"      },\n"
	"      set: function (t, k, v) { if (k === 'href') { go(v, false); return true; } throw blocked(); },\n"
	"      has: function () { throw blocked(); },\n"
	"      ownKeys: function () { return []; },\n"
	"      getOwnPropertyDescriptor: function () { throw blocked(); },\n"
	"      defineProperty: function () { throw blocked(); },\n"
	"      deleteProperty: function () { throw blocked(); },\n"
	"      getPrototypeOf: function () { return null; },\n"
	"      setPrototypeOf: function () { return false; },\n"
	"      preventExtensions: function () { return false; },\n"
	"      isExtensible: function () { return true; }\n"
	"    });\n"
	"    proxy = new Proxy(Object.create(null), {\n"
	"      get: function (t, k) {\n"
	"        if (k === 'window' || k === 'self' || k === 'frames') return proxy;\n"
	"        if (k === 'postMessage') return post;\n"
	"        if (k === 'closed') return closed();\n"
	"        if (k === 'length') return native.info(kind, frame, 1);\n"
	"        if (k === 'location') return location;\n"
	"        if (k === 'parent' || k === 'top') return relative(k);\n"
	"        if (k === 'opener') return null;\n"
	"        if (k === 'focus' || k === 'blur' || k === 'close') return noop;\n"
	"        if (k === 'then' || k === Symbol.toPrimitive) return undefined;\n"
	"        if (k === Symbol.toStringTag) return 'Window';\n"
	"        throw blocked();\n"
	"      },\n"
	"      set: function (t, k, v) { if (k === 'location') { go(v, false); return true; } throw blocked(); },\n"
	"      has: function (t, k) { return k === 'postMessage' || k === 'closed' || k === 'length' || k === 'location' || k === 'window' ||\n"
	"        k === 'self' || k === 'frames' || k === 'parent' || k === 'top' || k === 'opener' || k === 'focus' || k === 'blur' || k === 'close'; },\n"
	"      ownKeys: function () { return []; },\n"
	"      getOwnPropertyDescriptor: function () { throw blocked(); },\n"
	"      defineProperty: function () { throw blocked(); },\n"
	"      deleteProperty: function () { throw blocked(); },\n"
	"      getPrototypeOf: function () { return null; },\n"
	"      setPrototypeOf: function () { return false; },\n"
	"      preventExtensions: function () { return false; },\n"
	"      isExtensible: function () { return true; }\n"
	"    });\n"
	"    return proxy;\n"
	"  }\n"
	"  native.setRemote(make, write, read);\n"
	"})";

/* Starts an empty tape. */
void
bind_tape_init(
	struct bind_tape *tape)
{
	/* No items. */
	wb_vector_init(&tape->items, sizeof(struct bind_tape_item));
}

/* Lets go of a tape's items and their strings. */
void
bind_tape_release(
	struct bind_tape *tape)
{
	struct bind_tape_item *item;
	size_t index;

	/* Each string, then the list. */
	for (index = 0; index < tape->items.count; index++) {
		item = wb_vector_at(&tape->items, index);
		free(item->units);
	}
	wb_vector_release(&tape->items);
}

/*
 * Makes a window reach windows of other agents through the embedder's
 * calls; framed says it is the top-level window of a frame of another agent
 * (its parent and its top are of other agents; parent_is_top says they are
 * the same window).
 */
void
bind_window_set_remote(
	struct bind_window *window,
	const struct bind_remote_calls *calls,
	void *context,
	int framed,
	int parent_is_top)
{
	/* The calls, and what the window's parent and top are. */
	window->host.remote = calls;
	window->host.remote_context = context;
	window->remote_framed = framed;
	window->remote_parent_is_top = parent_is_top;
}

/* Marks what the remote prelude left in a window. */
void
bind_remote_trace(
	struct vm_heap *heap,
	struct bind_window *window)
{
	/* Its functions and the proxies it made. */
	if (window->remote_maker != NULL)
		vm_heap_mark(heap, window->remote_maker);
	if (window->remote_write != NULL)
		vm_heap_mark(heap, window->remote_write);
	if (window->remote_read != NULL)
		vm_heap_mark(heap, window->remote_read);
	if (window->remote_parent != NULL)
		vm_heap_mark(heap, window->remote_parent);
	if (window->remote_top != NULL)
		vm_heap_mark(heap, window->remote_top);
}

/*
 * Gives the proxy of the frame of another agent an iframe element of the
 * viewer's agent shows: the one the element keeps, made the first time by
 * the element's document's window.  Reports 0 (null when the element shows
 * no such frame), or why not.
 */
int
bind_remote_child(
	struct bind_window *viewer,
	struct dom_element *element,
	vm_value *result)
{
	struct bind_window *owner;
	uint64_t frame;
	int status;

	UNUSED_PARAMETER(viewer);

	/* The element's frame, by its number. */
	*result = VM_VALUE_NULL;
	owner = element->node.document->view;
	if (owner == NULL || owner->detached || owner->host.remote == NULL || element->frame_remote == NULL)
		return 0;
	if (element->frame_proxy != NULL) {
		*result = vm_value_cell(element->frame_proxy);
		return 0;
	}
	frame = owner->host.remote->frame(owner->host.remote_context, element);
	if (frame == 0)
		return 0;

	/* Made once, kept by the element until it leaves its document or shows another document. */
	status = remote_make(owner, BIND_REMOTE_CHILD, frame, vm_value_cell(owner->realm->global), result);
	if (status != 0)
		return status;
	if (vm_value_is_object(*result))
		element->frame_proxy = vm_value_as_cell(*result);

	/* Succeeded: the proxy. */
	return 0;
}

/*
 * Gives the parent's (BIND_REMOTE_PARENT) or the top's (BIND_REMOTE_TOP)
 * proxy of the top-level window of a frame of another agent, made once and
 * kept by the window (the same one when its parent is its top).
 */
int
bind_remote_relative(
	struct bind_window *window,
	int kind,
	vm_value *result)
{
	struct vm_cell **slot;
	int status;

	/* The parent's, or the top's (the parent's when they are one). */
	if (kind == BIND_REMOTE_TOP && window->remote_parent_is_top)
		kind = BIND_REMOTE_PARENT;
	slot = kind == BIND_REMOTE_PARENT ? &window->remote_parent : &window->remote_top;
	if (*slot != NULL) {
		*result = vm_value_cell(*slot);
		return 0;
	}

	/* Made once. */
	status = remote_make(window, kind, 0, VM_VALUE_UNDEFINED, result);
	if (status != 0)
		return status;
	if (vm_value_is_object(*result))
		*slot = vm_value_as_cell(*result);

	/* Succeeded: the proxy. */
	return 0;
}

/*
 * Delivers a message from a window of another agent to a window (a task
 * of the window's thread): unless the window's origin is not the one the
 * message may go to, its value is read from the tape into the window's
 * heap and a trusted MessageEvent with the sender's origin and its proxy
 * as the source is dispatched at the window.  The source is the proxy of
 * the sender as the window sees it: its parent's or its top's
 * (source_kind), or the frame an iframe element of its agent shows
 * (source_element).  Reports 0 (a message that may not go there, or whose
 * value could not be read, is dropped), or a fatal error.
 */
int
bind_remote_deliver(
	struct bind_window *window,
	const struct bind_tape *tape,
	const char *origin,
	const char *target_origin,
	int source_kind,
	struct dom_element *source_element)
{
	struct vm_realm *realm;
	struct bind_event *event;
	struct vm_string *type;
	struct vm_cell *roots[3];
	struct wb_buffer own;
	vm_value array;
	vm_value value;
	vm_value event_value;
	vm_value source;
	unsigned registered;
	unsigned index;
	int canceled;
	int status;

	/* A window gone, or of another origin than the one the message may go to. */
	if (window->detached)
		return 0;
	realm = window->realm;
	if (strcmp(target_origin, "*") != 0) {
		wb_buffer_init(&own);
		status = remote_origin(window, &own);
		if (status == 0 && strcmp(target_origin, "/") == 0)
			target_origin = origin;
		if (status != 0 || strcmp(wb_buffer_string(&own), target_origin) != 0) {
			wb_buffer_release(&own);
			return status == ENOMEM ? ENOMEM : 0;
		}
		wb_buffer_release(&own);
	}
	status = remote_ready(window);
	if (status != 0)
		return status;

	/* The temporary roots: the tape's array, the value and the event. */
	roots[0] = NULL;
	roots[1] = NULL;
	roots[2] = NULL;
	registered = 0;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto done;
		registered++;
	}

	/* The value, read in the window's heap (one it cannot read is dropped, the console told). */
	status = remote_tape_read(realm, tape, &array);
	if (status != 0)
		goto done;
	roots[0] = vm_value_as_cell(array);
	status = vm_call(realm, vm_value_cell(window->remote_read), VM_VALUE_UNDEFINED, &array, 1, &value);
	if (status == VM_THROWN) {
		realm->exception = VM_VALUE_UNDEFINED;
		bind_console(window, BIND_CONSOLE_ERROR, "A message from a frame of another origin could not be read");
		status = 0;
		goto done;
	}
	if (status != 0)
		goto done;
	if (vm_value_is_cell(value))
		roots[1] = vm_value_as_cell(value);

	/* The source as the window sees it. */
	if (source_element != NULL)
		status = bind_remote_child(window, source_element, &source);
	else
		status = bind_remote_relative(window, source_kind, &source);
	if (status != 0)
		goto done;

	/* The event: its data, its origin, its source, no ports. */
	type = vm_atom_from_ascii(realm->heap, "message");
	if (type == NULL) {
		status = ENOMEM;
		goto done;
	}
	status = bind_event_create(window, BIND_MESSAGE_EVENT, type, &event_value, &event);
	if (status != 0)
		goto done;
	roots[2] = vm_value_as_cell(event_value);
	event->detail = value;
	event->source = source;
	event->trusted = 1;
	event->origin = vm_string_from_utf8(realm->heap, origin, strlen(origin));
	if (event->origin == NULL) {
		status = ENOMEM;
		goto done;
	}
	status = js_builtin_array(realm, NULL, 0, &event->ports);
	if (status != 0)
		goto done;

	/* Dispatched now: this is the task. */
	status = bind_dispatch(window, vm_value_cell(realm->global), event_value, &canceled);
	if (status == VM_THROWN) {
		realm->exception = VM_VALUE_UNDEFINED;
		status = 0;
	}

done:
	/* The roots go on either outcome. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}
	return status;
}

/*
 * Points a window at a URL for a window of another agent (its location
 * set through a proxy): a top-level window asks the program, as its own
 * script would; a frame's is loaded into its iframe at the next scan.
 */
int
bind_window_request_navigation(
	struct bind_window *window,
	const char *url,
	int replace)
{
	char *copy;
	int started;

	/* A window gone navigates nowhere. */
	if (window->detached)
		return 0;

	/* A frame's. */
	if (window->frame != NULL)
		return bind_frame_navigate(window, url, strlen(url), &started);

	/* A top-level window's, which the program takes (bind_take_navigation). */
	copy = strdup(url);
	if (copy == NULL)
		return ENOMEM;
	free(window->top_navigation);
	window->top_navigation = copy;
	window->top_navigation_replace = replace;
	return 0;
}

/* Runs the remote prelude in a window the first time it needs it (its natives are this file's). */
static int
remote_ready(
	struct bind_window *window)
{
	static const struct {
		const char *name;
		unsigned length;
		vm_native native;
	} natives[] = {
		{ "setRemote", 3, remote_set },
		{ "post", 4, remote_post },
		{ "navigate", 4, remote_navigate },
		{ "info", 3, remote_info },
		{ "detach", 1, remote_detach },
		{ "detached", 1, remote_detached },
	};
	struct vm_realm *realm;
	struct vm_object *object;
	struct vm_function *function;
	struct vm_string *source;
	vm_value maker;
	vm_value args[2];
	vm_value ignored;
	size_t index;
	int status;

	/* Once. */
	if (window->remote_maker != NULL)
		return 0;
	realm = window->realm;

	/* The natives. */
	object = vm_object_create(realm->heap, realm->object_prototype);
	if (object == NULL)
		return ENOMEM;
	args[0] = vm_value_cell(object);
	args[1] = vm_value_cell(realm->global);
	for (index = 0; index < sizeof(natives) / sizeof(natives[0]); index++) {
		status = js_builtin_function(realm, natives[index].name, natives[index].length, natives[index].native, NULL, &function);
		if (status == 0)
			status = js_builtin_value(realm, object, natives[index].name, vm_value_cell(function), JS_BUILTIN_METHOD);
		if (status != 0)
			return status;
	}

	/* The source, run with them. */
	source = vm_string_from_utf8(realm->heap, remote_source, sizeof(remote_source) - 1U);
	if (source == NULL)
		return ENOMEM;
	status = js_builtin_evaluate(realm, source, 1, &maker);
	if (status == 0)
		status = vm_call(realm, maker, VM_VALUE_UNDEFINED, args, 2, &ignored);
	if (status == VM_THROWN) {
		realm->exception = VM_VALUE_UNDEFINED;
		return EINVAL;
	}
	if (status != 0)
		return status;
	if (window->remote_maker == NULL)
		return EINVAL;

	/* Succeeded: the functions are the window's. */
	return 0;
}

/* setRemote(maker, write, read): the remote prelude's functions. */
static int
remote_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	unsigned index;

	UNUSED_PARAMETER(this_value);
	*result = VM_VALUE_UNDEFINED;
	for (index = 0; index < 3U; index++) {
		if (!vm_value_is_callable(js_argument(args, count, index)))
			return vm_throw_type_error(realm, "setRemote: a parameter is not a function.");
	}
	window = bind_window_of(realm);
	window->remote_maker = vm_value_as_cell(args[0]);
	window->remote_write = vm_value_as_cell(args[1]);
	window->remote_read = vm_value_as_cell(args[2]);
	return 0;
}

/* post(kind, frame, tape, targetOrigin): a message written by the prelude, given to the embedder. */
static int
remote_post(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct bind_tape tape;
	struct wb_buffer target;
	struct wb_buffer origin;
	double kind;
	double frame;
	int status;

	UNUSED_PARAMETER(this_value);
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	if (window->host.remote == NULL || window->detached)
		return 0;
	status = vm_to_number(realm, js_argument(args, count, 0), &kind);
	if (status == 0)
		status = vm_to_number(realm, js_argument(args, count, 1), &frame);
	if (status != 0)
		return status;

	/* The tape, the origin it may go to and the sender's. */
	bind_tape_init(&tape);
	wb_buffer_init(&target);
	wb_buffer_init(&origin);
	status = remote_tape_write(realm, js_argument(args, count, 2), &tape);
	if (status == 0)
		status = remote_utf8(realm, js_argument(args, count, 3), &target);
	if (status == 0)
		status = remote_origin(window, &origin);
	if (status == 0)
		status = window->host.remote->post(window->host.remote_context, window, (int)kind, (uint64_t)frame, &tape, wb_buffer_string(&target),
		    wb_buffer_string(&origin));
	else
		bind_tape_release(&tape);
	wb_buffer_release(&target);
	wb_buffer_release(&origin);

	/* A window gone takes no message: nothing happens. */
	if (status == ENOENT)
		status = 0;
	return status;
}

/* navigate(kind, frame, url, replace): the window pointed at a URL, resolved against the caller's document. */
static int
remote_navigate(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_buffer text;
	struct wb_buffer base;
	struct wb_buffer href;
	struct net_url base_url;
	struct net_url url;
	double kind;
	double frame;
	int replace;
	int status;

	UNUSED_PARAMETER(this_value);
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	if (window->host.remote == NULL || window->detached)
		return 0;
	status = vm_to_number(realm, js_argument(args, count, 0), &kind);
	if (status == 0)
		status = vm_to_number(realm, js_argument(args, count, 1), &frame);
	if (status != 0)
		return status;
	replace = js_argument(args, count, 3) == VM_VALUE_TRUE;

	/* The URL against the caller's document's. */
	wb_buffer_init(&text);
	wb_buffer_init(&base);
	wb_buffer_init(&href);
	memset(&base_url, 0, sizeof(base_url));
	memset(&url, 0, sizeof(url));
	status = remote_utf8(realm, js_argument(args, count, 2), &text);
	if (status == 0)
		status = bind_document_url(window->document, 1, &base);
	if (status == 0)
		status = net_url_parse(wb_buffer_string(&base), base.length, NULL, &base_url);
	if (status == 0) {
		status = net_url_parse(wb_buffer_string(&text), text.length, &base_url, &url);
		if (status == EINVAL) {
			status = bind_throw_dom(realm, "SyntaxError", "Failed to set the 'href' property on 'Location': The URL is invalid.");
			goto done;
		}
	}
	if (status == 0)
		status = net_url_serialize(&url, 0, &href);
	if (status == 0)
		status = window->host.remote->navigate(window->host.remote_context, window, (int)kind, (uint64_t)frame, wb_buffer_string(&href), replace);
	if (status == ENOENT)
		status = 0;

done:
	net_url_release(&url);
	net_url_release(&base_url);
	wb_buffer_release(&text);
	wb_buffer_release(&base);
	wb_buffer_release(&href);
	return status;
}

/* info(kind, frame, what): whether the window is closed (1 or 0), or how many frames it has. */
static int
remote_info(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	double kind;
	double frame;
	double what;
	int status;

	UNUSED_PARAMETER(this_value);
	window = bind_window_of(realm);
	status = vm_to_number(realm, js_argument(args, count, 0), &kind);
	if (status == 0)
		status = vm_to_number(realm, js_argument(args, count, 1), &frame);
	if (status == 0)
		status = vm_to_number(realm, js_argument(args, count, 2), &what);
	if (status != 0)
		return status;

	/* A window that reaches none is closed and has none. */
	*result = vm_value_int32((int)what == BIND_REMOTE_CLOSED ? 1 : 0);
	if (window->host.remote == NULL || window->detached)
		return 0;
	*result = vm_value_int32(window->host.remote->info(window->host.remote_context, (int)kind, (uint64_t)frame, (int)what));
	return 0;
}

/* detach(buffer): an ArrayBuffer transferred with a message keeps no bytes. */
static int
remote_detach(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(this_value);
	*result = VM_VALUE_UNDEFINED;
	js_typed_detach(js_argument(args, count, 0));
	return 0;
}

/* detached(buffer): whether an ArrayBuffer was transferred already. */
static int
remote_detached(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(this_value);
	*result = js_typed_detached(js_argument(args, count, 0)) ? VM_VALUE_TRUE : VM_VALUE_FALSE;
	return 0;
}

/* Makes a proxy of a window of another agent in a window's heap (the prelude run first). */
static int
remote_make(
	struct bind_window *window,
	int kind,
	uint64_t frame,
	vm_value owner,
	vm_value *result)
{
	vm_value args[3];
	int status;

	/* The maker, run with the window's natives. */
	*result = VM_VALUE_NULL;
	status = remote_ready(window);
	if (status != 0)
		return status;
	args[0] = vm_value_int32(kind);
	args[1] = vm_value_number((double)frame);
	args[2] = owner;
	status = vm_call(window->realm, vm_value_cell(window->remote_maker), VM_VALUE_UNDEFINED, args, 3, result);
	if (status == VM_THROWN) {
		window->realm->exception = VM_VALUE_UNDEFINED;
		*result = VM_VALUE_NULL;
		return 0;
	}
	return status;
}

/* Copies the prelude's tape (an array of plain items) out of the heap. */
static int
remote_tape_write(
	struct vm_realm *realm,
	vm_value array,
	struct bind_tape *tape)
{
	struct bind_tape_item item;
	struct vm_string *string;
	vm_value length_value;
	vm_value value;
	double length;
	size_t index;
	size_t at;
	int status;

	/* Its length. */
	if (!vm_value_is_object(array))
		return EINVAL;
	status = vm_get(realm, array, vm_value_cell(vm_atom_from_ascii(realm->heap, "length")), &length_value);
	if (status == 0)
		status = vm_to_number(realm, length_value, &length);
	if (status != 0)
		return status;
	if (length < 0.0 || length > (double)REMOTE_TAPE_MAX)
		return bind_throw_dom(realm, "DataCloneError", "The message is too large to be cloned.");

	/* Each item. */
	for (index = 0; index < (size_t)length; index++) {
		status = vm_get(realm, array, vm_value_int32((int32_t)index), &value);
		if (status != 0)
			return status;
		memset(&item, 0, sizeof(item));
		if (value == VM_VALUE_UNDEFINED) {
			item.type = BIND_TAPE_UNDEFINED;
		} else if (value == VM_VALUE_NULL) {
			item.type = BIND_TAPE_NULL;
		} else if (value == VM_VALUE_FALSE) {
			item.type = BIND_TAPE_FALSE;
		} else if (value == VM_VALUE_TRUE) {
			item.type = BIND_TAPE_TRUE;
		} else if (vm_value_is_number(value)) {
			item.type = BIND_TAPE_NUMBER;
			item.number = vm_value_as_number(value);
		} else if (vm_value_is_string(value)) {
			string = (struct vm_string *)vm_value_as_cell(value);
			item.type = BIND_TAPE_STRING;
			item.length = string->length;
			item.units = malloc(item.length == 0 ? 1U : item.length * sizeof(uint16_t));
			if (item.units == NULL)
				return ENOMEM;
			for (at = 0; at < item.length; at++)
				item.units[at] = vm_string_at(string, at);
		} else {
			return EINVAL;
		}
		status = wb_vector_push(&tape->items, &item);
		if (status != 0) {
			free(item.units);
			return status;
		}
	}

	/* Succeeded: the tape holds the message. */
	return 0;
}

/* Makes an array of a heap of a tape's items. */
static int
remote_tape_read(
	struct vm_realm *realm,
	const struct bind_tape *tape,
	vm_value *array)
{
	const struct bind_tape_item *item;
	struct vm_string *string;
	vm_value value;
	size_t index;
	int status;

	/* The array, then each item into it (it is rooted by the caller once made). */
	status = js_builtin_array(realm, NULL, 0, array);
	if (status != 0)
		return status;
	for (index = 0; index < tape->items.count; index++) {
		item = wb_vector_at(&tape->items, index);
		switch (item->type) {
		case BIND_TAPE_NULL:
			value = VM_VALUE_NULL;
			break;
		case BIND_TAPE_FALSE:
			value = VM_VALUE_FALSE;
			break;
		case BIND_TAPE_TRUE:
			value = VM_VALUE_TRUE;
			break;
		case BIND_TAPE_NUMBER:
			value = vm_value_number(item->number);
			break;
		case BIND_TAPE_STRING:
			string = vm_string_from_units(realm->heap, item->units, item->length);
			if (string == NULL)
				return ENOMEM;
			value = vm_value_cell(string);
			break;
		default:
			value = VM_VALUE_UNDEFINED;
			break;
		}
		status = vm_set(realm, *array, vm_value_int32((int32_t)index), value, 1);
		if (status != 0)
			return status;
	}

	/* Succeeded: the array holds the tape. */
	return 0;
}

/* A window's origin, serialized. */
static int
remote_origin(
	struct bind_window *window,
	struct wb_buffer *origin)
{
	vm_value value;
	int status;

	/* The location's origin. */
	status = bind_location_part(window, BIND_LOCATION_ORIGIN, &value);
	if (status != 0)
		return status;
	if (!vm_value_is_string(value))
		return wb_buffer_append(origin, "null", 4);
	return vm_string_to_utf8((struct vm_string *)vm_value_as_cell(value), origin);
}

/* A value as a string, in UTF-8. */
static int
remote_utf8(
	struct vm_realm *realm,
	vm_value value,
	struct wb_buffer *out)
{
	struct vm_string *string;
	int status;

	/* Its string, then its bytes. */
	status = bind_to_string(realm, value, &string);
	if (status != 0)
		return status;
	return vm_string_to_utf8(string, out);
}
