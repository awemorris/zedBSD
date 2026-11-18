/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Power page (ws052-p013; the design is plan/ws052/phase007/phase.md
 * section 8, the 2026-10-05 user decision N4): how long without input
 * before the computer sleeps, on the power adapter and on battery (Never,
 * 5, 10, 15, 30, 60 or 120 minutes; 30 and 15 unless chosen).  The screen
 * goes out at half that time; that is said, not chosen.  A computer that
 * cannot sleep says so, and the times still put the screen out at half.
 *
 * The two sliders step through the times; the release saves the time into
 * the user's preferences (look.c: power.sleep.ac and power.sleep.battery),
 * which the compositor follows at once (its sleep.c).  The battery's
 * charge is shown in the system bar; the page does not show it yet.
 *
 * A second card has how long without input before the screen locks
 * (Never, 1, 2, 5, 10, 15, 30 or 60 minutes; Never unless chosen), saved
 * as power.lock, which the compositor also follows at once (its
 * settings.c).  A sleep locks the screen whatever this says.
 */

#include "settings.h"

#include <stdio.h>
#include <string.h>

/* The three sliders (hit indices), and their places in look->sliders. */
#define POWER_AC		2
#define POWER_BATTERY		3
#define POWER_LOCK		4

/* The card's margin, a slider's block, and the text sizes. */
#define POWER_PAD		18
#define POWER_GAP		16
#define POWER_BLOCK		84
#define POWER_TEXT_TITLE	15U
#define POWER_TEXT_SMALL	13U

/* The number of the sleep's times, and of the lock's. */
#define POWER_SLEEP_STEPS	7
#define POWER_LOCK_STEPS	8

/*
 * The times one slider steps through, in minutes (0 is never), and their
 * number; each points at a constant table and lives for the program.
 */
struct power_steps {
	const int *minutes;
	int count;
};

/* The sleep's times, the same on the power adapter and on battery. */
static const int power_sleep_minutes[POWER_SLEEP_STEPS] = { 0, 5, 10, 15, 30, 60, 120 };

/* The lock's times: shorter ones than the sleep's, an hour at most. */
static const int power_lock_minutes[POWER_LOCK_STEPS] = { 0, 1, 2, 5, 10, 15, 30, 60 };

/* The sleep sliders' steps, constant. */
static const struct power_steps power_sleep = { power_sleep_minutes, POWER_SLEEP_STEPS };

/* The lock slider's steps, constant. */
static const struct power_steps power_lock = { power_lock_minutes, POWER_LOCK_STEPS };

static int power_block(struct se_app *app, struct kl_canvas *canvas, int index, const struct power_steps *steps, const char *label, int minutes, int x, int y, int width);
static int power_step_of(const struct power_steps *steps, int minutes);
static void power_text(int minutes, char *text, size_t size);
static int power_can_sleep(struct se_app *app);

/*
 * Draws the Power page: the card of the sleep's two times and the line
 * about the screen, then the card of the lock's time.  Returns the edge
 * below them.
 */
int
se_power_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const char *line;
	int height;
	int can;
	int y;

	/* What the card says under its title: whether the computer can sleep. */
	can = power_can_sleep(app);
	line = kl_tr("The screen turns off after half that time.");
	if (!can)
		line = kl_tr("This computer cannot sleep. The screen turns off after half that time.");

	/* The card: the adapter's time, then the battery's. */
	height = se_card_height(0, 1) + 2 * POWER_BLOCK;
	y = se_card_begin(app, canvas, x, top, width, height, kl_tr("Sleep"), line);
	y = power_block(app, canvas, POWER_AC, &power_sleep, kl_tr("Sleep after, on the power adapter"), app->look.sleep_ac, x, y, width);
	(void)power_block(app, canvas, POWER_BATTERY, &power_sleep, kl_tr("Sleep after, on battery"), app->look.sleep_battery, x, y, width);

	/* The lock's card under it. */
	top += height + POWER_GAP;
	height = se_card_height(0, 1) + POWER_BLOCK;
	y = se_card_begin(app, canvas, x, top, width, height, kl_tr("Screen Lock"), kl_tr("The screen locks after this long without input. A sleep locks it too."));
	(void)power_block(app, canvas, POWER_LOCK, &power_lock, kl_tr("Lock the screen after"), app->look.lock_minutes, x, y, width);

	/* The edge below the cards. */
	return top + height + POWER_GAP;
}

/*
 * Moves a slider of the Power page to the time under the pointer, and saves
 * it when it is let go.
 */
void
se_power_drag(
	struct se_app *app,
	int index,
	int x,
	unsigned phase)
{
	const struct power_steps *steps;
	const char *key;
	float fraction;
	int *value;
	int step;

	/* Only the three sliders are dragged. */
	if (index == POWER_AC) {
		key = "power.sleep.ac";
		value = &app->look.sleep_ac;
		steps = &power_sleep;
	} else if (index == POWER_BATTERY) {
		key = "power.sleep.battery";
		value = &app->look.sleep_battery;
		steps = &power_sleep;
	} else if (index == POWER_LOCK) {
		key = "power.lock";
		value = &app->look.lock_minutes;
		steps = &power_lock;
	} else {
		return;
	}

	/* The time under the pointer, on the slider's steps. */
	fraction = se_slider_fraction(&app->look.sliders[index - POWER_AC], x);
	step = (int)(fraction * (float)(steps->count - 1) + 0.5f);
	if (step < 0)
		step = 0;
	if (step > steps->count - 1)
		step = steps->count - 1;

	/* While held the page shows it; the release saves it. */
	*value = steps->minutes[step];
	app->dirty = 1;
	app->look.dragging = 1;
	if (phase != SE_DRAG_END)
		return;

	/* Let go: saved, and the compositor follows. */
	app->look.dragging = 0;
	se_look_set_number(app, key, *value, 0);
}

/* Draws one slider's block: its label and time, the slider, and the ends' words; returns the edge below it. */
static int
power_block(
	struct se_app *app,
	struct kl_canvas *canvas,
	int index,
	const struct power_steps *steps,
	const char *label,
	int minutes,
	int x,
	int y,
	int width)
{
	struct kl_text_line line;
	const char *left;
	const char *right;
	char value[48];
	float fraction;
	int enabled;
	int value_width;
	int step;

	/* The label at the left, the time at the right. */
	kl_text_metrics(app->text, POWER_TEXT_TITLE, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + POWER_PAD + 2, y + line.ascent, label, POWER_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
	power_text(minutes, value, sizeof(value));
	value_width = kl_text_width(app->text, value, strlen(value), POWER_TEXT_TITLE, 0);
	(void)kl_text_draw(app->text, canvas, x + width - POWER_PAD - value_width, y + line.ascent, value, strlen(value), POWER_TEXT_TITLE, 0, SE_COLOR_TEXT_SECONDARY);

	/* The slider at the time's step; it works only when the settings can be changed. */
	step = power_step_of(steps, minutes);
	fraction = (float)step / (float)(steps->count - 1);
	enabled = 0;
	if (app->look.writable)
		enabled = 1;
	se_slider_draw(app, canvas, x + POWER_PAD + 14, y + 24, width - 2 * POWER_PAD - 28, fraction, enabled, index, &app->look.sliders[index - POWER_AC]);

	/* The ends' words: the lock's longest time is an hour, the sleep's two. */
	left = kl_tr("Never");
	right = kl_tr("2 hours");
	if (steps == &power_lock)
		right = kl_tr("1 hour");
	kl_text_metrics(app->text, POWER_TEXT_SMALL, &line);
	(void)kl_text_draw(app->text, canvas, x + POWER_PAD + 2, y + 62 + line.ascent, left, strlen(left), POWER_TEXT_SMALL, 0, SE_COLOR_TEXT_FAINT);
	value_width = kl_text_width(app->text, right, strlen(right), POWER_TEXT_SMALL, 0);
	(void)kl_text_draw(app->text, canvas, x + width - POWER_PAD - value_width, y + 62 + line.ascent, right, strlen(right), POWER_TEXT_SMALL, 0, SE_COLOR_TEXT_FAINT);

	/* The edge below the block. */
	return y + POWER_BLOCK;
}

/* Finds the step nearest a time (a time set elsewhere need not be one of the steps). */
static int
power_step_of(
	const struct power_steps *steps,
	int minutes)
{
	int nearest;
	int distance;
	int best;
	int step;

	/* Never is its own step. */
	if (minutes <= 0)
		return 0;

	/* The step whose time is closest. */
	nearest = 1;
	best = -1;
	for (step = 1; step < steps->count; step++) {
		distance = steps->minutes[step] - minutes;
		if (distance < 0)
			distance = -distance;
		if (best < 0 || distance < best) {
			best = distance;
			nearest = step;
		}
	}

	/* Succeeded: the nearest step. */
	return nearest;
}

/* Writes a time in words: "Never", or a number of minutes. */
static void
power_text(
	int minutes,
	char *text,
	size_t size)
{
	char number[16];

	/* Never. */
	if (minutes <= 0) {
		(void)snprintf(text, size, "%s", kl_tr("Never"));
		return;
	}

	/* The minutes, in the language's plural. */
	(void)snprintf(number, sizeof(number), "%d", minutes);
	(void)kl_tr_format(text, size, kl_trn("{1} minute", "{1} minutes", (unsigned long)minutes), number, (const char *)NULL);
}

/* Tells whether the computer can sleep: the desktop offers the sleep among the power's actions. */
static int
power_can_sleep(
	struct se_app *app)
{
	struct kl_power_state state;
	unsigned bit;

	/* Without the system extension nothing is known: the times are shown as for a computer that sleeps. */
	if (app->system == NULL)
		return 1;

	/* The actions the desktop offers now. */
	memset(&state, 0, sizeof(state));
	kl_system_power_get_state(app->system, &state);
	bit = KL_POWER_ACTION_BIT(KL_POWER_SUSPEND);
	if ((state.actions & bit) == 0U)
		return 0;

	/* Succeeded: the computer can sleep. */
	return 1;
}
