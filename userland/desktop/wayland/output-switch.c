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
#include <stdlib.h>
#include <string.h>

/* How often the hotplug fence is looked at (ms): each look is a request to the display node. */
#define OUTPUT_HOTPLUG_MS	250U

/* A move to an external display (kwl_output_use_external): what was kept off before, and what goes on with the result. */
struct output_use {
	VkDisplayKHR previous;
	kwl_display_fn *then;
	void *context;
};

/* The looks at the displays the hotplug's tick makes (output_listed). */
#define OUTPUT_LOOK_FIRST	1U
#define OUTPUT_LOOK_SIGNAL	2U
#define OUTPUT_LOOK_HEADS	3U
#define OUTPUT_LOOK_RECOVER	4U

/* A look at the displays: the list the display thread reads, and what the look goes on with. */
struct output_list {
	struct kwl_display_list list;
	unsigned look;
};

/* The most displays one move tries: the one shown, the machine's own, and every one connected. */
#define OUTPUT_MOVE_MAX		(KWL_COMPOSE_DISPLAYS + 2U)

/*
 * A move of the output (kwl_output_move): the displays to try in order
 * (candidates, count of them, the next to try), for each whether nothing
 * is kept off from its try on, what goes on with the result (then and
 * context), the last try's result (error); and the try running: its
 * display (target), size, refresh and name, what the output had to go
 * back to, and whether it is the same display opened again.
 */
struct output_move {
	VkDisplayKHR candidates[OUTPUT_MOVE_MAX];
	unsigned char clear_kept[OUTPUT_MOVE_MAX];
	unsigned count;
	unsigned next;
	kwl_display_fn *then;
	void *context;
	int error;
	VkDisplayKHR target;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	char name[KWL_COMPOSE_NAME];
	VkDisplayKHR old_display;
	uint32_t old_mode_width;
	uint32_t old_mode_height;
	uint32_t old_refresh;
	char old_name[KWL_COMPOSE_NAME];
	int reopen;
};

/*
 * A turn of the anchor on the same panel, for the pointer's move to the
 * same physical point (kwl_output_pointer_turn): the angles before and
 * after, the panel's size, and where the pointer was and went (for the
 * log; -1 while it is not on the anchor).
 */
struct output_turn {
	unsigned before;
	unsigned after;
	uint32_t physical_width;
	uint32_t physical_height;
	int32_t *seen;
};

static VkResult output_list_apply(struct kwl_server *server, const struct kwl_display_list *list);
static void output_list_later(struct kwl_server *server, unsigned look);
static void output_listed(struct kwl_server *server, int error, void *context);
static void output_recover_listed(struct kwl_server *server, VkResult listed);
static int output_internal(const struct kwl_compose *compose, unsigned index);
static int output_index(const struct kwl_compose *compose, VkDisplayKHR display);
static void output_hotplug_register(struct kwl_server *server);
static void output_recover(struct kwl_server *server);
static void output_used_external(struct kwl_server *server, int error, void *context);
static void output_heads_sync(struct kwl_server *server);
static int output_fail_back(struct kwl_server *server);
static void output_failed_back(struct kwl_server *server, int error, void *context);
static void output_recover_rest(struct kwl_server *server);
static void output_recovered(struct kwl_server *server, int error, void *context);
static void output_move_parked(struct kwl_server *server, int error, void *context);
static void output_move_try(struct kwl_server *server, struct output_move *move);
static void output_move_closed(struct kwl_server *server, int error, void *context);
static void output_move_opened(struct kwl_server *server, int error, void *context);
static void output_move_back(struct kwl_server *server, int error, void *context);
static void output_move_end(struct kwl_server *server, struct output_move *move, int error);
static void output_sync_then(struct kwl_server *server, int error, void *context);
static void output_resized(struct kwl_server *server, unsigned glass_rebuild);
static void output_pointer_clamp(const void *context, const struct kwl_plane_rect *outputs, int32_t *x, int32_t *y, unsigned *output);
static void output_pointer_turn(const void *context, const struct kwl_plane_rect *outputs, int32_t *x, int32_t *y, unsigned *output);

/*
 * Follows the displays once each pass of the event loop (at most every
 * OUTPUT_HOTPLUG_MS): the hotplug fence's signal, and a display gone from
 * under the output.  The displays are read by the display thread (the
 * backend may probe the connectors); output_listed goes on with the list.
 */
void
kwl_output_tick(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	VkFence previous;
	VkResult status;
	uint64_t now;
	int lost;

	/* A displays result held back too long is answered (displays-shell.c, BUG-266). */
	now = kwl_milliseconds();
	kwl_displays_pending_tick(server, now);

	/* Only a device that follows the hotplug, while window mode runs, the display does not change and no list is being read. */
	compose = server->compose;
	if (compose == NULL || compose->register_device_event == NULL || !server->windowed)
		return;
	if (kwl_display_busy(server) || compose->listing)
		return;

	/*
	 * Not too often: the hotplug fence has no event of its own (it is asked
	 * for the displays' changes), so it is looked at on the clock; a lost
	 * output at once.
	 */
	if (compose->hotplug != VK_NULL_HANDLE && now - compose->hotplug_checked_ms < OUTPUT_HOTPLUG_MS && compose->output_lost != 1U) {
		kwl_due(server, compose->hotplug_checked_ms + OUTPUT_HOTPLUG_MS);
		return;
	}
	compose->hotplug_checked_ms = now;
	kwl_due(server, now + OUTPUT_HOTPLUG_MS);
	if (compose->output_lost == 1U)
		kwl_due_soon(server);

	/* The first look: a fence, then the displays as they are, and their heads. */
	if (compose->hotplug == VK_NULL_HANDLE) {
		output_hotplug_register(server);
		output_list_later(server, OUTPUT_LOOK_FIRST);
		return;
	}

	/* A signal: a new fence first, so that a change during the enumeration is not missed. */
	status = vkGetFenceStatus(compose->device, compose->hotplug);
	if (status == VK_SUCCESS) {
		previous = compose->hotplug;
		compose->hotplug = VK_NULL_HANDLE;
		output_hotplug_register(server);
		vkDestroyFence(compose->device, previous, NULL);

		/* The displays now; a display refused before, or whose move failed, is tried again. */
		compose->limited = 0U;
		compose->move_failed = 0U;
		output_list_later(server, OUTPUT_LOOK_SIGNAL);
		return;
	}

	/* The display under the output is gone: the output moves, while the passes go on. */
	if (compose->output_lost == 1U) {
		output_recover(server);
		return;
	}

	/*
	 * The heads brought in line after the output opened again (the list is
	 * read again first), and when a head lost its display.
	 */
	lost = kwl_heads_lost(server);
	if (!compose->heads_stale && !lost)
		return;
	output_list_later(server, OUTPUT_LOOK_HEADS);
}

/*
 * Reads the displays on the display thread for a look (OUTPUT_LOOK_*);
 * output_listed goes on.  Without memory the look is tried again later.
 */
static void
output_list_later(
	struct kwl_server *server,
	unsigned look)
{
	struct output_list *list;

	/* The record, then the reading. */
	list = calloc(1, sizeof(*list));
	if (list == NULL)
		return;
	list->look = look;
	server->compose->listing = 1U;
	kwl_display_enumerate(server, &list->list, output_listed, list);
}

/*
 * Goes on with a look once the displays are read: the list taken in, then
 * by the look -- after a signal, the output lost when its display is gone
 * (and moved); a lost output's move; else an anchor turned off moved, or
 * the heads brought in line.
 */
static void
output_listed(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kwl_compose *compose;
	struct output_list *list;
	VkResult listed;
	unsigned look;
	int moving;
	int index;

	/* The list taken in. */
	(void)error;
	list = context;
	compose = server->compose;
	compose->listing = 0U;
	listed = output_list_apply(server, &list->list);
	look = list->look;
	free(list);

	/* A lost output's move, from the list read now. */
	if (look == OUTPUT_LOOK_RECOVER) {
		output_recover_listed(server, listed);
		return;
	}

	/* After a signal. */
	if (look == OUTPUT_LOOK_SIGNAL) {
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

		/* The display under the output is gone: the output moves (the heads follow its opening, heads_stale). */
		if (compose->output_lost == 1U) {
			output_recover_listed(server, listed);
			return;
		}
	}

	/*
	 * An anchor turned off by the user gives the desktop to a display on
	 * (the heads follow the output's opening, heads_stale), else
	 * the heads are brought in line.
	 */
	moving = kwl_displays_anchor_follow_later(server);
	if (moving)
		return;
	output_heads_sync(server);
}

/*
 * Brings the heads in line in a change of the display while the passes go
 * on (their swapchains made and destroyed by the display thread); one that
 * cannot begin now is tried again at a later look.
 */
static void
output_heads_sync(
	struct kwl_server *server)
{
	int error;

	/* The change, or a later look. */
	error = kwl_heads_sync_later(server);
	if (error != 0)
		server->compose->heads_stale = 1U;
}

/*
 * Moves the output to another display and waits for the move (where the
 * caller needs its result on return: the lid, Settings' anchor turned off):
 * the move's steps run on the event loop's thread (kwl_output_move).
 * Returns 0, ENODEV before window mode, ENXIO for a display not connected
 * now, or EAGAIN when its swapchain was refused (the output is back on the
 * display it had, when it could be).
 */
int
kwl_output_switch(
	struct kwl_server *server,
	VkDisplayKHR target)
{
	struct kwl_compose *compose;
	int result;
	int error;

	/* Only window mode's output moves. */
	compose = server->compose;
	if (compose == NULL || !server->windowed)
		return ENODEV;

	/* A change running ends first; this one's steps run here. */
	kwl_display_settle(server);
	result = EINPROGRESS;
	compose->display_inline++;
	error = kwl_output_move(server, &target, NULL, 1U, output_sync_then, &result);
	compose->display_inline--;
	if (error != 0)
		return error;

	/* The move's result. */
	return result;
}

/*
 * Moves the output to the first of some displays that takes it, in a
 * change of the display (display-thread.c) while the passes go on: for
 * each display in turn, the swapchain closed and the display given back, a
 * swapchain opened on the other display at its native size, and the
 * desktop fitted to it; a display whose swapchain is refused is marked
 * limited and the output goes back to the display it had, then the next is
 * tried.  An output closed (its display gone, and no other taken) is opened
 * on the display.  clear_kept (NULL for none) says for each display that
 * nothing is kept off from its try on (the machine's own display
 * again).  then(server, error, context) is called at the end with 0
 * (a display took the output, or shows it already), ENXIO (the last was not
 * connected) or EAGAIN (the last refused it).  Returns 0 when the move
 * began, ENODEV before window mode, EINVAL without a display, ENOMEM, or
 * EBUSY while another change of the display runs (the caller tries again
 * later).
 */
int
kwl_output_move(
	struct kwl_server *server,
	const VkDisplayKHR *candidates,
	const unsigned char *clear_kept,
	unsigned count,
	kwl_display_fn *then,
	void *context)
{
	struct kwl_compose *compose;
	struct output_move *move;
	unsigned index;
	int busy;
	int error;

	/* Only window mode's output moves, to some display. */
	compose = server->compose;
	if (compose == NULL || !server->windowed)
		return ENODEV;
	if (count == 0U)
		return EINVAL;
	busy = kwl_display_busy(server);
	if (busy)
		return EBUSY;

	/* The displays to try, in their order. */
	move = calloc(1, sizeof(*move));
	if (move == NULL)
		return ENOMEM;
	if (count > OUTPUT_MOVE_MAX)
		count = OUTPUT_MOVE_MAX;
	for (index = 0U; index < count; index++) {
		move->candidates[index] = candidates[index];
		if (clear_kept != NULL)
			move->clear_kept[index] = clear_kept[index];
	}

	/* Records how many displays are tried, who is told the end, and the error if none is connected. */
	move->count = count;
	move->then = then;
	move->context = context;
	move->error = ENXIO;

	/* The change, which goes on once the render thread is parked. */
	error = kwl_display_begin(server, output_move_parked, move);
	if (error != 0) {
		free(move);
		return error;
	}

	/* Succeeded: the move runs. */
	return 0;
}

/* A move, the render thread parked: the first display tried. */
static void
output_move_parked(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* The tries. */
	(void)error;
	output_move_try(server, context);
}

/*
 * Tries the next display of a move: one shown already ends it, one not
 * connected now is passed over, any other closes the output (the move
 * goes on at output_move_closed).  With none left the move ends with the
 * last try's result.
 */
static void
output_move_try(
	struct kwl_server *server,
	struct output_move *move)
{
	struct kwl_compose *compose;
	VkResult result;

	/* Each display left, in its order. */
	compose = server->compose;
	while (move->next < move->count) {
		move->target = move->candidates[move->next];
		if (move->clear_kept[move->next])
			compose->kept_off = VK_NULL_HANDLE;
		move->next++;

		/* The display it shows already. */
		if (move->target == compose->display && compose->output_open && !compose->output_lost) {
			output_move_end(server, move, 0);
			return;
		}

		/* The other display's size, refresh and name; one not connected now cannot take the output. */
		result = kwl_compose_display_read(server, move->target, &move->width, &move->height, &move->refresh, move->name, sizeof(move->name));
		if (result != VK_SUCCESS) {
			printf("KWL OUTPUT switch refused name=%s result=%d\n", move->name, (int)result);
			move->error = ENXIO;
			continue;
		}

		/*
		 * What the output had, to go back to: the display, its mode and
		 * refresh (the angles it showed are compose->opened).
		 */
		move->old_display = compose->display;
		move->old_mode_width = compose->mode_width;
		move->old_mode_height = compose->mode_height;
		move->old_refresh = server->refresh;
		memcpy(move->old_name, compose->display_name, sizeof(move->old_name));

		/* The same display opened again (another generation of it, or back after a wait) is not a move. */
		move->reopen = 0;
		if (move->target == move->old_display)
			move->reopen = 1;

		/* The swapchain closed and the display given back. */
		kwl_display_close(server, output_move_closed, move);
		return;
	}

	/* None took the output. */
	output_move_end(server, move, move->error);
}

/*
 * A move's try, the output closed: the other display, at its native size
 * (unrotated until the output's opening chooses the angle displays.conf
 * keeps for its key, kwl_compose_output_prepare), is opened.
 */
static void
output_move_closed(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kwl_compose *compose;
	struct output_move *move;

	/* The other display. */
	(void)error;
	move = context;
	compose = server->compose;
	compose->output_lost = 0U;
	compose->display = move->target;
	memcpy(compose->display_name, move->name, sizeof(compose->display_name));
	kwl_output_size_set(server, move->width, move->height, KWL_ROTATE_0);
	server->refresh = move->refresh;
	kwl_display_open(server, output_move_opened, move);
}

/*
 * A move's try, the other display opened (or refused): refused (the limit
 * of outputs shown at once), the display is marked and the output goes
 * back; taken, the move ends.
 */
static void
output_move_opened(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kwl_compose *compose;
	struct output_move *move;
	uint64_t started;
	int index;

	/* Refused: marked, and the output goes back to what it had. */
	move = context;
	compose = server->compose;
	if (error != 0) {
		index = output_index(compose, move->target);
		if (index >= 0)
			compose->limited |= (uint32_t)1U << (unsigned)index;
		printf("KWL OUTPUT switch failed name=%s errno=%d\n", move->name, error);
		compose->display = move->old_display;
		memcpy(compose->display_name, move->old_name, sizeof(compose->display_name));
		kwl_output_size_set(server, move->old_mode_width, move->old_mode_height, compose->opened.rotation);
		compose->rotation_selected = compose->opened.selected;
		compose->scanout_rotation = compose->opened.scanout;
		compose->turn_rotation = compose->opened.turn;
		server->refresh = move->old_refresh;
		kwl_display_open(server, output_move_back, move);
		return;
	}

	/* The desktop follows what the output shows now against what it showed when it last opened. */
	started = kwl_milliseconds();
	(void)kwl_output_rotated(server);
	printf("KWL DISPLAY step=fit ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/*
	 * A move is proven by its first frame (BUG-266): until it is
	 * presented, the display it left is kept to go back to.  The same
	 * display opened again proves nothing new.
	 */
	compose->switch_failed = 0U;
	if (!move->reopen) {
		compose->switch_proving = 1U;
		compose->switch_from = move->old_display;
		memcpy(compose->switch_from_name, move->old_name, sizeof(compose->switch_from_name));
	}

	/*
	 * The opening fitted the desktop to the logical size and the angle the
	 * output has now and told the clients of a new size (WS217); the same
	 * size on another display (its refresh, its name) is told here, so
	 * that a move always tells them (review-4 L44).
	 */
	if ((compose->opened_steps & KWL_ROTATE_STEP_RESIZED) == 0U)
		kwl_outputs_changed(server);

	/* The log tells a move from the same display opened again (the tests read it). */
	if (move->reopen) {
		printf("KWL OUTPUT reopen name=%s width=%u height=%u refresh_mhz=%u\n", compose->display_name, move->width, move->height, move->refresh);
	} else {
		printf("KWL OUTPUT switch name=%s width=%u height=%u refresh_mhz=%u\n", compose->display_name, move->width, move->height, move->refresh);
	}

	/* Succeeded: the output shows the target display. */
	output_move_end(server, move, 0);
}

/* A refused try, the output opened back on the display it had (or not): the next display is tried. */
static void
output_move_back(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct output_move *move;

	/* Back, the desktop following what it shows; neither display: the output waits for the next hotplug. */
	move = context;
	if (error == 0) {
		(void)kwl_output_rotated(server);
	} else {
		printf("KWL OUTPUT none: the display it had did not open again errno=%d\n", error);
		server->compose->output_lost = 2U;
	}

	/* The try was refused; the next one. */
	move->error = EAGAIN;
	output_move_try(server, move);
}

/* Ends a move: the change ends, then its result is told. */
static void
output_move_end(
	struct kwl_server *server,
	struct output_move *move,
	int error)
{
	kwl_display_fn *then;
	void *context;

	/* The record goes first. */
	then = move->then;
	context = move->context;
	free(move);

	/* The change ends, then the result. */
	kwl_display_end(server);
	if (then != NULL)
		then(server, error, context);
}

/* Keeps the result of a move whose steps ran here. */
static void
output_sync_then(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* The result for the caller. */
	(void)server;
	*(int *)context = error;
}

/*
 * Sets the output's display mode and the angle it shows, and the desktop's
 * logical size, the mode turned by the angle: the one place that writes
 * server->width and height (WS217).  Before the compositor's device is
 * open (no compose yet) it sets the logical size alone, from the size
 * asked for.
 */
void
kwl_output_size_set(
	struct kwl_server *server,
	uint32_t mode_width,
	uint32_t mode_height,
	unsigned rotation)
{
	struct kwl_compose *compose;
	uint32_t width;
	uint32_t height;

	/* The logical size: the mode on its side at a quarter turn. */
	kwl_rotate_size(rotation, mode_width, mode_height, &width, &height);

	/* The output's mode and angle, once its device is open. */
	compose = server->compose;
	if (compose != NULL) {
		compose->mode_width = mode_width;
		compose->mode_height = mode_height;
		compose->rotation = rotation;
	}

	/* The desktop's size, which the windows, the plane and wl_output take. */
	server->width = width;
	server->height = height;
}

/*
 * Fits the desktop to the output's logical size after it changed with an
 * angle (output-rotate.c, WS217): the windows, the pointer and, with
 * glass_rebuild, the look's images.
 */
void
kwl_output_fit(
	struct kwl_server *server,
	unsigned glass_rebuild)
{
	/* The same fitting as an output opened at another size. */
	output_resized(server, glass_rebuild);
}

/*
 * Names the steps of an output's change for the log ("turn|rotated|resized"),
 * or "none" (WS217).
 */
void
kwl_output_steps_name(
	unsigned steps,
	char *text,
	size_t size)
{
	static const char *const names[] = { "reopen", "turn", "rotated", "resized", "save" };
	const char *bar;
	size_t used;
	unsigned index;
	int written;

	/* Each step taken, joined by bars. */
	text[0] = '\0';
	used = 0U;
	for (index = 0U; index < sizeof(names) / sizeof(names[0]); index++) {
		/* A step not taken has no name in the log. */
		if ((steps & (1U << index)) == 0U)
			continue;

		/* A bar before every name but the first. */
		bar = "|";
		if (used == 0U)
			bar = "";
		written = snprintf(text + used, size - used, "%s%s", bar, names[index]);
		if (written < 0 || (size_t)written >= size - used)
			return;
		used += (size_t)written;
	}

	/* No step at all. */
	if (used == 0U)
		(void)snprintf(text, size, "none");
}

/*
 * Moves the pointer on the anchor to the same physical point of its panel
 * after the angle shown changed from before to after (WS217, review S1):
 * its logical place at the old angle is turned to the panel and back at
 * the new one, inside the new size.  seen gets where it was and went
 * (x, y, x', y'; -1 when it is not on the anchor).
 */
void
kwl_output_pointer_turn(
	struct kwl_server *server,
	unsigned before,
	unsigned after,
	int32_t *seen)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	struct output_turn turn;

	/* The angles and the panel; the pointer's places not known yet. */
	memset(&turn, 0, sizeof(turn));
	turn.before = before;
	turn.after = after;
	turn.physical_width = server->compose->mode_width;
	turn.physical_height = server->compose->mode_height;
	turn.seen = seen;
	seen[0] = -1;
	seen[1] = -1;
	seen[2] = -1;
	seen[3] = -1;

	/* The outputs at the new size, and the pointer moved with them. */
	(void)kwl_outputs(server, outputs);
	kwl_pointer_warp(server, outputs, output_pointer_turn, &turn);
}

/*
 * Gives what the output shows now (WS217): its logical size, the angle
 * chosen and the angle shown, and how it is shown.  Without the device
 * open, only the size.
 */
void
kwl_output_state(
	struct kwl_server *server,
	struct kwl_rotate_state *state)
{
	struct kwl_compose *compose;

	/* The logical size. */
	memset(state, 0, sizeof(*state));
	state->width = server->width;
	state->height = server->height;

	/* The angles, once the device is open. */
	compose = server->compose;
	if (compose == NULL)
		return;
	state->selected = compose->rotation_selected;
	state->rotation = compose->rotation;
	state->scanout = compose->scanout_rotation;
	state->turn = compose->turn_rotation;
}

/*
 * Takes the steps of an output opened (again, or on another display) from
 * what it showed when it last opened or turned (WS217, design 4.6 B and
 * review-4 C7): the pair of the logical size and the angle shown is
 * compared, the desktop fitted to a new size and the clients told, and a
 * new angle drawn again everywhere and logged (the input's mapping follows
 * it from ws217-p003, Settings from p004).  The opening made the surface
 * and the turn's images itself.  What the output shows is kept for the
 * next opening.  Returns the steps (KWL_ROTATE_STEP_*).
 */
unsigned
kwl_output_rotated(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	struct kwl_rotate_state after;
	char names[64];
	int32_t pointer[4];
	unsigned steps;
	unsigned before_rotation;
	int same_panel;

	/* What the output shows now, against what it showed when it last opened, and whether on the same panel. */
	compose = server->compose;
	kwl_output_state(server, &after);
	steps = kwl_rotate_steps(&compose->opened, &after, KWL_ROTATE_MODE_OPEN);
	before_rotation = compose->opened.rotation;
	same_panel = 0;
	if (compose->opened_display == compose->display)
		same_panel = 1;
	compose->opened = after;
	compose->opened_display = compose->display;
	compose->opened_steps = steps;

	/*
	 * The drag and drop, the grabs, the fingers and the pens end what they
	 * were doing at the old angle first, without moving the pointer
	 * (ws217-p003 review S1): the turn below keeps it on its physical point.
	 */
	if ((steps & KWL_ROTATE_STEP_ROTATED) != 0U)
		kwl_input_rotated(server);

	/*
	 * Another angle on the same panel keeps the pointer on its physical
	 * point, before the fitting below would keep it inside the new size
	 * (review S1); on another panel the fitting's clamp alone.
	 */
	pointer[0] = -1;
	pointer[1] = -1;
	pointer[2] = -1;
	pointer[3] = -1;
	if ((steps & KWL_ROTATE_STEP_ROTATED) != 0U && same_panel)
		kwl_output_pointer_turn(server, before_rotation, after.rotation, pointer);

	/* The desktop fitted to another logical size (the look's images, the windows, the pointer), and the clients told. */
	if ((steps & KWL_ROTATE_STEP_RESIZED) != 0U) {
		output_resized(server, 1U);
		kwl_outputs_changed(server);
	}

	/* The input thread computes the motions over what the output shows now. */
	kwl_input_geometry(server);

	/* Another angle moves every point of the output: everything is drawn again, and the tests read it. */
	if ((steps & KWL_ROTATE_STEP_ROTATED) != 0U) {
		server->dirty = 1;
		kwl_output_steps_name(steps, names, sizeof(names));
		printf("KWL OUTPUT rotate key=%s rotation=%u selected=%u scanout=%u turn=%u width=%u height=%u mode=%ux%u steps=%s by=open "
		    "pointer=%d,%d>%d,%d\n",
		    compose->display_name, after.rotation, after.selected, after.scanout, after.turn, after.width, after.height,
		    compose->mode_width, compose->mode_height, names, pointer[0], pointer[1], pointer[2], pointer[3]);
	}

	/* Succeeded: the steps taken. */
	return steps;
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
 * Moves the output to the first external display that takes it, in a
 * change of the display while the passes go on; the machine's own display
 * left (under the closed lid) is kept off, no head for it,
 * unless no display takes the output.  then(server, error, context) is
 * called at the end with 0 or the last try's error.  Returns 0 when the
 * move began, ENODEV without the hotplug's list, ENOENT when no external
 * display is connected, or kwl_output_move's error.
 */
int
kwl_output_use_external(
	struct kwl_server *server,
	kwl_display_fn *then,
	void *context)
{
	VkDisplayKHR candidates[OUTPUT_MOVE_MAX];
	struct kwl_compose *compose;
	struct output_use *use;
	unsigned count;
	unsigned index;
	int internal;
	int shown;
	int error;

	/* Without the hotplug there is no list. */
	compose = server->compose;
	if (compose == NULL)
		return ENODEV;

	/* Each external display not refused, and not one whose move failed (BUG-266). */
	count = 0U;
	for (index = 0U; index < compose->display_count; index++) {
		internal = output_internal(compose, index);
		if (internal)
			continue;
		if ((compose->limited & ((uint32_t)1U << index)) != 0U)
			continue;
		if ((compose->move_failed & ((uint32_t)1U << index)) != 0U)
			continue;
		candidates[count] = compose->displays[index];
		count++;
	}

	/* Refuses when no external display is left to try. */
	if (count == 0U)
		return ENOENT;

	/* The machine's own display left is kept off; a move that fails keeps off what was kept off before. */
	use = calloc(1, sizeof(*use));
	if (use == NULL)
		return ENOMEM;
	use->previous = compose->kept_off;
	use->then = then;
	use->context = context;
	shown = output_index(compose, compose->display);
	if (shown >= 0) {
		internal = output_internal(compose, (unsigned)shown);
		if (internal)
			compose->kept_off = compose->display;
	}

	/* The move. */
	error = kwl_output_move(server, candidates, NULL, count, output_used_external, use);
	if (error != 0) {
		compose->kept_off = use->previous;
		free(use);
		return error;
	}

	/* Succeeded: the move runs. */
	return 0;
}

/* The move to an external display ended: a failure keeps off what was kept off before; the result is told. */
static void
output_used_external(
	struct kwl_server *server,
	int error,
	void *context)
{
	kwl_display_fn *then;
	struct output_use *use;
	void *then_context;

	/* The record goes first. */
	use = context;
	if (error != 0)
		server->compose->kept_off = use->previous;
	then = use->then;
	then_context = use->context;
	free(use);

	/* The result. */
	then(server, error, then_context);
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
 * Moves the output back to the machine's own display, in a change of the
 * display while the passes go on; nothing is kept off from now on: the
 * external display becomes a head again.  then(server,
 * error, context) is called at the end (0 also when it shows it already).
 * Returns 0 when the move began, ENODEV without the hotplug's list, ENOENT
 * when it is not connected, or kwl_output_move's error.
 */
int
kwl_output_use_internal(
	struct kwl_server *server,
	kwl_display_fn *then,
	void *context)
{
	struct kwl_compose *compose;
	VkDisplayKHR target;
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

	/* The move. */
	compose->kept_off = VK_NULL_HANDLE;
	target = compose->displays[index];
	error = kwl_output_move(server, &target, NULL, 1U, then, context);

	/* Reports why the move could not begin. */
	if (error != 0)
		return error;

	/* Succeeded: the move began. */
	return 0;
}

/*
 * Takes in the displays read (kwl_display_list_read) and logs them; an
 * unreadable list keeps the one read before.  Returns the read's result.
 */
static VkResult
output_list_apply(
	struct kwl_server *server,
	const struct kwl_display_list *list)
{
	struct kwl_compose *compose;
	unsigned index;
	int internal;
	int shown;

	/* Unreadable. */
	compose = server->compose;
	if (list->result != VK_SUCCESS) {
		printf("KWL OUTPUT displays unreadable result=%d\n", (int)list->result);
		return list->result;
	}

	/* Each one's handle and name. */
	compose->display_count = list->count;
	for (index = 0U; index < list->count; index++) {
		compose->displays[index] = list->displays[index];
		(void)snprintf(compose->display_names[index], sizeof(compose->display_names[index]), "%s", list->names[index]);
	}

	/* Each one in the log, whether it is the machine's own and whether it is shown (the tests read it). */
	for (index = 0U; index < list->count; index++) {
		internal = output_internal(compose, index);
		shown = 0;
		if (compose->displays[index] == compose->display)
			shown = 1;
		printf("KWL OUTPUT display index=%u name=%s internal=%d shown=%d width=%u height=%u\n",
		       index, compose->display_names[index], internal, shown, list->widths[index], list->heights[index]);
	}

	/* The count, which the tests read. */
	printf("KWL OUTPUT displays count=%u\n", list->count);

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
	/* The displays as they are now, read by the display thread. */
	output_list_later(server, OUTPUT_LOOK_RECOVER);
}

/*
 * Goes on moving a lost output with the displays read now.  An unreadable
 * list waits for the next hotplug: the topology moved under the
 * enumeration after the fence's registration, so the fence signals again.
 */
static void
output_recover_listed(
	struct kwl_server *server,
	VkResult listed)
{
	struct kwl_compose *compose;
	int started;

	/* Unreadable. */
	compose = server->compose;
	if (listed != VK_SUCCESS) {
		compose->output_lost = 2U;
		return;
	}

	/* A move whose first frame the display refused: back to the display it left (BUG-266), the rest after its failure. */
	if (compose->switch_failed) {
		started = output_fail_back(server);
		if (started)
			return;
	}

	/* The display it had, the machine's own, any other. */
	output_recover_rest(server);
}

/*
 * Moves the output from a lost display to the display it had when it is
 * still connected and its move did not fail, else to the machine's own
 * display (nothing kept off from then on), else to any other
 * connected one.  Without one, the output waits for the next hotplug.
 */
static void
output_recover_rest(
	struct kwl_server *server)
{
	VkDisplayKHR candidates[OUTPUT_MOVE_MAX];
	unsigned char clear_kept[OUTPUT_MOVE_MAX];
	struct kwl_compose *compose;
	unsigned count;
	unsigned index;
	int internal;
	int shown;
	int error;

	/* The display it had, when it is still connected and its move did not fail. */
	compose = server->compose;
	count = 0U;
	shown = output_index(compose, compose->display);
	if (shown >= 0 && (compose->move_failed & ((uint32_t)1U << (unsigned)shown)) == 0U) {
		candidates[count] = compose->display;
		clear_kept[count] = 0U;
		count++;
	}

	/* The machine's own display first. */
	for (index = 0U; index < compose->display_count; index++) {
		internal = output_internal(compose, index);
		if (!internal)
			continue;
		candidates[count] = compose->displays[index];
		clear_kept[count] = 1U;
		count++;
		break;
	}

	/* Any other display connected. */
	for (index = 0U; index < compose->display_count && count < OUTPUT_MOVE_MAX; index++) {
		candidates[count] = compose->displays[index];
		clear_kept[count] = 0U;
		count++;
	}

	/* The move, while the passes go on. */
	error = ENOENT;
	if (count > 0U)
		error = kwl_output_move(server, candidates, clear_kept, count, output_recovered, NULL);
	if (error != 0)
		output_recovered(server, error, NULL);
}

/* A lost output's move ended: without a display taking it, the output waits for the next hotplug. */
static void
output_recovered(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* Taken. */
	(void)context;
	if (error == 0)
		return;

	/* None. */
	server->compose->output_lost = 2U;
	printf("KWL OUTPUT waiting: no display takes the output errno=%d\n", error);
}

/*
 * Takes the output back to the display it left, after the display it moved
 * to refused the move's first frame (BUG-266): the failed display is not
 * moved to again until the next hotplug, the display left is on again, and
 * the clients hear the displays as they are (output_failed_back).  Returns
 * 1 when the move back began (the rest of the recovery follows its
 * failure), 0 when it could not (the display left is gone).
 */
static int
output_fail_back(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	char failed_name[KWL_COMPOSE_NAME];
	VkDisplayKHR back_display;
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
		return 0;
	}

	/* The move back. */
	printf("KWL OUTPUT switch failed name=%s back=%s\n", failed_name, compose->switch_from_name);
	back_display = compose->switch_from;
	error = kwl_output_move(server, &back_display, NULL, 1U, output_failed_back, NULL);
	if (error != 0)
		return 0;

	/* Succeeded: the move back runs. */
	return 1;
}

/*
 * The move back after a refused first frame ended: back, the display is on
 * again and the clients (Settings) hear the displays as they are; refused,
 * the rest of the recovery is tried.
 */
static void
output_failed_back(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kwl_compose *compose;

	/* Refused: the display it had, the machine's own, any other. */
	(void)context;
	compose = server->compose;
	if (error != 0) {
		output_recover_rest(server);
		return;
	}

	/* Going back is not a move to prove: the display showed the desktop before. */
	compose->switch_proving = 0U;

	/* The display it went back to is on again, and the clients (Settings) hear the displays as they are. */
	kwl_displays_move_failed(server, compose->display_name);
}

/*
 * Fits the desktop to an output of a new size: the look's images (unless
 * the caller made them at the new size already, WS217's rotation), the
 * windows, the pointer.
 */
static void
output_resized(
	struct kwl_server *server,
	unsigned glass_rebuild)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	int error;

	/* The look's output-sized images. */
	if (server->glass && glass_rebuild) {
		error = kwl_glass_resize(server);
		if (error != 0)
			printf("KWL OUTPUT resize glass errno=%d\n", error);
	}

	/* The windows. */
	kwl_glass_output_resized(server);

	/*
	 * The pointer inside the output, when it is on it (not on a head,
	 * ws113-p007): kept from where the cursor's box has it, the output's new
	 * size given to the box in the same warp (ws213-p004).
	 */
	(void)kwl_outputs(server, outputs);
	kwl_pointer_warp(server, outputs, output_pointer_clamp, NULL);

	/* Everything is drawn again. */
	server->dirty = 1;
	printf("KWL OUTPUT resized width=%u height=%u refresh_mhz=%u\n", server->width, server->height, server->refresh);
}

/* Keeps a pointer on the anchor inside its new size (a warp's move). */
static void
output_pointer_clamp(
	const void *context,
	const struct kwl_plane_rect *outputs,
	int32_t *x,
	int32_t *y,
	unsigned *output)
{
	/* A pointer on a head keeps its place. */
	(void)context;
	if (*output != KWL_PLANE_ANCHOR)
		return;

	/* The last column and row of the anchor at most. */
	if (*x >= (int32_t)outputs[KWL_PLANE_ANCHOR].width)
		*x = (int32_t)outputs[KWL_PLANE_ANCHOR].width - 1;
	if (*y >= (int32_t)outputs[KWL_PLANE_ANCHOR].height)
		*y = (int32_t)outputs[KWL_PLANE_ANCHOR].height - 1;
}

/* Turns a pointer on the anchor to the same physical point at the new angle (a warp's move). */
static void
output_pointer_turn(
	const void *context,
	const struct kwl_plane_rect *outputs,
	int32_t *x,
	int32_t *y,
	unsigned *output)
{
	const struct output_turn *turn;

	/* A pointer on a head keeps its place. */
	turn = context;
	if (*output != KWL_PLANE_ANCHOR)
		return;

	/* Where it was, the same point of the panel at the new angle, and where it went. */
	turn->seen[0] = *x;
	turn->seen[1] = *y;
	kwl_rotate_pointer_place(turn->before, turn->after, turn->physical_width, turn->physical_height, x, y);
	if (*x >= (int32_t)outputs[KWL_PLANE_ANCHOR].width)
		*x = (int32_t)outputs[KWL_PLANE_ANCHOR].width - 1;
	if (*y >= (int32_t)outputs[KWL_PLANE_ANCHOR].height)
		*y = (int32_t)outputs[KWL_PLANE_ANCHOR].height - 1;
	turn->seen[2] = *x;
	turn->seen[3] = *y;
}
