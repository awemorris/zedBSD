/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The geometry of elements for scripts (ws074-p031): getBoundingClientRect
 * and getClientRects with DOMRect, the client, offset and scroll sizes,
 * and the window's scroll position.
 *
 * The host says where a node is on the page as it is laid out now (it
 * lays the page out first when the document changed, as other browsers
 * do when a script asks), in CSS pixels from the document's top left; the
 * rectangles a script gets are the viewport's, the document's scroll taken
 * off.  The root element's client size is the viewport's and its scroll
 * size the document's; any other element's scroll size is its client size
 * (the layout does not keep how far a box's content overflows it), and
 * its scroll position is 0 (boxes do not scroll).  A node without a box
 * has an empty rectangle and sizes of 0.
 *
 * ws074-p082 adds offsetParent, offsetTop and offsetLeft (CSSOM View: the
 * nearest positioned ancestor, table cell or table, or the body), and the
 * scroll a script asks for: window.scrollTo, scroll and scrollBy, an
 * element's scrollIntoView, and the root element's scrollTo, scroll,
 * scrollBy and scrollTop, which move the document (the host keeps the
 * scroll inside it; boxes do not scroll, and the page not sideways).
 */

#include "bind/internal.h"
#include "css/css.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * The measures of an element the getters report (the part a getter asks
 * geometry_measure for).
 */
enum geometry_part {
	GEOMETRY_CLIENT_WIDTH,
	GEOMETRY_CLIENT_HEIGHT,
	GEOMETRY_CLIENT_TOP,
	GEOMETRY_CLIENT_LEFT,
	GEOMETRY_SCROLL_WIDTH,
	GEOMETRY_SCROLL_HEIGHT,
	GEOMETRY_SCROLL_TOP,
	GEOMETRY_SCROLL_LEFT,
	GEOMETRY_OFFSET_WIDTH,
	GEOMETRY_OFFSET_HEIGHT
};

/*
 * The parts of a DOMRect its getters report (the part is the getter
 * function's data).
 */
enum geometry_rect_part {
	GEOMETRY_RECT_X,
	GEOMETRY_RECT_Y,
	GEOMETRY_RECT_WIDTH,
	GEOMETRY_RECT_HEIGHT,
	GEOMETRY_RECT_TOP,
	GEOMETRY_RECT_RIGHT,
	GEOMETRY_RECT_BOTTOM,
	GEOMETRY_RECT_LEFT
};

/*
 * The state of a DOMRect, the cell its object wraps: its place and size
 * in CSS pixels.  The width and height may be negative when a script makes
 * the rectangle so; top, right, bottom and left are the edges either way.
 */
struct geometry_rect {
	struct vm_cell cell;
	double x;
	double y;
	double width;
	double height;
};

/*
 * One member of DOMRect: its name and the part its getter reports.
 */
struct geometry_rect_member {
	const char *name;
	int part;
	int writable;
};

static int geometry_measure(struct vm_realm *realm, vm_value this_value, int part, vm_value *result);
static int geometry_element_this(struct vm_realm *realm, vm_value this_value, struct dom_element **element);
static int geometry_box(struct bind_window *window, struct dom_element *element, struct bind_box *box);
static void geometry_element_scroll(struct bind_window *window, struct dom_element *element, double x, int given_x, double y, int given_y);
static int geometry_is_root(const struct bind_window *window, const struct dom_element *element);
static void geometry_scroll(const struct bind_window *window, double *x, double *y);
static double geometry_round(double value);
static int geometry_position(struct bind_window *window, struct dom_element *element, int *position);
static int geometry_offset_parent(struct bind_window *window, struct dom_element *element, struct dom_element **parent);
static int geometry_fixed_container(struct bind_window *window, struct dom_element *element, int *contains);
static int geometry_offset(struct vm_realm *realm, vm_value this_value, int top, vm_value *result);
static int geometry_scroll_arguments(struct vm_realm *realm, const vm_value *args, unsigned count, double *x, double *y, int *given_x, int *given_y);
static void geometry_scroll_move(struct bind_window *window, double x, double y);
static double geometry_finite(double value);
static int geometry_stops_offset(const struct dom_node *node);
static int geometry_rect_create(struct bind_window *window, double x, double y, double width, double height, vm_value *value);
static int geometry_rect_of(struct vm_realm *realm, vm_value value, struct geometry_rect **rect);
static int geometry_rect_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int geometry_rect_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int geometry_rect_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int geometry_rect_to_json(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static double geometry_rect_value(const struct geometry_rect *rect, int part);

/* The state of a DOMRect, which refers to nothing. */
static const struct vm_cell_type geometry_rect_type = { "dom-rect", NULL, NULL };

/*
 * The members of DOMRect, in the order toJSON writes them.  The table is
 * constant for the life of the program.
 */
static const struct geometry_rect_member geometry_rect_members[] = {
	{ "x", GEOMETRY_RECT_X, 1 },
	{ "y", GEOMETRY_RECT_Y, 1 },
	{ "width", GEOMETRY_RECT_WIDTH, 1 },
	{ "height", GEOMETRY_RECT_HEIGHT, 1 },
	{ "top", GEOMETRY_RECT_TOP, 0 },
	{ "right", GEOMETRY_RECT_RIGHT, 0 },
	{ "bottom", GEOMETRY_RECT_BOTTOM, 0 },
	{ "left", GEOMETRY_RECT_LEFT, 0 },
	{ NULL, 0, 0 }
};

/*
 * The operations of DOMRect; its attributes are installed from
 * geometry_rect_members by bind_geometry_install.  The table is constant
 * for the life of the program.
 */
static const struct bind_operation geometry_rect_operations[] = {
	{ "toJSON", 0, geometry_rect_to_json },
	{ NULL, 0, NULL }
};

/*
 * The DOMRect interface.
 */
const struct bind_interface bind_dom_rect_interface = {
	"DOMRect", BIND_NO_PARENT, 0, geometry_rect_construct, NULL, geometry_rect_operations, NULL
};

/*
 * Adds DOMRect's attributes to its prototype: an accessor for each part,
 * whose getter and setter know the part.
 */
int
bind_geometry_install(
	struct bind_window *window)
{
	const struct geometry_rect_member *member;
	struct vm_realm *realm;
	struct vm_function *getter;
	struct vm_function *setter;
	struct vm_accessor *accessor;
	vm_value setter_value;
	int status;

	/* Each member of the table. */
	realm = window->realm;
	for (member = geometry_rect_members; member->name != NULL; member++) {
		status = js_builtin_function(realm, member->name, 0, geometry_rect_get, NULL, &getter);
		if (status != 0)
			return status;
		getter->data = vm_value_int32(member->part);

		/* x, y, width and height can be set. */
		setter_value = VM_VALUE_UNDEFINED;
		if (member->writable) {
			status = js_builtin_function(realm, member->name, 1, geometry_rect_set, NULL, &setter);
			if (status != 0)
				return status;
			setter->data = vm_value_int32(member->part);
			setter_value = vm_value_cell(setter);
		}

		/* The accessor on the prototype. */
		accessor = vm_accessor_create(realm->heap, vm_value_cell(getter), setter_value);
		if (accessor == NULL)
			return ENOMEM;
		status = js_builtin_value(realm, window->prototypes[BIND_DOM_RECT], member->name, vm_value_cell(accessor), VM_PROPERTY_ACCESSOR | VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);
		if (status != 0)
			return status;
	}

	/* Succeeded: the prototype has every member. */
	return 0;
}

/*
 * Reports the rectangle an element takes in the viewport, the union of
 * its boxes' border boxes (getBoundingClientRect).
 */
int
bind_get_bounding_client_rect(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	struct bind_box box;
	double scroll_x;
	double scroll_y;
	int found;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element, and where it is on the page (nowhere without a box). */
	window = bind_window_of(realm);
	status = geometry_element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	found = geometry_box(window, element, &box);

	/* The viewport's rectangle: the scroll taken off, unless there is no box, which is all zero. */
	scroll_x = 0.0;
	scroll_y = 0.0;
	if (found)
		geometry_scroll(window, &scroll_x, &scroll_y);

	/* Succeeded: the rectangle. */
	status = geometry_rect_create(window, box.x - scroll_x, box.y - scroll_y, box.width, box.height, result);
	if (status != 0)
		return status;
	return 0;
}

/*
 * Lists the rectangles of an element's boxes in the viewport
 * (getClientRects): one, their union, for an element with a box, and none
 * for one without.
 */
int
bind_get_client_rects(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	struct vm_object *array;
	struct bind_box box;
	vm_value rect;
	double scroll_x;
	double scroll_y;
	int found;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element, its box, and an empty list. */
	window = bind_window_of(realm);
	status = geometry_element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	found = geometry_box(window, element, &box);
	status = bind_array_create(realm, &array);
	if (status != 0)
		return status;

	/* An element without a box has no rectangle. */
	if (!found) {
		*result = vm_value_cell(array);
		return 0;
	}

	/* The rectangle in the viewport. */
	geometry_scroll(window, &scroll_x, &scroll_y);
	status = geometry_rect_create(window, box.x - scroll_x, box.y - scroll_y, box.width, box.height, &rect);
	if (status != 0)
		return status;
	status = vm_object_define(realm->heap, array, vm_value_int32(0), rect, VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;

	/* Succeeded: the list. */
	*result = vm_value_cell(array);
	return 0;
}

/* Reports the width of an element's padding box, or the viewport's for the root (clientWidth). */
int
bind_client_width(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The measure. */
	status = geometry_measure(realm, this_value, GEOMETRY_CLIENT_WIDTH, result);
	if (status != 0)
		return status;

	/* Succeeded: the width. */
	return 0;
}

/* Reports the height of an element's padding box, or the viewport's for the root (clientHeight). */
int
bind_client_height(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The measure. */
	status = geometry_measure(realm, this_value, GEOMETRY_CLIENT_HEIGHT, result);
	if (status != 0)
		return status;

	/* Succeeded: the height. */
	return 0;
}

/* Reports the width of an element's top border (clientTop). */
int
bind_client_top(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The measure. */
	status = geometry_measure(realm, this_value, GEOMETRY_CLIENT_TOP, result);
	if (status != 0)
		return status;

	/* Succeeded: the border. */
	return 0;
}

/* Reports the width of an element's left border (clientLeft). */
int
bind_client_left(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The measure. */
	status = geometry_measure(realm, this_value, GEOMETRY_CLIENT_LEFT, result);
	if (status != 0)
		return status;

	/* Succeeded: the border. */
	return 0;
}

/* Reports the width of what an element scrolls over, or the document's for the root (scrollWidth). */
int
bind_scroll_width(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The measure. */
	status = geometry_measure(realm, this_value, GEOMETRY_SCROLL_WIDTH, result);
	if (status != 0)
		return status;

	/* Succeeded: the width. */
	return 0;
}

/* Reports the height of what an element scrolls over, or the document's for the root (scrollHeight). */
int
bind_scroll_height(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The measure. */
	status = geometry_measure(realm, this_value, GEOMETRY_SCROLL_HEIGHT, result);
	if (status != 0)
		return status;

	/* Succeeded: the height. */
	return 0;
}

/* Reports how far an element is scrolled down, the document's for the root (scrollTop). */
int
bind_scroll_top(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The measure. */
	status = geometry_measure(realm, this_value, GEOMETRY_SCROLL_TOP, result);
	if (status != 0)
		return status;

	/* Succeeded: the distance. */
	return 0;
}

/* Reports how far an element is scrolled across, the document's for the root (scrollLeft). */
int
bind_scroll_left(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The measure. */
	status = geometry_measure(realm, this_value, GEOMETRY_SCROLL_LEFT, result);
	if (status != 0)
		return status;

	/* Succeeded: the distance. */
	return 0;
}

/*
 * Takes a new scroll position for an element (the setter of scrollTop and
 * scrollLeft), which is ignored: boxes do not scroll in this pass, and the
 * document is scrolled by its viewer.
 */
int
bind_scroll_position_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	double position;
	double scroll_x;
	double scroll_y;
	int status;

	/* The element, and the number, whose conversion may throw. */
	*result = VM_VALUE_UNDEFINED;
	status = geometry_element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = vm_to_number(realm, js_argument(args, count, 0), &position);
	if (status != 0)
		return status;

	/* The root scrolls with the document; another element's content scrolls where it can. */
	window = bind_window_of(realm);
	if (geometry_is_root(window, element)) {
		geometry_scroll(window, &scroll_x, &scroll_y);
		geometry_scroll_move(window, geometry_finite(position), scroll_y);
		return 0;
	}
	geometry_element_scroll(window, element, position, 1, 0.0, 0);

	/* Succeeded: the content is scrolled. */
	return 0;
}

/* Reports the width of an element's border box (offsetWidth). */
int
bind_offset_width(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The measure. */
	status = geometry_measure(realm, this_value, GEOMETRY_OFFSET_WIDTH, result);
	if (status != 0)
		return status;

	/* Succeeded: the width. */
	return 0;
}

/* Reports the height of an element's border box (offsetHeight). */
int
bind_offset_height(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The measure. */
	status = geometry_measure(realm, this_value, GEOMETRY_OFFSET_HEIGHT, result);
	if (status != 0)
		return status;

	/* Succeeded: the height. */
	return 0;
}

/*
 * Takes a new scroll position for the root element (the setter of
 * scrollTop), which scrolls the document; any other element's is ignored.
 */
int
bind_scroll_top_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	double position;
	double scroll_x;
	double scroll_y;
	int root;
	int status;

	/* The element, and the number, whose conversion may throw. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	status = geometry_element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = vm_to_number(realm, js_argument(args, count, 0), &position);
	if (status != 0)
		return status;

	/* The root scrolls with the document; another element's content scrolls where it can. */
	root = geometry_is_root(window, element);
	if (!root) {
		geometry_element_scroll(window, element, 0.0, 0, position, 1);
		return 0;
	}

	/* Succeeded: the document scrolls down to it. */
	geometry_scroll(window, &scroll_x, &scroll_y);
	geometry_scroll_move(window, scroll_x, geometry_finite(position));
	return 0;
}

/*
 * Reports the element an element's offsetTop and offsetLeft are measured
 * from, or null (offsetParent).
 */
int
bind_offset_parent(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	struct dom_element *parent;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element and its offset parent. */
	window = bind_window_of(realm);
	status = geometry_element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = geometry_offset_parent(window, element, &parent);
	if (status != 0)
		return status;

	/* None is null. */
	if (parent == NULL) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* Succeeded: its object. */
	status = bind_wrap(window, &parent->node, result);
	if (status != 0)
		return status;
	return 0;
}

/* Reports how far an element's border box is below its offset parent's padding box (offsetTop). */
int
bind_offset_top(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The distance down. */
	status = geometry_offset(realm, this_value, 1, result);
	if (status != 0)
		return status;

	/* Succeeded: the distance. */
	return 0;
}

/* Reports how far an element's border box is right of its offset parent's padding box (offsetLeft). */
int
bind_offset_left(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The distance across. */
	status = geometry_offset(realm, this_value, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the distance. */
	return 0;
}

/*
 * Scrolls the document to a place: two numbers (across, down) or a
 * dictionary with left and top (window.scrollTo and window.scroll).
 */
int
bind_window_scroll_to(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	double scroll_x;
	double scroll_y;
	double x;
	double y;
	int given_x;
	int given_y;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The place; a coordinate not given stays where it is. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	status = geometry_scroll_arguments(realm, args, count, &x, &y, &given_x, &given_y);
	if (status != 0)
		return status;
	geometry_scroll(window, &scroll_x, &scroll_y);
	if (!given_x)
		x = scroll_x;
	if (!given_y)
		y = scroll_y;

	/* Succeeded: the document scrolls there. */
	geometry_scroll_move(window, x, y);
	return 0;
}

/*
 * Scrolls the document by a distance: two numbers (across, down) or a
 * dictionary with left and top (window.scrollBy).
 */
int
bind_window_scroll_by(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	double scroll_x;
	double scroll_y;
	double x;
	double y;
	int given_x;
	int given_y;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The distance; a coordinate not given does not move. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	status = geometry_scroll_arguments(realm, args, count, &x, &y, &given_x, &given_y);
	if (status != 0)
		return status;
	if (!given_x)
		x = 0.0;
	if (!given_y)
		y = 0.0;

	/* Succeeded: the document scrolls by it. */
	geometry_scroll(window, &scroll_x, &scroll_y);
	geometry_scroll_move(window, scroll_x + x, scroll_y + y);
	return 0;
}

/*
 * Scrolls an element's content to a place (Element's scrollTo and scroll):
 * the root element's is the document's; other boxes do not scroll.
 */
int
bind_element_scroll_to(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	double x;
	double y;
	int given_x;
	int given_y;
	int root;
	int status;

	/* The element and the place, whose conversion may throw. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	status = geometry_element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = geometry_scroll_arguments(realm, args, count, &x, &y, &given_x, &given_y);
	if (status != 0)
		return status;

	/* The root scrolls with the document; another element's content scrolls where it can. */
	root = geometry_is_root(window, element);
	if (!root) {
		geometry_element_scroll(window, element, x, given_x, y, given_y);
		return 0;
	}

	/* Succeeded: the document scrolls. */
	status = bind_window_scroll_to(realm, VM_VALUE_UNDEFINED, args, count, result);
	return status;
}

/*
 * Scrolls an element's content by a distance (Element's scrollBy): the
 * root element's is the document's; other boxes do not scroll.
 */
int
bind_element_scroll_by(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	double x;
	double y;
	int given_x;
	int given_y;
	int root;
	int status;

	/* The element and the distance, whose conversion may throw. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	status = geometry_element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = geometry_scroll_arguments(realm, args, count, &x, &y, &given_x, &given_y);
	if (status != 0)
		return status;

	/* The root scrolls with the document; another element's content scrolls where it can. */
	root = geometry_is_root(window, element);
	if (!root) {
		geometry_element_scroll(window, element, element->scroll_x + x, given_x, element->scroll_y + y, given_y);
		return 0;
	}

	/* Succeeded: the document scrolls. */
	status = bind_window_scroll_by(realm, VM_VALUE_UNDEFINED, args, count, result);
	return status;
}

/* The alignments scrollIntoView names. */
enum {
	ALIGN_START,
	ALIGN_END,
	ALIGN_CENTER,
	ALIGN_NEAREST
};

/* Reads a scrollIntoView alignment option ("start", "end", "center" or "nearest"); the default stays when it is absent. */
static int
geometry_align_option(
	struct vm_realm *realm,
	vm_value options,
	const char *name,
	int *align)
{
	struct vm_string *text;
	vm_value value;
	int present;
	int status;

	status = bind_get_option(realm, options, name, &present, &value);
	if (status != 0)
		return status;
	if (!present)
		return 0;
	status = bind_to_string(realm, value, &text);
	if (status != 0)
		return status;
	if (vm_string_equal_ascii(text, "start"))
		*align = ALIGN_START;
	else if (vm_string_equal_ascii(text, "end"))
		*align = ALIGN_END;
	else if (vm_string_equal_ascii(text, "center"))
		*align = ALIGN_CENTER;
	else if (vm_string_equal_ascii(text, "nearest"))
		*align = ALIGN_NEAREST;
	return 0;
}

/*
 * Finds how far a scroll container moves to put a target's span [t0, t1] where an alignment says in its scrollport [s0, s1]
 * (flip: the axis starts at the larger side).  The container's position grows by the result.
 */
static double
geometry_align_distance(
	int align,
	int flip,
	double t0,
	double t1,
	double s0,
	double s1)
{
	if (align == ALIGN_NEAREST) {
		/* Already filling or inside the port stays; otherwise the nearer edge. */
		if ((t0 < s0 && t1 > s1) || (t0 >= s0 && t1 <= s1))
			return 0.0;
		if ((t0 < s0 && t1 - t0 < s1 - s0) || (t1 > s1 && t1 - t0 > s1 - s0))
			return t0 - s0;
		return t1 - s1;
	}
	if (align == ALIGN_CENTER)
		return (t0 + t1) / 2.0 - (s0 + s1) / 2.0;
	if ((align == ALIGN_START) != (flip != 0))
		return t0 - s0;
	return t1 - s1;
}

/*
 * Scrolls the scroll containers around an element, then the document, so the element is in view (scrollIntoView):
 * the block edge named at "start" (the default; true), "end" (false), "center" or "nearest", and the inline edge likewise
 * (nearest by default).  An element without a box does not move anything.
 */
int
bind_scroll_into_view(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	struct dom_element *scroller;
	const struct dom_node *walk;
	struct bind_box box;
	struct bind_box port;
	vm_value argument;
	double scroll_x;
	double scroll_y;
	double y;
	double dx;
	double dy;
	int block_align;
	int inline_align;
	int found;
	int status;

	/* The element and where its edges go. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	status = geometry_element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	argument = js_argument(args, count, 0);
	block_align = ALIGN_START;
	inline_align = ALIGN_NEAREST;
	if (vm_value_is_object(argument)) {
		status = geometry_align_option(realm, argument, "block", &block_align);
		if (status == 0)
			status = geometry_align_option(realm, argument, "inline", &inline_align);
		if (status != 0)
			return status;
	} else if (argument == VM_VALUE_FALSE) {
		block_align = ALIGN_END;
	}

	/* Nothing moves for an element without a box. */
	found = geometry_box(window, element, &box);
	if (!found)
		return 0;

	/* The scroll containers around it, nearest first (the root and the body scroll with the document). */
	for (walk = dom_flat_parent(&element->node); walk != NULL; walk = dom_flat_parent(walk)) {
		if (walk->type != DOM_ELEMENT)
			continue;
		scroller = (struct dom_element *)walk;
		if (walk->parent == &window->document->node || (walk->parent != NULL && walk->parent->parent == &window->document->node))
			continue;
		if (!geometry_box(window, scroller, &port) || !port.block || !port.scrolls)
			continue;
		found = geometry_box(window, element, &box);
		if (!found)
			return 0;

		/* The block axis is the vertical one in a horizontal writing mode. */
		dx = geometry_align_distance(port.scroll_vertical ? block_align : inline_align, port.scroll_flip_x, box.x, box.x + box.width,
		    port.x + port.border_left, port.x + port.width - port.border_right);
		dy = geometry_align_distance(port.scroll_vertical ? inline_align : block_align, port.scroll_flip_y, box.y, box.y + box.height,
		    port.y + port.border_top, port.y + port.height - port.border_bottom);
		geometry_element_scroll(window, scroller, scroller->scroll_x + dx, 1, scroller->scroll_y + dy, 1);
	}

	/* The document scrolls down to the block edge. */
	found = geometry_box(window, element, &box);
	if (!found)
		return 0;
	geometry_scroll(window, &scroll_x, &scroll_y);
	y = scroll_y + geometry_align_distance(block_align, 0, box.y, box.y + box.height, scroll_y, scroll_y + window->viewport_height);

	/* Succeeded: the document scrolls there. */
	geometry_scroll_move(window, scroll_x, y);
	return 0;
}

/* Reports how far the document is scrolled across (scrollX and pageXOffset). */
int
bind_window_scroll_x(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	double scroll_x;
	double scroll_y;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document's scroll. */
	window = bind_window_of(realm);
	geometry_scroll(window, &scroll_x, &scroll_y);

	/* Succeeded: across. */
	*result = vm_value_number(scroll_x);
	return 0;
}

/* Reports how far the document is scrolled down (scrollY and pageYOffset). */
int
bind_window_scroll_y(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	double scroll_x;
	double scroll_y;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document's scroll. */
	window = bind_window_of(realm);
	geometry_scroll(window, &scroll_x, &scroll_y);

	/* Succeeded: down. */
	*result = vm_value_number(scroll_y);
	return 0;
}

/*
 * Reports one of an element's client, scroll or offset measures, in whole
 * pixels as other browsers report them.
 */
static int
geometry_measure(
	struct vm_realm *realm,
	vm_value this_value,
	int part,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	struct bind_box box;
	double document_width;
	double document_height;
	double scroll_x;
	double scroll_y;
	double value;
	int found;
	int root;
	int status;

	/* The element, where it is, and whether it is the root, whose client area is the viewport. */
	window = bind_window_of(realm);
	status = geometry_element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	found = geometry_box(window, element, &box);
	root = geometry_is_root(window, element);

	/* The document's size and scroll, which the root reports. */
	document_width = 0.0;
	document_height = 0.0;
	if (root && window->host.document_size != NULL)
		window->host.document_size(window->host.context, &document_width, &document_height);
	geometry_scroll(window, &scroll_x, &scroll_y);

	/* Chooses the measure. */
	value = 0.0;
	switch (part) {
	case GEOMETRY_CLIENT_WIDTH:
	case GEOMETRY_SCROLL_WIDTH:
		/* The padding box's width, the viewport's for the root, and nothing for an inline box. */
		if (root) {
			value = window->viewport_width;
		} else if (found && box.block) {
			value = box.width - box.border_left - box.border_right;
		}

		/* The root scrolls over the document when it is wider; another block over its content's overflow. */
		if (part == GEOMETRY_SCROLL_WIDTH && root && document_width > value)
			value = document_width;
		if (part == GEOMETRY_SCROLL_WIDTH && !root && found && box.block && box.scroll_width > value)
			value = box.scroll_width;
		break;
	case GEOMETRY_CLIENT_HEIGHT:
	case GEOMETRY_SCROLL_HEIGHT:
		/* The padding box's height, the viewport's for the root, and nothing for an inline box. */
		if (root) {
			value = window->viewport_height;
		} else if (found && box.block) {
			value = box.height - box.border_top - box.border_bottom;
		}

		/* The root scrolls over the document when it is taller. */
		if (part == GEOMETRY_SCROLL_HEIGHT && root && document_height > value)
			value = document_height;
		if (part == GEOMETRY_SCROLL_HEIGHT && !root && found && box.block && box.scroll_height > value)
			value = box.scroll_height;
		break;
	case GEOMETRY_CLIENT_TOP:
		/* The top border of a block. */
		if (found && box.block)
			value = box.border_top;
		break;
	case GEOMETRY_CLIENT_LEFT:
		/* The left border of a block. */
		if (found && box.block)
			value = box.border_left;
		break;
	case GEOMETRY_SCROLL_TOP:
		/* The root scrolls with the document; another element by what it was scrolled to. */
		if (root)
			value = scroll_y;
		else if (found && box.block)
			value = element->scroll_y;
		break;
	case GEOMETRY_SCROLL_LEFT:
		/* The root scrolls with the document; another element by what it was scrolled to. */
		if (root)
			value = scroll_x;
		else if (found && box.block)
			value = element->scroll_x;
		break;
	case GEOMETRY_OFFSET_WIDTH:
		/* The border box's width. */
		if (found)
			value = box.width;
		break;
	default:
		/* The border box's height. */
		if (found)
			value = box.height;
		break;
	}

	/* Succeeded: the measure in whole pixels. */
	*result = vm_value_number(geometry_round(value));
	return 0;
}

/* Finds the element a geometry method's this value stands for, throwing a TypeError otherwise. */
static int
geometry_element_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_element **element)
{
	struct dom_node *node;
	int status;

	/* The node, which must be an element. */
	node = bind_node_of(this_value);
	if (node == NULL || node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the element. */
	*element = (struct dom_element *)node;
	return 0;
}

/*
 * Asks the host where an element is on the page; reports whether it has
 * a box (without one, the box is all zero).
 */
static int
geometry_box(
	struct bind_window *window,
	struct dom_element *element,
	struct bind_box *box)
{
	int found;

	/* Nothing until the host says. */
	memset(box, 0, sizeof(*box));
	if (window->host.node_box == NULL)
		return 0;

	/* The host's answer; a node without a box leaves it zero. */
	found = window->host.node_box(window->host.context, &element->node, box);
	if (!found) {
		memset(box, 0, sizeof(*box));
		return 0;
	}

	/* Succeeded: the element has a box. */
	return 1;
}

/* Tells whether an element is the document's root element (the html element). */
static int
geometry_is_root(
	const struct bind_window *window,
	const struct dom_element *element)
{
	/* The root is the document's child. */
	if (element->node.parent != &window->document->node)
		return 0;

	/* Succeeded: it is the root. */
	return 1;
}

/* Puts a number inside a range. */
static void
geometry_clamp(
	double *value,
	double low,
	double high)
{
	if (*value < low)
		*value = low;
	if (*value > high)
		*value = high;
}

/*
 * Scrolls an element's content to a place on the axes given (its scroll container's range: from zero to its scroll size
 * less its client size; a box that does not scroll stays), and has the page laid out again when it moved.
 */
static void
geometry_element_scroll(
	struct bind_window *window,
	struct dom_element *element,
	double x,
	int given_x,
	double y,
	int given_y)
{
	struct bind_box box;
	double next_x;
	double next_y;
	double range_x;
	double range_y;

	/* Only a block that clips its overflow scrolls. */
	if (!geometry_box(window, element, &box) || !box.block || !box.scrolls)
		return;
	range_x = box.scroll_width - (box.width - box.border_left - box.border_right);
	range_y = box.scroll_height - (box.height - box.border_top - box.border_bottom);
	if (range_x < 0.0)
		range_x = 0.0;
	if (range_y < 0.0)
		range_y = 0.0;
	next_x = element->scroll_x;
	next_y = element->scroll_y;
	if (given_x)
		next_x = geometry_finite(x);
	if (given_y)
		next_y = geometry_finite(y);
	/* The range runs from the scroll origin: nonpositive when the origin is at the right or bottom edge. */
	geometry_clamp(&next_x, box.scroll_flip_x ? -range_x : 0.0, box.scroll_flip_x ? 0.0 : range_x);
	geometry_clamp(&next_y, box.scroll_flip_y ? -range_y : 0.0, box.scroll_flip_y ? 0.0 : range_y);
	if (next_x == element->scroll_x && next_y == element->scroll_y)
		return;
	element->scroll_x = next_x;
	element->scroll_y = next_y;
	element->node.document->generation++;
}

/* Finds how far the document is scrolled, in CSS pixels (0 when the host does not say). */
static void
geometry_scroll(
	const struct bind_window *window,
	double *x,
	double *y)
{
	/* Nothing until the host says. */
	*x = 0.0;
	*y = 0.0;

	/* The host's scroll. */
	if (window->host.scroll != NULL)
		window->host.scroll(window->host.context, x, y);
}

/* Rounds a measure to whole pixels, halves up. */
static double
geometry_round(
	double value)
{
	/* The nearest whole number. */
	return floor(value + 0.5);
}

/* Makes a DOMRect with a place and a size. */
static int
geometry_rect_create(
	struct bind_window *window,
	double x,
	double y,
	double width,
	double height,
	vm_value *value)
{
	struct geometry_rect *rect;
	struct vm_object *object;

	/* The state. */
	rect = vm_heap_alloc(window->realm->heap, &geometry_rect_type, sizeof(*rect));
	if (rect == NULL)
		return ENOMEM;
	rect->x = x;
	rect->y = y;
	rect->width = width;
	rect->height = height;

	/* The object with DOMRect's prototype. */
	object = vm_object_create(window->realm->heap, window->prototypes[BIND_DOM_RECT]);
	if (object == NULL)
		return ENOMEM;
	object->kind = VM_KIND_PLATFORM;
	object->internal = vm_value_cell(rect);

	/* Succeeded: the rectangle. */
	*value = vm_value_cell(object);
	return 0;
}

/* Finds the state of the DOMRect a value is, throwing a TypeError otherwise. */
static int
geometry_rect_of(
	struct vm_realm *realm,
	vm_value value,
	struct geometry_rect **rect)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int is_object;
	int is_cell;
	int status;

	/* A platform object. */
	is_object = vm_value_is_object(value);
	if (!is_object) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* One the binding made. */
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind != VM_KIND_PLATFORM) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Whose cell is a rectangle's state. */
	is_cell = vm_value_is_cell(object->internal);
	if (!is_cell) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* A cell of a rectangle's type. */
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &geometry_rect_type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the state. */
	*rect = (struct geometry_rect *)cell;
	return 0;
}

/* Makes a DOMRect from new DOMRect(x, y, width, height), each 0 when not given. */
static int
geometry_rect_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	double numbers[4];
	unsigned index;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Each of the four numbers. */
	window = bind_window_of(realm);
	for (index = 0; index < 4U; index++) {
		numbers[index] = 0.0;
		if (index >= count)
			continue;

		/* The argument as a number. */
		status = vm_to_number(realm, args[index], &numbers[index]);
		if (status != 0)
			return status;
	}

	/* Succeeded: the rectangle. */
	status = geometry_rect_create(window, numbers[0], numbers[1], numbers[2], numbers[3], result);
	if (status != 0)
		return status;
	return 0;
}

/* Reports the part of a DOMRect the getter knows. */
static int
geometry_rect_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct geometry_rect *rect;
	struct vm_function *callee;
	int part;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The rectangle and the part. */
	status = geometry_rect_of(realm, this_value, &rect);
	if (status != 0)
		return status;
	callee = js_builtin_callee(realm);
	part = vm_value_as_int32(callee->data);

	/* Succeeded: the part's number. */
	*result = vm_value_number(geometry_rect_value(rect, part));
	return 0;
}

/* Sets the place or the size of a DOMRect (x, y, width and height). */
static int
geometry_rect_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct geometry_rect *rect;
	struct vm_function *callee;
	double number;
	int part;
	int status;

	/* The rectangle, the part and the number. */
	*result = VM_VALUE_UNDEFINED;
	status = geometry_rect_of(realm, this_value, &rect);
	if (status != 0)
		return status;
	callee = js_builtin_callee(realm);
	part = vm_value_as_int32(callee->data);
	status = vm_to_number(realm, js_argument(args, count, 0), &number);
	if (status != 0)
		return status;

	/* Chooses the field the part is. */
	switch (part) {
	case GEOMETRY_RECT_X:
		rect->x = number;
		break;
	case GEOMETRY_RECT_Y:
		rect->y = number;
		break;
	case GEOMETRY_RECT_WIDTH:
		rect->width = number;
		break;
	default:
		rect->height = number;
		break;
	}

	/* Succeeded: the rectangle is changed. */
	return 0;
}

/* Makes a plain object of a DOMRect's eight numbers (toJSON). */
static int
geometry_rect_to_json(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	const struct geometry_rect_member *member;
	struct geometry_rect *rect;
	struct vm_object *object;
	double number;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The rectangle and a plain object. */
	status = geometry_rect_of(realm, this_value, &rect);
	if (status != 0)
		return status;
	object = vm_object_create(realm->heap, realm->object_prototype);
	if (object == NULL)
		return ENOMEM;

	/* Each member, as a data property. */
	for (member = geometry_rect_members; member->name != NULL; member++) {
		number = geometry_rect_value(rect, member->part);
		status = js_builtin_value(realm, object, member->name, vm_value_number(number), VM_PROPERTY_DEFAULT);
		if (status != 0)
			return status;
	}

	/* Succeeded: the object. */
	*result = vm_value_cell(object);
	return 0;
}

/* Reports one part of a rectangle: its place, its size or one of its edges. */
static double
geometry_rect_value(
	const struct geometry_rect *rect,
	int part)
{
	/* Chooses the part. */
	switch (part) {
	case GEOMETRY_RECT_X:
		return rect->x;
	case GEOMETRY_RECT_Y:
		return rect->y;
	case GEOMETRY_RECT_WIDTH:
		return rect->width;
	case GEOMETRY_RECT_HEIGHT:
		return rect->height;
	case GEOMETRY_RECT_TOP:
		return fmin(rect->y, rect->y + rect->height);
	case GEOMETRY_RECT_RIGHT:
		return fmax(rect->x, rect->x + rect->width);
	case GEOMETRY_RECT_BOTTOM:
		return fmax(rect->y, rect->y + rect->height);
	default:
		break;
	}

	/* The left edge. */
	return fmin(rect->x, rect->x + rect->width);
}

/*
 * Finds an element's computed position (static when the host cannot say:
 * an element outside the document).
 */
static int
geometry_position(
	struct bind_window *window,
	struct dom_element *element,
	int *position)
{
	struct css_style *style;
	int error;

	/* Static until the host says. */
	*position = CSS_POSITION_STATIC;
	if (window->host.computed_style == NULL)
		return 0;

	/* The element's style (on the heap: it is large). */
	style = malloc(sizeof(*style));
	if (style == NULL)
		return ENOMEM;
	error = window->host.computed_style(window->host.context, element, style);
	if (error == 0) {
		*position = style->position;

		/* A transformed element is the containing block of what is positioned inside it: as good as positioned here. */
		if (*position == CSS_POSITION_STATIC && style->transform_set)
			*position = CSS_POSITION_RELATIVE;
	}
	free(style);

	/* An element outside the document is static. */
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;

	/* Succeeded: the position. */
	return 0;
}

/* Tells whether an element is the containing block of the fixed boxes inside it (a transform, a backdrop filter, will-change). */
static int
geometry_fixed_container(
	struct bind_window *window,
	struct dom_element *element,
	int *contains)
{
	struct css_style *style;
	int error;

	*contains = 0;
	if (window->host.computed_style == NULL)
		return 0;
	style = malloc(sizeof(*style));
	if (style == NULL)
		return ENOMEM;
	error = window->host.computed_style(window->host.context, element, style);
	if (error == 0)
		*contains = style->transform_set || style->backdrop_filter_count != 0 || (style->will_change_contains & 1) != 0;
	free(style);
	if (error == ENOENT)
		return 0;
	return error;
}

/*
 * Finds an element's offset parent (CSSOM View): none for the root, the
 * body, an element without a box or a fixed one; otherwise its nearest
 * ancestor that is positioned, the body, or a td, th or table element of
 * static position.
 */
static int
geometry_offset_parent(
	struct bind_window *window,
	struct dom_element *element,
	struct dom_element **parent)
{
	struct dom_element *ancestor;
	struct dom_node *walk;
	struct bind_box box;
	int position;
	int found;
	int root;
	int body;
	int stops;
	int error;

	/* The root and the body have none. */
	*parent = NULL;
	root = geometry_is_root(window, element);
	if (root)
		return 0;
	body = dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_BODY);
	if (body)
		return 0;

	/* Nor does an element without a box. */
	found = geometry_box(window, element, &box);
	if (!found)
		return 0;

	/* Nor a fixed one. */
	error = geometry_position(window, element, &position);
	if (error != 0)
		return error;
	if (position == CSS_POSITION_FIXED) {
		/* Unless a transformed ancestor is the containing block of what is fixed inside it. */
		for (walk = dom_flat_parent(&element->node); walk != NULL && walk->type == DOM_ELEMENT; walk = dom_flat_parent(walk)) {
			ancestor = (struct dom_element *)walk;
			error = geometry_fixed_container(window, ancestor, &stops);
			if (error != 0)
				return error;
			if (stops) {
				*parent = ancestor;
				return 0;
			}
		}
		return 0;
	}

	/* The nearest ancestor that is one. */
	for (walk = dom_flat_parent(&element->node); walk != NULL && walk->type == DOM_ELEMENT; walk = dom_flat_parent(walk)) {
		ancestor = (struct dom_element *)walk;
		error = geometry_position(window, ancestor, &position);
		if (error != 0)
			return error;

		/* A positioned one. */
		if (position != CSS_POSITION_STATIC) {
			*parent = ancestor;
			return 0;
		}

		/* The body, a table cell or a table. */
		stops = geometry_stops_offset(walk);
		if (stops) {
			*parent = ancestor;
			return 0;
		}
	}

	/* None. */
	return 0;
}

/*
 * Reports how far an element's border box is from its offset parent's
 * padding box (from the document's origin when the parent is none or the
 * body), down or across, in whole pixels.
 */
static int
geometry_offset(
	struct vm_realm *realm,
	vm_value this_value,
	int top,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	struct dom_element *parent;
	struct bind_box parent_box;
	struct bind_box box;
	double value;
	int body;
	int found;
	int status;

	/* The element and its box; without one it is 0. */
	window = bind_window_of(realm);
	status = geometry_element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	found = geometry_box(window, element, &box);
	if (!found) {
		*result = vm_value_int32(0);
		return 0;
	}

	/* The offset parent. */
	status = geometry_offset_parent(window, element, &parent);
	if (status != 0)
		return status;

	/* From the document's origin, or from the parent's padding box. */
	value = box.x;
	if (top)
		value = box.y;
	body = 0;
	if (parent != NULL)
		body = dom_element_is(&parent->node, DOM_NS_HTML, DOM_TAG_BODY);
	if (parent != NULL && !body) {
		geometry_box(window, parent, &parent_box);
		if (top) {
			value -= parent_box.y + parent_box.border_top;
		} else {
			value -= parent_box.x + parent_box.border_left;
		}
	}

	/* Succeeded: in whole pixels. */
	*result = vm_value_number(geometry_round(value));
	return 0;
}

/*
 * Reads the arguments of the scroll methods: a dictionary with left and
 * top, or two numbers (across, down); says which were given.
 */
static int
geometry_scroll_arguments(
	struct vm_realm *realm,
	const vm_value *args,
	unsigned count,
	double *x,
	double *y,
	int *given_x,
	int *given_y)
{
	vm_value first;
	vm_value value;
	int is_object;
	int present;
	int status;

	/* Nothing given yet. */
	*x = 0.0;
	*y = 0.0;
	*given_x = 0;
	*given_y = 0;
	first = js_argument(args, count, 0);

	/* A dictionary's left and top. */
	is_object = vm_value_is_object(first);
	if (is_object || count == 0 || first == VM_VALUE_UNDEFINED) {
		status = bind_get_option(realm, first, "left", &present, &value);
		if (status != 0)
			return status;
		if (present) {
			status = vm_to_number(realm, value, x);
			if (status != 0)
				return status;
			*given_x = 1;
		}

		/* Then its top. */
		status = bind_get_option(realm, first, "top", &present, &value);
		if (status != 0)
			return status;
		if (present) {
			status = vm_to_number(realm, value, y);
			if (status != 0)
				return status;
			*given_y = 1;
		}

		/* The coordinates as numbers that are finite. */
		*x = geometry_finite(*x);
		*y = geometry_finite(*y);
		return 0;
	}

	/* Two numbers. */
	status = vm_to_number(realm, first, x);
	if (status != 0)
		return status;
	status = vm_to_number(realm, js_argument(args, count, 1), y);
	if (status != 0)
		return status;

	/* Succeeded: both, as numbers that are finite. */
	*x = geometry_finite(*x);
	*y = geometry_finite(*y);
	*given_x = 1;
	*given_y = 1;
	return 0;
}

/* Asks the host to scroll the document to a place (it keeps the place inside the document). */
static void
geometry_scroll_move(
	struct bind_window *window,
	double x,
	double y)
{
	/* Nothing without a host that scrolls. */
	if (window->host.scroll_to == NULL)
		return;

	/* The host's scroll. */
	window->host.scroll_to(window->host.context, x, y);
}

/* Makes a number that is not finite 0, as the scroll methods take them. */
static double
geometry_finite(
	double value)
{
	/* NaN and the infinities are 0. */
	if (value != value)
		return 0.0;
	if (value > 1e300 || value < -1e300)
		return 0.0;

	/* Succeeded: the number. */
	return value;
}

/* Tells whether a static ancestor is an offset parent: the body, a td, a th or a table element. */
static int
geometry_stops_offset(
	const struct dom_node *node)
{
	static const int tags[] = { DOM_TAG_BODY, DOM_TAG_TD, DOM_TAG_TH, DOM_TAG_TABLE };
	size_t index;
	int is;

	/* Each of the elements. */
	for (index = 0; index < sizeof(tags) / sizeof(tags[0]); index++) {
		is = dom_element_is(node, DOM_NS_HTML, tags[index]);
		if (is)
			return 1;
	}

	/* Any other element is not. */
	return 0;
}
