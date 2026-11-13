/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's screen capture for the Agent Acceptance Test
 * (ws173-p002).  Test images only: this file is built into the compositor
 * only when the image's configuration sets ZEDBSD_TEST_SCREEN_CAPTURE=y;
 * otherwise shot-none.c, whose functions do nothing, takes its place, so a
 * release image has no capture at all.
 *
 * The compositor listens on $XDG_RUNTIME_DIR/keiland-shot.sock, or on
 * /tmp/keiland-shot.<uid>.sock when XDG_RUNTIME_DIR is not set (the login
 * screen), mode 0600 and owned by the compositor's user: only that user
 * and root can connect.  A connection sends one line:
 *
 *   PING   answered "ACTIVE" when this compositor shows the display now,
 *          "INACTIVE" otherwise
 *   SHOT   the next frame is drawn whole and copied from its swapchain
 *          image (Vulkan readback, on i915 and on Venus alike); answered
 *          "OK width height format rotation=R scanout=S turn=T" (a VkFormat
 *          number, and the output's angles, WS217: the image is the
 *          swapchain's, physical) and then
 *          width x height x 4 bytes of pixels, rows top to bottom, or
 *          "ERROR why"
 *   DISPLAYS   the displays shown (ws113-p004b): "OK" and lines "mode=M",
 *          "output name=N width=W height=H x=X y=Y rotation=R scanout=S
 *          turn=T", "head ..." for each
 *          display shown besides the output
 *   MODE extended|mirror   the mode applied, answered "OK saved=E" or
 *          "ERROR errno=E"
 *   PLACE KEY X Y   a display's place in the extended mode applied, the
 *          same answers
 *   SHOT partial   as SHOT, but the frame is not asked to be drawn whole
 *          (the next frame as its damage makes it, WS217)
 *   POLICY any|default   the angles the displays may take (WS217: every
 *          one the build can turn, a test's), answered "OK mask=M why=W"
 *   ROTATE N   the anchor's angle chosen as Settings chooses it (WS217),
 *          answered "OK applied=A saved=S rotation=R width=W height=H" or
 *          "ERROR errno=E"
 *
 * keiland-shot (userland/tests/keiland-shot) turns the pixels into a PNG.
 * One request is served at a time; the wait for the frame is at most
 * SHOT_WAIT_MS.  The pixels are copied out of the buffer on the event
 * loop's thread and written by a thread of their own (shot-send) with a
 * send timeout: a reader slower than the display must not hold the event
 * loop, whose clients wait for their frame callbacks meanwhile.
 */

#include "kwl.h"
#include "compose.h"
#include "threads.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* How long a request waits for its frame, and the longest write of the pixels. */
#define SHOT_WAIT_MS		3000U
#define SHOT_SEND_SECONDS	10

/* The stages of a request. */
#define SHOT_IDLE		0
#define SHOT_REQUESTED		1
#define SHOT_RECORDED		2
#define SHOT_SENDING		3

/*
 * The capture's state, one for the compositor's life: the listening
 * socket and its path, the connection waiting for its frame, the request's
 * stage and time, and the host-visible buffer the frame is copied into.
 */
struct shot_state {
	int listener;
	char path[108];
	int client;
	int stage;
	uint64_t asked_ms;
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize size;
	uint32_t width;
	uint32_t height;
};

/* The state; listener -1 until the socket is open. */
static struct shot_state shot = { -1, { 0 }, -1, SHOT_IDLE, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0, 0 };

/*
 * One sending of a frame's pixels: the connection (the sender's from the
 * start until the event loop takes the sending back), the header and the
 * pixels copied out of the buffer, and what came of it.  lock guards done
 * and sent, which the sender writes last; the event loop joins the sender
 * once done is set (SHOT_SENDING until then).  wake is the event loop's
 * wake pipe, which the sender writes once it is done.
 */
struct shot_send {
	struct kwl_thread thread;
	pthread_mutex_t lock;
	int connection;
	char header[96];
	uint8_t *bytes;
	size_t size;
	uint32_t width;
	uint32_t height;
	size_t sent;
	unsigned done;
	int wake;
};

/* The sending under way, while the stage is SHOT_SENDING. */
static struct shot_send shot_sending;

/*
 * The timer of a request whose frame does not come: armed for
 * SHOT_WAIT_MS after the request while a connection waits for its frame,
 * disarmed when the request ends or its sending starts.  Readied by the
 * first request.
 */
static struct kwl_timer shot_wait_timer;

static void shot_answer(int connection, const char *line);
static int shot_displays_request(const char *line);
static void shot_displays(struct kwl_server *server, int connection, const char *line);
static void shot_policy(struct kwl_server *server, int connection, const char *word);
static void shot_rotate(struct kwl_server *server, int connection, const char *number);
static void shot_pointer(struct kwl_server *server, int connection);
static void shot_finish(struct kwl_server *server, const char *error);
static void shot_wait_fired(void *context, struct kwl_timer *timer);
static int shot_buffer(struct kwl_compose *compose);
static void shot_buffer_free(struct kwl_compose *compose);
static int shot_send_start(int connection, const char *header, const void *pixels, size_t size, int wake);
static void *shot_send_run(void *argument);
static int shot_send_end(struct kwl_server *server, unsigned wait);

/*
 * Takes a test image's own option of the command line (WS217):
 * --rotate-policy=any|default, the angles the displays may take (every one
 * the build can turn, a test's, or those the hardware allows).  Returns 1
 * for an option taken, 0 for another argument, or -1 for a word it does
 * not know.  A product's compositor (shot-none.c) takes none.
 */
int
kwl_shot_option(
	struct kwl_server *server,
	const char *argument)
{
	int match;

	/* Only the policy's option. */
	match = strncmp(argument, "--rotate-policy=", 16U);
	if (match != 0)
		return 0;

	/* Every angle the build can turn. */
	match = strcmp(argument + 16, "any");
	if (match == 0) {
		server->rotate_policy = KWL_ROTATE_POLICY_ANY;
		return 1;
	}

	/* The hardware's angles alone, as without the option. */
	match = strcmp(argument + 16, "default");
	if (match == 0) {
		server->rotate_policy = KWL_ROTATE_POLICY_DEFAULT;
		return 1;
	}

	/* Another word is a mistake. */
	return -1;
}

/* Tells whether this compositor was built with the capture (it was). */
int
kwl_shot_enabled(
	void)
{
	return 1;
}

/* Tells whether a capture waits for the next composed frame (the game mode composes one for it, scanout.c). */
int
kwl_shot_waiting(
	void)
{
	/* A request not yet copied. */
	if (shot.stage == SHOT_REQUESTED)
		return 1;

	/* None. */
	return 0;
}

/* Opens the capture's socket. */
void
kwl_shot_open(
	struct kwl_server *server)
{
	struct sockaddr_un address;
	const char *runtime;
	int status;

	/* The path: the runtime directory, else /tmp with the user's ID. */
	(void)server;
	runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime != NULL && runtime[0] == '/')
		snprintf(shot.path, sizeof(shot.path), "%s/keiland-shot.sock", runtime);
	else
		snprintf(shot.path, sizeof(shot.path), "/tmp/keiland-shot.%u.sock", (unsigned)getuid());

	/* A listening socket that does not wait, the user's alone. */
	shot.listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (shot.listener < 0)
		return;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", shot.path);
	(void)unlink(shot.path);
	status = bind(shot.listener, (struct sockaddr *)&address, sizeof(address));
	if (status == 0)
		status = listen(shot.listener, 2);
	if (status != 0) {
		(void)close(shot.listener);
		shot.listener = -1;
		return;
	}

	/* The user's alone (root may connect too). */
	(void)chmod(shot.path, 0600);
	printf("KWL SHOT listening path=%s\n", shot.path);
}

/* Closes the capture's socket and gives back its buffer. */
void
kwl_shot_close(
	struct kwl_server *server)
{
	/* A sending under way ends first (the end of the session may wait for it). */
	(void)shot_send_end(server, 1U);

	/* A request still waiting is answered. */
	if (shot.client >= 0)
		shot_finish(server, "closing");

	/* The socket and its name. */
	if (shot.listener >= 0) {
		(void)close(shot.listener);
		shot.listener = -1;
		(void)unlink(shot.path);
	}
}

/*
 * Serves the socket once a pass: a new connection's line, and the time
 * limit of a request waiting for its frame.
 */
void
kwl_shot_tick(
	struct kwl_server *server)
{
	struct timeval timeout;
	char line[128];
	ssize_t count;
	int connection;
	int active;
	int match;
	int displays;
	int sending;
	int error;

	/* A sending of pixels under way holds the next request back until it has ended (its end wakes the loop). */
	sending = shot_send_end(server, 0U);
	if (sending)
		return;

	/* A new connection, one at a time (one that comes wakes the loop). */
	if (shot.listener < 0 || shot.client >= 0)
		return;
	kwl_watch(server, shot.listener, POLLIN);
	connection = accept(shot.listener, NULL, NULL);
	if (connection < 0)
		return;

	/* Its line, which comes at once (a short wait), and a send that does not hang the compositor for long. */
	timeout.tv_sec = 0;
	timeout.tv_usec = 200000;
	(void)setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	timeout.tv_sec = SHOT_SEND_SECONDS;
	timeout.tv_usec = 0;
	(void)setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	memset(line, 0, sizeof(line));
	count = read(connection, line, sizeof(line) - 1U);
	if (count <= 0) {
		(void)close(connection);
		return;
	}

	/* PING: whether this compositor shows the display now. */
	active = server->compose != NULL && server->compose->output_open && server->os_paused == 0;
	match = strncmp(line, "PING", 4U);
	if (match == 0) {
		if (active)
			shot_answer(connection, "ACTIVE\n");
		else
			shot_answer(connection, "INACTIVE\n");
		(void)close(connection);
		return;
	}

	/* The displays' requests of ws113-p004b (DISPLAYS, MODE, PLACE). */
	displays = shot_displays_request(line);
	if (displays) {
		shot_displays(server, connection, line);
		(void)close(connection);
		return;
	}

	/* The angles' requests of WS217 (POLICY, ROTATE). */
	match = strncmp(line, "POLICY ", 7U);
	if (match == 0) {
		shot_policy(server, connection, line + 7);
		(void)close(connection);
		return;
	}

	/* ROTATE: the anchor's angle chosen. */
	match = strncmp(line, "ROTATE ", 7U);
	if (match == 0) {
		shot_rotate(server, connection, line + 7);
		(void)close(connection);
		return;
	}

	/* POINTER: where the pointer is, in the plane's logical pixels (WS217's absolute devices). */
	match = strncmp(line, "POINTER", 7U);
	if (match == 0) {
		shot_pointer(server, connection);
		(void)close(connection);
		return;
	}

	/* Anything but SHOT. */
	match = strncmp(line, "SHOT", 4U);
	if (match != 0) {
		shot_answer(connection, "ERROR unknown-request\n");
		(void)close(connection);
		return;
	}

	/* SHOT needs the display, and readable swapchain images. */
	if (!active) {
		shot_answer(connection, "ERROR inactive\n");
		(void)close(connection);
		return;
	}

	/* Swapchain images a copy can be made from. */
	if (!server->compose->readback) {
		shot_answer(connection, "ERROR unreadable\n");
		(void)close(connection);
		return;
	}

	/* The buffer the frame is copied into. */
	error = shot_buffer(server->compose);
	if (error != 0) {
		shot_answer(connection, "ERROR buffer\n");
		(void)close(connection);
		return;
	}

	/*
	 * The next frame copies its image; a frame is asked for, drawn whole,
	 * unless "SHOT partial" asks for the next frame as it comes (its damage
	 * alone drawn: a turned output's partial turn pass, WS217).
	 */
	shot.client = connection;
	shot.stage = SHOT_REQUESTED;
	shot.asked_ms = kwl_milliseconds();
	if (shot_wait_timer.fire == NULL)
		kwl_timer_init(&shot_wait_timer, "shot-wait", shot_wait_fired);
	kwl_timer_set(server, &shot_wait_timer, shot.asked_ms + SHOT_WAIT_MS + 1U);
	match = strncmp(line, "SHOT partial", 12U);
	if (match != 0)
		server->dirty = 1;
	printf("KWL SHOT requested width=%u height=%u\n", shot.width, shot.height);
}

/*
 * Records the copy of a frame's swapchain image into the buffer, after
 * the frame's render pass (which leaves the image for presentation), when a
 * request waits; the image goes back to the presentation layout.
 */
void
kwl_shot_record(
	struct kwl_server *server,
	struct kwl_list *command,
	VkImage image)
{
	VkImageMemoryBarrier barrier;
	VkBufferMemoryBarrier visible;
	VkBufferImageCopy copy;

	/* Only a request waiting for a frame. */
	(void)server;
	if (shot.stage != SHOT_REQUESTED)
		return;

	/* The image, from presentation to the copy's source. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	kwl_list_pipeline_barrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U,
	    0U, NULL, 0U, NULL, 1U, &barrier);

	/* The whole image into the buffer, rows packed. */
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1U;
	copy.imageExtent.width = shot.width;
	copy.imageExtent.height = shot.height;
	copy.imageExtent.depth = 1U;
	kwl_list_copy_image_to_buffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buffer, 1U, &copy);

	/* The copy visible to the host after the fence. */
	memset(&visible, 0, sizeof(visible));
	visible.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	visible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	visible.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	visible.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	visible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	visible.buffer = shot.buffer;
	visible.size = VK_WHOLE_SIZE;
	kwl_list_pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, NULL, 1U,
	    &visible, 0U, NULL);

	/* The image back to presentation. */
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = 0U;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	kwl_list_pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0U, 0U, NULL,
	    0U, NULL, 1U, &barrier);

	/* The frame carries the copy. */
	shot.stage = SHOT_RECORDED;
}

/*
 * Sends the copied frame once its fence signaled: the pixels are copied out
 * of the buffer here and written by the sender's thread, so that the event
 * loop goes on (without the thread they are written here, as before).
 */
void
kwl_shot_complete(
	struct kwl_server *server)
{
	char header[96];
	void *map;
	const uint8_t *bytes;
	size_t sent;
	ssize_t count;
	VkResult result;
	int error;

	/* Only a frame that carried the copy. */
	if (shot.stage != SHOT_RECORDED || shot.client < 0)
		return;

	/* The pixels. */
	result = vkMapMemory(server->compose->device, shot.memory, 0U, shot.size, 0U, &map);
	if (result != VK_SUCCESS) {
		shot_finish(server, "map");
		return;
	}

	/* The header. */
	snprintf(header, sizeof(header), "OK %u %u %d rotation=%u scanout=%u turn=%u\n", shot.width, shot.height, (int)server->compose->format,
	    server->compose->rotation, server->compose->scanout_rotation, server->compose->turn_rotation);

	/* The sender takes the connection and a copy of the pixels; the buffer is given back below. */
	error = shot_send_start(shot.client, header, map, (size_t)shot.size, server->wake[1]);
	if (error == 0) {
		vkUnmapMemory(server->compose->device, shot.memory);
		shot.client = -1;
		kwl_timer_disarm(&server->timers, &shot_wait_timer);
		shot.stage = SHOT_SENDING;
		if (!server->compose->in_flight)
			shot_buffer_free(server->compose);
		return;
	}

	/* Without the sender the bytes are written here. */
	printf("KWL SHOT sender errno=%d the pixels are written in the event loop\n", error);
	shot_answer(shot.client, header);
	bytes = map;
	sent = 0U;
	while (sent < (size_t)shot.size) {
		count = write(shot.client, bytes + sent, (size_t)shot.size - sent);
		if (count <= 0)
			break;
		sent += (size_t)count;
	}

	/* The memory is given back to the device. */
	vkUnmapMemory(server->compose->device, shot.memory);
	printf("KWL SHOT sent width=%u height=%u bytes=%llu\n", shot.width, shot.height, (unsigned long long)sent);

	/* The request is done. */
	shot_finish(server, NULL);
}

/* Writes a short answer. */
static void
shot_answer(
	int connection,
	const char *line)
{
	(void)write(connection, line, strlen(line));
}

/* Ends the request: an error's answer when there is one, the connection closed, the buffer given back. */
static void
shot_finish(
	struct kwl_server *server,
	const char *error)
{
	char line[64];

	/* The error. */
	if (error != NULL && shot.client >= 0) {
		snprintf(line, sizeof(line), "ERROR %s\n", error);
		shot_answer(shot.client, line);
		printf("KWL SHOT error=%s\n", error);
	}

	/* The connection and the buffer; no frame is waited for any more. */
	if (shot.client >= 0)
		(void)close(shot.client);
	shot.client = -1;
	kwl_timer_disarm(&server->timers, &shot_wait_timer);
	shot.stage = SHOT_IDLE;
	if (server->compose != NULL && !server->compose->in_flight)
		shot_buffer_free(server->compose);
}

/* Makes the host-visible buffer for one frame of the output (kept from an earlier request of the same size). */
static int
shot_buffer(
	struct kwl_compose *compose)
{
	VkPhysicalDeviceMemoryProperties memory;
	VkMemoryRequirements requirements;
	VkBufferCreateInfo create;
	VkMemoryAllocateInfo allocate;
	VkMemoryPropertyFlags wanted;
	uint32_t index;
	VkResult result;

	/* One kept from before, for the same output. */
	if (shot.buffer != VK_NULL_HANDLE && shot.width == compose->output.width && shot.height == compose->output.height)
		return 0;
	shot_buffer_free(compose);

	/* The buffer: four bytes a pixel. */
	shot.width = compose->output.width;
	shot.height = compose->output.height;
	shot.size = (VkDeviceSize)shot.width * shot.height * 4U;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create.size = shot.size;
	create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(compose->device, &create, NULL, &shot.buffer);
	if (result != VK_SUCCESS) {
		shot.buffer = VK_NULL_HANDLE;
		return EIO;
	}

	/* Host-visible, coherent memory for it. */
	vkGetBufferMemoryRequirements(compose->device, shot.buffer, &requirements);
	vkGetPhysicalDeviceMemoryProperties(compose->physical, &memory);
	wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (index = 0U; index < memory.memoryTypeCount; index++) {
		if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
		    (memory.memoryTypes[index].propertyFlags & wanted) == wanted)
			break;
	}

	/* Without such memory there is no capture. */
	if (index == memory.memoryTypeCount) {
		shot_buffer_free(compose);
		return EIO;
	}

	/* The memory, bound. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = index;
	result = vkAllocateMemory(compose->device, &allocate, NULL, &shot.memory);
	if (result == VK_SUCCESS)
		result = vkBindBufferMemory(compose->device, shot.buffer, shot.memory, 0U);
	if (result != VK_SUCCESS) {
		shot_buffer_free(compose);
		return EIO;
	}

	/* Succeeded: the buffer. */
	return 0;
}

/* Gives back the buffer (no frame in flight uses it). */
static void
shot_buffer_free(
	struct kwl_compose *compose)
{
	/* The buffer and its memory. */
	if (shot.buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(compose->device, shot.buffer, NULL);
	if (shot.memory != VK_NULL_HANDLE)
		vkFreeMemory(compose->device, shot.memory, NULL);
	shot.buffer = VK_NULL_HANDLE;
	shot.memory = VK_NULL_HANDLE;
	shot.width = 0U;
	shot.height = 0U;
}

/* Tells whether a request is one of the displays' (DISPLAYS, MODE, PLACE). */
static int
shot_displays_request(
	const char *line)
{
	int match;

	/* Each of the three words. */
	match = strncmp(line, "DISPLAYS", 8U);
	if (match == 0)
		return 1;
	match = strncmp(line, "MODE ", 5U);
	if (match == 0)
		return 1;
	match = strncmp(line, "PLACE ", 6U);
	if (match == 0)
		return 1;

	/* Another request. */
	return 0;
}

/*
 * Answers a displays' request (ws113-p004b): the description, or a mode
 * or a place applied through the choice the system extension applies.
 */
static void
shot_displays(
	struct kwl_server *server,
	int connection,
	const char *line)
{
	struct kwl_display_config wanted;
	char text[2048];
	char key[KWL_DISPLAYS_KEY];
	char answer[64];
	int32_t x;
	int32_t y;
	size_t length;
	int match;
	int saved;
	int error;

	/* Only window mode has displays. */
	if (server->compose == NULL) {
		shot_answer(connection, "ERROR inactive\n");
		return;
	}

	/* DISPLAYS: the description after "OK". */
	match = strncmp(line, "DISPLAYS", 8U);
	if (match == 0) {
		memcpy(text, "OK\n", 4U);
		length = kwl_displays_describe(server, text + 3, sizeof(text) - 3U);
		if (length == 0U) {
			shot_answer(connection, "ERROR too-long\n");
			return;
		}

		/* The description sent. */
		shot_answer(connection, text);
		return;
	}

	/* The choice now, changed as asked. */
	wanted = server->compose->config;
	wanted.mode = server->compose->display_mode;
	match = strncmp(line, "MODE ", 5U);
	if (match == 0) {
		/* The mode named. */
		match = strncmp(line + 5, "mirror", 6U);
		if (match == 0) {
			wanted.mode = KWL_DISPLAYS_MIRROR;
		} else {
			wanted.mode = KWL_DISPLAYS_EXTENDED;
		}
	} else {
		/* A place: the key and the two coordinates. */
		error = kwl_displays_parse_place(line + 6, key, sizeof(key), &x, &y);
		if (error != 0) {
			shot_answer(connection, "ERROR place\n");
			return;
		}

		/* The place in the choice. */
		error = kwl_displays_set(&wanted, key, x, y);
		if (error != 0) {
			(void)snprintf(answer, sizeof(answer), "ERROR errno=%d\n", error);
			shot_answer(connection, answer);
			return;
		}
	}

	/* Applied, or refused with nothing changed. */
	error = kwl_displays_apply(server, &wanted, &saved);
	if (error != 0) {
		(void)snprintf(answer, sizeof(answer), "ERROR errno=%d\n", error);
		shot_answer(connection, answer);
		return;
	}

	/* Succeeded: applied, and whether it was written. */
	(void)snprintf(answer, sizeof(answer), "OK saved=%d\n", saved);
	shot_answer(connection, answer);
}

/*
 * Answers POLICY any|default (WS217): which angles the displays may take,
 * every one the build can turn (a test's) or those the hardware allows;
 * answered "OK mask=M why=W" (the anchor's).
 */
static void
shot_policy(
	struct kwl_server *server,
	int connection,
	const char *word)
{
	char answer[64];
	unsigned policy;
	int match;
	int differs;

	/* Only window mode has displays. */
	if (server->compose == NULL) {
		shot_answer(connection, "ERROR inactive\n");
		return;
	}

	/* The policy named: any, or default. */
	policy = KWL_ROTATE_POLICY_DEFAULT;
	match = strncmp(word, "any", 3U);
	if (match == 0)
		policy = KWL_ROTATE_POLICY_ANY;

	/* Another word is a mistake. */
	differs = strncmp(word, "default", 7U);
	if (match != 0 && differs != 0) {
		shot_answer(connection, "ERROR policy\n");
		return;
	}

	/* Applied, and the anchor's angles under it. */
	kwl_output_policy_set(server, policy);
	(void)snprintf(answer, sizeof(answer), "OK mask=%u why=%u\n", server->compose->rotations, server->compose->rotate_why);
	shot_answer(connection, answer);
}

/*
 * Answers ROTATE N (WS217): the user's choice of the anchor's angle, as
 * Settings will make it; answered "OK applied=A saved=S rotation=R
 * width=W height=H" or "ERROR errno=E".
 */
static void
shot_rotate(
	struct kwl_server *server,
	int connection,
	const char *number)
{
	char answer[128];
	char *end;
	unsigned long angle;
	unsigned applied;
	int saved;
	int error;

	/* Only window mode has displays. */
	if (server->compose == NULL) {
		shot_answer(connection, "ERROR inactive\n");
		return;
	}

	/* The angle, a number. */
	angle = strtoul(number, &end, 10);
	if (end == number || angle > 360UL) {
		shot_answer(connection, "ERROR angle\n");
		return;
	}

	/* Chosen, and shown when the output is open. */
	error = kwl_output_select_rotation(server, KWL_PLANE_ANCHOR, (unsigned)angle, &applied, &saved);
	if (error != 0) {
		(void)snprintf(answer, sizeof(answer), "ERROR errno=%d\n", error);
		shot_answer(connection, answer);
		return;
	}

	/* Succeeded: what was done, and the desktop's size now. */
	(void)snprintf(answer, sizeof(answer), "OK applied=%u saved=%d rotation=%u width=%u height=%u\n", applied, saved,
	    server->compose->rotation, server->width, server->height);
	shot_answer(connection, answer);
}

/*
 * Answers where the pointer is (WS217): the place an absolute device put it
 * at, as the cursor's box has it (the input thread's latest), in the
 * plane's logical pixels, and its output.
 */
static void
shot_pointer(
	struct kwl_server *server,
	int connection)
{
	char answer[96];
	int32_t x;
	int32_t y;
	unsigned output;

	/* The box's place, or the main thread's before the threads run. */
	x = server->pointer_x;
	y = server->pointer_y;
	output = server->pointer_output;
	if (server->threads_prepared)
		kwl_cursor_box_read(&server->cursor_box, &x, &y, &output);

	/* Succeeded: the place. */
	(void)snprintf(answer, sizeof(answer), "OK x=%d y=%d output=%u\n", x, y, output);
	shot_answer(connection, answer);
}

/*
 * Starts the sender of one frame: a copy of the pixels, the header and the
 * connection go to its thread, which writes wake (the event loop's wake
 * pipe) once it is done.  Returns 0, or ENOMEM or the thread's errno value
 * (the caller then still owns the connection).
 */
static int
shot_send_start(
	int connection,
	const char *header,
	const void *pixels,
	size_t size,
	int wake)
{
	int error;

	/* The pixels, copied: the buffer goes back to the device at once. */
	memset(&shot_sending, 0, sizeof(shot_sending));
	shot_sending.bytes = malloc(size);
	if (shot_sending.bytes == NULL)
		return ENOMEM;
	memcpy(shot_sending.bytes, pixels, size);
	shot_sending.size = size;
	shot_sending.width = shot.width;
	shot_sending.height = shot.height;
	shot_sending.connection = connection;
	shot_sending.wake = wake;
	(void)snprintf(shot_sending.header, sizeof(shot_sending.header), "%s", header);

	/* The lock of what the sender reports. */
	error = pthread_mutex_init(&shot_sending.lock, NULL);
	if (error != 0) {
		free(shot_sending.bytes);
		shot_sending.bytes = NULL;
		return error;
	}

	/* The thread, which writes and then says it is done. */
	error = kwl_thread_start(&shot_sending.thread, "shot-send", shot_send_run, &shot_sending);
	if (error != 0) {
		pthread_mutex_destroy(&shot_sending.lock);
		free(shot_sending.bytes);
		shot_sending.bytes = NULL;
		return error;
	}

	/* Succeeded: the sender owns the connection until it is done. */
	return 0;
}

/* Writes the header and the pixels on the sender's thread. */
static void *
shot_send_run(
	void *argument)
{
	struct shot_send *send;
	size_t sent;
	ssize_t count;

	/* The sending's record. */
	send = argument;

	/* The header, then every byte; a write that fails or times out ends it. */
	shot_answer(send->connection, send->header);
	sent = 0U;
	while (sent < send->size) {
		count = write(send->connection, send->bytes + sent, send->size - sent);
		if (count <= 0)
			break;
		sent += (size_t)count;
	}

	/* done tells the event loop that the thread is to be joined. */
	pthread_mutex_lock(&send->lock);

	send->sent = sent;
	send->done = 1U;

	pthread_mutex_unlock(&send->lock);

	/* The event loop's poll returns and its pass joins the sender. */
	kwl_thread_wake(send->wake);

	/* The thread ends. */
	return NULL;
}

/*
 * Ends a sending that is done (or, with wait, waits for it): the sender is
 * joined, the connection closed and the copy freed.  Returns 1 while a
 * sending is still under way, 0 when none is.
 */
static int
shot_send_end(
	struct kwl_server *server,
	unsigned wait)
{
	unsigned done;

	/* No sending. */
	if (shot.stage != SHOT_SENDING)
		return 0;

	/* Whether the sender has written everything it could. */
	pthread_mutex_lock(&shot_sending.lock);

	done = shot_sending.done;

	pthread_mutex_unlock(&shot_sending.lock);

	/* Still writing: the event loop does not wait, the end of the session does. */
	if (!done && !wait)
		return 1;

	/* The sender, its connection and its copy. */
	kwl_thread_join(&shot_sending.thread);
	printf("KWL SHOT sent width=%u height=%u bytes=%llu\n", shot_sending.width, shot_sending.height, (unsigned long long)shot_sending.sent);
	(void)close(shot_sending.connection);
	free(shot_sending.bytes);
	shot_sending.bytes = NULL;
	pthread_mutex_destroy(&shot_sending.lock);

	/* The request is done; the buffer goes back once no frame uses it. */
	shot.stage = SHOT_IDLE;
	if (server->compose != NULL && !server->compose->in_flight)
		shot_buffer_free(server->compose);

	/* Succeeded: no sending is under way. */
	return 0;
}

/*
 * A request's frame did not come within SHOT_WAIT_MS: the request ends
 * with the timeout's error.
 */
static void
shot_wait_fired(
	void *context,
	struct kwl_timer *timer)
{
	/* Only the capture's timer comes here. */
	(void)timer;

	/* Only a connection still waiting for its frame. */
	if (shot.client < 0)
		return;

	/* Succeeded: the request ends. */
	shot_finish(context, "timeout");
}
