/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the status pill's control panel (ws192-p001): the
 * compositor's userland/desktop/wayland/status-panel.c with plane.c, the
 * drawing and the bar's widgets replaced by stand-ins that record what the
 * panel asked of them.  It opens the panel from the pill, presses each of
 * its controls, drags the slider, and closes it the three ways, on a wide
 * and a narrow output.  Each check prints "ok" or "FAIL"; the last line is
 * "status-panel-host-test: PASS" or "FAIL".
 */

#include "userland/desktop/wayland/glass.h"
#include "userland/desktop/wayland/ime.h"

#include <stdio.h>
#include <string.h>

/* The output's size the stand-in reports. */
static int32_t test_output_width = 1280;

/* What the stand-ins were asked, and what they answer. */
static int test_alt;
static int test_network_open;
static int test_network_switches;
static int test_network_opened;
static int test_bluetooth_switches;
static int test_bluetooth_opened;
static int test_bluetooth_shown = 1;
static int test_ime_next;
static unsigned test_volume = 40U;
static unsigned test_muted;
static unsigned test_final;
static int test_slides;
static int test_failures;

/* Records one check. */
static void
check(
	const char *name,
	int passed)
{
	/* The check's outcome, on the standard error (the standard output is the panel's log). */
	if (passed) {
		fprintf(stderr, "%s: ok\n", name);
	} else {
		fprintf(stderr, "%s: FAIL\n", name);
		test_failures++;
	}
}

/* The drawing does nothing on the host. */
void glass_shape_init(struct glass_shape *shape, float x, float y, float width, float height) { memset(shape, 0, sizeof(*shape)); shape->quad[0] = x; shape->quad[1] = y; shape->quad[2] = width; shape->quad[3] = height; }
void glass_shape_draw(struct kwl_server *server, VkCommandBuffer command, const struct glass_shape *shape) { (void)server; (void)command; (void)shape; }
void glass_draw_solid(struct kwl_server *server, VkCommandBuffer command, float x, float y, float width, float height, float radius, const float *color) { (void)server; (void)command; (void)x; (void)y; (void)width; (void)height; (void)radius; (void)color; }
int32_t glass_text_width(struct kwl_server *server, enum glass_size size, const char *text) { (void)server; (void)size; return (int32_t)strlen(text) * 8; }
void glass_draw_text(struct kwl_server *server, VkCommandBuffer command, enum glass_size size, int32_t x, int32_t baseline, const char *text, int32_t limit, const float *color) { (void)server; (void)command; (void)size; (void)x; (void)baseline; (void)text; (void)limit; (void)color; }
void glass_draw_text_middle(struct kwl_server *server, VkCommandBuffer command, enum glass_size size, int32_t x, int32_t baseline, const char *text, int32_t limit, const float *color) { (void)server; (void)command; (void)size; (void)x; (void)baseline; (void)text; (void)limit; (void)color; }
const char *kl_tr(const char *english) { return english; }
void kwl_accent_colour(const struct kwl_server *server, int dark_ground, unsigned part, float alpha, float *out) { (void)server; (void)dark_ground; (void)part; out[0] = 0.0f; out[1] = 0.4f; out[2] = 1.0f; out[3] = alpha; }
unsigned kwl_accent_as_is(struct kwl_server *server) { (void)server; return 0U; }
void kwl_accent_done(struct kwl_server *server, unsigned previous) { (void)server; (void)previous; }

/* One output at the origin, as wide as the test says. */
int kwl_output_rect(struct kwl_server *server, unsigned slot, struct kwl_plane_rect *rect) { (void)server; (void)slot; memset(rect, 0, sizeof(*rect)); rect->width = (uint32_t)test_output_width; rect->height = 800U; return 0; }
int kwl_input_alt_held(const struct kwl_server *server) { (void)server; return test_alt; }

/* The bar's widgets: what the panel reads, and what it asks of them. */
int kwl_network_is_open(void) { return test_network_open; }
int kwl_bluetooth_is_open(void) { return 0; }
int kwl_volume_is_open(void) { return 0; }
int kwl_bluetooth_bar_width(void) { return test_bluetooth_shown ? 34 : 0; }
int32_t kwl_ime_indicator_width(struct kwl_server *server) { (void)server; return 30; }
void kwl_network_panel_state(unsigned *usable, unsigned *on, char *text, size_t size) { *usable = 1U; *on = 1U; (void)snprintf(text, size, "Connected to Kei Lab"); }
void kwl_network_panel_switch(struct kwl_server *server) { (void)server; test_network_switches++; }
void kwl_network_panel_open(struct kwl_server *server, unsigned slot) { (void)server; (void)slot; test_network_opened++; }
void kwl_bluetooth_panel_state(unsigned *usable, unsigned *on, char *text, size_t size) { *usable = 1U; *on = 0U; (void)snprintf(text, size, "Bluetooth is off"); }
void kwl_bluetooth_panel_switch(struct kwl_server *server) { (void)server; test_bluetooth_switches++; }
void kwl_bluetooth_panel_open(struct kwl_server *server, unsigned slot) { (void)server; (void)slot; test_bluetooth_opened++; }
void kwl_volume_panel_state(unsigned *value, unsigned *muted, int *sound) { *value = test_volume; *muted = test_muted; *sound = 1; }
void kwl_volume_panel_slide(struct kwl_server *server, unsigned value, unsigned final) { (void)server; test_volume = value; test_final = final; test_slides++; }
void kwl_volume_panel_mute(struct kwl_server *server) { (void)server; test_muted = !test_muted; }
int kwl_ime_panel_label(struct kwl_server *server, char *label, size_t size) { (void)server; (void)snprintf(label, size, "A"); return 1; }
void kwl_ime_panel_next(struct kwl_server *server) { (void)server; test_ime_next++; }

/* Presses (or releases) the left button at a point. */
static int
press(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	uint32_t state)
{
	int taken;

	/* The pointer there, then the button. */
	server->pointer_x = x;
	server->pointer_y = y;
	taken = kwl_status_panel_button(server, KWL_BUTTON_LEFT, state);

	/* What the panel said. */
	return taken;
}

/* Reads an item's place from the panel's log since it last opened; returns 1 when found. */
static int
item(
	const char *log,
	const char *name,
	int32_t *x,
	int32_t *y,
	int32_t *width,
	int32_t *height)
{
	char wanted[64];
	char line[256];
	FILE *stream;
	int found;

	/* The last line of that item after the last opening. */
	found = 0;
	(void)fflush(stdout);
	(void)snprintf(wanted, sizeof(wanted), "KWL STATUS item name=%s x=", name);
	stream = fopen(log, "r");
	if (stream == NULL)
		return 0;
	while (fgets(line, sizeof(line), stream) != NULL) {
		if (strncmp(line, "KWL STATUS panel open ", 22) == 0)
			found = 0;
		if (strncmp(line, wanted, strlen(wanted)) != 0)
			continue;
		if (sscanf(line + strlen(wanted) - 2, "x=%d y=%d width=%d height=%d", x, y, width, height) == 4)
			found = 1;
	}
	(void)fclose(stream);

	/* Whether it was there. */
	return found;
}

/* Runs the checks; the log of the panel goes to the file named first. */
int
main(
	int argc,
	char **argv)
{
	static struct kwl_server server;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	int32_t panel_x;
	int found;
	int taken;

	/* The log the panel prints, read back for the places of its items. */
	if (argc < 2) {
		fprintf(stderr, "usage: status-panel-host-test LOG\n");
		return 2;
	}
	if (freopen(argv[1], "w", stdout) == NULL)
		return 2;
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* A 1280-wide output with a battery; the pill at 900..1080, the clock's pill ending at 1272. */
	server.power.percent = 80;
	kwl_status_panel_place(&server, 0U, 900, 0, 180, 1272);

	/* A press beside the pill is not the panel's; Alt+click on it is the icons'. */
	taken = press(&server, 880, 20, 1U);
	test_alt = 1;
	taken |= press(&server, 950, 20, 1U);
	test_alt = 0;

	/* A press on the pill opens the panel; its release is the panel's. */
	taken |= !press(&server, 950, 20, 1U);
	taken |= !press(&server, 950, 20, 0U);
	found = item(argv[1], "wifi", &x, &y, &width, &height);
	panel_x = x - 16;
	check("opens from the pill only", taken == 0 && kwl_status_panel_is_open());
	check("lines up with the clock's right edge", found && panel_x + 360 == 1272 && width == 328 && height >= 48);

	/* The Wi-Fi switch, then the rest of its row (the network's menu; the panel goes). */
	found = item(argv[1], "wifi-switch", &x, &y, &width, &height);
	(void)press(&server, x + width / 2, y + height / 2, 1U);
	(void)press(&server, x + width / 2, y + height / 2, 0U);
	check("the Wi-Fi switch switches", found && test_network_switches == 1 && kwl_status_panel_is_open());
	found = item(argv[1], "wifi", &x, &y, &width, &height);
	(void)press(&server, x + 30, y + height / 2, 1U);
	check("the Wi-Fi row opens the network's menu and closes", found && test_network_opened == 1 && !kwl_status_panel_is_open());

	/* With the network's menu up, a press on the pill is the menu's. */
	test_network_open = 1;
	taken = press(&server, 950, 20, 1U);
	test_network_open = 0;
	check("a menu up keeps the pill's press", taken == 0 && !kwl_status_panel_is_open());

	/* Open again: Bluetooth's switch and its row. */
	(void)press(&server, 1000, 20, 1U);
	(void)press(&server, 1000, 20, 0U);
	found = item(argv[1], "bluetooth-switch", &x, &y, &width, &height);
	(void)press(&server, x + 2, y + 2, 1U);
	(void)press(&server, x + 2, y + 2, 0U);
	check("the Bluetooth switch switches", found && test_bluetooth_switches == 1);

	/* Mute, then the slider: a press at the left end, a drag to the right end, the release. */
	found = item(argv[1], "mute", &x, &y, &width, &height);
	(void)press(&server, x + width / 2, y + height / 2, 1U);
	(void)press(&server, x + width / 2, y + height / 2, 0U);
	check("mute switches", found && test_muted == 1U);
	found = item(argv[1], "volume", &x, &y, &width, &height);
	(void)press(&server, x, y + height / 2, 1U);
	check("a press at the slider's left end is 0 without the sound", found && test_volume == 0U && test_final == 0U);
	server.pointer_x = x + width;
	(void)kwl_status_panel_motion(&server);
	check("a drag to the right end is 100", test_volume == 100U && test_final == 0U);
	server.pointer_x = x + width / 2;
	(void)kwl_status_panel_motion(&server);
	(void)press(&server, x + width / 2, y + height / 2, 0U);
	check("the release sets the middle with the sound", test_volume >= 45U && test_volume <= 55U && test_final == 1U);

	/* The input method's row asks for the next language. */
	found = item(argv[1], "input", &x, &y, &width, &height);
	(void)press(&server, x + width / 2, y + height / 2, 1U);
	(void)press(&server, x + width / 2, y + height / 2, 0U);
	check("the input row asks for the next language", found && test_ime_next == 1);

	/* The battery shows: its row is there. */
	found = item(argv[1], "battery", &x, &y, &width, &height);
	check("the battery shows", found && height == 44);

	/* Closed by Esc, by a press outside (taken), and by the pill again. */
	taken = kwl_status_panel_key(&server, 1U, 1U);
	check("Esc closes", taken && !kwl_status_panel_is_open());
	(void)press(&server, 950, 20, 1U);
	(void)press(&server, 950, 20, 0U);
	taken = press(&server, 100, 600, 1U);
	check("a press outside closes and goes no further", taken && !kwl_status_panel_is_open());
	(void)press(&server, 950, 20, 0U);
	(void)press(&server, 950, 20, 1U);
	(void)press(&server, 950, 20, 0U);
	taken = press(&server, 950, 20, 1U);
	check("the pill closes it again", taken && !kwl_status_panel_is_open());
	(void)press(&server, 950, 20, 0U);

	/* A narrow output: the panel keeps its margins; without Bluetooth its row goes. */
	test_output_width = 300;
	test_bluetooth_shown = 0;
	kwl_status_panel_place(&server, 0U, 100, 0, 100, 292);
	(void)press(&server, 150, 20, 1U);
	(void)press(&server, 150, 20, 0U);
	found = item(argv[1], "wifi", &x, &y, &width, &height);
	check("a narrow output keeps the margins", found && x - 16 == 8 && width == 300 - 16 - 32);
	found = item(argv[1], "bluetooth-switch", &x, &y, &width, &height);
	check("no Bluetooth row without a controller", !found);

	/* The verdict. */
	if (test_failures != 0) {
		fprintf(stderr, "status-panel-host-test: FAIL\n");
		return 1;
	}
	fprintf(stderr, "status-panel-host-test: PASS\n");
	return 0;
}
