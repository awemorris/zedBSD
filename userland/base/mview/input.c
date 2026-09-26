/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Turns pointer and keyboard events into camera changes and logs each change.
 */

#include "model.h"

#include <stdio.h>
#include <string.h>

/* Linux evdev key codes, which zdesktop forwards unchanged with no keymap. */
#define INPUT_KEY_ESC		1U
#define INPUT_KEY_MINUS		12U
#define INPUT_KEY_EQUAL		13U
#define INPUT_KEY_Q		16U
#define INPUT_KEY_R		19U
#define INPUT_KEY_KPMINUS	74U
#define INPUT_KEY_KPPLUS	78U
#define INPUT_KEY_UP		103U
#define INPUT_KEY_LEFT		105U
#define INPUT_KEY_RIGHT		106U
#define INPUT_KEY_DOWN		108U

/* Linux evdev button codes carried by wl_pointer.button. */
#define INPUT_BUTTON_LEFT	0x110U
#define INPUT_BUTTON_RIGHT	0x111U
#define INPUT_BUTTON_MIDDLE	0x112U

/* wl_pointer.axis value for the vertical wheel. */
#define INPUT_AXIS_VERTICAL	0U

/*
 * The scroll distance of one wheel notch when no discrete count is sent.
 * zdesktop, like libinput, reports fifteen units per notch.
 */
#define INPUT_AXIS_NOTCH	15.0

/* Degrees of orbit per pixel of left-button drag. */
#define INPUT_ORBIT_PER_PIXEL	0.5f

/* Degrees of orbit per arrow key press. */
#define INPUT_ORBIT_PER_KEY	5.0f

static void input_log(const struct mview_input *input, const char *kind, const char *detail);

/*
 * Prepares input handling for one camera and one run token.
 *
 * The height converts pan movements from pixels to view-plane units.
 */
void
mview_input_init(
	struct mview_input *input,
	struct mview_camera *camera,
	const char *token,
	uint32_t height)
{
	/* No button is held and the pointer has not entered the surface yet. */
	memset(input, 0, sizeof(*input));
	input->camera = camera;
	input->token = token;
	input->height = height;

	/* Succeeded: input events can now be applied. */
	return;
}

/*
 * Records the pointer position without moving the camera.
 *
 * Entering the surface sets the reference point that the next drag motion is
 * measured from.
 */
void
mview_input_position(
	struct mview_input *input,
	double x,
	double y)
{
	/* The next motion is measured from here. */
	input->x = x;
	input->y = y;
	input->have_position = 1;

	/* Succeeded: the reference position is current. */
	return;
}

/*
 * Applies one pointer motion: left drag orbits, right or middle drag pans.
 */
void
mview_input_motion(
	struct mview_input *input,
	double x,
	double y)
{
	double dx;
	double dy;

	/* A motion without a known previous position only establishes one. */
	if (input->have_position == 0) {
		mview_input_position(input, x, y);
		return;
	}

	/* The movement since the previous position drives any drag. */
	dx = x - input->x;
	dy = y - input->y;
	mview_input_position(input, x, y);

	/* Without a held button the pointer only moves; the view does not change. */
	if (input->left == 0 && input->right == 0 && input->middle == 0)
		return;

	/* A motion that did not move leaves the view unchanged. */
	if (dx == 0.0 && dy == 0.0)
		return;

	/* The left button has priority: it turns the model with the drag. */
	if (input->left != 0) {
		mview_camera_orbit(input->camera, (float)dx * INPUT_ORBIT_PER_PIXEL, (float)dy * INPUT_ORBIT_PER_PIXEL);
	} else {
		mview_camera_pan(input->camera, (float)dx, (float)dy, input->height);
	}

	/*
	 * The drag is logged once per frame by mview_input_flush, so a burst of
	 * motion events produces one line carrying their summed movement.
	 */
	input->changed = 1;
	input->motion_events++;
	input->motion_dx += dx;
	input->motion_dy += dy;

	/* Succeeded: the drag has been applied to the camera. */
	return;
}

/*
 * Logs the drag motions applied since the previous flush as one line.
 *
 * The main loop calls this once per frame; other events call it first so the
 * log keeps the order in which events were applied.
 */
void
mview_input_flush(
	struct mview_input *input)
{
	char detail[128];

	/* Nothing has moved since the previous line. */
	if (input->motion_events == 0U)
		return;

	/* One line reports where the pointer ended and how far it moved. */
	snprintf(
		detail,
		sizeof(detail),
		"x=%.1f y=%.1f dx=%.1f dy=%.1f events=%u",
		input->x,
		input->y,
		input->motion_dx,
		input->motion_dy,
		input->motion_events);
	input_log(input, "motion", detail);

	/* The next drag starts a new summary. */
	input->motion_events = 0U;
	input->motion_dx = 0.0;
	input->motion_dy = 0.0;

	/* Succeeded: the coalesced drag is visible to the harness. */
	return;
}

/*
 * Records a button press or release; the drag itself happens on motion.
 */
void
mview_input_button(
	struct mview_input *input,
	uint32_t button,
	int pressed)
{
	char detail[64];

	/* Maps the evdev button to the drag it controls; others are ignored. */
	switch (button) {
	case INPUT_BUTTON_LEFT:
		input->left = pressed;
		break;
	case INPUT_BUTTON_RIGHT:
		input->right = pressed;
		break;
	case INPUT_BUTTON_MIDDLE:
		input->middle = pressed;
		break;
	default:
		return;
	}

	/* A drag that ends here is logged before its release. */
	mview_input_flush(input);

	/* The harness sees each delivered button even though the view is unchanged. */
	if (pressed != 0) {
		snprintf(detail, sizeof(detail), "button=0x%x state=pressed", button);
	} else {
		snprintf(detail, sizeof(detail), "button=0x%x state=released", button);
	}

	/* Publishes the delivered button. */
	input_log(input, "button", detail);

	/* Succeeded: the button state is current. */
	return;
}

/*
 * Applies one scroll event: the vertical wheel zooms.
 *
 * Positive values scroll down, which moves the viewer away.  A discrete
 * wheel count, when the compositor sent one, is used as is; otherwise the
 * value is rounded to notches and a partial scroll still takes one step.
 */
void
mview_input_axis(
	struct mview_input *input,
	uint32_t axis,
	double value,
	int32_t discrete)
{
	char detail[96];
	double magnitude;
	int notches;

	/* Horizontal scrolling leaves the view unchanged. */
	if (axis != INPUT_AXIS_VERTICAL)
		return;

	/* A zero-length scroll carries no direction. */
	if (value == 0.0 && discrete == 0)
		return;

	/* The scroll distance, regardless of direction. */
	magnitude = value;
	if (magnitude < 0.0)
		magnitude = -magnitude;

	/* Rounds to the nearest notch; a partial scroll still takes one step. */
	notches = (int)(magnitude / INPUT_AXIS_NOTCH + 0.5);
	if (notches < 1)
		notches = 1;

	/* The scroll direction chooses between moving away and moving closer. */
	if (value < 0.0)
		notches = -notches;

	/* A wheel's own click count is exact and replaces the estimate. */
	if (discrete != 0)
		notches = discrete;

	/* A runaway scroll is bounded to a finite number of steps each way. */
	if (notches > 100)
		notches = 100;

	/* The lower bound mirrors the upper one. */
	if (notches < -100)
		notches = -100;

	/* A drag in progress is logged before the zoom. */
	mview_input_flush(input);

	/* Applies and publishes the zoom. */
	mview_camera_zoom(input->camera, notches);
	input->changed = 1;
	snprintf(detail, sizeof(detail), "axis=%u value=%.2f discrete=%d notches=%d", axis, value, (int)discrete, notches);
	input_log(input, "axis", detail);

	/* Succeeded: the zoom has been applied to the camera. */
	return;
}

/*
 * Applies one key press: R resets, arrows orbit, plus and minus zoom, Q and
 * Escape quit.  Releases and unknown keys are ignored.
 */
void
mview_input_key(
	struct mview_input *input,
	uint32_t key,
	int pressed)
{
	char detail[64];

	/* Only presses act; releases carry no command. */
	if (pressed == 0)
		return;

	/* A drag in progress is logged before the key. */
	mview_input_flush(input);

	/* Maps the evdev key to its viewer command. */
	switch (key) {
	case INPUT_KEY_R:
		mview_camera_reset(input->camera);
		break;
	case INPUT_KEY_LEFT:
		mview_camera_orbit(input->camera, -INPUT_ORBIT_PER_KEY, 0.0f);
		break;
	case INPUT_KEY_RIGHT:
		mview_camera_orbit(input->camera, INPUT_ORBIT_PER_KEY, 0.0f);
		break;
	case INPUT_KEY_UP:
		mview_camera_orbit(input->camera, 0.0f, -INPUT_ORBIT_PER_KEY);
		break;
	case INPUT_KEY_DOWN:
		mview_camera_orbit(input->camera, 0.0f, INPUT_ORBIT_PER_KEY);
		break;
	case INPUT_KEY_EQUAL:
	case INPUT_KEY_KPPLUS:
		mview_camera_zoom(input->camera, -1);
		break;
	case INPUT_KEY_MINUS:
	case INPUT_KEY_KPMINUS:
		mview_camera_zoom(input->camera, 1);
		break;
	case INPUT_KEY_Q:
	case INPUT_KEY_ESC:
		/* The main loop ends after the frame in progress. */
		input->quit = 1;
		break;
	default:
		return;
	}

	/* Publishes the command and the view it produced. */
	input->changed = 1;
	snprintf(detail, sizeof(detail), "key=%u state=pressed", key);
	input_log(input, "key", detail);

	/* Succeeded: the key command has been applied. */
	return;
}

/*
 * Forgets held buttons when the pointer leaves or the device goes away.
 *
 * A release that happens outside the surface is never delivered, so a drag
 * must not continue when the pointer returns.
 */
void
mview_input_release_all(
	struct mview_input *input)
{
	/* No drag survives the loss of pointer focus. */
	input->left = 0;
	input->right = 0;
	input->middle = 0;
	input->have_position = 0;

	/* Succeeded: no button is considered held. */
	return;
}

/* Prints one MVIEW INPUT line with the event and the resulting view. */
static void
input_log(
	const struct mview_input *input,
	const char *kind,
	const char *detail)
{
	const struct mview_camera *camera;

	/* The line carries the full view so each event can be checked on its own. */
	camera = input->camera;
	printf(
		"MVIEW INPUT run=%s kind=%s %s yaw=%.2f pitch=%.2f distance=%.4f pan=%.4f,%.4f\n",
		input->token,
		kind,
		detail,
		(double)camera->yaw,
		(double)camera->pitch,
		(double)camera->distance,
		(double)camera->pan[0],
		(double)camera->pan[1]);
	fflush(stdout);

	/* Succeeded: the event is visible to the harness. */
	return;
}
