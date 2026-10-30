/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Initial about:blank HTML iframe contexts share their tab's heap and agent,
 * with separate realms, globals and Documents. A connected element retains
 * its collectible child; removal retires nested contexts synchronously.
 */

#include "bind/internal.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

/* Bounds synchronous recursive retirement through nested child Documents. */
#define FRAME_MAX_DEPTH 64U

static int frame_src_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int frame_src_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int frame_svg_document(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int frame_document(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int frame_global(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* Synchronous child accessors create only an initial, verified blank context. */
static const struct bind_attribute frame_attributes[] = {
	{ "src", frame_src_get, frame_src_set },
	{ "contentDocument", frame_document, NULL },
	{ "contentWindow", frame_global, NULL },
	{ NULL, NULL, NULL }
};

/* SVG access reports only a current actual SVG resource owner. */
static const struct bind_operation frame_operations[] = {
	{ "getSVGDocument", 0, frame_svg_document },
	{ NULL, 0, NULL }
};

/* HTML iframes retain the ordinary HTMLElement properties and methods. */
const struct bind_interface bind_html_iframe_element_interface = {
	"HTMLIFrameElement", BIND_HTML_ELEMENT, 0, NULL, frame_attributes, frame_operations, NULL
};

static int frame_loaded_document(struct vm_realm *realm, vm_value receiver, int tag, int svg, vm_value *result);
static int frame_context(struct vm_realm *realm, vm_value this_value, struct bind_window **window);
static int frame_create(struct vm_realm *caller, struct bind_window *parent, struct dom_element *element, struct bind_window **window);
static int frame_tree(struct dom_document *document);
static int frame_window_this(struct vm_realm *realm, vm_value this_value, struct bind_window **window);
static void frame_removed(struct dom_document *document, struct dom_node *node);
static void frame_remove_subtree(struct dom_node *root);

/*
 * Reports an object's actual loaded Document without creating a blank context.
 */
int
bind_object_document(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The native branded helper finds only connected current resource owners. */
	error = frame_loaded_document(realm, receiver, DOM_TAG_OBJECT, 0, result);
	if (error != 0)
		return error;

	/* Succeeded: the actual loaded Document or null is available. */
	return 0;
}

/*
 * Reports an object's actual loaded SVG Document.
 */
int
bind_object_svg_document(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Declared native content kind determines SVG availability. */
	error = frame_loaded_document(realm, receiver, DOM_TAG_OBJECT, 1, result);
	if (error != 0)
		return error;

	/* Succeeded: this is the actual SVG resource owner or null. */
	return 0;
}

/*
 * Refreshes a managed child's viewport from its actual embedding content box.
 *
 * Empty hosts retain the supplied integer viewport contract; active layout hosts
 * provide current used geometry without reading script-visible element properties.
 */
int
bind_frame_viewport(
	struct bind_window *window)
{
	struct bind_window *parent;
	struct dom_element *frame;
	struct vm_cell *roots[2];
	struct bind_box box;
	double width;
	double height;
	unsigned index;
	int connected;
	int found;
	int status;

	/* Primary hosts and retired managed contexts retain their embedding contract. */
	if (!window->owned ||
	    window->detached ||
	    window->frame == NULL)
		return 0;

	/* Resolve the frame's actual current parent owner without consulting global aliases. */
	frame = window->frame;
	parent = frame->node.document->view;
	if (parent == NULL ||
	    parent->detached ||
	    parent->host.node_box == NULL)
		return 0;

	/* Mere ownership by a Document does not make a detached frame rendered. */
	connected = dom_is_inclusive_ancestor(&parent->document->node, &frame->node);
	if (!connected)
		return 0;

	/* A host layout callback may allocate or retire the only connected frame edge. */
	roots[0] = &window->realm->cell;
	roots[1] = &frame->node.cell;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(window->realm->heap, &roots[index]);
		if (status != 0) {
			/* Release only slots whose registration succeeded before the callback. */
			while (index != 0) {
				index--;
				vm_heap_remove_root(window->realm->heap, &roots[index]);
			}

			/* Propagate the failed temporary ownership registration. */
			return status;
		}
	}

	/* No returned geometry is observed if the callback retired or reparented this context. */
	memset(&box, 0, sizeof(box));
	found = parent->host.node_box(parent->host.context, &frame->node, &box);
	if (window->detached ||
	    window->frame != frame ||
	    frame->node.document->view != parent) {
		vm_heap_remove_root(window->realm->heap, &roots[1]);
		vm_heap_remove_root(window->realm->heap, &roots[0]);
		return 0;
	}

	/* Missing layout boxes produce an actual zero-size child viewport. */
	width = 0;
	height = 0;
	if (found) {
		/* Borders and padding surround, rather than enlarge, the child's content viewport. */
		width = box.width - box.border_left - box.border_right - box.padding_left - box.padding_right;
		height = box.height - box.border_top - box.border_bottom - box.padding_top - box.padding_bottom;
	}

	/* Native negative content extents clamp to zero independently on each axis. */
	if (width < 0)
		width = 0;

	/* Height has the same nonnegative content-box bound. */
	if (height < 0)
		height = 0;

	/* Reject NaN, infinity and out-of-int extents before the existing integer conversion. */
	status = 0;
	if (!(width >= 0) ||
	    !(height >= 0) ||
	    width > INT_MAX ||
	    height > INT_MAX) {
		status = EOVERFLOW;
	} else {
		/* Exact zero stays zero; the established viewport contract uses integral CSS pixels. */
		bind_window_set_viewport(window, (int)width, (int)height);
	}

	/* No managed ownership edge is registered beyond this one geometry observation. */
	vm_heap_remove_root(window->realm->heap, &roots[1]);
	vm_heap_remove_root(window->realm->heap, &roots[0]);
	if (status != 0)
		return status;

	/* Succeeded: later media cache lookups use the current actual child viewport. */
	return 0;
}

/*
 * Attaches a completed Window to its Document without a DOM-to-binding include.
 */
void
bind_frames_attach(
	struct bind_window *window)
{
	/* Managed Document.context already supplies the traced lifetime edge. */
	window->document->view = window;
	window->document->removed = frame_removed;
	window->document->attribute_changed = bind_environment_attribute_mutation;

	/* Succeeded: wrapper creation and removal can find the actual owner. */
	return;
}

/*
 * Retires every child context of a detached Window while keeping its Document.
 */
void
bind_frames_detach(
	struct bind_window *window)
{
	/* Each nested owner is detached before the enclosing Window can disappear. */
	frame_remove_subtree(&window->document->node);

	/* Succeeded: no connected element retains a retired child context. */
	return;
}

/*
 * Clears an explicit primary Window's Document hooks before its C record dies.
 */
void
bind_frames_release(
	struct bind_window *window)
{
	/* The primary embedder calls this while its DOM cells are still live. */
	bind_window_detach(window);
	window->document->view = NULL;
	window->document->removed = NULL;
	window->document->attribute_changed = NULL;

	/* Succeeded: subsequent DOM removal cannot call the released primary host. */
	return;
}

/*
 * Reports the connected child window of an embedding element (an HTML iframe
 * or object), or NULL when the node holds none.
 */
struct bind_window *
bind_frame_child(
	const struct dom_node *node)
{
	const struct dom_element *element;
	const struct vm_realm *realm;

	/* Only an HTML iframe or object holds a frame. */
	if (node->type != DOM_ELEMENT)
		return NULL;
	element = (const struct dom_element *)node;
	if (element->ns != DOM_NS_HTML)
		return NULL;
	if (element->tag != DOM_TAG_IFRAME && element->tag != DOM_TAG_OBJECT)
		return NULL;

	/* Its published context, if there is one. */
	realm = (const struct vm_realm *)element->child_context;
	if (realm == NULL)
		return NULL;
	return realm->host;
}

/*
 * Points a frame's own document at another location, as the document's script
 * does when it navigates: the frame element's source attribute stays, and the
 * page loads the location into the frame at its next checkpoint.  Reports in
 * *started whether this window is a connected frame's (a top-level window is
 * navigated by its host).
 */
int
bind_frame_navigate(
	struct bind_window *window,
	const char *location,
	size_t length,
	int *started)
{
	struct vm_string *string;

	/* Only a connected frame can be pointed elsewhere. */
	*started = 0;
	if (window->detached || window->frame == NULL)
		return 0;

	/* The location is a string of the heap, which the element keeps alive. */
	string = vm_string_from_utf8(window->realm->heap, location, length);
	if (string == NULL)
		return ENOMEM;
	window->frame->child_navigation = string;

	/* Succeeded: the frame's next scan loads the location. */
	*started = 1;
	return 0;
}

/*
 * Reports the receiver Window's parent, or itself for a top-level context.
 */
int
bind_frame_parent(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_object *global;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Borrowed getters must resolve the receiver's actual Window across realms. */
	status = frame_window_this(realm, this_value, &window);
	if (status != 0)
		return status;

	/* The parent relation remains readable from a saved detached Window. */
	global = window->parent_global;
	if (global == NULL)
		global = window->realm->global;
	*result = vm_value_cell(global);

	/* Succeeded: the receiver's parent identity is reported. */
	return 0;
}

/*
 * Reports the receiver Window's outermost ancestor.
 */
int
bind_frame_top(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_object *global;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Validates a real Window without invoking user-defined document getters. */
	status = frame_window_this(realm, this_value, &window);
	if (status != 0)
		return status;

	/* A top-level context has no separate ancestor global. */
	global = window->top_global;
	if (global == NULL)
		global = window->realm->global;
	*result = vm_value_cell(global);

	/* Succeeded: the outermost ancestor's identity is reported. */
	return 0;
}

/*
 * Reports the receiver Window itself for the frames attribute.
 */
int
bind_frame_self(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The frames identity is the Window, not a parent alias or a new Array. */
	status = frame_window_this(realm, this_value, &window);
	if (status != 0)
		return status;
	*result = vm_value_cell(window->realm->global);

	/* Succeeded: the receiver's frames identity is reported. */
	return 0;
}

/*
 * Reports whether a saved Window's browsing context has been retired.
 */
int
bind_frame_closed(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The Window remains a valid receiver after its frame is removed. */
	status = frame_window_this(realm, this_value, &window);
	if (status != 0)
		return status;

	/* Retirement changes state without destroying reachable script objects. */
	*result = VM_VALUE_FALSE;
	if (window->detached)
		*result = VM_VALUE_TRUE;

	/* Succeeded: the receiver's lifecycle state is reported. */
	return 0;
}

/*
 * Reports a child's connected iframe element, or null after retirement.
 */
int
bind_frame_element(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Resolves the actual receiver rather than the borrowed getter's realm. */
	status = frame_window_this(realm, this_value, &window);
	if (status != 0)
		return status;

	/* Top-level and retired contexts have no connected frame element. */
	*result = VM_VALUE_NULL;
	if (window->frame == NULL)
		return 0;

	/* The parent Document chooses the frame wrapper's original realm. */
	status = bind_wrap(window, &window->frame->node, result);
	if (status != 0)
		return status;

	/* Succeeded: the connected frame's cached wrapper is reported. */
	return 0;
}

/* Reports a connected iframe's child Document without loading foreign content. */
static int
frame_document(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Detached, inactive and unsupported sandbox contexts expose no Document. */
	*result = VM_VALUE_NULL;
	status = frame_context(realm, this_value, &window);
	if (status != 0)
		return status;

	/* An absent supported context has no child Document to wrap. */
	if (window == NULL)
		return 0;

	/* Its own realm's wrapper was installed with its global document property. */
	status = bind_wrap(window, &window->document->node, result);
	if (status != 0)
		return status;

	/* Succeeded: a distinct initial child Document is reported. */
	return 0;
}

/* Reports a connected iframe's distinct child global object. */
static int
frame_global(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* No caller-realm alias is returned when there is no supported context. */
	*result = VM_VALUE_NULL;
	status = frame_context(realm, this_value, &window);
	if (status != 0)
		return status;

	/* An absent supported context has no child Window to report. */
	if (window == NULL)
		return 0;

	/* Publishes this child's own global, independently of the caller's realm. */
	*result = vm_value_cell(window->realm->global);

	/* Succeeded: a distinct initial child Window is reported. */
	return 0;
}

/* Finds or creates an initial child only while its owning Document is active. */
static int
frame_context(
	struct vm_realm *realm,
	vm_value this_value,
	struct bind_window **window)
{
	struct dom_node *node;
	struct dom_element *element;
	struct bind_window *parent;
	struct vm_realm *child;
	struct vm_string *sandbox;
	int connected;
	int status;

	/* The interface getter accepts only an actual HTML iframe element. */
	*window = NULL;
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Non-element nodes cannot own an HTML iframe context. */
	if (node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Foreign namespaces and unrelated HTMLElement receivers are not iframes. */
	element = (struct dom_element *)node;
	if (element->ns != DOM_NS_HTML || element->tag != DOM_TAG_IFRAME) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* An inactive saved Document cannot create new browsing contexts. */
	parent = node->document->view;
	if (parent == NULL || parent->detached)
		return 0;

	/* Mere ownership by a Document does not connect a detached element. */
	connected = dom_is_inclusive_ancestor(&node->document->node, node);
	if (!connected)
		return 0;

	/* Unsupported sandbox origins must never expose an unverified child global. */
	sandbox = dom_attribute_ascii(element, "sandbox");
	if (sandbox != NULL)
		return 0;

	/* Repeated observation preserves the one published context's identity. */
	if (element->child_context != NULL) {
		child = (struct vm_realm *)element->child_context;
		*window = child->host;
		return 0;
	}

	/* Publishes only a complete initial context, propagating allocation failures. */
	status = frame_create(realm, parent, element, window);
	if (status != 0)
		return status;

	/* Succeeded: the connected element now owns its initial context. */
	return 0;
}

/* Builds an initial blank tree under explicit ownership before context installation. */
static int
frame_create(
	struct vm_realm *caller,
	struct bind_window *parent,
	struct dom_element *element,
	struct bind_window **window)
{
	struct dom_document *document;
	struct vm_cell *root;
	int status;

	/* Preserve the existing synchronous nesting exception. */
	if (parent->context_depth >= FRAME_MAX_DEPTH) {
		status = vm_throw_range_error(caller, "Maximum frame nesting depth exceeded.");
		return status;
	}

	/* Register the slot before allocating any part of the initial tree. */
	root = NULL;
	status = vm_heap_add_root(caller->heap, &root);
	if (status != 0)
		return status;
	document = dom_document_create(caller->heap);
	if (document == NULL) {
		vm_heap_remove_root(caller->heap, &root);
		return ENOMEM;
	}

	/* The protected Document owns each node as soon as it is attached. */
	root = &document->node.cell;
	document->quirks = DOM_QUIRKS;
	status = frame_tree(document);
	if (status == 0)
		status = bind_frame_install(parent, element, document, window);
	vm_heap_remove_root(caller->heap, &root);
	if (status != 0)
		return status;

	/* Initial blank contexts are synchronously complete before observation returns. */
	bind_window_set_ready_state(*window, "complete");

	/* Succeeded: the actual initial blank pair is owned by the iframe. */
	return 0;
}

/* Creates html/head/body without a parser or script-created replacement stream. */
static int
frame_tree(
	struct dom_document *document)
{
	struct vm_string *name;
	struct dom_element *html;
	struct dom_element *head;
	struct dom_element *body;

	/* Each allocation is checked before adding the next owned node. */
	name = vm_atom_from_ascii(document->heap, "html");
	if (name == NULL)
		return ENOMEM;

	/* Adds the root only after its name has been allocated successfully. */
	html = dom_element_create(document, DOM_NS_HTML, name, NULL);
	if (html == NULL)
		return ENOMEM;
	dom_append_child(&document->node, &html->node);

	/* The empty head precedes the empty body in the initial document. */
	name = vm_atom_from_ascii(document->heap, "head");
	if (name == NULL)
		return ENOMEM;

	/* Adds an empty head as the root's first child. */
	head = dom_element_create(document, DOM_NS_HTML, name, NULL);
	if (head == NULL)
		return ENOMEM;
	dom_append_child(&html->node, &head->node);

	/* The body exists synchronously when script first observes the context. */
	name = vm_atom_from_ascii(document->heap, "body");
	if (name == NULL)
		return ENOMEM;

	/* Adds the synchronously accessible body after the empty head. */
	body = dom_element_create(document, DOM_NS_HTML, name, NULL);
	if (body == NULL)
		return ENOMEM;
	dom_append_child(&html->node, &body->node);

	/* Succeeded: the canonical blank tree is complete. */
	return 0;
}

/* Validates a Window through its immutable own document property and owner. */
static int
frame_window_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct bind_window **window)
{
	struct vm_object *object;
	struct vm_property property;
	struct dom_node *node;
	vm_value key;
	int object_value;
	int found;
	int status;

	/* No property getter or user code runs while the receiver is branded. */
	*window = NULL;
	object_value = vm_value_is_object(this_value);
	if (!object_value) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Real Window.document is an own, immutable data property in this binding. */
	object = (struct vm_object *)vm_value_as_cell(this_value);
	key = vm_key_from_ascii(realm->heap, "document");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	found = vm_object_get_own_ordinary(object, key, &property);
	if (!found) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* An arbitrary accessor property cannot impersonate a real Window. */
	if ((property.attributes & VM_PROPERTY_ACCESSOR) != 0) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* A fake object with somebody else's Document still fails global identity. */
	node = bind_node_of(*property.value);
	if (node == NULL || node->type != DOM_DOCUMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Saved detached windows retain their owner, but released primaries do not. */
	*window = node->document->view;
	if (*window == NULL) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* The Document must name this exact global, not merely a copied property. */
	if ((*window)->realm->global != object) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: this receiver has a live retained Window record. */
	return 0;
}

/* Removes browsing-context ownership before DOM unlinks the removed subtree. */
static void
frame_removed(
	struct dom_document *document,
	struct dom_node *node)
{
	UNUSED_PARAMETER(document);

	/* Every iframe below a removed ancestor loses its connected context. */
	frame_remove_subtree(node);

	/* Succeeded: DOM may now unlink without retaining active child contexts. */
	return;
}

/* Walks DOM iteratively and recursively retires only bounded child contexts. */
static void
frame_remove_subtree(
	struct dom_node *root)
{
	struct dom_node *walk;
	struct dom_element *element;
	struct vm_realm *realm;
	struct bind_window *window;
	struct bind_window *owner;

	/* The root itself may be the iframe whose connection is disappearing. */
	walk = root;
	while (walk != NULL) {
		/* Ordinary nodes have no browsing context to retire. */
		if (walk->type == DOM_ELEMENT) {
			element = (struct dom_element *)walk;

			/* Removal invalidates pending work even if this element is reinserted before completion. */
			element->child_epoch++;
			element->child_load_seen = 0;
			element->child_source = NULL;
			element->child_attribute = NULL;
			element->child_navigation = NULL;

			/* Only published child contexts participate in synchronous retirement. */
			if (element->child_context != NULL) {
				/* Clears connection ownership before descending into nested contexts. */
				realm = (struct vm_realm *)element->child_context;
				window = realm->host;
				element->child_context = NULL;

				/* A complete child host loses its connected element before detaching. */
				if (window != NULL) {
					window->frame = NULL;
					owner = element->node.document->view;
					if (owner != NULL)
						window->realm->job_target = owner->realm;
					bind_window_detach(window);
				}
			}
		}

		/* Context retirement never changes the enclosing DOM traversal links. */
		walk = bind_following(walk, root);
	}

	/* Succeeded: every connected child edge in this subtree was retired. */
	return;
}

/* Reflects iframe src against its actual Document base. */
static int
frame_src_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Brand before reading the native source attribute. */
	error = bind_reflect_url_get(realm, receiver, DOM_TAG_IFRAME, "src", result);
	if (error != 0)
		return error;

	/* Succeeded: src is the real reflected URL. */
	return 0;
}

/* Reflects iframe src after one WebIDL scalar conversion. */
static int
frame_src_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* Brand before any potentially reentrant argument conversion. */
	error = bind_reflect_url_set(realm, receiver, DOM_TAG_IFRAME, "src", args, count, result);
	if (error != 0)
		return error;

	/* Succeeded: native src controls subsequent resource tasks. */
	return 0;
}

/* Reports only a current SVG MIME resource loaded by this iframe. */
static int
frame_svg_document(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The interface brand and actual content kind are checked independently. */
	error = frame_loaded_document(realm, receiver, DOM_TAG_IFRAME, 1, result);
	if (error != 0)
		return error;

	/* Succeeded: the loaded SVG owner or null is available. */
	return 0;
}

/* Finds an existing current native owner without constructing a replacement. */
static int
frame_loaded_document(
	struct vm_realm *realm,
	vm_value receiver,
	int tag,
	int svg,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_element *element;
	struct vm_realm *child;
	struct bind_window *parent;
	struct bind_window *window;
	struct vm_string *sandbox;
	int connected;
	int error;

	/* Borrowing an accessor cannot forge an iframe or object interface. */
	*result = VM_VALUE_NULL;
	error = bind_this_node(realm, receiver, &node);
	if (error != 0)
		return error;
	if (node->type != DOM_ELEMENT) {
		error = bind_throw_illegal(realm);
		return error;
	}

	/* Use actual namespace and tag, independent of public prototype mutation. */
	element = (struct dom_element *)node;
	if (element->ns != DOM_NS_HTML || element->tag != tag) {
		error = bind_throw_illegal(realm);
		return error;
	}

	/* Inactive Documents and disconnected nodes expose no active child. */
	parent = node->document->view;
	if (parent == NULL || parent->detached)
		return 0;
	connected = dom_is_inclusive_ancestor(&node->document->node, node);
	if (!connected)
		return 0;
	sandbox = dom_attribute_ascii(element, "sandbox");
	if (sandbox != NULL || element->child_context == NULL)
		return 0;

	/* SVG availability follows the selected actual response MIME processing kind. */
	child = (struct vm_realm *)element->child_context;
	window = child->host;
	if (window == NULL || window->detached)
		return 0;
	if (svg && window->document->content != DOM_CONTENT_SVG)
		return 0;
	error = bind_wrap(window, &window->document->node, result);
	if (error != 0)
		return error;

	/* Succeeded: cached wrapping preserves actual owner identity. */
	return 0;
}
