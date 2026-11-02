/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the compositor's log lines of pointer presses no client
 * hears (seat.c, "KWL PRESS"): it links seat.c with stand-ins for the rest
 * of the compositor, drives kwl_seat_button, and reads the lines back from
 * the file stdout was sent to.  It checks that a press the shell takes
 * names what took it, that a press and release a client hears write
 * nothing, that the release of a press a client heard but the shell took is
 * named with how long it was held, that presses with no surface or no
 * wl_pointer are named, that the lines are limited per second, and that
 * the delivery itself is unchanged (the client's events are counted).
 */

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/wayland/menu.h"
#include "userland/desktop/wayland/titlebar.h"
#include "userland/desktop/wayland/popup.h"
#include "userland/desktop/wayland/toplevel.h"
#include "userland/desktop/wayland/subsurface.h"
#include "userland/desktop/wayland/data.h"
#include "userland/desktop/wayland/extras.h"
#include "userland/desktop/wayland/keymap.h"
#include "userland/desktop/wayland/ime.h"
#include "userland/desktop/wayland/super-tap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTN_LEFT_CODE	0x110U

/* What the stand-in glass look does with the next button: 0 passes it, 1 takes it naming glass_by. */
static int glass_takes;
static const char *glass_by;

/* The stand-in clock. */
static uint64_t now_ms = 5000U;

/* The client's events (kwl_emit), by opcode. */
static unsigned emitted_button;

/* The checks that failed. */
static unsigned failures;
static unsigned checks;

static struct kwl_server server;
static struct kwl_client client;
static struct kwl_object surface;
static struct kwl_object pointer;

/* ---- the stand-ins ---- */

int kwl_arrange_key(struct kwl_server *s, uint32_t key, uint32_t state) { (void)s; (void)key; (void)state; return 0; }
int kwl_arrange_move_end(struct kwl_server *s) { (void)s; return 0; }
int kwl_bluetooth_key(struct kwl_server *s, uint32_t key, uint32_t state) { (void)s; (void)key; (void)state; return 0; }
struct kwl_object *kwl_create(struct kwl_client *c, uint32_t id, enum kwl_kind kind, uint32_t version) { (void)c; (void)id; (void)kind; (void)version; return NULL; }
void kwl_cursor_frame(struct kwl_server *s, uint32_t edges) { (void)s; (void)edges; }
void kwl_data_drag_cancel(struct kwl_server *s) { (void)s; }
void kwl_data_drag_motion(struct kwl_server *s, uint32_t time) { (void)s; (void)time; }
void kwl_data_drag_release(struct kwl_server *s) { (void)s; }
void kwl_data_focus(struct kwl_server *s, struct kwl_object *focus) { (void)s; (void)focus; }
int kwl_displays_key(struct kwl_server *s, uint32_t key, uint32_t state) { (void)s; (void)key; (void)state; return 0; }
int
kwl_emit(struct kwl_client *c, uint32_t object, uint32_t opcode, const void *payload, size_t size)
{
	(void)c; (void)payload; (void)size;
	if (object == pointer.id && opcode == 3U)
		emitted_button++;
	return 0;
}
int kwl_emit_fd(struct kwl_client *c, uint32_t object, uint32_t opcode, const void *payload, size_t size, int descriptor) { (void)c; (void)object; (void)opcode; (void)payload; (void)size; (void)descriptor; return 0; }
struct kwl_object *kwl_find(struct kwl_client *c, uint32_t id) { (void)c; (void)id; return NULL; }
int
kwl_glass_button(struct kwl_server *s, uint32_t button, uint32_t state, const char **by)
{
	(void)s; (void)button; (void)state;
	if (!glass_takes)
		return 0;
	if (by != NULL && glass_by != NULL)
		*by = glass_by;
	return 1;
}
int kwl_glass_edge_button(struct kwl_server *s, uint32_t button, uint32_t state, const char **by) { (void)s; (void)button; (void)state; (void)by; return 0; }
int kwl_glass_edge_motion(struct kwl_server *s) { (void)s; return 0; }
int kwl_glass_fullscreen_input(struct kwl_server *s) { (void)s; return 0; }
int kwl_glass_key(struct kwl_server *s, uint32_t key, uint32_t state) { (void)s; (void)key; (void)state; return 0; }
int kwl_glass_motion(struct kwl_server *s) { (void)s; return 0; }
struct kwl_object *
kwl_glass_part_at(struct kwl_server *s, int32_t x, int32_t y, const char **part)
{
	(void)s; (void)x; (void)y;
	*part = "body";
	return &surface;
}
void kwl_glass_toplevel_move_end(struct kwl_server *s, struct kwl_object *w) { (void)s; (void)w; }
int kwl_greeter_button(struct kwl_server *s, uint32_t button, uint32_t state) { (void)s; (void)button; (void)state; return 1; }
int kwl_greeter_key(struct kwl_server *s, uint32_t key, uint32_t state) { (void)s; (void)key; (void)state; return 0; }
void kwl_greeter_motion(struct kwl_server *s) { (void)s; }
void kwl_greeter_wheel(struct kwl_server *s, int32_t vertical) { (void)s; (void)vertical; }
int kwl_home_axis(struct kwl_server *s, int32_t vertical, int32_t horizontal) { (void)s; (void)vertical; (void)horizontal; return 0; }
int kwl_home_key(struct kwl_server *s, uint32_t key, uint32_t state) { (void)s; (void)key; (void)state; return 0; }
void kwl_home_toggle(struct kwl_server *s, const char *via) { (void)s; (void)via; }
int kwl_ime_field_key(struct kwl_server *s, uint32_t time, uint32_t key, uint32_t state) { (void)s; (void)time; (void)key; (void)state; return 0; }
void kwl_ime_focus(struct kwl_server *s, struct kwl_object *previous) { (void)s; (void)previous; }
int kwl_ime_home_key(struct kwl_server *s, uint32_t time, uint32_t key, uint32_t state) { (void)s; (void)time; (void)key; (void)state; return 0; }
int kwl_ime_key_early(struct kwl_server *s, uint32_t time, uint32_t key, uint32_t state) { (void)s; (void)time; (void)key; (void)state; return 0; }
int kwl_ime_key_grab(struct kwl_server *s, uint32_t time, uint32_t key, uint32_t state, int composing_only) { (void)s; (void)time; (void)key; (void)state; (void)composing_only; return 0; }
void kwl_ime_modifiers(struct kwl_server *s) { (void)s; }
void kwl_keyboard_key_typed(struct kwl_server *s, uint32_t state) { (void)s; (void)state; }
int kwl_keymap_descriptor(uint32_t *size) { *size = 0U; return -1; }
int kwl_lock(struct kwl_server *s, const char *reason) { (void)s; (void)reason; return 0; }
uint64_t kwl_log_us(void) { return now_ms * 1000U; }
int kwl_menu_grab_key(struct kwl_server *s, uint32_t key, uint32_t state) { (void)s; (void)key; (void)state; return 0; }
int kwl_menu_key(struct kwl_server *s, uint32_t key, uint32_t state) { (void)s; (void)key; (void)state; return 0; }
uint64_t kwl_milliseconds(void) { return now_ms; }
int kwl_network_key(struct kwl_server *s, uint32_t key, uint32_t state) { (void)s; (void)key; (void)state; return 0; }
void kwl_object_destroy(struct kwl_object *object) { (void)object; }
void kwl_ping_send(struct kwl_client *c) { (void)c; }
int kwl_popup_button(struct kwl_server *s, uint32_t button, uint32_t state) { (void)s; (void)button; (void)state; return 0; }
struct kwl_object *kwl_popup_chain_at(struct kwl_server *s) { (void)s; return NULL; }
struct kwl_object *kwl_popup_focus(struct kwl_server *s, struct kwl_object *target) { (void)s; return target; }
void kwl_primary_focus(struct kwl_server *s, struct kwl_object *focus) { (void)s; (void)focus; }
struct kwl_object *kwl_subsurface_at(struct kwl_object *root, int32_t x, int32_t y) { (void)root; (void)x; (void)y; return NULL; }
void kwl_super_tap_cancel(struct kwl_super_tap *tap) { (void)tap; }
int kwl_super_tap_key(struct kwl_super_tap *tap, uint32_t key, uint32_t state, uint32_t others_held, uint64_t now) { (void)tap; (void)key; (void)state; (void)others_held; (void)now; return 0; }
int kwl_titlebar_axis(struct kwl_server *s, int32_t vertical, int32_t horizontal) { (void)s; (void)vertical; (void)horizontal; return 0; }
int kwl_titlebar_key(struct kwl_server *s, uint32_t key, uint32_t state) { (void)s; (void)key; (void)state; return 0; }
void kwl_titlebar_key_released(uint32_t key) { (void)key; }
int kwl_titlebar_tab_key(struct kwl_server *s, uint32_t key, uint32_t state) { (void)s; (void)key; (void)state; return 0; }
int kwl_toplevel_button(struct kwl_server *s, uint32_t state) { (void)s; (void)state; return 0; }
int kwl_toplevel_motion(struct kwl_server *s) { (void)s; return 0; }
int kwl_volume_axis(struct kwl_server *s, int32_t vertical, int32_t horizontal) { (void)s; (void)vertical; (void)horizontal; return 0; }

/* ---- the test ---- */

/* The log file stdout goes to, and how far it has been read. */
static const char *log_path;
static long log_read;

/* Reads the lines written since the last call into text (NUL-terminated). */
static void
take_lines(char *text, size_t room)
{
	FILE *file;
	size_t got;

	fflush(stdout);
	text[0] = '\0';
	file = fopen(log_path, "r");
	if (file == NULL)
		return;
	(void)fseek(file, log_read, SEEK_SET);
	got = fread(text, 1, room - 1U, file);
	text[got] = '\0';
	log_read += (long)got;
	fclose(file);
}

/* Counts the lines of text that hold a piece. */
static unsigned
count_lines(const char *text, const char *piece)
{
	const char *at;
	unsigned count;

	count = 0U;
	for (at = strstr(text, piece); at != NULL; at = strstr(at + 1, piece))
		count++;
	return count;
}

static void
check(int ok, const char *what, const char *text)
{
	checks++;
	if (ok)
		return;
	failures++;
	fprintf(stderr, "FAIL: %s\n--- lines ---\n%s-------------\n", what, text);
}

/* One button at the stand-in clock, which then moves on by step ms. */
static void
button(uint32_t state, uint64_t step)
{
	kwl_seat_button(&server, (uint32_t)now_ms, BTN_LEFT_CODE, state);
	now_ms += step;
}

int
main(int argc, char **argv)
{
	static char text[65536];
	unsigned before;
	unsigned index;

	if (argc != 2) {
		fprintf(stderr, "usage: host-press-log LOGFILE\n");
		return 2;
	}
	log_path = argv[1];
	if (freopen(log_path, "w", stdout) == NULL) {
		fprintf(stderr, "cannot write %s\n", log_path);
		return 2;
	}

	/* One client with one window and one wl_pointer, focused, in the glass look. */
	client.server = &server;
	client.number = 7U;
	client.objects = &surface;
	surface.client = &client;
	surface.kind = KWL_SURFACE;
	surface.id = 21U;
	surface.next = &pointer;
	snprintf(surface.app_id, sizeof(surface.app_id), "%s", "org.example.settings");
	pointer.client = &client;
	pointer.kind = KWL_POINTER;
	pointer.id = 30U;
	pointer.version = 7U;
	server.clients = &client;
	server.focus = &surface;
	server.glass = 1U;
	server.windowed = 1U;
	server.pointer_x = 400;
	server.pointer_y = 300;

	/* 1. A short click the client hears writes nothing, and the client hears press and release. */
	before = emitted_button;
	glass_takes = 0;
	button(1U, 45U);
	button(0U, 10U);
	take_lines(text, sizeof(text));
	check(count_lines(text, "KWL PRESS") == 0U, "a click the client hears writes no line", text);
	check(emitted_button - before == 2U, "the client hears the press and the release", text);

	/* 2. A press the glass look takes names what took it, and the client hears nothing. */
	before = emitted_button;
	glass_takes = 1;
	glass_by = "titlebar";
	button(1U, 30U);
	take_lines(text, sizeof(text));
	check(count_lines(text, "KWL PRESS press-kept reason=titlebar button=272 source=pointer x=400 y=300 output=0 under=21 part=body under_app=org.example.settings focus=21 role=other client=7 app=org.example.settings pointer=21 ") == 1U,
	    "a press the title bar takes is named with its point, window and focus", text);
	check(emitted_button == before, "the client hears nothing of a kept press", text);

	/* Its release (also taken) writes nothing: no client had the press. */
	button(0U, 10U);
	take_lines(text, sizeof(text));
	check(count_lines(text, "KWL PRESS") == 0U, "the release of a kept press writes no line", text);

	/* 3. A press the client hears whose release the shell keeps: release-kept with how long it was held. */
	glass_takes = 0;
	button(1U, 52U);
	glass_takes = 1;
	glass_by = "title-tap";
	button(0U, 10U);
	take_lines(text, sizeof(text));
	check(count_lines(text, "KWL PRESS release-kept reason=title-tap ") == 1U && strstr(text, " held_ms=52 ") != NULL,
	    "the release of a press the client heard, kept by the shell, is named with held_ms", text);

	/* 4. A press the glass look takes without naming a part is named glass. */
	glass_by = NULL;
	button(1U, 10U);
	button(0U, 10U);
	take_lines(text, sizeof(text));
	check(count_lines(text, "KWL PRESS press-kept reason=glass ") == 1U, "a press the glass look takes unnamed is reason=glass", text);

	/* 5. The lock screen takes the press. */
	glass_takes = 0;
	server.locked = 1U;
	button(1U, 10U);
	button(0U, 10U);
	server.locked = 0U;
	take_lines(text, sizeof(text));
	check(count_lines(text, "KWL PRESS press-kept reason=lock ") == 1U, "a press on the lock screen is reason=lock", text);

	/* 6. With no surface focused the press is lost. */
	server.focus = NULL;
	button(1U, 10U);
	button(0U, 10U);
	take_lines(text, sizeof(text));
	check(count_lines(text, "KWL PRESS press-lost reason=no-surface ") == 1U && strstr(text, " focus=0 role=none client=0 app=- pointer=0 ") != NULL,
	    "a press with no surface is press-lost reason=no-surface", text);
	server.focus = &surface;

	/* 7. A client without a live wl_pointer: the press and the release of it are lost. */
	pointer.dead = 1U;
	button(1U, 40U);
	button(0U, 10U);
	take_lines(text, sizeof(text));
	check(count_lines(text, "KWL PRESS press-lost reason=no-pointer ") == 1U, "a press to a client with no wl_pointer is press-lost reason=no-pointer", text);
	check(count_lines(text, "KWL PRESS release-lost reason=no-pointer ") == 1U && strstr(text, " held_ms=40 ") != NULL,
	    "its release is release-lost with held_ms", text);
	pointer.dead = 0U;

	/* 8. At most 20 lines in one second; the next line after says how many were left out. */
	now_ms += 2000U;
	glass_takes = 1;
	glass_by = "menu";
	for (index = 0; index < 25U; index++) {
		button(1U, 0U);
		button(0U, 1U);
	}
	take_lines(text, sizeof(text));
	check(count_lines(text, "KWL PRESS press-kept reason=menu ") == 20U, "25 kept presses in a second write 20 lines", text);
	now_ms += 1000U;
	button(1U, 10U);
	button(0U, 10U);
	take_lines(text, sizeof(text));
	check(count_lines(text, "KWL PRESS press-kept reason=menu ") == 1U && strstr(text, " skipped=5\n") != NULL,
	    "the next second's line says skipped=5", text);

	/* 9. A client-requested move's release goes to its origin: nothing is logged. */
	glass_takes = 0;
	button(1U, 10U);
	server.interactive_window = &surface;
	server.interactive_surface = &surface;
	server.interactive_button = BTN_LEFT_CODE;
	server.drag = &surface;
	before = emitted_button;
	button(0U, 10U);
	take_lines(text, sizeof(text));
	check(count_lines(text, "KWL PRESS") == 0U, "a window operation's release that reaches its origin writes no line", text);
	check(emitted_button - before == 1U, "the origin hears that release", text);
	server.drag = NULL;

	fprintf(stderr, "host-press-log: %u/%u checks passed\n", checks - failures, checks);
	if (failures != 0U) {
		fprintf(stderr, "host-press-log: FAIL\n");
		return 1;
	}
	fprintf(stderr, "host-press-log: PASS\n");
	return 0;
}
