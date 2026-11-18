/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scroll of the library (ws090-p003, plan/ws090/design.md section 6):
 * one part of a window whose content is larger than the part.
 *
 * Three things move the content, one at a time: a glide (the wheel and
 * the keys), which closes on its target by e every KL_SCROLL_GLIDE_US and
 * is a pure function of the time since it started (Text Editor's glide);
 * a finger, whose drag and fling libkeiland's scroller follows with its
 * inertia and rubber band; and a move to a place at once.  The latest one
 * takes over: a wheel turned while the content flies stops the flight, a
 * finger that touches stops a glide.
 *
 * A touch pad's two fingers (KL_VERSION 40, BUG-211) hold the content as a
 * finger on the screen does: each of their moves moves it at once (no
 * glide, BUG-218), and when they lift (axis_stop) it flies on at their
 * velocity.  The scroller does both (kl_scroller_axis, KL_VERSION 41), with
 * the same inertia and rubber band as a finger's.
 *
 * The ends are 0..content-viewport on each axis that scrolls, and the
 * rubber band is measured against the viewport; kl_scroll_set_bounds
 * (KL_VERSION 61, ws090-p015) gives other ends and another size instead,
 * for a view whose position runs below 0 (Terminal's scrollback) or whose
 * content is laid out by the program (Notes' zoomed page).
 */

#include <keiland/keiland.h>

#include <errno.h>
#include <math.h>
#include <string.h>

/* A glide stops within this distance of its target, in pixels. */
#define SCROLL_GLIDE_STOP	0.5

/* The bars: their thickness, their shortest length, their gap from the viewport's edges and their darkest alpha. */
#define SCROLL_BAR_WIDTH	4
#define SCROLL_BAR_MIN		24
#define SCROLL_BAR_GAP		3
#define SCROLL_BAR_ALPHA	110.0

static void scroll_bounds(struct kl_scroll *scroll);
static void scroll_clamp(struct kl_scroll *scroll);
static void scroll_place(struct kl_scroll *scroll, double x, double y, uint64_t now_us);
static double scroll_minimum_x(const struct kl_scroll *scroll);
static double scroll_minimum_y(const struct kl_scroll *scroll);
static double scroll_within(double value, double minimum, double maximum);

/*
 * Makes a scroll along some axes (KL_SCROLL_X, KL_SCROLL_Y), at 0, 0
 * with nothing to scroll yet.
 *
 * Returns 0, EINVAL (no axis) or ENOMEM.
 */
int
kl_scroll_init(
	struct kl_scroll *scroll,
	unsigned axes)
{
	/* Nothing moves yet. */
	memset(scroll, 0, sizeof(*scroll));

	/* At least one axis. */
	if ((axes & (KL_SCROLL_X | KL_SCROLL_Y)) == 0U)
		return EINVAL;
	scroll->axes = axes;

	/* The finger's scroller. */
	scroll->scroller = kl_scroller_create();
	if (scroll->scroller == NULL)
		return ENOMEM;

	/* Succeeded: the scroll waits for its sizes. */
	return 0;
}

/*
 * Frees what a scroll holds.
 */
void
kl_scroll_release(
	struct kl_scroll *scroll)
{
	/* The scroller, when it was made. */
	if (scroll->scroller != NULL)
		kl_scroller_destroy(scroll->scroller);
	memset(scroll, 0, sizeof(*scroll));
}

/*
 * Sets the content's size and the viewport's; a position past the new
 * ends moves back within them (unless a finger holds the content).
 */
void
kl_scroll_set_size(
	struct kl_scroll *scroll,
	double content_width,
	double content_height,
	double viewport_width,
	double viewport_height)
{
	/* The sizes, whose ends replace any kl_scroll_set_bounds gave. */
	scroll->content_width = content_width;
	scroll->content_height = content_height;
	scroll->viewport_width = viewport_width;
	scroll->viewport_height = viewport_height;
	scroll->bounded = 0;

	/* The scroller's bounds, and the position within them. */
	scroll_bounds(scroll);
	if (!scroll->touched)
		scroll_clamp(scroll);

	/* A glide's target within the new ends too. */
	scroll->to_x = scroll_within(scroll->to_x, scroll_minimum_x(scroll), kl_scroll_limit_x(scroll));
	scroll->to_y = scroll_within(scroll->to_y, scroll_minimum_y(scroll), kl_scroll_limit_y(scroll));
}

/*
 * Sets the ends of the position on each axis and the size the rubber band
 * past them is measured against, in place of the ends of the sizes
 * (KL_VERSION 61); a position past the new ends moves back within them
 * (unless a finger holds the content).  An axis the scroll does not move
 * along keeps 0.
 *
 * Returns 0, or EINVAL for a maximum below its minimum or a band not above
 * zero.
 */
int
kl_scroll_set_bounds(
	struct kl_scroll *scroll,
	double minimum_x,
	double maximum_x,
	double minimum_y,
	double maximum_y,
	double band_width,
	double band_height)
{
	/* Ends in order. */
	if (maximum_x < minimum_x)
		return EINVAL;
	if (maximum_y < minimum_y)
		return EINVAL;

	/* A rubber band with a size. */
	if (band_width <= 0.0)
		return EINVAL;
	if (band_height <= 0.0)
		return EINVAL;

	/* The ends and the band, which the limits and the scroller follow from now. */
	scroll->bounded = 1;
	scroll->minimum_x = minimum_x;
	scroll->maximum_x = maximum_x;
	scroll->minimum_y = minimum_y;
	scroll->maximum_y = maximum_y;
	scroll->band_width = band_width;
	scroll->band_height = band_height;

	/* The scroller's bounds, and the position within them. */
	scroll_bounds(scroll);
	if (!scroll->touched)
		scroll_clamp(scroll);

	/* A glide's target within the new ends too. */
	scroll->to_x = scroll_within(scroll->to_x, scroll_minimum_x(scroll), kl_scroll_limit_x(scroll));
	scroll->to_y = scroll_within(scroll->to_y, scroll_minimum_y(scroll), kl_scroll_limit_y(scroll));

	/* Succeeded: the ends hold. */
	return 0;
}

/*
 * Turns the wheel: the content glides on by dx, dy pixels (added to a
 * glide under way), within its ends.
 */
void
kl_scroll_wheel(
	struct kl_scroll *scroll,
	double dx,
	double dy,
	uint64_t now_us)
{
	double x;
	double y;

	/* From the glide's target when one is under way, else from where the content is. */
	x = scroll->x;
	y = scroll->y;
	if (scroll->gliding) {
		x = scroll->to_x;
		y = scroll->to_y;
	}

	/* Only the axes the scroll moves along. */
	if ((scroll->axes & KL_SCROLL_X) != 0U)
		x += dx;
	if ((scroll->axes & KL_SCROLL_Y) != 0U)
		y += dy;

	/* A glide there. */
	kl_scroll_move_to(scroll, x, y, 1, now_us);
}

/*
 * Moves the content to a place within its ends: gliding there, or at once.
 * A finger's flight stops.
 */
void
kl_scroll_move_to(
	struct kl_scroll *scroll,
	double x,
	double y,
	int glide,
	uint64_t now_us)
{
	/* The place within the ends. */
	x = scroll_within(x, scroll_minimum_x(scroll), kl_scroll_limit_x(scroll));
	y = scroll_within(y, scroll_minimum_y(scroll), kl_scroll_limit_y(scroll));

	/* The finger no longer owns the content. */
	scroll->touched = 0;
	scroll->released = 0;

	/* At once. */
	if (!glide) {
		scroll->gliding = 0;
		scroll_place(scroll, x, y, now_us);
		return;
	}

	/* A glide from here, starting now. */
	scroll->from_x = scroll->x;
	scroll->from_y = scroll->y;
	scroll->to_x = x;
	scroll->to_y = y;
	scroll->glide_us = now_us;
	scroll->gliding = 1;
}

/*
 * Glides the content just far enough to show a rectangle of it (content
 * coordinates) whole, or its start when it is larger than the viewport.
 */
void
kl_scroll_reveal(
	struct kl_scroll *scroll,
	const struct kl_rect *rect,
	uint64_t now_us)
{
	double x;
	double y;

	/* From the glide's target when one is under way. */
	x = scroll->x;
	y = scroll->y;
	if (scroll->gliding) {
		x = scroll->to_x;
		y = scroll->to_y;
	}

	/* Across: past the right edge, then the left (which wins). */
	if ((double)(rect->x + rect->width) > x + scroll->viewport_width)
		x = (double)(rect->x + rect->width) - scroll->viewport_width;
	if ((double)rect->x < x)
		x = (double)rect->x;

	/* Down: past the bottom, then the top (which wins). */
	if ((double)(rect->y + rect->height) > y + scroll->viewport_height)
		y = (double)(rect->y + rect->height) - scroll->viewport_height;
	if ((double)rect->y < y)
		y = (double)rect->y;

	/* Nothing to move. */
	if (x == scroll->x && y == scroll->y && !scroll->gliding)
		return;

	/* A glide there. */
	kl_scroll_move_to(scroll, x, y, 1, now_us);
}

/*
 * Carries out a scrolling key: the arrows by a line, Page Up and Page Down
 * by the viewport less a line, Home and End (with Control) to the ends.
 * Returns 1 when the key was a scrolling one.
 */
int
kl_scroll_key(
	struct kl_scroll *scroll,
	uint32_t key,
	unsigned modifiers,
	double line,
	uint64_t now_us)
{
	double page;
	double x;
	double y;

	/* From the glide's target when one is under way. */
	x = scroll->x;
	y = scroll->y;
	if (scroll->gliding) {
		x = scroll->to_x;
		y = scroll->to_y;
	}

	/* A page keeps a line of what showed. */
	page = scroll->viewport_height - line;
	if (page < line)
		page = line;

	/* The key's move. */
	switch (key) {
	case KL_KEY_UP:
		y -= line;
		break;
	case KL_KEY_DOWN:
		y += line;
		break;
	case KL_KEY_LEFT:
		x -= line;
		break;
	case KL_KEY_RIGHT:
		x += line;
		break;
	case KL_KEY_PAGEUP:
		y -= page;
		break;
	case KL_KEY_PAGEDOWN:
	case KL_KEY_SPACE:
		/* Space pages down, and Shift+Space up. */
		if (key == KL_KEY_SPACE && (modifiers & KL_MOD_SHIFT) != 0U) {
			y -= page;
			break;
		}

		/* The rest go down a page. */
		y += page;
		break;
	case KL_KEY_HOME:
		/* Home goes to the start (with or without Control). */
		y = scroll_minimum_y(scroll);
		if ((modifiers & KL_MOD_CTRL) == 0U)
			x = scroll_minimum_x(scroll);
		break;
	case KL_KEY_END:
		y = kl_scroll_limit_y(scroll);
		break;
	default:
		return 0;
	}

	/* A glide there. */
	kl_scroll_move_to(scroll, x, y, 1, now_us);
	return 1;
}

/*
 * A finger touches the content: the glide stops and the scroller holds
 * it.  Returns 1 when the touch caught content that was flying (the touch
 * then only stops it and does not tap).
 */
int
kl_scroll_press(
	struct kl_scroll *scroll,
	uint64_t now_us)
{
	int caught;

	/* The scroller from where the content is (a glide stops there). */
	if (!scroll->touched) {
		scroll_bounds(scroll);
		kl_scroller_set_position(scroll->scroller, scroll->x, scroll->y);
	}

	/* A glide under way ends where the content is. */
	scroll->gliding = 0;

	/* The finger holds the content. */
	caught = kl_scroller_press(scroll->scroller, now_us);
	scroll->touched = 1;
	scroll->released = 0;

	/* Succeeded: whether a flight was caught. */
	return caught;
}

/*
 * The finger has moved by dx, dy since it touched (the total).
 */
void
kl_scroll_drag(
	struct kl_scroll *scroll,
	double dx,
	double dy)
{
	/* Only a held content follows a finger. */
	if (!scroll->touched)
		return;

	/* The scroller moves the content the other way (a finger moving down shows what is above). */
	kl_scroller_drag(scroll->scroller, dx, dy);
}

/*
 * The finger lifts with a velocity (pixels a second, as the finger moved):
 * the content flies on, or settles within its ends.  Returns 1 when it
 * flies (KL_VERSION 61), 0 when it settles or no finger held it.
 */
int
kl_scroll_fling(
	struct kl_scroll *scroll,
	double vx,
	double vy,
	uint64_t now_us)
{
	int flung;

	/* Only a held content flies. */
	if (!scroll->touched)
		return 0;

	/* The scroller takes the velocity; the content is the scroller's until it rests. */
	flung = kl_scroller_release(scroll->scroller, now_us, vx, vy);
	scroll->released = 1;
	if (flung == 0)
		return 0;

	/* Succeeded: the content flies. */
	return 1;
}

/*
 * The fingers were taken away: no flight, and content past an end springs
 * back.
 */
void
kl_scroll_cancel(
	struct kl_scroll *scroll,
	uint64_t now_us)
{
	/* Only a held content. */
	if (!scroll->touched)
		return;

	/* The scroller springs back. */
	kl_scroller_cancel(scroll->scroller, now_us);
	scroll->released = 1;
}

/*
 * Scrolls by an axis event of a window: a wheel's glides as kl_scroll_wheel
 * does; a touch pad's fingers (KL_AXIS_SOURCE_FINGER) hold the content and
 * move it at once by dx, dy (as a wheel scrolls: down and right positive),
 * the first move catching content that flies.  The event's time is taken
 * as now (kl_scroll_axis_at keeps them apart).
 */
void
kl_scroll_axis(
	struct kl_scroll *scroll,
	double dx,
	double dy,
	unsigned source,
	uint64_t now_us)
{
	/* The same, at the event's time. */
	(void)kl_scroll_axis_at(scroll, dx, dy, source, now_us, now_us);
}

/*
 * The touch pad's fingers have lifted: the content flies on at their
 * velocity (none when they rested before lifting), or settles within its
 * ends.  Returns 1 when it flies (KL_VERSION 41), 0 otherwise.
 */
int
kl_scroll_axis_stop(
	struct kl_scroll *scroll,
	uint64_t now_us)
{
	int flung;

	/* The same, at the event's time, without the velocity. */
	flung = kl_scroll_axis_stop_at(scroll, now_us, now_us, NULL, NULL);
	if (flung == 0)
		return 0;

	/* Succeeded: the content flies. */
	return 1;
}

/*
 * Scrolls by an axis event as kl_scroll_axis does, with the event's own
 * time (event_us, which the fingers' velocity is worked out from) apart
 * from the time the steps use (now_us).  A touch pad's first move takes
 * the content from where the scroll has it, also while the fingers hold
 * it after kl_scroll_move_to handed a new place over.  Returns 1 when the
 * fingers' first move caught content that flew (KL_VERSION 61), 0
 * otherwise.
 */
int
kl_scroll_axis_at(
	struct kl_scroll *scroll,
	double dx,
	double dy,
	unsigned source,
	uint64_t event_us,
	uint64_t now_us)
{
	int holding;
	int caught;

	/* A wheel, or anything that is not fingers, glides. */
	if (source != KL_AXIS_SOURCE_FINGER) {
		kl_scroll_wheel(scroll, dx, dy, now_us);
		return 0;
	}

	/* The fingers' first move takes the content: the scroller from where it is (a glide stops there). */
	holding = kl_scroller_axis_holding(scroll->scroller);
	if (!holding || !scroll->touched) {
		if (!scroll->touched) {
			scroll_bounds(scroll);
			kl_scroller_set_position(scroll->scroller, scroll->x, scroll->y);
		}

		/* The fingers own the content from here: no glide, and they have not lifted. */
		scroll->gliding = 0;
		scroll->touched = 1;
		scroll->released = 0;
	}

	/* Only the axes the scroll moves along. */
	if ((scroll->axes & KL_SCROLL_X) == 0U)
		dx = 0.0;
	if ((scroll->axes & KL_SCROLL_Y) == 0U)
		dy = 0.0;

	/* The scroller holds the content and moves it with the fingers (catching a flight at the first move). */
	caught = kl_scroller_axis(scroll->scroller, dx, dy, event_us, now_us);
	scroll->moved_us = now_us;
	if (scroll->moved_us == 0U)
		scroll->moved_us = 1U;
	if (caught == 0)
		return 0;

	/* Succeeded: a flight was caught. */
	return 1;
}

/*
 * The touch pad's fingers have lifted at event_us: the content flies on
 * from now_us at their velocity, as kl_scroll_axis_stop does, and the
 * velocity is given (pixels a second, a wheel's way; either may be NULL;
 * 0 when the fingers held nothing).  Returns 1 when it flies (KL_VERSION
 * 61), 0 otherwise.
 */
int
kl_scroll_axis_stop_at(
	struct kl_scroll *scroll,
	uint64_t event_us,
	uint64_t now_us,
	double *vx,
	double *vy)
{
	int holding;
	int flung;

	/* No velocity until the scroller gives one. */
	if (vx != NULL)
		*vx = 0.0;
	if (vy != NULL)
		*vy = 0.0;

	/* Only content the fingers hold. */
	holding = kl_scroller_axis_holding(scroll->scroller);
	if (!holding)
		return 0;

	/* The scroller throws it; the content is the scroller's until it rests. */
	flung = kl_scroller_axis_stop(scroll->scroller, event_us, now_us, vx, vy);
	scroll->released = 1;
	if (flung == 0)
		return 0;

	/* Succeeded: the content flies. */
	return 1;
}

/*
 * Tells whether a touch pad's fingers hold the content (from their first
 * move until they lift, KL_VERSION 61).
 */
int
kl_scroll_axis_holding(
	const struct kl_scroll *scroll)
{
	int holding;

	/* The scroller follows the fingers. */
	holding = kl_scroller_axis_holding(scroll->scroller);
	if (holding == 0)
		return 0;

	/* Succeeded: the fingers hold it. */
	return 1;
}

/*
 * Moves time on: the position at a time, from a finger's scroller or a
 * glide.  Returns 1 while the content moves by itself (the window should
 * draw the next frame).
 */
int
kl_scroll_step(
	struct kl_scroll *scroll,
	uint64_t now_us)
{
	double share;
	double left_x;
	double left_y;
	double x;
	double y;
	int moving;

	/* A finger's content is where the scroller has it. */
	if (scroll->touched) {
		moving = kl_scroller_step(scroll->scroller, now_us, &x, &y);
		scroll_place(scroll, x, y, now_us);

		/* At rest after the finger lifted: the content is the scroll's again. */
		if (!moving && scroll->released) {
			scroll->touched = 0;
			scroll->released = 0;
			scroll_clamp(scroll);
		}

		/* Reports whether it flies on (a held content moves with the finger's input, not by itself). */
		return moving;
	}

	/* No glide: nothing moves. */
	if (!scroll->gliding)
		return 0;

	/* The glide's share done by now: 1 - e^(-t / time constant). */
	share = 1.0;
	if (now_us > scroll->glide_us)
		share = 1.0 - exp(-(double)(now_us - scroll->glide_us) / (double)KL_SCROLL_GLIDE_US);
	if (now_us <= scroll->glide_us)
		share = 0.0;
	x = scroll->from_x + (scroll->to_x - scroll->from_x) * share;
	y = scroll->from_y + (scroll->to_y - scroll->from_y) * share;

	/* Within half a pixel on both axes: there, and the glide ends. */
	left_x = fabs(scroll->to_x - x);
	left_y = fabs(scroll->to_y - y);
	if (left_x < SCROLL_GLIDE_STOP && left_y < SCROLL_GLIDE_STOP) {
		scroll->gliding = 0;
		scroll_place(scroll, scroll->to_x, scroll->to_y, now_us);
		return 0;
	}

	/* Succeeded: under way. */
	scroll_place(scroll, x, y, now_us);
	return 1;
}

/*
 * Reports how far the content scrolls across (0 when it fits or does not
 * scroll across).
 */
double
kl_scroll_limit_x(
	const struct kl_scroll *scroll)
{
	/* An axis the scroll does not move along. */
	if ((scroll->axes & KL_SCROLL_X) == 0U)
		return 0.0;

	/* The end kl_scroll_set_bounds gave. */
	if (scroll->bounded)
		return scroll->maximum_x;

	/* Content that fits. */
	if (scroll->content_width <= scroll->viewport_width)
		return 0.0;

	/* Reports what does not show. */
	return scroll->content_width - scroll->viewport_width;
}

/*
 * Reports how far the content scrolls down (0 when it fits or does not
 * scroll down).
 */
double
kl_scroll_limit_y(
	const struct kl_scroll *scroll)
{
	/* An axis the scroll does not move along. */
	if ((scroll->axes & KL_SCROLL_Y) == 0U)
		return 0.0;

	/* The end kl_scroll_set_bounds gave. */
	if (scroll->bounded)
		return scroll->maximum_y;

	/* Content that fits. */
	if (scroll->content_height <= scroll->viewport_height)
		return 0.0;

	/* Reports what does not show. */
	return scroll->content_height - scroll->viewport_height;
}

/*
 * Draws the scroll bars inside a viewport while the content moves and as
 * they fade after it stops.  Returns 1 while they fade (the window should
 * draw again).
 */
int
kl_scroll_draw_bars(
	const struct kl_scroll *scroll,
	struct kl_canvas *canvas,
	const struct kl_rect *viewport,
	const struct kl_theme *theme,
	uint64_t now_us)
{
	double since;
	double alpha;
	double length;
	double place;
	double limit;
	kl_color color;

	/* Never moved, or long enough ago: no bars. */
	if (scroll->moved_us == 0U || now_us < scroll->moved_us)
		return 0;
	since = (double)(now_us - scroll->moved_us);
	if (since >= (double)KL_SCROLL_FADE_US)
		return 0;

	/* The ink, fading over the last half of the time. */
	alpha = SCROLL_BAR_ALPHA;
	if (since > (double)KL_SCROLL_FADE_US / 2.0)
		alpha *= 1.0 - (since - (double)KL_SCROLL_FADE_US / 2.0) / ((double)KL_SCROLL_FADE_US / 2.0);
	color = (theme->icon & 0x00ffffffU) | ((uint32_t)alpha << 24);

	/* The vertical bar, when the content is taller than the viewport. */
	limit = kl_scroll_limit_y(scroll);
	if (limit > 0.0) {
		length = (double)viewport->height * scroll->viewport_height / scroll->content_height;
		if (length < (double)SCROLL_BAR_MIN)
			length = (double)SCROLL_BAR_MIN;
		place = scroll_within(scroll->y, 0.0, limit) / limit * ((double)viewport->height - length - 2.0 * SCROLL_BAR_GAP);
		kl_canvas_round(canvas, (float)(viewport->x + viewport->width - SCROLL_BAR_WIDTH - SCROLL_BAR_GAP),
				 (float)((double)viewport->y + SCROLL_BAR_GAP + place),
				 (float)SCROLL_BAR_WIDTH,
				 (float)length,
				 (float)SCROLL_BAR_WIDTH / 2.0f,
				 color);
	}

	/* The horizontal bar, when the content is wider. */
	limit = kl_scroll_limit_x(scroll);
	if (limit > 0.0) {
		length = (double)viewport->width * scroll->viewport_width / scroll->content_width;
		if (length < (double)SCROLL_BAR_MIN)
			length = (double)SCROLL_BAR_MIN;
		place = scroll_within(scroll->x, 0.0, limit) / limit * ((double)viewport->width - length - 2.0 * SCROLL_BAR_GAP);
		kl_canvas_round(canvas, (float)((double)viewport->x + SCROLL_BAR_GAP + place),
				 (float)(viewport->y + viewport->height - SCROLL_BAR_WIDTH - SCROLL_BAR_GAP),
				 (float)length,
				 (float)SCROLL_BAR_WIDTH,
				 (float)SCROLL_BAR_WIDTH / 2.0f,
				 color);
	}

	/* Succeeded: the bars fade on. */
	return 1;
}

/* Gives the scroller the ends and the rubber band's size (the viewport's unless set; it needs one above zero). */
static void
scroll_bounds(
	struct kl_scroll *scroll)
{
	double minimum_x;
	double minimum_y;
	double width;
	double height;

	/* The band: the viewport, or the size kl_scroll_set_bounds gave. */
	width = scroll->viewport_width;
	height = scroll->viewport_height;
	if (scroll->bounded) {
		width = scroll->band_width;
		height = scroll->band_height;
	}

	/* At least a pixel each way. */
	if (width < 1.0)
		width = 1.0;
	if (height < 1.0)
		height = 1.0;

	/* The ends of each axis (an axis that does not move has none). */
	minimum_x = scroll_minimum_x(scroll);
	minimum_y = scroll_minimum_y(scroll);
	(void)kl_scroller_set_bounds(scroll->scroller, minimum_x, kl_scroll_limit_x(scroll), minimum_y, kl_scroll_limit_y(scroll), width, height);
}

/* Keeps the position within the ends. */
static void
scroll_clamp(
	struct kl_scroll *scroll)
{
	/* Each axis within its ends. */
	scroll->x = scroll_within(scroll->x, scroll_minimum_x(scroll), kl_scroll_limit_x(scroll));
	scroll->y = scroll_within(scroll->y, scroll_minimum_y(scroll), kl_scroll_limit_y(scroll));
}

/* Puts the content at a place, noting when it moved. */
static void
scroll_place(
	struct kl_scroll *scroll,
	double x,
	double y,
	uint64_t now_us)
{
	/* The same place: nothing moved. */
	if (x == scroll->x && y == scroll->y)
		return;

	/* The place, and the time the bars show from. */
	scroll->x = x;
	scroll->y = y;
	scroll->moved_us = now_us;
	if (scroll->moved_us == 0U)
		scroll->moved_us = 1U;
}

/* Reports where the position may go back to across: kl_scroll_set_bounds' minimum, or 0. */
static double
scroll_minimum_x(
	const struct kl_scroll *scroll)
{
	/* An axis the scroll does not move along stays at 0. */
	if ((scroll->axes & KL_SCROLL_X) == 0U)
		return 0.0;

	/* The ends of the sizes start at 0. */
	if (!scroll->bounded)
		return 0.0;

	/* Reports the minimum given. */
	return scroll->minimum_x;
}

/* Reports where the position may go back to down: kl_scroll_set_bounds' minimum, or 0. */
static double
scroll_minimum_y(
	const struct kl_scroll *scroll)
{
	/* An axis the scroll does not move along stays at 0. */
	if ((scroll->axes & KL_SCROLL_Y) == 0U)
		return 0.0;

	/* The ends of the sizes start at 0. */
	if (!scroll->bounded)
		return 0.0;

	/* Reports the minimum given. */
	return scroll->minimum_y;
}

/* Reports a value within minimum..maximum (the minimum wins when they cross). */
static double
scroll_within(
	double value,
	double minimum,
	double maximum)
{
	/* Past the end. */
	if (value > maximum)
		value = maximum;

	/* Below the start. */
	if (value < minimum)
		return minimum;

	/* Within. */
	return value;
}

/*
 * Draws an overlay scroll bar (scroll-bar.c) on a canvas: the faint track
 * while the bar is thick, and the thumb.  Returns 1 while the bar still
 * changes with time (the window should draw again), 0 otherwise.
 */
int
kl_scroll_bar_draw(
	const struct kl_scroll_bar *bar,
	struct kl_canvas *canvas,
	const struct kl_rect *viewport,
	double content,
	double offset,
	uint64_t now_us)
{
	struct kl_scroll_bar_shape shape;
	kl_color track;
	kl_color thumb;
	int shown;
	int busy;

	/* What shows now; nothing is drawn of a bar that does not. */
	shown = kl_scroll_bar_shape(bar, viewport, content, offset, now_us, &shape);
	busy = kl_scroll_bar_busy(bar, now_us);
	if (shown == 0)
		return busy;

	/* The track, light and faint, only while the bar is thick. */
	if (shape.thick != 0) {
		track = 0x00f4f4f4U | ((uint32_t)(150.0 * shape.alpha) << 24);
		kl_canvas_round(canvas, (float)shape.track_x, (float)shape.track_y, (float)shape.track_width,
				 (float)shape.track_height, (float)shape.track_width / 2.0f, track);
	}

	/* The thumb, a dark grey rounded at its ends. */
	thumb = 0x00303030U | ((uint32_t)(SCROLL_BAR_ALPHA * shape.alpha) << 24);
	kl_canvas_round(canvas, (float)shape.thumb_x, (float)shape.thumb_y, (float)shape.thumb_width,
			 (float)shape.thumb_height, (float)shape.thumb_width / 2.0f, thumb);

	/* Succeeded: reports whether it changes on. */
	return busy;
}
