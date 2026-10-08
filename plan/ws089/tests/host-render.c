/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws089: drives Settings' interface on the host and draws its frames into
 * PPM pictures, without Wayland or Vulkan.
 *
 *   settings-render [OPTION]... ACTION...
 *
 * The preferences are the test's own, in build/ws089-host/render-home
 * (HOME is set to it; run from the top of the tree).
 *
 * Options (before the actions):
 *   --font=PATH        the font (default userland/desktop/fonts/Mahora-Regular.ttf)
 *   --size=WxH         the window's size (default 1180x800)
 *   --page=WORD        the page shown first (default Home)
 *   --network=SCENARIO a made-up network (host-network.c: wifi, wired, absent, down; default wifi)
 *   --dark             the dark appearance's colours (ws089-p017, palette.c)
 * With HOST_ACCOUNT_RESULT=ERRNO in the environment the desktop offers the account (host-kl-system.c, ws160-p002):
 * the system is a stand-in, polled after each action, and a password change is answered with that errno.
 *
 * Actions, run in order:
 *   move=X,Y  click=X,Y  scroll=PIXELS  key=CODE[:MODS]  action=N
 *   drag=X,Y,X2        (presses at X,Y, moves to X2 and lets go there)
 *   touch=X,Y,X2,Y2    (a finger touches at X,Y, moves to X2,Y2 in four steps and lifts there, ws089-p012)
 *   text=STRING        (types lower-case letters and digits as keys)
 *   control=N          (clicks the page's control N of the last frame)
 *   tb=CONTROL:DETAIL  (a titlebar control chosen)
 *   search=TEXT        (the titlebar's search field's text as typed; '_' is a space)
 *   searchdone=HOW     (the search field's editing ended: 0 Enter, 1 Esc, 2 left)
 *   result=N           (clicks the search's result N of the last frame)
 *   draw=PATH          (draws the frame into a PPM picture)
 *   peek=PATH          (writes the frame the last action drew, without drawing again: a lit row drawn alone)
 *   hits               (prints the clickable regions of the last frame)
 *   state              (prints what the titlebar and the menus show)
 */

#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void host_network_fake(struct se_app *app, const char *scenario);
static int host_write_ppm(const char *path, const uint32_t *pixels, int width, int height);
static void host_event(struct se_app *app, unsigned type, int x, int y, uint32_t button, int pressed, uint32_t key, uint32_t modifiers);
static void host_finger(struct se_app *app, unsigned type, int x, int y, int pressed);

int
main(
	int argc,
	char **argv)
{
	static struct se_app app;
	static struct kl_text text;
	struct kl_canvas canvas;
	struct se_titlebar_state titlebar;
	struct se_titlebar_event event;
	struct se_event wheel;
	struct se_menu_state menu;
	const struct se_page *page;
	const char *font;
	const char *scenario;
	const char *typed;
	static const char letters[] = "qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";
	static const char digits[] = "1234567890";
	const char *found;
	char home[1024];
	char *space;
	uint32_t *pixels;
	unsigned kind;
	unsigned start;
	unsigned code;
	unsigned mods;
	int width;
	int height;
	int x;
	int y;
	int to_x;
	int to_y;
	int step;
	int index;
	int error;

	/* The options. */
	font = "userland/desktop/fonts/Mahora-Regular.ttf";
	width = SE_WIDTH;
	height = SE_HEIGHT;
	start = SE_PAGE_HOME;
	scenario = "wifi";
	for (index = 1; index < argc && strncmp(argv[index], "--", 2) == 0; index++) {
		if (strncmp(argv[index], "--font=", 7) == 0)
			font = argv[index] + 7;
		if (strncmp(argv[index], "--size=", 7) == 0)
			(void)sscanf(argv[index] + 7, "%dx%d", &width, &height);
		if (strncmp(argv[index], "--network=", 10) == 0)
			scenario = argv[index] + 10;
		if (strcmp(argv[index], "--dark") == 0)
			se_palette_set(KL_APPEARANCE_DARK);
		if (strncmp(argv[index], "--page=", 7) == 0) {
			page = se_page_find(argv[index] + 7);
			if (page != NULL)
				start = page->id;
		}
	}

	/* The font, the canvas and the interface. */
	/* The tree's companions (Mahora Bold, the monospaced fallback): the host has no installed ones (ws090-p023). */
	kl_text_companions("userland/desktop/fonts/Mahora-Bold.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf");
	error = kl_text_open(&text, font, NULL);
	if (error != 0) {
		fprintf(stderr, "font %s: %d\n", font, error);
		return 1;
	}
	pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
	if (pixels == NULL)
		return 1;
	error = kl_canvas_init(&canvas, pixels, (size_t)width, width, height);
	if (error != 0)
		return 1;
	/* About's names are the desktop's (ws188-p002); the host test shows fixed ones. */
	(void)snprintf(app.about.system, sizeof(app.about.system), "%s", "Host test");
	(void)snprintf(app.about.kernel, sizeof(app.about.kernel), "%s", "Host kernel");
	(void)snprintf(app.about.machine, sizeof(app.about.machine), "%s", "host");
	(void)snprintf(app.about.graphics, sizeof(app.about.graphics), "%s", "Host test (no GPU)");
	(void)snprintf(app.about.display, sizeof(app.about.display), "%dx%d", width, height);
	app.now = 3723000U;
	host_network_fake(&app, scenario);

	/*
	 * A home of the test's own under build/ (never the
	 * real home): build/ws089-host/render-home, an absolute path.
	 */
	if (getcwd(home, sizeof(home) - 64) == NULL)
		return 1;
	strcat(home, "/build/ws089-host/render-home");
	(void)mkdir(home, 0700);
	setenv("HOME", home, 1);
	se_look_open(&app, NULL);
	se_ui_init(&app, &text, start);
	if (getenv("HOST_ACCOUNT_RESULT") != NULL)
		app.system = (struct kl_system *)&app;
	se_ui_draw(&app, &canvas);

	/* Each action, a frame after it (and the stand-in system's answers taken). */
	for (; index < argc; index++) {
		if (app.system != NULL)
			se_system_poll(&app);
		if (sscanf(argv[index], "move=%d,%d", &x, &y) == 2) {
			host_event(&app, SE_EVENT_MOTION, x, y, 0, 0, 0, 0);
		} else if (sscanf(argv[index], "click=%d,%d", &x, &y) == 2) {
			host_event(&app, SE_EVENT_MOTION, x, y, 0, 0, 0, 0);
			host_event(&app, SE_EVENT_BUTTON, x, y, SE_BUTTON_LEFT, 1, 0, 0);
			host_event(&app, SE_EVENT_BUTTON, x, y, SE_BUTTON_LEFT, 0, 0, 0);
		} else if (sscanf(argv[index], "drag=%d,%d,%u", &x, &y, &code) == 3) {
			host_event(&app, SE_EVENT_MOTION, x, y, 0, 0, 0, 0);
			host_event(&app, SE_EVENT_BUTTON, x, y, SE_BUTTON_LEFT, 1, 0, 0);
			host_event(&app, SE_EVENT_MOTION, (x + (int)code) / 2, y, 0, 0, 0, 0);
			host_event(&app, SE_EVENT_MOTION, (int)code, y, 0, 0, 0, 0);
			host_event(&app, SE_EVENT_BUTTON, (int)code, y, SE_BUTTON_LEFT, 0, 0, 0);
		} else if (sscanf(argv[index], "touch=%d,%d,%d,%d", &x, &y, &to_x, &to_y) == 4) {
			host_finger(&app, SE_EVENT_MOTION, x, y, 0);
			host_finger(&app, SE_EVENT_BUTTON, x, y, 1);
			for (step = 1; step <= 4; step++)
				host_finger(&app, SE_EVENT_MOTION, x + (to_x - x) * step / 4, y + (to_y - y) * step / 4, 0);
			host_finger(&app, SE_EVENT_BUTTON, to_x, to_y, 0);
		} else if (sscanf(argv[index], "scroll=%d", &y) == 1) {
			memset(&wheel, 0, sizeof(wheel));
			wheel.type = SE_EVENT_AXIS;
			wheel.x = app.layout.page.x + 40;
			wheel.y = app.layout.page.y + 40;
			wheel.scroll = y;
			se_ui_event(&app, &wheel);
		} else if (sscanf(argv[index], "key=%u:%u", &code, &mods) == 2) {
			host_event(&app, SE_EVENT_KEY, 0, 0, 0, 1, code, mods);
			host_event(&app, SE_EVENT_KEY, 0, 0, 0, 0, code, mods);
		} else if (sscanf(argv[index], "key=%u", &code) == 1) {
			host_event(&app, SE_EVENT_KEY, 0, 0, 0, 1, code, 0);
			host_event(&app, SE_EVENT_KEY, 0, 0, 0, 0, code, 0);
		} else if (strncmp(argv[index], "text=", 5) == 0) {
			for (typed = argv[index] + 5; *typed != '\0'; typed++) {
				found = strchr(digits, *typed);
				code = 0;
				if (found != NULL)
					code = 2U + (unsigned)(found - digits);
				found = memchr(letters, *typed, sizeof(letters) - 1U);
				if (*typed >= 'a' && *typed <= 'z' && found != NULL)
					code = 16U + (unsigned)(found - letters);
				if (code != 0U) {
					host_event(&app, SE_EVENT_KEY, 0, 0, 0, 1, code, 0);
					host_event(&app, SE_EVENT_KEY, 0, 0, 0, 0, code, 0);
				}
			}
		} else if (strncmp(argv[index], "search=", 7) == 0) {
			memset(&event, 0, sizeof(event));
			event.kind = SE_TITLEBAR_CHANGED;
			event.id = SE_CONTROL_SEARCH;
			(void)snprintf(event.text, sizeof(event.text), "%s", argv[index] + 7);
			for (space = event.text; *space != '\0'; space++) {
				if (*space == '_')
					*space = ' ';
			}
			se_ui_titlebar(&app, &event);
		} else if (sscanf(argv[index], "searchdone=%u", &code) == 1) {
			memset(&event, 0, sizeof(event));
			event.kind = SE_TITLEBAR_DONE;
			event.id = SE_CONTROL_SEARCH;
			event.detail = code;
			(void)snprintf(event.text, sizeof(event.text), "%s", app.search.query);
			se_ui_titlebar(&app, &event);
		} else if (sscanf(argv[index], "control=%u", &code) == 1 || sscanf(argv[index], "result=%u", &code) == 1) {
			kind = SE_HIT_CONTROL;
			if (strncmp(argv[index], "result=", 7) == 0)
				kind = SE_HIT_RESULT;
			for (x = app.hit_count - 1; x >= 0; x--) {
				if (app.hits[x].kind == kind && app.hits[x].index == (int)code)
					break;
			}
			if (x < 0) {
				fprintf(stderr, "no control %u\n", code);
				return 2;
			}
			y = app.hits[x].rect.y + app.hits[x].rect.height / 2;
			code = (unsigned)(app.hits[x].rect.x + app.hits[x].rect.width / 2);
			host_event(&app, SE_EVENT_MOTION, (int)code, y, 0, 0, 0, 0);
			host_event(&app, SE_EVENT_BUTTON, (int)code, y, SE_BUTTON_LEFT, 1, 0, 0);
			host_event(&app, SE_EVENT_BUTTON, (int)code, y, SE_BUTTON_LEFT, 0, 0, 0);
		} else if (sscanf(argv[index], "action=%u", &code) == 1) {
			se_ui_action(&app, code);
		} else if (sscanf(argv[index], "tb=%u:%u", &code, &mods) == 2) {
			memset(&event, 0, sizeof(event));
			event.kind = SE_TITLEBAR_ACTIVATED;
			event.id = code;
			event.detail = mods;
			se_ui_titlebar(&app, &event);
		} else if (strncmp(argv[index], "peek=", 5) == 0) {
			/* The pixels as the last action's frame left them, not drawn again (BUG-226: a lit row drawn alone). */
			error = host_write_ppm(argv[index] + 5, pixels, width, height);
			if (error != 0)
				return 1;
		} else if (strncmp(argv[index], "draw=", 5) == 0) {
			/* The Wallpaper page's small copies (BUG-152, read by a thread) are waited for, so the frame is the settled one. */
			while (se_look_wait(&app) >= 0) {
				(void)usleep(10000);
				se_look_poll(&app, app.now);
			}

			/* The Storage page's counts (ws089-p023, on threads) are waited for too. */
			se_storage_poll(&app, app.now);
			while (se_storage_wait(&app) >= 0) {
				(void)usleep(10000);
				app.now += 10U;
				se_storage_poll(&app, app.now);
			}
			se_ui_draw(&app, &canvas);
			if (app.dirty != 0)
				se_ui_draw(&app, &canvas);
			error = host_write_ppm(argv[index] + 5, pixels, width, height);
			if (error != 0)
				return 1;
		} else if (strcmp(argv[index], "hits") == 0) {
			for (x = 0; x < app.hit_count; x++)
				printf("HIT kind=%u index=%d x=%d y=%d w=%d h=%d\n", app.hits[x].kind, app.hits[x].index, app.hits[x].rect.x, app.hits[x].rect.y, app.hits[x].rect.width, app.hits[x].rect.height);
		} else if (strcmp(argv[index], "state") == 0) {
			se_ui_titlebar_state(&app, &titlebar);
			se_ui_menu_state(&app, &menu);
			printf("STATE page=%s back=%d forward=%d parts=%d last=%s sidebar=%d menu-page=%u search=%d query=%s results=%u focus=%u\n", se_pages[app.page].word, titlebar.can_back, titlebar.can_forward, titlebar.part_count, titlebar.parts[titlebar.part_count - 1], titlebar.sidebar, menu.page, app.search.active, titlebar.query, app.search.count, titlebar.focus_serial);
		} else {
			fprintf(stderr, "unknown action %s\n", argv[index]);
			return 2;
		}
		se_ui_draw(&app, &canvas);
	}

	/* Done. */
	kl_canvas_release(&canvas);
	free(pixels);
	kl_text_close(&text);
	return 0;
}

/* Hands one input to the interface. */
static void
host_event(
	struct se_app *app,
	unsigned type,
	int x,
	int y,
	uint32_t button,
	int pressed,
	uint32_t key,
	uint32_t modifiers)
{
	struct se_event event;

	/* The input. */
	memset(&event, 0, sizeof(event));
	event.type = type;
	event.x = x;
	event.y = y;
	event.button = button;
	event.pressed = pressed;
	event.key = key;
	event.modifiers = modifiers;
	event.time = app->now;
	se_ui_event(app, &event);
}

/* Writes premultiplied pixels over white as a binary PPM; nonzero on failure. */
static int
host_write_ppm(
	const char *path,
	const uint32_t *pixels,
	int width,
	int height)
{
	FILE *file;
	uint32_t pixel;
	unsigned alpha;
	unsigned char rgb[3];
	int index;
	int channel;

	/* The file. */
	file = fopen(path, "wb");
	if (file == NULL)
		return -1;
	fprintf(file, "P6\n%d %d\n255\n", width, height);

	/* Each pixel over a pale ground (premultiplied: colour + ground * (1 - alpha)). */
	for (index = 0; index < width * height; index++) {
		pixel = pixels[index];
		alpha = pixel >> 24;
		for (channel = 0; channel < 3; channel++)
			rgb[channel] = (unsigned char)(((pixel >> (16 - 8 * channel)) & 0xffU) + (0xd8U * (255U - alpha)) / 255U);
		fwrite(rgb, 1, 3, file);
	}

	/* Done. */
	fclose(file);
	return 0;
}

/* Gives the interface a finger's move or press on the touch screen (the pointer's events, marked as a finger's). */
static void
host_finger(
	struct se_app *app,
	unsigned type,
	int x,
	int y,
	int pressed)
{
	struct se_event event;

	/* The input, as the window makes it of a finger. */
	memset(&event, 0, sizeof(event));
	event.type = type;
	event.x = x;
	event.y = y;
	event.button = SE_BUTTON_LEFT;
	event.pressed = pressed;
	event.time = app->now;
	event.touch = 1;
	se_ui_event(app, &event);
}
