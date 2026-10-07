/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pages of the input and of the sound (ws089-p005):
 *
 *   Mouse     a mouse's speed, acceleration and natural scrolling;
 *   Touchpad  the same for the touch pads (ws089-p024);
 *   Keyboard  how fast a held key repeats and how long before it starts;
 *   Sound     the volume and mute (the system bar's, sound.c, ws100-p005) and
 *             whether the sound service runs.
 *
 * The sliders are saved when let go, the switch when clicked, into the
 * user's preferences (look.c), which the compositor follows within a second.
 */

#include "settings.h"

#include <stdio.h>
#include <string.h>

/* The controls of the input pages (hit indices), past the Appearance page's slider (1). */
#define INPUT_SPEED		2
#define INPUT_NATURAL		3
#define INPUT_RATE		4
#define INPUT_DELAY		5
#define INPUT_ACCEL		6
#define INPUT_PAD_SPEED		7
#define INPUT_PAD_ACCEL		8
#define INPUT_PAD_NATURAL	9

/* The ranges, defaults and steps of the keys, as the compositor takes them (settings-keys.c). */
#define INPUT_SPEED_MIN		25
#define INPUT_SPEED_MAX		300
#define INPUT_SPEED_MOUSE	150
#define INPUT_SPEED_PAD		100
#define INPUT_SPEED_STEP	5
#define INPUT_ACCEL_MIN		0
#define INPUT_ACCEL_MAX		3
#define INPUT_ACCEL_MOUSE	3
#define INPUT_ACCEL_PAD		2
#define INPUT_RATE_MIN		5
#define INPUT_RATE_MAX		60
#define INPUT_RATE_DEFAULT	25
#define INPUT_DELAY_MIN		150
#define INPUT_DELAY_MAX		1000
#define INPUT_DELAY_DEFAULT	400
#define INPUT_DELAY_STEP	10

/* The space between two cards, a card's inner margin, a slider's block, and the text sizes. */
#define INPUT_GAP		16
#define INPUT_PAD		18
#define INPUT_BLOCK		84
#define INPUT_TEXT_TITLE	15U
#define INPUT_TEXT_SMALL	13U

/*
 * One slider of the input pages: its control, its key, its range, its
 * default, its step and the words at its ends.
 */
struct input_slider {
	int index;
	const char *key;
	const char *label;
	int minimum;
	int maximum;
	int fallback;
	int step;
	const char *left;
	const char *right;
};

/* The sliders, in the order of look->sliders (index - INPUT_SPEED, the switch's place unused). */
static const struct input_slider input_sliders[] = {
	{ INPUT_SPEED, "mouse.speed", "Pointer speed", INPUT_SPEED_MIN, INPUT_SPEED_MAX, INPUT_SPEED_MOUSE, INPUT_SPEED_STEP, "Slow", "Fast" },
	{ INPUT_RATE, "keyboard.repeat.rate", "Repeat rate", INPUT_RATE_MIN, INPUT_RATE_MAX, INPUT_RATE_DEFAULT, 1, "Slow", "Fast" },
	{ INPUT_DELAY, "keyboard.repeat.delay", "Delay before repeat", INPUT_DELAY_MIN, INPUT_DELAY_MAX, INPUT_DELAY_DEFAULT, INPUT_DELAY_STEP, "Short", "Long" },
	{ INPUT_ACCEL, "mouse.acceleration", "Acceleration", INPUT_ACCEL_MIN, INPUT_ACCEL_MAX, INPUT_ACCEL_MOUSE, 1, "None", "Strong" },
	{ INPUT_PAD_SPEED, "touchpad.speed", "Pointer speed", INPUT_SPEED_MIN, INPUT_SPEED_MAX, INPUT_SPEED_PAD, INPUT_SPEED_STEP, "Slow", "Fast" },
	{ INPUT_PAD_ACCEL, "touchpad.acceleration", "Acceleration", INPUT_ACCEL_MIN, INPUT_ACCEL_MAX, INPUT_ACCEL_PAD, 1, "None", "Strong" }
};

/* The acceleration's levels in words. */
static const char *const input_accel_words[] = { "None", "Mild", "Medium", "Strong" };

static const struct input_slider *input_slider_of(int index);
static int *input_value(struct se_app *app, int index);
static void input_value_text(int index, int value, char *text, size_t size);
static int input_slider_block(struct se_app *app, struct kl_canvas *canvas, const struct input_slider *slider, int x, int y, int width);
static int input_pointer_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, const char *title, int speed, int acceleration, int natural, int on, const char *line);
static int input_note(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, const char *text);
static int input_saving(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);

/*
 * Draws the Mouse page: a mouse's speed, acceleration and natural
 * scrolling (ws089-p024).  Returns the edge below it.
 */
int
se_mouse_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	int card;
	int y;

	/* A line about saving first, when nothing can be saved. */
	card = input_saving(app, canvas, x, top, width);

	/* The card, then what it applies to. */
	y = input_pointer_card(app, canvas, x, card, width, "Pointer", INPUT_SPEED, INPUT_ACCEL, INPUT_NATURAL, app->look.mouse_natural, "The content moves the way the wheel turns.");
	y = input_note(app, canvas, x, y + INPUT_GAP, width, "These apply to a mouse. A touch pad has its own page; a touch screen or a tablet places the pointer where it is touched.");

	/* The edge below the cards. */
	return y;
}

/*
 * Draws the Touchpad page: the touch pads' speed, acceleration and natural
 * scrolling (ws089-p024).  Returns the edge below it.
 */
int
se_touchpad_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	int card;
	int y;

	/* A line about saving first, when nothing can be saved. */
	card = input_saving(app, canvas, x, top, width);

	/* The card, then the gestures the touch pad knows (WS159 taps, WS142 gestures). */
	y = input_pointer_card(app, canvas, x, card, width, "Pointer", INPUT_PAD_SPEED, INPUT_PAD_ACCEL, INPUT_PAD_NATURAL, app->look.touchpad_natural, "The content moves with two fingers, as on a phone.");
	top = y + INPUT_GAP;
	y = se_card_begin(app, canvas, x, top, width, se_card_height(4, 1), "Gestures", NULL);
	y = se_row_value(app, canvas, x, y, width, "Click", "Tap with one finger", 0);
	y = se_row_value(app, canvas, x, y, width, "Scroll", "Move two fingers", 0);
	y = se_row_value(app, canvas, x, y, width, "Switch windows", "Tap with three fingers", 0);
	(void)se_row_value(app, canvas, x, y, width, "Wiseview, the desktop", "Two fingers in from an edge", 1);
	y = top + se_card_height(4, 1);

	/* The edge below the cards. */
	return y;
}

/*
 * Draws the Keyboard page: the repeat's rate and delay.  Returns the edge
 * below it.
 */
int
se_keyboard_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	int height;
	int card;
	int y;

	/* A line about saving first, when nothing can be saved. */
	card = input_saving(app, canvas, x, top, width);

	/* The card of the repeat: its rate, then its delay. */
	height = se_card_height(0, 1) + 2 * INPUT_BLOCK;
	y = se_card_begin(app, canvas, x, card, width, height, "Key Repeat", NULL);
	y = input_slider_block(app, canvas, input_slider_of(INPUT_RATE), x, y, width);
	y = input_slider_block(app, canvas, input_slider_of(INPUT_DELAY), x, y, width);

	/* When it applies, and what comes later. */
	y = input_note(app, canvas, x, card + height + INPUT_GAP, width, "Applications started from now on repeat keys this way. Keyboard layouts are coming in a later version of Kei.");

	/* The edge below the cards. */
	return y;
}

/*
 * Draws the Sound page: the volume's slider and the mute switch (the
 * system bar's volume, ws100-p005), and whether the sound service runs.
 * Returns the edge below it.
 */
int
se_sound_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct kl_text_line line;
	const char *service;
	const char *note;
	char value[32];
	int available;
	int running;
	int value_width;
	int height;
	int card;
	int y;

	/* A line about saving first, when nothing can be saved. */
	card = input_saving(app, canvas, x, top, width);

	/* The volume's card: its label and value, the slider, the words at its ends, then the mute switch. */
	available = se_sound_available(app);
	height = se_card_height(0, 1) + INPUT_BLOCK + 56;
	y = se_card_begin(app, canvas, x, card, width, height, "Volume", NULL);
	kl_text_metrics(app->text, INPUT_TEXT_TITLE, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + INPUT_PAD + 2, y + line.ascent, "Output volume", INPUT_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
	if (available == 0) {
		/* Without a sound output there is no volume to show (ws089-p012 C4). */
		(void)snprintf(value, sizeof(value), "%s", "\xe2\x80\x94");
	} else if (app->sound.muted) {
		(void)snprintf(value, sizeof(value), "%s", "Muted");
	} else {
		(void)snprintf(value, sizeof(value), "%d%%", app->sound.value);
	}

	/* The value at the right, then the slider (greyed without a sound output). */
	value_width = kl_text_width(app->text, value, strlen(value), INPUT_TEXT_TITLE, 0);
	(void)kl_text_draw(app->text, canvas, x + width - INPUT_PAD - value_width, y + line.ascent, value, strlen(value), INPUT_TEXT_TITLE, 0, SE_COLOR_TEXT_SECONDARY);
	se_slider_draw(app, canvas, x + INPUT_PAD + 14, y + 24, width - 2 * INPUT_PAD - 28, (float)app->sound.value / 100.0f, available, SE_SOUND_VOLUME, &app->sound.slider);

	/* The ends' words. */
	kl_text_metrics(app->text, INPUT_TEXT_SMALL, &line);
	(void)kl_text_draw(app->text, canvas, x + INPUT_PAD + 2, y + 62 + line.ascent, "Quiet", 5U, INPUT_TEXT_SMALL, 0, SE_COLOR_TEXT_FAINT);
	value_width = kl_text_width(app->text, "Loud", 4U, INPUT_TEXT_SMALL, 0);
	(void)kl_text_draw(app->text, canvas, x + width - INPUT_PAD - value_width, y + 62 + line.ascent, "Loud", 4U, INPUT_TEXT_SMALL, 0, SE_COLOR_TEXT_FAINT);
	y += INPUT_BLOCK;

	/* Mute: its label and line at the left, the switch at the right. */
	kl_text_metrics(app->text, INPUT_TEXT_TITLE, &line);
	kl_canvas_line(canvas, (float)(x + INPUT_PAD), (float)y - 0.5f, (float)(x + width - INPUT_PAD), (float)y - 0.5f, 1.0f, SE_COLOR_SEPARATOR);
	(void)kl_text_draw_fit(app->text, canvas, x + INPUT_PAD + 2, y + 10 + line.ascent, "Mute", INPUT_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
	(void)kl_text_draw_fit(app->text, canvas, x + INPUT_PAD + 2, y + 32 + line.ascent, "No sound plays while it is on.", INPUT_TEXT_SMALL, 0, width - 120, SE_COLOR_TEXT_SECONDARY);
	se_toggle_draw(app, canvas, x + width - INPUT_PAD - 44, y + 16, app->sound.muted, available, SE_SOUND_MUTE);

	/* The output's card: the service's state. */
	card += height + INPUT_GAP;
	running = se_sound_running(app);
	service = "Not running";
	if (!app->sound.live)
		service = "Not available on this desktop";
	else if (running != 0 && !app->sound.state.device)
		service = "Running, no sound output";
	else if (running != 0)
		service = "Running";
	y = se_card_begin(app, canvas, x, card, width, se_card_height(1, 1), "Output", NULL);
	(void)se_row_value(app, canvas, x, y, width, "Sound service", service, 1);

	/* What the volume is, and why it cannot be changed now. */
	note = "The system bar's volume icon changes the same volume.";
	if (!app->sound.live)
		note = "The sound is set from this desktop's own settings.";
	else if (!available)
		note = "The volume can be changed when the sound service runs with a sound output.";
	y = input_note(app, canvas, x, card + se_card_height(1, 1) + INPUT_GAP, width, note);

	/* The edge below the cards. */
	return y;
}

/*
 * Carries out a click on a control of the input pages: a switch of
 * natural scrolling, a mouse's or the touch pads'.
 */
void
se_input_press(
	struct se_app *app,
	int index)
{
	/* A mouse's switch turns, and is saved. */
	if (index == INPUT_NATURAL) {
		app->look.mouse_natural = !app->look.mouse_natural;
		se_look_set_number(app, "mouse.natural", app->look.mouse_natural, 0);
		return;
	}

	/* The touch pads' switch turns, and is saved; the sliders are dragged. */
	if (index == INPUT_PAD_NATURAL) {
		app->look.touchpad_natural = !app->look.touchpad_natural;
		se_look_set_number(app, "touchpad.natural", app->look.touchpad_natural, 1);
	}
}

/*
 * Follows a drag on a slider of the input pages: the value moves with the
 * pointer in its steps, and is saved when the button is let go.
 */
void
se_input_drag(
	struct se_app *app,
	int index,
	int x,
	unsigned phase)
{
	const struct input_slider *slider;
	float fraction;
	int *value;
	int number;

	/* Only a slider is dragged. */
	slider = input_slider_of(index);
	if (slider == NULL)
		return;

	/* The value under the pointer, on the slider's steps. */
	fraction = se_slider_fraction(&app->look.sliders[index - INPUT_SPEED], x);
	number = slider->minimum + (int)(fraction * (float)(slider->maximum - slider->minimum) + 0.5f);
	number = (number + slider->step / 2) / slider->step * slider->step;
	if (number < slider->minimum)
		number = slider->minimum;
	if (number > slider->maximum)
		number = slider->maximum;

	/* While held the page shows it; the release saves it. */
	value = input_value(app, index);
	*value = number;
	app->dirty = 1;
	app->look.dragging = 1;
	if (phase != SE_DRAG_END)
		return;

	/* Let go: saved, and the compositor follows. */
	app->look.dragging = 0;
	se_look_set_number(app, slider->key, number, slider->fallback);
}

/* Finds a slider by its control, or NULL. */
static const struct input_slider *
input_slider_of(
	int index)
{
	size_t which;

	/* Each slider's control. */
	for (which = 0; which < sizeof(input_sliders) / sizeof(input_sliders[0]); which++) {
		if (input_sliders[which].index == index)
			return &input_sliders[which];
	}

	/* No slider has that control. */
	return NULL;
}

/* Finds the value a slider shows and moves. */
static int *
input_value(
	struct se_app *app,
	int index)
{
	/* Each slider's value. */
	switch (index) {
	case INPUT_SPEED:
		return &app->look.mouse_speed;
	case INPUT_ACCEL:
		return &app->look.mouse_acceleration;
	case INPUT_PAD_SPEED:
		return &app->look.touchpad_speed;
	case INPUT_PAD_ACCEL:
		return &app->look.touchpad_acceleration;
	case INPUT_RATE:
		return &app->look.repeat_rate;
	default:
		break;
	}

	/* The delay's is the last. */
	return &app->look.repeat_delay;
}

/* Says a slider's value in words: a percentage, keys a second, milliseconds. */
static void
input_value_text(
	int index,
	int value,
	char *text,
	size_t size)
{
	/* Each slider's unit. */
	switch (index) {
	case INPUT_SPEED:
	case INPUT_PAD_SPEED:
		(void)snprintf(text, size, "%d%%", value);
		break;
	case INPUT_ACCEL:
	case INPUT_PAD_ACCEL:
		if (value < INPUT_ACCEL_MIN)
			value = INPUT_ACCEL_MIN;
		if (value > INPUT_ACCEL_MAX)
			value = INPUT_ACCEL_MAX;
		(void)snprintf(text, size, "%s", input_accel_words[value]);
		break;
	case INPUT_RATE:
		(void)snprintf(text, size, "%d a second", value);
		break;
	default:
		(void)snprintf(text, size, "%d ms", value);
		break;
	}
}

/* Draws a slider's block: its label and value, the slider, and the words at its ends. Returns the edge below it. */
static int
input_slider_block(
	struct se_app *app,
	struct kl_canvas *canvas,
	const struct input_slider *slider,
	int x,
	int y,
	int width)
{
	struct kl_text_line line;
	char value[32];
	float fraction;
	int number;
	int enabled;
	int value_width;

	/* The label at the left, the value at the right. */
	number = *input_value(app, slider->index);
	kl_text_metrics(app->text, INPUT_TEXT_TITLE, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + INPUT_PAD + 2, y + line.ascent, slider->label, INPUT_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
	input_value_text(slider->index, number, value, sizeof(value));
	value_width = kl_text_width(app->text, value, strlen(value), INPUT_TEXT_TITLE, 0);
	(void)kl_text_draw(app->text, canvas, x + width - INPUT_PAD - value_width, y + line.ascent, value, strlen(value), INPUT_TEXT_TITLE, 0, SE_COLOR_TEXT_SECONDARY);

	/* The slider; it works only when the settings can be changed. */
	fraction = (float)(number - slider->minimum) / (float)(slider->maximum - slider->minimum);
	enabled = 0;
	if (app->look.writable)
		enabled = 1;
	se_slider_draw(app, canvas, x + INPUT_PAD + 14, y + 24, width - 2 * INPUT_PAD - 28, fraction, enabled, slider->index, &app->look.sliders[slider->index - INPUT_SPEED]);

	/* The ends' words. */
	kl_text_metrics(app->text, INPUT_TEXT_SMALL, &line);
	(void)kl_text_draw(app->text, canvas, x + INPUT_PAD + 2, y + 62 + line.ascent, slider->left, strlen(slider->left), INPUT_TEXT_SMALL, 0, SE_COLOR_TEXT_FAINT);
	value_width = kl_text_width(app->text, slider->right, strlen(slider->right), INPUT_TEXT_SMALL, 0);
	(void)kl_text_draw(app->text, canvas, x + width - INPUT_PAD - value_width, y + 62 + line.ascent, slider->right, strlen(slider->right), INPUT_TEXT_SMALL, 0, SE_COLOR_TEXT_FAINT);

	/* The edge below the block. */
	return y + INPUT_BLOCK;
}

/*
 * Draws a pointer's card: its title, the speed's and the acceleration's
 * sliders, and the switch of natural scrolling (on or not) with its line.
 * Returns the edge below it.
 */
static int
input_pointer_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	const char *title,
	int speed,
	int acceleration,
	int natural,
	int on,
	const char *line_text)
{
	struct kl_text_line line;
	int enabled;
	int height;
	int y;

	/* The card: the two sliders, then the switch. */
	height = se_card_height(0, 1) + 2 * INPUT_BLOCK + 56;
	y = se_card_begin(app, canvas, x, top, width, height, title, NULL);
	y = input_slider_block(app, canvas, input_slider_of(speed), x, y, width);
	y = input_slider_block(app, canvas, input_slider_of(acceleration), x, y, width);

	/* Natural scrolling: its label and line at the left, the switch at the right. */
	kl_text_metrics(app->text, INPUT_TEXT_TITLE, &line);
	kl_canvas_line(canvas, (float)(x + INPUT_PAD), (float)y - 0.5f, (float)(x + width - INPUT_PAD), (float)y - 0.5f, 1.0f, SE_COLOR_SEPARATOR);
	(void)kl_text_draw_fit(app->text, canvas, x + INPUT_PAD + 2, y + 10 + line.ascent, "Natural scrolling", INPUT_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
	(void)kl_text_draw_fit(app->text, canvas, x + INPUT_PAD + 2, y + 32 + line.ascent, line_text, INPUT_TEXT_SMALL, 0, width - 120, SE_COLOR_TEXT_SECONDARY);
	enabled = 0;
	if (app->look.writable)
		enabled = 1;
	se_toggle_draw(app, canvas, x + width - INPUT_PAD - 44, y + 16, on, enabled, natural);

	/* The edge below the card. */
	return top + height;
}

/* Draws a quiet card with one line; returns the edge below it. */
static int
input_note(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	const char *text)
{
	int height;
	int baseline;

	/* One line in a low card. */
	height = 56;
	(void)se_card_begin(app, canvas, x, top, width, height, NULL, NULL);
	baseline = kl_text_center(INPUT_TEXT_SMALL, top, height);
	(void)kl_text_draw_fit(app->text, canvas, x + INPUT_PAD + 2, baseline, text, INPUT_TEXT_SMALL, 0, width - 2 * INPUT_PAD, SE_COLOR_TEXT_SECONDARY);

	/* The edge below the card. */
	return top + height;
}

/* Draws the line about saving (in red), when nothing can be saved or the last save failed; returns the edge below it. */
static int
input_saving(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const char *text;

	/* The last failure, or the lack of a home. */
	text = app->look.message;
	if (text[0] == '\0' && !app->look.writable)
		text = "These settings cannot be changed on this desktop.";
	if (text[0] == '\0')
		return top;

	/* One line in red. */
	(void)kl_text_draw_fit(app->text, canvas, x + 2, top + 16, text, INPUT_TEXT_SMALL, 0, width, SE_COLOR_BAD);

	/* The edge below the line. */
	return top + 30;
}
