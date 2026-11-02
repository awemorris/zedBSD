/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The threads of the iframes of a view of its own threads (frames.c),
 * shared by front.c and view.c and by nobody else.
 *
 * Every iframe of the page shown (nested ones too) has a script thread of
 * its own, which runs its browsing context's tasks: its timers, the
 * callbacks of the requests it made (through a client of the view's loader
 * of its own) and its documents' loads.  The frames of a view are one
 * agent: their heap is one, and one thread at a time runs its tasks, the
 * one that holds the agent (view_frames_enter, _leave for the page's own
 * thread).  A thread waits for the agent between its tasks only, so that
 * no other thread's stack holds the heap's values while one collects.
 */

#ifndef KEILAND_BROWSER_VIEW_FRAMES_H
#define KEILAND_BROWSER_VIEW_FRAMES_H

#include <browser/browser.h>

#include "vm/vm.h"

#include <stddef.h>

struct view_frames;
struct page;
struct page_frame_calls;

/*
 * Makes the frames of a view (and tells them the view, once it is made,
 * before its first page): the agent its pages run in, the wake of the
 * page's own thread (ring_top) and the first call of each frame's thread
 * (enter), with their contexts.  Reports 0, ENOMEM.
 */
int view_frames_create(struct vm_agent *agent, void (*ring_top)(void *context), void (*enter)(void *context), void *context, struct view_frames **frames);
void view_frames_set_view(struct view_frames *frames, struct browser_view *view);

/* Ends the frames' threads and their clients before the view's loader goes, and lets the frames go (on the page's own thread). */
void view_frames_close(struct view_frames *frames);
void view_frames_destroy(struct view_frames *frames);

/* The page's own thread takes the agent for its tasks, and gives it back with the frames' due work woken. */
void view_frames_enter(struct view_frames *frames);
void view_frames_leave(struct view_frames *frames);

/* Wakes out of their waited requests the frames' threads (from any thread, as the view navigates or ends). */
void view_frames_abort_waited(struct view_frames *frames);

/* The frames of other agents (remote.c), the room for one more frame, a runner's wake, and the page's own thread's delivery of what they ask. */
struct view_remotes *view_frames_remotes(struct view_frames *frames);
int view_frames_room(struct view_frames *frames);
void view_frames_ring(struct view_frames *frames, void *runner);
void view_frames_deliver(struct view_frames *frames);

/* How many iframes have a thread now (from any thread). */
size_t view_frames_count(struct view_frames *frames);

/* The calls a page gives its iframes their threads through (context: the view's frames). */
const struct page_frame_calls *view_frames_calls(void);

/* The page's clock of the view shown, in milliseconds (view.c), for the frames' timers; the agent is held. */
double view_page_time(const struct browser_view *view);

#endif
