/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The About page: the mark and the system's name, then two cards of
 * values, this computer (its name, processor, graphics and screen) and its
 * software (the system's version, the kernel, the architecture and how
 * long it has run).
 */

#include "settings.h"

#include "include/kern/os-name.h"

#include <stdio.h>
#include <string.h>

/* The hero card's height, the mark's size in it, and the name's and the tagline's text sizes. */
#define ABOUT_HERO_HEIGHT	148
#define ABOUT_MARK		100U
#define ABOUT_TEXT_NAME		44U
#define ABOUT_TEXT_TAGLINE	14U

/*
 * The kernel's name as it is written, which the hero's tagline names
 * ("powered by") while the system is called something else.
 */
#define ABOUT_KERNEL_NAME	"zedBSD"

/*
 * How often the machine's monitor samples while About is open, in
 * milliseconds (ws089-p013): quickly until the first memory has come, so
 * that the row shows at once (q830), then every two seconds.
 */
#define ABOUT_MONITOR_FIRST_MS	250U
#define ABOUT_MONITOR_MS	2000U

/* The space between two cards, and a button's height. */
#define ABOUT_CARD_GAP		18
#define ABOUT_BUTTON_HEIGHT	32

/*
 * One row of a card of values: its label and its value.  The rows of a
 * card are worked out for each frame; the texts belong to the caller and
 * outlive the card's drawing.
 */
struct about_row {
	const char *label;
	const char *value;
};

static int about_hero(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int about_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, const char *title, const struct about_row *rows, int count);
static int about_add(struct about_row *rows, int count, const char *label, const char *value);
static void about_uptime(uint64_t milliseconds, char *text, size_t size);
static void about_memory(uint64_t total, uint64_t free_bytes, char *text, size_t size);

/*
 * Draws the About page's cards from a top edge; returns the edge below
 * them.
 */
int
se_about_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct about_row rows[7];
	const struct se_about *about;
	const char *system;
	char memory[64];
	char text[64];
	int count;
	int y;

	/* The mark and the name. */
	about = &app->about;
	y = about_hero(app, canvas, x, top, width);

	/* This computer: the values that could be read. */
	count = 0;
	count = about_add(rows, count, "Computer name", about->host);
	count = about_add(rows, count, "Processor", about->processor);
	text[0] = '\0';
	if (about->cores > 0U)
		(void)snprintf(text, sizeof(text), "%u", about->cores);
	count = about_add(rows, count, "Processors", text);
	count = about_add(rows, count, "Graphics", about->graphics);
	count = about_add(rows, count, "Display", about->display);
	memory[0] = '\0';
	if (about->memory_known)
		about_memory(about->memory_total, about->memory_free, memory, sizeof(memory));
	count = about_add(rows, count, "Memory", memory);
	y = about_card(app, canvas, x, y + ABOUT_CARD_GAP, width, "This computer", rows, count);

	/* The memory follows the machine's monitor from now (ws089-p013). */
	if (app->monitor == NULL && app->system != NULL) {
		app->monitor = kl_system_monitor_open(app->system, ABOUT_MONITOR_FIRST_MS);
		se_log("ABOUT monitor open=%d", app->monitor != NULL);
	}

	/* Software: the system's version (the build's name without os-release), the kernel, the architecture and the time since the machine started. */
	count = 0;
	system = ZEDBSD_OS_NAME;
	if (about->system[0] != '\0')
		system = about->system;
	count = about_add(rows, count, "Operating system", system);
	count = about_add(rows, count, "Kernel", about->kernel);
	count = about_add(rows, count, "Architecture", about->machine);
	about_uptime(app->now, text, sizeof(text));
	count = about_add(rows, count, "Up for", text);
	y = about_card(app, canvas, x, y + ABOUT_CARD_GAP, width, "Software", rows, count);

	/* The Welcome again (ws164-p002). */
	y += ABOUT_CARD_GAP;
	(void)se_button_draw(app, canvas, x, y, kl_tr("Show Welcome again"), 0, 1, SE_ABOUT_WELCOME);

	/* The edge below the button. */
	return y + ABOUT_BUTTON_HEIGHT;
}

/*
 * Carries out a press of the About page's control: "Show Welcome again"
 * starts the Welcome in the window (ws164-p002).
 */
void
se_about_press(
	struct se_app *app,
	int index)
{
	/* The Welcome. */
	if (index == SE_ABOUT_WELCOME)
		se_welcome_start(app);
}

/* Draws the hero card: the mark, the system's name and the tagline; returns the edge below it. */
static int
about_hero(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct kl_text_line name;
	struct kl_text_line tagline;
	int left;
	int baseline;
	int powered;
	int differs;

	/* The card, untitled. */
	(void)se_card_begin(app, canvas, x, top, width, ABOUT_HERO_HEIGHT, NULL, NULL);

	/* The mark at the card's left, in the middle of its height. */
	se_mark_draw(canvas, x + 28, top + (ABOUT_HERO_HEIGHT - (int)ABOUT_MARK) / 2, ABOUT_MARK, 1.0f);

	/*
	 * The system's name, in slate, beside the mark: in the middle of the
	 * card alone, or above the tagline that names the kernel while the
	 * system is called something other than the kernel.
	 */
	kl_text_metrics(app->text, ABOUT_TEXT_NAME, &name);
	kl_text_metrics(app->text, ABOUT_TEXT_TAGLINE, &tagline);
	left = x + 28 + (int)ABOUT_MARK + 24;
	differs = strcmp(ZEDBSD_OS_NAME, ABOUT_KERNEL_NAME);
	powered = 0;
	if (differs != 0)
		powered = 1;
	if (powered)
		baseline = top + (ABOUT_HERO_HEIGHT - name.height - 6 - tagline.height) / 2 + name.ascent;
	else
		baseline = top + (ABOUT_HERO_HEIGHT - name.height) / 2 + name.ascent;
	(void)kl_text_draw_fit(app->text, canvas, left, baseline, ZEDBSD_OS_NAME, ABOUT_TEXT_NAME, 0, width - (left - x) - 20, SE_COLOR_TITLE);

	/* The tagline under it: the kernel's name as it is written. */
	if (powered) {
		baseline += name.descent + 6 + tagline.ascent;
		(void)kl_text_draw_fit(app->text, canvas, left, baseline, "powered by " ABOUT_KERNEL_NAME, ABOUT_TEXT_TAGLINE, 0, width - (left - x) - 20, SE_COLOR_TEXT_SECONDARY);
	}

	/* The edge below the card. */
	return top + ABOUT_HERO_HEIGHT;
}

/* Draws a titled card of rows of values; returns the edge below it. */
static int
about_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	const char *title,
	const struct about_row *rows,
	int count)
{
	int height;
	int y;
	int index;

	/* The card, as tall as its rows. */
	height = se_card_height(count, 1);
	y = se_card_begin(app, canvas, x, top, width, height, title, NULL);

	/* Each row, a line between two. */
	for (index = 0; index < count; index++)
		y = se_row_value(app, canvas, x, y, width, rows[index].label, rows[index].value, index + 1 == count);

	/* The edge below the card. */
	return top + height;
}

/* Adds a row when its value is known (an empty value is not shown); returns the new count. */
static int
about_add(
	struct about_row *rows,
	int count,
	const char *label,
	const char *value)
{
	/* A value that could not be read is left out. */
	if (value == NULL || value[0] == '\0')
		return count;

	/* The row. */
	rows[count].label = label;
	rows[count].value = value;

	/* One row more. */
	return count + 1;
}

/*
 * Writes how long the machine has run, from the monotonic clock (which
 * starts with the machine), in days, hours and minutes.
 */
static void
about_uptime(
	uint64_t milliseconds,
	char *text,
	size_t size)
{
	unsigned long minutes;
	unsigned long hours;
	unsigned long days;

	/* The whole minutes, hours and days. */
	minutes = (unsigned long)(milliseconds / 60000U);
	hours = minutes / 60UL;
	days = hours / 24UL;
	minutes %= 60UL;
	hours %= 24UL;

	/* The largest units that matter. */
	if (days > 0UL) {
		(void)snprintf(text, size, "%lu d %lu h %lu min", days, hours, minutes);
	} else if (hours > 0UL) {
		(void)snprintf(text, size, "%lu h %lu min", hours, minutes);
	} else {
		(void)snprintf(text, size, "%lu min", minutes);
	}
}

/*
 * Takes the newest frame of the machine's monitor (ws089-p013): the memory
 * About shows, drawn again when it changed (logged the first time).
 */
void
se_about_follow(
	struct se_app *app)
{
	struct kl_monitor_frame frame;
	int taken;
	int first;

	/* A monitor with a new frame of the memory. */
	if (app->monitor == NULL)
		return;
	taken = kl_system_monitor_take(app->monitor, &frame);
	if (!taken || (frame.valid & KL_MONITOR_FRAME_MEMORY) == 0U)
		return;

	/* Kept; About drawn again when it shows. */
	first = !app->about.memory_known;
	app->about.memory_known = 1;
	app->about.memory_total = frame.memory_total;
	app->about.memory_free = frame.memory_free;
	if (app->page == SE_PAGE_ABOUT)
		app->dirty = 1;
	if (!first)
		return;

	/* The first: logged, and the monitor opened again at the slower period. */
	se_log("ABOUT memory total=%llu free=%llu", (unsigned long long)frame.memory_total, (unsigned long long)frame.memory_free);
	kl_system_monitor_close(app->monitor);
	app->monitor = kl_system_monitor_open(app->system, ABOUT_MONITOR_MS);
}

/* Writes the memory as "16 GB (9.3 GB free)". */
static void
about_memory(
	uint64_t total,
	uint64_t free_bytes,
	char *text,
	size_t size)
{
	double gib;
	double free_gib;

	/* In GB (binary), the total whole when it is large. */
	gib = (double)total / (1024.0 * 1024.0 * 1024.0);
	free_gib = (double)free_bytes / (1024.0 * 1024.0 * 1024.0);
	if (gib >= 10.0) {
		(void)snprintf(text, size, "%.0f GB (%.1f GB free)", gib, free_gib);
		return;
	}

	/* Smaller, with a tenth. */
	(void)snprintf(text, size, "%.1f GB (%.1f GB free)", gib, free_gib);
}
