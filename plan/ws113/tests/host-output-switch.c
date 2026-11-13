/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the compositor's one output moved to another display
 * and of the follow of the displays (userland/desktop/wayland/output-switch.c),
 * with stand-ins for Vulkan's hotplug fence, the stop fence and the queue,
 * for the display thread's steps (run here at once) and for the output's
 * opening, and with the real hotplug thread (threads.c).
 *
 * It checks: the hotplug thread's first fence told, and the displays read
 * once for it; the machine's own display by name and, without such a
 * name, the one started on; a move at the new size with the desktop
 * fitted and the clients told; a refused swapchain marked limited and the
 * output back on the display it had; a hotplug told by the thread, its
 * next fence registered before the signaled one goes, the limit
 * forgotten; no wake and no reading of the list without an event; a
 * display gone and the output moved to the machine's own; the output
 * waiting when no display takes it; a display gone from the list at a
 * hotplug, with no frame drawn, losing the output; a display still listed
 * whose swapchain went out of date opened again rather than left; an
 * unreadable list changing nothing; a hotplug outside window mode, a loss
 * during a change of the display and heads refused for a change running
 * held until what held them ends; a move whose first frame the display
 * refused going back to the display it left (BUG-266); the stop ending
 * the thread's wait and every fence going; a first registration refused
 * still reading the displays once; and a device lost ending the thread's
 * wait.
 */

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/wayland/compose.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The longest a check waits for the hotplug thread, in milliseconds. */
#define TEST_WAIT_MS	5000

/* How long a check that no wake comes listens, in milliseconds. */
#define TEST_QUIET_MS	200

/* The most hotplug fences one run registers. */
#define TEST_FENCES	64U

/* The handles of the stand-in's hotplug fences start above the first, the stop fences' above the second. */
#define TEST_FENCE_BASE	0x1000U
#define TEST_STOP_BASE	0x2000U

/* A display the stand-in reports: its handle, name and size, and whether a swapchain on it is refused. */
struct test_display {
	VkDisplayKHR handle;
	const char *name;
	uint32_t width;
	uint32_t height;
	int refuses;
};

/* The displays connected now, as the display thread's reading would find them. */
static struct test_display test_displays[4];
static unsigned test_display_count;

/* The list cannot be read (the topology moved under the enumeration). */
static int test_list_error;

/*
 * The stand-in's fences, shared with the hotplug thread under test_lock
 * (test_changed is signaled at every change): for each hotplug fence
 * registered whether it is signaled, how many were registered and
 * destroyed, whether a registration is refused, the stop fences made and
 * whether the last one is signaled, whether the device is lost, and how
 * many waits returned.
 */
static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_changed = PTHREAD_COND_INITIALIZER;
static unsigned char test_signaled[TEST_FENCES];
static unsigned test_registered;
static unsigned test_destroyed;
static int test_register_refused;
static unsigned test_stops;
static int test_stop_signaled;
static int test_device_lost;
static unsigned test_waits;

/* The heads brought in line, a head that lost its display, and whether a change of the heads cannot begin now. */
static unsigned test_syncs;
static int test_heads_lost;
static int test_heads_busy;

/* What the output did: opens, closes, resizes, wl_output tellings. */
static unsigned test_opens;
static unsigned test_closes;
static unsigned test_resizes;
static unsigned test_told;

/* The readings of the displays the display thread was asked for. */
static unsigned test_lists;

/* BUG-266: the held displays results answered (and how), and the display a failed move went back to. */
static unsigned test_settled;
static int test_settled_failed;
static char test_back[64];

/* The number of failed checks (the hotplug thread's stand-ins count too). */
static int test_failures;

static void check(int condition, const char *what);
static VKAPI_ATTR VkResult VKAPI_CALL test_register(VkDevice device, const VkDeviceEventInfoEXT *pDeviceEventInfo, const VkAllocationCallbacks *pAllocator, VkFence *pFence);
static void test_display_set(unsigned index, uintptr_t handle, const char *name, uint32_t width, uint32_t height);
static void test_then(struct kwl_server *server, int error, void *context);
static int test_use_external(struct kwl_server *server);
static int test_use_internal(struct kwl_server *server);
static int test_woken(struct kwl_server *server, int milliseconds);
static void test_hotplug(struct kwl_server *server);
static unsigned test_waits_now(void);
static void test_deadline(struct timespec *deadline);
static void test_server(struct kwl_server *server, struct kwl_compose *compose);

/* Stands in for the clock. */
uint64_t
kwl_milliseconds(void)
{
	/* The test does not depend on time. */
	return 1000U;
}

/* Stands in for a fence's creation: the stop fence. */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFence(
	VkDevice device,
	const VkFenceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkFence *pFence)
{
	/* The stand-in has one device. */
	(void)device;
	(void)pCreateInfo;
	(void)pAllocator;

	/* A new stop fence, not signaled. */
	pthread_mutex_lock(&test_lock);

	test_stops++;
	test_stop_signaled = 0;
	*pFence = (VkFence)(uintptr_t)(TEST_STOP_BASE + test_stops);

	pthread_mutex_unlock(&test_lock);

	/* Succeeded: made. */
	return VK_SUCCESS;
}

/* Stands in for a fence's status: a hotplug fence's signal, or the stop. */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetFenceStatus(
	VkDevice device,
	VkFence fence)
{
	uintptr_t handle;
	VkResult status;

	/* The stand-in has one device. */
	(void)device;
	handle = (uintptr_t)fence;

	/* The stop fence, or a hotplug fence. */
	pthread_mutex_lock(&test_lock);

	status = VK_NOT_READY;
	if (handle > TEST_STOP_BASE) {
		if (test_stop_signaled)
			status = VK_SUCCESS;
	} else if (handle > TEST_FENCE_BASE && handle - TEST_FENCE_BASE < TEST_FENCES) {
		if (test_signaled[handle - TEST_FENCE_BASE])
			status = VK_SUCCESS;
	}

	pthread_mutex_unlock(&test_lock);

	/* Succeeded: signaled or not. */
	return status;
}

/* Stands in for the hotplug thread's wait: until a fence it names signals, or the device is lost. */
VKAPI_ATTR VkResult VKAPI_CALL
vkWaitForFences(
	VkDevice device,
	uint32_t fenceCount,
	const VkFence *pFences,
	VkBool32 waitAll,
	uint64_t timeout)
{
	struct timespec deadline;
	uintptr_t handle;
	VkResult status;
	uint32_t index;
	int ended;
	int error;

	/* The hotplug thread waits for any of two fences with no time limit. */
	(void)device;
	check(fenceCount == 2U && !waitAll && timeout == UINT64_MAX, "the hotplug thread waits for its fence or the stop, with no time limit");

	/* Until one signals; a wait the test never ends fails the check instead of hanging. */
	test_deadline(&deadline);
	pthread_mutex_lock(&test_lock);

	status = VK_TIMEOUT;
	for (;;) {
		/* A device lost ends every wait. */
		if (test_device_lost) {
			status = VK_ERROR_DEVICE_LOST;
			break;
		}

		/* Any fence named that signaled. */
		ended = 0;
		for (index = 0U; index < fenceCount; index++) {
			handle = (uintptr_t)pFences[index];
			if (handle > TEST_STOP_BASE && test_stop_signaled)
				ended = 1;
			if (handle > TEST_FENCE_BASE && handle < TEST_STOP_BASE && test_signaled[handle - TEST_FENCE_BASE])
				ended = 1;
		}

		/* One did. */
		if (ended) {
			status = VK_SUCCESS;
			break;
		}

		/* The next change, or the end of the test's patience. */
		error = pthread_cond_timedwait(&test_changed, &test_lock, &deadline);
		if (error == ETIMEDOUT)
			break;
	}
	test_waits++;

	pthread_mutex_unlock(&test_lock);

	/* Succeeded: the wait's result. */
	check(status != VK_TIMEOUT, "the hotplug thread's wait ends at a signal, the stop or the device's loss");
	return status;
}

/* Stands in for the stop's empty submit: the stop fence signals, unless the device is lost. */
VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(
	VkQueue queue,
	uint32_t submitCount,
	const VkSubmitInfo *pSubmits,
	VkFence fence)
{
	VkResult status;

	/* Only the stop's empty submit comes here. */
	(void)queue;
	(void)pSubmits;
	check(submitCount == 0U && (uintptr_t)fence > TEST_STOP_BASE, "the stop is an empty submit of the stop fence");

	/* The stop fence signaled, and the waits told. */
	pthread_mutex_lock(&test_lock);

	status = VK_ERROR_DEVICE_LOST;
	if (!test_device_lost) {
		test_stop_signaled = 1;
		status = VK_SUCCESS;
	}
	pthread_cond_broadcast(&test_changed);

	pthread_mutex_unlock(&test_lock);

	/* Succeeded: submitted, or refused for the device's loss. */
	return status;
}

/* Stands in for a fence's end: the hotplug fences counted, the stop fence passed by. */
VKAPI_ATTR void VKAPI_CALL
vkDestroyFence(
	VkDevice device,
	VkFence fence,
	const VkAllocationCallbacks *pAllocator)
{
	uintptr_t handle;

	/* The stand-in has one device. */
	(void)device;
	(void)pAllocator;
	handle = (uintptr_t)fence;

	/* The stop fence. */
	if (handle > TEST_STOP_BASE)
		return;

	/*
	 * A hotplug fence that was registered, destroyed once; before the stop
	 * and the device's loss only after the next one is registered.
	 */
	pthread_mutex_lock(&test_lock);

	check(handle > TEST_FENCE_BASE, "only a registered fence is destroyed");
	test_destroyed++;
	check(test_registered >= test_destroyed, "no fence is destroyed twice");
	if (!test_stop_signaled && !test_device_lost)
		check(handle - TEST_FENCE_BASE < test_registered, "the next fence is registered before the signaled one goes");

	pthread_mutex_unlock(&test_lock);
}

/* Stands in for the queue's lock (the render and cursor threads do not run here). */
void
kwl_queue_lock(
	struct kwl_compose *compose)
{
	/* Nothing else submits. */
	(void)compose;
}

/* Stands in for the queue's lock given back. */
void
kwl_queue_unlock(
	struct kwl_compose *compose)
{
	/* Nothing else submits. */
	(void)compose;
}

/* Stands in for reading a display's size and name. */
VkResult
kwl_compose_display_read(
	struct kwl_server *server,
	VkDisplayKHR display,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh,
	char *name,
	size_t size)
{
	unsigned index;

	/* The stand-in's list. */
	(void)server;
	for (index = 0U; index < test_display_count; index++) {
		if (test_displays[index].handle == display) {
			*width = test_displays[index].width;
			*height = test_displays[index].height;
			*refresh = 60000U;
			(void)snprintf(name, size, "%s", test_displays[index].name);
			return VK_SUCCESS;
		}
	}

	/* Not connected. */
	return VK_ERROR_SURFACE_LOST_KHR;
}

/* Stands in for the display thread's reading of the displays, run here at once. */
void
kwl_display_enumerate(
	struct kwl_server *server,
	struct kwl_display_list *list,
	kwl_display_fn *then,
	void *context)
{
	unsigned index;

	/* An unreadable list, as when the topology moves under the walk. */
	test_lists++;
	memset(list, 0, sizeof(*list));
	list->result = VK_SUCCESS;
	if (test_list_error)
		list->result = VK_ERROR_UNKNOWN;

	/* Else the displays connected now. */
	for (index = 0U; index < test_display_count && list->result == VK_SUCCESS; index++) {
		list->displays[index] = test_displays[index].handle;
		(void)snprintf(list->names[index], sizeof(list->names[index]), "%s", test_displays[index].name);
		list->widths[index] = test_displays[index].width;
		list->heights[index] = test_displays[index].height;
		list->count++;
	}

	/* Succeeded: the reading goes on. */
	then(server, 0, context);
}

/* Stands in for whether a change of the display runs. */
int
kwl_display_busy(
	struct kwl_server *server)
{
	/* Succeeded: the compositor's flag. */
	return (int)server->compose->display_busy;
}

/* Stands in for the wait for a change running (the steps run here at once, so none runs). */
void
kwl_display_settle(
	struct kwl_server *server)
{
	/* Nothing runs. */
	(void)server;
}

/* Stands in for the beginning of a change: the render thread parked at once. */
int
kwl_display_begin(
	struct kwl_server *server,
	kwl_display_fn *parked,
	void *context)
{
	/* One change at a time. */
	if (server->compose->display_busy)
		return EBUSY;

	/* Succeeded: the change's first step. */
	server->compose->display_busy = 1U;
	parked(server, 0, context);
	return 0;
}

/* Stands in for the end of a change: a follow held back by it is woken, as the real end does. */
void
kwl_display_end(
	struct kwl_server *server)
{
	/* Succeeded: no change runs. */
	server->compose->display_busy = 0U;
	kwl_output_follow_wake(server);
}

/* Stands in for the output's close by the display thread. */
void
kwl_display_close(
	struct kwl_server *server,
	kwl_display_fn *then,
	void *context)
{
	/* Succeeded: closed. */
	test_closes++;
	server->compose->output_open = 0U;
	then(server, 0, context);
}

/* Stands in for the output's opening by the display thread: on a connected display that takes a swapchain. */
void
kwl_display_open(
	struct kwl_server *server,
	kwl_display_fn *then,
	void *context)
{
	unsigned index;

	/* Opened on a connected display that takes a swapchain. */
	test_opens++;
	for (index = 0U; index < test_display_count; index++) {
		if (test_displays[index].handle == server->compose->display && !test_displays[index].refuses) {
			server->compose->output_open = 1U;
			then(server, 0, context);
			return;
		}
	}

	/* Refused, or gone. */
	then(server, EIO, context);
}

/* Stands in for the look's images. */
int
kwl_glass_resize(
	struct kwl_server *server)
{
	/* Counted. */
	(void)server;
	test_resizes++;
	return 0;
}

/* Stands in for the windows' fitting. */
void
kwl_glass_output_resized(
	struct kwl_server *server)
{
	/* Nothing to fit. */
	(void)server;
}

/* Stands in for bringing the heads in line (heads.c), at once, or refused while the test says a change runs. */
int
kwl_heads_sync_later(
	struct kwl_server *server)
{
	/* Another change running. */
	if (test_heads_busy)
		return EBUSY;

	/* Succeeded: counted, the heads in line. */
	server->compose->heads_stale = 0U;
	test_syncs++;
	return 0;
}

/* Stands in for the anchor's move off a display turned off (heads.c, ws113-p014): no display is off here. */
int
kwl_displays_anchor_follow_later(
	struct kwl_server *server)
{
	/* Nothing to follow. */
	(void)server;
	return 0;
}

/* Stands in for the held displays result's answer (displays-shell.c, BUG-266). */
void
kwl_displays_move_settled(
	struct kwl_server *server,
	int failed)
{
	/* Counted, with how the move ended. */
	(void)server;
	test_settled++;
	test_settled_failed = failed;
}

/* Stands in for the choice taking in a failed move (heads.c, BUG-266). */
void
kwl_displays_move_failed(
	struct kwl_server *server,
	const char *back)
{
	/* The display gone back to. */
	(void)server;
	(void)snprintf(test_back, sizeof(test_back), "%s", back);
}

/* Stands in for a head's lost display (heads.c). */
int
kwl_heads_lost(
	struct kwl_server *server)
{
	/* As the test says. */
	(void)server;
	return test_heads_lost;
}

/* Stands in for the outputs' rectangles (heads.c): the anchor alone, at the desktop's size. */
unsigned
kwl_outputs(
	struct kwl_server *server,
	struct kwl_plane_rect *outputs)
{
	/* No head is open here. */
	memset(outputs, 0, KWL_PLANE_SLOTS * sizeof(outputs[0]));
	outputs[KWL_PLANE_ANCHOR].width = server->width;
	outputs[KWL_PLANE_ANCHOR].height = server->height;

	/* Succeeded: every slot. */
	return KWL_PLANE_SLOTS;
}

/* Stands in for the pointer's warp (input.c, ws213-p004): the move applied to the pointer on the anchor. */
void
kwl_pointer_warp(
	struct kwl_server *server,
	const struct kwl_plane_rect *outputs,
	kwl_cursor_box_move_fn *move,
	const void *context)
{
	unsigned output;

	/* The pointer is on the anchor here. */
	output = KWL_PLANE_ANCHOR;
	move(context, outputs, &server->pointer_x, &server->pointer_y, &output);
}

/* Stands in for the input's end of what the absolute devices did at an old angle (input.c, ws217-p003): none here. */
void
kwl_input_rotated(
	struct kwl_server *server)
{
	/* No device. */
	(void)server;
}

/* Stands in for giving the input thread the geometry (input.c, ws217-p003): no thread here. */
void
kwl_input_geometry(
	struct kwl_server *server)
{
	/* No thread. */
	(void)server;
}

/* Stands in for telling the clients. */
void
kwl_outputs_changed(
	struct kwl_server *server)
{
	/* Counted. */
	(void)server;
	test_told++;
}

/*
 * Runs the checks.
 */
int
main(void)
{
	struct kwl_server server;
	struct kwl_compose compose;
	unsigned lists;
	unsigned waits;
	int available;
	int woken;
	int error;

	/* The machine's panel and an HDMI display, the compositor on the panel in window mode. */
	test_server(&server, &compose);
	test_display_set(0U, 0x11U, "zedbsd-port-v1:pci:0000:00:02.0:edp:A", 1920U, 1200U);
	test_display_set(1U, 0x22U, "zedbsd-port-v1:pci:0000:00:02.0:hdmi:B", 1280U, 720U);
	test_display_count = 2U;
	compose.display = (VkDisplayKHR)(uintptr_t)0x11U;
	compose.boot_display = compose.display;
	compose.output_open = 1U;

	/* The thread's first fence, told by a wake; the displays read once and the heads brought in line. */
	kwl_output_hotplug_start(&server);
	check(compose.hotplug.started == 1U, "the hotplug thread starts");
	woken = test_woken(&server, TEST_WAIT_MS);
	check(woken, "the first fence wakes the loop");
	kwl_output_hotplug_take(&server);
	check(test_registered == 1U && compose.display_count == 2U && test_lists == 1U, "the first look registers and lists");
	check(test_syncs == 1U, "the first look brings the heads in line");
	available = kwl_output_external_available(&server);
	check(available, "the HDMI display is external and available");

	/* Without an event, no wake and no reading of the displays. */
	woken = test_woken(&server, TEST_QUIET_MS);
	kwl_output_hotplug_take(&server);
	check(!woken && test_lists == 1U, "nothing looks at the displays without an event");

	/* The move to the external display: closed, opened at its size, resized, told. */
	error = test_use_external(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x22U, "the output moves to HDMI");
	check(compose.kept_off == (VkDisplayKHR)(uintptr_t)0x11U, "the panel left under the lid is kept off (no head)");
	check(server.width == 1280U && server.height == 720U && test_resizes == 1U && test_told == 1U, "the desktop fits 1280x720 and the clients are told");
	check(server.pointer_x == 1279 && server.pointer_y == 719, "the pointer stays inside");
	available = kwl_output_external_available(&server);
	check(!available, "the external display shown is not offered again");

	/* Back to the machine's own display. */
	error = test_use_internal(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x11U && server.width == 1920U, "the output comes back to the panel");
	check(compose.kept_off == VK_NULL_HANDLE, "nothing is kept off once the output is back on the panel");

	/* A refused swapchain: limited, and the output back on the panel. */
	test_displays[1].refuses = 1;
	error = test_use_external(&server);
	check(error == EAGAIN && compose.display == (VkDisplayKHR)(uintptr_t)0x11U && server.width == 1920U, "a refused move goes back");
	check((compose.limited & 2U) != 0U, "the refused display is limited");
	check(compose.kept_off == VK_NULL_HANDLE, "a refused move keeps nothing off");
	available = kwl_output_external_available(&server);
	check(!available, "a limited display is not offered");

	/* A hotplug: the thread registers the next fence before the signaled one goes, wakes once; the limit is forgotten. */
	test_displays[1].refuses = 0;
	waits = test_waits_now();
	test_hotplug(&server);
	check(test_registered == 2U && test_destroyed == 1U && compose.limited == 0U, "a hotplug registers again and forgets the limit");
	check(test_lists == 2U, "a hotplug reads the displays once");
	check(test_waits_now() == waits + 1U, "the thread woke once for the hotplug");

	/* On HDMI, which is unplugged: the frames' loss asks for the follow, which moves the output back to the panel. */
	error = test_use_external(&server);
	check(error == 0, "on HDMI again");
	test_display_count = 1U;
	compose.output_lost = 1U;
	kwl_output_follow_later(&server);
	woken = test_woken(&server, TEST_WAIT_MS);
	kwl_output_hotplug_take(&server);
	check(woken, "a lost output wakes the loop");
	check(compose.display == (VkDisplayKHR)(uintptr_t)0x11U && compose.output_lost == 0U && server.width == 1920U, "a lost display moves the output to the panel");

	/* Without any display: the output waits for the next hotplug, then opens on the one that comes. */
	test_display_count = 0U;
	compose.output_lost = 1U;
	kwl_output_follow_later(&server);
	(void)test_woken(&server, TEST_WAIT_MS);
	kwl_output_hotplug_take(&server);
	check(compose.output_lost == 2U, "no display: the output waits");
	test_display_set(0U, 0x33U, "zedbsd-port-v1:pci:0000:00:02.0:hdmi:B", 1024U, 768U);
	test_display_count = 1U;
	test_hotplug(&server);
	check(compose.output_lost == 0U && compose.display == (VkDisplayKHR)(uintptr_t)0x33U && server.width == 1024U, "a display that comes takes the waiting output");

	/* A virtual adapter: no name says built in; the display started on is the machine's own. */
	test_display_set(0U, 0x44U, "Venus virtual display 0", 1280U, 800U);
	test_display_set(1U, 0x55U, "Venus virtual display 1", 1024U, 768U);
	test_display_count = 2U;
	compose.display = (VkDisplayKHR)(uintptr_t)0x44U;
	compose.boot_display = compose.display;
	compose.output_open = 1U;
	compose.output_lost = 0U;
	test_hotplug(&server);
	error = test_use_external(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x55U, "the other virtual display is external");
	error = test_use_internal(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x44U, "the virtual display started on is the machine's own");

	/* A move to the display shown changes nothing. */
	test_opens = 0U;
	error = test_use_internal(&server);
	check(error == 0 && test_opens == 0U, "the display shown is not opened again");

	/* On the external display, out of date (a hotplug elsewhere): opened again there, not moved. */
	error = test_use_external(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x55U, "on the external virtual display");
	test_opens = 0U;
	compose.output_lost = 1U;
	kwl_output_follow_later(&server);
	(void)test_woken(&server, TEST_WAIT_MS);
	kwl_output_hotplug_take(&server);
	check(compose.display == (VkDisplayKHR)(uintptr_t)0x55U && compose.output_lost == 0U && test_opens == 1U, "an out-of-date display still listed is opened again");
	check(server.width == 1024U, "the size stays the external display's");

	/* A hotplug whose list cannot be read: nothing is lost, the list read before stays. */
	test_list_error = 1;
	test_hotplug(&server);
	check(compose.output_lost == 0U && compose.display_count == 2U, "an unreadable list changes nothing");
	test_list_error = 0;

	/* The external display unplugged while no frame is drawn: the hotplug alone moves the output. */
	test_display_count = 1U;
	test_hotplug(&server);
	check(compose.display == (VkDisplayKHR)(uintptr_t)0x44U && compose.output_lost == 0U && server.width == 1280U, "a display gone from the list loses the output and it moves");

	/* A hotplug outside window mode (the game mode) is held, and followed once window mode runs again. */
	test_display_count = 2U;
	server.windowed = 0U;
	lists = test_lists;
	test_hotplug(&server);
	check(test_lists == lists && compose.follow_pending == 1U, "a hotplug outside window mode reads nothing yet");
	server.windowed = 1U;
	kwl_output_follow_wake(&server);
	woken = test_woken(&server, TEST_WAIT_MS);
	kwl_output_hotplug_take(&server);
	check(woken && test_lists == lists + 1U && compose.display_count == 2U, "window mode again reads the displays the hotplug changed");

	/* A loss during a change of the display is held without another wake, and followed at the change's end. */
	compose.display_busy = 1U;
	compose.output_lost = 1U;
	test_display_count = 1U;
	kwl_output_follow_later(&server);
	(void)test_woken(&server, TEST_WAIT_MS);
	kwl_output_hotplug_take(&server);
	check(compose.output_lost == 1U && compose.follow_pending == 1U, "a loss during a change waits for its end");
	woken = test_woken(&server, TEST_QUIET_MS);
	check(!woken, "a held follow does not wake the loop again by itself");
	kwl_display_end(&server);
	woken = test_woken(&server, TEST_WAIT_MS);
	kwl_output_hotplug_take(&server);
	check(woken && compose.output_lost == 0U && compose.display == (VkDisplayKHR)(uintptr_t)0x44U, "the change's end follows the loss");

	/* Heads that cannot be brought in line during another change stay owed, and are at its end. */
	test_heads_busy = 1;
	test_display_count = 2U;
	test_hotplug(&server);
	check(compose.heads_stale == 1U && compose.follow_pending == 1U, "heads refused for a change running stay owed");
	test_heads_busy = 0;
	kwl_output_follow_wake(&server);
	(void)test_woken(&server, TEST_WAIT_MS);
	kwl_output_hotplug_take(&server);
	check(compose.heads_stale == 0U, "the heads are brought in line after the change");

	/*
	 * BUG-266: on the panel with a USB-C DisplayPort display; the move to it
	 * is not proven until its first frame, which the display refuses: the
	 * output goes back to the panel, the held result says the move failed,
	 * and the DisplayPort display is not moved to again until a hotplug.
	 */
	test_display_set(0U, 0x11U, "zedbsd-port-v1:pci:0000:00:02.0:edp:A", 1920U, 1080U);
	test_display_set(1U, 0x66U, "zedbsd-port-v1:pci:0000:00:02.0:dp:TC2", 1920U, 1280U);
	test_display_count = 2U;
	compose.display = (VkDisplayKHR)(uintptr_t)0x11U;
	compose.boot_display = compose.display;
	compose.output_open = 1U;
	compose.output_lost = 0U;
	compose.kept_off = VK_NULL_HANDLE;
	test_hotplug(&server);
	error = test_use_external(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x66U, "the output moves to the DisplayPort display");
	check(compose.switch_proving == 1U && compose.switch_from == (VkDisplayKHR)(uintptr_t)0x11U, "the move waits for its first frame, the panel kept to go back to");
	compose.switch_proving = 0U;
	compose.switch_failed = 1U;
	compose.output_lost = 1U;
	test_settled = 0U;
	kwl_output_follow_later(&server);
	(void)test_woken(&server, TEST_WAIT_MS);
	kwl_output_hotplug_take(&server);
	check(compose.display == (VkDisplayKHR)(uintptr_t)0x11U && compose.output_lost == 0U && server.height == 1080U, "a refused first frame takes the output back to the panel");
	check(compose.switch_failed == 0U && compose.switch_proving == 0U, "the way back is not a move to prove");
	check(test_settled == 1U && test_settled_failed, "the held result says the move failed");
	check(strcmp(test_back, "zedbsd-port-v1:pci:0000:00:02.0:edp:A") == 0, "the choice takes in the panel gone back to");
	check((compose.move_failed & 2U) != 0U && (compose.limited & 2U) == 0U, "the failed display is marked, not limited (it may still be a head)");
	available = kwl_output_external_available(&server);
	check(!available, "a display whose move failed is not offered");
	error = test_use_external(&server);
	check(error == ENOENT && compose.display == (VkDisplayKHR)(uintptr_t)0x11U, "the output does not move to it again");
	test_hotplug(&server);
	check(compose.move_failed == 0U, "a hotplug forgets the failed move");

	/* The stop: the empty submit ends the thread's wait, and every hotplug fence goes. */
	kwl_output_hotplug_stop(&server);
	check(compose.hotplug.started == 0U, "the hotplug thread is joined");
	check(test_registered == test_destroyed, "every hotplug fence is destroyed");
	close(server.wake[0]);
	close(server.wake[1]);

	/* A first registration refused: the thread ends, and the displays are read once all the same. */
	test_server(&server, &compose);
	test_register_refused = 1;
	lists = test_lists;
	kwl_output_hotplug_start(&server);
	woken = test_woken(&server, TEST_WAIT_MS);
	kwl_output_hotplug_take(&server);
	check(woken && test_lists == lists + 1U, "without a fence the displays are still read once");
	kwl_output_hotplug_stop(&server);
	check(compose.hotplug.started == 0U, "the thread that ended is joined");
	test_register_refused = 0;
	close(server.wake[0]);
	close(server.wake[1]);

	/* A device lost: the thread's wait ends on the loss, the stop's submit is refused, and the thread is joined. */
	test_server(&server, &compose);
	kwl_output_hotplug_start(&server);
	(void)test_woken(&server, TEST_WAIT_MS);
	kwl_output_hotplug_take(&server);
	pthread_mutex_lock(&test_lock);
	test_device_lost = 1;
	pthread_cond_broadcast(&test_changed);
	pthread_mutex_unlock(&test_lock);
	woken = test_woken(&server, TEST_WAIT_MS);
	check(woken, "the device's loss is told");
	kwl_output_hotplug_take(&server);
	kwl_output_hotplug_stop(&server);
	check(compose.hotplug.started == 0U && test_registered == test_destroyed, "a lost device's thread is joined and its fence goes");
	close(server.wake[0]);
	close(server.wake[1]);

	/* Reports the outcome. */
	if (test_failures != 0) {
		fprintf(stderr, "host-output-switch: %d checks failed\n", test_failures);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("host-output-switch: PASS\n");
	return 0;
}

/* Counts and reports one failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A failed check is reported and counted. */
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", what);
		test_failures++;
	}
}

/* Stands in for the hotplug registration: numbered fences, or a refusal as the test says. */
static VKAPI_ATTR VkResult VKAPI_CALL
test_register(
	VkDevice device,
	const VkDeviceEventInfoEXT *pDeviceEventInfo,
	const VkAllocationCallbacks *pAllocator,
	VkFence *pFence)
{
	VkResult status;

	/* The hotplug event. */
	(void)device;
	(void)pAllocator;
	check(pDeviceEventInfo->deviceEvent == VK_DEVICE_EVENT_TYPE_DISPLAY_HOTPLUG_EXT, "the hotplug event is registered");

	/* A numbered fence, not signaled, unless the test refuses it. */
	pthread_mutex_lock(&test_lock);

	status = VK_ERROR_OUT_OF_HOST_MEMORY;
	if (!test_register_refused && test_registered + 1U < TEST_FENCES) {
		test_registered++;
		test_signaled[test_registered] = 0U;
		*pFence = (VkFence)(uintptr_t)(TEST_FENCE_BASE + test_registered);
		status = VK_SUCCESS;
	}

	pthread_mutex_unlock(&test_lock);

	/* Succeeded: registered, or refused. */
	return status;
}

/* Sets one display of the stand-in's list. */
static void
test_display_set(
	unsigned index,
	uintptr_t handle,
	const char *name,
	uint32_t width,
	uint32_t height)
{
	/* The display's fields. */
	test_displays[index].handle = (VkDisplayKHR)handle;
	test_displays[index].name = name;
	test_displays[index].width = width;
	test_displays[index].height = height;
	test_displays[index].refuses = 0;
}

/* Keeps the result of a move, told at its end. */
static void
test_then(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* The result for the caller. */
	(void)server;
	*(int *)context = error;
}

/* Moves the output to an external display; returns the error the move could not begin with, or its result. */
static int
test_use_external(
	struct kwl_server *server)
{
	int result;
	int error;

	/* The move, which the stand-ins run to its end at once. */
	result = EINPROGRESS;
	error = kwl_output_use_external(server, test_then, &result);
	if (error != 0)
		return error;

	/* Succeeded: the move's result. */
	return result;
}

/* Moves the output to the machine's own display; returns the error the move could not begin with, or its result. */
static int
test_use_internal(
	struct kwl_server *server)
{
	int result;
	int error;

	/* The move, which the stand-ins run to its end at once. */
	result = EINPROGRESS;
	error = kwl_output_use_internal(server, test_then, &result);
	if (error != 0)
		return error;

	/* Succeeded: the move's result. */
	return result;
}

/* Waits up to some milliseconds for a byte on the loop's wake pipe and empties it; returns 1 when one came. */
static int
test_woken(
	struct kwl_server *server,
	int milliseconds)
{
	unsigned char bytes[64];
	struct pollfd wake;
	ssize_t got;
	int ready;

	/* The wake pipe's read end. */
	wake.fd = server->wake[0];
	wake.events = POLLIN;
	wake.revents = 0;
	ready = poll(&wake, 1U, milliseconds);
	if (ready <= 0)
		return 0;

	/* Emptied, as the loop does. */
	do {
		got = read(server->wake[0], bytes, sizeof(bytes));
	} while (got > 0);

	/* Succeeded: woken. */
	return 1;
}

/* Signals the newest hotplug fence and takes the thread's note at its wake, as the loop would. */
static void
test_hotplug(
	struct kwl_server *server)
{
	unsigned registered;
	int woken;

	/* The newest fence signaled. */
	pthread_mutex_lock(&test_lock);

	registered = test_registered;
	test_signaled[registered] = 1U;
	pthread_cond_broadcast(&test_changed);

	pthread_mutex_unlock(&test_lock);

	/* The thread's note, then the follow. */
	woken = test_woken(server, TEST_WAIT_MS);
	check(woken, "a hotplug wakes the loop");
	kwl_output_hotplug_take(server);
	check(test_registered == registered + 1U, "a hotplug registers the next fence");
}

/* Gives how many times the hotplug thread's wait has returned. */
static unsigned
test_waits_now(void)
{
	unsigned waits;

	/* Read under the stand-in's lock. */
	pthread_mutex_lock(&test_lock);

	waits = test_waits;

	pthread_mutex_unlock(&test_lock);

	/* Succeeded: the count. */
	return waits;
}

/* Gives the latest time a stand-in's wait may last. */
static void
test_deadline(
	struct timespec *deadline)
{
	/* Now, and the test's patience after. */
	(void)clock_gettime(CLOCK_REALTIME, deadline);
	deadline->tv_sec += TEST_WAIT_MS / 1000;
}

/* Gives the server and the compositor's state a new start: window mode, a device with the hotplug, a wake pipe. */
static void
test_server(
	struct kwl_server *server,
	struct kwl_compose *compose)
{
	int error;

	/* The state, in window mode on a device whose library offers the hotplug. */
	memset(server, 0, sizeof(*server));
	memset(compose, 0, sizeof(*compose));
	server->compose = compose;
	server->windowed = 1U;
	server->glass = 1U;
	server->width = 1920U;
	server->height = 1200U;
	server->pointer_x = 1900;
	server->pointer_y = 1100;
	compose->device = (VkDevice)(uintptr_t)0x77U;
	compose->register_device_event = test_register;

	/* The loop's wake pipe, whose ends do not block. */
	error = pipe(server->wake);
	if (error != 0) {
		fprintf(stderr, "host-output-switch: pipe errno=%d\n", errno);
		exit(1);
	}
	(void)fcntl(server->wake[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(server->wake[1], F_SETFL, O_NONBLOCK);
}
