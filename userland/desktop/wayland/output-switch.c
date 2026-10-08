/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The one output moved to another display while the compositor runs
 * (ws113-p004a; the design is plan/ws113/phase004/phase.md, "p004a").
 *
 * The compositor shows one display at a time.  It follows the displays
 * through the hotplug fence of VK_EXT_display_control (ws113-p003): when
 * the fence signals, a new fence is registered first, the displays are
 * enumerated again, and the old fence goes.  The output moves when it is
 * asked to (kwl_output_use_external and kwl_output_use_internal, for the
 * lid's choice of ws052-p012), and by itself when the display it shows is
 * gone: to the machine's own display first, else to any other.  A move
 * closes the swapchain and gives the display back, opens a swapchain on
 * the other display at its native size, and fits the desktop to that size
 * (the look's images, the windows, the clients' wl_output).  A display
 * whose swapchain is refused (the limit of outputs shown at once) is
 * marked limited and tried again after the next hotplug; the output goes
 * back to the display it had.
 *
 * The machine's own display is the one whose name says it is built in (a
 * key "...:edp:..." of D-ID A2), or, when no name says so (a virtual
 * adapter), the display the compositor started on.
 *
 * The other connected displays are heads (heads.c, ws113-p004b), brought
 * in line after each enumeration, after the output opens again (a move
 * closes the heads with the output), and when a head loses its display.
 * The display the output leaves for an external one under a closed lid is
 * kept off (no head) until the output comes back to it.
 */

#include "kwl.h"
#include "compose.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* How often the hotplug fence is looked at (ms): each look is a request to the display node. */
#define OUTPUT_HOTPLUG_MS	250U

static VkResult output_enumerate(struct kwl_server *server);
static int output_internal(const struct kwl_compose *compose, unsigned index);
static int output_index(const struct kwl_compose *compose, VkDisplayKHR display);
static void output_hotplug_register(struct kwl_server *server);
static void output_recover(struct kwl_server *server);
static int output_fail_back(struct kwl_server *server);
static void output_resized(struct kwl_server *server);

/*
 * Follows the displays once each pass of the event loop (at most every
 * OUTPUT_HOTPLUG_MS): the hotplug fence's signal, and a display gone from
 * under the output.
 */
void
kwl_output_tick(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	VkFence previous;
	VkResult status;
	VkResult listed;
	uint64_t now;
	unsigned signalled;
	int index;
	int lost;

	/* A displays result held back too long is answered (displays-shell.c, BUG-266). */
	now = kwl_milliseconds();
	kwl_displays_pending_tick(server, now);

	/* Only a device that follows the hotplug, while window mode runs. */
	compose = server->compose;
	if (compose == NULL || compose->register_device_event == NULL || !server->windowed)
		return;

	/* Not too often. */
	if (compose->hotplug != VK_NULL_HANDLE && now - compose->hotplug_checked_ms < OUTPUT_HOTPLUG_MS && compose->output_lost != 1U)
		return;
	compose->hotplug_checked_ms = now;

	/* The first look: a fence, the displays as they are, and their heads. */
	if (compose->hotplug == VK_NULL_HANDLE) {
		output_hotplug_register(server);
		output_enumerate(server);
		kwl_heads_sync(server);
		kwl_displays_anchor_follow(server);
		return;
	}

	/* A signal: a new fence first, so that a change during the enumeration is not missed. */
	signalled = 0U;
	status = vkGetFenceStatus(compose->device, compose->hotplug);
	if (status == VK_SUCCESS) {
		signalled = 1U;
		previous = compose->hotplug;
		compose->hotplug = VK_NULL_HANDLE;
		output_hotplug_register(server);
		vkDestroyFence(compose->device, previous, NULL);

		/* The displays now; a display refused before, or whose move failed, is tried again. */
		compose->limited = 0U;
		compose->move_failed = 0U;
		listed = output_enumerate(server);

		/* An output waiting for a display tries the ones there are now. */
		if (compose->output_lost == 2U)
			compose->output_lost = 1U;

		/*
		 * The display shown is gone from the list: the output is lost
		 * now, not at the next frame's acquire, which an idle desktop
		 * may not draw for a long time.  An unreadable list says nothing
		 * (the topology moved under it, and the new fence signals again).
		 */
		index = output_index(compose, compose->display);
		if (listed == VK_SUCCESS &&
		    compose->output_open &&
		    compose->output_lost == 0U &&
		    index < 0) {
			printf("KWL OUTPUT lost operation=hotplug name=%s\n", compose->display_name);
			compose->output_lost = 1U;
		}
	}

	/* The display under the output is gone: the output moves. */
	if (compose->output_lost == 1U)
		output_recover(server);

	/*
	 * The heads brought in line: after the hotplug's enumeration, after the
	 * output opened again (the list is read again first), and when a head
	 * lost its display.
	 */
	lost = kwl_heads_lost(server);
	if (!signalled &&
	    !compose->heads_stale &&
	    !lost)
		return;
	if (!signalled)
		(void)output_enumerate(server);
	kwl_heads_sync(server);

	/* An anchor turned off by the user gives the desktop to a display on (ws113-p014). */
	kwl_displays_anchor_follow(server);
}

/*
 * Moves the output to another display: the swapchain closed and the display
 * given back, a swapchain opened on the other display at its native size,
 * and the desktop fitted to it.  An output closed (its display gone, and
 * no other taken) is opened on the display.  Returns 0, ENODEV before
 * window mode, ENXIO for a display not connected now, or EAGAIN when its
 * swapchain was refused (the output is back on the display it had, when it
 * could be).
 */
int
kwl_output_switch(
	struct kwl_server *server,
	VkDisplayKHR target)
{
	struct kwl_compose *compose;
	char old_name[KWL_COMPOSE_NAME];
	char name[KWL_COMPOSE_NAME];
	VkDisplayKHR old_display;
	uint32_t old_width;
	uint32_t old_height;
	uint32_t old_refresh;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	VkResult result;
	int index;
	int error;
	int reopen;

	/* Only window mode's output moves. */
	compose = server->compose;
	if (compose == NULL || !server->windowed)
		return ENODEV;

	/* The display it shows already. */
	if (target == compose->display && compose->output_open && !compose->output_lost)
		return 0;

	/* The other display's size, refresh and name; one not connected now cannot take the output. */
	result = kwl_compose_display_read(server, target, &width, &height, &refresh, name, sizeof(name));
	if (result != VK_SUCCESS) {
		printf("KWL OUTPUT switch refused name=%s result=%d\n", name, (int)result);
		return ENXIO;
	}

	/* What the output had, to go back to. */
	old_display = compose->display;
	old_width = server->width;
	old_height = server->height;
	old_refresh = server->refresh;
	memcpy(old_name, compose->display_name, sizeof(old_name));

	/* The same display opened again (another generation of it, or back after a wait) is not a move. */
	reopen = 0;
	if (target == old_display)
		reopen = 1;

	/* The swapchain closed and the display given back. */
	kwl_compose_output_close(server);
	compose->output_lost = 0U;

	/* The other display, at its native size. */
	compose->display = target;
	memcpy(compose->display_name, name, sizeof(compose->display_name));
	server->width = width;
	server->height = height;
	server->refresh = refresh;
	error = kwl_compose_output_open(server);
	if (error != 0) {
		/* Refused (the limit of outputs shown at once): marked, and the output goes back. */
		index = output_index(compose, target);
		if (index >= 0)
			compose->limited |= (uint32_t)1U << (unsigned)index;
		printf("KWL OUTPUT switch failed name=%s errno=%d\n", name, error);
		compose->display = old_display;
		memcpy(compose->display_name, old_name, sizeof(compose->display_name));
		server->width = old_width;
		server->height = old_height;
		server->refresh = old_refresh;
		error = kwl_compose_output_open(server);
		if (error != 0) {
			/* Neither display: the output waits for the next hotplug. */
			printf("KWL OUTPUT none: the display it had did not open again errno=%d\n", error);
			compose->output_lost = 2U;
		}

		/* The move was refused. */
		return EAGAIN;
	}

	/*
	 * A move is proven by its first frame (BUG-266): until it is
	 * presented, the display it left is kept to go back to.  The same
	 * display opened again proves nothing new.
	 */
	compose->switch_failed = 0U;
	if (!reopen) {
		compose->switch_proving = 1U;
		compose->switch_from = old_display;
		memcpy(compose->switch_from_name, old_name, sizeof(compose->switch_from_name));
	}

	/* The desktop fitted to the new size, and the clients told. */
	if (width != old_width || height != old_height)
		output_resized(server);
	kwl_outputs_changed(server);

	/* The log tells a move from the same display opened again (the tests read it). */
	if (reopen) {
		printf("KWL OUTPUT reopen name=%s width=%u height=%u refresh_mhz=%u\n", compose->display_name, width, height, refresh);
	} else {
		printf("KWL OUTPUT switch name=%s width=%u height=%u refresh_mhz=%u\n", compose->display_name, width, height, refresh);
	}

	/* Succeeded: the output shows the target display. */
	return 0;
}

/*
 * Tells whether an external display is connected that the output could
 * move to (not the one shown, not refused since the last hotplug).
 */
int
kwl_output_external_available(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	unsigned index;
	int internal;

	/* Without the hotplug there is no list. */
	compose = server->compose;
	if (compose == NULL)
		return 0;

	/* Each display connected at the last enumeration. */
	for (index = 0U; index < compose->display_count; index++) {
		/* Not the machine's own, not the one shown, not refused. */
		internal = output_internal(compose, index);
		if (internal)
			continue;
		if (compose->displays[index] == compose->display)
			continue;
		if ((compose->limited & ((uint32_t)1U << index)) != 0U)
			continue;
		if ((compose->move_failed & ((uint32_t)1U << index)) != 0U)
			continue;
		return 1;
	}

	/* Succeeded: none. */
	return 0;
}

/*
 * Moves the output to the first external display that takes it.  Returns
 * 0, ENOENT when none is connected, or the last move's error.
 */
int
kwl_output_use_external(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	VkDisplayKHR previous;
	unsigned index;
	int internal;
	int shown;
	int error;

	/* Without the hotplug there is no list. */
	compose = server->compose;
	if (compose == NULL)
		return ENODEV;

	/* Each external display, until one takes the output. */
	error = ENOENT;
	for (index = 0U; index < compose->display_count; index++) {
		/* Not the machine's own, not refused, not one whose move failed (BUG-266). */
		internal = output_internal(compose, index);
		if (internal)
			continue;
		if ((compose->limited & ((uint32_t)1U << index)) != 0U)
			continue;
		if ((compose->move_failed & ((uint32_t)1U << index)) != 0U)
			continue;

		/* The move; the machine's own display left (under the closed lid) is kept off, no head for it (ws113-p004b). */
		previous = compose->kept_off;
		shown = output_index(compose, compose->display);
		if (shown >= 0) {
			internal = output_internal(compose, (unsigned)shown);
			if (internal)
				compose->kept_off = compose->display;
		}

		/* The move itself; a refused one keeps off what was kept off before. */
		error = kwl_output_switch(server, compose->displays[index]);
		if (error == 0)
			return 0;
		compose->kept_off = previous;
	}

	/* No external display took the output. */
	return error;
}

/* Tells whether a display connected now is the machine's own (see the file's comment). */
int
kwl_output_display_internal(
	struct kwl_server *server,
	VkDisplayKHR display)
{
	struct kwl_compose *compose;
	int index;
	int internal;

	/* Its place in the list of the last enumeration. */
	compose = server->compose;
	if (compose == NULL)
		return 0;
	index = output_index(compose, display);
	if (index < 0)
		return 0;

	/* Succeeded: whether it is built in. */
	internal = output_internal(compose, (unsigned)index);
	return internal;
}

/*
 * Moves the output back to the machine's own display.  Returns 0 (also
 * when it shows it already), ENOENT when it is not connected, or the
 * move's error.
 */
int
kwl_output_use_internal(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	unsigned index;
	int internal;
	int error;

	/* Without the hotplug there is no list. */
	compose = server->compose;
	if (compose == NULL)
		return ENODEV;

	/* The machine's own display. */
	for (index = 0U; index < compose->display_count; index++) {
		internal = output_internal(compose, index);
		if (internal)
			break;
	}

	/* Not connected. */
	if (index == compose->display_count)
		return ENOENT;

	/* The move; nothing is kept off from now on: the external display becomes a head again (ws113-p004b). */
	compose->kept_off = VK_NULL_HANDLE;
	error = kwl_output_switch(server, compose->displays[index]);
	if (error != 0)
		return error;

	/* Succeeded: the output shows the machine's own display. */
	return 0;
}

/*
 * Reads the displays connected now, with their names, and logs them;
 * an unreadable list keeps the one read before.
 */
static VkResult
output_enumerate(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	VkDisplayPropertiesKHR properties[KWL_COMPOSE_DISPLAYS];
	VkResult result;
	uint32_t count;
	unsigned index;
	int internal;
	int shown;

	/* The displays, up to the ones followed. */
	compose = server->compose;
	count = KWL_COMPOSE_DISPLAYS;
	result = vkGetPhysicalDeviceDisplayPropertiesKHR(compose->physical, &count, properties);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
		printf("KWL OUTPUT displays unreadable result=%d\n", (int)result);
		return result;
	}

	/* Each one's handle and name. */
	compose->display_count = count;
	for (index = 0U; index < count; index++) {
		compose->displays[index] = properties[index].display;
		compose->display_names[index][0] = '\0';
		if (properties[index].displayName != NULL) {
			(void)snprintf(compose->display_names[index], sizeof(compose->display_names[index]), "%s",
			    properties[index].displayName);
		}
	}

	/* Each one in the log, whether it is the machine's own and whether it is shown (the tests read it). */
	for (index = 0U; index < count; index++) {
		internal = output_internal(compose, index);
		shown = 0;
		if (compose->displays[index] == compose->display)
			shown = 1;
		printf("KWL OUTPUT display index=%u name=%s internal=%d shown=%d width=%u height=%u\n",
		       index, compose->display_names[index], internal, shown,
		       properties[index].physicalResolution.width, properties[index].physicalResolution.height);
	}

	/* The count, which the tests read. */
	printf("KWL OUTPUT displays count=%u\n", count);

	/* Succeeded: the list is the displays connected now. */
	return VK_SUCCESS;
}

/* Tells whether a display of the list is the machine's own (see the file's comment). */
static int
output_internal(
	const struct kwl_compose *compose,
	unsigned index)
{
	const char *found;
	unsigned other;

	/* A name that says it is the built-in panel. */
	found = strstr(compose->display_names[index], ":edp:");
	if (found != NULL)
		return 1;

	/* Another display's name says so: this one is not. */
	for (other = 0U; other < compose->display_count; other++) {
		found = strstr(compose->display_names[other], ":edp:");
		if (found != NULL)
			return 0;
	}

	/* Succeeded: no name says; the display the compositor started on is its own. */
	if (compose->displays[index] == compose->boot_display)
		return 1;
	return 0;
}

/* Finds a display's place in the list, or -1. */
static int
output_index(
	const struct kwl_compose *compose,
	VkDisplayKHR display)
{
	unsigned index;

	/* Handles compare only for equality. */
	for (index = 0U; index < compose->display_count; index++) {
		if (compose->displays[index] == display)
			return (int)index;
	}

	/* Not in the list. */
	return -1;
}

/* Registers the next hotplug fence; a failure leaves the hotplug unfollowed until the next look. */
static void
output_hotplug_register(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	VkDeviceEventInfoEXT event;
	VkResult result;

	/* The display hotplug event. */
	compose = server->compose;
	memset(&event, 0, sizeof(event));
	event.sType = VK_STRUCTURE_TYPE_DEVICE_EVENT_INFO_EXT;
	event.deviceEvent = VK_DEVICE_EVENT_TYPE_DISPLAY_HOTPLUG_EXT;
	result = compose->register_device_event(compose->device, &event, NULL, &compose->hotplug);
	if (result != VK_SUCCESS) {
		compose->hotplug = VK_NULL_HANDLE;
		printf("KWL OUTPUT hotplug unavailable result=%d\n", (int)result);
	}
}

/*
 * Moves the output from a display that is lost.  A display still
 * connected (a topology event elsewhere gave it another generation, and
 * its swapchain went out of date) is opened again, so that a hotplug of
 * another display never takes the output from the one chosen.  A display
 * gone gives the output to the machine's own display first, else to any
 * other connected one.  Without one, the output stays closed until a
 * hotplug brings a display.
 */
static void
output_recover(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	VkResult listed;
	unsigned index;
	int shown;
	int error;

	/*
	 * The displays as they are now.  An unreadable list waits for the next
	 * hotplug: the topology moved under the enumeration after the fence's
	 * registration, so the fence signals again.
	 */
	compose = server->compose;
	listed = output_enumerate(server);
	if (listed != VK_SUCCESS) {
		compose->output_lost = 2U;
		return;
	}

	/* A move whose first frame the display refused: back to the display it left (BUG-266). */
	if (compose->switch_failed) {
		error = output_fail_back(server);
		if (error == 0)
			return;
	}

	/* The display it had, when it is still connected and its move did not fail. */
	shown = output_index(compose, compose->display);
	if (shown >= 0 && (compose->move_failed & ((uint32_t)1U << (unsigned)shown)) == 0U) {
		error = kwl_output_switch(server, compose->display);
		if (error == 0)
			return;
	}

	/* The machine's own display first. */
	error = kwl_output_use_internal(server);
	if (error == 0)
		return;

	/* Any other display connected. */
	for (index = 0U; index < compose->display_count; index++) {
		error = kwl_output_switch(server, compose->displays[index]);
		if (error == 0)
			return;
	}

	/* None: the output waits for the next hotplug. */
	compose->output_lost = 2U;
	printf("KWL OUTPUT waiting: no display takes the output errno=%d\n", error);
}

/*
 * Takes the output back to the display it left, after the display it moved
 * to refused the move's first frame (BUG-266): the failed display is not
 * moved to again until the next hotplug, the display left is on again, and
 * the clients hear the displays as they are.  Returns 0, ENXIO when the
 * display left is gone, or the move's error.
 */
static int
output_fail_back(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	char failed_name[KWL_COMPOSE_NAME];
	int failed;
	int back;
	int error;

	/* The failure is taken once, and a result held back for the move says it failed. */
	compose = server->compose;
	compose->switch_failed = 0U;
	compose->switch_proving = 0U;
	kwl_displays_move_settled(server, 1);

	/* The display that failed is not moved to again until the next hotplug. */
	memcpy(failed_name, compose->display_name, sizeof(failed_name));
	failed = output_index(compose, compose->display);
	if (failed >= 0)
		compose->move_failed |= (uint32_t)1U << (unsigned)failed;

	/* The display it left, when it is still connected. */
	back = output_index(compose, compose->switch_from);
	if (back < 0) {
		printf("KWL OUTPUT switch failed name=%s: the display it left is gone\n", failed_name);
		return ENXIO;
	}

	/* The move back. */
	printf("KWL OUTPUT switch failed name=%s back=%s\n", failed_name, compose->switch_from_name);
	error = kwl_output_switch(server, compose->switch_from);
	if (error != 0)
		return error;

	/* Going back is not a move to prove: the display showed the desktop before. */
	compose->switch_proving = 0U;

	/* The display it went back to is on again, and the clients (Settings) hear the displays as they are. */
	kwl_displays_move_failed(server, compose->display_name);

	/* Succeeded: the output is back on the display it left. */
	return 0;
}

/* Fits the desktop to an output of a new size: the look's images, the windows, the pointer. */
static void
output_resized(
	struct kwl_server *server)
{
	int error;

	/* The look's output-sized images. */
	if (server->glass) {
		error = kwl_glass_resize(server);
		if (error != 0)
			printf("KWL OUTPUT resize glass errno=%d\n", error);
	}

	/* The windows. */
	kwl_glass_output_resized(server);

	/* The pointer inside the output, when it is on it (not on a head, ws113-p007). */
	if (server->pointer_output == KWL_PLANE_ANCHOR && server->pointer_x >= (int32_t)server->width)
		server->pointer_x = (int32_t)server->width - 1;
	if (server->pointer_output == KWL_PLANE_ANCHOR && server->pointer_y >= (int32_t)server->height)
		server->pointer_y = (int32_t)server->height - 1;

	/* Everything is drawn again. */
	server->dirty = 1;
	printf("KWL OUTPUT resized width=%u height=%u refresh_mhz=%u\n", server->width, server->height, server->refresh);
}
