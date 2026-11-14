/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The front of a view of its own threads (view/front.h): the view's script
 * thread, the mailbox the program's thread queues its inputs and commands
 * in, the state, the notifications and the committed frames the script
 * thread publishes, and the program's and the drawing thread's side of
 * all of them.
 *
 * The mailbox is one FIFO under the front's lock: the program's thread
 * pushes, the script thread takes everything there is at once and runs
 * each entry, in order, as a task of the view's agent, then the view's own
 * round (the network's news, the timers, the images), then publishes.  A
 * pointer move right after a move replaces it, a wheel turn after a turn
 * adds to it, a size or a color scheme after the same replaces it;
 * nothing else is merged, dropped or moved.  At most FRONT_PRESSES_MAX
 * presses (a button or a key pressed, text) wait at once; a release, the
 * focus and the commands are always taken, so that nothing stays held.
 *
 * A navigation (a load, a link, the history) and the view's end ask the
 * task that runs to end (the page it runs in goes away) and wake the
 * script thread out of a request it waits for, before they are queued.
 */

#include "view/front.h"
#include "view/frames.h"
#include "view/remote.h"
#include "view/style.h"
#include "base/base.h"
#include "base/thread.h"
#include "net/net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most presses that wait in the mailbox at once (more are refused with ENOBUFS). */
#define FRONT_PRESSES_MAX	1024U

/* The most notifications that wait for the program at once (more console lines are counted, then told in one line). */
#define FRONT_NOTES_MAX		1024U

/* The stack of the script thread: the interpreter's recursion and the collector's scan of it. */
#define FRONT_STACK_64		(8U * 1024U * 1024U)
#define FRONT_STACK_32		(2U * 1024U * 1024U)

/* What an entry of the mailbox asks the script thread. */
enum front_kind {
	FRONT_LOAD,
	FRONT_FOLLOW,
	FRONT_GO,
	FRONT_STOP,
	FRONT_RESIZE,
	FRONT_SCHEME,
	FRONT_SCROLL_TO,
	FRONT_OVERSCROLL,
	FRONT_MOVE,
	FRONT_BUTTON,
	FRONT_LEAVE,
	FRONT_WHEEL,
	FRONT_KEY,
	FRONT_FOCUS,
	FRONT_FOCUS_FIELD,
	FRONT_COMPOSE,
	FRONT_COMMIT,
	FRONT_IMAGE
};

/* What a notification tells the program. */
enum front_note_kind {
	FRONT_NOTE_REDRAW,
	FRONT_NOTE_TITLE,
	FRONT_NOTE_COMMITTED,
	FRONT_NOTE_LOAD,
	FRONT_NOTE_CONSOLE,
	FRONT_NOTE_SCRIPT_ERROR,
	FRONT_NOTE_IMAGE,
	FRONT_NOTE_FIELD
};

/*
 * One entry of the mailbox: its kind and its arguments (which of them a
 * kind uses is the call's), with the strings the entry owns (NULL when
 * none).  checked says a commit carries the session the program saw.
 */
struct front_item {
	int kind;
	int number;
	int other;
	int pressed;
	int checked;
	uint32_t modifiers;
	uint32_t delete_before;
	uint32_t delete_after;
	unsigned width;
	unsigned height;
	double x;
	double y;
	double dx;
	double dy;
	uint64_t session;
	char *text;
	char *code;
	char *typed;
};

/*
 * One notification for the program's callbacks, which browser_view_process
 * calls on the program's thread: its kind and arguments, with the strings
 * and the image it owns (until it is told).  held says the notification
 * tells of a state the script thread has not published yet (a page
 * committed, a load, a title): it waits, with every notification after
 * it, until the next publish, so that the program's getters read that
 * state when its callback runs.  browser_view_process marks a note it put
 * back into the queue FRONT_NOTE_MOVED in its own copy.
 */
#define FRONT_NOTE_MOVED	2

struct front_note {
	int held;
	int kind;
	int state;
	int error;
	int level;
	char *text;
	size_t length;
	char *reason;
	struct browser_image *image;
	float caret[4];
};

/*
 * The front of one view.
 *
 * The program's thread made it and ends it; the script thread (thread)
 * runs the view.  Under lock: the mailbox (items, presses waiting), rung
 * (the loader has news), ending (the view is being destroyed; read with
 * __atomic outside the lock), the creation's outcome (created, error), the
 * state published last, the notifications waiting (notes, with the console
 * lines dropped and whether a redraw waits), whether the program's wake is
 * owed, the latest committed frame and whether a present came since the
 * script thread last looked (opportunity).  changed is signalled for a new
 * entry, news, a present and the end, and broadcast when the creation
 * ended.
 *
 * The script thread's alone: dirty (the view asked for a redraw since the
 * last committed frame), serial (the number of the last state), the
 * number of the last committed frame, the agent's running side and the
 * style thread the agent lends its heaps to (style, made and ended by the
 * script thread) and the threads of the page's iframes (frames, made
 * before the view and ended after it, by the script thread; the agent is
 * held through them for every task).  The program's thread's alone: shown
 * (the state its getters read, replaced in browser_view_process) and the
 * copy of the options and callbacks.
 */
struct view_front {
	struct browser_view *view;
	struct wb_thread thread;
	struct wb_mutex lock;
	struct wb_cond changed;
	struct wb_vector items;
	size_t presses;
	int rung;
	int ending;
	int created;
	int error;
	struct browser_view_options options;
	struct browser_callbacks program;
	struct browser_callbacks inner;
	struct vm_agent agent;
	struct view_snapshot published;
	struct view_snapshot shown;
	struct wb_vector notes;
	size_t console_dropped;
	int redraw_noted;
	int wake_owed;
	struct view_commit *latest;
	int opportunity;
	int dirty;
	uint64_t serial;
	uint64_t commit_serial;
	struct view_style *style;
	struct view_frames *frames;
};

/*
 * The front whose script thread the calling thread is (NULL on every other
 * thread): set by the script thread itself when it starts, so that it
 * knows itself without reading the handle the program's thread stores.
 */
static _Thread_local struct view_front *front_current;

static void front_run(void *argument);
static void front_enter(void *context);
static void front_loop(struct view_front *front);
static void front_execute(struct view_front *front, struct front_item *item);
static void front_publish(struct view_front *front);
static int front_commit(struct view_front *front, unsigned width, unsigned height);
static int front_push(struct browser_view *view, struct front_item *item);
static void front_navigation(struct view_front *front);
static int front_note(struct view_front *front, struct front_note *note);
static void front_note_free(struct front_note *note);
static void front_item_free(struct front_item *item);
static void front_snapshot_free(struct view_snapshot *snapshot);
static int front_snapshot_copy(const struct view_snapshot *from, struct view_snapshot *to);
static int front_snapshot_same(const struct view_snapshot *left, const struct view_snapshot *right);
static int front_string_same(const char *left, const char *right);
static char *front_copy(const char *text);
static char *front_copy_bytes(const char *text, size_t length);
static void front_ring(void *context);
static void front_redraw(void *context, struct browser_view *view);
static void front_title(void *context, struct browser_view *view, const char *title);
static void front_committed(void *context, struct browser_view *view);
static void front_load(void *context, struct browser_view *view, enum browser_load_state state, const char *url, int error, const char *reason);
static enum browser_policy front_link(void *context, struct browser_view *view, const char *href);
static void front_console(void *context, struct browser_view *view, int level, const char *text, size_t length);
static void front_script_error(void *context, struct browser_view *view, int error);
static void front_tell(struct view_front *front, struct front_note *note);

/*
 * Makes a view of its own threads: the front, then the script thread,
 * which makes the view itself and runs it; returns when the view was made
 * (0, with *view) or could not be (the error: ENOMEM, EAGAIN when the
 * thread could not start, or the view's own).
 */
int
view_front_create(
	const struct browser_view_options *options,
	struct browser_view **view)
{
	struct view_front *front;
	size_t stack;
	int error;

	/* The front, with the program's options and callbacks. */
	front = calloc(1, sizeof(*front));
	if (front == NULL)
		return ENOMEM;
	front->options = *options;
	if (options->callbacks != NULL)
		front->program = *options->callbacks;
	wb_vector_init(&front->items, sizeof(struct front_item));
	wb_vector_init(&front->notes, sizeof(struct front_note));
	vm_agent_init(&front->agent);
	front->published.range_error = ENOENT;
	front->published.progress = -1;
	front->shown.range_error = ENOENT;
	front->shown.progress = -1;

	/* The callbacks the view calls on the script thread, which become notifications. */
	front->inner.redraw = front_redraw;
	front->inner.title = front_title;
	front->inner.committed = front_committed;
	front->inner.load = front_load;
	front->inner.link = front_link;
	front->inner.console = front_console;
	front->inner.script_error = front_script_error;
	front->inner.context = front;

	/* Its lock and condition. */
	error = wb_mutex_init(&front->lock);
	if (error == 0)
		error = wb_cond_init(&front->changed);
	if (error != 0) {
		wb_cond_destroy(&front->changed);
		wb_mutex_destroy(&front->lock);
		free(front);
		return error;
	}

	/* The script thread, with a stack for deep scripts (smaller where the address space is). */
	stack = FRONT_STACK_64;
	if (sizeof(void *) < 8U)
		stack = FRONT_STACK_32;
	error = wb_thread_start("browser-script", stack, front_run, front, &front->thread);
	if (error != 0) {
		wb_cond_destroy(&front->changed);
		wb_mutex_destroy(&front->lock);
		free(front);
		return error;
	}

	/* The creation's outcome. */
	wb_mutex_lock(&front->lock);

	while (!front->created)
		wb_cond_wait(&front->changed, &front->lock);
	error = front->error;

	wb_mutex_unlock(&front->lock);

	/* A view that could not be made ended its thread. */
	if (error != 0) {
		wb_thread_join(&front->thread);
		wb_cond_destroy(&front->changed);
		wb_mutex_destroy(&front->lock);
		wb_vector_release(&front->items);
		wb_vector_release(&front->notes);
		free(front);
		return error;
	}

	/* Succeeded: the view runs on its thread. */
	*view = front->view;
	return 0;
}

/*
 * Ends a view of its own threads, from the program's thread: the task that
 * runs is asked to end and a request it waits for is woken, the script
 * thread is told to end the view and is joined, and the front goes.
 */
void
view_front_destroy(
	struct browser_view *view)
{
	struct view_front *front;
	struct front_item *item;
	struct front_note *note;
	size_t index;

	/* The page's work is stopped where it is. */
	front = view_front_of(view);
	front_navigation(front);

	/* The end, which the script thread sees before any entry left. */
	wb_mutex_lock(&front->lock);

	__atomic_store_n(&front->ending, 1, __ATOMIC_RELEASE);
	wb_cond_signal(&front->changed);

	wb_mutex_unlock(&front->lock);

	/* The thread ends the view and itself. */
	wb_thread_join(&front->thread);

	/* What was left: entries, notifications, the states and the latest committed frame. */
	for (index = 0; index < front->items.count; index++) {
		item = wb_vector_at(&front->items, index);
		front_item_free(item);
	}
	for (index = 0; index < front->notes.count; index++) {
		note = wb_vector_at(&front->notes, index);
		front_note_free(note);
	}
	wb_vector_release(&front->items);
	wb_vector_release(&front->notes);
	front_snapshot_free(&front->published);
	front_snapshot_free(&front->shown);
	view_commit_release(front->latest);
	wb_cond_destroy(&front->changed);
	wb_mutex_destroy(&front->lock);
	free(front);
}

/*
 * Tells whether a call into a view is the program's to hand to the front
 * (1: the view runs on its own threads and the caller is not its script
 * thread) or is to be run as it is (0).
 */
int
view_front_window(
	const struct browser_view *view)
{
	struct view_front *front;

	/* A view of the caller's thread has no front. */
	front = view_front_of(view);
	if (front == NULL)
		return 0;

	/* The script thread runs the view itself. */
	if (front_current == front)
		return 0;

	/* The program's call. */
	return 1;
}

/* Queues a load of a location (follow: resolved against the page shown, as a link) as a new step. */
int
view_front_load(
	struct browser_view *view,
	const char *location,
	int follow)
{
	struct front_item item;
	int error;

	/* The entry, with its own copy of the location. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_LOAD;
	if (follow)
		item.kind = FRONT_FOLLOW;
	item.text = front_copy(location);
	if (item.text == NULL)
		return ENOMEM;

	/* The page shown goes away: its work stops first. */
	front_navigation(view_front_of(view));
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the load is queued. */
	return 0;
}

/* Queues a step through the history (0 is a reload). */
int
view_front_go(
	struct browser_view *view,
	int steps)
{
	struct front_item item;
	int possible;
	int error;

	/* A step outside the history the program sees does nothing. */
	possible = browser_view_can_go(view, steps);
	if (!possible)
		return 0;

	/* The entry; the page shown goes away, its work stops first. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_GO;
	item.number = steps;
	front_navigation(view_front_of(view));
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the step is queued. */
	return 0;
}

/* Queues a stop of the load under way (the page's scripts go on). */
void
view_front_stop(
	struct browser_view *view)
{
	struct front_item item;

	/* The entry (only a lack of memory loses it). */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_STOP;
	(void)front_push(view, &item);
}

/* Queues a new size. */
int
view_front_resize(
	struct browser_view *view,
	unsigned width,
	unsigned height)
{
	struct front_item item;
	int error;

	/* The entry. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_RESIZE;
	item.width = width;
	item.height = height;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the size is queued. */
	return 0;
}

/* Queues a color scheme. */
void
view_front_color_scheme(
	struct browser_view *view,
	int scheme)
{
	struct front_item item;

	/* The entry (only a lack of memory loses it). */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_SCHEME;
	item.number = scheme;
	(void)front_push(view, &item);
}

/* Queues a scroll placed by the program. */
int
view_front_scroll_to(
	struct browser_view *view,
	double x,
	double y)
{
	const struct view_snapshot *shown;
	struct front_item item;
	int error;

	/* No page shown has no scroll. */
	shown = view_front_snapshot(view);
	if (shown->url == NULL)
		return ENOENT;

	/* The entry. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_SCROLL_TO;
	item.x = x;
	item.y = y;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the scroll is queued. */
	return 0;
}

/* Queues the program's shift of the content past an end. */
int
view_front_overscroll(
	struct browser_view *view,
	double dx,
	double dy)
{
	struct front_item item;
	int error;

	/* The entry. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_OVERSCROLL;
	item.dx = dx;
	item.dy = dy;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the shift is queued. */
	return 0;
}

/* Queues a move of the pointer. */
int
view_front_pointer_move(
	struct browser_view *view,
	float x,
	float y,
	uint32_t modifiers)
{
	struct front_item item;
	int error;

	/* The entry. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_MOVE;
	item.x = x;
	item.y = y;
	item.modifiers = modifiers;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the move is queued. */
	return 0;
}

/* Queues a press or a release of a button. */
int
view_front_pointer_button(
	struct browser_view *view,
	float x,
	float y,
	int button,
	int pressed,
	uint32_t modifiers)
{
	struct front_item item;
	int error;

	/* The entry. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_BUTTON;
	item.x = x;
	item.y = y;
	item.number = button;
	item.pressed = pressed;
	item.modifiers = modifiers;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the button is queued. */
	return 0;
}

/* Queues the pointer's leaving. */
int
view_front_pointer_leave(
	struct browser_view *view)
{
	struct front_item item;
	int error;

	/* The entry. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_LEAVE;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the leave is queued. */
	return 0;
}

/* Queues a turn of the wheel. */
int
view_front_wheel(
	struct browser_view *view,
	float x,
	float y,
	float delta_x,
	float delta_y,
	uint32_t modifiers)
{
	struct front_item item;
	int error;

	/* The entry. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_WHEEL;
	item.x = x;
	item.y = y;
	item.dx = delta_x;
	item.dy = delta_y;
	item.modifiers = modifiers;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the turn is queued. */
	return 0;
}

/* Queues a key pressed or let go, with its own copies of the names and the text. */
int
view_front_key(
	struct browser_view *view,
	const char *key,
	const char *code,
	const char *text,
	int pressed,
	int repeat,
	uint32_t modifiers)
{
	struct front_item item;
	int error;

	/* The entry. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_KEY;
	item.pressed = pressed;
	item.other = repeat;
	item.modifiers = modifiers;
	item.text = front_copy(key);
	item.code = front_copy(code);
	item.typed = front_copy(text);
	if (item.text == NULL || item.code == NULL || item.typed == NULL) {
		front_item_free(&item);
		return ENOMEM;
	}
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the key is queued. */
	return 0;
}

/* Queues the program's gaining or losing the focus. */
int
view_front_focus(
	struct browser_view *view,
	int focused)
{
	struct front_item item;
	int error;

	/* The entry. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_FOCUS;
	item.number = focused;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the focus is queued. */
	return 0;
}

/* Queues a focus of a field by its purpose; the answer is the field callback. */
int
view_front_focus_field(
	struct browser_view *view,
	const char *autocomplete)
{
	struct front_item item;
	int error;

	/* The entry, with its own copy of the token. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_FOCUS_FIELD;
	item.text = front_copy(autocomplete);
	if (item.text == NULL)
		return ENOMEM;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the focus is queued. */
	return 0;
}

/* Queues the text an input method composes. */
int
view_front_compose(
	struct browser_view *view,
	const char *preedit,
	int begin,
	int end)
{
	struct front_item item;
	int error;

	/* The entry, with its own copy of the text. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_COMPOSE;
	item.number = begin;
	item.other = end;
	item.text = front_copy(preedit);
	if (item.text == NULL)
		return ENOMEM;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the composition is queued. */
	return 0;
}

/* Queues text committed (checked: only into the session the program saw). */
int
view_front_commit_text(
	struct browser_view *view,
	int checked,
	uint64_t session,
	const char *text,
	uint32_t delete_before,
	uint32_t delete_after)
{
	struct front_item item;
	int error;

	/* The entry, with its own copy of the text. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_COMMIT;
	item.checked = checked;
	item.session = session;
	item.delete_before = delete_before;
	item.delete_after = delete_after;
	item.text = front_copy(text);
	if (item.text == NULL)
		return ENOMEM;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the text is queued. */
	return 0;
}

/* Queues a request for the image at a place; the answer is the image callback. */
int
view_front_request_image(
	struct browser_view *view,
	float x,
	float y)
{
	struct front_item item;
	int error;

	/* The entry. */
	memset(&item, 0, sizeof(item));
	item.kind = FRONT_IMAGE;
	item.x = x;
	item.y = y;
	error = front_push(view, &item);
	if (error != 0)
		return error;

	/* Succeeded: the request is queued. */
	return 0;
}

/*
 * Reports whether a task of the view's pages runs now (1) or not (0), from
 * any thread: the number of the one that has run longest and how long, in
 * milliseconds.  The page's top and its iframes of its origin take turns
 * in the view's agent; each frame of another origin has its own agent, and
 * runs beside them.
 */
int
view_front_script_busy(
	const struct browser_view *view,
	uint32_t *task,
	uint32_t *milliseconds)
{
	struct view_front *front;
	uint32_t other_task;
	uint32_t other_milliseconds;
	int other_busy;
	int busy;

	/* The view's agent's word. */
	front = view_front_of(view);
	busy = vm_agent_task_since(&front->agent, task, milliseconds);

	/* The frames of other agents', the longer of the two kept. */
	other_busy = view_remotes_busy(view_frames_remotes(front->frames), &other_task, &other_milliseconds);
	if (other_busy && (!busy || other_milliseconds > *milliseconds)) {
		*task = other_task;
		*milliseconds = other_milliseconds;
		busy = 1;
	}

	/* Whether any of them runs a task. */
	return busy;
}

/*
 * Ends the task of the view's pages of a number, from the program's
 * thread, as a navigation ends the page's (front_navigation) but leaving
 * the page: the task leaves at the engine's next look, and what it waits
 * for (a synchronous XMLHttpRequest, a parser's script) is woken.  Only
 * the agent that runs the task is asked (task numbers are the process's):
 * the view's, or a frame's of another origin, whose other tasks go on.
 * Reports 0, or ESRCH when that task does not run any more (it ended, or
 * another began).  A wait the next task began in the moment between the
 * two looks at the number may be woken too: it fails as a network error
 * would.
 */
int
view_front_stop_script(
	struct browser_view *view,
	uint32_t task)
{
	struct view_front *front;
	struct net_loader *loader;
	uint32_t running;
	int error;

	/* A task of a frame of another agent, when the view's agent does not run it. */
	front = view_front_of(view);
	running = vm_agent_task_current(&front->agent);
	if (task == 0U)
		return ESRCH;
	if (running != task) {
		error = view_remotes_stop(view_frames_remotes(front->frames), task);
		return error;
	}

	/* The view's agent's task. */
	vm_agent_request_termination(&front->agent, task);

	/* Its waits, woken while it still runs (the loader lives as long as the view); the frames of other agents go on. */
	running = vm_agent_task_current(&front->agent);
	if (running == task) {
		loader = view_loader(view);
		if (loader != NULL)
			net_loader_client_abort_waited(net_loader_own_client(loader));
		view_frames_abort_agent_waited(front->frames);
	}

	/* Succeeded: the task is asked to end. */
	return 0;
}

/* Reports the state the program's getters read (the one browser_view_process brought up last). */
const struct view_snapshot *
view_front_snapshot(
	const struct browser_view *view)
{
	struct view_front *front;

	/* The program's copy. */
	front = view_front_of(view);
	return &front->shown;
}

/* Copies the focused field's text around the caret from the state shown (browser_view_text_context). */
int
view_front_text_context(
	const struct browser_view *view,
	char *text,
	size_t size,
	size_t *cursor,
	int *purpose,
	unsigned *hints)
{
	const struct view_snapshot *shown;
	size_t length;

	/* The focus takes no text, or there is no room. */
	shown = view_front_snapshot(view);
	if (!shown->has_context || shown->context_text == NULL || size == 0)
		return 0;

	/* As much of the value as fits, ending at a character's start. */
	length = strlen(shown->context_text);
	if (length >= size) {
		length = size - 1U;
		while (length > 0 && ((unsigned char)shown->context_text[length] & 0xC0U) == 0x80U)
			length--;
	}
	memcpy(text, shown->context_text, length);
	text[length] = '\0';

	/* The caret inside what was copied, and what the control is for. */
	*cursor = shown->cursor;
	if (*cursor > length)
		*cursor = length;
	*purpose = shown->purpose;
	*hints = shown->hints;

	/* Succeeded: the focus takes text. */
	return 1;
}

/*
 * The program's side of browser_view_process: the state the script thread
 * published last becomes the one the getters read, then the program's
 * callbacks are called with the notifications that waited, in order.
 */
void
view_front_process(
	struct browser_view *view)
{
	struct view_front *front;
	struct view_snapshot next;
	struct wb_vector notes;
	struct front_note *note;
	struct front_note dropped;
	char line[96];
	size_t lost;
	size_t index;
	size_t told;
	int pushed;
	int copied;
	int error;

	/* The notifications up to the first one whose state is not published yet (it and those after it wait), the lines dropped, and a copy of the newest state. */
	front = view_front_of(view);
	memset(&next, 0, sizeof(next));
	copied = 0;
	error = 0;
	wb_mutex_lock(&front->lock);

	notes = front->notes;
	wb_vector_init(&front->notes, sizeof(struct front_note));
	told = notes.count;
	for (index = 0; index < notes.count; index++) {
		note = wb_vector_at(&notes, index);
		if (note->held && told == notes.count)
			told = index;

		/* A waiting one goes back to the queue, in order, and is marked moved here (one that cannot is told now). */
		if (index >= told) {
			pushed = wb_vector_push(&front->notes, note);
			note->held = 0;
			if (pushed == 0)
				note->held = FRONT_NOTE_MOVED;
		}
	}
	lost = front->console_dropped;
	front->console_dropped = 0;
	front->redraw_noted = 0;
	front->wake_owed = 0;
	if (front->published.serial != front->shown.serial) {
		error = front_snapshot_copy(&front->published, &next);
		copied = 1;
	}

	wb_mutex_unlock(&front->lock);

	/* The new state for the getters (the old one's strings go now). */
	if (copied && error == 0) {
		front_snapshot_free(&front->shown);
		front->shown = next;
	} else if (copied) {
		front_snapshot_free(&next);
	}

	/* Each notification that may be told, told and let go (the waiting ones are the queue's again). */
	for (index = 0; index < notes.count; index++) {
		note = wb_vector_at(&notes, index);
		if (note->held == FRONT_NOTE_MOVED)
			continue;
		front_tell(front, note);
		front_note_free(note);
	}
	wb_vector_release(&notes);

	/* The console lines there was no room for, in one line. */
	if (lost != 0) {
		memset(&dropped, 0, sizeof(dropped));
		dropped.kind = FRONT_NOTE_CONSOLE;
		dropped.level = 1;
		snprintf(line, sizeof(line), "%zu console lines were dropped", lost);
		dropped.text = line;
		dropped.length = strlen(line);
		front_tell(front, &dropped);
	}
}

/* Holds the latest committed frame of a view of its own threads, from any thread (NULL before the first). */
struct view_commit *
view_front_commit(
	struct browser_view *view)
{
	struct view_front *front;
	struct view_commit *commit;

	/* The latest, held before the front may let it go. */
	front = view_front_of(view);
	wb_mutex_lock(&front->lock);

	commit = front->latest;
	if (commit != NULL)
		__atomic_fetch_add(&commit->refs, 1U, __ATOMIC_RELAXED);

	wb_mutex_unlock(&front->lock);

	/* Succeeded: the frame, or none yet. */
	return commit;
}

/* Lets go of a hold on a committed frame, from any thread (NULL is nothing); the last frees it and lets go of its scene. */
void
view_commit_release(
	struct view_commit *commit)
{
	unsigned refs;

	/* Nothing held, or held by others still. */
	if (commit == NULL)
		return;
	refs = __atomic_sub_fetch(&commit->refs, 1U, __ATOMIC_ACQ_REL);
	if (refs != 0U)
		return;

	/* The last: the scene's hold, then the frame. */
	paint_scene_release(commit->scene);
	free(commit);
}

/*
 * Tells a view of its own threads, from the drawing thread, that a frame
 * was presented: the script thread takes it as the chance for the page's
 * animation frames and its rendering update.
 */
void
view_front_presented(
	struct browser_view *view,
	uint64_t serial)
{
	struct view_front *front;

	UNUSED_PARAMETER(serial);

	/* The chance, and the script thread wakes. */
	front = view_front_of(view);
	wb_mutex_lock(&front->lock);

	front->opportunity = 1;
	wb_cond_signal(&front->changed);

	wb_mutex_unlock(&front->lock);
}

/* Reports how many entries wait in a view's mailbox now (merged ones count once). */
size_t
view_front_queued(
	const struct browser_view *view)
{
	struct view_front *front;
	size_t count;

	/* The mailbox's length, under its lock. */
	front = view_front_of(view);
	wb_mutex_lock(&front->lock);

	count = front->items.count;

	wb_mutex_unlock(&front->lock);

	/* Succeeded: the count. */
	return count;
}

/*
 * Reports how many works the view's pages lent to its style thread and how
 * long they waited for them in all, in nanoseconds (0 and 0 for a view of
 * the caller's thread); any thread may ask.
 */
void
view_front_lend_counts(
	const struct browser_view *view,
	uint64_t *lends,
	uint64_t *waited_ns)
{
	struct view_front *front;

	/* A view of the caller's thread lends nothing. */
	*lends = 0;
	*waited_ns = 0;
	front = view_front_of(view);
	if (front == NULL)
		return;

	/* The style thread's counts. */
	view_style_counts(front->style, lends, waited_ns);
}

/* Reports the threads of a front's iframes (NULL before the script thread made them and after it ended them). */
struct view_frames *
view_front_frames(
	struct view_front *front)
{
	/* The script thread's. */
	return front->frames;
}

/* Reports the agent a front's pages run their tasks in. */
struct vm_agent *
view_front_agent(
	struct view_front *front)
{
	/* The front's own. */
	return &front->agent;
}

/*
 * Wakes the drawing for a new scene of a frame of another agent (from
 * its thread): the latest committed frame is drawn again with it.
 */
void
view_front_wake_drawing(
	struct view_front *front)
{
	/* The program's drawing thread. */
	if (front != NULL && front->options.frame_wake != NULL)
		front->options.frame_wake(front->options.frame_wake_context);
}

/*
 * The script thread: starts the style thread, makes the view with its own
 * frame as the pages' stack base and the front's callbacks and wake, tells
 * the program's thread how that went, runs the view until its end, and
 * ends it and the style thread.
 */
static void
front_run(
	void *argument)
{
	struct browser_view_options options;
	struct view_front *front;
	struct browser_view *view;
	int error;

	/* This thread knows itself as the front's. */
	front = argument;
	front_current = front;

	/* The style thread, which the agent lends the pages' heaps to for their style, layout and display lists, and the iframes' threads. */
	error = view_style_start(front_enter, front, &front->style);
	if (error == 0)
		vm_agent_set_lender(&front->agent, view_style_lend, front->style);
	if (error == 0)
		error = view_frames_create(&front->agent, front_ring, front_enter, front, &front->frames);

	/* The view, of this thread. */
	options = front->options;
	options.threading = BROWSER_THREADING_CALLER;
	options.stack_base = __builtin_frame_address(0);
	options.callbacks = &front->inner;
	options.wake = front_ring;
	options.wake_context = front;
	options.gpu = NULL;
	options.frame_wake = NULL;
	options.frame_wake_context = NULL;
	view = NULL;
	if (error == 0) {
		view_frames_enter(front->frames);
		error = browser_view_create(&options, &view);
		if (error == 0) {
			view_attach_front(view, front);
			front->view = view;
			view_frames_set_view(front->frames, view);
			front_publish(front);
		}
		view_frames_leave(front->frames);
	}

	/* The program's thread hears how it went. */
	wb_mutex_lock(&front->lock);

	front->error = error;
	front->created = 1;
	wb_cond_broadcast(&front->changed);

	wb_mutex_unlock(&front->lock);

	/* A view that could not be made ends here, with its frames and its style thread. */
	if (error != 0) {
		view_frames_destroy(front->frames);
		front->frames = NULL;
		vm_agent_set_lender(&front->agent, NULL, NULL);
		view_style_stop(front->style);
		front->style = NULL;
		return;
	}

	/* The view runs until its end, then goes (its iframes' threads with its pages), then the style thread that served its pages. */
	front_loop(front);
	view_frames_enter(front->frames);
	browser_view_destroy(view);
	view_frames_leave(front->frames);
	view_frames_destroy(front->frames);
	front->frames = NULL;
	vm_agent_set_lender(&front->agent, NULL, NULL);
	view_style_stop(front->style);
	front->style = NULL;
}

/* The style thread's first call: it knows itself as the front's too (the view's calls it may make are its own). */
static void
front_enter(
	void *context)
{
	/* The front's thread. */
	front_current = context;
}

/*
 * The script thread's loop: waits for an entry, the loader's news or the
 * view's next timer, takes the agent from the iframes' threads, runs the
 * entries that came (each a task), the view's round (a task), publishes,
 * and gives the agent back, until the view's end.
 */
static void
front_loop(
	struct view_front *front)
{
	struct wb_vector taken;
	struct front_item *item;
	size_t index;
	int opportunity;
	int timeout;
	int ending;

	/* The first wait's bound (the page's timers are the agent's). */
	view_frames_enter(front->frames);
	timeout = browser_view_timeout(front->view);
	view_frames_leave(front->frames);

	/* Round after round. */
	for (;;) {
		/* The wait: for an entry, news or the end, at most until the next timer. */
		wb_mutex_lock(&front->lock);

		if (front->items.count == 0 && !front->rung && !front->ending && !front->opportunity) {
			if (timeout < 0)
				wb_cond_wait(&front->changed, &front->lock);
			else
				(void)wb_cond_wait_for(&front->changed, &front->lock, (uint32_t)timeout);
		}
		taken = front->items;
		wb_vector_init(&front->items, sizeof(struct front_item));
		front->presses = 0;
		front->rung = 0;
		opportunity = front->opportunity;
		front->opportunity = 0;
		ending = front->ending;

		wb_mutex_unlock(&front->lock);

		/* Each entry in order, as a task, unless the view is ending. */
		view_frames_enter(front->frames);
		for (index = 0; index < taken.count; index++) {
			item = wb_vector_at(&taken, index);
			ending = __atomic_load_n(&front->ending, __ATOMIC_ACQUIRE);
			if (!ending) {
				vm_agent_task_begin(&front->agent);
				front_execute(front, item);
				vm_agent_task_end(&front->agent);
			}
			front_item_free(item);
		}
		wb_vector_release(&taken);

		/* The end. */
		ending = __atomic_load_n(&front->ending, __ATOMIC_ACQUIRE);
		if (ending) {
			view_frames_leave(front->frames);
			break;
		}

		/* What the frames of other agents asked of the page's documents, a task. */
		vm_agent_task_begin(&front->agent);
		view_frames_deliver(front->frames);
		vm_agent_task_end(&front->agent);

		/* A present came: the page's animation frames are due in the round. */
		if (opportunity)
			view_frame_opportunity(front->view);

		/* The view's round (the news, the timers, the images), a task too. */
		vm_agent_task_begin(&front->agent);
		browser_view_process(front->view);
		vm_agent_task_end(&front->agent);

		/* What the program sees, the next wait's bound, and the agent for the iframes' threads. */
		front_publish(front);
		timeout = browser_view_timeout(front->view);
		view_frames_leave(front->frames);
	}
}

/* Runs one entry of the mailbox on the script thread (the view's calls run as a view of this thread). */
static void
front_execute(
	struct view_front *front,
	struct front_item *item)
{
	struct browser_view *view;
	struct browser_image *image;
	struct front_note note;
	float caret[4];
	uint64_t session;
	int error;

	/* The entry's call. */
	view = front->view;
	error = 0;
	memset(&note, 0, sizeof(note));

	/* By what the entry asks. */
	switch (item->kind) {
	case FRONT_LOAD:
		error = browser_view_load(view, item->text);
		break;
	case FRONT_FOLLOW:
		error = browser_view_follow(view, item->text);
		break;
	case FRONT_GO:
		error = browser_view_go(view, item->number);
		break;
	case FRONT_STOP:
		browser_view_stop(view);
		break;
	case FRONT_RESIZE:
		error = browser_view_resize(view, item->width, item->height);
		front->dirty = 1;
		break;
	case FRONT_SCHEME:
		browser_view_set_color_scheme(view, (enum browser_color_scheme)item->number);
		break;
	case FRONT_SCROLL_TO:
		error = browser_view_scroll_to(view, item->x, item->y);
		break;
	case FRONT_OVERSCROLL:
		error = browser_view_set_overscroll(view, item->dx, item->dy);
		break;
	case FRONT_MOVE:
		error = browser_view_pointer_move(view, (float)item->x, (float)item->y, item->modifiers);
		break;
	case FRONT_BUTTON:
		error = browser_view_pointer_button(view, (float)item->x, (float)item->y, item->number, item->pressed, item->modifiers);
		break;
	case FRONT_LEAVE:
		error = browser_view_pointer_leave(view);
		break;
	case FRONT_WHEEL:
		error = browser_view_wheel(view, (float)item->x, (float)item->y, (float)item->dx, (float)item->dy, item->modifiers);
		break;
	case FRONT_KEY:
		error = browser_view_key(view, item->text, item->code, item->typed, item->pressed, item->other, item->modifiers);
		break;
	case FRONT_FOCUS:
		error = browser_view_focus(view, item->number);
		break;
	case FRONT_FOCUS_FIELD:
		/* The focus, and the answer with the field's caret. */
		note.kind = FRONT_NOTE_FIELD;
		note.error = browser_view_focus_field(view, item->text);
		if (note.error == 0)
			(void)browser_view_text_target(view, caret);
		if (note.error == 0)
			memcpy(note.caret, caret, sizeof(caret));
		(void)front_note(front, &note);
		break;
	case FRONT_COMPOSE:
		error = browser_view_compose(view, item->text, item->number, item->other);
		break;
	case FRONT_COMMIT:
		/* Text for another session than the program saw is dropped. */
		session = browser_view_text_session(view);
		if (item->checked && session != item->session)
			break;
		error = browser_view_commit_text(view, item->text, item->delete_before, item->delete_after);
		break;
	case FRONT_IMAGE:
		/* The image, copied for the program, or why there is none. */
		note.kind = FRONT_NOTE_IMAGE;
		image = calloc(1, sizeof(*image));
		note.error = ENOMEM;
		if (image != NULL)
			note.error = browser_view_image_at(view, (float)item->x, (float)item->y, image);
		if (note.error == 0) {
			note.image = image;
		} else {
			free(image);
		}
		(void)front_note(front, &note);
		break;
	default:
		break;
	}

	/* A navigation that failed is the load's failure; another failure (but a stopped script) is the scripts'. */
	if (error == 0 || error == VM_TERMINATED)
		return;
	memset(&note, 0, sizeof(note));
	if (item->kind == FRONT_LOAD || item->kind == FRONT_FOLLOW || item->kind == FRONT_GO) {
		note.kind = FRONT_NOTE_LOAD;
		note.state = BROWSER_LOAD_FAILED;
		note.error = error;
		note.text = front_copy(item->text);
		note.reason = front_copy("");
		(void)front_note(front, &note);
		return;
	}

	/* The scripts' error (an entry without a page is nothing). */
	if (error == ENOENT)
		return;
	note.kind = FRONT_NOTE_SCRIPT_ERROR;
	note.error = error;
	(void)front_note(front, &note);
}

/*
 * Publishes what the program sees after a round, on the script thread: the
 * view's state (with a new number when it changed), a new committed frame
 * when the view asked for a redraw or its size changed, and the program's
 * wake when it has something new.
 */
static void
front_publish(
	struct view_front *front)
{
	struct view_snapshot next;
	struct view_snapshot old;
	struct front_note *note;
	size_t index;
	int changed;
	int painted;
	int wake;
	int error;

	/* The state now. */
	memset(&next, 0, sizeof(next));
	error = view_fill_snapshot(front->view, &next);
	if (error != 0) {
		front_snapshot_free(&next);
		return;
	}

	/* A committed frame at the view's size, when the page changed. */
	painted = 0;
	if (front->dirty || front->latest == NULL || front->latest->width != next.width || front->latest->height != next.height) {
		error = front_commit(front, next.width, next.height);
		if (error == 0)
			painted = 1;
	}

	/* The state replaces the published one when it differs. */
	memset(&old, 0, sizeof(old));
	wb_mutex_lock(&front->lock);

	changed = !front_snapshot_same(&front->published, &next);
	if (changed) {
		front->serial++;
		next.serial = front->serial;
		old = front->published;
		front->published = next;
	}

	/* The notifications of the state now published may be told. */
	for (index = 0; index < front->notes.count; index++) {
		note = wb_vector_at(&front->notes, index);
		note->held = 0;
	}
	wake = 0;
	if (!front->wake_owed && (changed || front->notes.count != 0 || front->console_dropped != 0)) {
		front->wake_owed = 1;
		wake = 1;
	}

	wb_mutex_unlock(&front->lock);

	/* What was replaced, or what was the same, goes. */
	if (changed)
		front_snapshot_free(&old);
	else
		front_snapshot_free(&next);

	/* The drawing side hears of a new committed frame, the program of the rest. */
	if (painted && front->options.frame_wake != NULL)
		front->options.frame_wake(front->options.frame_wake_context);
	if (wake && front->options.wake != NULL)
		front->options.wake(front->options.wake_context);
}

/*
 * Commits the page as it is now at a size, on the script thread: its scene
 * (a new one when the page painted again), the scroll and the size, as the
 * latest committed frame; the one before goes when nobody draws from it.
 * Reports 0, or an error (no page: ENOENT; the latest stays).
 */
static int
front_commit(
	struct view_front *front,
	unsigned width,
	unsigned height)
{
	struct view_commit *commit;
	struct view_commit *old;
	struct paint_scene *scene;
	layout_unit scroll_y;
	int error;

	/* A view without a size has nothing to commit. */
	front->dirty = 0;
	if (width == 0 || height == 0)
		return EINVAL;

	/* The page laid out and painted as it is now, as a scene. */
	error = view_commit_scene(front->view, &scene, &scroll_y);
	if (error != 0)
		return error;

	/* The frame, with the next number. */
	commit = calloc(1, sizeof(*commit));
	if (commit == NULL) {
		paint_scene_release(scene);
		return ENOMEM;
	}
	front->commit_serial++;
	commit->refs = 1U;
	commit->scene = scene;
	commit->scroll_y = scroll_y;
	commit->width = width;
	commit->height = height;
	commit->serial = front->commit_serial;

	/* The latest; the old one goes when nobody draws from it. */
	wb_mutex_lock(&front->lock);

	old = front->latest;
	front->latest = commit;

	wb_mutex_unlock(&front->lock);

	/* The front's hold on the old one ends. */
	view_commit_release(old);

	/* Succeeded: the drawing thread draws this one next. */
	return 0;
}

/* Queues an entry (its strings become the mailbox's), merging it with the last one when they merge; ENOBUFS or ENOMEM frees them. */
static int
front_push(
	struct browser_view *view,
	struct front_item *item)
{
	struct view_front *front;
	struct front_item *last;
	int press;
	int merged;
	int error;

	/* Presses are bounded; releases, the focus and the commands are not. */
	front = view_front_of(view);
	press = 0;
	if ((item->kind == FRONT_BUTTON || item->kind == FRONT_KEY) && item->pressed)
		press = 1;
	if (item->kind == FRONT_COMPOSE || item->kind == FRONT_COMMIT)
		press = 1;

	/* The mailbox. */
	error = 0;
	merged = 0;
	wb_mutex_lock(&front->lock);

	last = NULL;
	if (front->items.count != 0)
		last = wb_vector_at(&front->items, front->items.count - 1U);
	if (last != NULL && last->kind == item->kind) {
		if (item->kind == FRONT_MOVE || item->kind == FRONT_RESIZE || item->kind == FRONT_SCHEME) {
			/* The newer move, size or scheme stands for both. */
			*last = *item;
			merged = 1;
		} else if (item->kind == FRONT_WHEEL && last->modifiers == item->modifiers) {
			/* The turns add up, at the newer place. */
			last->dx += item->dx;
			last->dy += item->dy;
			last->x = item->x;
			last->y = item->y;
			merged = 1;
		}
	}
	if (!merged && press && front->presses >= FRONT_PRESSES_MAX)
		error = ENOBUFS;
	if (!merged && error == 0)
		error = wb_vector_push(&front->items, item);
	if (!merged && error == 0 && press)
		front->presses++;
	if (error == 0)
		wb_cond_signal(&front->changed);

	wb_mutex_unlock(&front->lock);

	/* An entry refused is let go. */
	if (error != 0) {
		front_item_free(item);
		return error;
	}

	/* Succeeded: the entry waits for the script thread. */
	return 0;
}

/*
 * Stops the work of the page shown, from the program's thread, before a
 * navigation or the view's end: the task that runs is asked to end, and a
 * request the script thread waits for (a synchronous XMLHttpRequest, a
 * parser's script) is woken.
 */
static void
front_navigation(
	struct view_front *front)
{
	struct net_loader *loader;
	uint32_t task;

	/* The task that runs now, whatever it is. */
	task = vm_agent_task_current(&front->agent);
	vm_agent_request_termination(&front->agent, task);

	/* The requests the script thread and the iframes' threads wait for (the loader lives as long as the view). */
	loader = view_loader(front->view);
	if (loader != NULL)
		net_loader_client_abort_waited(net_loader_own_client(loader));
	view_frames_abort_waited(front->frames);
}

/*
 * Adds a notification for the program (on the script thread); a console
 * line past the bound is only counted.  Reports 0 or ENOMEM (the
 * notification's strings and image are let go then).
 */
static int
front_note(
	struct view_front *front,
	struct front_note *note)
{
	int error;

	/* A page committed, a load and a title wait for the state they tell of to be published. */
	note->held = 0;
	if (note->kind == FRONT_NOTE_COMMITTED ||
	    note->kind == FRONT_NOTE_LOAD ||
	    note->kind == FRONT_NOTE_TITLE)
		note->held = 1;

	/* The queue. */
	error = 0;
	wb_mutex_lock(&front->lock);

	if (note->kind == FRONT_NOTE_CONSOLE && front->notes.count >= FRONT_NOTES_MAX) {
		front->console_dropped++;
		error = ENOBUFS;
	} else {
		error = wb_vector_push(&front->notes, note);
	}

	wb_mutex_unlock(&front->lock);

	/* A notification not queued is let go. */
	if (error != 0) {
		front_note_free(note);
		return error;
	}

	/* Succeeded: browser_view_process tells it. */
	return 0;
}

/* Lets a notification's strings and image go. */
static void
front_note_free(
	struct front_note *note)
{
	/* What it owns. */
	free(note->text);
	free(note->reason);
	if (note->image != NULL) {
		browser_view_image_release(note->image);
		free(note->image);
	}
	note->text = NULL;
	note->reason = NULL;
	note->image = NULL;
}

/* Lets an entry's strings go. */
static void
front_item_free(
	struct front_item *item)
{
	/* What it owns. */
	free(item->text);
	free(item->code);
	free(item->typed);
	item->text = NULL;
	item->code = NULL;
	item->typed = NULL;
}

/* Lets a state's strings go. */
static void
front_snapshot_free(
	struct view_snapshot *snapshot)
{
	/* What it owns. */
	free(snapshot->url);
	free(snapshot->title);
	free(snapshot->context_text);
	snapshot->url = NULL;
	snapshot->title = NULL;
	snapshot->context_text = NULL;
}

/* Copies a state with strings of its own; ENOMEM leaves the copy's strings to front_snapshot_free. */
static int
front_snapshot_copy(
	const struct view_snapshot *from,
	struct view_snapshot *to)
{
	/* The numbers, then the strings. */
	*to = *from;
	to->url = NULL;
	to->title = NULL;
	to->context_text = NULL;
	if (from->url != NULL) {
		to->url = front_copy(from->url);
		if (to->url == NULL)
			return ENOMEM;
	}
	if (from->title != NULL) {
		to->title = front_copy(from->title);
		if (to->title == NULL)
			return ENOMEM;
	}
	if (from->context_text != NULL) {
		to->context_text = front_copy(from->context_text);
		if (to->context_text == NULL)
			return ENOMEM;
	}

	/* Succeeded: the copy is the program's. */
	return 0;
}

/* Tells whether two states say the same (the number aside). */
static int
front_snapshot_same(
	const struct view_snapshot *left,
	const struct view_snapshot *right)
{
	/* The strings. */
	if (!front_string_same(left->url, right->url))
		return 0;
	if (!front_string_same(left->title, right->title))
		return 0;
	if (!front_string_same(left->context_text, right->context_text))
		return 0;

	/* The numbers. */
	if (left->history_index != right->history_index || left->history_count != right->history_count)
		return 0;
	if (left->progress != right->progress || left->scroll_y != right->scroll_y)
		return 0;
	if (left->document_height != right->document_height || left->largest_x != right->largest_x)
		return 0;
	if (left->largest_y != right->largest_y || left->range_error != right->range_error)
		return 0;
	if (left->width != right->width || left->height != right->height)
		return 0;
	if (left->text_session != right->text_session || left->has_target != right->has_target)
		return 0;
	if (memcmp(left->caret, right->caret, sizeof(left->caret)) != 0)
		return 0;
	if (left->has_context != right->has_context || left->cursor != right->cursor)
		return 0;
	if (left->purpose != right->purpose || left->hints != right->hints)
		return 0;

	/* The same. */
	return 1;
}

/* Tells whether two strings (either may be NULL) are the same. */
static int
front_string_same(
	const char *left,
	const char *right)
{
	int differs;

	/* Both missing, or one. */
	if (left == NULL || right == NULL) {
		if (left == right)
			return 1;
		return 0;
	}

	/* The characters. */
	differs = strcmp(left, right);
	if (differs != 0)
		return 0;

	/* The same text. */
	return 1;
}

/* Copies a string (NULL is copied as an empty one). */
static char *
front_copy(
	const char *text)
{
	char *copy;

	/* NULL is empty. */
	if (text == NULL)
		text = "";

	/* The copy. */
	copy = front_copy_bytes(text, strlen(text));

	/* Succeeded or not: the copy, or NULL. */
	return copy;
}

/* Copies so many bytes of a text, ended with a NUL. */
static char *
front_copy_bytes(
	const char *text,
	size_t length)
{
	char *copy;

	/* The room. */
	copy = malloc(length + 1U);
	if (copy == NULL)
		return NULL;

	/* The bytes and the end. */
	memcpy(copy, text, length);
	copy[length] = '\0';

	/* Succeeded: the copy. */
	return copy;
}

/* The loader's wake (on its thread): the script thread has news to take. */
static void
front_ring(
	void *context)
{
	struct view_front *front;

	/* The mark, and the script thread wakes. */
	front = context;
	wb_mutex_lock(&front->lock);

	front->rung = 1;
	wb_cond_signal(&front->changed);

	wb_mutex_unlock(&front->lock);
}

/* The view's redraw (on the script thread): a new committed frame after this round, and the program's redraw once. */
static void
front_redraw(
	void *context,
	struct browser_view *view)
{
	struct view_front *front;
	struct front_note note;
	int owed;

	UNUSED_PARAMETER(view);

	/* The committed frame is owed. */
	front = context;
	front->dirty = 1;

	/* One redraw waits for the program at a time. */
	wb_mutex_lock(&front->lock);

	owed = front->redraw_noted;
	front->redraw_noted = 1;

	wb_mutex_unlock(&front->lock);

	/* The program's redraw. */
	if (owed)
		return;
	memset(&note, 0, sizeof(note));
	note.kind = FRONT_NOTE_REDRAW;
	(void)front_note(front, &note);
}

/* The view's new title (on the script thread). */
static void
front_title(
	void *context,
	struct browser_view *view,
	const char *title)
{
	struct front_note note;

	UNUSED_PARAMETER(view);

	/* The title, copied. */
	memset(&note, 0, sizeof(note));
	note.kind = FRONT_NOTE_TITLE;
	note.text = front_copy(title);
	if (note.text == NULL)
		return;
	(void)front_note(context, &note);
}

/* A page became the one shown (on the script thread). */
static void
front_committed(
	void *context,
	struct browser_view *view)
{
	struct front_note note;

	UNUSED_PARAMETER(view);

	/* The notification. */
	memset(&note, 0, sizeof(note));
	note.kind = FRONT_NOTE_COMMITTED;
	(void)front_note(context, &note);
}

/* A load's news (on the script thread). */
static void
front_load(
	void *context,
	struct browser_view *view,
	enum browser_load_state state,
	const char *url,
	int error,
	const char *reason)
{
	struct front_note note;

	UNUSED_PARAMETER(view);

	/* The news, with copies of the location and the reason. */
	memset(&note, 0, sizeof(note));
	note.kind = FRONT_NOTE_LOAD;
	note.state = (int)state;
	note.error = error;
	note.text = front_copy(url);
	note.reason = front_copy(reason);
	if (note.text == NULL || note.reason == NULL) {
		front_note_free(&note);
		return;
	}
	(void)front_note(context, &note);
}

/* Whether to follow a link: the program's answer, asked on the script thread. */
static enum browser_policy
front_link(
	void *context,
	struct browser_view *view,
	const char *href)
{
	struct view_front *front;
	enum browser_policy policy;

	/* Without the program's callback a link is followed. */
	front = context;
	if (front->program.link == NULL)
		return BROWSER_POLICY_ALLOW;

	/* The program's answer. */
	policy = front->program.link(front->program.context, view, href);
	return policy;
}

/* A console line (on the script thread). */
static void
front_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	struct front_note note;

	UNUSED_PARAMETER(view);

	/* The line, copied. */
	memset(&note, 0, sizeof(note));
	note.kind = FRONT_NOTE_CONSOLE;
	note.level = level;
	note.text = front_copy_bytes(text, length);
	note.length = length;
	if (note.text == NULL)
		return;
	(void)front_note(context, &note);
}

/* A timer's script failed (on the script thread); a stopped one is no failure. */
static void
front_script_error(
	void *context,
	struct browser_view *view,
	int error)
{
	struct front_note note;

	UNUSED_PARAMETER(view);

	/* A task asked to end. */
	if (error == VM_TERMINATED)
		return;

	/* The notification. */
	memset(&note, 0, sizeof(note));
	note.kind = FRONT_NOTE_SCRIPT_ERROR;
	note.error = error;
	(void)front_note(context, &note);
}

/* Tells the program one notification with its callback (on the program's thread); an image nobody takes is let go by the caller. */
static void
front_tell(
	struct view_front *front,
	struct front_note *note)
{
	const struct browser_callbacks *calls;
	struct browser_view *view;

	/* The program's callbacks. */
	calls = &front->program;
	view = front->view;

	/* By what the notification tells. */
	switch (note->kind) {
	case FRONT_NOTE_REDRAW:
		if (calls->redraw != NULL)
			calls->redraw(calls->context, view);
		break;
	case FRONT_NOTE_TITLE:
		if (calls->title != NULL)
			calls->title(calls->context, view, note->text);
		break;
	case FRONT_NOTE_COMMITTED:
		if (calls->committed != NULL)
			calls->committed(calls->context, view);
		break;
	case FRONT_NOTE_LOAD:
		if (calls->load != NULL)
			calls->load(calls->context, view, (enum browser_load_state)note->state, note->text, note->error, note->reason);
		break;
	case FRONT_NOTE_CONSOLE:
		if (calls->console != NULL)
			calls->console(calls->context, view, note->level, note->text, note->length);
		break;
	case FRONT_NOTE_SCRIPT_ERROR:
		if (calls->script_error != NULL)
			calls->script_error(calls->context, view, note->error);
		break;
	case FRONT_NOTE_IMAGE:
		/* The image is lent for the call (the program takes its pixels by clearing them). */
		if (calls->image != NULL)
			calls->image(calls->context, view, note->error, note->image);
		break;
	case FRONT_NOTE_FIELD:
		if (calls->field != NULL) {
			if (note->error == 0)
				calls->field(calls->context, view, 0, note->caret);
			else
				calls->field(calls->context, view, note->error, NULL);
		}
		break;
	default:
		break;
	}
}
