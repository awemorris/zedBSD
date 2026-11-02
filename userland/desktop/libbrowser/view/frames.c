/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The threads of the iframes of a view of its own threads (view/frames.h).
 *
 * The agent: one thread at a time holds it (held, with the frame that
 * holds it; NULL for the page's own thread) under the frames' lock, and a
 * thread that wants it waits on freed.  A frame's thread that is told to
 * end while it waits for the agent stops waiting, so that the thread that
 * holds the agent may join it (an iframe leaves its document in a task).
 *
 * A frame's thread waits on its own condition for its news (the network's,
 * work another thread left it) or its next timer, takes the agent, runs
 * its round as a task (the callbacks of its requests, its timers, its
 * documents' loads, a microtask checkpoint), and gives the agent back.
 * Whoever gives the agent back wakes the frames whose next timer came
 * sooner than they wait for (a message posted to them, an animation frame
 * after a present), and a frame's thread wakes the page's own thread, which
 * lays out, paints and commits the page with its frames.
 *
 * A frame that ends goes to the dead list: the thread that holds the
 * agent joins its thread (at once, or, for a frame that ended itself, at
 * the next hold), and its client stays open, its news dispatched by the
 * holders, until no request of it is under way.
 */

#include "view/frames.h"
#include "page/page.h"
#include "bind/internal.h"
#include "base/base.h"
#include "base/os-time.h"
#include "base/thread.h"
#include "net/net.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The most frames of a view, its page's own included (fewer where the address space is small). */
#define FRAMES_MAX_64		64U
#define FRAMES_MAX_32		16U

/* The stack of a frame's thread, as the page's own script thread's. */
#define FRAMES_STACK_64		(8U * 1024U * 1024U)
#define FRAMES_STACK_32		(2U * 1024U * 1024U)

/* How long one round dispatches the network's news, in nanoseconds (the rest at the next round). */
#define FRAMES_DISPATCH_NS	50000000ULL

/* The deadline of a frame without a timer. */
#define FRAMES_NO_DEADLINE	UINT64_MAX

/*
 * One iframe's thread.
 *
 * Under lock: rung (news, or work left for it), orphan (its thread ended:
 * its client's news are the holders'), and deadline_ns (when its next
 * timer is due on the monotonic clock).  ending is set once, under lock
 * and through __atomic, read through __atomic.  The rest is the holder's
 * of the agent, but stack_base, the thread's own.
 */
struct view_frame {
	struct view_frames *frames;
	struct page *page;
	struct dom_element *element;
	struct wb_thread thread;
	struct wb_mutex lock;
	struct wb_cond changed;
	int rung;
	int ending;
	int orphan;
	int joined;
	uint64_t deadline_ns;
	struct net_loader_client *client;
	const void *stack_base;
};

/*
 * The frames of a view.  Under lock: held and holder (the agent),
 * top_waiting (the page's own thread waits for it: it goes there next),
 * and the lists' changes (the holder changes them; another thread reads them under
 * lock only): live (struct view_frame *, the iframes with a thread) and
 * dead (ended, to be joined and emptied).
 */
struct view_frames {
	struct browser_view *view;
	struct vm_agent *agent;
	void (*ring_top)(void *context);
	void (*enter)(void *context);
	void *context;
	struct wb_mutex lock;
	struct wb_cond freed;
	int held;
	int top_waiting;
	struct view_frame *holder;
	struct wb_vector live;
	struct wb_vector dead;
	size_t limit;
};

/* The frame whose thread the calling thread is (NULL on every other thread). */
static _Thread_local struct view_frame *frames_current;

static int frames_attach(void *context, struct page *page, struct dom_element *element);
static void frames_detach(void *context, struct dom_element *element);
static void frames_ring(void *context, void *runner);
static void frames_notify(void *context);
static void frames_run(void *argument);
static void frames_round(struct view_frame *frame);
static int frames_acquire(struct view_frames *frames, struct view_frame *frame);
static void frames_release(struct view_frames *frames);
static void frames_wake_due(struct view_frames *frames);
static uint64_t frames_deadline(struct view_frames *frames, struct view_frame *frame);
static void frames_reap(struct view_frames *frames);
static void frames_remove(struct wb_vector *list, struct view_frame *frame);
static void frames_free(struct view_frame *frame);

/* The calls the pages of a view give their iframes threads through. */
static const struct page_frame_calls frames_calls = {
	frames_attach,
	frames_detach,
	frames_ring
};

/* Makes the frames of a view (none yet). */
int
view_frames_create(
	struct vm_agent *agent,
	void (*ring_top)(void *context),
	void (*enter)(void *context),
	void *context,
	struct view_frames **frames)
{
	struct view_frames *made;
	int error;

	/* The record, its lock and its condition. */
	*frames = NULL;
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->agent = agent;
	made->ring_top = ring_top;
	made->enter = enter;
	made->context = context;
	made->limit = FRAMES_MAX_64;
	if (sizeof(void *) < 8U)
		made->limit = FRAMES_MAX_32;
	wb_vector_init(&made->live, sizeof(struct view_frame *));
	wb_vector_init(&made->dead, sizeof(struct view_frame *));
	error = wb_mutex_init(&made->lock);
	if (error == 0)
		error = wb_cond_init(&made->freed);
	if (error != 0) {
		wb_cond_destroy(&made->freed);
		wb_mutex_destroy(&made->lock);
		free(made);
		return error;
	}

	/* Succeeded: no frame has a thread yet. */
	*frames = made;
	return 0;
}

/*
 * Ends the frames' threads and closes their clients, on the page's own
 * thread holding the agent, after the view's pages went (their iframes'
 * threads ended with them) and before its loader goes.
 */
void
view_frames_close(
	struct view_frames *frames)
{
	struct view_frame *frame;
	size_t index;

	/* Nothing to end. */
	if (frames == NULL)
		return;

	/* A frame still live (none should be: its page went) ends like a dead one. */
	for (index = 0; index < frames->live.count; index++) {
		frame = *(struct view_frame **)wb_vector_at(&frames->live, index);
		wb_mutex_lock(&frame->lock);
		__atomic_store_n(&frame->ending, 1, __ATOMIC_RELEASE);
		wb_cond_signal(&frame->changed);
		wb_mutex_unlock(&frame->lock);
		(void)wb_vector_push(&frames->dead, &frame);
	}
	wb_mutex_lock(&frames->lock);
	frames->live.count = 0;
	wb_cond_broadcast(&frames->freed);
	wb_mutex_unlock(&frames->lock);

	/* Each dead one joined and let go, its requests with its client. */
	for (index = 0; index < frames->dead.count; index++) {
		frame = *(struct view_frame **)wb_vector_at(&frames->dead, index);
		if (!frame->joined)
			wb_thread_join(&frame->thread);
		frames_free(frame);
	}
	wb_mutex_lock(&frames->lock);
	frames->dead.count = 0;
	wb_mutex_unlock(&frames->lock);
}

/* Lets the frames of a view go (view_frames_close ended them), on the page's own thread. */
void
view_frames_destroy(
	struct view_frames *frames)
{
	/* Nothing to let go. */
	if (frames == NULL)
		return;

	/* Whatever is left ends, then the lists, the condition and the lock. */
	view_frames_close(frames);
	wb_vector_release(&frames->live);
	wb_vector_release(&frames->dead);
	wb_cond_destroy(&frames->freed);
	wb_mutex_destroy(&frames->lock);
	free(frames);
}

/* Tells the frames their view (made after them, before its first page). */
void
view_frames_set_view(
	struct view_frames *frames,
	struct browser_view *view)
{
	/* The view whose clock the frames' timers go by. */
	frames->view = view;
}

/* The page's own thread takes the agent (and empties the dead frames it can). */
void
view_frames_enter(
	struct view_frames *frames)
{
	/* The agent, then the dead. */
	(void)frames_acquire(frames, NULL);
	frames_reap(frames);
}

/* The page's own thread gives the agent back. */
void
view_frames_leave(
	struct view_frames *frames)
{
	/* The frames' due work woken, then the agent. */
	frames_release(frames);
}

/* Wakes the frames' threads out of the requests they wait for (from any thread). */
void
view_frames_abort_waited(
	struct view_frames *frames)
{
	struct view_frame *frame;
	size_t index;

	/* Every frame's client, live or dead, under the lists' lock. */
	if (frames == NULL)
		return;
	wb_mutex_lock(&frames->lock);

	for (index = 0; index < frames->live.count; index++) {
		frame = *(struct view_frame **)wb_vector_at(&frames->live, index);
		net_loader_client_abort_waited(frame->client);
	}
	for (index = 0; index < frames->dead.count; index++) {
		frame = *(struct view_frame **)wb_vector_at(&frames->dead, index);
		net_loader_client_abort_waited(frame->client);
	}

	wb_mutex_unlock(&frames->lock);
}

/* Reports how many iframes have a thread now. */
size_t
view_frames_count(
	struct view_frames *frames)
{
	size_t count;

	/* The live list's length, under its lock. */
	if (frames == NULL)
		return 0;
	wb_mutex_lock(&frames->lock);

	count = frames->live.count;

	wb_mutex_unlock(&frames->lock);

	/* Succeeded: the count. */
	return count;
}

/* Reports the calls a page gives its iframes their threads through. */
const struct page_frame_calls *
view_frames_calls(void)
{
	/* The one table. */
	return &frames_calls;
}

/*
 * Gives an iframe its thread, with a client of the view's loader of its
 * own (the agent is held).  Reports 0, EAGAIN (the view has as many frames
 * as it may, or the thread could not start), ENOMEM.
 */
static int
frames_attach(
	void *context,
	struct page *page,
	struct dom_element *element)
{
	struct view_frames *frames;
	struct view_frame *frame;
	size_t stack;
	int error;

	/* A frame more than the view may have (its page's own counts) gets none. */
	frames = context;
	if (frames->live.count + 1U >= frames->limit || page->loader == NULL)
		return EAGAIN;

	/* The record, its lock, its condition and its client. */
	frame = calloc(1, sizeof(*frame));
	if (frame == NULL)
		return ENOMEM;
	frame->frames = frames;
	frame->page = page;
	frame->element = element;
	frame->rung = 1;
	frame->deadline_ns = FRAMES_NO_DEADLINE;
	error = wb_mutex_init(&frame->lock);
	if (error == 0)
		error = wb_cond_init(&frame->changed);
	if (error == 0)
		error = net_loader_client_open(page->loader, frames_notify, frame, &frame->client);
	if (error == 0) {
		wb_mutex_lock(&frames->lock);
		error = wb_vector_push(&frames->live, &frame);
		wb_mutex_unlock(&frames->lock);
	}
	if (error != 0) {
		frames_free(frame);
		return error;
	}

	/* The thread, which waits for the agent this thread holds. */
	element->frame_thread = frame;
	stack = FRAMES_STACK_64;
	if (sizeof(void *) < 8U)
		stack = FRAMES_STACK_32;
	error = wb_thread_start("browser-script", stack, frames_run, frame, &frame->thread);
	if (error != 0) {
		element->frame_thread = NULL;
		wb_mutex_lock(&frames->lock);
		frames_remove(&frames->live, frame);
		wb_mutex_unlock(&frames->lock);
		frames_free(frame);
		if (error != ENOMEM)
			error = EAGAIN;
		return error;
	}

	/* Succeeded: the iframe's tasks run on its thread. */
	return 0;
}

/*
 * Ends the thread of an iframe that leaves its document (the agent is
 * held): it is told to end and joined, unless it is the calling thread
 * (then the next holder joins it); its client empties later.
 */
static void
frames_detach(
	void *context,
	struct dom_element *element)
{
	struct view_frames *frames;
	struct view_frame *frame;
	int error;

	/* The iframe runs on its document's thread from now on. */
	frames = context;
	frame = element->frame_thread;
	element->frame_thread = NULL;
	if (frame == NULL)
		return;

	/* From the live list to the dead one. */
	wb_mutex_lock(&frames->lock);

	frames_remove(&frames->live, frame);
	error = wb_vector_push(&frames->dead, &frame);

	wb_mutex_unlock(&frames->lock);

	/* The end, which the thread sees in its wait for news or for the agent. */
	wb_mutex_lock(&frame->lock);

	__atomic_store_n(&frame->ending, 1, __ATOMIC_RELEASE);
	wb_cond_signal(&frame->changed);

	wb_mutex_unlock(&frame->lock);
	wb_mutex_lock(&frames->lock);

	wb_cond_broadcast(&frames->freed);

	wb_mutex_unlock(&frames->lock);

	/* Joined now, unless it is this thread. */
	if (frame != frames_current) {
		wb_thread_join(&frame->thread);
		frame->joined = 1;
		wb_mutex_lock(&frame->lock);
		frame->orphan = 1;
		wb_mutex_unlock(&frame->lock);
	}

	/* A frame the dead list had no room for goes at once (its client's requests abort); the others empty at a later hold. */
	if (error != 0 && frame->joined)
		frames_free(frame);
}

/* Wakes a runner's thread (NULL: the page's own) for work left there. */
static void
frames_ring(
	void *context,
	void *runner)
{
	struct view_frames *frames;
	struct view_frame *frame;

	/* The page's own thread. */
	frames = context;
	if (runner == NULL) {
		frames->ring_top(frames->context);
		return;
	}

	/* A frame's. */
	frame = runner;
	wb_mutex_lock(&frame->lock);

	frame->rung = 1;
	wb_cond_signal(&frame->changed);

	wb_mutex_unlock(&frame->lock);
}

/* A frame's client has news (on the network's thread): its thread wakes, or the page's own once it ended. */
static void
frames_notify(
	void *context)
{
	struct view_frame *frame;
	int orphan;

	/* The mark, for the frame's thread or the holders. */
	frame = context;
	wb_mutex_lock(&frame->lock);

	orphan = frame->orphan;
	frame->rung = 1;
	wb_cond_signal(&frame->changed);

	wb_mutex_unlock(&frame->lock);

	/* An ended frame's news are dispatched by the page's own thread. */
	if (orphan)
		frame->frames->ring_top(frame->frames->context);
}

/*
 * A frame's thread: waits for its news or its next timer, takes the agent
 * and runs its round, gives it back, until it is told to end.
 */
static void
frames_run(
	void *argument)
{
	struct view_frame *frame;
	struct view_frames *frames;
	uint64_t now;
	uint64_t wait;
	int ending;

	/* This thread is the frame's: its view's, its runner, its requests' client and its stack's base. */
	frame = argument;
	frames = frame->frames;
	frames->enter(frames->context);
	frames_current = frame;
	bind_set_thread_runner(frame);
	page_set_thread_client(frame->client);
	frame->stack_base = __builtin_frame_address(0);

	/* Round after round. */
	for (;;) {
		/* The wait: for news or the end, at most until the next timer. */
		wb_mutex_lock(&frame->lock);

		while (!frame->rung && !__atomic_load_n(&frame->ending, __ATOMIC_ACQUIRE)) {
			if (frame->deadline_ns == FRAMES_NO_DEADLINE) {
				wb_cond_wait(&frame->changed, &frame->lock);
				continue;
			}
			now = wb_os_monotonic_ns();
			if (now >= frame->deadline_ns)
				break;
			wait = (frame->deadline_ns - now + 999999ULL) / 1000000ULL;
			if (wait > 1000ULL)
				wait = 1000ULL;
			(void)wb_cond_wait_for(&frame->changed, &frame->lock, (uint32_t)wait);
		}
		frame->rung = 0;
		ending = __atomic_load_n(&frame->ending, __ATOMIC_ACQUIRE);

		wb_mutex_unlock(&frame->lock);

		/* The end, before the agent or while waiting for it. */
		if (ending)
			break;
		if (!frames_acquire(frames, frame))
			break;

		/* The round, then the agent goes back. */
		frames_round(frame);
		frames_release(frames);
	}
}

/*
 * A frame's round, with the agent held, as one task: the callbacks of its
 * requests (for a while), its timers due, its documents' loads and a
 * microtask checkpoint; then when its next timer is due.
 */
static void
frames_round(
	struct view_frame *frame)
{
	struct view_frames *frames;
	struct bind_window *window;
	struct vm_heap *heap;
	const void *saved;
	uint64_t deadline;
	int left;
	int status;

	/* The heap scans this thread's stack while it runs the task. */
	frames = frame->frames;
	heap = frame->page->heap;
	saved = vm_heap_stack_base(heap);
	vm_heap_set_stack_base(heap, frame->stack_base);
	vm_agent_task_begin(frames->agent);

	/* The callbacks of its requests that ended. */
	left = net_loader_client_dispatch_until(frame->client, wb_os_monotonic_ns() + FRAMES_DISPATCH_NS);

	/* Its timers due, at the page's time, and the checkpoint after them (its documents' loads are activated there). */
	status = 0;
	window = NULL;
	if (!__atomic_load_n(&frame->ending, __ATOMIC_ACQUIRE))
		window = bind_frame_child(&frame->element->node);
	if (window != NULL && !window->detached) {
		bind_window_set_time(window, view_page_time(frames->view));
		status = bind_run_timers(window);
		if (status == 0)
			status = bind_checkpoint(window);
	} else if (!__atomic_load_n(&frame->ending, __ATOMIC_ACQUIRE)) {
		status = page_frames_checkpoint(frame->page);
	}
	(void)status;
	vm_agent_task_end(frames->agent);

	/* When it is due next, and whether news are left. */
	deadline = frames_deadline(frames, frame);
	wb_mutex_lock(&frame->lock);

	frame->deadline_ns = deadline;
	if (left)
		frame->rung = 1;

	wb_mutex_unlock(&frame->lock);
	vm_heap_set_stack_base(heap, saved);
}

/* Takes the agent for a frame's thread (frame) or the page's own (NULL); 0 when the frame was told to end meanwhile. */
static int
frames_acquire(
	struct view_frames *frames,
	struct view_frame *frame)
{
	int ending;

	/* Waits while another thread holds it, and a frame's thread while the page's own waits for it too (the page is laid out and shown there). */
	wb_mutex_lock(&frames->lock);

	ending = 0;
	if (frame == NULL)
		frames->top_waiting++;
	for (;;) {
		if (frame != NULL)
			ending = __atomic_load_n(&frame->ending, __ATOMIC_ACQUIRE);
		if (ending || (!frames->held && (frame == NULL || frames->top_waiting == 0)))
			break;
		wb_cond_wait(&frames->freed, &frames->lock);
	}
	if (frame == NULL)
		frames->top_waiting--;
	if (!ending) {
		frames->held = 1;
		frames->holder = frame;
	}

	wb_mutex_unlock(&frames->lock);

	/* Succeeded, unless the frame ends. */
	return !ending;
}

/*
 * Gives the agent back (the holder): the frames whose next timer came
 * sooner wake, and a frame's round wakes the page's own thread (which
 * lays the page out with the frame's changes and empties the dead).
 */
static void
frames_release(
	struct view_frames *frames)
{
	/* The frames' due work, the page's own thread after a frame's round. */
	frames_wake_due(frames);
	if (frames_current != NULL)
		frames->ring_top(frames->context);

	/* The agent goes. */
	wb_mutex_lock(&frames->lock);

	frames->held = 0;
	frames->holder = NULL;
	wb_cond_broadcast(&frames->freed);

	wb_mutex_unlock(&frames->lock);
}

/* Wakes each live frame (but the calling thread's) whose next timer is due sooner than it waits for (the agent is held). */
static void
frames_wake_due(
	struct view_frames *frames)
{
	struct view_frame *frame;
	uint64_t deadline;
	size_t index;

	/* Each frame's due time against its wait. */
	for (index = 0; index < frames->live.count; index++) {
		frame = *(struct view_frame **)wb_vector_at(&frames->live, index);
		if (frame == frames_current)
			continue;
		deadline = frames_deadline(frames, frame);
		if (deadline == FRAMES_NO_DEADLINE)
			continue;
		wb_mutex_lock(&frame->lock);

		if (deadline < frame->deadline_ns) {
			frame->deadline_ns = deadline;
			frame->rung = 1;
			wb_cond_signal(&frame->changed);
		}

		wb_mutex_unlock(&frame->lock);
	}
}

/* Reports when a live frame's next timer is due on the monotonic clock (FRAMES_NO_DEADLINE: none; the agent is held). */
static uint64_t
frames_deadline(
	struct view_frames *frames,
	struct view_frame *frame)
{
	struct bind_window *window;
	uint64_t now;
	double due;
	double wait;

	/* The frame's window, when it has one. */
	if (__atomic_load_n(&frame->ending, __ATOMIC_ACQUIRE))
		return FRAMES_NO_DEADLINE;
	window = bind_frame_child(&frame->element->node);
	if (window == NULL || window->detached || !bind_next_timer(window, &due))
		return FRAMES_NO_DEADLINE;

	/* The page's time to the monotonic clock. */
	now = wb_os_monotonic_ns();
	wait = due - view_page_time(frames->view);
	if (wait <= 0.0)
		return now;
	if (wait > 3600000.0)
		wait = 3600000.0;
	return now + (uint64_t)(wait * 1000000.0);
}

/*
 * Empties the dead frames, on the page's own thread as it takes the
 * agent: each one's thread joined, its client's news dispatched here, and
 * the frame let go once none of its requests is under way.
 */
static void
frames_reap(
	struct view_frames *frames)
{
	struct view_frame *frame;
	size_t index;

	/* Each dead one, from the last (one let go leaves the list). */
	index = frames->dead.count;
	while (index != 0) {
		index--;
		frame = *(struct view_frame **)wb_vector_at(&frames->dead, index);
		if (frame == frames_current)
			continue;
		if (!frame->joined) {
			wb_thread_join(&frame->thread);
			frame->joined = 1;
			wb_mutex_lock(&frame->lock);
			frame->orphan = 1;
			wb_mutex_unlock(&frame->lock);
		}
		(void)net_loader_client_dispatch_until(frame->client, wb_os_monotonic_ns() + FRAMES_DISPATCH_NS);
		if (net_loader_client_pending(frame->client) != 0)
			continue;
		wb_mutex_lock(&frames->lock);
		frames_remove(&frames->dead, frame);
		wb_mutex_unlock(&frames->lock);
		frames_free(frame);
	}
}

/* Takes a frame out of a list (in order; the lists' lock is held). */
static void
frames_remove(
	struct wb_vector *list,
	struct view_frame *frame)
{
	struct view_frame **slot;
	size_t index;

	/* Its slot, and the later ones close up. */
	for (index = 0; index < list->count; index++) {
		slot = wb_vector_at(list, index);
		if (*slot != frame)
			continue;
		memmove(slot, slot + 1, (list->count - index - 1U) * sizeof(*slot));
		list->count--;
		return;
	}
}

/* Lets a frame go whose thread ended or never started: its client (its requests abort), its condition, its lock. */
static void
frames_free(
	struct view_frame *frame)
{
	/* The client first: no news after it. */
	net_loader_client_close(frame->client);
	wb_cond_destroy(&frame->changed);
	wb_mutex_destroy(&frame->lock);
	free(frame);
}
