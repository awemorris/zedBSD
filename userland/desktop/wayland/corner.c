/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The top-right corner's swipe (ws079-p010, plan/ws079/design-input-notes.md
 * section 4): a contact that starts in the top-right corner and moves
 * towards the bottom left brings Notes, the handwriting application.
 *
 * It is the mirror image of App Home's top-left gesture (home.c) with the
 * same numbers.  A contact is a press of the left pointer button, a finger
 * on a touch screen (touch.c passes it through the shell as the pointer's
 * left button, with server->shell_source saying it is a finger), and later
 * the tip of a pen; each source feeds the same recogniser through
 * kwl_corner_contact_begin, _move and _end.  A contact that begins
 * in the corner belongs to the gesture until it ends.  It arms once it has
 * moved left and down by CORNER_ARM each, within CORNER_ARM_MS of the
 * press; it commits when it ends near the diagonal, either far enough along
 * it (CORNER_COMMIT) or quickly enough (CORNER_FLICK at CORNER_FLICK_SPEED).
 * Anything else lets it go without an effect.
 *
 * While armed, a glass quarter disc grows from the corner out to the
 * contact, as though the corner of the desktop were being turned back, with
 * Notes' name in it.  It brightens once letting go would bring Notes and
 * greys while the contact is off the diagonal.  When the contact ends it
 * shrinks back into the corner, or, on a commit, grows and fades.
 *
 * The corner works over a fullscreen window and over App Home too; on Home
 * a commit closes Home as Notes comes (the 2026-09-28 decision at the end of
 * design-input-notes.md).
 *
 * A commit finds the window whose app_id is "notes": it comes to the top of
 * the desktop shown and the compositor makes it fullscreen.  Without one,
 * /bin/notes --fullscreen is started, and for CORNER_LAUNCH_WAIT_MS another
 * commit does not start a second one.
 */

#include "glass.h"
#include "menu.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The corner a contact starts in: this many pixels from the top and from the right edge. */
#define CORNER_ZONE		28

/* How far left and down a contact moves before it is the gesture, and how soon. */
#define CORNER_ARM		14
#define CORNER_ARM_MS		1500U

/*
 * The diagonal: the shorter of the two movements must be at least
 * CORNER_CONE_NUMERATOR / CORNER_CONE_DENOMINATOR of the longer, which keeps
 * the contact within 25 degrees of the 45-degree line to the bottom left.
 */
#define CORNER_CONE_NUMERATOR	9
#define CORNER_CONE_DENOMINATOR	25

/* How far along the diagonal the gesture is complete, and how far a letting go there commits it. */
#define CORNER_DISTANCE		360.0f
#define CORNER_COMMIT		108.0f

/* A flick: at least this far along the diagonal, at this speed (pixels a millisecond) over the last CORNER_FLICK_MS. */
#define CORNER_FLICK		40.0f
#define CORNER_FLICK_SPEED	0.8f
#define CORNER_FLICK_MS		100U

/* How many recent points of the contact are kept for its speed. */
#define CORNER_SAMPLES		16U

/* How long the hint takes to settle after the contact ends. */
#define CORNER_SETTLE_MS	200U

/* How long a started Notes may take to show its window before another commit starts it again. */
#define CORNER_LAUNCH_WAIT_MS	5000U

/* What the gesture brings: the application's identity and how it is started. */
#define CORNER_APP_ID		"notes"
#define CORNER_PROGRAM		KEILAND_BINDIR "/notes"
#define CORNER_COMMAND		KEILAND_BINDIR "/notes --fullscreen"

/* The name shown in the hint once it is large enough. */
#define CORNER_LABEL		"Notes"
#define CORNER_LABEL_RADIUS	96.0f

/*
 * One point a contact passed through, and when: the input event's time in
 * wrapping milliseconds (evdev's, as Wayland carries it), so that a pause of
 * the compositor does not change how fast the contact seems to move.
 *
 * The recent points give the contact's speed when it ends.
 */
struct corner_sample {
	int32_t x;
	int32_t y;
	uint32_t time;
};

/*
 * The contact the gesture follows, from its start in the corner to its end.
 *
 * Its start has two times: the input event's (start_time), which the arming
 * limit and the speed are measured in, and the compositor's clock
 * (start_clock_ms), which the limit is kept by when no event comes.  Only
 * one contact is followed at a time; a contact of another source while one
 * is followed goes on to the rest of the desktop.
 */
struct corner_contact {
	unsigned active;
	enum kwl_contact_source source;
	int32_t start_x;
	int32_t start_y;
	uint32_t start_time;
	uint64_t start_clock_ms;
	int32_t x;
	int32_t y;
	unsigned armed;
	unsigned expired;
	struct corner_sample samples[CORNER_SAMPLES];
	unsigned sample_next;
	unsigned sample_count;
};

/*
 * The gesture's whole state: the contact, the hint settling after it, the
 * start of Notes being waited for, and whether the corner's place has been
 * logged (once, for the tests that measure it against the system bar).
 */
struct corner_state {
	struct corner_contact contact;
	unsigned zone_logged;
	unsigned settling;
	unsigned settle_committed;
	uint64_t settle_ms;
	float settle_radius;
	unsigned launching;
	uint64_t launch_ms;
};

/*
 * The gesture of the one compositor in this process.
 *
 * It is zero (no contact, nothing settling or waited for) at start-up.  The
 * compositor's single thread is the only one that reads or changes it: the
 * input handlers change the contact, the clock ends the settling and the
 * wait.
 */
static struct corner_state corner;

static int corner_in_zone(struct kwl_server *server, int32_t x, int32_t y);
static void corner_sample(int32_t x, int32_t y, uint32_t time);
static float corner_speed(void);
static int corner_on_diagonal(int32_t dx, int32_t dy);
static void corner_finish(struct kwl_server *server, unsigned committed, const char *reason);
static void corner_act(struct kwl_server *server);
static struct kwl_object *corner_find_notes(struct kwl_server *server);
static float corner_radius(void);
static float corner_ease(float t);
static const char *corner_source_name(enum kwl_contact_source source);

/*
 * Starts following a contact that begins in the top-right corner.
 *
 * Returns 1 when the contact is the gesture's (it then has every movement
 * and the end of the contact), 0 when it goes on to the rest of the desktop.
 */
int
kwl_corner_contact_begin(
	struct kwl_server *server,
	enum kwl_contact_source source,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	int inside;
	int control;
	int open;

	/* The login and lock screens have no corner gesture. */
	if (server->greeter || server->locked)
		return 0;

	/* A contact already followed keeps the gesture; this one goes on. */
	if (corner.contact.active)
		return 0;

	/* Only a contact in the corner. */
	inside = corner_in_zone(server, x, y);
	if (!inside)
		return 0;

	/*
	 * A mouse press on a docked window's control in the bar (its close
	 * button lies in the corner) is the bar's, not the gesture's (BUG-245).
	 * A finger there still starts the swipe.
	 */
	if (source == KWL_CONTACT_POINTER) {
		control = kwl_glass_bar_control_at(server, x, y);
		if (control)
			return 0;
	}

	/* An open menu closes on a press anywhere, the corner too, before the gesture could start. */
	open = kwl_network_is_open();
	if (open)
		return 0;
	open = kwl_bluetooth_is_open();
	if (open)
		return 0;
	open = kwl_status_panel_is_open();
	if (open)
		return 0;
	open = kwl_menu_is_open();
	if (open)
		return 0;

	/* The contact from its start; it is not armed yet. */
	memset(&corner.contact, 0, sizeof(corner.contact));
	corner.contact.active = 1;
	corner.contact.source = source;
	corner.contact.start_x = x;
	corner.contact.start_y = y;
	corner.contact.start_time = time;
	corner.contact.start_clock_ms = kwl_milliseconds();
	corner.contact.x = x;
	corner.contact.y = y;
	corner_sample(x, y, time);

	/* Succeeded: the contact is the gesture's. */
	printf("KWL CORNER press source=%s x=%d y=%d\n", corner_source_name(source), x, y);
	return 1;
}

/*
 * Follows a contact the gesture has to a new point.  Returns 1 when the
 * contact is the gesture's.
 */
int
kwl_corner_contact_move(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	uint32_t elapsed;
	int32_t dx;
	int32_t dy;

	/* A contact the gesture does not have goes on. */
	if (!corner.contact.active)
		return 0;

	/* The point, for the hint and for the speed at the end. */
	corner.contact.x = x;
	corner.contact.y = y;
	corner_sample(x, y, time);

	/* A contact that timed out stays the gesture's until it ends, without effect. */
	if (corner.contact.expired)
		return 1;

	/* An armed contact redraws the hint. */
	if (corner.contact.armed) {
		server->dirty = 1;
		return 1;
	}

	/* Too late to arm: the contact is let go (still the gesture's until it ends). */
	elapsed = time - corner.contact.start_time;
	if (elapsed > CORNER_ARM_MS) {
		corner.contact.expired = 1;
		printf("KWL CORNER cancel reason=timeout\n");
		return 1;
	}

	/* The contact arms once it has moved far enough left and far enough down. */
	dx = corner.contact.start_x - x;
	dy = y - corner.contact.start_y;
	if (dx < CORNER_ARM || dy < CORNER_ARM)
		return 1;

	/* Succeeded: the contact is armed, and the hint shows from now on. */
	corner.contact.armed = 1;
	corner.settling = 0;
	server->dirty = 1;
	printf("KWL CORNER armed ms=%u\n", elapsed);
	return 1;
}

/*
 * Ends a contact the gesture has: commits it when it ended near the
 * diagonal, far enough along it or quickly enough, and lets it go
 * otherwise.  Returns 1 when the contact was the gesture's.
 */
int
kwl_corner_contact_end(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	int32_t dx;
	int32_t dy;
	int diagonal;
	float progress;
	float speed;

	/* A contact the gesture does not have goes on. */
	if (!corner.contact.active)
		return 0;

	/* The last point, which the speed is measured to. */
	corner.contact.x = x;
	corner.contact.y = y;
	corner_sample(x, y, time);

	/* A contact that timed out ends without effect (its cancel was logged then). */
	if (corner.contact.expired) {
		corner.contact.active = 0;
		return 1;
	}

	/* A contact that never armed ends without effect, like a click on the clock. */
	if (!corner.contact.armed) {
		corner.contact.active = 0;
		printf("KWL CORNER cancel reason=unarmed\n");
		return 1;
	}

	/* A contact that ends off the diagonal to the bottom left is let go. */
	dx = corner.contact.start_x - x;
	dy = y - corner.contact.start_y;
	diagonal = corner_on_diagonal(dx, dy);
	if (!diagonal) {
		corner_finish(server, 0, "direction");
		return 1;
	}

	/* Far enough along the diagonal commits. */
	progress = ((float)dx + (float)dy) * 0.5f;
	if (progress >= CORNER_COMMIT) {
		printf("KWL CORNER commit via=distance progress=%.0f\n", (double)progress);
		corner_finish(server, 1, NULL);
		corner_act(server);
		return 1;
	}

	/* A quick flick commits after a shorter way. */
	speed = corner_speed();
	if (progress >= CORNER_FLICK && speed >= CORNER_FLICK_SPEED) {
		printf("KWL CORNER commit via=flick progress=%.0f speed=%.2f\n", (double)progress, (double)speed);
		corner_finish(server, 1, NULL);
		corner_act(server);
		return 1;
	}

	/* Succeeded: too short and too slow, the contact is let go. */
	printf("KWL CORNER speed=%.2f progress=%.0f\n", (double)speed, (double)progress);
	corner_finish(server, 0, "short");
	return 1;
}

/*
 * Feeds the left pointer button to the gesture: a press begins a contact,
 * the release ends it.  Returns 1 when the button is the gesture's.
 */
int
kwl_corner_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	int taken;

	/* Only the left button is a contact. */
	if (button != KWL_BUTTON_LEFT)
		return 0;

	/* A release ends the contact, when it is the gesture's (at the time of the button's event). */
	if (state == 0) {
		taken = kwl_corner_contact_end(server, server->pointer_x, server->pointer_y, server->input_time);
		return taken;
	}

	/* Succeeded: a press may begin one (the mouse's, or a finger's passed as the pointer, touch.c). */
	taken = kwl_corner_contact_begin(server, server->shell_source, server->pointer_x, server->pointer_y, server->input_time);
	return taken;
}

/*
 * Feeds the pointer's movement to the gesture while the left button's
 * contact is followed.  Returns 1 when the movement is the gesture's.
 */
int
kwl_corner_motion(
	struct kwl_server *server)
{
	int taken;

	/* Only the contact of the source moving the pointer now follows it (the mouse's, or a finger's). */
	if (!corner.contact.active || corner.contact.source != server->shell_source)
		return 0;

	/* Succeeded: the contact moves with the pointer (at the time of the motion's event). */
	taken = kwl_corner_contact_move(server, server->pointer_x, server->pointer_y, server->input_time);
	return taken;
}

/*
 * Keeps the gesture's time: a contact that does not arm in time is let go,
 * the settling hint draws until it is done, and a start of Notes whose
 * window never came is forgotten.
 */
void
kwl_corner_tick(
	struct kwl_server *server)
{
	uint64_t now;

	/* The corner's place, once, beside the network icon's (network.c) in the log. */
	if (!corner.zone_logged && server->width > 0U) {
		corner.zone_logged = 1;
		printf("KWL CORNER zone x=%d y=0 width=%d height=%d\n", (int)server->width - CORNER_ZONE, CORNER_ZONE, CORNER_ZONE);
	}

	/*
	 * The pointer's or a finger's contact whose release never came (its
	 * device went away with the left button's bit held) ends, so the
	 * corner is not held forever.
	 */
	if (corner.contact.active &&
	    corner.contact.source != KWL_CONTACT_PEN &&
	    (server->buttons_down & 1U) == 0U) {
		corner.contact.active = 0;
		corner.settling = 0;
		server->dirty = 1;
		printf("KWL CORNER cancel reason=lost\n");
	}

	/* A contact that is still waiting to arm times out. */
	now = kwl_milliseconds();
	if (corner.contact.active &&
	    !corner.contact.armed &&
	    !corner.contact.expired &&
	    now - corner.contact.start_clock_ms > CORNER_ARM_MS) {
		corner.contact.expired = 1;
		printf("KWL CORNER cancel reason=timeout\n");
	}

	/* The settling hint draws every frame until its time is over. */
	if (corner.settling) {
		server->dirty = 1;
		if (now - corner.settle_ms >= CORNER_SETTLE_MS)
			corner.settling = 0;
	}

	/* A start of Notes whose window did not come in time may be tried again. */
	if (corner.launching && now - corner.launch_ms > CORNER_LAUNCH_WAIT_MS) {
		corner.launching = 0;
		printf("KWL CORNER launch expired\n");
	}
}

/*
 * Tells whether the gesture's hint is on the output: an armed contact, or
 * the hint settling after one.  While it is, the output is composed, even
 * over a fullscreen window, so the hint can be drawn.
 */
int
kwl_corner_showing(
	void)
{
	/* An armed contact that has not timed out. */
	if (corner.contact.active &&
	    corner.contact.armed &&
	    !corner.contact.expired)
		return 1;

	/* The hint settling after the contact ended. */
	if (corner.settling)
		return 1;

	/* Nothing shows. */
	return 0;
}

/*
 * Draws the hint: a glass quarter disc from the top-right corner out to the
 * contact, with Notes' name in it once it is large enough.
 */
void
kwl_corner_draw(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float ink[4] = { 0.10f, 0.16f, 0.30f, 1.0f };
	struct glass_shape shape;
	float color[4];
	float radius;
	float opacity;
	float t;
	float corner_x;
	float along;
	int32_t dx;
	int32_t dy;
	int32_t width;
	int ready;
	int diagonal;
	uint64_t elapsed;

	/* Nothing to draw without the hint. */
	ready = kwl_corner_showing();
	if (!ready)
		return;

	/* How large the disc is, and how strongly it shows: settling, it shrinks back or grows and fades. */
	radius = corner_radius();
	opacity = 1.0f;
	if (corner.settling) {
		elapsed = kwl_milliseconds() - corner.settle_ms;
		t = (float)elapsed / (float)CORNER_SETTLE_MS;
		if (t > 1.0f)
			t = 1.0f;
		t = corner_ease(t);
		if (corner.settle_committed) {
			radius = corner.settle_radius + (CORNER_DISTANCE * 1.4f - corner.settle_radius) * t;
			opacity = 1.0f - t;
		} else {
			radius = corner.settle_radius * (1.0f - t);
		}
	}

	/* A disc too small to see is not drawn. */
	if (radius < 2.0f || opacity <= 0.0f)
		return;

	/* Whether letting go now would commit, and whether the contact is on the diagonal. */
	ready = 0;
	diagonal = 1;
	if (!corner.settling) {
		dx = corner.contact.start_x - corner.contact.x;
		dy = corner.contact.y - corner.contact.start_y;
		diagonal = corner_on_diagonal(dx, dy);
		if (diagonal && ((float)dx + (float)dy) * 0.5f >= CORNER_COMMIT)
			ready = 1;
	}

	/* A soft shadow under the turned-back corner. */
	corner_x = (float)server->width;
	glass_shape_init(&shape, corner_x - radius, -radius, radius * 2.0f, radius * 2.0f);
	shape.quad[0] -= 40.0f;
	shape.quad[1] -= 40.0f;
	shape.quad[2] += 80.0f;
	shape.quad[3] += 80.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = radius;
	shape.soft = 18.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.26f;
	shape.opacity = opacity;
	glass_shape_draw(server, command, &shape);

	/* The glass disc about the corner, white, greyer while off the diagonal. */
	glass_shape_init(&shape, corner_x - radius, -radius, radius * 2.0f, radius * 2.0f);
	shape.mode = MODE_GLASS;
	shape.radius = radius;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.78f;
	if (!diagonal) {
		shape.color[0] = 0.86f;
		shape.color[1] = 0.88f;
		shape.color[2] = 0.92f;
		shape.color[3] = 0.60f;
	}

	/* The disc's edge and fade, drawn. */
	shape.edge = 0.85f;
	shape.opacity = opacity;
	glass_shape_draw(server, command, &shape);

	/* Its rim, blue once letting go would bring Notes. */
	glass_shape_init(&shape, corner_x - radius, -radius, radius * 2.0f, radius * 2.0f);
	shape.mode = MODE_RING;
	shape.radius = radius;
	shape.soft = 2.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.70f;
	if (ready) {
		shape.color[0] = 0.25f;
		shape.color[1] = 0.52f;
		shape.color[2] = 0.95f;
		shape.color[3] = 0.95f;
	}

	/* The rim's fade, drawn. */
	shape.opacity = opacity;
	glass_shape_draw(server, command, &shape);

	/* Notes' name, along the diagonal inside the disc, once there is room for it. */
	if (radius < CORNER_LABEL_RADIUS)
		return;
	memcpy(color, ink, sizeof(color));
	color[3] = opacity * (radius - CORNER_LABEL_RADIUS) / 40.0f;
	if (color[3] > opacity)
		color[3] = opacity;
	width = glass_text_width(server, SIZE_TITLE, CORNER_LABEL);
	along = radius * 0.42f;
	glass_draw_text(server, command, SIZE_TITLE, (int32_t)(corner_x - along) - width / 2, (int32_t)along + 6, CORNER_LABEL, width + 1, color);
}

/* Tells whether a point is in the top-right corner a contact may start the gesture from. */
static int
corner_in_zone(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	/* Below the corner, or left of it. */
	if (y < 0 || y >= CORNER_ZONE)
		return 0;
	if (x < (int32_t)server->width - CORNER_ZONE || x >= (int32_t)server->width)
		return 0;

	/* In it. */
	return 1;
}

/* Keeps a point of the contact among the recent ones, the oldest replaced first. */
static void
corner_sample(
	int32_t x,
	int32_t y,
	uint32_t time)
{
	struct corner_sample *sample;

	/* The slot after the newest. */
	sample = &corner.contact.samples[corner.contact.sample_next];
	sample->x = x;
	sample->y = y;
	sample->time = time;

	/* The ring moves on; it holds at most CORNER_SAMPLES points. */
	corner.contact.sample_next = (corner.contact.sample_next + 1U) % CORNER_SAMPLES;
	if (corner.contact.sample_count < CORNER_SAMPLES)
		corner.contact.sample_count++;
}

/*
 * Works out how fast the contact moved along the diagonal just before its
 * newest point, in pixels a millisecond: from the newest point at least
 * CORNER_FLICK_MS older (or the oldest kept, when none is that old) to the
 * newest point.
 */
static float
corner_speed(
	void)
{
	const struct corner_sample *newest;
	const struct corner_sample *from;
	const struct corner_sample *sample;
	unsigned index;
	unsigned slot;
	uint32_t elapsed;
	float moved;

	/* The newest point; without an older one there is no speed. */
	if (corner.contact.sample_count < 2U)
		return 0.0f;
	slot = (corner.contact.sample_next + CORNER_SAMPLES - 1U) % CORNER_SAMPLES;
	newest = &corner.contact.samples[slot];

	/* Back from the newest, to the first point old enough, or the oldest there is. */
	from = NULL;
	for (index = 1U; index < corner.contact.sample_count; index++) {
		slot = (corner.contact.sample_next + CORNER_SAMPLES - 1U - index) % CORNER_SAMPLES;
		sample = &corner.contact.samples[slot];
		from = sample;
		elapsed = newest->time - sample->time;
		if (elapsed >= CORNER_FLICK_MS)
			break;
	}

	/* Points at the same moment give no speed. */
	elapsed = newest->time - from->time;
	if (elapsed == 0U)
		return 0.0f;

	/* The distance along the diagonal (left and down are forward) over the time. */
	moved = ((float)(from->x - newest->x) + (float)(newest->y - from->y)) * 0.5f;

	/* Reports the speed. */
	return moved / (float)elapsed;
}

/* Tells whether a movement (left and down positive) is within 25 degrees of the diagonal to the bottom left. */
static int
corner_on_diagonal(
	int32_t dx,
	int32_t dy)
{
	int32_t shorter;
	int32_t longer;

	/* Up or right of the start is off the diagonal. */
	if (dx <= 0 || dy <= 0)
		return 0;

	/* The shorter movement against the longer one. */
	shorter = dx;
	longer = dy;
	if (dy < dx) {
		shorter = dy;
		longer = dx;
	}

	/* A movement outside the cone about the diagonal is off it. */
	if (shorter * CORNER_CONE_DENOMINATOR < longer * CORNER_CONE_NUMERATOR)
		return 0;

	/* On the diagonal. */
	return 1;
}

/* Ends the followed contact: the hint settles back into the corner, or grows and fades on a commit. */
static void
corner_finish(
	struct kwl_server *server,
	unsigned committed,
	const char *reason)
{
	/* The hint starts settling from the size it had. */
	corner.settle_radius = corner_radius();
	corner.settle_committed = committed;
	corner.settle_ms = kwl_milliseconds();
	corner.settling = 1;
	server->dirty = 1;

	/* The contact is over. */
	corner.contact.active = 0;
	if (!committed)
		printf("KWL CORNER cancel reason=%s\n", reason);
}

/*
 * Brings Notes: its window to the top and fullscreen, or, without one,
 * starts it (once while its window is waited for).
 */
static void
corner_act(
	struct kwl_server *server)
{
	struct kwl_object *surface;
	struct kwl_object *top;
	pid_t child;
	int error;
	int missing;

	/* App Home, when it shows, closes as Notes comes: the swipe works on Home too. */
	kwl_home_dismiss(server, "notes");

	/* Notes' window, when it has one. */
	surface = corner_find_notes(server);
	if (surface != NULL) {
		corner.launching = 0;

		/* Already on top and fullscreen here: nothing to do. */
		top = kwl_top_window(server);
		if (surface == top && surface->fullscreen) {
			printf("KWL CORNER notes surface=%u already client=%llu\n", surface->id, (unsigned long long)surface->client->number);
			return;
		}

		/* A minimized window, or one on another desktop, comes back to the desktop shown. */
		surface->minimized = 0;
		surface->desktop = server->desktop;

		/* It comes to the top and takes the focus, the way a click on it does. */
		kwl_glass_raise(server, surface);

		/* The compositor makes it fullscreen, and tells it (a fullscreen window is only raised). */
		error = kwl_window_enter_fullscreen(surface);
		if (error != 0) {
			printf("KWL CORNER notes surface=%u fullscreen error=%d client=%llu\n", surface->id, error, (unsigned long long)surface->client->number);
			return;
		}

		/* Succeeded: Notes is on top, fullscreen. */
		server->dirty = 1;
		printf("KWL CORNER notes surface=%u raise fullscreen client=%llu\n", surface->id, (unsigned long long)surface->client->number);
		return;
	}

	/* Notes already starting: a second one is not started. */
	if (corner.launching) {
		printf("KWL CORNER notes waiting\n");
		return;
	}

	/* Without the program there is nothing to start. */
	missing = access(CORNER_PROGRAM, X_OK);
	if (missing != 0) {
		printf("KWL CORNER notes missing errno=%d\n", errno);
		return;
	}

	/* Notes is started, and asks for fullscreen itself before its window first shows. */
	child = kwl_spawn(server, CORNER_COMMAND);
	if (child < 0) {
		printf("KWL CORNER notes launch error=%d\n", errno);
		return;
	}

	/* Succeeded: its window is waited for. */
	corner.launching = 1;
	corner.launch_ms = kwl_milliseconds();
	printf("KWL CORNER notes launch pid=%d\n", (int)child);
}

/* Finds Notes' window: the most recently raised mapped toplevel whose app_id is "notes"; NULL for none. */
static struct kwl_object *
corner_find_notes(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	struct kwl_object *found;
	int differs;

	/* Every window of every client, the highest map order kept. */
	found = NULL;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only a live, mapped toplevel. */
			if (surface->kind != KWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->role == NULL ||
			    surface->role->top == NULL)
				continue;

			/* Only Notes. */
			differs = strcmp(surface->app_id, CORNER_APP_ID);
			if (differs != 0)
				continue;

			/* The one raised last. */
			if (found == NULL || surface->map_order > found->map_order)
				found = surface;
		}
	}

	/* Succeeded: the window, or NULL. */
	return found;
}

/* Works out the hint's radius from the followed contact: the distance from the corner out to it. */
static float
corner_radius(
	void)
{
	float dx;
	float dy;
	float progress;

	/* No armed contact: no disc. */
	if (!corner.contact.active || !corner.contact.armed)
		return 0.0f;

	/* How far along the diagonal the contact is, as App Home measures its own gesture. */
	dx = (float)(corner.contact.start_x - corner.contact.x);
	dy = (float)(corner.contact.y - corner.contact.start_y);
	progress = (dx + dy) * 0.5f;
	if (progress < 0.0f)
		progress = 0.0f;
	if (progress > CORNER_DISTANCE)
		progress = CORNER_DISTANCE;

	/* The disc reaches the contact: a little more than its way along the diagonal from the corner. */
	return 24.0f + progress * 1.3f;
}

/* Eases a time from 0 to 1: fast at first, slowing at the end. */
static float
corner_ease(
	float t)
{
	float rest;

	/* The cube of what is left. */
	rest = 1.0f - t;

	/* Reports the eased time. */
	return 1.0f - rest * rest * rest;
}

/* Names a contact's source for the log. */
static const char *
corner_source_name(
	enum kwl_contact_source source)
{
	/* One name per source. */
	switch (source) {
	case KWL_CONTACT_PEN:
		return "pen";
	case KWL_CONTACT_TOUCH:
		return "touch";
	case KWL_CONTACT_POINTER:
	default:
		return "pointer";
	}
}
