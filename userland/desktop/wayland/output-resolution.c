/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The choice of the output's resolution while the compositor runs
 * (resolution.h says what is offered, how a monitor is known and how a
 * trial goes).
 *
 * The modes offered for a display come from its driver through
 * VK_KHR_display (its default first) and the mode shown now.  A trial
 * opens the output again at the new mode on the same display
 * (kwl_output_reopen_mode): the swapchain closed and the display given
 * back, a surface of the mode and a new swapchain, the desktop fitted to
 * the new size and the clients told; the driver changes the display's
 * timing at the first present of a frame of the new size.  The opening's
 * first frame proves it (compose->resolution_proving, compose.c); then
 * the user has the trial's time to keep it.  A trial not kept, a display
 * that did not show its frame, a client of the trial gone, the session
 * locked, a game shown or the display lost puts the old mode back.  Only
 * a kept trial is written to displays.conf, by the monitor's identity.
 *
 * Only the display the desktop is shown on, while it is shown alone, can
 * be changed in this version; another display, or one beside it, is not.
 */

#include "kwl.h"
#include "compose.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How often a trial looks at what calls it off (a lock, a game, the display lost), and how soon a refused opening is asked again (ms). */
#define OUTPUT_RESOLUTION_LOOK_MS	500U
#define OUTPUT_RESOLUTION_RETRY_MS	200U

/*
 * The trial's own state on the event loop's thread: the displays object
 * that asked for the change running (asker and request, while its answer
 * is owed, answered when the switch ends), the object whose client began
 * the trial (owner; its going calls the trial off), the timer of the
 * trial's times, an opening a display change refused (retried until the
 * state's time ends), whether a client's going called the trial off (done
 * at the timer, outside the object's destruction), and whether a keep
 * wrote the choice (saved, for its answer).
 */
static struct {
	struct kwl_object *asker;
	uint32_t request;
	unsigned asked;
	struct kwl_object *owner;
	struct kwl_timer timer;
	unsigned pending_open;
	struct kwl_resolution_mode pending_mode;
	unsigned cancel_soon;
	int saved;
} output_resolution;

static unsigned output_resolution_listed(struct kwl_server *server, VkDisplayKHR display, struct kwl_resolution_mode *listed, unsigned capacity);
static void output_resolution_current(struct kwl_server *server, VkDisplayKHR display, const char *key, struct kwl_resolution_mode *current);
static unsigned output_resolution_why(struct kwl_server *server, VkDisplayKHR display);
static VkDisplayKHR output_resolution_display(struct kwl_server *server, const char *key);
static void output_resolution_do(struct kwl_server *server, const struct kwl_resolution_step *step);
static void output_resolution_open(struct kwl_server *server, const struct kwl_resolution_mode *mode);
static void output_resolution_opened(struct kwl_server *server, int error, void *context);
static void output_resolution_arm(struct kwl_server *server);
static void output_resolution_fired(void *context, struct kwl_timer *timer);
static int output_resolution_called_off(struct kwl_server *server);

/*
 * Makes the modes offered for a display (its connector's key): the
 * driver's list made into the user's (resolution.c) with the mode shown
 * now, and why (KWL_RESOLUTION_WHY_*) none can be chosen now (0: one can).
 * Returns how many were written to offered.
 */
unsigned
kwl_output_resolution_offered(
	struct kwl_server *server,
	VkDisplayKHR display,
	const char *key,
	struct kwl_resolution_mode *offered,
	unsigned capacity,
	unsigned *why)
{
	struct kwl_resolution_mode listed[KWL_RESOLUTION_MODES];
	struct kwl_resolution_mode current;
	unsigned count;
	unsigned made;

	/* The driver's modes and the one shown now. */
	count = output_resolution_listed(server, display, listed, KWL_RESOLUTION_MODES);
	output_resolution_current(server, display, key, &current);

	/* The user's list. */
	made = kwl_resolution_candidates(listed, count, &current, offered, capacity);

	/* Why it cannot be chosen now: one mode, or what stands in the way. */
	*why = output_resolution_why(server, display);
	if (made < 2U)
		*why |= KWL_RESOLUTION_WHY_FIXED;

	/* Succeeded: the modes offered. */
	return made;
}

/*
 * Makes the identity of a display's monitor (resolution.h) from the EDID
 * the system gives, or its connector's key.
 */
void
kwl_output_resolution_identity(
	struct kwl_server *server,
	VkDisplayKHR display,
	const char *key,
	char *identity,
	size_t size)
{
	uint8_t edid[512];
	size_t length;
	int error;

	/* The EDID, when there is one. */
	length = kwl_os_display_edid(server, display, edid, sizeof(edid));

	/* The identity; one that does not fit is the connector's key. */
	error = kwl_resolution_identity(edid, length, key, identity, size);
	if (error != 0)
		(void)snprintf(identity, size, "%s", key);
}

/*
 * Finds the mode the user kept for a display's monitor, when the display
 * still offers it (its exact refresh from the driver).  Returns 1 with it,
 * 0 when none is kept or it is not offered now.
 */
int
kwl_output_resolution_kept(
	struct kwl_server *server,
	VkDisplayKHR display,
	const char *key,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh)
{
	struct kwl_resolution_mode listed[KWL_RESOLUTION_MODES];
	char identity[KWL_RESOLUTION_IDENTITY];
	uint32_t kept_width;
	uint32_t kept_height;
	uint32_t kept_refresh;
	unsigned count;
	int found;

	/* Only once the file is read, and not on the login screen. */
	if (server->compose == NULL || !server->compose->config_loaded || server->greeter)
		return 0;

	/* The monitor's line. */
	kwl_output_resolution_identity(server, display, key, identity, sizeof(identity));
	found = kwl_displays_resolution(&server->compose->config, identity, &kept_width, &kept_height, &kept_refresh);
	if (!found)
		return 0;

	/* Only a mode the display offers now. */
	count = output_resolution_listed(server, display, listed, KWL_RESOLUTION_MODES);
	found = kwl_resolution_find(listed, count, kept_width, kept_height, kept_refresh);
	if (found < 0) {
		printf("KWL RESOLUTION kept unavailable id=%s mode=%ux%u@%u\n", identity, kept_width, kept_height, kept_refresh);
		return 0;
	}

	/* Succeeded: the mode kept, at the driver's refresh. */
	*width = listed[found].width;
	*height = listed[found].height;
	*refresh = listed[found].refresh_mhz;
	printf("KWL RESOLUTION kept id=%s mode=%ux%u@%u\n", identity, *width, *height, *refresh);
	return 1;
}

/*
 * Begins the trial of a mode on a display (its connector's key) for a
 * displays object's set_resolution or reset_resolution (reset: the
 * display's default, and its monitor's choice forgotten when kept):
 * refresh 0 takes the refresh shown now, else the fastest.  Returns 0
 * when the answer is given (or held until the switch ends), or the errno
 * the request is answered with at once: ENODEV for no such display,
 * ENOTSUP for a mode not offered or a display that cannot change now (one
 * mode, not the desktop's), ENOSPC beside another display, EBUSY during a
 * change or a game, EINVAL for no size.
 */
int
kwl_output_resolution_set(
	struct kwl_server *server,
	struct kwl_object *object,
	uint32_t request,
	const char *key,
	uint32_t width,
	uint32_t height,
	uint32_t refresh,
	unsigned reset)
{
	struct kwl_resolution_mode offered[KWL_RESOLUTION_MODES];
	struct kwl_resolution_mode current;
	struct kwl_resolution_mode fallback;
	struct kwl_resolution_mode target;
	struct kwl_resolution_trial before;
	struct kwl_resolution_step step;
	struct kwl_compose *compose;
	char identity[KWL_RESOLUTION_IDENTITY];
	VkDisplayKHR display;
	unsigned count;
	unsigned index;
	unsigned why;
	int found;
	int error;

	/* The display, connected. */
	compose = server->compose;
	display = output_resolution_display(server, key);
	if (compose == NULL || display == VK_NULL_HANDLE)
		return ENODEV;

	/* A size, unless the default is asked for. */
	if (!reset && (width == 0U || height == 0U))
		return EINVAL;

	/* What it offers, and whether it can change now. */
	count = kwl_output_resolution_offered(server, display, key, offered, KWL_RESOLUTION_MODES, &why);
	if ((why & KWL_RESOLUTION_WHY_GAME) != 0U)
		return EBUSY;
	if ((why & KWL_RESOLUTION_WHY_ALONE) != 0U)
		return ENOSPC;
	if ((why & (KWL_RESOLUTION_WHY_FIXED | KWL_RESOLUTION_WHY_ANCHOR)) != 0U)
		return ENOTSUP;

	/* The default, the one marked native. */
	fallback = offered[0];
	for (index = 0U; index < count; index++) {
		if ((offered[index].flags & KWL_RESOLUTION_NATIVE) != 0U)
			fallback = offered[index];
	}

	/* The mode asked for: the default, or one of those offered. */
	output_resolution_current(server, display, key, &current);
	target = fallback;
	if (!reset) {
		found = kwl_resolution_pick(offered, count, width, height, refresh, current.refresh_mhz);
		if (found < 0)
			return ENOTSUP;
		target = offered[found];
	}

	/* An answer owed for a change running is given first. */
	if (output_resolution.asked)
		return EBUSY;

	/* The trial, which a refused opening puts back as it was. */
	kwl_output_resolution_identity(server, display, key, identity, sizeof(identity));
	before = compose->resolution;
	error = kwl_resolution_begin(&compose->resolution, key, identity, &target, &current, &fallback, reset, kwl_milliseconds(), &step);
	if (error != 0)
		return error;
	printf("KWL RESOLUTION set key=%s id=%s mode=%ux%u@%u reset=%u state=%u\n", key, identity, target.width, target.height, target.refresh_mhz,
	    reset, compose->resolution.state);

	/* The mode shown already: answered and kept (or forgotten) at once. */
	output_resolution.asker = object;
	output_resolution.request = request;
	output_resolution.asked = 1U;
	if ((step.what & KWL_RESOLUTION_DO_OPEN) == 0U) {
		output_resolution_do(server, &step);
		return 0;
	}

	/* The opening at the new mode; refused now, nothing changed. */
	compose->resolution_display = display;
	error = kwl_output_reopen_mode(server, step.open.width, step.open.height, step.open.refresh_mhz, output_resolution_opened, NULL);
	if (error != 0) {
		compose->resolution = before;
		output_resolution.asked = 0U;
		printf("KWL RESOLUTION set refused key=%s errno=%d\n", key, error);
		return error;
	}

	/* Succeeded: the answer waits for the switch, and the trial is the asking client's. */
	output_resolution.owner = object;
	output_resolution.pending_open = 0U;
	output_resolution_arm(server);
	kwl_displays_tell(server);
	return 0;
}

/*
 * Keeps the mode in its trial on a display: the choice is written for the
 * monitor (or forgotten, for a reset).  saved is 0 when the choice was
 * given to displays.conf's writer, else its error (-1 when nothing was to
 * be written).  Returns 0, EBUSY while the switch runs, or ENOENT when the
 * display is not in its trial.
 */
int
kwl_output_resolution_keep(
	struct kwl_server *server,
	const char *key,
	int *saved)
{
	struct kwl_resolution_step step;
	int error;

	/* Nothing written yet. */
	*saved = -1;
	if (server->compose == NULL)
		return ENOENT;

	/* The trial ends, kept. */
	error = kwl_resolution_keep(&server->compose->resolution, key, &step);
	if (error != 0)
		return error;
	output_resolution.saved = -1;
	output_resolution_do(server, &step);

	/* Succeeded: the mode stays, written. */
	*saved = output_resolution.saved;
	printf("KWL RESOLUTION keep key=%s saved=%d\n", key, *saved);
	return 0;
}

/*
 * Puts a display in its trial back to the mode it had.  Returns 0, or
 * ENOENT when the display is not in its trial.
 */
int
kwl_output_resolution_revert(
	struct kwl_server *server,
	const char *key)
{
	struct kwl_resolution_step step;
	int error;

	/* Back now, or once the switch shows. */
	if (server->compose == NULL)
		return ENOENT;
	error = kwl_resolution_revert(&server->compose->resolution, key, kwl_milliseconds(), &step);
	if (error != 0)
		return error;
	printf("KWL RESOLUTION revert key=%s\n", key);
	output_resolution_do(server, &step);

	/* Succeeded: the old mode comes back. */
	return 0;
}

/*
 * Takes the first frame of an opening at a new mode: shown (1), or
 * refused by the display (0).
 */
void
kwl_output_resolution_presented(
	struct kwl_server *server,
	int shown)
{
	struct kwl_resolution_step step;
	uint64_t now;

	/* The trial goes on from the frame. */
	now = kwl_milliseconds();
	if (shown) {
		kwl_resolution_shown(&server->compose->resolution, now, &step);
	} else {
		kwl_resolution_failed(&server->compose->resolution, now, &step);
	}
	printf("KWL RESOLUTION presented shown=%d state=%u mode=%ux%u@%u\n", shown, server->compose->resolution.state, server->compose->mode_width,
	    server->compose->mode_height, server->refresh);
	output_resolution_do(server, &step);
}

/*
 * Forgets a displays object that goes: its answer is owed to nobody, and
 * the trial its client began is called off (at the timer, outside the
 * object's destruction).
 */
void
kwl_output_resolution_object_gone(
	struct kwl_object *object)
{
	/* Nobody to answer. */
	if (output_resolution.asked && output_resolution.asker == object)
		output_resolution.asked = 0U;

	/* The trial of a client gone goes back. */
	if (output_resolution.owner != object)
		return;
	output_resolution.owner = NULL;
	output_resolution.cancel_soon = 1U;
	kwl_timer_set(object->client->server, &output_resolution.timer, kwl_milliseconds());
}

/*
 * Tells a display's trial (by its connector's key): its state
 * (KWL_RESOLUTION_*, IDLE when it is not in one) and the time left to keep
 * it (ms).
 */
void
kwl_output_resolution_report(
	struct kwl_server *server,
	const char *key,
	unsigned *state,
	uint32_t *remaining_ms)
{
	const struct kwl_resolution_trial *trial;
	int differs;

	/* Not in a trial. */
	*state = KWL_RESOLUTION_IDLE;
	*remaining_ms = 0U;
	if (server->compose == NULL)
		return;
	trial = &server->compose->resolution;
	if (trial->state == KWL_RESOLUTION_IDLE)
		return;

	/* The display of the trial. */
	differs = strcmp(trial->key, key);
	if (differs != 0)
		return;
	*state = trial->state;
	*remaining_ms = kwl_resolution_remaining(trial, kwl_milliseconds());
}

/*
 * Reads a display's modes from its driver into listed (at most capacity),
 * its default first: the first mode of its native size.  Returns how many.
 */
static unsigned
output_resolution_listed(
	struct kwl_server *server,
	VkDisplayKHR display,
	struct kwl_resolution_mode *listed,
	unsigned capacity)
{
	VkDisplayPropertiesKHR properties[KWL_COMPOSE_DISPLAYS];
	VkDisplayModePropertiesKHR *modes;
	struct kwl_resolution_mode first;
	struct kwl_compose *compose;
	VkExtent2D native;
	VkResult result;
	uint32_t count;
	uint32_t index;
	unsigned written;

	/* The display's native size. */
	compose = server->compose;
	native.width = 0U;
	native.height = 0U;
	count = KWL_COMPOSE_DISPLAYS;
	result = vkGetPhysicalDeviceDisplayPropertiesKHR(compose->physical, &count, properties);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		count = 0U;
	for (index = 0U; index < count; index++) {
		if (properties[index].display == display)
			native = properties[index].physicalResolution;
	}

	/* The modes. */
	count = 0U;
	result = vkGetDisplayModePropertiesKHR(compose->physical, display, &count, NULL);
	if (result != VK_SUCCESS || count == 0U)
		return 0U;
	modes = calloc(count, sizeof(*modes));
	if (modes == NULL)
		return 0U;
	result = vkGetDisplayModePropertiesKHR(compose->physical, display, &count, modes);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
		free(modes);
		return 0U;
	}

	/* Each, while there is room. */
	written = 0U;
	for (index = 0U; index < count && written < capacity; index++) {
		listed[written].width = modes[index].parameters.visibleRegion.width;
		listed[written].height = modes[index].parameters.visibleRegion.height;
		listed[written].refresh_mhz = modes[index].parameters.refreshRate;
		listed[written].flags = 0U;
		written++;
	}
	free(modes);

	/* The first mode of the native size goes first, the others keeping their order. */
	for (index = 0U; index < written; index++) {
		if (listed[index].width != native.width || listed[index].height != native.height)
			continue;
		first = listed[index];
		memmove(&listed[1], &listed[0], index * sizeof(listed[0]));
		listed[0] = first;
		break;
	}

	/* Succeeded: the driver's modes. */
	return written;
}

/* Finds the mode a display shows now: the output's, else the one it would open at. */
static void
output_resolution_current(
	struct kwl_server *server,
	VkDisplayKHR display,
	const char *key,
	struct kwl_resolution_mode *current)
{
	struct kwl_compose *compose;
	VkResult result;

	/* Nothing known. */
	(void)key;
	memset(current, 0, sizeof(*current));
	compose = server->compose;

	/* The output's mode (its panel's, not turned). */
	if (display == compose->display && compose->output_open) {
		current->width = compose->mode_width;
		current->height = compose->mode_height;
		current->refresh_mhz = server->refresh;
		return;
	}

	/* Another display's own choice. */
	result = kwl_compose_display_read(server, display, &current->width, &current->height, &current->refresh_mhz, NULL, 0U);
	if (result != VK_SUCCESS)
		memset(current, 0, sizeof(*current));
}

/* Tells why a display's resolution cannot be chosen now, apart from its one mode (KWL_RESOLUTION_WHY_*; 0: it can). */
static unsigned
output_resolution_why(
	struct kwl_server *server,
	VkDisplayKHR display)
{
	struct kwl_compose *compose;
	unsigned slot;
	unsigned why;

	/* Only the display the desktop is shown on. */
	compose = server->compose;
	why = 0U;
	if (display != compose->display || !compose->output_open || compose->output_lost)
		why |= KWL_RESOLUTION_WHY_ANCHOR;

	/* Shown alone: no head beside it, no mirror of it. */
	for (slot = 0U; slot < KWL_HEADS; slot++) {
		if (compose->heads[slot].open)
			why |= KWL_RESOLUTION_WHY_ALONE;
	}
	if (compose->display_mode == KWL_DISPLAYS_MIRROR && compose->display_count > 1U)
		why |= KWL_RESOLUTION_WHY_ALONE;

	/* Not under a game's own frames. */
	if (server->scanout != NULL)
		why |= KWL_RESOLUTION_WHY_GAME;

	/* Succeeded: what stands in the way. */
	return why;
}

/* Finds the connected display of a connector's key, or VK_NULL_HANDLE. */
static VkDisplayKHR
output_resolution_display(
	struct kwl_server *server,
	const char *key)
{
	struct kwl_compose *compose;
	unsigned index;
	int differs;

	/* Each display of the last enumeration. */
	compose = server->compose;
	if (compose == NULL)
		return VK_NULL_HANDLE;
	for (index = 0U; index < compose->display_count; index++) {
		differs = strcmp(compose->display_names[index], key);
		if (differs == 0)
			return compose->displays[index];
	}

	/* Succeeded: not connected. */
	return VK_NULL_HANDLE;
}

/*
 * Does what a step of the trial asks: the output opened at a mode, the
 * choice written or forgotten, the request answered; then every displays
 * object is told the trial's new state.
 */
static void
output_resolution_do(
	struct kwl_server *server,
	const struct kwl_resolution_step *step)
{
	struct kwl_compose *compose;
	const struct kwl_resolution_trial *trial;
	int error;

	/* The trial's record. */
	compose = server->compose;
	trial = &compose->resolution;

	/* The choice written for the monitor. */
	if ((step->what & KWL_RESOLUTION_DO_SAVE) != 0U) {
		error = kwl_displays_set_resolution(&compose->config, trial->identity, step->save.width, step->save.height, step->save.refresh_mhz);
		output_resolution.saved = error;
		if (error == 0)
			output_resolution.saved = kwl_heads_config_save(server);
		printf("KWL RESOLUTION saved id=%s mode=%ux%u@%u errno=%d\n", trial->identity, step->save.width, step->save.height, step->save.refresh_mhz,
		    output_resolution.saved);
	}

	/* The monitor's choice forgotten (its display opens at its default). */
	if ((step->what & KWL_RESOLUTION_DO_FORGET) != 0U) {
		kwl_displays_forget_resolution(&compose->config, trial->identity);
		output_resolution.saved = kwl_heads_config_save(server);
		printf("KWL RESOLUTION forgot id=%s errno=%d\n", trial->identity, output_resolution.saved);
	}

	/* The output opened at a mode. */
	if ((step->what & KWL_RESOLUTION_DO_OPEN) != 0U)
		output_resolution_open(server, &step->open);

	/* A trial over has no client of its own. */
	if (trial->state == KWL_RESOLUTION_IDLE)
		output_resolution.owner = NULL;

	/* Everyone sees the new state; the timer follows it. */
	output_resolution_arm(server);
	kwl_displays_tell(server);

	/* The request that began the switch is answered after the snapshot that shows its outcome. */
	if ((step->what & KWL_RESOLUTION_DO_ANSWER) != 0U && output_resolution.asked) {
		output_resolution.asked = 0U;
		kwl_displays_resolution_answer(output_resolution.asker, output_resolution.request, step->answer_error);
	}
}

/*
 * Opens the output at a mode the trial asked for: a display change
 * running refuses it, and it is asked again at the timer; any other
 * refusal is a failure of the mode.
 */
static void
output_resolution_open(
	struct kwl_server *server,
	const struct kwl_resolution_mode *mode)
{
	struct kwl_resolution_step step;
	int error;

	/* The opening. */
	output_resolution.pending_open = 0U;
	error = kwl_output_reopen_mode(server, mode->width, mode->height, mode->refresh_mhz, output_resolution_opened, NULL);
	printf("KWL RESOLUTION open mode=%ux%u@%u errno=%d\n", mode->width, mode->height, mode->refresh_mhz, error);
	if (error == 0)
		return;

	/* Another change runs: asked again soon. */
	if (error == EBUSY) {
		output_resolution.pending_open = 1U;
		output_resolution.pending_mode = *mode;
		return;
	}

	/* The mode cannot be opened. */
	kwl_resolution_failed(&server->compose->resolution, kwl_milliseconds(), &step);
	output_resolution_do(server, &step);
}

/* Takes the end of an opening at a mode: refused (the output is back at its old mode), the mode failed. */
static void
output_resolution_opened(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kwl_resolution_step step;

	/* Opened: its first frame decides. */
	(void)context;
	if (error == 0)
		return;

	/* Refused. */
	printf("KWL RESOLUTION opened errno=%d\n", error);
	kwl_resolution_failed(&server->compose->resolution, kwl_milliseconds(), &step);
	output_resolution_do(server, &step);
}

/* Arms the timer while a trial runs: at its state's end, and meanwhile to look at what calls it off. */
static void
output_resolution_arm(
	struct kwl_server *server)
{
	const struct kwl_resolution_trial *trial;
	uint64_t now;
	uint64_t at;

	/* Readied once. */
	if (output_resolution.timer.fire == NULL)
		kwl_timer_init(&output_resolution.timer, "resolution", output_resolution_fired);

	/* No trial: nothing to wait for. */
	trial = &server->compose->resolution;
	if (trial->state == KWL_RESOLUTION_IDLE && !output_resolution.cancel_soon) {
		kwl_timer_disarm(&server->timers, &output_resolution.timer);
		return;
	}

	/* The earliest of the state's end, the next look, and a refused opening's retry. */
	now = kwl_milliseconds();
	at = now + OUTPUT_RESOLUTION_LOOK_MS;
	if (output_resolution.pending_open)
		at = now + OUTPUT_RESOLUTION_RETRY_MS;
	if (trial->state != KWL_RESOLUTION_IDLE && trial->deadline_ms < at)
		at = trial->deadline_ms;
	kwl_timer_set(server, &output_resolution.timer, at);
}

/*
 * Takes the trial's timer: a refused opening asked again, what calls the
 * trial off, and the end of its state.
 */
static void
output_resolution_fired(
	void *context,
	struct kwl_timer *timer)
{
	struct kwl_resolution_step step;
	struct kwl_resolution_mode mode;
	struct kwl_server *server;
	struct kwl_compose *compose;
	unsigned before;
	unsigned off;
	uint64_t now;

	/* The trial. */
	(void)timer;
	server = context;
	compose = server->compose;
	if (compose == NULL)
		return;
	now = kwl_milliseconds();

	/* A client's going, a lock, a game or the display lost calls it off. */
	off = output_resolution.cancel_soon;
	output_resolution.cancel_soon = 0U;
	if (output_resolution_called_off(server))
		off = 1U;
	if (off && compose->resolution.state != KWL_RESOLUTION_IDLE) {
		printf("KWL RESOLUTION called off state=%u\n", compose->resolution.state);
		kwl_resolution_cancel(&compose->resolution, now, &step);
		output_resolution_do(server, &step);
	}

	/* A refused opening, asked again while its state lasts. */
	if (output_resolution.pending_open && now < compose->resolution.deadline_ms) {
		mode = output_resolution.pending_mode;
		output_resolution_open(server, &mode);
		output_resolution_arm(server);
		return;
	}

	/* The state's time; a change of state is done and told. */
	before = compose->resolution.state;
	kwl_resolution_tick(&compose->resolution, now, &step);
	if (step.what != 0U || compose->resolution.state != before) {
		output_resolution_do(server, &step);
		return;
	}
	output_resolution_arm(server);
}

/*
 * Tells whether something calls a trial off: the session locked or
 * ended, a game shown, or the display of the trial no longer under the
 * output.
 */
static int
output_resolution_called_off(
	struct kwl_server *server)
{
	struct kwl_compose *compose;

	/* No trial to call off. */
	compose = server->compose;
	if (compose->resolution.state == KWL_RESOLUTION_IDLE)
		return 0;

	/* The session locked, or the login screen's. */
	if (server->locked || server->greeter)
		return 1;

	/* A game's own frames. */
	if (server->scanout != NULL)
		return 1;

	/* The display lost or left (a switch running keeps its display). */
	if (compose->display != compose->resolution_display)
		return 1;
	if (compose->output_lost && compose->resolution.state == KWL_RESOLUTION_TRIAL)
		return 1;

	/* Succeeded: nothing calls it off. */
	return 0;
}
