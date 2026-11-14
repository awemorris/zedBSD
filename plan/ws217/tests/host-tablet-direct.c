/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of a pen on the display (ws217-p003, T1-592 P3-3):
 * userland/desktop/wayland/tablet.c with its seat around it faked, fed the
 * reports peninject writes (hover, down, up), for a pen on the display
 * (direct) and one on a tablet beside it, at 0 and 90 degrees.  Each must
 * come over the client's surface (proximity_in), touch it (down, and the
 * line "KWL TABLET down"), and lift (up, proximity_out), at the logical
 * place the design gives.
 */

#include "kwl.h"
#include "tablet.h"
#include "extras.h"
#include "popup.h"
#include "rotate.h"
#include "userland/desktop/libkeiland-backend/keiland-backend-evdev.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The test pen's axes, as peninject's "size 1279 799" declares them, and its tilt's resolution. */
#define TEST_X_MAX		1279
#define TEST_Y_MAX		799
#define TEST_PRESSURE_MAX	4095
#define TEST_TILT_MAX		60
#define TEST_TILT_RESOLUTION	57

/* The physical panel. */
#define TEST_PANEL_WIDTH	1280U
#define TEST_PANEL_HEIGHT	800U

/* Event opcodes of zwp_tablet_tool_v2 the test watches. */
#define TEST_PROXIMITY_IN	6U
#define TEST_PROXIMITY_OUT	7U
#define TEST_DOWN		8U
#define TEST_UP			9U

/* The most tool events one case records. */
#define TEST_EVENTS		256U

/* The number of failed checks. */
static unsigned failures;

/* The angle the anchor shows, and the seat's fakes. */
static unsigned test_rotation;
static struct kwl_server test_server;
static struct kwl_client test_client;
static struct kwl_object test_surface;
static struct kwl_object test_tool;
static struct kwl_object test_tablet;
static uint32_t test_serial;

/* The tool events the client heard, in order. */
static uint32_t test_opcodes[TEST_EVENTS];
static unsigned test_count;

static void check(int condition, const char *what);
static void run_case(unsigned direct, unsigned rotation, int32_t pen_x, int32_t pen_y, int32_t surface_x, int32_t surface_y, int32_t want_x, int32_t want_y);
static void run_late(unsigned direct);
static void seat_reset(int32_t surface_x, int32_t surface_y);
static void report(struct kwl_input_device *input, const struct input_event *events, unsigned count);
static void event(struct input_event *slot, uint16_t type, uint16_t code, int32_t value);
static unsigned heard(uint32_t opcode);

/* Runs every case. */
int
main(void)
{
	/*
	 * A pen at (1000, 400) on the panel, the client's surface under it:
	 * at 0 degrees the logical point is the same, at 90 degrees it is
	 * (400, 279) on the pen on the display and (624, 640) on the pen
	 * tablet beside it (phase.md 12.3).
	 */
	run_case(1U, KWL_ROTATE_0, 1000, 400, 900, 300, 1000, 400);
	run_case(0U, KWL_ROTATE_0, 1000, 400, 900, 300, 1000, 400);
	run_case(1U, KWL_ROTATE_90, 1000, 400, 300, 200, 400, 279);
	run_case(0U, KWL_ROTATE_90, 1000, 400, 500, 600, 624, 640);

	/* What T1-592 saw: a node opened after the pen came into range (not a failure, the gap is reported). */
	run_late(1U);
	run_late(0U);

	/* The verdict. */
	if (failures != 0U) {
		printf("host-tablet-direct: %u failures\n", failures);
		return 1;
	}

	/* Succeeded. */
	printf("host-tablet-direct: PASS\n");
	return 0;
}

/* Counts a failed check and names it. */
static void
check(
	int condition,
	const char *what)
{
	/* A passed check says nothing. */
	if (condition)
		return;

	/* A failed one is counted and named. */
	failures++;
	printf("FAIL: %s\n", what);
}

/*
 * One case: a pen tablet node of the kind given is added, then hovers,
 * touches and lifts as peninject's "hover", "down" and "up" write it.
 */
static void
run_case(
	unsigned direct,
	unsigned rotation,
	int32_t pen_x,
	int32_t pen_y,
	int32_t surface_x,
	int32_t surface_y,
	int32_t want_x,
	int32_t want_y)
{
	struct kwl_input_device input;
	struct input_event events[KWL_INPUT_FRAME_MAX];
	char what[128];
	unsigned count;
	int error;

	/* The seat, at the case's angle. */
	seat_reset(surface_x, surface_y);
	test_rotation = rotation;

	/* The node, added as input.c adds a pen tablet. */
	memset(&input, 0, sizeof(input));
	input.fd = 3;
	input.tablet = 1U;
	input.live = 1U;
	input.direct = direct;
	snprintf(input.path, sizeof(input.path), "/dev/input/event%u", direct);
	error = kwl_tablet_add(&test_server, &input);
	snprintf(what, sizeof(what), "direct=%u rotation=%u: the tablet is added", direct, rotation);
	check(error == 0, what);

	/* hover X Y: the tool comes into range over the surface. */
	count = 0U;
	event(&events[count++], EV_KEY, BTN_TOOL_PEN, 1);
	event(&events[count++], EV_ABS, ABS_X, pen_x);
	event(&events[count++], EV_ABS, ABS_Y, pen_y);
	report(&input, events, count);
	snprintf(what, sizeof(what), "direct=%u rotation=%u: the pen is at (%d, %d), not (%d, %d)", direct, rotation, want_x, want_y, test_server.pointer_x,
		 test_server.pointer_y);
	check(test_server.pointer_x == want_x && test_server.pointer_y == want_y, what);
	snprintf(what, sizeof(what), "direct=%u rotation=%u: the surface hears proximity_in", direct, rotation);
	check(heard(TEST_PROXIMITY_IN) == 1U, what);

	/* down X Y P: the pen touches. */
	count = 0U;
	event(&events[count++], EV_ABS, ABS_PRESSURE, 2000);
	event(&events[count++], EV_ABS, ABS_TILT_X, 20);
	event(&events[count++], EV_KEY, BTN_TOUCH, 1);
	report(&input, events, count);
	snprintf(what, sizeof(what), "direct=%u rotation=%u: the surface hears down", direct, rotation);
	check(heard(TEST_DOWN) == 1U, what);

	/* up: the pen lifts and leaves. */
	count = 0U;
	event(&events[count++], EV_ABS, ABS_PRESSURE, 0);
	event(&events[count++], EV_KEY, BTN_TOUCH, 0);
	event(&events[count++], EV_KEY, BTN_TOOL_PEN, 0);
	report(&input, events, count);
	snprintf(what, sizeof(what), "direct=%u rotation=%u: the surface hears up and proximity_out", direct, rotation);
	check(heard(TEST_UP) == 1U && heard(TEST_PROXIMITY_OUT) == 1U, what);

	/* The node goes. */
	kwl_tablet_remove(&test_server, &input, 1);
}

/*
 * A node the compositor opens after the pen came into range (the scan runs
 * up to KWL_INPUT_SETTLE_SCAN_MS after the device's event, once sessiond
 * has given the node to the user): the approach (BTN_TOOL_PEN 1) was made
 * before anyone read the node, so the reports read after it have no tool
 * near, and nothing reaches the surface until the pen leaves and comes
 * back.  The compositor does not read the key state when it opens a node.
 */
static void
run_late(
	unsigned direct)
{
	struct kwl_input_device input;
	struct input_event events[KWL_INPUT_FRAME_MAX];
	unsigned count;
	int error;

	/* The seat at 0 degrees, the surface under the pen. */
	seat_reset(900, 300);
	test_rotation = KWL_ROTATE_0;

	/* The node is opened after the approach: its axes rest where the pen is, its keys are not read. */
	memset(&input, 0, sizeof(input));
	input.fd = 3;
	input.tablet = 1U;
	input.live = 1U;
	input.direct = direct;
	snprintf(input.path, sizeof(input.path), "/dev/input/event%u", direct);
	error = kwl_tablet_add(&test_server, &input);
	check(error == 0, "a late node is added");

	/* down's report as read: the pressure, the tilt and the touch, no tool. */
	count = 0U;
	event(&events[count++], EV_ABS, ABS_PRESSURE, 2000);
	event(&events[count++], EV_ABS, ABS_TILT_X, 20);
	event(&events[count++], EV_KEY, BTN_TOUCH, 1);
	report(&input, events, count);

	/* up's report. */
	count = 0U;
	event(&events[count++], EV_ABS, ABS_PRESSURE, 0);
	event(&events[count++], EV_KEY, BTN_TOUCH, 0);
	event(&events[count++], EV_KEY, BTN_TOOL_PEN, 0);
	report(&input, events, count);
	printf("LATE-OPEN direct=%u proximity_in=%u down=%u up=%u\n", direct, heard(TEST_PROXIMITY_IN), heard(TEST_DOWN), heard(TEST_UP));

	/* The node goes. */
	kwl_tablet_remove(&test_server, &input, 1);
}

/* Makes the seat: one client whose 200x200 surface is at the place given, with the pen's tool and the tablet. */
static void
seat_reset(
	int32_t surface_x,
	int32_t surface_y)
{
	/* Everything empty, no event heard. */
	memset(&test_server, 0, sizeof(test_server));
	memset(&test_client, 0, sizeof(test_client));
	memset(&test_surface, 0, sizeof(test_surface));
	memset(&test_tool, 0, sizeof(test_tool));
	memset(&test_tablet, 0, sizeof(test_tablet));
	test_count = 0U;

	/* The client and its tool and tablet, announced on seat 1. */
	test_client.server = &test_server;
	test_client.objects = &test_tool;
	test_server.clients = &test_client;
	test_tool.client = &test_client;
	test_tool.kind = KWL_TABLET_TOOL;
	test_tool.id = 10U;
	test_tool.tool_slot = 0U;
	test_tool.tablet_seat_number = 1U;
	test_tool.next = &test_tablet;
	test_tablet.client = &test_client;
	test_tablet.kind = KWL_TABLET;
	test_tablet.id = 11U;
	test_tablet.tablet_slot = 0U;
	test_tablet.tablet_seat_number = 1U;

	/* Its surface. */
	test_surface.client = &test_client;
	test_surface.id = 12U;
	test_surface.x = surface_x;
	test_surface.y = surface_y;
}

/* Applies one report of the node, as the main thread does (input.c's apply_report). */
static void
report(
	struct kwl_input_device *input,
	const struct input_event *events,
	unsigned count)
{
	/* The report, then its application. */
	memcpy(input->frame, events, count * sizeof(events[0]));
	input->frame_count = count;
	kwl_tablet_frame(&test_server, input, 0U);
	input->frame_count = 0U;
}

/* Fills one evdev event. */
static void
event(
	struct input_event *slot,
	uint16_t type,
	uint16_t code,
	int32_t value)
{
	/* The event, without a time. */
	memset(slot, 0, sizeof(*slot));
	slot->type = type;
	slot->code = code;
	slot->value = value;
}

/* Counts how often the tool object heard an event. */
static unsigned
heard(
	uint32_t opcode)
{
	unsigned index;
	unsigned times;

	/* Each event the client heard. */
	times = 0U;
	for (index = 0U; index < test_count; index++) {
		if (test_opcodes[index] == opcode)
			times++;
	}

	/* Succeeded: the count. */
	return times;
}

/* The axes of the test pen's node. */
int
kl_backend_input_absinfo(
	int descriptor,
	uint32_t axis,
	struct input_absinfo *info)
{
	/* Every axis starts at its rest. */
	(void)descriptor;
	memset(info, 0, sizeof(*info));
	switch (axis) {
	case ABS_X:
		info->maximum = TEST_X_MAX;
		return 0;
	case ABS_Y:
		info->maximum = TEST_Y_MAX;
		return 0;
	case ABS_PRESSURE:
		info->maximum = TEST_PRESSURE_MAX;
		return 0;
	case ABS_TILT_X:
	case ABS_TILT_Y:
		info->minimum = -TEST_TILT_MAX;
		info->maximum = TEST_TILT_MAX;
		info->resolution = TEST_TILT_RESOLUTION;
		return 0;
	default:
		return ENOTTY;
	}
}

/* The node's name. */
int
kl_backend_input_name(
	int descriptor,
	char *name,
	size_t size)
{
	/* The injector's pen. */
	(void)descriptor;
	snprintf(name, size, "Test pen (input-inject)");
	return 0;
}

/* The node's identity: not a USB device. */
int
kl_backend_input_id(
	int descriptor,
	struct input_id *id)
{
	/* A virtual bus. */
	(void)descriptor;
	memset(id, 0, sizeof(*id));
	return 0;
}

/* Records the events of the tool object. */
int
kwl_emit(
	struct kwl_client *client,
	uint32_t object,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	/* Only the tool's events are watched. */
	(void)client;
	(void)payload;
	(void)size;
	if (object == test_tool.id && test_count < TEST_EVENTS)
		test_opcodes[test_count++] = opcode;
	return 0;
}

/* No object is made by the cases (the tool and the tablet exist already). */
struct kwl_object *
kwl_create(
	struct kwl_client *client,
	uint32_t id,
	enum kwl_kind kind,
	uint32_t version)
{
	/* Nothing. */
	(void)client;
	(void)id;
	(void)kind;
	(void)version;
	return NULL;
}

/* No object is made by the cases. */
struct kwl_object *
kwl_create_server(
	struct kwl_client *client,
	enum kwl_kind kind,
	uint32_t version)
{
	/* Nothing. */
	(void)client;
	(void)kind;
	(void)version;
	return NULL;
}

/* No object is looked up by the cases. */
struct kwl_object *
kwl_find(
	struct kwl_client *client,
	uint32_t id)
{
	/* Nothing. */
	(void)client;
	(void)id;
	return NULL;
}

/* No object is destroyed by the cases. */
void
kwl_object_destroy(
	struct kwl_object *object)
{
	/* Nothing. */
	(void)object;
}

/* No case runs out of objects, so a failed client is a failed case. */
void
kwl_client_fail(
	struct kwl_client *client)
{
	/* Counts it. */
	(void)client;
	check(0, "no client fails");
}

/* The arrow comes back. */
void
kwl_cursor_default(
	struct kwl_server *server)
{
	/* Nothing to draw. */
	(void)server;
}

/* The cursor's old place is redrawn. */
void
kwl_damage_pointer(
	struct kwl_server *server,
	int32_t old_x,
	int32_t old_y)
{
	/* Nothing to draw. */
	(void)server;
	(void)old_x;
	(void)old_y;
}

/* The monotonic clock in microseconds. */
uint64_t
kwl_microseconds(void)
{
	struct timespec now;

	/* The clock. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/* The monotonic clock in milliseconds. */
uint64_t
kwl_milliseconds(void)
{
	/* From the microseconds. */
	return kwl_microseconds() / 1000U;
}

/* The next serial. */
uint32_t
kwl_next_serial(
	struct kwl_server *server)
{
	/* Counted here. */
	(void)server;
	test_serial++;
	return test_serial;
}

/* The anchor's angle; a head is not turned (heads.c). */
unsigned
kwl_output_rotation(
	struct kwl_server *server,
	unsigned slot)
{
	/* The case's angle on the anchor. */
	(void)server;
	if (slot != KWL_PLANE_ANCHOR)
		return KWL_ROTATE_0;
	return test_rotation;
}

/* An absolute device's place on the anchor, as heads.c's kwl_output_absolute computes it. */
void
kwl_output_absolute(
	struct kwl_server *server,
	unsigned slot,
	unsigned direct,
	const struct kwl_cursor_axis *across,
	const struct kwl_cursor_axis *down,
	int32_t *x,
	int32_t *y)
{
	struct kwl_cursor_geometry geometry;

	/* One output, the anchor: its logical picture is the panel turned by the angle. */
	memset(&geometry, 0, sizeof(geometry));
	geometry.output_count = 1U;
	geometry.absolute_slot = KWL_PLANE_ANCHOR;
	kwl_rotate_size(test_rotation, TEST_PANEL_WIDTH, TEST_PANEL_HEIGHT, &geometry.outputs[KWL_PLANE_ANCHOR].width,
			&geometry.outputs[KWL_PLANE_ANCHOR].height);
	geometry.physical[KWL_PLANE_ANCHOR].rotation = kwl_output_rotation(server, KWL_PLANE_ANCHOR);
	geometry.physical[KWL_PLANE_ANCHOR].width = TEST_PANEL_WIDTH;
	geometry.physical[KWL_PLANE_ANCHOR].height = TEST_PANEL_HEIGHT;

	/* The place, in 1/256 pixels. */
	kwl_cursor_geometry_place(&geometry, slot, direct, 256U, across, down, x, y);
}

/* The pointer is the anchor's. */
void
kwl_pointer_absolute(
	struct kwl_server *server)
{
	/* The anchor. */
	server->pointer_output = KWL_PLANE_ANCHOR;
}

/* The pointer goes where the pen is. */
void
kwl_pointer_warp_to(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	/* The main thread's view. */
	server->pointer_x = x;
	server->pointer_y = y;
}

/* The pointer's surface: the case's surface when the pointer is on it. */
void
kwl_seat_pointer_update(
	struct kwl_server *server)
{
	uint32_t width;
	uint32_t height;

	/* The surface's rectangle. */
	kwl_surface_size(&test_surface, &width, &height);
	server->pointer_surface = NULL;
	if (server->pointer_x < test_surface.x || server->pointer_y < test_surface.y)
		return;
	if (server->pointer_x >= test_surface.x + (int32_t)width || server->pointer_y >= test_surface.y + (int32_t)height)
		return;

	/* Succeeded: on it. */
	server->pointer_surface = &test_surface;
}

/* Every surface is 200 pixels square. */
void
kwl_surface_size(
	const struct kwl_object *surface,
	uint32_t *width,
	uint32_t *height)
{
	/* The size. */
	(void)surface;
	*width = 200U;
	*height = 200U;
}

/* The compositor's own UI takes no motion. */
int
kwl_seat_motion_shell(
	struct kwl_server *server,
	uint32_t time)
{
	/* Not taken. */
	(void)server;
	(void)time;
	return 0;
}

/* The compositor's own UI takes no button. */
int
kwl_seat_button_shell(
	struct kwl_server *server,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	/* Not taken. */
	(void)server;
	(void)time;
	(void)button;
	(void)state;
	return 0;
}

/* A pointer motion; the case's client has the tool, so none is expected. */
void
kwl_seat_motion(
	struct kwl_server *server,
	uint32_t time)
{
	/* Counted as a failure: the pen should go by the tablet protocol. */
	(void)server;
	(void)time;
	check(0, "the pen moved the pointer of a client with the tool");
}

/* A pointer motion to a client without the tool. */
void
kwl_seat_motion_deliver(
	struct kwl_server *server,
	uint32_t time)
{
	/* Nothing. */
	(void)server;
	(void)time;
}

/* A pointer button through the compositor. */
void
kwl_seat_button(
	struct kwl_server *server,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	/* Nothing. */
	(void)server;
	(void)time;
	(void)button;
	(void)state;
}

/* A pointer button to a client. */
void
kwl_seat_button_deliver(
	struct kwl_server *server,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	/* Nothing. */
	(void)server;
	(void)time;
	(void)button;
	(void)state;
}

/* The end of a pointer frame. */
void
kwl_seat_frame(
	struct kwl_server *server)
{
	/* Nothing. */
	(void)server;
}
