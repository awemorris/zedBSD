/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The displays shown besides the output (ws113-p004b; the design is
 * plan/ws113/phase004/phase.md, "p004b").
 *
 * The output (compose.c) is the desktop's display, the anchor: the windows,
 * the bar and the pointer are on it, and its size is the desktop's.  Every
 * other connected display is a head, with a surface and a swapchain of its
 * own at its native mode.  In the mirror mode a head shows the desktop:
 * each frame, after the output's image is drawn, the same command buffer
 * copies it into the head's image, fitted whole and centred with black
 * bars (D01).  In the extended mode a head shows its own part of the
 * logical plane, which has no window yet (a window's move to another
 * display is ws113-p007): the wallpaper covering it, drawn once when the
 * head opens or the wallpaper changes.  Each head's swapchain is presented
 * after the output's.
 *
 * The heads follow the displays (output-switch.c calls kwl_heads_sync
 * after each enumeration): a display connected gets a head (extended: at
 * the place displays.conf keeps for it, else right of the rightmost
 * display, D-HOTPLUG), a display gone loses its head.  A head whose
 * swapchain is refused (the limit of the displays shown at once, D-LIMIT)
 * is marked limited and tried again after the next hotplug, as the output
 * is.  No head is opened while the output is closed (a hand-over), and
 * the heads close with the output.  A display kept off by a choice (the
 * built-in panel under a closed lid, ws052-p012) gets no head.
 *
 * The choice is applied through kwl_displays_apply (the system extension
 * of ws113-p005 calls it): the places are checked first, nothing changes
 * when they are refused, and the choice applied is written to
 * displays.conf (D-STORE), whose failure is reported apart from the
 * applied choice.  Without displays.conf every display is extended
 * (D-BOOT2).
 */

#include "kwl.h"
#include "compose.h"
#include "displays.h"
#include "extras.h"
#include "popup.h"
#include "subsurface.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* displays.conf beside desktop.conf, and the longest path and text of it. */
#define HEADS_FOLDER		".config/keiland"
#define HEADS_FILE		"displays.conf"
#define HEADS_PATH_MAX		512U
#define HEADS_TEXT_MAX		4096U

/* How far past a head's edges the pointer's cursor may still reach into it (pixels, ws113-p007). */
#define HEADS_CURSOR_REACH	64

/* The first wl_output global name of a head, past every name of the fixed globals (protocol.c). */
#define HEADS_GLOBAL_FIRST	1000U

static int heads_open(struct kwl_server *server, struct kwl_head *head, VkDisplayKHR display, const char *name);
static void heads_close(struct kwl_server *server, struct kwl_head *head);
static VkResult heads_targets(struct kwl_compose *compose, struct kwl_head *head);
static void heads_targets_destroy(struct kwl_compose *compose, struct kwl_head *head);
static struct kwl_head *heads_find(struct kwl_compose *compose, VkDisplayKHR display);
static struct kwl_head *heads_free(struct kwl_compose *compose);
static void heads_place(struct kwl_server *server, struct kwl_head *head);
static unsigned heads_rects(struct kwl_server *server, const struct kwl_head *skip, struct kwl_display_rect *rects, unsigned room);
static int heads_needs_frame(struct kwl_server *server, const struct kwl_head *head);
static unsigned heads_shows(struct kwl_server *server, const struct kwl_head *head);
static unsigned heads_windows(struct kwl_server *server, unsigned slot, struct kwl_object **windows, unsigned capacity);
static void heads_record_mirror(struct kwl_server *server, VkCommandBuffer command, VkImage source, struct kwl_head *head);
static void heads_record_extended(struct kwl_server *server, VkCommandBuffer command, struct kwl_head *head);
static void heads_barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source_access, VkAccessFlags destination_access, VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage);
static int heads_path(char *path, size_t size, int folder);
static int heads_save(const struct kwl_display_config *config);
static void heads_mkdir(char *folder);
static void heads_changed(struct kwl_server *server);
static void heads_evacuate(struct kwl_server *server, unsigned slot);
static void heads_windows_follow(struct kwl_server *server, const struct kwl_plane_rect *before, unsigned leave);
static void heads_window_place(struct kwl_server *server, struct kwl_object *window, const struct kwl_plane_rect *from, const struct kwl_plane_rect *to, unsigned slot, const char *why);
static int heads_is_window(const struct kwl_object *object);
static void heads_redraw(struct kwl_server *server);
static int heads_index(const struct kwl_compose *compose, VkDisplayKHR display);
static int heads_off(const struct kwl_compose *compose, unsigned index);

/*
 * Reads displays.conf once: the mode and the places the user chose.  A
 * missing, unreadable or other version's file leaves the choice of no file
 * (every display extended, D-BOOT2).
 */
void
kwl_heads_config_load(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	char path[HEADS_PATH_MAX];
	char text[HEADS_TEXT_MAX];
	ssize_t count;
	int descriptor;
	int error;

	/* Once for the compositor's life. */
	compose = server->compose;
	if (compose == NULL || compose->config_loaded)
		return;
	compose->config_loaded = 1U;
	kwl_displays_config_init(&compose->config);

	/* The file, when there is one. */
	error = heads_path(path, sizeof(path), 0);
	if (error != 0)
		return;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return;

	/* Its text, as much as this version writes. */
	count = read(descriptor, text, sizeof(text) - 1U);
	(void)close(descriptor);
	if (count <= 0)
		return;
	text[count] = '\0';

	/* The choice it keeps; a text this version does not read keeps the choice of no file. */
	error = kwl_displays_parse(text, (size_t)count, &compose->config);
	if (error != 0) {
		printf("KWL DISPLAYS config unread errno=%d\n", error);
		return;
	}

	/* Succeeded: the mode chosen. */
	compose->display_mode = compose->config.mode;
	printf("KWL DISPLAYS config mode=%u places=%u\n", compose->config.mode, compose->config.count);
}

/*
 * Brings the heads in line with the displays of the last enumeration:
 * a head for each display connected but the output's, the one kept off,
 * the ones the user turned off (ws113-p014) and the ones refused since the
 * last hotplug; no head for a display gone or lost.
 */
void
kwl_heads_sync(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	unsigned index;
	unsigned changed;
	int listed;
	int off;
	int error;

	/* Only window mode's open output has heads. */
	compose = server->compose;
	if (compose == NULL)
		return;

	/* The heads of displays gone, lost, or now the output's go. */
	changed = 0U;
	for (index = 0U; index < KWL_HEADS; index++) {
		head = &compose->heads[index];
		if (!head->open)
			continue;
		listed = heads_index(compose, head->display);
		off = 0;
		if (listed >= 0)
			off = heads_off(compose, (unsigned)listed);
		if (listed >= 0 &&
		    !head->lost &&
		    !off &&
		    head->display != compose->display &&
		    head->display != compose->kept_off) {
			/* Kept, and drawn again: a hotplug may have made its swapchain out of date, which its next acquire tells. */
			head->dirty = 1U;
			server->dirty = 1;
			continue;
		}

		/* Its display is gone, lost, turned off (ws113-p014), or the output's now. */
		heads_close(server, head);
		changed = 1U;
	}

	/* Without the output, no head opens (the output is closed for a hand-over, or lost). */
	compose->heads_stale = 0U;
	if (!compose->output_open || compose->output_lost) {
		if (changed)
			heads_changed(server);
		return;
	}

	/* Each display connected that has no head and may have one. */
	for (index = 0U; index < compose->display_count; index++) {
		/* Not the output's, not one kept off, not one refused since the last hotplug, not one shown already. */
		if (compose->displays[index] == compose->display)
			continue;
		if (compose->displays[index] == compose->kept_off)
			continue;
		if ((compose->limited & ((uint32_t)1U << index)) != 0U)
			continue;
		off = heads_off(compose, index);
		if (off)
			continue;
		head = heads_find(compose, compose->displays[index]);
		if (head != NULL)
			continue;

		/* A head free for it. */
		head = heads_free(compose);
		if (head == NULL)
			break;

		/* Its swapchain; a display that refuses it is tried again after the next hotplug. */
		error = heads_open(server, head, compose->displays[index], compose->display_names[index]);
		if (error != 0) {
			compose->limited |= (uint32_t)1U << index;
			printf("KWL OUTPUT head refused name=%s errno=%d\n", compose->display_names[index], error);
			continue;
		}

		/* A head more. */
		changed = 1U;
	}

	/* The clients learn of the change. */
	if (changed)
		heads_changed(server);
}

/* Closes the head of a display, before the output moves to it. */
void
kwl_heads_close_display(
	struct kwl_server *server,
	VkDisplayKHR display)
{
	struct kwl_head *head;

	/* The display's head, when it has one. */
	if (server->compose == NULL)
		return;
	head = heads_find(server->compose, display);
	if (head == NULL)
		return;

	/* Closed, and the clients told. */
	heads_close(server, head);
	heads_changed(server);
}

/* Closes every head: the output closes (a hand-over, or the compositor's exit). */
void
kwl_heads_close_all(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	unsigned index;
	unsigned changed;

	/* Each open head. */
	compose = server->compose;
	if (compose == NULL)
		return;
	changed = 0U;
	for (index = 0U; index < KWL_HEADS; index++) {
		if (!compose->heads[index].open)
			continue;
		heads_close(server, &compose->heads[index]);
		changed = 1U;
	}

	/* The output opened again brings them back at the next look. */
	compose->heads_stale = 1U;
	if (changed)
		heads_changed(server);
}

/* Tells whether a head lost its display, so that the next look closes it. */
int
kwl_heads_lost(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	unsigned index;

	/* Each open head. */
	compose = server->compose;
	if (compose == NULL)
		return 0;
	for (index = 0U; index < KWL_HEADS; index++) {
		if (compose->heads[index].open && compose->heads[index].lost)
			return 1;
	}

	/* None lost. */
	return 0;
}

/*
 * Acquires an image of each head the frame draws: every head in the
 * mirror mode, an extended head that is to be drawn again.  An image not
 * ready now leaves the head to the next frame; a display gone marks the
 * head lost.  Returns how many extended heads missed the frame (another
 * frame is then wanted, ws113-p007).
 */
unsigned
kwl_heads_acquire(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	unsigned missed;
	unsigned index;
	int needs;
	VkResult result;

	/* Each open head. */
	compose = server->compose;
	missed = 0U;
	for (index = 0U; index < KWL_HEADS; index++) {
		head = &compose->heads[index];
		head->in_frame = 0U;
		if (!head->open || head->lost)
			continue;

		/* Only a head the frame changes. */
		needs = heads_needs_frame(server, head);
		if (!needs)
			continue;

		/* Its next image, without waiting: the output's frame does not wait for another display. */
		result = vkAcquireNextImageKHR(compose->device, head->output.swapchain, 0U, head->acquired, VK_NULL_HANDLE, &head->image);
		if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
			head->in_frame = 1U;
			continue;
		}

		/* Not ready: the next frame draws it (one is asked for, ws113-p007). */
		if (result == VK_NOT_READY || result == VK_TIMEOUT) {
			head->dirty = 1U;
			if (compose->display_mode == KWL_DISPLAYS_EXTENDED)
				missed++;
			continue;
		}

		/* The display went (or another failure of its swapchain): the head is closed at the next look. */
		printf("KWL OUTPUT head lost operation=acquire name=%s result=%d\n", head->name, (int)result);
		head->lost = 1U;
	}

	/* Succeeded: how many missed it. */
	return missed;
}

/*
 * Records each head's picture after the output's image is drawn: the
 * output's image copied (mirror) from `source`, or the wallpaper drawn
 * (extended).
 */
void
kwl_heads_record(
	struct kwl_server *server,
	VkCommandBuffer command,
	VkImage source)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	unsigned index;

	/* Each head with an image in the frame. */
	compose = server->compose;
	for (index = 0U; index < KWL_HEADS; index++) {
		head = &compose->heads[index];
		if (!head->in_frame)
			continue;

		/* The mode's picture. */
		if (compose->display_mode == KWL_DISPLAYS_MIRROR) {
			heads_record_mirror(server, command, source, head);
		} else {
			heads_record_extended(server, command, head);
		}
	}
}

/*
 * Lists the semaphores the frame's submission also waits for (each head's
 * acquire) with their stages.  Returns how many, at most `room`.
 */
unsigned
kwl_heads_waits(
	struct kwl_server *server,
	VkSemaphore *semaphores,
	VkPipelineStageFlags *stages,
	unsigned room)
{
	struct kwl_compose *compose;
	unsigned index;
	unsigned count;

	/* Each head in the frame, while there is room. */
	compose = server->compose;
	count = 0U;
	for (index = 0U; index < KWL_HEADS && count < room; index++) {
		if (!compose->heads[index].in_frame)
			continue;
		semaphores[count] = compose->heads[index].acquired;
		stages[count] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
		count++;
	}

	/* Succeeded: the waits. */
	return count;
}

/*
 * Lists the semaphores the frame's submission also signals (each head's
 * image's present semaphore).  Returns how many, at most `room`.
 */
unsigned
kwl_heads_signals(
	struct kwl_server *server,
	VkSemaphore *semaphores,
	unsigned room)
{
	struct kwl_compose *compose;
	unsigned index;
	unsigned count;

	/* Each head in the frame, while there is room. */
	compose = server->compose;
	count = 0U;
	for (index = 0U; index < KWL_HEADS && count < room; index++) {
		if (!compose->heads[index].in_frame)
			continue;
		semaphores[count] = compose->heads[index].rendered[compose->heads[index].image];
		count++;
	}

	/* Succeeded: the signals. */
	return count;
}

/*
 * Presents each head's image after the output's.  A display gone marks
 * the head lost; an extended head drawn is not drawn again until it
 * changes.
 */
void
kwl_heads_present(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	VkPresentInfoKHR present;
	unsigned index;
	VkResult result;

	/* Each head in the frame. */
	compose = server->compose;
	for (index = 0U; index < KWL_HEADS; index++) {
		head = &compose->heads[index];
		if (!head->in_frame)
			continue;
		head->in_frame = 0U;

		/* Its image to its display after the commands. */
		memset(&present, 0, sizeof(present));
		present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		present.waitSemaphoreCount = 1U;
		present.pWaitSemaphores = &head->rendered[head->image];
		present.swapchainCount = 1U;
		present.pSwapchains = &head->output.swapchain;
		present.pImageIndices = &head->image;
		result = vkQueuePresentKHR(compose->queue, &present);

		/* Shown: an extended head waits for a change. */
		if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
			head->dirty = 0U;
			continue;
		}

		/* The display went: the head is closed at the next look. */
		printf("KWL OUTPUT head lost operation=present name=%s result=%d\n", head->name, (int)result);
		head->lost = 1U;
	}
}

/*
 * Gives back the heads' images acquired for a frame that was not
 * submitted (its recording failed): they are drawn at the next frame.
 * XXX: an acquired image stays the swapchain's until it is presented;
 * the next frame acquires another.
 */
void
kwl_heads_frame_skipped(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	unsigned index;

	/* Each head in the frame leaves it. */
	compose = server->compose;
	for (index = 0U; index < KWL_HEADS; index++)
		compose->heads[index].in_frame = 0U;
}

/*
 * Applies a choice of the displays (the system extension of ws113-p005):
 * the mode, and in the extended mode the places of the displays shown
 * (by their keys; a display the choice does not place keeps its place).
 * The places are checked before anything changes.  The choice applied is
 * written to displays.conf; *saved is 0, or the error of writing it.
 * Returns 0, EINVAL for a mode that is not one, or the check's refusal
 * (EINVAL two displays overlap, ENOTCONN one is not joined, ERANGE one is
 * out of range); nothing changes then.
 */
int
kwl_displays_apply(
	struct kwl_server *server,
	const struct kwl_display_config *wanted,
	int *saved)
{
	struct kwl_display_rect rects[KWL_DISPLAYS_PLACES];
	struct kwl_plane_rect before[KWL_PLANE_SLOTS];
	struct kwl_compose *compose;
	struct kwl_head *head;
	const char *mode;
	int32_t output_x;
	int32_t output_y;
	int32_t head_x[KWL_HEADS];
	int32_t head_y[KWL_HEADS];
	unsigned count;
	unsigned index;
	int place;
	int error;

	/* Only window mode applies a choice. */
	*saved = 0;
	compose = server->compose;
	if (compose == NULL)
		return ENODEV;

	/* A mode of the two. */
	if (wanted->mode != KWL_DISPLAYS_EXTENDED && wanted->mode != KWL_DISPLAYS_MIRROR)
		return EINVAL;

	/* The places asked for, the output's first; a display not placed keeps its place. */
	output_x = compose->output_x;
	output_y = compose->output_y;
	place = kwl_displays_find(wanted, compose->display_name);
	if (place >= 0) {
		output_x = wanted->places[place].x;
		output_y = wanted->places[place].y;
	}

	/* The output's rectangle, then each open head's. */
	rects[0].x = output_x;
	rects[0].y = output_y;
	rects[0].width = server->width;
	rects[0].height = server->height;
	count = 1U;
	for (index = 0U; index < KWL_HEADS; index++) {
		head = &compose->heads[index];
		head_x[index] = head->x;
		head_y[index] = head->y;
		if (!head->open)
			continue;
		place = kwl_displays_find(wanted, head->name);
		if (place >= 0) {
			head_x[index] = wanted->places[place].x;
			head_y[index] = wanted->places[place].y;
		}

		/* Its rectangle, while there is room. */
		if (count < KWL_DISPLAYS_PLACES) {
			rects[count].x = head_x[index];
			rects[count].y = head_y[index];
			rects[count].width = head->width;
			rects[count].height = head->height;
			count++;
		}
	}

	/* The extended mode's places must be a joined desk of displays. */
	if (wanted->mode == KWL_DISPLAYS_EXTENDED) {
		error = kwl_displays_validate(rects, count);
		if (error != 0) {
			printf("KWL DISPLAYS apply refused errno=%d\n", error);
			return error;
		}
	}

	/* The mode and the places, applied; the windows and the pointer on the heads go along (ws113-p007). */
	(void)kwl_outputs(server, before);
	compose->display_mode = wanted->mode;
	compose->output_x = output_x;
	compose->output_y = output_y;
	for (index = 0U; index < KWL_HEADS; index++) {
		compose->heads[index].x = head_x[index];
		compose->heads[index].y = head_y[index];
		compose->heads[index].dirty = 1U;
	}

	/* The windows and the pointer on the heads go with them, or to the anchor for the mirror. */
	heads_windows_follow(server, before, wanted->mode == KWL_DISPLAYS_MIRROR);

	/* The displays turned off follow the mode: shown in the mirror, no head in the extended mode (ws113-p014). */
	kwl_heads_sync(server);
	kwl_displays_anchor_follow(server);

	/* The choice kept: the mode, the anchor (the output's display) and every place shown now. */
	compose->config.mode = wanted->mode;
	(void)snprintf(compose->config.anchor, sizeof(compose->config.anchor), "%s", compose->display_name);
	if (compose->display_name[0] != '\0')
		(void)kwl_displays_set(&compose->config, compose->display_name, output_x, output_y);
	for (index = 0U; index < KWL_HEADS; index++) {
		head = &compose->heads[index];
		if (!head->open || head->name[0] == '\0')
			continue;
		(void)kwl_displays_set(&compose->config, head->name, head->x, head->y);
	}

	/* Written apart from the choice applied (D-STORE). */
	*saved = heads_save(&compose->config);
	if (*saved != 0)
		printf("KWL DISPLAYS save failed errno=%d\n", *saved);

	/* Everything drawn again, and the clients told. */
	heads_changed(server);
	mode = "extended";
	if (compose->display_mode == KWL_DISPLAYS_MIRROR)
		mode = "mirror";
	printf("KWL DISPLAYS applied mode=%s\n", mode);

	/* Succeeded: the choice is applied. */
	return 0;
}

/*
 * Describes the displays shown, a line each, for the tests' capture
 * channel (shot.c): "mode=M" first, then "output name=N width=W height=H
 * x=X y=Y" and one "head ..." line for each open head.  Returns the
 * text's length, or 0 when it does not fit `size`.
 */
size_t
kwl_displays_describe(
	struct kwl_server *server,
	char *text,
	size_t size)
{
	struct kwl_compose *compose;
	const struct kwl_head *head;
	const char *mode;
	size_t used;
	unsigned index;
	int written;

	/* The mode. */
	compose = server->compose;
	if (compose == NULL)
		return 0U;
	mode = "extended";
	if (compose->display_mode == KWL_DISPLAYS_MIRROR)
		mode = "mirror";
	written = snprintf(text, size, "mode=%s\noutput name=%s width=%u height=%u x=%ld y=%ld\n", mode, compose->display_name,
	    server->width, server->height, (long)compose->output_x, (long)compose->output_y);
	if (written < 0 || (size_t)written >= size)
		return 0U;
	used = (size_t)written;

	/* Each open head. */
	for (index = 0U; index < KWL_HEADS; index++) {
		head = &compose->heads[index];
		if (!head->open)
			continue;
		written = snprintf(text + used, size - used, "head name=%s width=%u height=%u x=%ld y=%ld\n", head->name, head->width,
		    head->height, (long)head->x, (long)head->y);
		if (written < 0 || (size_t)written >= size - used)
			return 0U;
		used += (size_t)written;
	}

	/* Succeeded: the description. */
	return used;
}

/*
 * Tells a client's wl_output what a display shows: head 0 is the output,
 * head n the n-th head.  Returns 0, or ENOENT for a head not open.
 */
int
kwl_output_view(
	struct kwl_server *server,
	uint32_t head,
	struct kwl_output_view *view)
{
	struct kwl_compose *compose;
	const struct kwl_head *shown;

	/* The output: the desktop's size at the output's place. */
	compose = server->compose;
	memset(view, 0, sizeof(*view));
	view->index = head;
	if (head == 0U) {
		view->width = server->width;
		view->height = server->height;
		view->refresh = server->refresh;
		if (compose != NULL && compose->display_mode == KWL_DISPLAYS_EXTENDED) {
			view->x = compose->output_x;
			view->y = compose->output_y;
		}

		/* The output's view. */
		return 0;
	}

	/* A head that is not open shows nothing. */
	if (compose == NULL || head > KWL_HEADS)
		return ENOENT;
	shown = &compose->heads[head - 1U];
	if (!shown->open)
		return ENOENT;

	/* Its own size; in the mirror mode the desktop at the origin. */
	view->width = shown->width;
	view->height = shown->height;
	view->refresh = shown->refresh;
	if (compose->display_mode == KWL_DISPLAYS_EXTENDED) {
		view->x = shown->x;
		view->y = shown->y;
	}

	/* Succeeded: the head's view. */
	return 0;
}

/* Finds the head of a wl_output global's name: its number (1 for the first head), or 0. */
uint32_t
kwl_output_head_of_global(
	struct kwl_server *server,
	uint32_t name)
{
	struct kwl_compose *compose;
	unsigned index;

	/* Each open head's global. */
	compose = server->compose;
	if (compose == NULL || name == 0U)
		return 0U;
	for (index = 0U; index < KWL_HEADS; index++) {
		if (compose->heads[index].open && compose->heads[index].global == name)
			return index + 1U;
	}

	/* No head has it. */
	return 0U;
}

/* Lists the wl_output global names of the open heads; returns how many, at most `room`. */
unsigned
kwl_output_head_globals(
	struct kwl_server *server,
	uint32_t *names,
	unsigned room)
{
	struct kwl_compose *compose;
	unsigned index;
	unsigned count;

	/* Each open head with a global. */
	compose = server->compose;
	count = 0U;
	if (compose == NULL)
		return 0U;
	for (index = 0U; index < KWL_HEADS && count < room; index++) {
		if (!compose->heads[index].open || compose->heads[index].global == 0U)
			continue;
		names[count] = compose->heads[index].global;
		count++;
	}

	/* Succeeded: the names. */
	return count;
}

/*
 * Opens a head on a display: its surface at the display's native mode, the
 * display acquired, its swapchain (a copy's destination too, for the
 * mirror), its targets, its place and its wl_output global.  Returns 0,
 * ENXIO for a display not connected now, ENOTSUP for one whose format is
 * not the output's, or EIO (the swapchain refused: the limit of the
 * displays shown at once); nothing is kept then.
 */
static int
heads_open(
	struct kwl_server *server,
	struct kwl_head *head,
	VkDisplayKHR display,
	const char *name)
{
	struct kwl_compose *compose;
	VkSemaphoreCreateInfo semaphore;
	char read_name[KWL_COMPOSE_NAME];
	VkResult result;

	/* The display's size, refresh and name now. */
	compose = server->compose;
	memset(head, 0, sizeof(*head));
	result = kwl_compose_display_read(server, display, &head->width, &head->height, &head->refresh, read_name, sizeof(read_name));
	if (result != VK_SUCCESS)
		return ENXIO;
	head->display = display;
	(void)snprintf(head->name, sizeof(head->name), "%s", name);

	/* Its plane's surface at its native mode, in the output's format (the pass and pipelines are the output's). */
	result = vkdemo_display_open_on(compose->instance, compose->physical, display, head->width, head->height, &head->output);
	if (result == VK_SUCCESS)
		result = vkdemo_display_choose_format(compose->physical, &head->output);
	if (result != VK_SUCCESS) {
		vkdemo_display_close(compose->instance, compose->device, &head->output);
		return EIO;
	}

	/* A format other than the output's cannot take the output's pass. */
	if (head->output.format != compose->format) {
		vkdemo_display_close(compose->instance, compose->device, &head->output);
		return ENOTSUP;
	}

	/* The display's permission for its swapchain. */
	result = kwl_os_display_acquire(server, compose->physical, display);
	if (result != VK_SUCCESS) {
		vkdemo_display_close(compose->instance, compose->device, &head->output);
		return EIO;
	}

	/* Its FIFO swapchain, drawn into and copied into. */
	result = vkdemo_display_create_swapchain_usage(compose->physical, compose->device, compose->family, &head->output, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (result != VK_SUCCESS ||
	    head->output.image_count > KWL_SWAPCHAIN_MAX ||
	    head->output.format != compose->format) {
		vkdemo_display_close(compose->instance, compose->device, &head->output);
		kwl_os_display_release(server, compose->physical, display);
		return EIO;
	}

	/* A view, a framebuffer and a present semaphore for each image, and the acquire's semaphore. */
	result = heads_targets(compose, head);
	if (result == VK_SUCCESS) {
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		result = vkCreateSemaphore(compose->device, &semaphore, NULL, &head->acquired);
	}

	/* Without them nothing of the head is kept. */
	if (result != VK_SUCCESS) {
		heads_targets_destroy(compose, head);
		vkdemo_display_close(compose->instance, compose->device, &head->output);
		kwl_os_display_release(server, compose->physical, display);
		return EIO;
	}

	/* Its place, a global for the clients, and a first picture at the next frame. */
	head->open = 1U;
	heads_place(server, head);
	if (compose->next_global < HEADS_GLOBAL_FIRST)
		compose->next_global = HEADS_GLOBAL_FIRST;
	head->global = compose->next_global;
	compose->next_global++;
	kwl_output_global_add(server, head->global);
	head->dirty = 1U;
	server->dirty = 1;

	/* Succeeded: the head shows the display. */
	printf("KWL OUTPUT head open name=%s width=%u height=%u refresh_mhz=%u x=%ld y=%ld images=%u\n", head->name, head->width, head->height,
	       head->refresh, (long)head->x, (long)head->y, head->output.image_count);
	return 0;
}

/* Closes a head once no frame uses its images: its global, its targets, its swapchain and surface, and the display given back. */
static void
heads_close(
	struct kwl_server *server,
	struct kwl_head *head)
{
	struct kwl_compose *compose;
	char name[KWL_COMPOSE_NAME];

	/* Its windows and the pointer go to the anchor while its place is still known (D-HOTPLUG, ws113-p007). */
	compose = server->compose;
	heads_evacuate(server, (unsigned)(head - compose->heads) + 1U);

	/* The clients lose its global first. */
	if (head->global != 0U)
		kwl_output_global_remove(server, head->global);

	/* No frame may still use its images. */
	(void)vkDeviceWaitIdle(compose->device);

	/* Its objects, then the display given back once its swapchain is gone. */
	heads_targets_destroy(compose, head);
	if (head->acquired != VK_NULL_HANDLE)
		vkDestroySemaphore(compose->device, head->acquired, NULL);
	vkdemo_display_close(compose->instance, compose->device, &head->output);
	kwl_os_display_release(server, compose->physical, head->display);

	/* Forgotten. */
	memcpy(name, head->name, sizeof(name));
	memset(head, 0, sizeof(*head));
	printf("KWL OUTPUT head closed name=%s\n", name);
}

/* Creates a view, a framebuffer and a present semaphore for each of a head's images. */
static VkResult
heads_targets(
	struct kwl_compose *compose,
	struct kwl_head *head)
{
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	VkSemaphoreCreateInfo semaphore;
	uint32_t index;
	VkResult result;

	/* Each image in turn. */
	for (index = 0U; index < head->output.image_count; index++) {
		/* Its view. */
		memset(&view, 0, sizeof(view));
		view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.image = head->output.images[index];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = compose->format;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.levelCount = 1U;
		view.subresourceRange.layerCount = 1U;
		result = vkCreateImageView(compose->device, &view, NULL, &head->views[index]);
		if (result != VK_SUCCESS)
			return result;

		/* Its framebuffer of the head's size, for the output's pass. */
		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer.renderPass = compose->pass;
		framebuffer.attachmentCount = 1U;
		framebuffer.pAttachments = &head->views[index];
		framebuffer.width = head->output.width;
		framebuffer.height = head->output.height;
		framebuffer.layers = 1U;
		result = vkCreateFramebuffer(compose->device, &framebuffer, NULL, &head->framebuffers[index]);
		if (result != VK_SUCCESS)
			return result;

		/* The semaphore its present waits for. */
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		result = vkCreateSemaphore(compose->device, &semaphore, NULL, &head->rendered[index]);
		if (result != VK_SUCCESS)
			return result;
	}

	/* Succeeded. */
	return VK_SUCCESS;
}

/* Destroys a head's views, framebuffers and present semaphores, those that were made. */
static void
heads_targets_destroy(
	struct kwl_compose *compose,
	struct kwl_head *head)
{
	uint32_t index;

	/* Each image's objects. */
	for (index = 0U; index < KWL_SWAPCHAIN_MAX; index++) {
		if (head->framebuffers[index] != VK_NULL_HANDLE)
			vkDestroyFramebuffer(compose->device, head->framebuffers[index], NULL);
		if (head->views[index] != VK_NULL_HANDLE)
			vkDestroyImageView(compose->device, head->views[index], NULL);
		if (head->rendered[index] != VK_NULL_HANDLE)
			vkDestroySemaphore(compose->device, head->rendered[index], NULL);
		head->framebuffers[index] = VK_NULL_HANDLE;
		head->views[index] = VK_NULL_HANDLE;
		head->rendered[index] = VK_NULL_HANDLE;
	}
}

/* Finds the open head of a display, or NULL. */
static struct kwl_head *
heads_find(
	struct kwl_compose *compose,
	VkDisplayKHR display)
{
	unsigned index;

	/* Handles compare only for equality. */
	for (index = 0U; index < KWL_HEADS; index++) {
		if (compose->heads[index].open && compose->heads[index].display == display)
			return &compose->heads[index];
	}

	/* No head shows it. */
	return NULL;
}

/* Finds a head that is not open, or NULL. */
static struct kwl_head *
heads_free(
	struct kwl_compose *compose)
{
	unsigned index;

	/* The first one closed. */
	for (index = 0U; index < KWL_HEADS; index++) {
		if (!compose->heads[index].open)
			return &compose->heads[index];
	}

	/* Every head is open. */
	return NULL;
}

/*
 * Places a head opened: at the place displays.conf keeps for its key when
 * that place overlaps none and is joined to the others, else right of the
 * rightmost display (D-HOTPLUG).
 */
static void
heads_place(
	struct kwl_server *server,
	struct kwl_head *head)
{
	struct kwl_display_rect rects[KWL_DISPLAYS_PLACES];
	struct kwl_compose *compose;
	unsigned count;
	int place;
	int error;

	/* The other displays' rectangles. */
	compose = server->compose;
	count = heads_rects(server, head, rects, KWL_DISPLAYS_PLACES - 1U);

	/* The place kept, when it fits with the others. */
	place = kwl_displays_find(&compose->config, head->name);
	if (place >= 0) {
		rects[count].x = compose->config.places[place].x;
		rects[count].y = compose->config.places[place].y;
		rects[count].width = head->width;
		rects[count].height = head->height;
		error = kwl_displays_validate(rects, count + 1U);
		if (error == 0) {
			head->x = rects[count].x;
			head->y = rects[count].y;
			return;
		}
	}

	/* Succeeded: right of the rightmost display. */
	kwl_displays_place_right(rects, count, &head->x, &head->y);
}

/* Lists the rectangles of the output and of the open heads but `skip`; returns how many, at most `room`. */
static unsigned
heads_rects(
	struct kwl_server *server,
	const struct kwl_head *skip,
	struct kwl_display_rect *rects,
	unsigned room)
{
	struct kwl_compose *compose;
	unsigned index;
	unsigned count;

	/* The output first. */
	compose = server->compose;
	count = 0U;
	if (room == 0U)
		return 0U;
	rects[0].x = compose->output_x;
	rects[0].y = compose->output_y;
	rects[0].width = server->width;
	rects[0].height = server->height;
	count = 1U;

	/* Each other open head. */
	for (index = 0U; index < KWL_HEADS && count < room; index++) {
		if (!compose->heads[index].open || &compose->heads[index] == skip)
			continue;
		rects[count].x = compose->heads[index].x;
		rects[count].y = compose->heads[index].y;
		rects[count].width = compose->heads[index].width;
		rects[count].height = compose->heads[index].height;
		count++;
	}

	/* Succeeded: the rectangles. */
	return count;
}

/* Tells whether a frame changes a head: always in the mirror mode, in the extended mode when it is to be drawn again. */
static int
heads_needs_frame(
	struct kwl_server *server,
	const struct kwl_head *head)
{
	const struct kwl_import *wallpaper;
	unsigned shows;

	/* The mirror copies every frame. */
	if (server->compose->display_mode == KWL_DISPLAYS_MIRROR)
		return 1;

	/* An extended head asked to be drawn. */
	if (head->dirty)
		return 1;

	/* A head with windows, or the pointer near, now or in its last picture (ws113-p007). */
	shows = heads_shows(server, head);
	if (shows != 0U || head->showed != 0U)
		return 1;

	/* An extended head whose wallpaper changed. */
	wallpaper = kwl_glass_wallpaper(server);
	if (wallpaper != NULL && wallpaper->image != head->wallpaper)
		return 1;

	/* Nothing it shows changed. */
	return 0;
}

/*
 * Records the mirror's copy: the head's image cleared black, then the
 * output's image (the frame's own, just drawn) fitted whole into it.
 * Without an output image that is a copy's source, the head stays black.
 */
static void
heads_record_mirror(
	struct kwl_server *server,
	VkCommandBuffer command,
	VkImage source,
	struct kwl_head *head)
{
	struct kwl_compose *compose;
	struct kwl_display_rect fitted;
	VkClearColorValue black;
	VkImageSubresourceRange range;
	VkImageBlit blit;
	VkImage target;

	/* The head's image, ready for the copies. */
	compose = server->compose;
	target = head->output.images[head->image];
	heads_barrier(command, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0U, VK_ACCESS_TRANSFER_WRITE_BIT,
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	/* Black, the bars where the desktop does not fill it. */
	memset(&black, 0, sizeof(black));
	black.float32[3] = 1.0f;
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1U;
	range.layerCount = 1U;
	vkCmdClearColorImage(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1U, &range);

	/* The desktop fitted whole, when the output's image can be copied. */
	if (compose->readback) {
		/* The output's image, from presentation to the copy's source. */
		heads_barrier(command, source, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		heads_barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		/* The whole of it into the fitted rectangle. */
		kwl_displays_fit(compose->output.width, compose->output.height, head->output.width, head->output.height, &fitted);
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1U;
		blit.srcOffsets[1].x = (int32_t)compose->output.width;
		blit.srcOffsets[1].y = (int32_t)compose->output.height;
		blit.srcOffsets[1].z = 1;
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1U;
		blit.dstOffsets[0].x = fitted.x;
		blit.dstOffsets[0].y = fitted.y;
		blit.dstOffsets[1].x = fitted.x + (int32_t)fitted.width;
		blit.dstOffsets[1].y = fitted.y + (int32_t)fitted.height;
		blit.dstOffsets[1].z = 1;
		vkCmdBlitImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &blit, VK_FILTER_LINEAR);

		/* The output's image back to presentation. */
		heads_barrier(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0U,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	} else if (!compose->mirror_unsupported_logged) {
		/* Said once: the desktop is not copied. */
		printf("KWL OUTPUT mirror unsupported: the output's images cannot be a copy's source\n");
		compose->mirror_unsupported_logged = 1U;
	}

	/* The head's image to presentation. */
	heads_barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0U,
	    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

/*
 * Records an extended head's picture: the background, and the wallpaper
 * covering the head (its middle, proportions kept) in the glass look.
 */
static void
heads_record_extended(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_head *head)
{
	struct kwl_compose *compose;
	const struct kwl_import *wallpaper;
	struct kwl_object *windows[KWL_FRAME_WINDOWS];
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkDeviceSize offset;
	unsigned count;
	unsigned index;
	unsigned slot;
	float uv[4];

	/* The output's pass on the head's image, cleared to the background. */
	compose = server->compose;
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = KWL_BACKGROUND_RED;
	clear.color.float32[1] = KWL_BACKGROUND_GREEN;
	clear.color.float32[2] = KWL_BACKGROUND_BLUE;
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = compose->pass;
	pass.framebuffer = head->framebuffers[head->image];
	pass.renderArea.extent.width = head->output.width;
	pass.renderArea.extent.height = head->output.height;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The whole head. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)head->output.width;
	viewport.height = (float)head->output.height;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(command, 0U, 1U, &viewport);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = head->output.width;
	scissor.extent.height = head->output.height;
	vkCmdSetScissor(command, 0U, 1U, &scissor);
	offset = 0U;
	vkCmdBindVertexBuffers(command, 0U, 1U, &compose->corners, &offset);

	/* The wallpaper over the whole head (a quad over the desktop's size covers the viewport). */
	wallpaper = kwl_glass_wallpaper(server);
	head->wallpaper = VK_NULL_HANDLE;
	if (wallpaper != NULL) {
		kwl_displays_cover(wallpaper->width, wallpaper->height, head->output.width, head->output.height, uv);
		kwl_compose_image_quad(server, command, wallpaper, 0, 0, server->width, server->height, uv);
		head->wallpaper = wallpaper->image;
	}

	/* From here the pass draws the head's part of the plane: its windows and the cursor over them (ws113-p007). */
	slot = (unsigned)(head - compose->heads) + 1U;
	server->view_output = slot;
	server->view_x = head->x - compose->output_x;
	server->view_y = head->y - compose->output_y;
	server->view_width = head->output.width;
	server->view_height = head->output.height;
	compose->scissor_now = scissor;
	compose->backdrop_set = VK_NULL_HANDLE;
	count = heads_windows(server, slot, windows, KWL_FRAME_WINDOWS);
	if (server->glass) {
		kwl_glass_draw_head(server, command, windows, count);
	} else {
		/* The plain look: each window between its sub-surfaces, then the popups. */
		for (index = 0U; index < count; index++) {
			kwl_subsurface_draw(server, command, windows[index], (float)windows[index]->x, (float)windows[index]->y, 1.0f, 1.0f, 0U);
			kwl_compose_surface_quad(server, command, windows[index], kwl_compose_surface_image(windows[index]), windows[index]->x, windows[index]->y);
			kwl_subsurface_draw(server, command, windows[index], (float)windows[index]->x, (float)windows[index]->y, 1.0f, 1.0f, 1U);
		}

		/* Their popups over them. */
		kwl_popup_draw(server, command);
	}

	/* The cursor where it reaches the head, and what the head shows kept for the next frame's choice. */
	kwl_compose_cursor(server, command);
	head->showed = heads_shows(server, head);

	/* The anchor's part of the plane again, for whatever draws next. */
	server->view_output = KWL_PLANE_ANCHOR;
	server->view_x = 0;
	server->view_y = 0;
	server->view_width = 0U;
	server->view_height = 0U;

	/* Done; the pass leaves the image for presentation. */
	vkCmdEndRenderPass(command);
}

/*
 * Tells what a head shows besides its wallpaper: its windows (the frame's
 * list) and the pointer within a cursor's reach of it, App Home's
 * background, and its bar, as a mask (1 the windows, 2 the pointer, 4 App
 * Home, 8 the bar).
 */
static unsigned
heads_shows(
	struct kwl_server *server,
	const struct kwl_head *head)
{
	struct kwl_compose *compose;
	unsigned shows;
	unsigned slot;
	unsigned count;
	int64_t left;
	int64_t top;
	float home;

	/* Its windows in the frame being made. */
	compose = server->compose;
	slot = (unsigned)(head - compose->heads) + 1U;
	shows = 0U;
	count = heads_windows(server, slot, NULL, 0U);
	if (count > 0U)
		shows |= 1U;

	/* A drag and drop's icon and mark may reach into any head: every head draws while it goes on (ws189-p002). */
	if (server->dnd_active)
		shows |= 2U;

	/* The pointer, a cursor's size around the head's rectangle included. */
	left = (int64_t)head->x - compose->output_x - HEADS_CURSOR_REACH;
	top = (int64_t)head->y - compose->output_y - HEADS_CURSOR_REACH;
	if (server->pointer_x >= left &&
	    server->pointer_x < left + head->width + 2 * HEADS_CURSOR_REACH &&
	    server->pointer_y >= top &&
	    server->pointer_y < top + head->height + 2 * HEADS_CURSOR_REACH)
		shows |= 2U;

	/* App Home's background, while it shows on the anchor (ws113-p015). */
	home = 0.0f;
	if (server->glass)
		home = kwl_home_progress(server);
	if (home > 0.0f)
		shows |= 4U;

	/*
	 * Its bar (ws113-p015, the 2026-10-08 user decision): the clock, the
	 * status and the applications' icons, which change with the anchor's
	 * frames, in the session's glass look (not over the login or the lock
	 * screen, nor with the screen off).
	 */
	if (server->glass &&
	    server->windowed &&
	    !server->greeter &&
	    !server->locked &&
	    !server->screen_off)
		shows |= 8U;

	/* Succeeded: the mask. */
	return shows;
}

/*
 * Lists the windows of the frame being made that a head shows (bottom to
 * top), at most `capacity` of them (none with no list).  Returns how many
 * it has.
 */
static unsigned
heads_windows(
	struct kwl_server *server,
	unsigned slot,
	struct kwl_object **windows,
	unsigned capacity)
{
	struct kwl_compose *compose;
	unsigned count;
	unsigned index;

	/* The frame's heads' windows of this slot. */
	compose = server->compose;
	count = 0U;
	for (index = 0U; index < compose->frame_head_count; index++) {
		if (compose->frame_heads[index]->output != slot)
			continue;
		if (windows != NULL && count < capacity)
			windows[count] = compose->frame_heads[index];
		count++;
	}

	/* Succeeded: how many. */
	return count;
}

/* Records one image layout change with its access and stages. */
static void
heads_barrier(
	VkCommandBuffer command,
	VkImage image,
	VkImageLayout from,
	VkImageLayout to,
	VkAccessFlags source_access,
	VkAccessFlags destination_access,
	VkPipelineStageFlags source_stage,
	VkPipelineStageFlags destination_stage)
{
	VkImageMemoryBarrier barrier;

	/* The whole colour image. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = source_access;
	barrier.dstAccessMask = destination_access;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(command, source_stage, destination_stage, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
}

/* Writes displays.conf's path (folder nonzero: its folder's) from $HOME; returns 0, or ENOENT without a home. */
static int
heads_path(
	char *path,
	size_t size,
	int folder)
{
	const char *home;
	int written;

	/* The user's home. */
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return ENOENT;

	/* The folder, or the file in it. */
	if (folder) {
		written = snprintf(path, size, "%s/%s", home, HEADS_FOLDER);
	} else {
		written = snprintf(path, size, "%s/%s/%s", home, HEADS_FOLDER, HEADS_FILE);
	}

	/* A path too long for the buffer. */
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the path. */
	return 0;
}

/* Makes a folder and the missing folders above it (as mkdir -p), for the user alone; one that exists is left alone. */
static void
heads_mkdir(
	char *folder)
{
	size_t index;

	/* Each prefix that ends at a slash. */
	for (index = 1U; folder[index] != '\0'; index++) {
		if (folder[index] != '/')
			continue;

		/* The folder up to this slash. */
		folder[index] = '\0';
		(void)mkdir(folder, 0700);
		folder[index] = '/';
	}

	/* The folder itself. */
	(void)mkdir(folder, 0700);
}

/*
 * Writes a choice to displays.conf: a new file beside it, flushed, then
 * renamed over it (D-STORE), in the folder made when it is missing.
 * Returns 0 or an errno value; a failure leaves the file as it was.
 */
static int
heads_save(
	const struct kwl_display_config *config)
{
	char path[HEADS_PATH_MAX];
	char folder[HEADS_PATH_MAX];
	char temporary[HEADS_PATH_MAX + 8U];
	char text[HEADS_TEXT_MAX];
	size_t length;
	size_t done;
	ssize_t count;
	int descriptor;
	int status;
	int error;

	/* The text. */
	length = kwl_displays_format(config, text, sizeof(text));
	if (length == 0U)
		return ENOSPC;

	/* The paths, and the folder and those above it (made the user's alone when they are missing: a new home has no .config). */
	error = heads_path(path, sizeof(path), 0);
	if (error != 0)
		return error;
	error = heads_path(folder, sizeof(folder), 1);
	if (error != 0)
		return error;
	heads_mkdir(folder);

	/* A new file beside the file. */
	(void)snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
	descriptor = mkstemp(temporary);
	if (descriptor < 0)
		return errno;

	/* Every byte of the text, an interruption tried again. */
	error = 0;
	done = 0U;
	while (done < length) {
		count = write(descriptor, text + done, length - done);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			error = errno;
			break;
		}

		/* The bytes written. */
		done += (size_t)count;
	}

	/* Flushed before it takes the file's name; a close that fails is a failed write. */
	if (error == 0) {
		status = fsync(descriptor);
		if (status != 0)
			error = errno;
	}

	/* Closed either way. */
	status = close(descriptor);
	if (error == 0 && status != 0)
		error = EIO;

	/* A failed write leaves the file as it was. */
	if (error != 0) {
		(void)unlink(temporary);
		return error;
	}

	/* Renamed over the file, which replaces it at once. */
	status = rename(temporary, path);
	if (status != 0) {
		error = errno;
		(void)unlink(temporary);
		return error;
	}

	/* Succeeded: the file keeps the choice. */
	return 0;
}

/* Tells the clients the displays changed and draws everything again. */
static void
heads_changed(
	struct kwl_server *server)
{
	/* The wl_output of every display, the displays objects' snapshot (ws113-p005), and a whole frame. */
	kwl_outputs_changed(server);
	kwl_displays_tell(server);
	server->dirty = 1;
}

/*
 * Gives the outputs' rectangles of the plane (plane.h), KWL_PLANE_SLOTS of
 * them: the anchor's at the origin, and in the extended mode each open
 * head's at its place less the anchor's (the others without a size).
 * Returns how many.
 */
unsigned
kwl_outputs(
	struct kwl_server *server,
	struct kwl_plane_rect *outputs)
{
	struct kwl_compose *compose;
	const struct kwl_head *head;
	unsigned index;

	/* The anchor, and no head. */
	memset(outputs, 0, KWL_PLANE_SLOTS * sizeof(outputs[0]));
	outputs[KWL_PLANE_ANCHOR].width = server->width;
	outputs[KWL_PLANE_ANCHOR].height = server->height;
	compose = server->compose;
	if (compose == NULL || compose->display_mode != KWL_DISPLAYS_EXTENDED)
		return KWL_PLANE_SLOTS;

	/* Each head shown, while there are slots. */
	for (index = 0U; index < KWL_HEADS && index + 1U < KWL_PLANE_SLOTS; index++) {
		head = &compose->heads[index];
		if (!head->open || head->lost)
			continue;
		outputs[index + 1U].x = head->x - compose->output_x;
		outputs[index + 1U].y = head->y - compose->output_y;
		outputs[index + 1U].width = head->width;
		outputs[index + 1U].height = head->height;
	}

	/* Succeeded: every slot. */
	return KWL_PLANE_SLOTS;
}

/*
 * Gives an output's rectangle of the plane: the anchor's for one not shown.
 * Returns 1 when the output is shown, 0 when the anchor's was given.
 */
int
kwl_output_rect(
	struct kwl_server *server,
	unsigned slot,
	struct kwl_plane_rect *rect)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	unsigned count;

	/* The outputs shown now. */
	count = kwl_outputs(server, outputs);

	/* One not shown is the anchor. */
	if (slot >= count || outputs[slot].width == 0U) {
		*rect = outputs[KWL_PLANE_ANCHOR];
		return 0;
	}

	/* Succeeded: its rectangle. */
	*rect = outputs[slot];
	return 1;
}

/* Gives the output that holds a point of the plane: its slot, the anchor where none does. */
unsigned
kwl_output_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	unsigned count;
	int slot;

	/* The output, or the anchor. */
	count = kwl_outputs(server, outputs);
	slot = kwl_plane_at(outputs, count, x, y);
	if (slot < 0)
		return KWL_PLANE_ANCHOR;

	/* Succeeded: its slot. */
	return (unsigned)slot;
}

/*
 * Gives the least distance of a window's place (its body) below an
 * output's top: under its bar and a floating title bar (the system bar
 * on the anchor, a head's own bar on a head, ws113-p015; the glass look,
 * none otherwise).
 */
int32_t
kwl_output_top(
	struct kwl_server *server,
	unsigned slot)
{
	/* Only the glass look has title bars above the bodies. */
	if (!server->glass)
		return 0;

	/* The anchor has the system bar, a head its own bar (ws113-p015). */
	(void)slot;
	return KWL_GLASS_BAR + KWL_GLASS_GAP + KWL_GLASS_TITLE;
}

/*
 * Gives the output a surface is shown on: its window's (a sub-surface's
 * parent's, a popup's toplevel's); the anchor for a surface of no window.
 */
unsigned
kwl_window_output(
	struct kwl_object *surface)
{
	struct kwl_object *root;

	/* Up from a sub-surface to its parent. */
	root = surface;
	while (root != NULL && root->sub_parent != NULL)
		root = root->sub_parent;

	/* From a popup to its toplevel (the anchor for a popup without one). */
	if (root != NULL && root->role != NULL && root->role->top != NULL && root->role->top->kind == KWL_POPUP)
		root = kwl_popup_root(root);
	if (root == NULL)
		return KWL_PLANE_ANCHOR;

	/* Succeeded: the window's output. */
	return root->output;
}

/*
 * Moves a window to an output (the keyboard's move, the retreat from an
 * output gone): at the same share of the way across, kept inside it, and
 * shown there from the next frame, its sheet with it.
 */
void
kwl_window_to_output(
	struct kwl_server *server,
	struct kwl_object *window,
	unsigned slot,
	const char *why)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	unsigned count;
	unsigned from;

	/* Only to an output shown, from another. */
	count = kwl_outputs(server, outputs);
	if (slot >= count || outputs[slot].width == 0U || window->output == slot)
		return;
	from = window->output;
	if (from >= count || outputs[from].width == 0U)
		from = KWL_PLANE_ANCHOR;

	/* Succeeded: carried there. */
	heads_window_place(server, window, &outputs[from], &outputs[slot], slot, why);
}

/*
 * Makes a window an output's where it is (a move across a shared edge,
 * D-ATOMIC): from the next frame that output alone draws it, its sheet
 * with it.
 */
void
kwl_window_set_output(
	struct kwl_server *server,
	struct kwl_object *window,
	unsigned slot,
	const char *why)
{
	struct kwl_object *sheet;

	/* Only another output's. */
	if (window->output == slot || slot >= KWL_PLANE_SLOTS)
		return;

	/* The window and its sheet. */
	window->output = slot;
	sheet = kwl_sheet_of(window);
	if (sheet != NULL)
		sheet->output = slot;

	/* Drawn again everywhere, and logged. */
	heads_redraw(server);
	printf("KWL WINDOW output surface=%u client=%llu output=%u x=%d y=%d why=%s\n", window->id, (unsigned long long)window->client->number, slot, window->x,
	       window->y, why);
}

/*
 * Moves the pointer by a relative motion over the outputs (plane.h): into
 * the output the motion reaches, or stopped at an edge no output shares.
 * Gives the new place; the output it is on is kept.
 */
void
kwl_pointer_relative(
	struct kwl_server *server,
	int32_t dx,
	int32_t dy,
	int32_t *x,
	int32_t *y)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	unsigned count;
	unsigned slot;

	/* Over the outputs shown. */
	count = kwl_outputs(server, outputs);
	slot = kwl_plane_move(outputs, count, server->pointer_output, server->pointer_x, server->pointer_y, dx, dy, x, y);

	/* A crossing is logged once. */
	if (slot != server->pointer_output) {
		printf("KWL POINTER output=%u x=%d y=%d\n", slot, *x, *y);
		server->dirty = 1;
	}

	/* Succeeded: the output it is on. */
	server->pointer_output = slot;
}

/* Puts the pointer of an absolute device (a tablet, a touch screen) on the anchor, where it maps. */
void
kwl_pointer_absolute(
	struct kwl_server *server)
{
	/* A crossing back is logged once. */
	if (server->pointer_output != KWL_PLANE_ANCHOR) {
		printf("KWL POINTER output=%u absolute\n", KWL_PLANE_ANCHOR);
		server->dirty = 1;
	}

	/* Succeeded: the anchor's. */
	server->pointer_output = KWL_PLANE_ANCHOR;
}

/*
 * Takes the windows and the pointer off an output going away (a head
 * closed, the mirror mode chosen) to the anchor, at the same share of the
 * way across.
 */
static void
heads_evacuate(
	struct kwl_server *server,
	unsigned slot)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	struct kwl_client *client;
	struct kwl_object *object;
	const struct kwl_head *head;
	unsigned count;
	int32_t x;
	int32_t y;
	int window;

	/* The outputs, and the one going where it was (also when its display is already lost, so not shown). */
	count = kwl_outputs(server, outputs);
	if (slot == KWL_PLANE_ANCHOR || slot >= count || slot > KWL_HEADS)
		return;
	head = &server->compose->heads[slot - 1U];
	outputs[slot].x = head->x - server->compose->output_x;
	outputs[slot].y = head->y - server->compose->output_y;
	outputs[slot].width = head->width;
	outputs[slot].height = head->height;

	/* Its docked and arranged windows float where they were first (ws113-p015: the anchor keeps its own states). */
	kwl_glass_leave_quiet(server, slot, "retreat");
	kwl_arrange_end_all(server, slot, "retreat");

	/* Each window on it, carried to the anchor (no place to keep for one of an output not shown). */
	for (client = server->clients; client != NULL; client = client->next) {
		for (object = client->objects; object != NULL; object = object->next) {
			window = heads_is_window(object);
			if (!window || object->output != slot)
				continue;
			if (outputs[slot].width == 0U) {
				object->output = KWL_PLANE_ANCHOR;
				continue;
			}

			/* Carried. */
			heads_window_place(server, object, &outputs[slot], &outputs[KWL_PLANE_ANCHOR], KWL_PLANE_ANCHOR, "retreat");
		}
	}

	/* The pointer on it comes to the same share of the anchor. */
	if (server->pointer_output != slot)
		return;
	kwl_plane_carry(&outputs[slot], &outputs[KWL_PLANE_ANCHOR], server->pointer_x, server->pointer_y, 1U, 1U, 0, &x, &y);
	server->pointer_x = x;
	server->pointer_y = y;
	server->pointer_output = KWL_PLANE_ANCHOR;
	server->dirty = 1;
	printf("KWL POINTER output=0 x=%d y=%d why=retreat\n", x, y);
}

/*
 * Keeps the windows and the pointer with their outputs after the places
 * changed (`before`: the outputs' rectangles then): a head's go along by
 * its move; with `leave` (the mirror chosen) every head's go to the anchor.
 */
static void
heads_windows_follow(
	struct kwl_server *server,
	const struct kwl_plane_rect *before,
	unsigned leave)
{
	struct kwl_plane_rect after[KWL_PLANE_SLOTS];
	struct kwl_client *client;
	struct kwl_object *object;
	unsigned slot;
	int32_t dx;
	int32_t dy;
	int window;

	/* A head going away: its docked and arranged windows float where they were first (ws113-p015). */
	(void)kwl_outputs(server, after);
	for (slot = 1U; slot < KWL_PLANE_SLOTS; slot++) {
		if (!leave && after[slot].width != 0U && before[slot].width != 0U)
			continue;
		kwl_glass_leave_quiet(server, slot, "mode");
		kwl_arrange_end_all(server, slot, "mode");
	}

	/* The mirror: everything on the anchor, carried from where the heads were. */
	for (client = server->clients; client != NULL; client = client->next) {
		for (object = client->objects; object != NULL; object = object->next) {
			window = heads_is_window(object);
			if (!window || object->output == KWL_PLANE_ANCHOR || object->output >= KWL_PLANE_SLOTS)
				continue;
			slot = object->output;

			/* To the anchor when the head is not shown now (or the mirror is chosen). */
			if (leave || after[slot].width == 0U || before[slot].width == 0U) {
				heads_window_place(server, object, &before[slot], &after[KWL_PLANE_ANCHOR], KWL_PLANE_ANCHOR, "mode");
				continue;
			}

			/* Along with its head, and the place it comes back to from docked. */
			dx = after[slot].x - before[slot].x;
			dy = after[slot].y - before[slot].y;
			object->x += dx;
			object->y += dy;
			object->restore_x += dx;
			object->restore_y += dy;
		}
	}

	/* The pointer on a head: along with it, or to the anchor. */
	slot = server->pointer_output;
	if (slot == KWL_PLANE_ANCHOR || slot >= KWL_PLANE_SLOTS)
		return;
	if (leave || after[slot].width == 0U || before[slot].width == 0U) {
		kwl_plane_carry(&before[slot], &after[KWL_PLANE_ANCHOR], server->pointer_x, server->pointer_y, 1U, 1U, 0, &server->pointer_x, &server->pointer_y);
		server->pointer_output = KWL_PLANE_ANCHOR;
		return;
	}

	/* Along with its head. */
	server->pointer_x += after[slot].x - before[slot].x;
	server->pointer_y += after[slot].y - before[slot].y;
}

/* Carries a window (and its sheet) from one output's rectangle to another's, makes it that output's, and logs why. */
static void
heads_window_place(
	struct kwl_server *server,
	struct kwl_object *window,
	const struct kwl_plane_rect *from,
	const struct kwl_plane_rect *to,
	unsigned slot,
	const char *why)
{
	struct kwl_object *sheet;
	uint32_t width;
	uint32_t height;
	int32_t x;
	int32_t y;

	/* Its size, and its place there. */
	kwl_surface_size(window, &width, &height);
	kwl_plane_carry(from, to, window->x, window->y, width, height, kwl_output_top(server, slot), &x, &y);
	window->x = x;
	window->y = y;
	window->output = slot;

	/* Its sheet goes with it (sheet.c places it under the parent's title bar). */
	sheet = kwl_sheet_of(window);
	if (sheet != NULL)
		sheet->output = slot;

	/* Drawn again everywhere, and logged. */
	heads_redraw(server);
	printf("KWL WINDOW output surface=%u client=%llu output=%u x=%d y=%d why=%s\n", window->id, (unsigned long long)window->client->number, slot, window->x,
	       window->y, why);
}

/* Draws every output again at the next frame (a window came or went on one). */
static void
heads_redraw(
	struct kwl_server *server)
{
	unsigned index;

	/* The anchor's frame, and each head's with it. */
	server->dirty = 1;
	if (server->compose == NULL)
		return;
	for (index = 0U; index < KWL_HEADS; index++)
		server->compose->heads[index].dirty = 1U;
}

/* Tells whether an object is a window: a live surface with a toplevel role. */
static int
heads_is_window(
	const struct kwl_object *object)
{
	/* A surface of a toplevel. */
	if (object->kind != KWL_SURFACE || object->dead || object->role == NULL || object->role->top == NULL)
		return 0;
	if (object->role->top->kind != KWL_TOPLEVEL)
		return 0;

	/* Succeeded: a window. */
	return 1;
}

/*
 * Turns a display off, or on again, in the extended mode (ws113-p014,
 * the system extension's set_shown): kept in displays.conf, its head
 * closed (its windows to the anchor) or opened, and an anchor turned off
 * gives the desktop to a display on.  *saved is 0, or the error of writing
 * the file.  Returns 0, EINVAL in the mirror or for the last display on,
 * ENOENT for a key not connected, or the choice's error.
 */
int
kwl_displays_set_shown(
	struct kwl_server *server,
	const char *key,
	unsigned shown,
	int *saved)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	const char *what;
	unsigned index;
	unsigned other;
	unsigned others;
	int differs;
	int off;
	int error;

	/* Only the extended mode turns a display off. */
	*saved = 0;
	compose = server->compose;
	if (compose == NULL)
		return ENODEV;
	if (compose->display_mode != KWL_DISPLAYS_EXTENDED)
		return EINVAL;

	/* The display named, connected now. */
	for (index = 0U; index < compose->display_count; index++) {
		differs = strcmp(compose->display_names[index], key);
		if (differs == 0)
			break;
	}

	/* Not connected now. */
	if (index == compose->display_count)
		return ENOENT;

	/* Not the last display on (one kept off under a closed lid is not on). */
	others = 0U;
	for (other = 0U; other < compose->display_count && !shown; other++) {
		if (other == index || compose->displays[other] == compose->kept_off)
			continue;
		off = kwl_displays_is_off(&compose->config, compose->display_names[other]);
		if (!off)
			others++;
	}

	/* The last display on stays on. */
	if (!shown && others == 0U)
		return EINVAL;

	/* The choice, and the file. */
	error = kwl_displays_set_off(&compose->config, key, !shown);
	if (error != 0)
		return error;
	*saved = heads_save(&compose->config);
	if (*saved != 0)
		printf("KWL DISPLAYS save failed errno=%d\n", *saved);

	/* Its head closed (its windows and the pointer to the anchor), the heads of the displays on opened. */
	head = heads_find(compose, compose->displays[index]);
	if (!shown && head != NULL)
		heads_close(server, head);
	kwl_heads_sync(server);

	/* An anchor turned off gives the desktop away, and the clients hear of it. */
	kwl_displays_anchor_follow(server);
	heads_changed(server);
	what = "off";
	if (shown)
		what = "on";
	printf("KWL DISPLAYS %s name=%s\n", what, key);

	/* Succeeded: turned off or on. */
	return 0;
}

/*
 * Gives the desktop to a display on when the anchor's display is turned
 * off (ws113-p014): the output moves to the first connected display on that
 * takes it.  Nothing when the anchor is on, or no other display is on.
 */
void
kwl_displays_anchor_follow(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	unsigned index;
	int anchor;
	int off;
	int error;

	/* An open output whose display is turned off. */
	compose = server->compose;
	if (compose == NULL || !compose->output_open || compose->output_lost)
		return;
	anchor = heads_index(compose, compose->display);
	if (anchor < 0)
		return;
	off = heads_off(compose, (unsigned)anchor);
	if (!off)
		return;

	/* The first display on that takes the output. */
	for (index = 0U; index < compose->display_count; index++) {
		if ((int)index == anchor || compose->displays[index] == compose->kept_off)
			continue;
		if ((compose->limited & ((uint32_t)1U << index)) != 0U)
			continue;
		if ((compose->move_failed & ((uint32_t)1U << index)) != 0U)
			continue;
		off = heads_off(compose, index);
		if (off)
			continue;

		/* The move. */
		printf("KWL DISPLAYS anchor off name=%s to=%s\n", compose->display_name, compose->display_names[index]);
		error = kwl_output_switch(server, compose->displays[index]);
		if (error == 0)
			return;
	}
}

/*
 * Takes in a move of the output that failed at its first frame and went
 * back to the display `back` (BUG-266): that display is on again in the
 * choice (an anchor turned off gave the desktop to the display that
 * failed), the heads follow, and the clients hear the displays as they
 * are, so that Settings shows the choice did not hold.
 */
void
kwl_displays_move_failed(
	struct kwl_server *server,
	const char *back)
{
	struct kwl_compose *compose;
	int saved;
	int off;
	int error;

	/* The display gone back to is the anchor again, and on when the choice turned it off. */
	compose = server->compose;
	(void)snprintf(compose->config.anchor, sizeof(compose->config.anchor), "%s", back);
	off = kwl_displays_is_off(&compose->config, back);
	if (off) {
		error = kwl_displays_set_off(&compose->config, back, 0U);
		if (error != 0)
			printf("KWL DISPLAYS move failed: %s stays off errno=%d\n", back, error);
	}

	/* The choice written, apart from the displays as they are (D-STORE). */
	saved = heads_save(&compose->config);
	if (saved != 0)
		printf("KWL DISPLAYS save failed errno=%d\n", saved);

	/* The heads of the displays on, and the clients told. */
	kwl_heads_sync(server);
	heads_changed(server);
	printf("KWL DISPLAYS move failed back=%s on_again=%d\n", back, off);
}

/* Finds a display's place in the last enumeration: its index, or -1 when it is not connected. */
static int
heads_index(
	const struct kwl_compose *compose,
	VkDisplayKHR display)
{
	unsigned index;

	/* Handles compare only for equality. */
	for (index = 0U; index < compose->display_count; index++) {
		if (compose->displays[index] == display)
			return (int)index;
	}

	/* Not connected. */
	return -1;
}

/*
 * Tells whether a display connected now is off (ws113-p014): turned off
 * by the user, in the extended mode, while another connected display is
 * on (with none on, every display shows: the screen is never all off).
 */
static int
heads_off(
	const struct kwl_compose *compose,
	unsigned index)
{
	unsigned other;
	int off;

	/* Turned off, in the extended mode. */
	if (compose->display_mode != KWL_DISPLAYS_EXTENDED || index >= compose->display_count)
		return 0;
	off = kwl_displays_is_off(&compose->config, compose->display_names[index]);
	if (!off)
		return 0;

	/* Another display connected and on (not one kept off under a closed lid). */
	for (other = 0U; other < compose->display_count; other++) {
		if (other == index || compose->displays[other] == compose->kept_off)
			continue;
		off = kwl_displays_is_off(&compose->config, compose->display_names[other]);
		if (!off)
			return 1;
	}

	/* None: it shows. */
	return 0;
}
