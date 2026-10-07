/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of PDF Viewer's core (ws079-p006): the view, the frame,
 * the document cache and the chooser's request and answer, without Wayland
 * and Vulkan.
 *
 *   host-pdfviewer FONT DOCUMENT.pdf OUTDIR [SKIPPED.pdf [PASSWORD.pdf]]
 *
 * It opens a three-page document in a 1000x760 window and checks, writing
 * a frame of each step to a PPM in OUTDIR: the scroll mode (fit width, a
 * wheel scroll, a drag), the page mode (a sideways drag that turns to the
 * next page, one that springs back, the keys), the zoom (Ctrl+plus, minus
 * and 0), Home and End, the file chooser, a document that cannot be
 * opened, and the Annotate action.  With SKIPPED.pdf (a page with content
 * libpdf leaves out), the notice that some content could not be shown
 * (ws079-p007).  ws079-p015: the sidebar of thumbnails (F9, drawn while
 * the viewer waits, a click shows the page, the column follows the page,
 * a narrow window has none), and with PASSWORD.pdf (DOCUMENT.pdf encrypted
 * with the user password "secret" and the owner password "owner") the
 * password card: a wrong password, the user password typed and Enter,
 * Escape, and the owner password with a click on Open.  BUG-259: while
 * the window is resized the pages' rasters are stretched, and drawn again
 * at the new scale once the size has been still for a moment.
 */

#include "../../../userland/desktop/pdfviewer/viewer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The window's size in the test. */
#define TEST_WIDTH 1000
#define TEST_HEIGHT 760

static struct pv_app app;
static struct pv_text text;
static uint32_t pixels[TEST_WIDTH * TEST_HEIGHT];
static const char *out_dir;
static int failures;
static uint64_t now;

static void frame(const char *name);
static void draw_sized(int width);
static void key(uint32_t code, uint32_t modifiers);
static void wheel(int amount, uint32_t modifiers);
static void drag(int from_x, int from_y, int to_x, int to_y, int steps, int milliseconds);
static void settle(void);
static void check(int condition, const char *what);
static void click(int x, int y);
static void type_text(const char *text);
static int prefetch_all(void);

int
main(
	int argc,
	char **argv)
{
	double before;
	double raster_scale;
	int step;
	int card_x;
	int card_y;
	int card_width;
	int card_height;
	int error;
	size_t folder_length;
	const char *slash;

	if (argc < 4 || argc > 6) {
		fprintf(stderr, "usage: host-pdfviewer FONT DOCUMENT.pdf OUTDIR [SKIPPED.pdf [PASSWORD.pdf]]\n");
		return 2;
	}
	out_dir = argv[3];
	error = pv_text_open(&text, argv[1]);
	check(error == 0, "font opens");
	pv_app_init(&app, &text, TEST_WIDTH, TEST_HEIGHT);
	now = 1000;
	app.now = now;
	frame("00-empty");

	/* A missing file leaves a message. */
	error = pv_app_open(&app, "/nonexistent/missing.pdf");
	check(error != 0 && app.message[0] != '\0' && !app.has_document, "a missing file leaves a message");
	frame("01-missing");

	/* The document in the scroll mode, fitting the width. */
	error = pv_app_open(&app, argv[2]);
	check(error == 0 && app.has_document && app.document.count == 3, "the document opens with 3 pages");
	frame("02-scroll-fit-width");
	check(pv_app_scale(&app, 0) > 1.5 && pv_app_scale(&app, 0) < 1.7, "fit width scales A4 to the window");

	/* The wheel and a drag scroll. */
	wheel(600, 0);
	check(app.scroll_y == 600.0, "the wheel scrolls");
	drag(500, 600, 500, 200, 8, 160);
	check(app.scroll_y == 1000.0, "a drag scrolls with the pointer");
	frame("03-scroll-moved");

	/* End and Home. */
	key(PV_KEY_END, 0);
	check(app.scroll_y > 2000.0, "End goes to the last page");
	frame("04-scroll-end");
	key(PV_KEY_HOME, 0);
	check(app.scroll_y == 0.0, "Home goes to the first page");

	/* The zoom: in twice, out, and back to the fit. */
	before = pv_app_scale(&app, 0);
	key(PV_KEY_EQUAL, PV_MOD_CTRL);
	key(PV_KEY_EQUAL, PV_MOD_CTRL);
	check(app.fit == PV_FIT_CUSTOM && pv_app_scale(&app, 0) > before * 1.5, "Ctrl+plus zooms in");
	frame("05-zoom-in");
	key(PV_KEY_MINUS, PV_MOD_CTRL);
	check(pv_app_scale(&app, 0) < before * 1.3, "Ctrl+minus zooms out");
	key(PV_KEY_0, PV_MOD_CTRL);
	check(app.fit == PV_FIT_WIDTH, "Ctrl+0 returns to the fit");
	pv_app_action(&app, PV_ACTION_FIT_PAGE);
	frame("06-scroll-fit-page");
	check(app.fit == PV_FIT_PAGE, "Fit Page");

	/* The page mode: a swipe to the left turns to the next page. */
	pv_app_action(&app, PV_ACTION_MODE_PAGE);
	check(app.mode == PV_MODE_PAGE && app.page == 0, "the page mode starts on the page in view");
	frame("07-page-1");
	drag(800, 380, 420, 390, 6, 300);
	check(app.turning || app.page == 1, "a long swipe turns");
	settle();
	check(app.page == 1 && app.swipe == 0.0, "the swipe shows page 2");
	frame("09-page-2");

	/* A short, slow swipe springs back. */
	drag(500, 380, 560, 380, 6, 600);
	settle();
	check(app.page == 1, "a short swipe springs back");

	/* A frame in the middle of a swipe shows both pages. */
	app.pressed = 1;
	app.dragging = 1;
	app.swipe = -380.0;
	frame("08-swipe-middle");
	app.pressed = 0;
	app.dragging = 0;
	app.swipe = 0.0;

	/* The keys turn pages. */
	key(PV_KEY_PAGE_DOWN, 0);
	settle();
	check(app.page == 2, "Page Down turns to page 3");
	frame("10-page-3");
	key(PV_KEY_RIGHT, 0);
	settle();
	check(app.page == 2, "Right at the last page stays");
	key(PV_KEY_LEFT, 0);
	settle();
	check(app.page == 1, "Left turns back");
	key(PV_KEY_HOME, 0);
	check(app.page == 0, "Home shows page 1");

	/* Zoom in the page mode, and the scroll mode again keeps the page. */
	key(PV_KEY_EQUAL, PV_MOD_CTRL);
	frame("11-page-zoom");
	key(PV_KEY_0, PV_MOD_CTRL);
	check(app.fit == PV_FIT_PAGE, "Ctrl+0 fits the page in the page mode");
	key(PV_KEY_PAGE_DOWN, 0);
	settle();
	pv_app_action(&app, PV_ACTION_MODE_SCROLL);
	check(app.mode == PV_MODE_SCROLL && app.scroll_y > 1000.0, "the scroll mode keeps page 2 in view");

	/* Annotate asks the window to start Notes. */
	key(PV_KEY_E, PV_MOD_CTRL);
	check(app.want_annotate == 1, "Ctrl+E asks to annotate");

	/*
	 * The chooser (ws090-p008: libkeiland's file chooser, a window of its own
	 * that the program shows while the viewer waits for it): Ctrl+O asks for
	 * it at the document's folder, a cancel leaves the document, and the path
	 * chosen opens.
	 */
	key(PV_KEY_O, PV_MOD_CTRL);
	slash = strrchr(argv[2], '/');
	folder_length = 0U;
	if (slash != NULL)
		folder_length = (size_t)(slash - argv[2]);
	check(app.choosing == 1 && strlen(app.chooser_folder) == folder_length &&
	    strncmp(app.chooser_folder, argv[2], folder_length) == 0, "Ctrl+O asks for the chooser at the document's folder");
	frame("12-chooser");
	pv_app_chosen(&app, NULL);
	check(app.choosing == 0 && app.has_document && app.document.count == 3, "a cancelled chooser leaves the document");
	key(PV_KEY_O, PV_MOD_CTRL);
	pv_app_chosen(&app, argv[2]);
	check(app.choosing == 0 && app.has_document && app.document.count == 3 && strcmp(app.document.path, argv[2]) == 0,
	    "the file chosen opens");

	/* ws079-p015: the sidebar of thumbnails, from the first page of the scroll mode. */
	key(PV_KEY_HOME, 0);
	key(PV_KEY_F9, 0);
	check(app.thumbnails == 1 && pv_app_sidebar_width(&app) == PV_SIDEBAR_WIDTH, "F9 shows the sidebar");
	check(app.width == TEST_WIDTH - PV_SIDEBAR_WIDTH, "the pages are laid out beside the sidebar");
	check(pv_app_scale(&app, 0) < before, "the pages fit the narrower width");
	frame("14-thumbnails-waiting");
	check(app.document.pages[0].thumbnail == NULL, "a thumbnail is not drawn in the frame");
	check(prefetch_all() >= 3, "the viewer draws the thumbnails while it waits");
	check(app.document.pages[2].thumbnail != NULL && app.document.pages[2].thumbnail_width == 104, "the thumbnail fits A4 in the box");
	frame("15-thumbnails");

	/* A click on the third thumbnail shows its page, and the page mode keeps the sidebar. */
	click(80, PV_THUMBNAIL_TOP + 2 * PV_THUMBNAIL_SLOT + 60);
	settle();
	check(pv_app_current_page(&app) == 2, "a click on a thumbnail shows its page");
	frame("16-thumbnail-chosen");
	pv_app_action(&app, PV_ACTION_MODE_PAGE);
	key(PV_KEY_HOME, 0);
	settle();
	check(app.page == 0 && app.thumbnail_followed == 0, "the sidebar follows the page");
	frame("17-thumbnails-page-mode");
	pv_app_action(&app, PV_ACTION_MODE_SCROLL);

	/* A drag that starts on the sidebar does not choose. */
	drag(80, 300, 80, 250, 5, 100);
	check(pv_app_current_page(&app) == 0, "a drag on the sidebar does not choose a page");

	/* A narrow window has no sidebar; F9 hides it. */
	pv_app_resize(&app, 360, TEST_HEIGHT);
	check(pv_app_sidebar_width(&app) == 0 && app.width == 360, "a narrow window has no sidebar");
	pv_app_resize(&app, TEST_WIDTH, TEST_HEIGHT);
	check(pv_app_sidebar_width(&app) == PV_SIDEBAR_WIDTH, "the sidebar comes back with the room");
	key(PV_KEY_F9, 0);
	check(app.thumbnails == 0 && app.width == TEST_WIDTH, "F9 hides the sidebar");

	/* BUG-259: a resize drag stretches the rasters the pages have, and they are drawn again once it settles. */
	now += 250;
	app.now = now;
	pv_app_tick(&app, now);
	draw_sized(TEST_WIDTH);
	raster_scale = app.document.pages[0].raster_scale;
	for (step = 1; step <= 10; step++) {
		now += 16;
		app.now = now;
		pv_app_resize(&app, TEST_WIDTH - 20 * step, TEST_HEIGHT);
		pv_app_tick(&app, now);
		draw_sized(TEST_WIDTH - 20 * step);
	}

	/* The rasters are those before the drag, at the larger scale. */
	check(app.resizing && app.document.pages[0].raster_scale == raster_scale && pv_app_scale(&app, 0) < raster_scale,
	      "a resize drag stretches the pages it has, without drawing them again");
	now += 250;
	app.now = now;
	pv_app_tick(&app, now);
	check(!app.resizing && app.dirty, "the resize settles once the size is still");
	draw_sized(TEST_WIDTH - 200);
	check(app.document.pages[0].raster_scale == pv_app_scale(&app, 0), "the pages are drawn again at the new scale");
	pv_app_resize(&app, TEST_WIDTH, TEST_HEIGHT);
	now += 250;
	app.now = now;
	pv_app_tick(&app, now);

	/* ws079-p007: a document drawn whole has no notice; one with content left out has it. */
	check(!app.notice_shown, "a document drawn whole has no notice");
	if (argc >= 5) {
		error = pv_app_open(&app, argv[4]);
		check(error == 0, "the document with content left out opens");
		frame("13-notice");
		check(app.notice_shown, "content left out shows the notice");
	}

	/* ws079-p015: the password card. */
	if (argc == 6) {
		/* Without a password the card asks. */
		error = pv_app_open(&app, argv[5]);
		check(error == PDF_EPASSWORD && app.asking_password && !app.has_document, "an encrypted document asks for its password");
		frame("18-password");

		/* A wrong password is refused, and the card asks again. */
		type_text("wrong");
		check(app.password_length == 5, "the card takes the keys");
		key(PV_KEY_ENTER, 0);
		check(app.asking_password && app.password_wrong && app.password_length == 0 && !app.has_document,
		    "a wrong password is refused");
		frame("19-password-wrong");

		/* The user password, with a shifted key erased. */
		type_text("secreT");
		key(PV_KEY_BACKSPACE, 0);
		type_text("t");
		check(app.password_length == 6 && !app.password_wrong, "typing clears the refusal");
		frame("20-password-typed");
		key(PV_KEY_ENTER, 0);
		check(!app.asking_password && app.has_document && app.document.count == 3, "the user password opens the document");
		check(app.password[0] == '\0', "the password is not kept");
		frame("21-password-opened");

		/* Escape closes the card without a document. */
		error = pv_app_open(&app, argv[5]);
		check(error == PDF_EPASSWORD && app.asking_password, "the card asks again");
		key(PV_KEY_ESCAPE, 0);
		check(!app.asking_password && !app.has_document, "Escape cancels the card");

		/* The owner password and a click on Open. */
		error = pv_app_open(&app, argv[5]);
		type_text("owner");
		pv_password_layout(&app, &card_x, &card_y, &card_width, &card_height);
		click(card_x + card_width - PV_PASSWORD_BUTTON_INSET - PV_PASSWORD_BUTTON_WIDTH / 2,
		    card_y + card_height - PV_PASSWORD_BUTTON_INSET - PV_PASSWORD_BUTTON_HEIGHT / 2);
		check(!app.asking_password && app.has_document, "the owner password and Open open the document");
	}

	/* Ctrl+W closes the document, and again the window. */
	key(PV_KEY_W, PV_MOD_CTRL);
	check(!app.has_document && app.want_close == 0, "Ctrl+W closes the document");
	key(PV_KEY_W, PV_MOD_CTRL);
	check(app.want_close == 1, "Ctrl+W on an empty window closes it");

	pv_app_release(&app);
	pv_text_close(&text);
	if (failures != 0) {
		printf("host-pdfviewer: %d FAILED\n", failures);
		return 1;
	}
	printf("host-pdfviewer: ok\n");
	return 0;
}

/* Draws a frame and writes it as OUTDIR/NAME.ppm. */
static void
frame(
	const char *name)
{
	struct pv_canvas canvas;
	char path[1024];
	FILE *file;
	size_t index;
	unsigned char rgb[3];

	canvas.pixels = pixels;
	canvas.stride = TEST_WIDTH;
	canvas.width = TEST_WIDTH;
	canvas.height = TEST_HEIGHT;
	pv_draw(&app, &canvas);
	snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, name);
	file = fopen(path, "wb");
	if (file == NULL) {
		check(0, "the frame is written");
		return;
	}
	fprintf(file, "P6\n%d %d\n255\n", TEST_WIDTH, TEST_HEIGHT);
	for (index = 0; index < (size_t)TEST_WIDTH * TEST_HEIGHT; index++) {
		rgb[0] = (unsigned char)(pixels[index] >> 16);
		rgb[1] = (unsigned char)(pixels[index] >> 8);
		rgb[2] = (unsigned char)pixels[index];
		fwrite(rgb, 1, 3, file);
	}
	fclose(file);
}

/* Draws a frame of a width (narrower than the test's window) without writing it. */
static void
draw_sized(
	int width)
{
	struct pv_canvas canvas;

	/* The window's pixels, as narrow as asked. */
	canvas.pixels = pixels;
	canvas.stride = TEST_WIDTH;
	canvas.width = width;
	canvas.height = TEST_HEIGHT;
	pv_draw(&app, &canvas);
}

/* Presses a key. */
static void
key(
	uint32_t code,
	uint32_t modifiers)
{
	struct pv_event event;

	memset(&event, 0, sizeof(event));
	event.type = PV_EVENT_KEY;
	event.key = code;
	event.pressed = 1;
	event.modifiers = modifiers;
	event.time = now;
	pv_app_event(&app, &event);
}

/* Turns the wheel. */
static void
wheel(
	int amount,
	uint32_t modifiers)
{
	struct pv_event event;

	memset(&event, 0, sizeof(event));
	event.type = PV_EVENT_AXIS;
	event.scroll = amount;
	event.modifiers = modifiers;
	event.time = now;
	pv_app_event(&app, &event);
}

/* Drags the pointer with the left button from one place to another in steps over a time. */
static void
drag(
	int from_x,
	int from_y,
	int to_x,
	int to_y,
	int steps,
	int milliseconds)
{
	struct pv_event event;
	int step;

	memset(&event, 0, sizeof(event));
	event.type = PV_EVENT_BUTTON;
	event.button = PV_BUTTON_LEFT;
	event.pressed = 1;
	event.x = from_x;
	event.y = from_y;
	event.time = now;
	pv_app_event(&app, &event);
	for (step = 1; step <= steps; step++) {
		now += (uint64_t)(milliseconds / steps);
		app.now = now;
		memset(&event, 0, sizeof(event));
		event.type = PV_EVENT_MOTION;
		event.x = from_x + (to_x - from_x) * step / steps;
		event.y = from_y + (to_y - from_y) * step / steps;
		event.time = now;
		pv_app_event(&app, &event);
	}
	memset(&event, 0, sizeof(event));
	event.type = PV_EVENT_BUTTON;
	event.button = PV_BUTTON_LEFT;
	event.pressed = 0;
	event.x = to_x;
	event.y = to_y;
	event.time = now;
	pv_app_event(&app, &event);
}

/* Clicks the left button at a place. */
static void
click(
	int x,
	int y)
{
	struct pv_event event;

	memset(&event, 0, sizeof(event));
	event.type = PV_EVENT_BUTTON;
	event.button = PV_BUTTON_LEFT;
	event.pressed = 1;
	event.x = x;
	event.y = y;
	event.time = now;
	pv_app_event(&app, &event);
	event.pressed = 0;
	pv_app_event(&app, &event);
}

/* Types lower-case letters and capitals (with shift) on the US layout. */
static void
type_text(
	const char *text)
{
	static const char letters[] = "qwertyuiopasdfghjklzxcvbnm";
	static const uint32_t codes[] = {
		16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 30, 31, 32, 33, 34, 35, 36, 37, 38, 44, 45, 46, 47, 48, 49, 50
	};
	const char *found;
	char lower;

	for (; *text != '\0'; text++) {
		lower = *text;
		if (lower >= 'A' && lower <= 'Z')
			lower = (char)(lower - 'A' + 'a');
		found = strchr(letters, lower);
		if (found == NULL)
			continue;
		if (*text >= 'A' && *text <= 'Z') {
			key(codes[found - letters], PV_MOD_SHIFT);
		} else {
			key(codes[found - letters], 0);
		}
	}
}

/* Lets the viewer draw ahead as it does while it waits; reports how many drawings it made. */
static int
prefetch_all(void)
{
	int rounds;
	int drawn;

	drawn = 0;
	for (rounds = 0; rounds < 100; rounds++) {
		now += 16;
		pv_app_tick(&app, now);
		if (!pv_app_prefetch(&app))
			break;
		drawn++;
	}
	return drawn;
}

/* Lets time pass until a turn ends. */
static void
settle(void)
{
	int rounds;

	for (rounds = 0; rounds < 100; rounds++) {
		now += 16;
		pv_app_tick(&app, now);
		if (!app.turning)
			break;
	}
}

/* Counts a failed check. */
static void
check(
	int condition,
	const char *what)
{
	if (condition) {
		printf("ok: %s\n", what);
		return;
	}
	printf("FAILED: %s\n", what);
	failures++;
}
