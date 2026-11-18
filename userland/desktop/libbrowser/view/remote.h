/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frames of other agents of a view of its own threads (remote.c),
 * shared by frames.c, front.c and view.c and by nobody else.
 *
 * An iframe whose document is of another origin than its parent's (both
 * web origins) shows a frame of another agent: a page of its own (its
 * heap, its agent) on a browsing-context thread of its own (browser-script)
 * with a client of the view's loader of its own, which runs beside the
 * view's agent.  The view's agent reaches it by its number only: messages,
 * navigations and the questions its proxies ask go through the frames'
 * lock, and what it asks of the view's agent (its messages to its parent
 * and top, its navigations, the end of its load) waits in a queue that the
 * thread running its parent's document takes in a task of its own
 * (view_remotes_deliver).  It lays itself out at the size its parent gives
 * it, paints and commits a scene of its own, which the view's drawing
 * finds by its number (view_remotes_scene) where its parent's list has its
 * frame item.
 */

#ifndef KEILAND_BROWSER_VIEW_REMOTE_H
#define KEILAND_BROWSER_VIEW_REMOTE_H

#include <browser/browser.h>

#include "paint/paint.h"
#include "vm/vm.h"

#include <stddef.h>
#include <stdint.h>

struct view_remotes;
struct view_frames;
struct page;
struct page_pointer;
struct page_remote_response;
struct dom_element;
struct bind_remote_calls;

/*
 * Makes the frames of other agents of a view's frames (none yet), and
 * tells them the view (made after them).  Reports 0, ENOMEM.
 */
int view_remotes_create(struct view_frames *frames, struct view_remotes **remotes);
void view_remotes_set_view(struct view_remotes *remotes, struct browser_view *view);

/* Ends whatever frame is left (none should be: the pages went), and lets the record go (on the page's own thread). */
void view_remotes_close_all(struct view_remotes *remotes);
void view_remotes_destroy(struct view_remotes *remotes);

/* How many frames of other agents the view has now (from any thread). */
size_t view_remotes_count(struct view_remotes *remotes);

/* Delivers what the frames ask of the documents a thread runs (runner: frames.c's, NULL for the page's own), the agent held. */
void view_remotes_deliver(struct view_remotes *remotes, void *runner);

/* Wakes the frames' threads out of the requests they wait for and asks their tasks to end (the view navigates or ends; any thread). */
void view_remotes_abort(struct view_remotes *remotes);

/* The latest scene of a frame, held, and its scroll (paint_frame_lookup; any thread). */
struct paint_scene *view_remotes_scene(void *context, uint64_t frame, layout_unit *scroll_x, layout_unit *scroll_y);

/*
 * Hands a pointer's or a key's input to a frame (its viewport's place for
 * a pointer), on the page's own thread: type is the DOM's event type
 * ("mousemove", "mousedown", "mouseup", "wheel", "keydown", "keyup").
 * Reports 0, or ENOENT when it has ended.
 */
int view_remotes_pointer(struct view_remotes *remotes, uint64_t frame, const char *type, const struct page_pointer *pointer);
int view_remotes_key(struct view_remotes *remotes, uint64_t frame, const char *type, const char *key, const char *code, int repeat, unsigned modifiers);

/* The calls a page gives its iframes of other origins through (frames.c's table holds them). */
int view_remotes_load(void *context, struct page *page, struct dom_element *element, const struct page_remote_response *response);
void view_remotes_close(void *context, struct dom_element *element);
uint64_t view_remotes_frame(void *context, const struct dom_element *element, int width, int height);
const struct bind_remote_calls *view_remotes_parent_calls(void);
extern const struct bind_remote_calls view_remote_parent_calls;

/*
 * The task of the frames that has run longest (1 with its number and how
 * long, 0 when none runs), and the end of a frame's task of a number asked
 * for (0, or ESRCH when no frame runs it), for the question of a page that
 * does not answer (any thread).
 */
int view_remotes_busy(struct view_remotes *remotes, uint32_t *task, uint32_t *milliseconds);
int view_remotes_stop(struct view_remotes *remotes, uint32_t task);

/* Makes a frame's page on its thread (view.c): the view's fonts, console, loader and instance, the size it is laid out at, its heap's stack to here. */
int view_make_remote_page(struct browser_view *view, const void *stack_base, int width, int height, int dark, struct page **page);

#endif
