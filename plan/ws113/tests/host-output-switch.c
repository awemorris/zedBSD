/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the compositor's one output moved to another display
 * (ws113-p004a, userland/desktop/wayland/output-switch.c), with stand-ins
 * for Vulkan's display list and hotplug fence and for the output's
 * opening: the machine's own display by name and, without such a name,
 * the one started on; a move at the new size with the desktop fitted and
 * the clients told; a refused swapchain marked limited and the output back
 * on the display it had; a display gone and the output moved to the
 * machine's own; the hotplug fence registered again before the old one
 * goes; the output waiting when no display takes it; a display gone from
 * the list at a hotplug, with no frame drawn, losing the output; a
 * display still listed whose swapchain went out of date opened again
 * rather than left; and an unreadable list changing nothing.  BUG-266:
 * a move whose first frame the display refused goes back to the display
 * it left, the held result told it failed, and the display not moved to
 * again until the next hotplug.
 */

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/wayland/compose.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The displays the stand-in Vulkan reports: handles, names and sizes. */
struct test_display {
	VkDisplayKHR handle;
	const char *name;
	uint32_t width;
	uint32_t height;
	/* The swapchain on it is refused. */
	int refuses;
};

/* The displays connected now. */
static struct test_display test_displays[4];
static unsigned test_display_count;

/* The list cannot be read (the topology moved under the enumeration). */
static int test_list_error;

/* The hotplug fence's state: signaled, how many were registered and destroyed. */
static int test_hotplug_signaled;
static unsigned test_registered;
static unsigned test_destroyed;

/* How often the heads were brought in line, and whether a head lost its display (heads.c, ws113-p004b). */
static unsigned test_syncs;
static int test_heads_lost;

/* What the output did: opens, closes, resizes, wl_output tellings. */
static unsigned test_opens;
static unsigned test_closes;
static unsigned test_resizes;
static unsigned test_told;

/* The clock. */
static uint64_t test_now;

/* BUG-266: the held displays results answered (and how), and the display a failed move went back to. */
static unsigned test_settled;
static int test_settled_failed;
static char test_back[64];

/* The number of failed checks. */
static int test_failures;

static void check(int condition, const char *what);
static VKAPI_ATTR VkResult VKAPI_CALL test_register(VkDevice device, const VkDeviceEventInfoEXT *pDeviceEventInfo, const VkAllocationCallbacks *pAllocator, VkFence *pFence);
static void test_display_set(unsigned index, uintptr_t handle, const char *name, uint32_t width, uint32_t height);

/* Stands in for the clock. */
uint64_t
kwl_milliseconds(void)
{
	/* Succeeded: the test's time. */
	return test_now;
}

/* Stands in for the display list. */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceDisplayPropertiesKHR(
	VkPhysicalDevice physicalDevice,
	uint32_t *pPropertyCount,
	VkDisplayPropertiesKHR *pProperties)
{
	unsigned index;

	/* The stand-in has one GPU. */
	(void)physicalDevice;

	/* An unreadable list, as when the topology moves under the walk. */
	if (test_list_error)
		return VK_ERROR_UNKNOWN;

	/* Each connected display. */
	for (index = 0U; index < test_display_count && index < *pPropertyCount; index++) {
		memset(&pProperties[index], 0, sizeof(pProperties[index]));
		pProperties[index].display = test_displays[index].handle;
		pProperties[index].displayName = test_displays[index].name;
		pProperties[index].physicalResolution.width = test_displays[index].width;
		pProperties[index].physicalResolution.height = test_displays[index].height;
	}

	/* Succeeded: the list. */
	*pPropertyCount = index;
	return VK_SUCCESS;
}

/* Stands in for the hotplug fence's status. */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetFenceStatus(
	VkDevice device,
	VkFence fence)
{
	/* The stand-in has one device and one fence at a time. */
	(void)device;
	(void)fence;

	/* Succeeded: signaled or not as the test says. */
	if (test_hotplug_signaled)
		return VK_SUCCESS;
	return VK_NOT_READY;
}

/* Stands in for a fence's end. */
VKAPI_ATTR void VKAPI_CALL
vkDestroyFence(
	VkDevice device,
	VkFence fence,
	const VkAllocationCallbacks *pAllocator)
{
	/* The stand-in counts them. */
	(void)device;
	(void)pAllocator;
	check(fence != VK_NULL_HANDLE, "only a registered fence is destroyed");
	check(test_registered > test_destroyed + 1U, "the new fence is registered before the old one goes");
	test_destroyed++;
}

/* Stands in for the hotplug registration. */
static VKAPI_ATTR VkResult VKAPI_CALL
test_register(
	VkDevice device,
	const VkDeviceEventInfoEXT *pDeviceEventInfo,
	const VkAllocationCallbacks *pAllocator,
	VkFence *pFence)
{
	/* The stand-in hands out numbered fences. */
	(void)device;
	(void)pAllocator;
	check(pDeviceEventInfo->deviceEvent == VK_DEVICE_EVENT_TYPE_DISPLAY_HOTPLUG_EXT, "the hotplug event is registered");
	test_registered++;
	test_hotplug_signaled = 0;
	*pFence = (VkFence)(uintptr_t)(0x1000U + test_registered);
	return VK_SUCCESS;
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

/* Stands in for opening the output on the chosen display. */
int
kwl_compose_output_open(
	struct kwl_server *server)
{
	unsigned index;

	/* Opened on a connected display that takes a swapchain. */
	test_opens++;
	for (index = 0U; index < test_display_count; index++) {
		if (test_displays[index].handle == server->compose->display && !test_displays[index].refuses) {
			server->compose->output_open = 1U;
			return 0;
		}
	}

	/* Refused, or gone. */
	return EIO;
}

/* Stands in for closing the output. */
void
kwl_compose_output_close(
	struct kwl_server *server)
{
	/* Closed. */
	test_closes++;
	server->compose->output_open = 0U;
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

/* Stands in for bringing the heads in line (heads.c). */
void
kwl_heads_sync(
	struct kwl_server *server)
{
	/* Counted; the heads are in line. */
	server->compose->heads_stale = 0U;
	test_syncs++;
}

/* Stands in for the anchor's move off a display turned off (heads.c, ws113-p014): no display is off here. */
void
kwl_displays_anchor_follow(
	struct kwl_server *server)
{
	/* Nothing to follow. */
	(void)server;
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

/* Stands in for the held displays result's wait (displays-shell.c, BUG-266): nothing is held here. */
void
kwl_displays_pending_tick(
	struct kwl_server *server,
	uint64_t now)
{
	/* Nothing to answer. */
	(void)server;
	(void)now;
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
	int available;
	int error;

	/* The machine's panel and an HDMI display, the compositor on the panel. */
	memset(&server, 0, sizeof(server));
	memset(&compose, 0, sizeof(compose));
	server.compose = &compose;
	server.windowed = 1U;
	server.glass = 1U;
	server.width = 1920U;
	server.height = 1200U;
	server.pointer_x = 1900;
	server.pointer_y = 1100;
	compose.register_device_event = test_register;
	test_display_set(0U, 0x11U, "zedbsd-port-v1:pci:0000:00:02.0:edp:A", 1920U, 1200U);
	test_display_set(1U, 0x22U, "zedbsd-port-v1:pci:0000:00:02.0:hdmi:B", 1280U, 720U);
	test_display_count = 2U;
	compose.display = (VkDisplayKHR)(uintptr_t)0x11U;
	compose.boot_display = compose.display;
	compose.output_open = 1U;

	/* The first look registers a fence and lists the displays. */
	test_now = 1000U;
	kwl_output_tick(&server);
	check(test_registered == 1U && compose.display_count == 2U, "the first look registers and lists");
	check(test_syncs == 1U, "the first look brings the heads in line");
	available = kwl_output_external_available(&server);
	check(available, "the HDMI display is external and available");

	/* The move to the external display: closed, opened at its size, resized, told. */
	error = kwl_output_use_external(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x22U, "the output moves to HDMI");
	check(compose.kept_off == (VkDisplayKHR)(uintptr_t)0x11U, "the panel left under the lid is kept off (no head)");
	check(server.width == 1280U && server.height == 720U && test_resizes == 1U && test_told == 1U, "the desktop fits 1280x720 and the clients are told");
	check(server.pointer_x == 1279 && server.pointer_y == 719, "the pointer stays inside");
	available = kwl_output_external_available(&server);
	check(!available, "the external display shown is not offered again");

	/* Back to the machine's own display. */
	error = kwl_output_use_internal(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x11U && server.width == 1920U, "the output comes back to the panel");
	check(compose.kept_off == VK_NULL_HANDLE, "nothing is kept off once the output is back on the panel");

	/* A refused swapchain: limited, and the output back on the panel. */
	test_displays[1].refuses = 1;
	error = kwl_output_use_external(&server);
	check(error == EAGAIN && compose.display == (VkDisplayKHR)(uintptr_t)0x11U && server.width == 1920U, "a refused move goes back");
	check((compose.limited & 2U) != 0U, "the refused display is limited");
	check(compose.kept_off == VK_NULL_HANDLE, "a refused move keeps nothing off");
	available = kwl_output_external_available(&server);
	check(!available, "a limited display is not offered");

	/* A hotplug: the new fence before the old one goes, the limit forgotten. */
	test_displays[1].refuses = 0;
	test_now += 300U;
	test_hotplug_signaled = 1;
	kwl_output_tick(&server);
	check(test_registered == 2U && test_destroyed == 1U && compose.limited == 0U, "a hotplug registers again and forgets the limit");

	/* Not looked at too often. */
	test_hotplug_signaled = 1;
	test_now += 10U;
	kwl_output_tick(&server);
	check(test_registered == 2U, "the fence is not looked at again within 250 ms");

	/* On HDMI, which is unplugged: the output moves back to the panel by itself. */
	test_now += 300U;
	test_hotplug_signaled = 0;
	kwl_output_tick(&server);
	error = kwl_output_use_external(&server);
	check(error == 0, "on HDMI again");
	test_display_count = 1U;
	compose.output_lost = 1U;
	test_now += 1U;
	kwl_output_tick(&server);
	check(compose.display == (VkDisplayKHR)(uintptr_t)0x11U && compose.output_lost == 0U && server.width == 1920U, "a lost display moves the output to the panel");

	/* Without any display: the output waits for the next hotplug, then opens on the one that comes. */
	test_display_count = 0U;
	compose.output_lost = 1U;
	test_now += 300U;
	kwl_output_tick(&server);
	check(compose.output_lost == 2U, "no display: the output waits");
	test_display_set(0U, 0x33U, "zedbsd-port-v1:pci:0000:00:02.0:hdmi:B", 1024U, 768U);
	test_display_count = 1U;
	test_now += 300U;
	test_hotplug_signaled = 1;
	kwl_output_tick(&server);
	check(compose.output_lost == 0U && compose.display == (VkDisplayKHR)(uintptr_t)0x33U && server.width == 1024U, "a display that comes takes the waiting output");

	/* A virtual adapter: no name says built in; the display started on is the machine's own. */
	test_display_set(0U, 0x44U, "Venus virtual display 0", 1280U, 800U);
	test_display_set(1U, 0x55U, "Venus virtual display 1", 1024U, 768U);
	test_display_count = 2U;
	compose.display = (VkDisplayKHR)(uintptr_t)0x44U;
	compose.boot_display = compose.display;
	compose.output_open = 1U;
	compose.output_lost = 0U;
	test_now += 300U;
	test_hotplug_signaled = 1;
	kwl_output_tick(&server);
	error = kwl_output_use_external(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x55U, "the other virtual display is external");
	error = kwl_output_use_internal(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x44U, "the virtual display started on is the machine's own");

	/* A move to the display shown changes nothing. */
	test_opens = 0U;
	error = kwl_output_use_internal(&server);
	check(error == 0 && test_opens == 0U, "the display shown is not opened again");

	/* On the external display, out of date (a hotplug elsewhere): opened again there, not moved. */
	error = kwl_output_use_external(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x55U, "on the external virtual display");
	test_opens = 0U;
	compose.output_lost = 1U;
	test_now += 1U;
	kwl_output_tick(&server);
	check(compose.display == (VkDisplayKHR)(uintptr_t)0x55U && compose.output_lost == 0U && test_opens == 1U, "an out-of-date display still listed is opened again");
	check(server.width == 1024U, "the size stays the external display's");

	/* A hotplug whose list cannot be read: nothing is lost, the list read before stays. */
	test_list_error = 1;
	test_now += 300U;
	test_hotplug_signaled = 1;
	kwl_output_tick(&server);
	check(compose.output_lost == 0U && compose.display_count == 2U, "an unreadable list changes nothing");
	test_list_error = 0;

	/* The external display unplugged while no frame is drawn: the hotplug alone moves the output. */
	test_display_count = 1U;
	test_now += 300U;
	test_hotplug_signaled = 1;
	kwl_output_tick(&server);
	check(compose.display == (VkDisplayKHR)(uintptr_t)0x44U && compose.output_lost == 0U && server.width == 1280U, "a display gone from the list loses the output and it moves");

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
	test_now += 300U;
	test_hotplug_signaled = 1;
	kwl_output_tick(&server);
	error = kwl_output_use_external(&server);
	check(error == 0 && compose.display == (VkDisplayKHR)(uintptr_t)0x66U, "the output moves to the DisplayPort display");
	check(compose.switch_proving == 1U && compose.switch_from == (VkDisplayKHR)(uintptr_t)0x11U, "the move waits for its first frame, the panel kept to go back to");
	compose.switch_proving = 0U;
	compose.switch_failed = 1U;
	compose.output_lost = 1U;
	test_settled = 0U;
	test_now += 1U;
	kwl_output_tick(&server);
	check(compose.display == (VkDisplayKHR)(uintptr_t)0x11U && compose.output_lost == 0U && server.height == 1080U, "a refused first frame takes the output back to the panel");
	check(compose.switch_failed == 0U && compose.switch_proving == 0U, "the way back is not a move to prove");
	check(test_settled == 1U && test_settled_failed, "the held result says the move failed");
	check(strcmp(test_back, "zedbsd-port-v1:pci:0000:00:02.0:edp:A") == 0, "the choice takes in the panel gone back to");
	check((compose.move_failed & 2U) != 0U && (compose.limited & 2U) == 0U, "the failed display is marked, not limited (it may still be a head)");
	available = kwl_output_external_available(&server);
	check(!available, "a display whose move failed is not offered");
	error = kwl_output_use_external(&server);
	check(error == ENOENT && compose.display == (VkDisplayKHR)(uintptr_t)0x11U, "the output does not move to it again");
	test_now += 300U;
	test_hotplug_signaled = 1;
	kwl_output_tick(&server);
	check(compose.move_failed == 0U, "a hotplug forgets the failed move");

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
