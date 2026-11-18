/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The style thread of a view of its own threads (view/style.h): the queue of
 * the works lent to it, its loop, and the lender the view's agent lends its
 * heaps through.
 *
 * A lender queues a request on its own stack and waits on the answered
 * condition until the style thread marks it done; the style thread takes the
 * requests in order and runs each with the lender's thread's state of the
 * work (whether the scripts turned the animations off, whether a layout
 * keeps the position options that fit), which the style and layout code
 * read from their thread.
 */

#include "view/style.h"
#include "base/base.h"
#include "base/os-time.h"
#include "base/thread.h"
#include "css/css.h"
#include "layout/layout.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The stack of the style thread: the layout's recursion, as deep as the script thread's. */
#define STYLE_STACK_64		(8U * 1024U * 1024U)
#define STYLE_STACK_32		(2U * 1024U * 1024U)

/*
 * One work lent to the style thread, on its lender's stack: the work and its
 * argument, the lender's state for it, the work's outcome and whether it
 * ran (done, under the style's lock), and the next request in the queue.
 */
struct style_request {
	vm_lend_work work;
	void *argument;
	int animation_off;
	int remember;
	int result;
	int done;
	struct style_request *next;
};

/*
 * The style thread of one view.
 *
 * Under lock: the queue of requests (first, last), ending (no work comes any
 * more; the thread ends once the queue is empty) and the counts (lends, the
 * lenders' waits in nanoseconds).  asked is signalled for a new request and
 * the end, answered is broadcast when a request is done.  enter and context
 * are set before the thread starts and read by it once.
 */
struct view_style {
	struct wb_thread thread;
	struct wb_mutex lock;
	struct wb_cond asked;
	struct wb_cond answered;
	struct style_request *first;
	struct style_request *last;
	int ending;
	uint64_t lends;
	uint64_t waited_ns;
	void (*enter)(void *context);
	void *context;
};

static void style_run(void *argument);

/*
 * Starts the style thread, which first makes the call given on itself;
 * reports 0 with *style, ENOMEM, or the thread's or the lock's error.
 */
int
view_style_start(
	void (*enter)(void *context),
	void *context,
	struct view_style **style)
{
	struct view_style *made;
	size_t stack;
	int error;

	/* The style's record. */
	*style = NULL;
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->enter = enter;
	made->context = context;

	/* Its lock and conditions. */
	error = wb_mutex_init(&made->lock);
	if (error == 0)
		error = wb_cond_init(&made->asked);
	if (error == 0)
		error = wb_cond_init(&made->answered);
	if (error != 0) {
		wb_cond_destroy(&made->answered);
		wb_cond_destroy(&made->asked);
		wb_mutex_destroy(&made->lock);
		free(made);
		return error;
	}

	/* The thread, with a stack for deep layouts (smaller where the address space is). */
	stack = STYLE_STACK_64;
	if (sizeof(void *) < 8U)
		stack = STYLE_STACK_32;
	error = wb_thread_start("browser-style", stack, style_run, made, &made->thread);
	if (error != 0) {
		wb_cond_destroy(&made->answered);
		wb_cond_destroy(&made->asked);
		wb_mutex_destroy(&made->lock);
		free(made);
		return error;
	}

	/* Succeeded: works may be lent to it. */
	*style = made;
	return 0;
}

/*
 * Ends the style thread once no work can be lent to it any more (the
 * view's pages are gone): it runs what is queued, ends and is joined.
 */
void
view_style_stop(
	struct view_style *style)
{
	/* A style that never started has nothing to end. */
	if (style == NULL)
		return;

	/* The end, which the thread sees once its queue is empty. */
	wb_mutex_lock(&style->lock);

	style->ending = 1;
	wb_cond_signal(&style->asked);

	wb_mutex_unlock(&style->lock);

	/* The thread ends, then its record. */
	wb_thread_join(&style->thread);
	wb_cond_destroy(&style->answered);
	wb_cond_destroy(&style->asked);
	wb_mutex_destroy(&style->lock);
	free(style);
}

/*
 * Lends a work to the style thread (the agent's lender, vm_lender): queues
 * it with the calling thread's state for it and waits until it ran.
 * Reports the work's outcome.
 */
int
view_style_lend(
	void *context,
	vm_lend_work work,
	void *argument)
{
	struct style_request request;
	struct view_style *style;
	uint64_t started;
	uint64_t ended;
	int ending;
	int error;

	/* The request, with what the style and layout code read from their thread. */
	style = context;
	memset(&request, 0, sizeof(request));
	request.work = work;
	request.argument = argument;
	request.animation_off = css_animation_is_off();
	request.remember = layout_try_remembers();

	/* Queued, then waited for (a style that is ending takes no more). */
	started = wb_os_monotonic_ns();
	wb_mutex_lock(&style->lock);

	ending = style->ending;
	if (!ending) {
		if (style->last != NULL) {
			style->last->next = &request;
		} else {
			style->first = &request;
		}

		/* The request is the last one asked now. */
		style->last = &request;
		wb_cond_signal(&style->asked);
		while (!request.done)
			wb_cond_wait(&style->answered, &style->lock);
		style->lends++;
	}

	wb_mutex_unlock(&style->lock);

	/* A style that ends lets the work run here (no page lends by then; this is the safe answer). */
	if (ending) {
		error = work(argument);
		if (error != 0)
			return error;
		return 0;
	}

	/* The wait counted. */
	ended = wb_os_monotonic_ns();
	wb_mutex_lock(&style->lock);

	style->waited_ns += ended - started;

	wb_mutex_unlock(&style->lock);

	/* The work's outcome. */
	error = request.result;
	if (error != 0)
		return error;

	/* Succeeded: the work ran on the style thread. */
	return 0;
}

/* Reports how many works were lent and how long their lenders waited, in nanoseconds, in all. */
void
view_style_counts(
	struct view_style *style,
	uint64_t *lends,
	uint64_t *waited_ns)
{
	/* Nothing without a style thread. */
	*lends = 0;
	*waited_ns = 0;
	if (style == NULL)
		return;

	/* The counts. */
	wb_mutex_lock(&style->lock);

	*lends = style->lends;
	*waited_ns = style->waited_ns;

	wb_mutex_unlock(&style->lock);
}

/*
 * The style thread: makes its first call, marks itself a borrower of heaps,
 * then runs each request in order with its lender's state until the end.
 */
static void
style_run(
	void *argument)
{
	struct style_request *request;
	struct view_style *style;
	int result;

	/* The thread's first call and its mark: it runs no script and never collects. */
	style = argument;
	if (style->enter != NULL)
		style->enter(style->context);
	vm_thread_set_borrower(1);

	/* The queue, under the lock except while a work runs. */
	wb_mutex_lock(&style->lock);

	/* Request after request, until the end with an empty queue. */
	for (;;) {
		/* The next request, or the end. */
		while (style->first == NULL && !style->ending)
			wb_cond_wait(&style->asked, &style->lock);
		if (style->first == NULL)
			break;

		/* The first request leaves the queue. */
		request = style->first;
		style->first = request->next;
		if (style->first == NULL)
			style->last = NULL;

		wb_mutex_unlock(&style->lock);

		/* The work, with its lender's state. */
		css_animation_off(request->animation_off);
		layout_try_remember(request->remember);
		result = request->work(request->argument);

		/* Its lender hears it ran. */
		wb_mutex_lock(&style->lock);

		request->result = result;
		request->done = 1;
		wb_cond_broadcast(&style->answered);
	}

	wb_mutex_unlock(&style->lock);
}
