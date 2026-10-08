/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The user's input as the page's DOM events, and the focus (ws074-p056).
 *
 * A pointer event goes to the element under the point (the root element
 * where there is none); a key event to the focused element, or the body
 * when nothing is focused.  The view (view/view.c) does the default
 * actions of the events the page's scripts leave uncanceled; the page
 * keeps what they need: the focused element, moved in the sequential
 * focus order by Tab (the elements with a tabindex of 1 or more in its
 * order, then the others that can be focused in the document's order) or
 * to the element a press lands on, and the element an Enter activates.
 *
 * The focused element's ring is drawn when the keyboard moved the focus
 * there and the view has its program's focus: page_paint adds it on top of
 * the display list, so the CPU and the GPU renderers draw it alike.
 */

#include "page/page.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The deepest element nesting walked (the parser caps nesting too). */
#define INPUT_DEPTH		512

/* The most elements the sequential focus order holds (the rest cannot be reached with Tab). */
#define INPUT_FOCUS_MAX		4096U

/* The focus ring: its color (0xAARRGGBB), its thickness and its gap from the element, in pixels. */
#define INPUT_RING_COLOR	0xFF1A73E8U
#define INPUT_RING_WIDTH	2
#define INPUT_RING_GAP		1

/* What an element's tabindex says of it: no tabindex (or one that is not a number), or a number. */
#define INPUT_TABINDEX_NONE	0
#define INPUT_TABINDEX_SET	1

/*
 * One element of the sequential focus order: the element and its place in
 * the order (its tabindex when that is 1 or more, else 0), with its place
 * in the document to keep ties in the document's order.
 */
struct input_stop {
	struct dom_element *element;
	long order;
	size_t position;
};

static struct dom_node *input_element_at(struct page *page, int x, int y);
static struct dom_node *input_key_target(struct page *page);
static struct dom_element *input_focused(struct page *page);
static int input_set_focus(struct page *page, struct dom_element *element, int visible);
static int input_focusable(struct page *page, const struct dom_element *element, long *order);
static int input_tabindex(struct page *page, const struct dom_element *element, long *value);
static int input_has_attribute(struct page *page, const struct dom_element *element, const char *name, struct dom_attribute **attribute);
static int input_equal_folded(const struct vm_string *string, const char *ascii);
static int input_has_token(const struct vm_string *string, const char *token);
static int input_rendered(struct page *page, const struct dom_node *node);
static struct dom_node *input_next(const struct dom_node *node, const struct dom_node *root);
static int input_link_of(struct page *page, const struct dom_node *node, struct wb_buffer *href, int *found);
static int input_is_button(const struct dom_node *node);
static int input_compare_stops(const struct input_stop *left, const struct input_stop *right);
static int input_activate_control(struct page *page, struct dom_element *element, struct wb_buffer *href, int *found);

/*
 * Fires a mouse event of a type (mousedown, mouseup, click) at the element
 * under a point of the page; *canceled says whether a listener canceled
 * its default action.
 */
int
page_mouse_event(
	struct page *page,
	const char *type,
	const struct page_pointer *pointer,
	int *canceled)
{
	struct bind_mouse mouse;
	struct dom_node *target;
	int error;

	/* The element under the point, or the document's root element. */
	*canceled = 0;
	target = input_element_at(page, pointer->x, pointer->y);
	if (target == NULL)
		return 0;

	/* The event, with the pointer's place, the button and the modifiers. */
	memset(&mouse, 0, sizeof(mouse));
	mouse.client_x = (double)pointer->client_x;
	mouse.client_y = (double)pointer->client_y;
	mouse.page_x = (double)pointer->x;
	mouse.page_y = (double)pointer->y;
	mouse.button = pointer->button;
	mouse.modifiers = pointer->modifiers;
	error = bind_fire_mouse_event(page->window, target, type, &mouse, canceled);
	if (error != 0)
		return error;

	/* Succeeded: the event is dispatched. */
	return 0;
}

/*
 * Fires a wheel event at the element under a point of the page; *canceled
 * says whether a listener canceled the scroll.
 */
int
page_wheel_event(
	struct page *page,
	const struct page_pointer *pointer,
	int *canceled)
{
	struct bind_mouse mouse;
	struct dom_node *target;
	int error;

	/* The element under the point, or the document's root element. */
	*canceled = 0;
	target = input_element_at(page, pointer->x, pointer->y);
	if (target == NULL)
		return 0;

	/* The event, with the pointer's place, the modifiers and the distances. */
	memset(&mouse, 0, sizeof(mouse));
	mouse.client_x = (double)pointer->client_x;
	mouse.client_y = (double)pointer->client_y;
	mouse.page_x = (double)pointer->x;
	mouse.page_y = (double)pointer->y;
	mouse.modifiers = pointer->modifiers;
	mouse.delta_x = pointer->delta_x;
	mouse.delta_y = pointer->delta_y;
	error = bind_fire_wheel_event(page->window, target, &mouse, canceled);
	if (error != 0)
		return error;

	/* Succeeded: the event is dispatched. */
	return 0;
}

/*
 * Fires a key event of a type (keydown, keyup, keypress) at the focused
 * element, or the body; *canceled says whether a listener canceled its
 * default action.
 */
int
page_key_event(
	struct page *page,
	const char *type,
	const struct bind_key *key,
	int *canceled)
{
	struct dom_node *target;
	int error;

	/* The focused element, or the body, or the root element. */
	*canceled = 0;
	target = input_key_target(page);
	if (target == NULL)
		return 0;

	/* The event. */
	error = bind_fire_key_event(page->window, target, type, key, canceled);
	if (error != 0)
		return error;

	/* Succeeded: the event is dispatched. */
	return 0;
}

/*
 * Moves the focus to what a press at a point of the page lands on (the
 * default action of a mousedown): the nearest element around it that can
 * be focused, without a ring, or nothing when there is none.
 */
int
page_focus_at(
	struct page *page,
	int x,
	int y)
{
	struct dom_node *node;
	long order;
	int depth;
	int focusable;
	int editing;
	int error;

	/* The element under the point, then its ancestors. */
	node = input_element_at(page, x, y);
	for (depth = 0; node != NULL && depth < INPUT_DEPTH; depth++) {
		/* The first element that can be focused takes the focus. */
		if (node->type == DOM_ELEMENT) {
			focusable = input_focusable(page, (struct dom_element *)node, &order);
			if (focusable)
				break;
		}

		/* Otherwise its parent. */
		node = node->parent;
	}

	/* An element was found, or nothing takes the focus. */
	if (depth == INPUT_DEPTH)
		node = NULL;
	error = input_set_focus(page, (struct dom_element *)node, 0);
	if (error != 0)
		return error;

	/*
	 * A text control shows its ring whichever way it got the focus (as
	 * :focus-visible matches it in Chromium), and its caret goes where the
	 * press was.
	 */
	editing = page_is_editing(page);
	if (editing) {
		page->focus_visible = 1;
		page->focus_generation++;
		error = page_place_caret(page, x);
		if (error != 0)
			return error;
	}

	/* Succeeded: the focus is where the press landed. */
	return 0;
}

/*
 * Moves the focus to the next element of the sequential focus order, or
 * the previous one (Tab and Shift+Tab), with its ring.  Past either end
 * nothing is focused, and the next move starts again from that end.
 */
int
page_focus_move(
	struct page *page,
	int backward)
{
	struct input_stop *stops;
	struct input_stop stop;
	struct dom_element *current;
	struct dom_node *node;
	struct dom_element *element;
	struct dom_element *chosen;
	size_t count;
	size_t index;
	size_t moved;
	size_t position;
	long order;
	int comparison;
	int focusable;
	int rendered;
	int error;

	/* The room for the order's elements. */
	stops = calloc(INPUT_FOCUS_MAX, sizeof(*stops));
	if (stops == NULL)
		return ENOMEM;

	/* The order's elements, in the document's order. */
	count = 0;
	position = 0;
	for (node = page->document->node.first_child;
	     node != NULL && count < INPUT_FOCUS_MAX;
	     node = input_next(node, &page->document->node)) {
		position++;
		if (node->type != DOM_ELEMENT)
			continue;

		/* An element that can be focused with a tabindex that is not negative, and is drawn. */
		element = (struct dom_element *)node;
		focusable = input_focusable(page, element, &order);
		if (!focusable || order < 0)
			continue;
		rendered = input_rendered(page, node);
		if (!rendered)
			continue;
		stops[count].element = element;
		stops[count].order = order;
		stops[count].position = position;
		count++;
	}

	/* The order: tabindex 1 and up first, in their order, then the others; ties in the document's order. */
	for (index = 1; index < count; index++) {
		stop = stops[index];

		/* The element moves back past every one that goes after it. */
		moved = index;
		while (moved > 0) {
			comparison = input_compare_stops(&stop, &stops[moved - 1U]);
			if (comparison >= 0)
				break;
			stops[moved] = stops[moved - 1U];
			moved--;
		}

		/* It goes in the place left. */
		stops[moved] = stop;
	}

	/* The focused element's place in the order (count when it is not in it). */
	current = input_focused(page);
	for (index = 0; index < count; index++) {
		if (stops[index].element == current)
			break;
	}

	/* The next element: from nothing, the first (or the last going back); past an end, nothing. */
	chosen = NULL;
	if (count != 0 && index == count) {
		if (backward)
			chosen = stops[count - 1U].element;
		else
			chosen = stops[0].element;
	} else if (!backward && index + 1U < count) {
		chosen = stops[index + 1U].element;
	} else if (backward && index > 0) {
		chosen = stops[index - 1U].element;
	}

	/* The order is not needed any more. */
	free(stops);

	/* The focus moves there, with its ring. */
	error = input_set_focus(page, chosen, 1);
	if (error != 0)
		return error;

	/* A text control reached with Tab takes typing after its text. */
	error = page_caret_to_end(page);
	if (error != 0)
		return error;

	/* Succeeded: the focus moved. */
	return 0;
}

/*
 * Moves the focus, with its ring, to the first control in the document's
 * order that takes text (a text field, a password field or a textarea),
 * can be focused, is drawn, and whose autocomplete attribute holds a
 * token (ASCII, in any case; "one-time-code").  Reports 1 when one took
 * the focus, 0 when there is none (the focus stays), or a negative errno
 * value when its listeners failed.
 */
int
page_focus_field(
	struct page *page,
	const char *token)
{
	struct dom_attribute *attribute;
	struct dom_element *element;
	struct dom_node *node;
	long order;
	int focusable;
	int rendered;
	int found;
	int kind;
	int held;
	int error;

	/* Each element in the document's order. */
	for (node = page->document->node.first_child;
	     node != NULL;
	     node = input_next(node, &page->document->node)) {
		if (node->type != DOM_ELEMENT)
			continue;
		element = (struct dom_element *)node;

		/* A control that takes text. */
		kind = dom_control_kind(element);
		if (kind != DOM_CONTROL_TEXT && kind != DOM_CONTROL_PASSWORD && kind != DOM_CONTROL_TEXTAREA)
			continue;

		/* With the token in its autocomplete attribute. */
		found = input_has_attribute(page, element, "autocomplete", &attribute);
		if (!found)
			continue;
		held = input_has_token(attribute->value, token);
		if (!held)
			continue;

		/* One that can be focused and is drawn. */
		focusable = input_focusable(page, element, &order);
		if (!focusable)
			continue;
		rendered = input_rendered(page, node);
		if (rendered)
			break;
	}

	/* None. */
	if (node == NULL)
		return 0;

	/* The focus moves there, with its ring, and typing goes after its text. */
	error = input_set_focus(page, element, 1);
	if (error != 0)
		return -error;
	error = page_caret_to_end(page);
	if (error != 0)
		return -error;

	/* Succeeded: the control has the focus. */
	return 1;
}

/*
 * Tells the page whether its view has its program's focus: the focused
 * element gets blur and focusout when it goes, and focus and focusin when
 * it comes back, and the ring is drawn only while it is there.
 */
int
page_window_focus(
	struct page *page,
	int focused)
{
	struct dom_element *element;
	int error;

	/* No change is nothing to do. */
	if (page->window_focused == focused)
		return 0;
	page->window_focused = focused;
	page->focus_generation++;

	/* The focused element hears of it. */
	element = input_focused(page);
	if (element == NULL)
		return 0;

	/* Losing the focus: blur, then focusout. */
	if (!focused) {
		error = bind_fire_focus_event(page->window, &element->node, "blur", 0);
		if (error != 0)
			return error;
		error = bind_fire_focus_event(page->window, &element->node, "focusout", 1);
		if (error != 0)
			return error;
		return 0;
	}

	/* Having it again: focus, then focusin. */
	error = bind_fire_focus_event(page->window, &element->node, "focus", 0);
	if (error != 0)
		return error;
	error = bind_fire_focus_event(page->window, &element->node, "focusin", 1);
	if (error != 0)
		return error;

	/* Succeeded: the element heard of it. */
	return 0;
}

/*
 * Activates the focused element (the default action of Enter): a link or
 * a button gets a click; unless a listener cancels it, a link's href is
 * written and *found set, for the view to follow.
 */
int
page_activate_focused(
	struct page *page,
	struct wb_buffer *href,
	int *found)
{
	struct dom_element *element;
	struct bind_mouse mouse;
	int is_button;
	int canceled;
	int kind;
	int error;

	/* Nothing focused activates nothing. */
	*found = 0;
	element = input_focused(page);
	if (element == NULL)
		return 0;

	/* A form control is activated as a control (a field submits its form, a button is clicked). */
	kind = dom_control_kind(element);
	if (kind != DOM_CONTROL_NONE) {
		error = input_activate_control(page, element, href, found);
		return error;
	}

	/* The link it is in, if any. */
	error = input_link_of(page, &element->node, href, found);
	if (error != 0)
		return error;

	/* Only a link or a button is activated by Enter. */
	is_button = input_is_button(&element->node);
	if (!*found && !is_button)
		return 0;

	/* The click, from the keyboard: no place and the main button. */
	memset(&mouse, 0, sizeof(mouse));
	error = bind_fire_mouse_event(page->window, &element->node, "click", &mouse, &canceled);
	if (error != 0)
		return error;

	/* A canceled click follows no link. */
	if (canceled)
		*found = 0;

	/* Succeeded: the element is activated. */
	return 0;
}

/*
 * Carries out the activation of the form control a click landed on at a
 * point of the page (the click itself was fired and not canceled): a
 * checkbox or radio button changes, a submit button submits its form, with
 * the location to go to in href and *found set.
 */
int
page_click_control(
	struct page *page,
	int x,
	int y,
	struct wb_buffer *href,
	int *found)
{
	struct dom_node *node;
	int handled;
	int error;

	/* The element under the point. */
	*found = 0;
	node = input_element_at(page, x, y);
	if (node == NULL || node->type != DOM_ELEMENT)
		return 0;

	/* A video with controls plays, pauses or goes to a time (ws121-p006). */
	handled = page_media_click(page, (struct dom_element *)node, x, y);
	if (handled)
		return 0;

	/* Its activation as a control (nothing for another element). */
	error = page_activate_control(page, (struct dom_element *)node, 0, href, found, &handled);
	if (error != 0)
		return error;

	/* Succeeded: the control is activated. */
	return 0;
}

/*
 * Tells whether the focused element is pressed by Space: a button, a
 * checkbox or a radio button (Space scrolls the page otherwise).
 */
int
page_focus_pressable(
	struct page *page)
{
	struct dom_element *element;
	int kind;

	/* Nothing focused presses nothing. */
	element = input_focused(page);
	if (element == NULL)
		return 0;

	/* The kinds Space presses. */
	kind = dom_control_kind(element);
	switch (kind) {
	case DOM_CONTROL_SUBMIT:
	case DOM_CONTROL_BUTTON:
	case DOM_CONTROL_RESET:
	case DOM_CONTROL_CHECKBOX:
	case DOM_CONTROL_RADIO:
		return 1;
	default:
		break;
	}

	/* Anything else is not pressed. */
	return 0;
}

/*
 * Finds the rectangle of the focused element on the laid out page, in
 * layout units; returns whether there is a focused element with a box.
 */
int
page_focus_rect(
	struct page *page,
	struct layout_rect *rect)
{
	struct dom_element *element;
	int found;

	/* Nothing focused, or a page not laid out, has no rectangle. */
	element = input_focused(page);
	if (element == NULL)
		return 0;
	if (!page->laid_out)
		return 0;

	/* The element's boxes. */
	found = layout_node_bounds(&page->layout, &element->node, rect);

	/* Reports whether it has any. */
	return found;
}

/*
 * Tells whether the focus changed since the display list was made (the
 * ring moved, came or went), so the page must be painted again.
 */
int
page_needs_paint(
	const struct page *page)
{
	/* A page not painted needs it. */
	if (!page->painted)
		return 1;

	/* A change of the focus since. */
	if (page->painted_focus != page->focus_generation)
		return 1;

	/* A picture of a video that came since (ws121-p004). */
	if (page->painted_media != page->media_generation)
		return 1;

	/* The display list is up to date. */
	return 0;
}

/*
 * Adds the focus ring to the display list just made (page_paint), when the
 * keyboard put the focus on an element with a box and the view has its
 * program's focus.
 */
int
page_paint_focus(
	struct page *page)
{
	struct layout_rect rect;
	layout_unit gap;
	int found;
	int error;

	/* The list shows the focus as it is now. */
	page->painted_focus = page->focus_generation;

	/* A ring only for a focus the keyboard moved, while the view has the focus. */
	if (!page->focus_visible)
		return 0;
	if (!page->window_focused)
		return 0;

	/* The focused element's rectangle. */
	found = page_focus_rect(page, &rect);
	if (!found)
		return 0;

	/* The ring, a little outside it. */
	gap = INPUT_RING_GAP * LAYOUT_UNIT;
	error = paint_add_ring(
		&page->paint,
		rect.x - gap,
		rect.y - gap,
		rect.width + 2 * gap,
		rect.height + 2 * gap,
		INPUT_RING_WIDTH * LAYOUT_UNIT,
		INPUT_RING_COLOR);
	if (error != 0)
		return error;

	/* Succeeded: the ring is drawn over the page. */
	return 0;
}

/*
 * Activates a focused form control from the keyboard: a text field submits
 * its form (implicit submission); a button, a checkbox or a radio button
 * gets a click and, unless it is canceled, does what it does.
 */
static int
input_activate_control(
	struct page *page,
	struct dom_element *element,
	struct wb_buffer *href,
	int *found)
{
	struct bind_mouse mouse;
	int handled;
	int canceled;
	int pressable;
	int kind;
	int error;

	/* A field submits its form as Enter in it does. */
	*found = 0;
	kind = dom_control_kind(element);
	if (kind == DOM_CONTROL_TEXT || kind == DOM_CONTROL_PASSWORD) {
		error = page_activate_control(page, element, 1, href, found, &handled);
		return error;
	}

	/* Only the pressable controls are activated by the keyboard; a textarea or a select is not. */
	pressable = page_focus_pressable(page);
	if (!pressable)
		return 0;

	/* The click, from the keyboard: no place and the main button. */
	memset(&mouse, 0, sizeof(mouse));
	error = bind_fire_mouse_event(page->window, &element->node, "click", &mouse, &canceled);
	if (error != 0)
		return error;
	if (canceled)
		return 0;

	/* What the control does. */
	error = page_activate_control(page, element, 0, href, found, &handled);
	if (error != 0)
		return error;

	/* Succeeded: the control is activated. */
	return 0;
}

/* Finds the element under a point of the document, or the root element when no box is there. */
static struct dom_node *
input_element_at(
	struct page *page,
	int x,
	int y)
{
	struct dom_node *node;

	/* The deepest box under the point, which may be text. */
	node = NULL;
	if (page->laid_out)
		node = layout_hit_node(&page->layout, (layout_unit)x * LAYOUT_UNIT, (layout_unit)y * LAYOUT_UNIT);

	/* Text's element is its parent. */
	while (node != NULL && node->type != DOM_ELEMENT)
		node = node->parent;

	/* Nothing under the point is the root element. */
	if (node == NULL) {
		for (node = page->document->node.first_child; node != NULL; node = node->next) {
			if (node->type == DOM_ELEMENT)
				break;
		}
	}

	/* The element. */
	return node;
}

/* Finds where the keys go: the focused element, or the body, or the root element. */
static struct dom_node *
input_key_target(
	struct page *page)
{
	struct dom_element *focused;
	struct dom_node *root;
	struct dom_node *child;
	int is_body;

	/* The focused element. */
	focused = input_focused(page);
	if (focused != NULL)
		return &focused->node;

	/* The root element. */
	for (root = page->document->node.first_child; root != NULL; root = root->next) {
		if (root->type == DOM_ELEMENT)
			break;
	}

	/* A document without one has nothing to take the keys. */
	if (root == NULL)
		return NULL;

	/* Its body, when it has one. */
	for (child = root->first_child; child != NULL; child = child->next) {
		is_body = dom_element_is(child, DOM_NS_HTML, DOM_TAG_BODY);
		if (is_body)
			return child;
	}

	/* A document without a body: the root element. */
	return root;
}

/*
 * Reports the focused element, forgetting one that a script took out of
 * the document (it can no longer have the focus).
 */
static struct dom_element *
input_focused(
	struct page *page)
{
	int connected;

	/* Nothing focused. */
	if (page->focused == NULL)
		return NULL;

	/* An element out of the document loses the focus without events. */
	connected = dom_is_inclusive_ancestor(&page->document->node, &page->focused->node);
	if (!connected) {
		page_compose_end(page, page->focused);
		page->focused = NULL;
		page->focus_visible = 0;
		page->focus_generation++;
		return NULL;
	}

	/* The element. */
	return page->focused;
}

/*
 * Moves the focus to an element (NULL for none), with its ring when the
 * keyboard moved it (visible): the old element gets blur and focusout, the
 * new one focus and focusin.
 */
static int
input_set_focus(
	struct page *page,
	struct dom_element *element,
	int visible)
{
	struct dom_element *old;
	int error;

	/* The same element keeps the focus; only whether its ring shows may change. */
	old = input_focused(page);
	if (old == element) {
		/* The ring's change is painted with the next generation. */
		if (page->focus_visible != visible) {
			page->focus_visible = visible;
			page->focus_generation++;
		}

		/* The focus stays where it was. */
		return 0;
	}

	/*
	 * What an input method was composing in the old element goes into its
	 * value before the focus leaves it, as a click in it would put it
	 * (input fires; ws177-p019).
	 */
	error = page_compose_commit(page, old);
	if (error != 0)
		return error;

	/*
	 * The new focus is recorded before the events run, so a listener that
	 * asks sees it; the generation paints the ring where it now is, and
	 * the session's change starts the program's input method again for the
	 * new element (ws177-p019).
	 */
	page->focused = element;
	page->focus_visible = visible;
	page->focus_generation++;
	page->document->compose_session++;

	/* The old element: blur, then focusout. */
	if (old != NULL) {
		error = bind_fire_focus_event(page->window, &old->node, "blur", 0);
		if (error != 0)
			return error;
		error = bind_fire_focus_event(page->window, &old->node, "focusout", 1);
		if (error != 0)
			return error;
	}

	/* No new element hears nothing more. */
	if (element == NULL)
		return 0;

	/* The new one: focus, then focusin. */
	error = bind_fire_focus_event(page->window, &element->node, "focus", 0);
	if (error != 0)
		return error;
	error = bind_fire_focus_event(page->window, &element->node, "focusin", 1);
	if (error != 0)
		return error;

	/* Succeeded: the focus moved. */
	return 0;
}

/*
 * Tells whether an element can be focused, and its place in the sequential
 * focus order (*order: its tabindex, or 0 without one; negative keeps it
 * out of the order).  A tabindex makes any element focusable; without one,
 * a link with an href, a button, a select and a textarea can be, and an
 * input that is not hidden, unless disabled.
 */
static int
input_focusable(
	struct page *page,
	const struct dom_element *element,
	long *order)
{
	struct dom_attribute *attribute;
	long value;
	int tabindex;
	int present;
	int hidden;

	/* A tabindex that is a number decides. */
	*order = 0;
	tabindex = input_tabindex(page, element, &value);
	if (tabindex == INPUT_TABINDEX_SET) {
		*order = value;
		return 1;
	}

	/* Only HTML elements are focusable by their kind. */
	if (element->ns != DOM_NS_HTML)
		return 0;

	/* A link or an image map's area with an href. */
	if (element->tag == DOM_TAG_A || element->tag == DOM_TAG_AREA) {
		present = input_has_attribute(page, element, "href", &attribute);
		return present;
	}

	/* The form controls, unless disabled. */
	if (element->tag != DOM_TAG_BUTTON &&
	    element->tag != DOM_TAG_INPUT &&
	    element->tag != DOM_TAG_SELECT &&
	    element->tag != DOM_TAG_TEXTAREA)
		return 0;
	present = input_has_attribute(page, element, "disabled", &attribute);
	if (present)
		return 0;

	/* An input of the hidden type is never focused. */
	if (element->tag == DOM_TAG_INPUT) {
		present = input_has_attribute(page, element, "type", &attribute);
		if (present) {
			hidden = input_equal_folded(attribute->value, "hidden");
			if (hidden)
				return 0;
		}
	}

	/* The control can be focused. */
	return 1;
}

/*
 * Reads an element's tabindex as the HTML standard parses an integer:
 * spaces, an optional sign and digits.  Returns INPUT_TABINDEX_SET with the
 * value, or INPUT_TABINDEX_NONE for no tabindex or one that is not a number.
 */
static int
input_tabindex(
	struct page *page,
	const struct dom_element *element,
	long *value)
{
	struct dom_attribute *attribute;
	const struct vm_string *text;
	const unsigned char *latin1;
	unsigned char character;
	size_t index;
	long number;
	int negative;
	int digits;
	int present;
	int space;

	/* The attribute. */
	*value = 0;
	present = input_has_attribute(page, element, "tabindex", &attribute);
	if (!present)
		return INPUT_TABINDEX_NONE;

	/* A number is ASCII; a value of wider characters is not one. */
	text = attribute->value;
	if ((text->flags & VM_STRING_WIDE) != 0U)
		return INPUT_TABINDEX_NONE;
	latin1 = vm_string_latin1(text);

	/* The leading spaces (the standard's ASCII whitespace). */
	for (index = 0; index < text->length; index++) {
		character = latin1[index];
		space = 0;

		/* Which characters are spaces. */
		switch (character) {
		case ' ':
		case '\t':
		case '\n':
		case '\f':
		case '\r':
			space = 1;
			break;
		default:
			break;
		}

		/* The first that is not ends them. */
		if (!space)
			break;
	}

	/* The sign. */
	negative = 0;
	if (index < text->length && latin1[index] == '-') {
		negative = 1;
		index++;
	} else if (index < text->length && latin1[index] == '+') {
		index++;
	}

	/* The digits, as far as they go (a huge value stops growing). */
	number = 0;
	digits = 0;
	for (; index < text->length; index++) {
		character = latin1[index];
		if (character < '0' || character > '9')
			break;
		if (number < 100000000L)
			number = number * 10 + (long)(character - '0');
		digits++;
	}

	/* No digit is no number. */
	if (digits == 0)
		return INPUT_TABINDEX_NONE;

	/* Succeeded: the number, with its sign. */
	*value = number;
	if (negative)
		*value = -number;
	return INPUT_TABINDEX_SET;
}

/* Tells whether an element has an attribute (by its name in lower case), and finds it. */
static int
input_has_attribute(
	struct page *page,
	const struct dom_element *element,
	const char *name,
	struct dom_attribute **attribute)
{
	struct vm_string *atom;

	/* The name as the atom the elements keep; without memory, no attribute is found. */
	*attribute = NULL;
	atom = vm_atom_from_ascii(page->heap, name);
	if (atom == NULL)
		return 0;

	/* The attribute. */
	*attribute = dom_element_find_attribute(element, DOM_NS_NONE, atom);
	if (*attribute == NULL)
		return 0;

	/* The element has it. */
	return 1;
}

/* Tells whether a string is an ASCII word in any case (the word is in lower case). */
static int
input_equal_folded(
	const struct vm_string *string,
	const char *ascii)
{
	const unsigned char *latin1;
	unsigned char character;
	size_t length;
	size_t index;

	/* The lengths must agree, and the word is never of wide characters. */
	length = strlen(ascii);
	if (string->length != length)
		return 0;
	if ((string->flags & VM_STRING_WIDE) != 0U)
		return 0;

	/* Each character, folded to lower case. */
	latin1 = vm_string_latin1(string);
	for (index = 0; index < length; index++) {
		character = latin1[index];
		if (character >= 'A' && character <= 'Z')
			character = (unsigned char)(character - 'A' + 'a');
		if (character != (unsigned char)ascii[index])
			return 0;
	}

	/* The same word. */
	return 1;
}

/* Tells whether a string's tokens (between ASCII spaces) hold a word, in any ASCII case (the word is in lower case). */
static int
input_has_token(
	const struct vm_string *string,
	const char *token)
{
	const unsigned char *latin1;
	unsigned char character;
	size_t length;
	size_t start;
	size_t index;
	size_t end;

	/* Tokens are ASCII; a string of wide characters is not read. */
	if ((string->flags & VM_STRING_WIDE) != 0U)
		return 0;
	latin1 = vm_string_latin1(string);
	length = strlen(token);

	/* Each token. */
	start = 0;
	while (start < string->length) {
		/* Its spaces before it. */
		if (latin1[start] == ' ' || latin1[start] == '\t' || latin1[start] == '\n' || latin1[start] == '\f' || latin1[start] == '\r') {
			start++;
			continue;
		}

		/* Its end. */
		end = start;
		while (end < string->length && latin1[end] != ' ' && latin1[end] != '\t' && latin1[end] != '\n' && latin1[end] != '\f' && latin1[end] != '\r')
			end++;

		/* The word, compared folded. */
		if (end - start == length) {
			for (index = 0; index < length; index++) {
				character = latin1[start + index];
				if (character >= 'A' && character <= 'Z')
					character = (unsigned char)(character - 'A' + 'a');
				if (character != (unsigned char)token[index])
					break;
			}

			/* The whole word. */
			if (index == length)
				return 1;
		}

		/* The next token. */
		start = end;
	}

	/* Not held. */
	return 0;
}

/* Tells whether a node has a box on the laid out page. */
static int
input_rendered(
	struct page *page,
	const struct dom_node *node)
{
	struct layout_rect rect;
	int found;

	/* A page not laid out draws nothing. */
	if (!page->laid_out)
		return 0;

	/* The node's boxes. */
	found = layout_node_bounds(&page->layout, node, &rect);

	/* Reports whether it has any. */
	return found;
}

/* Finds the node after another in the document's order, within a root (NULL at the end). */
static struct dom_node *
input_next(
	const struct dom_node *node,
	const struct dom_node *root)
{
	/* The first child, when there is one. */
	if (node->first_child != NULL)
		return node->first_child;

	/* Otherwise the next sibling of the node or of the nearest ancestor that has one. */
	while (node != NULL && node != root) {
		if (node->next != NULL)
			return node->next;
		node = node->parent;
	}

	/* The end of the root. */
	return NULL;
}

/* Finds the <a href> a node is in (the node itself or an ancestor) and writes its href. */
static int
input_link_of(
	struct page *page,
	const struct dom_node *node,
	struct wb_buffer *href,
	int *found)
{
	struct dom_attribute *attribute;
	int is_link;
	int present;
	int depth;
	int error;

	/* The node, then its ancestors. */
	*found = 0;
	for (depth = 0; node != NULL && depth < INPUT_DEPTH; depth++) {
		/* An HTML <a> with an href is the link. */
		is_link = dom_element_is(node, DOM_NS_HTML, DOM_TAG_A);
		if (is_link) {
			present = input_has_attribute(page, (const struct dom_element *)node, "href", &attribute);
			if (present)
				break;
		}

		/* Otherwise its parent. */
		node = node->parent;
	}

	/* No link around the node. */
	if (node == NULL || depth == INPUT_DEPTH)
		return 0;

	/* The href's value. */
	error = vm_string_to_utf8(attribute->value, href);
	if (error != 0)
		return error;

	/* Succeeded: the link is found. */
	*found = 1;
	return 0;
}

/* Tells whether a node is a button (a <button>, or an input of a button's type). */
static int
input_is_button(
	const struct dom_node *node)
{
	int is_button;

	/* A <button>. */
	is_button = dom_element_is(node, DOM_NS_HTML, DOM_TAG_BUTTON);
	if (is_button)
		return 1;

	/* The form controls are activated as controls (page_activate_control) before this is asked. */
	return 0;
}

/*
 * Orders two elements of the sequential focus order: a positive tabindex
 * before none, smaller positive ones first, and otherwise the document's
 * order.  Negative when left goes first.
 */
static int
input_compare_stops(
	const struct input_stop *left,
	const struct input_stop *right)
{
	/* A positive tabindex goes before a zero one. */
	if (left->order > 0 && right->order == 0)
		return -1;
	if (left->order == 0 && right->order > 0)
		return 1;

	/* Two positive ones by their value. */
	if (left->order < right->order)
		return -1;
	if (left->order > right->order)
		return 1;

	/* Otherwise the document's order. */
	if (left->position < right->position)
		return -1;
	if (left->position > right->position)
		return 1;

	/* The same element. */
	return 0;
}
