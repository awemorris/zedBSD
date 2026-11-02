/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The front of a view of its own threads (BROWSER_THREADING_OWN), shared by
 * view.c and front.c and by nobody else.
 *
 * The view's pages, its loader and everything view.c keeps live on the
 * view's script thread, which front.c starts and which runs view.c as a
 * view of the caller's thread runs it.  The program's thread reaches the
 * view through the front only: each public call of view.c first asks
 * view_front_window, and when it is the program's thread hands the call to
 * front.c, which queues it in the view's mailbox (an input, a command) or
 * answers from the state the script thread published last (a getter).
 * The script thread takes the mailbox in order, each entry a task of the
 * view's agent, and publishes the state, the notifications for the
 * program's callbacks and, when the page changed, a committed frame after
 * each round.  The drawing thread draws the latest committed frame
 * (browser_view_render, _render_pixels) and tells of each present
 * (browser_view_presented), a chance for the page's animation frames.
 */

#ifndef KEILAND_BROWSER_VIEW_FRONT_H
#define KEILAND_BROWSER_VIEW_FRONT_H

#include <browser/browser.h>

#include "paint/paint.h"
#include "vm/vm.h"

#include <stddef.h>
#include <stdint.h>

/* The most bytes of the focused field's value the published state keeps (browser_view_text_context). */
#define VIEW_SNAPSHOT_TEXT_MAX	8192U

struct view_front;
struct net_loader;

/*
 * A committed frame of the page: the scene of its display list (shared by
 * the frames of the same list), the scroll it is drawn at, the view's size
 * it was laid out for and its number (growing, from 1).  Nothing changes
 * it once made; refs counts its holders (the front's latest, a drawing
 * under way, the drawing thread's last) and is changed atomically.
 */
struct view_commit {
	unsigned refs;
	struct paint_scene *scene;
	layout_unit scroll_y;
	unsigned width;
	unsigned height;
	uint64_t serial;
};

/*
 * The state of a view the script thread publishes for the program's
 * getters: the page's location and title (NULL before the first page; the
 * title is the location when the page has none), the history's step shown
 * and its length, the load's progress, the scroll, the document's height and
 * how far it scrolls (range_error is the range's error, ENOENT without a
 * page), the view's size, the text input's session and target (has_target
 * with the caret) and what is around the caret (has_context with the
 * value's text, cut to VIEW_SNAPSHOT_TEXT_MAX, the caret's offset in it, the
 * purpose and the hints), and the state's number.
 */
struct view_snapshot {
	char *url;
	char *title;
	size_t history_index;
	size_t history_count;
	int progress;
	double scroll_y;
	double document_height;
	double largest_x;
	double largest_y;
	int range_error;
	unsigned width;
	unsigned height;
	uint64_t text_session;
	int has_target;
	float caret[4];
	int has_context;
	char *context_text;
	size_t cursor;
	int purpose;
	unsigned hints;
	uint64_t serial;
};

/* The front's side (front.c): the view of its own threads, made and ended. */
int view_front_create(const struct browser_view_options *options, struct browser_view **view);
void view_front_destroy(struct browser_view *view);

/* Whether a call into the view is the program's (1: the front takes it) or the view's own thread's (0). */
int view_front_window(const struct browser_view *view);

/* The program's commands and inputs, queued (front.c). */
int view_front_load(struct browser_view *view, const char *location, int follow);
int view_front_go(struct browser_view *view, int steps);
void view_front_stop(struct browser_view *view);
int view_front_resize(struct browser_view *view, unsigned width, unsigned height);
void view_front_color_scheme(struct browser_view *view, int scheme);
int view_front_scroll_to(struct browser_view *view, double x, double y);
int view_front_overscroll(struct browser_view *view, double dx, double dy);
int view_front_pointer_move(struct browser_view *view, float x, float y, uint32_t modifiers);
int view_front_pointer_button(struct browser_view *view, float x, float y, int button, int pressed, uint32_t modifiers);
int view_front_pointer_leave(struct browser_view *view);
int view_front_wheel(struct browser_view *view, float x, float y, float delta_x, float delta_y, uint32_t modifiers);
int view_front_key(struct browser_view *view, const char *key, const char *code, const char *text, int pressed, int repeat, uint32_t modifiers);
int view_front_focus(struct browser_view *view, int focused);
int view_front_focus_field(struct browser_view *view, const char *autocomplete);
int view_front_compose(struct browser_view *view, const char *preedit, int begin, int end);
int view_front_commit_text(struct browser_view *view, int checked, uint64_t session, const char *text, uint32_t delete_before, uint32_t delete_after);
int view_front_request_image(struct browser_view *view, float x, float y);

/* The task the view's pages run and how long, from any thread; ending it by its number, from the program's (front.c). */
int view_front_script_busy(const struct browser_view *view, uint32_t *task, uint32_t *milliseconds);
int view_front_stop_script(struct browser_view *view, uint32_t task);

/* The program's getters, from the published state (front.c). */
const struct view_snapshot *view_front_snapshot(const struct browser_view *view);
int view_front_text_context(const struct browser_view *view, char *text, size_t size, size_t *cursor, int *purpose, unsigned *hints);
void view_front_process(struct browser_view *view);

/* The drawing thread's side (front.c): the latest committed frame, held (NULL before the first), let go, and a present told. */
struct view_commit *view_front_commit(struct browser_view *view);
void view_commit_release(struct view_commit *commit);
void view_front_presented(struct browser_view *view, uint64_t serial);

/* How many entries wait in the view's mailbox now (front.c), from any thread. */
size_t view_front_queued(const struct browser_view *view);

/* How many works the view's pages lent to its style thread, and how long they waited for them (front.c), from any thread. */
void view_front_lend_counts(const struct browser_view *view, uint64_t *lends, uint64_t *waited_ns);

/* The agent the view's pages run their tasks in, and the threads of their iframes (front.c). */
struct vm_agent *view_front_agent(struct view_front *front);
struct view_frames *view_front_frames(struct view_front *front);

/* The view's side (view.c): the front a view runs under, its loader and its state. */
void view_attach_front(struct browser_view *view, struct view_front *front);
struct view_front *view_front_of(const struct browser_view *view);
struct net_loader *view_loader(const struct browser_view *view);
int view_fill_snapshot(struct browser_view *view, struct view_snapshot *snapshot);
int view_commit_scene(struct browser_view *view, struct paint_scene **scene, layout_unit *scroll_y);
void view_frame_opportunity(struct browser_view *view);

#endif
