/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The controls of the library (ws090-p005): the button, the switch and the
 * slider, in Settings' look (plan/ws089, settings/widgets.c: a 32-pixel
 * button with 8-pixel corners, the accent for the main one; a 44 by 24
 * switch whose knob moves to the accent's side).
 *
 * Each is drawn and takes its input in one call (plan/ws090/design.md
 * section 4): a click, a tap, and Enter or Space while it has the focus
 * press a button and flip a switch; a slider follows the pointer or the
 * finger that holds its knob, and the arrows, Page Up, Page Down, Home and
 * End while it has the focus.
 */

#include "internal.h"

#include <math.h>
#include <string.h>

/* A button's side margin, and its label's size. */
#define WIDGETS_BUTTON_SIDE	16
#define WIDGETS_TEXT_BUTTON	14U

/* How much darker a control is under the pointer, and how far a disabled one fades. */
#define WIDGETS_HOVER_SHADE	0.08f

/* The sum of an ink's channels above which it is light (the ground under it is shaded darker). */
#define WIDGETS_INK_MIDDLE	384U
#define WIDGETS_DISABLED_FADE	0.6f

/* The faded ground a disabled control mixes into. */
#define WIDGETS_FADED		kl_theme_choose(KL_RGB(0xeef1f5), KL_RGB(0x2a2e36))

/* The switch's knob: its radius and its distance from the track's ends. */
#define WIDGETS_KNOB_RADIUS	9.5f
#define WIDGETS_KNOB_INSET	12.0f

/* The slider's track thickness and its knob's radius. */
#define WIDGETS_TRACK		4.0f
#define WIDGETS_SLIDER_KNOB	9.0f

/* How many steps Page Up and Page Down move a slider. */
#define WIDGETS_SLIDER_PAGE	10.0

/* The focus ring's gap from the control and its width. */
#define WIDGETS_RING_GAP	3.0f
#define WIDGETS_RING		2.0f

static void widgets_ring(const struct kl_style *style, const struct kl_rect *rect, float radius);
static kl_color widgets_shade(kl_color ground, kl_color ink);
static int widgets_slider_key(uint32_t code, unsigned modifiers);
static double widgets_snap(double value, double minimum, double maximum, double step);

/*
 * Reports how wide a button with a label is.
 */
int
kl_button_width(
	const struct kl_style *style,
	const char *label)
{
	int width;

	/* The label, bold, and a margin each side. */
	width = kl_text_width(style->text, label, strlen(label), WIDGETS_TEXT_BUTTON, 1);

	/* Reports the button's width. */
	return width + 2 * WIDGETS_BUTTON_SIDE;
}

/*
 * Draws a button (KL_BUTTON_* flags) and reports 1 when it was pressed
 * since the last frame.
 */
int
kl_button(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	const struct kl_rect *rect,
	const char *label,
	unsigned flags)
{
	int pressed;

	/* The button alone under its id. */
	pressed = keiui_button(ui, style, id, 0U, rect, label, flags);
	return pressed;
}

/*
 * Draws a button that is one of several under an id (a dialog's) and
 * reports 1 when it was pressed since the last frame.
 */
int
keiui_button(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	uint32_t index,
	const struct kl_rect *rect,
	const char *label,
	unsigned flags)
{
	const struct kl_theme *theme;
	kl_color ground;
	kl_color edge;
	kl_color ink;
	unsigned state;
	int enabled;
	int width;
	int pressed;
	int ring;
	int strong;

	/* The record: a working button takes the keyboard. */
	theme = style->theme;
	enabled = 1;
	if ((flags & KL_BUTTON_DISABLED) != 0U)
		enabled = 0;
	state = 0;
	if (enabled && ui != NULL)
		state = keiui_ui_widget(ui, id, index, rect, KEIUI_FOCUSABLE);
	ring = 0;
	if (ui != NULL)
		ring = keiui_ui_focus_ring(ui);

	/* Its colours: white with an edge, the accent for the main one, red for a dangerous one. */
	ground = theme->control;
	edge = theme->control_edge;
	ink = theme->text;
	if ((flags & KL_BUTTON_PRIMARY) != 0U) {
		ground = theme->accent;
		edge = theme->accent;
		ink = theme->accent_ink;
	}

	/* A dangerous one is red. */
	if ((flags & KL_BUTTON_DANGER) != 0U) {
		ground = theme->danger;
		edge = theme->danger;
		ink = KL_RGB(0xffffff);
	}

	/*
	 * Darker under the pointer or while held; the main and the dangerous
	 * one are shaded away from their ink instead, so that the label keeps
	 * its contrast on any accent (ws179-p001).
	 */
	strong = 0;
	if ((flags & (KL_BUTTON_PRIMARY | KL_BUTTON_DANGER)) != 0U)
		strong = 1;
	if ((state & (KL_HIT_HOT | KL_HIT_ACTIVE)) != 0U) {
		if (strong)
			ground = widgets_shade(ground, ink);
		else
			ground = kl_color_mix(ground, theme->text, WIDGETS_HOVER_SHADE);
	}

	/* Faded when it does nothing: the main and the dangerous one keep their ink a little fainter. */
	if (!enabled) {
		ground = kl_color_mix(ground, WIDGETS_FADED, WIDGETS_DISABLED_FADE);
		edge = kl_color_mix(edge, WIDGETS_FADED, WIDGETS_DISABLED_FADE);
		if (strong)
			ink = KL_RGBA(ink, 220);
		else
			ink = theme->text_faint;
	}

	/* The button, its label in the middle, and the focus's ring. */
	kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, theme->control_radius, ground);
	kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, theme->control_radius, 1.0f, edge);
	width = kl_text_width(style->text, label, strlen(label), WIDGETS_TEXT_BUTTON, 1);
	(void)kl_text_draw(style->text, style->canvas, rect->x + (rect->width - width) / 2, kl_text_center(WIDGETS_TEXT_BUTTON, rect->y, rect->height), label, strlen(label), WIDGETS_TEXT_BUTTON, 1, ink);
	if ((state & KL_HIT_FOCUSED) != 0U && ring)
		widgets_ring(style, rect, theme->control_radius);

	/* A disabled button, or one without the input, is never pressed. */
	if (!enabled || ui == NULL)
		return 0;

	/* Pressed: clicked, tapped, or Enter or Space while it has the focus. */
	pressed = 0;
	if ((state & KL_HIT_CLICKED) != 0U)
		pressed = 1;
	if ((state & KL_HIT_FOCUSED) != 0U)
		pressed |= keiui_ui_take_activate(ui, id, index);

	/* Reports whether it was pressed. */
	return pressed;
}

/*
 * Draws a button that is a picture alone (KL_BUTTON_* flags) and reports
 * 1 when it was pressed since the last frame.
 */
int
kl_icon_button(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	uint32_t index,
	const struct kl_rect *rect,
	enum kl_icon icon,
	int pixels,
	unsigned flags)
{
	const struct kl_theme *theme;
	kl_color ground;
	kl_color edge;
	kl_color ink;
	unsigned state;
	float radius;
	int enabled;
	int pressed;
	int quiet;
	int side;
	int lit;
	int ring;

	/* The record: a working button takes the keyboard. */
	theme = style->theme;
	enabled = 1;
	if ((flags & KL_BUTTON_DISABLED) != 0U)
		enabled = 0;
	state = 0;
	if (enabled && ui != NULL)
		state = keiui_ui_widget(ui, id, index, rect, KEIUI_FOCUSABLE);
	ring = 0;
	if (ui != NULL)
		ring = keiui_ui_focus_ring(ui);
	lit = 0;
	if ((state & (KL_HIT_HOT | KL_HIT_ACTIVE)) != 0U)
		lit = 1;

	/* Its shape: the control's rounded square, or a circle as wide as its shorter side. */
	radius = theme->control_radius;
	if ((flags & KL_BUTTON_ROUND) != 0U) {
		side = rect->width;
		if (rect->height < side)
			side = rect->height;
		radius = (float)side * 0.5f;
	}

	/* Its colours: white within an edge and the main ink, darker under the pointer or while held. */
	quiet = 0;
	if ((flags & KL_BUTTON_QUIET) != 0U)
		quiet = 1;
	ground = theme->control;
	edge = theme->control_edge;
	ink = theme->text;
	if (lit)
		ground = kl_color_mix(ground, theme->text, WIDGETS_HOVER_SHADE);

	/* A quiet one: the secondary ink, and only the hover's ground under the pointer or while held. */
	if (quiet) {
		ground = 0;
		edge = 0;
		ink = theme->text_secondary;
		if (lit)
			ground = theme->hover;
	}

	/* Faded when it does nothing: the faint ink, and a faded ground and edge (a quiet one has none). */
	if (!enabled) {
		ink = theme->text_faint;
		if (!quiet) {
			ground = kl_color_mix(ground, WIDGETS_FADED, WIDGETS_DISABLED_FADE);
			edge = kl_color_mix(edge, WIDGETS_FADED, WIDGETS_DISABLED_FADE);
		}
	}

	/* The ground and its edge (none for a quiet one at rest), the icon in the middle, and the focus's ring. */
	if (ground != 0)
		kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, radius, ground);
	if (edge != 0)
		kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, radius, 1.0f, edge);
	kl_icon_draw(style->canvas, icon, (float)rect->x + (float)(rect->width - pixels) * 0.5f, (float)rect->y + (float)(rect->height - pixels) * 0.5f, (float)pixels, ink);
	if ((state & KL_HIT_FOCUSED) != 0U && ring)
		widgets_ring(style, rect, radius);

	/* A disabled button, or one without the input, is never pressed. */
	if (!enabled || ui == NULL)
		return 0;

	/* Pressed: clicked, tapped, or Enter or Space while it has the focus. */
	pressed = 0;
	if ((state & KL_HIT_CLICKED) != 0U)
		pressed = 1;
	if ((state & KL_HIT_FOCUSED) != 0U)
		pressed |= keiui_ui_take_activate(ui, id, index);

	/* Reports whether it was pressed. */
	return pressed;
}

/*
 * Draws a switch with its top left at (x, y) (the theme's size) and flips
 * *on when it is pressed; reports 1 when it changed.  flags:
 * KL_BUTTON_DISABLED.
 */
int
kl_switch(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	int x,
	int y,
	int *on,
	unsigned flags)
{
	const struct kl_theme *theme;
	struct kl_rect rect;
	struct kl_rect reach;
	kl_color track;
	kl_color knob;
	unsigned state;
	float knob_x;
	int ring;
	int changed;
	int enabled;

	/* The record, a little larger than the switch for a finger. */
	theme = style->theme;
	rect.x = x;
	rect.y = y;
	rect.width = theme->switch_width;
	rect.height = theme->switch_height;
	reach.x = x - 4;
	reach.y = y - 4;
	reach.width = rect.width + 8;
	reach.height = rect.height + 8;
	enabled = 1;
	if ((flags & KL_BUTTON_DISABLED) != 0U)
		enabled = 0;
	state = 0;
	if (enabled)
		state = keiui_ui_widget(ui, id, 0U, &reach, KEIUI_FOCUSABLE);
	ring = keiui_ui_focus_ring(ui);

	/* Flipped: clicked, tapped, or Enter or Space while it has the focus. */
	changed = 0;
	if ((state & KL_HIT_CLICKED) != 0U)
		changed = 1;
	if ((state & KL_HIT_FOCUSED) != 0U)
		changed |= keiui_ui_take_activate(ui, id, 0U);

	/* The switch flips. */
	if (changed)
		*on = !*on;

	/* The track's colour and the knob's place. */
	track = theme->track;
	knob_x = (float)x + WIDGETS_KNOB_INSET;
	if (*on) {
		track = theme->accent;
		knob_x = (float)(x + rect.width) - WIDGETS_KNOB_INSET;
	}

	/* The knob is white; a switch that does nothing is faded. */
	knob = KL_RGB(0xffffff);
	if (!enabled) {
		track = kl_color_mix(track, WIDGETS_FADED, WIDGETS_DISABLED_FADE);
		knob = KL_RGB(0xf6f7f9);
	}

	/* The track, the knob and the focus's ring. */
	kl_canvas_round(style->canvas, (float)x, (float)y, (float)rect.width, (float)rect.height, (float)rect.height * 0.5f, track);
	kl_canvas_circle(style->canvas, knob_x, (float)y + (float)rect.height * 0.5f, WIDGETS_KNOB_RADIUS, knob);
	if ((state & KL_HIT_FOCUSED) != 0U && ring)
		widgets_ring(style, &rect, (float)rect.height * 0.5f);

	/* Reports whether it changed. */
	return changed;
}

/*
 * Draws a slider across a rectangle for *value within minimum..maximum
 * (on steps of step, none when 0) and moves the value with the input;
 * reports 1 when it changed.
 */
int
kl_slider(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	const struct kl_rect *rect,
	double minimum,
	double maximum,
	double step,
	double *value)
{
	int changed;

	/* An enabled slider. */
	changed = kl_slider_flags(ui, style, id, rect, minimum, maximum, step, value, 0U);
	return changed;
}

/*
 * Draws a slider as kl_slider does, with flags (KL_VERSION 48,
 * ws090-p023): KL_BUTTON_DISABLED draws it faded, takes no input and
 * leaves the value.  Reports 1 when the value changed.
 */
int
kl_slider_flags(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	const struct kl_rect *rect,
	double minimum,
	double maximum,
	double step,
	double *value,
	unsigned flags)
{
	const struct kl_theme *theme;
	kl_color fill;
	kl_color knob_color;
	int enabled;
	uint32_t code;
	unsigned modifiers;
	unsigned state;
	double before;
	double share;
	double key_step;
	double pointer_x;
	double pointer_y;
	float left;
	float right;
	float middle;
	int ring;
	float knob;
	int taken;

	/* The record: it takes the keyboard and a drag (a disabled one nothing). */
	theme = style->theme;
	enabled = 1;
	if ((flags & KL_BUTTON_DISABLED) != 0U)
		enabled = 0;
	state = 0U;
	if (enabled)
		state = keiui_ui_widget(ui, id, 0U, rect, KEIUI_FOCUSABLE | KEIUI_DRAGGABLE);
	ring = keiui_ui_focus_ring(ui);
	before = *value;
	left = (float)rect->x + WIDGETS_SLIDER_KNOB;
	right = (float)(rect->x + rect->width) - WIDGETS_SLIDER_KNOB;

	/* Held or clicked: the value where the pointer or the finger is along the track. */
	if ((state & (KL_HIT_ACTIVE | KL_HIT_CLICKED)) != 0U && right > left) {
		kl_ui_pointer(ui, &pointer_x, &pointer_y);
		share = ((double)pointer_x - (double)left) / (double)(right - left);
		*value = widgets_snap(minimum + share * (maximum - minimum), minimum, maximum, step);
	}

	/* The keys while it has the focus (a disabled one has none): a step, a page, the ends. */
	key_step = step;
	if (key_step <= 0.0)
		key_step = (maximum - minimum) / 100.0;
	for (;;) {
		taken = 0;
		if (enabled)
			taken = keiui_ui_take_key(ui, id, 0U, widgets_slider_key, &code, &modifiers);
		if (!taken)
			break;
		switch (code) {
		case KL_KEY_LEFT:
		case KL_KEY_DOWN:
			*value -= key_step;
			break;
		case KL_KEY_RIGHT:
		case KL_KEY_UP:
			*value += key_step;
			break;
		case KL_KEY_PAGEDOWN:
			*value -= key_step * WIDGETS_SLIDER_PAGE;
			break;
		case KL_KEY_PAGEUP:
			*value += key_step * WIDGETS_SLIDER_PAGE;
			break;
		case KL_KEY_HOME:
			*value = minimum;
			break;
		case KL_KEY_END:
			*value = maximum;
			break;
		default:
			break;
		}

		/* Kept on a step, inside the range. */
		*value = widgets_snap(*value, minimum, maximum, step);
	}

	/* The knob's place along the track. */
	share = 0.0;
	if (maximum > minimum)
		share = (*value - minimum) / (maximum - minimum);
	knob = left + (float)share * (right - left);
	middle = (float)rect->y + (float)rect->height * 0.5f;

	/* The accent and the knob, faded for a slider that does nothing. */
	fill = theme->accent;
	knob_color = KL_RGB(0xffffff);
	if (!enabled) {
		fill = kl_color_mix(fill, WIDGETS_FADED, WIDGETS_DISABLED_FADE);
		knob_color = WIDGETS_FADED;
	}

	/* The track, the part up to the knob in the accent, the knob with a soft edge, and the focus's ring. */
	kl_canvas_round(style->canvas, left, middle - WIDGETS_TRACK * 0.5f, right - left, WIDGETS_TRACK, WIDGETS_TRACK * 0.5f, theme->track);
	kl_canvas_round(style->canvas, left, middle - WIDGETS_TRACK * 0.5f, knob - left, WIDGETS_TRACK, WIDGETS_TRACK * 0.5f, fill);
	kl_canvas_circle(style->canvas, knob, middle + 1.0f, WIDGETS_SLIDER_KNOB + 1.0f, KL_RGBA(0x1f3a66, 40));
	kl_canvas_circle(style->canvas, knob, middle, WIDGETS_SLIDER_KNOB, knob_color);
	kl_canvas_ring(style->canvas, knob, middle, WIDGETS_SLIDER_KNOB, 1.0f, 1.0f, theme->control_edge);
	if ((state & KL_HIT_FOCUSED) != 0U && ring)
		widgets_ring(style, rect, (float)rect->height * 0.5f);

	/* Reports whether the value changed. */
	if (*value != before)
		return 1;
	return 0;
}

/* Draws the keyboard focus's ring around a control. */
static void
widgets_ring(
	const struct kl_style *style,
	const struct kl_rect *rect,
	float radius)
{
	/* A ring of the accent a little outside the control. */
	kl_canvas_round_border(style->canvas, (float)rect->x - WIDGETS_RING_GAP, (float)rect->y - WIDGETS_RING_GAP,
				(float)rect->width + 2.0f * WIDGETS_RING_GAP,
				(float)rect->height + 2.0f * WIDGETS_RING_GAP,
				radius + WIDGETS_RING_GAP,
				WIDGETS_RING,
				KL_RGBA(style->theme->accent, 150));
}

/* Shades a ground away from the ink drawn on it: darker under a light ink, lighter under a dark one. */
static kl_color
widgets_shade(
	kl_color ground,
	kl_color ink)
{
	unsigned brightness;
	kl_color shaded;

	/* How bright the ink is: the sum of its channels. */
	brightness = ((ink >> 16) & 0xffU) + ((ink >> 8) & 0xffU) + (ink & 0xffU);

	/* A light ink: the ground darker; a dark one: lighter. */
	if (brightness > WIDGETS_INK_MIDDLE)
		shaded = kl_color_mix(ground, KL_RGB(0x000000), WIDGETS_HOVER_SHADE);
	else
		shaded = kl_color_mix(ground, KL_RGB(0xffffff), WIDGETS_HOVER_SHADE);

	/* The shaded ground. */
	return shaded;
}

/* Keeps a value within its ends and on its steps (none when step is 0). */
static double
widgets_snap(
	double value,
	double minimum,
	double maximum,
	double step)
{
	double steps;

	/* On a step from the minimum. */
	if (step > 0.0) {
		steps = floor((value - minimum) / step + 0.5);
		value = minimum + steps * step;
	}

	/* Within the ends. */
	if (value < minimum)
		value = minimum;
	if (value > maximum)
		value = maximum;

	/* Reports the value. */
	return value;
}

/* Tells whether a slider takes a key: the arrows, Page Up and Page Down, Home and End (without Control or Alt). */
static int
widgets_slider_key(
	uint32_t code,
	unsigned modifiers)
{
	/* A command is the application's. */
	if ((modifiers & (KL_MOD_CTRL | KL_MOD_ALT)) != 0U)
		return 0;

	/* The keys that move the value. */
	switch (code) {
	case KL_KEY_LEFT:
	case KL_KEY_RIGHT:
	case KL_KEY_UP:
	case KL_KEY_DOWN:
	case KL_KEY_PAGEUP:
	case KL_KEY_PAGEDOWN:
	case KL_KEY_HOME:
	case KL_KEY_END:
		return 1;
	default:
		break;
	}

	/* Any other. */
	return 0;
}
