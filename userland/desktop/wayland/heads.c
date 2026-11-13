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

/*
 * One writing of displays.conf, made ready on the main thread (the paths
 * read HOME there): the file, its folder, and the text.
 */
struct heads_write {
	char path[HEADS_PATH_MAX];
	char folder[HEADS_PATH_MAX];
	char text[HEADS_TEXT_MAX];
	size_t length;
};

/*
 * One writing of displays.conf given to the file I/O thread (io.c): the
 * lane's job, the writing's ticket (the choices given up to it are in
 * it), the writing, and how it went (its error, and the times given,
 * started and ended, for the log).  A newer writing replaces one not
 * started yet: the file ends with the latest choice either way.
 */
struct heads_file_job {
	struct kwl_lane_job lane;
	uint64_t ticket;
	struct heads_write write;
	int error;
	uint64_t given_ms;
	uint64_t started_ms;
	uint64_t ended_ms;
};

/*
 * The writings of displays.conf: the ticket of the last one given, the
 * last one written (every choice given up to it is in the file, or failed
 * to be), and the error of that writing.  Only the event loop's thread
 * reads and writes it.
 */
struct heads_saving {
	uint64_t given;
	uint64_t written;
	int error;
};

/*
 * A warp of the pointer off an output going away (heads_evacuate): the
 * output and its rectangle before it went, and where the warp says whether
 * the pointer was on it and carried.
 */
struct heads_pointer_carry {
	unsigned slot;
	struct kwl_plane_rect from;
	unsigned *carried;
};

/*
 * A warp of the pointer along with the heads that moved
 * (heads_windows_follow): the outputs' rectangles before and after, and
 * whether everything goes to the anchor (the mirror chosen).
 */
struct heads_pointer_shift {
	const struct kwl_plane_rect *before;
	const struct kwl_plane_rect *after;
	unsigned leave;
};

/*
 * A bringing in line of the heads (kwl_heads_sync): what is left of the
 * heads let go (rests, rest_count of them), the displays that get a head
 * (opens, open_count of them, each with the head chosen), whether the
 * heads changed, and whether it is a step of another change of the
 * display (nested: that change ends it).
 */
struct heads_sync {
	struct kwl_head_rest rests[KWL_HEADS];
	unsigned rest_count;
	struct kwl_head_opening opens[KWL_HEADS];
	unsigned open_count;
	unsigned changed;
	unsigned nested;
};

static int heads_take(struct kwl_server *server, struct kwl_head *head, struct kwl_head_opening *opening);
static void heads_close(struct kwl_server *server, struct kwl_head *head);
static void heads_let_go(struct kwl_server *server, struct kwl_head *head, struct kwl_head_rest *rest);
static VkResult heads_targets(struct kwl_compose *compose, struct kwl_head *head);
static void heads_targets_destroy(struct kwl_compose *compose, struct kwl_head *head);
static struct kwl_head *heads_find(struct kwl_compose *compose, VkDisplayKHR display);
static void heads_place(struct kwl_server *server, struct kwl_head *head);
static unsigned heads_rects(struct kwl_server *server, const struct kwl_head *skip, struct kwl_display_rect *rects, unsigned room);
static int heads_needs_frame(struct kwl_server *server, const struct kwl_head *head);
static unsigned heads_shows(struct kwl_server *server, const struct kwl_head *head);
static unsigned heads_windows(struct kwl_server *server, unsigned slot, struct kwl_object **windows, unsigned capacity);
static void heads_record_mirror(struct kwl_server *server, struct kwl_list *command, VkImage source, VkImageLayout layout, struct kwl_head *head);
static void heads_record_extended(struct kwl_server *server, struct kwl_list *command, struct kwl_head *head);
static void heads_barrier(struct kwl_list *command, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source_access, VkAccessFlags destination_access, VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage);
static int heads_path(char *path, size_t size, int folder);
static int heads_write_ready(const struct kwl_display_config *config, struct heads_write *write_job);
static int heads_write_file(struct heads_write *write_job);
static void heads_file_run(struct kwl_lane_job *job);
static void heads_file_done(void *context, struct kwl_lane_job *job);
static void heads_file_drop(struct kwl_lane_job *job);
static void heads_mkdir(char *folder);
static void heads_changed(struct kwl_server *server);
static void heads_evacuate(struct kwl_server *server, unsigned slot);
static void heads_sync_parked(struct kwl_server *server, int error, void *context);
static void heads_sync_made(struct kwl_server *server, int error, void *context);
static void heads_windows_follow(struct kwl_server *server, const struct kwl_plane_rect *before, unsigned leave);
static void heads_pointer_evacuate(const void *context, const struct kwl_plane_rect *outputs, int32_t *x, int32_t *y, unsigned *output);
static void heads_pointer_follow(const void *context, const struct kwl_plane_rect *outputs, int32_t *x, int32_t *y, unsigned *output);
static void heads_window_place(struct kwl_server *server, struct kwl_object *window, const struct kwl_plane_rect *from, const struct kwl_plane_rect *to, unsigned slot, const char *why);
static int heads_is_window(const struct kwl_object *object);
static void heads_redraw(struct kwl_server *server);
static int heads_index(const struct kwl_compose *compose, VkDisplayKHR display);
static int heads_off(const struct kwl_compose *compose, unsigned index);

/* The writings of displays.conf (struct heads_saving), the event loop's thread's alone. */
static struct heads_saving heads_saving;

/* The operations of a writing of displays.conf on the file I/O thread. */
static const struct kwl_lane_ops heads_file_ops = {
	heads_file_run,
	heads_file_done,
	heads_file_drop
};

/*
 * Gives the displays' choice as it is now to be written to displays.conf
 * by the file I/O thread, so that the event loop does not wait for the
 * file's flush (seconds on UFS); without the thread it is written here.
 * Returns 0 (given, or written) or an errno value; a failure leaves the
 * file as it was.  The writing that holds the choice is the ticket
 * kwl_heads_config_ticket gives now, and kwl_displays_written answers the
 * requests that wait for it.
 */
int
kwl_heads_config_save(
	struct kwl_server *server)
{
	struct heads_file_job *job;
	int error;

	/* The job's record. */
	job = calloc(1U, sizeof(*job));
	if (job == NULL)
		return ENOMEM;

	/* The text and the paths, made here. */
	error = heads_write_ready(&server->compose->config, &job->write);
	if (error != 0) {
		free(job);
		return error;
	}

	/* A ticket after every one given before. */
	heads_saving.given++;
	job->ticket = heads_saving.given;
	job->given_ms = kwl_milliseconds();

	/* The thread writes it, after the writings given before; a newer one replaces it while it waits. */
	error = kwl_io_file_submit(server, &job->lane, &heads_file_ops, KWL_IO_KEY_DISPLAYS);
	if (error == 0)
		return 0;

	/* Without the thread the file is written here. */
	error = heads_write_file(&job->write);
	heads_saving.written = job->ticket;
	heads_saving.error = error;
	free(job);
	if (error != 0)
		return error;

	/* Succeeded: the file holds the choice. */
	return 0;
}

/*
 * Gives the ticket of the writing of displays.conf that holds the choice
 * as it was last given (0: none given yet).
 */
uint64_t
kwl_heads_config_ticket(void)
{
	/* The last writing given holds every choice given before it. */
	return heads_saving.given;
}

/*
 * Tells whether the writing of a ticket is over (the file holds its
 * choice, or failed to): 1 with its error in *error, or 0 while it waits.
 */
int
kwl_heads_config_settled(
	uint64_t ticket,
	int *error)
{
	/* Not written yet. */
	*error = 0;
	if (ticket > heads_saving.written)
		return 0;

	/* Succeeded: written by this writing or a later one, whose error it is. */
	*error = heads_saving.error;
	return 1;
}

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

	/* The rotations the file keeps, and its rotate lines that were not one (WS217). */
	if (compose->config.rotation_count != 0U || compose->config.rotation_dropped != 0U)
		printf("KWL DISPLAYS config rotations=%u dropped=%u\n", compose->config.rotation_count, compose->config.rotation_dropped);
}

/*
 * Brings the heads in line with the displays of the last enumeration:
 * a head for each display connected but the output's, the one kept off,
 * the ones the user turned off (ws113-p014) and the ones refused since the
 * last hotplug; no head for a display gone or lost.  Waits for it: the
 * steps run here (where the caller needs the heads on return: Settings'
 * choice applied, a display turned on or off).
 */
void
kwl_heads_sync(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	struct heads_sync *sync;

	/* Only with the device. */
	compose = server->compose;
	if (compose == NULL)
		return;

	/* A change running ends first, unless this is a step of it (the render thread is parked then). */
	kwl_display_settle(server);
	sync = calloc(1, sizeof(*sync));
	if (sync == NULL) {
		printf("KWL OUTPUT heads errno=%d\n", ENOMEM);
		return;
	}
	compose->display_inline++;
	if (compose->display_busy) {
		sync->nested = 1U;
		heads_sync_parked(server, 0, sync);
	} else if (kwl_display_begin(server, heads_sync_parked, sync) != 0) {
		free(sync);
	}
	compose->display_inline--;
}

/*
 * Brings the heads in line as kwl_heads_sync does, in a change of the
 * display while the passes go on: the heads' swapchains are destroyed and
 * made by the display thread.  Returns 0, or EBUSY while another change
 * runs (a later look tries again: heads_stale stays).
 */
int
kwl_heads_sync_later(
	struct kwl_server *server)
{
	struct heads_sync *sync;
	int error;

	/* Only with the device. */
	if (server->compose == NULL)
		return 0;

	/* The change. */
	sync = calloc(1, sizeof(*sync));
	if (sync == NULL)
		return ENOMEM;
	error = kwl_display_begin(server, heads_sync_parked, sync);
	if (error != 0)
		free(sync);
	return error;
}

/*
 * Brings the heads in line, the render thread parked and drained: the
 * heads of displays gone, lost, turned off or now the output's are let go,
 * and a head is chosen for each display that may have one; the display
 * thread destroys the old swapchains and makes the new ones
 * (heads_sync_made goes on).
 */
static void
heads_sync_parked(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kwl_head_opening *opening;
	struct kwl_compose *compose;
	struct heads_sync *sync;
	struct kwl_head *head;
	unsigned taken;
	unsigned index;
	unsigned slot;
	int listed;
	int off;

	/* The heads of displays gone, lost, or now the output's go. */
	(void)error;
	sync = context;
	compose = server->compose;
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
		heads_let_go(server, head, &sync->rests[sync->rest_count]);
		sync->rest_count++;
		sync->changed = 1U;
	}

	/* Without the output, no head opens (the output is closed for a hand-over, or lost). */
	compose->heads_stale = 0U;
	taken = 0U;
	if (compose->output_open && !compose->output_lost) {
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

			/* A head free for it (one not chosen for another display of this look). */
			for (slot = 0U; slot < KWL_HEADS; slot++) {
				if (!compose->heads[slot].open && (taken & (1U << slot)) == 0U)
					break;
			}
			if (slot == KWL_HEADS)
				break;
			taken |= 1U << slot;
			opening = &sync->opens[sync->open_count];
			opening->slot = slot;
			opening->index = index;
			opening->display = compose->displays[index];
			(void)snprintf(opening->name, sizeof(opening->name), "%s", compose->display_names[index]);
			sync->open_count++;
		}
	}

	/* The display thread's part, then heads_sync_made. */
	kwl_display_heads(server, sync->rests, sync->rest_count, sync->opens, sync->open_count, heads_sync_made, sync);
}

/*
 * Takes in what the display thread did for the heads: the heads closed
 * are told, each head made is taken in (its targets, its place, a global),
 * a display that refused its swapchain is tried again after the next
 * hotplug; the clients learn of the change, and the change of the display
 * ends (unless the heads were brought in line as a step of another).
 */
static void
heads_sync_made(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kwl_head_opening *opening;
	struct kwl_compose *compose;
	struct heads_sync *sync;
	unsigned index;

	/* The heads closed. */
	(void)error;
	sync = context;
	compose = server->compose;
	for (index = 0U; index < sync->rest_count; index++)
		printf("KWL OUTPUT head closed name=%s\n", sync->rests[index].name);

	/* Each head made, or refused. */
	for (index = 0U; index < sync->open_count; index++) {
		opening = &sync->opens[index];
		if (opening->error == 0)
			opening->error = heads_take(server, &compose->heads[opening->slot], opening);
		if (opening->error != 0) {
			compose->limited |= (uint32_t)1U << opening->index;
			printf("KWL OUTPUT head refused name=%s errno=%d\n", opening->name, opening->error);
			continue;
		}
		sync->changed = 1U;
	}

	/* The clients learn of the change, and the change ends. */
	if (sync->changed)
		heads_changed(server);
	if (!sync->nested)
		kwl_display_end(server);
	free(sync);
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

/*
 * Lets go of every head with the output (a move, a hand-over, the game
 * mode, the compositor's exit), the render thread parked and drained: each
 * head's main part here, its swapchain, surface and display kept in rests
 * (count of them, at most KWL_HEADS) for the display thread to destroy
 * (kwl_heads_rest_destroy) with the output's.
 */
void
kwl_heads_close_all(
	struct kwl_server *server,
	struct kwl_head_rest *rests,
	unsigned *count)
{
	struct kwl_compose *compose;
	unsigned index;
	unsigned changed;

	/* Each open head. */
	*count = 0U;
	compose = server->compose;
	if (compose == NULL)
		return;
	changed = 0U;
	for (index = 0U; index < KWL_HEADS; index++) {
		if (!compose->heads[index].open)
			continue;
		heads_let_go(server, &compose->heads[index], &rests[*count]);
		(*count)++;
		changed = 1U;
	}

	/* The output opened again brings them back: the follow after this change. */
	compose->heads_stale = 1U;
	kwl_output_follow_later(server);
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
 * Lists the heads a frame needs now (open, shown, and changed or showing
 * windows or the pointer), for the render thread's want (ws213-p004).
 * Returns how many, at most room.
 */
unsigned
kwl_heads_wanted(
	struct kwl_server *server,
	unsigned *slots,
	unsigned room)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	unsigned count;
	unsigned index;
	int needs;

	/* Each open head the frame changes. */
	compose = server->compose;
	count = 0U;
	for (index = 0U; index < KWL_HEADS && count < room; index++) {
		head = &compose->heads[index];
		if (!head->open || head->lost)
			continue;
		needs = heads_needs_frame(server, head);
		if (!needs)
			continue;

		/* Wanted. */
		slots[count] = index;
		count++;
	}

	/* Succeeded: how many. */
	return count;
}

/*
 * Takes the heads' images the render thread holds into the frame being
 * walked (ws213-p004, design 4.1): every held image is drawn and presented,
 * also of a head that no longer needs a frame.  A head that needs one but
 * has no image is drawn at a later frame (one is asked for in the extended
 * mode); one whose acquire failed otherwise is lost.  Returns how many
 * missed the frame.
 */
unsigned
kwl_heads_take(
	struct kwl_server *server,
	const struct kwl_render_held *held)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	unsigned missed;
	unsigned index;
	VkResult result;
	int needs;

	/* Each open head. */
	compose = server->compose;
	missed = 0U;
	for (index = 0U; index < KWL_HEADS; index++) {
		head = &compose->heads[index];
		head->in_frame = 0U;
		if (!head->open || head->lost)
			continue;

		/* An image held: drawn and presented in this frame. */
		if (held->heads[index]) {
			head->in_frame = 1U;
			head->image = held->head_images[index];
			continue;
		}

		/* Only a head the frame changes is missed. */
		needs = heads_needs_frame(server, head);
		if (!needs)
			continue;

		/* Not ready (or not asked yet): the next frame draws it (one is asked for, ws113-p007). */
		result = held->head_results[index];
		if (result == VK_SUCCESS || result == VK_NOT_READY || result == VK_TIMEOUT) {
			head->dirty = 1U;
			if (compose->display_mode == KWL_DISPLAYS_EXTENDED)
				missed++;
			continue;
		}

		/* The display went (or another failure of its swapchain): the follow of the displays closes the head. */
		printf("KWL OUTPUT head lost operation=acquire name=%s result=%d\n", head->name, (int)result);
		head->lost = 1U;
		kwl_output_follow_later(server);
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
	struct kwl_list *command,
	VkImage source,
	VkImageLayout layout)
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
			heads_record_mirror(server, command, source, layout, head);
		} else {
			heads_record_extended(server, command, head);
		}
	}
}

/*
 * Applies the presents of the heads' images the render thread made for a
 * completed frame: an extended head shown is not drawn again until it
 * changes; a display gone marks the head lost.
 */
void
kwl_heads_presented(
	struct kwl_server *server,
	const struct kwl_render_outcome *outcome)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	unsigned index;
	VkResult result;

	/* Each head the frame presented. */
	compose = server->compose;
	for (index = 0U; index < KWL_HEADS; index++) {
		head = &compose->heads[index];
		head->in_frame = 0U;
		if (!outcome->heads[index] || !head->open)
			continue;

		/* Shown: an extended head waits for a change. */
		result = outcome->head_results[index];
		if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
			head->dirty = 0U;
			continue;
		}

		/* The display went: the follow of the displays closes the head. */
		printf("KWL OUTPUT head lost operation=present name=%s result=%d\n", head->name, (int)result);
		head->lost = 1U;
		kwl_output_follow_later(server);
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
	*saved = kwl_heads_config_save(server);
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
 * x=X y=Y rotation=R scanout=S turn=T" (the logical size and the angles,
 * WS217) and one "head ..." line for each open head.  Returns the
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
	written = snprintf(text, size, "mode=%s\noutput name=%s width=%u height=%u x=%ld y=%ld rotation=%u scanout=%u turn=%u\n", mode,
	    compose->display_name, server->width, server->height, (long)compose->output_x, (long)compose->output_y, compose->rotation,
	    compose->scanout_rotation, compose->turn_rotation);
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
		view->mode_width = server->width;
		view->mode_height = server->height;

		/* The display's angle and physical mode, once the device is open (WS217). */
		if (compose != NULL) {
			view->rotation = compose->rotation;
			view->mode_width = compose->mode_width;
			view->mode_height = compose->mode_height;
		}

		/* Its place in the extended plane. */
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

	/* Its own size, unrotated (a head's rotation is ws217-p005's); in the mirror mode the desktop at the origin. */
	view->width = shown->width;
	view->height = shown->height;
	view->refresh = shown->refresh;
	view->mode_width = shown->width;
	view->mode_height = shown->height;
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
 * Makes a head's swapchain on a display (opening's), on the display
 * thread: the display's size, refresh and name now, its surface at its
 * native mode in the output's format (the pass and pipelines are the
 * output's), the display acquired, its FIFO swapchain (a copy's
 * destination too, for the mirror).  It touches no field of the
 * compositor's.  opening's error is 0, ENXIO for a display not connected
 * now, ENOTSUP for one whose format is not the output's, or EIO (the
 * swapchain refused: the limit of the displays shown at once); nothing is
 * kept then.
 */
void
kwl_heads_make(
	struct kwl_server *server,
	struct kwl_head_opening *opening)
{
	struct kwl_compose *compose;
	char read_name[KWL_COMPOSE_NAME];
	VkResult result;

	/* The display's size, refresh and name now. */
	compose = server->compose;
	memset(&opening->output, 0, sizeof(opening->output));
	result = kwl_compose_display_read(server, opening->display, &opening->width, &opening->height, &opening->refresh, read_name, sizeof(read_name));
	if (result != VK_SUCCESS) {
		opening->error = ENXIO;
		return;
	}

	/* Its plane's surface at its native mode, in the output's format. */
	result = vkdemo_display_open_on(compose->instance, compose->physical, opening->display, opening->width, opening->height, &opening->output);
	if (result == VK_SUCCESS)
		result = vkdemo_display_choose_format(compose->physical, &opening->output);
	if (result != VK_SUCCESS) {
		vkdemo_display_close(compose->instance, compose->device, &opening->output);
		opening->error = EIO;
		return;
	}

	/* A format other than the output's cannot take the output's pass. */
	if (opening->output.format != compose->format) {
		vkdemo_display_close(compose->instance, compose->device, &opening->output);
		opening->error = ENOTSUP;
		return;
	}

	/* The display's permission for its swapchain. */
	result = kwl_os_display_acquire(server, compose->physical, opening->display);
	if (result != VK_SUCCESS) {
		vkdemo_display_close(compose->instance, compose->device, &opening->output);
		opening->error = EIO;
		return;
	}

	/* Its FIFO swapchain, drawn into and copied into. */
	result = vkdemo_display_create_swapchain_usage(compose->physical, compose->device, compose->family, &opening->output, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (result != VK_SUCCESS ||
	    opening->output.image_count > KWL_SWAPCHAIN_MAX ||
	    opening->output.format != compose->format) {
		vkdemo_display_close(compose->instance, compose->device, &opening->output);
		kwl_os_display_release(server, compose->physical, opening->display);
		opening->error = EIO;
		return;
	}

	/* Succeeded: the swapchain for the main thread to take in. */
	opening->error = 0;
}

/*
 * Takes a head's swapchain the display thread made into a head, on the
 * main thread: a view, a framebuffer and a present semaphore for each
 * image and the acquire's semaphore, its place and its wl_output global.
 * Returns 0, or EIO when they could not be made (nothing is kept: the
 * swapchain is destroyed and the display given back here, a failure's
 * path).
 */
static int
heads_take(
	struct kwl_server *server,
	struct kwl_head *head,
	struct kwl_head_opening *opening)
{
	struct kwl_compose *compose;
	VkSemaphoreCreateInfo semaphore;
	VkResult result;

	/* The display and its swapchain. */
	compose = server->compose;
	memset(head, 0, sizeof(*head));
	head->display = opening->display;
	head->width = opening->width;
	head->height = opening->height;
	head->refresh = opening->refresh;
	head->output = opening->output;
	(void)snprintf(head->name, sizeof(head->name), "%s", opening->name);

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
		kwl_os_display_release(server, compose->physical, head->display);
		memset(head, 0, sizeof(*head));
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
	struct kwl_head_rest rest;
	struct kwl_compose *compose;

	/*
	 * No frame may still use its images: the render thread parks after the
	 * frame in flight and the tasks before (ws213-p004), forgets the image
	 * it holds of it, and the device is idle.
	 */
	compose = server->compose;
	kwl_compose_park(server);
	heads_let_go(server, head, &rest);
	kwl_device_wait_idle(compose);

	/* The swapchain and surface, then the display given back; the render thread goes on. */
	kwl_heads_rest_destroy(server, &rest, 1U);
	kwl_compose_unpark(server);
	printf("KWL OUTPUT head closed name=%s\n", rest.name);
}

/*
 * Lets go of a head on the main thread, the render thread parked: its
 * windows and the pointer go to the anchor, its global, the image the
 * render thread holds of it, its targets and its semaphore; rest keeps its
 * swapchain and surface and its display, for kwl_heads_rest_destroy once
 * no frame and no present of it may still run.
 */
static void
heads_let_go(
	struct kwl_server *server,
	struct kwl_head *head,
	struct kwl_head_rest *rest)
{
	struct kwl_compose *compose;

	/* Its windows and the pointer go to the anchor while its place is still known (D-HOTPLUG, ws113-p007). */
	compose = server->compose;
	heads_evacuate(server, (unsigned)(head - compose->heads) + 1U);

	/* The clients lose its global first. */
	if (head->global != 0U)
		kwl_output_global_remove(server, head->global);

	/* The image held of it, its targets and its semaphore. */
	kwl_compose_forget(server, 0U, 1U << (unsigned)(head - compose->heads));
	heads_targets_destroy(compose, head);
	if (head->acquired != VK_NULL_HANDLE)
		vkDestroySemaphore(compose->device, head->acquired, NULL);

	/* What is left, and the head forgotten. */
	rest->output = head->output;
	rest->display = head->display;
	memcpy(rest->name, head->name, sizeof(rest->name));
	memset(head, 0, sizeof(*head));
}

/*
 * Destroys heads' swapchains and surfaces and gives their displays back,
 * once no frame and no present of them may still run (the device idle).
 * It touches no field of the compositor's, so the display thread may call
 * it (display-thread.c).
 */
void
kwl_heads_rest_destroy(
	struct kwl_server *server,
	struct kwl_head_rest *rests,
	unsigned count)
{
	struct kwl_compose *compose;
	unsigned index;

	/* Each, its display given back once its swapchain is gone. */
	compose = server->compose;
	for (index = 0U; index < count; index++) {
		vkdemo_display_close(compose->instance, compose->device, &rests[index].output);
		kwl_os_display_release(server, compose->physical, rests[index].display);
	}
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
	struct kwl_list *command,
	VkImage source,
	VkImageLayout layout,
	struct kwl_head *head)
{
	struct kwl_compose *compose;
	struct kwl_display_rect fitted;
	VkClearColorValue black;
	VkImageSubresourceRange range;
	VkImageBlit blit;
	VkImage target;
	VkPipelineStageFlags written;
	unsigned copyable;

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
	kwl_list_clear_color_image(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1U, &range);

	/*
	 * Whether the desktop can be copied: the swapchain's image when the
	 * display allows it, a turned output's turn image always (it is made
	 * to be copied from, WS217; said once).
	 */
	copyable = compose->readback;
	written = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	if (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		copyable = 1U;
		written = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		if (!compose->mirror_turn_logged) {
			printf("KWL OUTPUT mirror source=turn\n");
			compose->mirror_turn_logged = 1U;
		}
	}

	/* The desktop fitted whole, when the output's image can be copied. */
	if (copyable) {
		/* The output's image, from presentation (or the turn pass's sampling) to the copy's source. */
		heads_barrier(command, source, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    VK_ACCESS_TRANSFER_READ_BIT, written, VK_PIPELINE_STAGE_TRANSFER_BIT);
		heads_barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		/* The whole of it into the fitted rectangle. */
		kwl_displays_fit(compose->target_width, compose->target_height, head->output.width, head->output.height, &fitted);
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1U;
		blit.srcOffsets[1].x = (int32_t)compose->target_width;
		blit.srcOffsets[1].y = (int32_t)compose->target_height;
		blit.srcOffsets[1].z = 1;
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1U;
		blit.dstOffsets[0].x = fitted.x;
		blit.dstOffsets[0].y = fitted.y;
		blit.dstOffsets[1].x = fitted.x + (int32_t)fitted.width;
		blit.dstOffsets[1].y = fitted.y + (int32_t)fitted.height;
		blit.dstOffsets[1].z = 1;
		kwl_list_blit_image(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &blit, VK_FILTER_LINEAR);

		/* The output's image back to presentation, or the turn image to the next frame's passes and copies. */
		if (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
			heads_barrier(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_ACCESS_TRANSFER_READ_BIT,
			    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
		} else {
			heads_barrier(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_ACCESS_TRANSFER_READ_BIT, 0U,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
		}
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
	struct kwl_list *command,
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
	pass.renderPass = kwl_compose_target_pass(compose, (unsigned)(head - compose->heads) + 1U, 0U);
	pass.framebuffer = head->framebuffers[head->image];
	pass.renderArea.extent.width = head->output.width;
	pass.renderArea.extent.height = head->output.height;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	kwl_list_begin_render_pass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	kwl_compose_bind_forget(server->compose);

	/* The whole head. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)head->output.width;
	viewport.height = (float)head->output.height;
	viewport.maxDepth = 1.0f;
	kwl_list_set_viewport(command, 0U, 1U, &viewport);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = head->output.width;
	scissor.extent.height = head->output.height;
	kwl_list_set_scissor(command, 0U, 1U, &scissor);
	compose->scissor_now = scissor;
	offset = 0U;
	kwl_list_bind_vertex_buffers(command, 0U, 1U, &compose->corners, &offset);

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
	kwl_batch_flush(server, command);
	kwl_list_end_render_pass(command);
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
	struct kwl_list *command,
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
	kwl_list_pipeline_barrier(command, source_stage, destination_stage, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
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
 * Makes a writing of displays.conf ready on the main thread: the choice's
 * text, the file's path and its folder's.  Returns 0, ENOSPC for a text
 * longer than the file may be, or the error of the paths.
 */
static int
heads_write_ready(
	const struct kwl_display_config *config,
	struct heads_write *write_job)
{
	int error;

	/* The text. */
	write_job->length = kwl_displays_format(config, write_job->text, sizeof(write_job->text));
	if (write_job->length == 0U)
		return ENOSPC;

	/* The file's path. */
	error = heads_path(write_job->path, sizeof(write_job->path), 0);
	if (error != 0)
		return error;

	/* Its folder's. */
	error = heads_path(write_job->folder, sizeof(write_job->folder), 1);
	if (error != 0)
		return error;

	/* Succeeded: the writing is ready. */
	return 0;
}

/*
 * Writes displays.conf: a new file beside it, flushed, then renamed over
 * it (D-STORE), in the folder made when it is missing (made the user's
 * alone: a new home has no .config).  It touches nothing but the writing
 * given, so the file I/O thread runs it.  Returns 0 or an errno value;
 * a failure leaves the file as it was.
 */
static int
heads_write_file(
	struct heads_write *write_job)
{
	char temporary[HEADS_PATH_MAX + 8U];
	const char *text;
	size_t length;
	size_t done;
	ssize_t count;
	int descriptor;
	int status;
	int error;

	/* The folder and those above it. */
	heads_mkdir(write_job->folder);
	text = write_job->text;
	length = write_job->length;

	/* A new file beside the file. */
	(void)snprintf(temporary, sizeof(temporary), "%s.XXXXXX", write_job->path);
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
	status = rename(temporary, write_job->path);
	if (status != 0) {
		error = errno;
		(void)unlink(temporary);
		return error;
	}

	/* Succeeded: the file keeps the choice. */
	return 0;
}

/* Writes displays.conf on the file I/O thread, timed for the log. */
static void
heads_file_run(
	struct kwl_lane_job *lane_job)
{
	struct heads_file_job *job;

	/* The writing, and when it started and ended. */
	job = (struct heads_file_job *)lane_job;
	job->started_ms = kwl_milliseconds();
	job->error = heads_write_file(&job->write);
	job->ended_ms = kwl_milliseconds();
}

/*
 * Takes a writing of displays.conf that ran, on the event loop's thread:
 * the choices given up to its ticket are written (or failed to be), and
 * the requests that wait for them are answered.
 */
static void
heads_file_done(
	void *context,
	struct kwl_lane_job *lane_job)
{
	struct kwl_server *server;
	struct heads_file_job *job;

	/* How it went, logged with how long it waited and took. */
	server = context;
	job = (struct heads_file_job *)lane_job;
	printf(
	    "KWL DISPLAYS saved by=io-file ticket=%llu errno=%d wait_ms=%llu ms=%llu\n",
	    (unsigned long long)job->ticket,
	    job->error,
	    (unsigned long long)(job->started_ms - job->given_ms),
	    (unsigned long long)(job->ended_ms - job->started_ms));

	/* The file holds every choice up to its ticket. */
	heads_saving.written = job->ticket;
	heads_saving.error = job->error;

	/* The requests that waited for it are answered. */
	kwl_displays_written(server, job->ticket, job->error);

	/* The record. */
	free(job);
}

/* Frees a writing that did not run: a newer one, which holds its choice too, replaced it, or the thread stopped. */
static void
heads_file_drop(
	struct kwl_lane_job *lane_job)
{
	/* The record; the newer writing answers its requests. */
	free(lane_job);
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
 * Gives what the pointer's place is computed over (WS217): the outputs'
 * logical rectangles, each one's panel and the angle shown on it, and the
 * output the absolute pointer maps onto (the anchor until devices are
 * bound to displays, ws217-p006).  A head is not turned yet (ws217-p005):
 * its panel is its logical size.
 */
void
kwl_output_geometry(
	struct kwl_server *server,
	struct kwl_cursor_geometry *geometry)
{
	unsigned slot;

	/* The rectangles; the absolute pointer on the anchor. */
	memset(geometry, 0, sizeof(*geometry));
	geometry->output_count = kwl_outputs(server, geometry->outputs);
	geometry->absolute_slot = KWL_PLANE_ANCHOR;

	/* Each output's panel: its logical size turned back by its angle. */
	for (slot = 0U; slot < KWL_PLANE_SLOTS; slot++) {
		geometry->physical[slot].rotation = kwl_output_rotation(server, slot);
		kwl_rotate_size(geometry->physical[slot].rotation,
				geometry->outputs[slot].width,
				geometry->outputs[slot].height,
				&geometry->physical[slot].width,
				&geometry->physical[slot].height);
	}
}

/*
 * Gives the angle an output shows its picture at (WS217): the anchor's
 * angle shown (0 while a game pauses the turn), 0 for a head (ws217-p005).
 */
unsigned
kwl_output_rotation(
	struct kwl_server *server,
	unsigned slot)
{
	/* A head, or no device open yet. */
	if (slot != KWL_PLANE_ANCHOR || server->compose == NULL)
		return KWL_ROTATE_0;

	/* Succeeded: the anchor's angle. */
	return server->compose->rotation;
}

/*
 * Maps an absolute device's report onto an output (WS217, plan/ws217/
 * phase001 6.1), in 24.8 fixed point of the plane: a direct device's
 * place on the output's panel turned back by its angle, any other's on
 * its logical picture as it is.
 */
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

	/* The outputs as they are now. */
	kwl_output_geometry(server, &geometry);

	/* The place, in 1/256 pixels. */
	kwl_cursor_geometry_place(&geometry, slot, direct, 256U, across, down, x, y);
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
	struct kwl_plane_rect now[KWL_PLANE_SLOTS];
	struct heads_pointer_carry carry;
	struct kwl_client *client;
	struct kwl_object *object;
	const struct kwl_head *head;
	unsigned carried;
	unsigned count;
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

	/*
	 * The pointer on it comes to the same share of the anchor.  Where it is
	 * is the cursor's box's place, which the input thread may have moved
	 * past the main thread's view; the outputs as they are now go to the
	 * box in the same warp (ws213-p004, the design's 6.1 N-3).
	 */
	carry.slot = slot;
	carry.from = outputs[slot];
	carry.carried = &carried;
	carried = 0U;
	(void)kwl_outputs(server, now);
	kwl_pointer_warp(server, now, heads_pointer_evacuate, &carry);
	if (!carried)
		return;
	server->dirty = 1;
	printf("KWL POINTER output=0 x=%d y=%d why=retreat\n", server->pointer_x, server->pointer_y);
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
	struct heads_pointer_shift follow;
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

	/*
	 * The pointer on a head: along with it, or to the anchor, from where the
	 * cursor's box has it, with the outputs as they are now given to the box
	 * in the same warp (ws213-p004, the design's 6.1 N-3).
	 */
	follow.before = before;
	follow.after = after;
	follow.leave = leave;
	kwl_pointer_warp(server, after, heads_pointer_follow, &follow);
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
	*saved = kwl_heads_config_save(server);
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
 * Gives the desktop to a display on when the anchor's display is turned off,
 * as kwl_displays_anchor_follow does, in a change of the display while the
 * passes go on (the hotplug's looks): the displays on are tried in order.
 * Returns 1 when the move began, 0 when none was needed or could begin.
 */
int
kwl_displays_anchor_follow_later(
	struct kwl_server *server)
{
	VkDisplayKHR candidates[KWL_COMPOSE_DISPLAYS];
	struct kwl_compose *compose;
	unsigned count;
	unsigned index;
	int anchor;
	int off;
	int error;

	/* An open output whose display is turned off. */
	compose = server->compose;
	if (compose == NULL || !compose->output_open || compose->output_lost)
		return 0;
	anchor = heads_index(compose, compose->display);
	if (anchor < 0)
		return 0;
	off = heads_off(compose, (unsigned)anchor);
	if (!off)
		return 0;

	/* The displays on that may take the output, in order. */
	count = 0U;
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
		if (count == 0U)
			printf("KWL DISPLAYS anchor off name=%s to=%s\n", compose->display_name, compose->display_names[index]);
		candidates[count] = compose->displays[index];
		count++;
	}

	/* The move. */
	if (count == 0U)
		return 0;
	error = kwl_output_move(server, candidates, NULL, count, NULL, NULL);
	if (error != 0)
		return 0;

	/* Succeeded: the move runs. */
	return 1;
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
	saved = kwl_heads_config_save(server);
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

/* Carries the pointer from an output going away to the same share of the anchor, when the box has it there (a warp's move). */
static void
heads_pointer_evacuate(
	const void *context,
	const struct kwl_plane_rect *outputs,
	int32_t *x,
	int32_t *y,
	unsigned *output)
{
	const struct heads_pointer_carry *carry;

	/* Only a pointer on that output. */
	carry = context;
	if (*output != carry->slot)
		return;

	/* The same share of the anchor. */
	kwl_plane_carry(&carry->from, &outputs[KWL_PLANE_ANCHOR], *x, *y, 1U, 1U, 0, x, y);
	*output = KWL_PLANE_ANCHOR;
	*carry->carried = 1U;
}

/* Moves the pointer on a head along with it, or to the anchor when the head is not shown now (a warp's move). */
static void
heads_pointer_follow(
	const void *context,
	const struct kwl_plane_rect *outputs,
	int32_t *x,
	int32_t *y,
	unsigned *output)
{
	const struct heads_pointer_shift *follow;
	unsigned slot;

	/* Only a pointer on a head. */
	(void)outputs;
	follow = context;
	slot = *output;
	if (slot == KWL_PLANE_ANCHOR || slot >= KWL_PLANE_SLOTS)
		return;

	/* To the anchor when the head is not shown now (or the mirror is chosen). */
	if (follow->leave || follow->after[slot].width == 0U || follow->before[slot].width == 0U) {
		kwl_plane_carry(&follow->before[slot], &follow->after[KWL_PLANE_ANCHOR], *x, *y, 1U, 1U, 0, x, y);
		*output = KWL_PLANE_ANCHOR;
		return;
	}

	/* Along with its head. */
	*x += follow->after[slot].x - follow->before[slot].x;
	*y += follow->after[slot].y - follow->before[slot].y;
}
