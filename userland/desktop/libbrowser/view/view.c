/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The view (the component's interface, <browser/browser.h>): the page shown and
 * its session history, the page
 * being fetched, the scroll, the page's clock and the network, moved out
 * of the window (ws074-p054).  A file or data: page is read at once; an
 * http or https page after the first is fetched without blocking, and the
 * page shown stays until it has arrived (BROWSER_FETCH_DEFAULT; the other
 * ways of fetching read every page at once, or fetch the first one too).
 * The page's timers run on the monotonic clock from the time the page was
 * made, and a page its scripts or its arriving images changed is laid out
 * and painted again the next time its boxes are needed.  The headless
 * modes settle the view on a virtual clock instead, and draw or dump it.
 * On the caller's GPU the view keeps the renderer (paint/gpu.h) and a
 * framebuffer for each of the caller's image views it drew into.
 *
 * An http or https page that arrived is not parsed at once when it names
 * parser-blocking scripts (<script src>) on the web: they are fetched
 * through the loader first, and the page is made once they all ended, so
 * that the parser finds them instead of reading each at once and blocking
 * the caller's window while the network works (BUG-207).
 *
 * The caller's input (ws074-p056) goes to the page as DOM events
 * (page/input.c); the view does the default actions the page's scripts
 * leave uncanceled: scrolling, following a link clicked or activated with
 * Enter, moving the focus with Tab, and the history's, reloading's and
 * stopping's keys and buttons.
 *
 * ws081-p006: the caller may also place the scroll itself (a touch
 * screen's scroller, which glides after a flick), learn how far the page
 * scrolls, and have the content drawn shifted past an end of the document
 * (the overscroll of a rubber band), the gap showing the page's canvas.
 */

#include "net/net.h"
#include "page/page.h"
#include "paint/gpu.h"

#include <browser/browser.h>

#include <errno.h>
#include <math.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The most pages the history remembers (the oldest is forgotten first). */
#define VIEW_HISTORY_MAX	64U

/* How much of the view a page scroll keeps in view, in pixels. */
#define VIEW_PAGE_OVERLAP	40

/* The longest a timer is waited for at once, in milliseconds. */
#define VIEW_WAIT_MAX		60000.0

/* How many of the loader's descriptors a headless settle polls at most. */
#define VIEW_POLL_MAX		64U

/* How many of the caller's image views the view keeps a framebuffer for (a swapchain's images, and a few more). */
#define VIEW_FRAMEBUFFERS_MAX	8U

/* How far a press of an arrow key scrolls, in pixels. */
#define VIEW_LINE_SCROLL	40

/* How far the pointer may move between a press and its release for a click, in pixels. */
#define VIEW_CLICK_SLOP		4.0f

/* How much room is kept around an element the focus scrolls into view, in pixels. */
#define VIEW_FOCUS_MARGIN	16

/* How far past an end the caller may shift the content, as a share of the view's height. */
#define VIEW_OVERSCROLL_MAX	1.0f

/* Where a scroll to an end of the document goes. */
#define VIEW_SCROLL_TOP		0
#define VIEW_SCROLL_BOTTOM	1

/* How a page is reached: a new step of the history, or a step already in it. */
enum view_step {
	VIEW_STEP_NEW,
	VIEW_STEP_KEEP
};

/* The keys with a default action, by what the action is. */
enum view_key {
	VIEW_KEY_OTHER,
	VIEW_KEY_BACK,
	VIEW_KEY_FORWARD,
	VIEW_KEY_REFRESH,
	VIEW_KEY_STOP,
	VIEW_KEY_R,
	VIEW_KEY_TAB,
	VIEW_KEY_ENTER,
	VIEW_KEY_LEFT,
	VIEW_KEY_RIGHT,
	VIEW_KEY_UP,
	VIEW_KEY_DOWN,
	VIEW_KEY_PAGE_UP,
	VIEW_KEY_PAGE_DOWN,
	VIEW_KEY_SPACE,
	VIEW_KEY_HOME,
	VIEW_KEY_END
};

/* A DOM key name with a default action, and which action. */
struct view_key_name {
	const char *name;
	int key;
};

/*
 * A framebuffer the view made over one of the caller's image views, at the
 * size it was made for, in the renderer's pass.  It lives until the caller
 * has the view forget its targets, or the renderer is made again.
 */
struct view_framebuffer {
	VkImageView image_view;
	uint32_t width;
	uint32_t height;
	VkFramebuffer framebuffer;
};

/*
 * An offscreen image of the engine's own: the renderer's device and image,
 * made by browser_offscreen_create and ended by browser_offscreen_destroy.
 */
struct browser_offscreen {
	struct paint_offscreen offscreen;
};

/*
 * One parser-blocking script fetched before its document is parsed
 * (BUG-207): the view it is for, its location, its request (NULL once it
 * ended), how it ended (0, or an errno value) and the bytes it brought.
 */
struct view_prefetch_script {
	struct browser_view *view;
	char *location;
	struct net_request *request;
	int error;
	struct wb_buffer bytes;
};

/*
 * A view: the page shown and its location, the history of locations
 * (index is the one shown), how far the page is scrolled (layout units),
 * how far the caller shifts the content past an end (overscroll_y, layout
 * units, positive down; the drawing's alone, zero on a new page),
 * the view's size and whether the page was laid out at another size
 * (resized), the fonts, the stack frame the pages' heaps scan up to, the
 * clock's time when the page shown (page_epoch) and the page being made
 * (open_epoch) began, how pages are fetched, the loader (NULL when every
 * page is read at once), the page being fetched (its location and how it
 * joins the history; NULL when none), the title shown, and the callbacks.
 *
 * On the GPU: the caller's device (has_device says the caller lent one),
 * the renderer on it (opened at the first drawing, for the target's format
 * and final layout; gpu_open says it is), the framebuffers made for the
 * caller's image views (oldest first), and the Vulkan call that failed
 * last.
 *
 * The input: where the pointer is (pointer_inside says it is over the
 * view), the button held down and where (press_button is -1 when none;
 * its release nearby is a click), and whether the caller's program has the
 * focus (a new page starts with it).
 */
struct browser_view {
	struct page *page;
	char *path;
	char *history[VIEW_HISTORY_MAX];
	size_t history_count;
	size_t history_index;
	layout_unit scroll_y;
	layout_unit overscroll_y;
	unsigned width;
	unsigned height;
	int resized;
	struct text_font_paths fonts;
	const void *stack_base;
	uint64_t page_epoch;
	uint64_t open_epoch;
	enum browser_fetch fetch;
	struct net_loader *loader;
	struct net_request *pending;
	char *pending_path;
	int pending_step;
	/* The history destination is published only when this pending page commits. */
	size_t pending_index;
	/*
	 * The document that arrived and waits for its parser-blocking scripts
	 * (BUG-207): its bytes and final URL (NULL when none waits), the
	 * scripts fetched ahead (struct view_prefetch_script *) and how many
	 * of them have not ended.  pending_path and the history's step stay
	 * the page's until it is shown.
	 */
	struct wb_buffer arrived;
	char *arrived_url;
	struct wb_vector prefetches;
	size_t prefetch_waiting;
	struct wb_buffer title;
	struct browser_callbacks callbacks;
	struct browser_gpu device;
	int has_device;
	struct paint_gpu gpu;
	int gpu_open;
	VkFormat gpu_format;
	VkImageLayout gpu_layout;
	struct view_framebuffer framebuffers[VIEW_FRAMEBUFFERS_MAX];
	size_t framebuffer_count;
	struct browser_gpu_failure failure;
	float pointer_x;
	float pointer_y;
	int pointer_inside;
	int press_button;
	float press_x;
	float press_y;
	int has_focus;
};

/*
 * The DOM key names with a default action.  The table is constant for the
 * life of the program and ends with a NULL name.
 */
static const struct view_key_name view_key_names[] = {
	{ "BrowserBack", VIEW_KEY_BACK },
	{ "BrowserForward", VIEW_KEY_FORWARD },
	{ "BrowserRefresh", VIEW_KEY_REFRESH },
	{ "F5", VIEW_KEY_REFRESH },
	{ "BrowserStop", VIEW_KEY_STOP },
	{ "Escape", VIEW_KEY_STOP },
	{ "r", VIEW_KEY_R },
	{ "R", VIEW_KEY_R },
	{ "Tab", VIEW_KEY_TAB },
	{ "Enter", VIEW_KEY_ENTER },
	{ "ArrowLeft", VIEW_KEY_LEFT },
	{ "ArrowRight", VIEW_KEY_RIGHT },
	{ "ArrowUp", VIEW_KEY_UP },
	{ "ArrowDown", VIEW_KEY_DOWN },
	{ "PageUp", VIEW_KEY_PAGE_UP },
	{ "PageDown", VIEW_KEY_PAGE_DOWN },
	{ " ", VIEW_KEY_SPACE },
	{ "Home", VIEW_KEY_HOME },
	{ "End", VIEW_KEY_END },
	{ NULL, VIEW_KEY_OTHER }
};

static int view_dimensions(unsigned width, unsigned height);
static int view_pixels(const uint32_t *pixels, unsigned width, unsigned height, size_t stride);
static int view_target(const struct browser_target *target);
static uint64_t view_clock(void);
static int view_navigate(struct browser_view *view, const char *path, int step, size_t history_index);
static int view_make_page(struct browser_view *view, struct page **page);
static int view_open_page(struct browser_view *view, const char *path, struct page **page);
static int view_show_page(struct browser_view *view, struct page *page, const char *path, int step, size_t history_index);
static int view_start_load(struct browser_view *view, const char *path, int step, size_t history_index);
static void view_document_arrived(void *context, struct net_request *request);
static void view_stop_load(struct browser_view *view, int report);
static int view_prefetch_start(struct browser_view *view, const unsigned char *bytes, size_t length, const char *url);
static void view_prefetch_arrived(void *context, struct net_request *request);
static void view_prefetch_commit(struct browser_view *view);
static void view_prefetch_clear(struct browser_view *view);
static void view_prefetch_free(struct wb_vector *scripts);
static void view_commit_document(struct browser_view *view, const unsigned char *bytes, size_t length, const char *url, struct wb_vector *scripts);
static int view_update(struct browser_view *view);
static void view_settle_network(struct browser_view *view);
static void view_clamp_scroll(struct browser_view *view);
static void view_update_title(struct browser_view *view);
static void view_scroll(struct browser_view *view, layout_unit distance);
static void view_redraw(struct browser_view *view);
static void view_failed(struct browser_view *view, const char *url, int error, const char *reason);
static void view_console(void *context, int level, const char *text, size_t length);
static int view_gpu_ready(struct browser_view *view, const struct browser_target *target);
static void view_gpu_close(struct browser_view *view);
static int view_framebuffer(struct browser_view *view, const struct browser_target *target, VkFramebuffer *framebuffer);
static int view_gpu_failed(struct browser_view *view, const char *operation, VkResult result);
static void view_pointer_at(const struct browser_view *view, float x, float y, int button, uint32_t modifiers, struct page_pointer *pointer);
static unsigned view_bind_modifiers(uint32_t modifiers);
static void view_changed(struct browser_view *view);
static int view_click(struct browser_view *view, float x, float y, uint32_t modifiers);
static void view_follow_link(struct browser_view *view, const char *href);
static int view_key_default(struct browser_view *view, const char *key, uint32_t modifiers);
static int view_key_named(const char *key);
static int view_move_focus(struct browser_view *view, int backward);
static int view_activate(struct browser_view *view);
static int view_scroll_into_view(struct browser_view *view);
static void view_scroll_pages(struct browser_view *view, int pages);
static void view_scroll_to(struct browser_view *view, int place);
static layout_unit view_drawn_scroll(const struct browser_view *view);

/*
 * Makes a view with its loader and no page.
 */
int
browser_view_create(
	const struct browser_view_options *options,
	struct browser_view **view)
{
	struct browser_view *made;
	int error;

	/* Refuses a missing output slot before trying to publish into it. */
	if (view == NULL)
		return EINVAL;

	/* A refused creation leaves the caller without a partially owned view. */
	*view = NULL;
	if (options == NULL)
		return EINVAL;

	/* A program built for another version lays its options out otherwise. */
	if (options->version != BROWSER_API_VERSION)
		return ENOTSUP;

	/* Rejects dimensions that cannot be represented by the layout engine. */
	error = view_dimensions(options->width, options->height);
	if (error != 0)
		return error;

	/* Rejects unknown fetching policies rather than silently treating them as defaults. */
	if (options->fetch < BROWSER_FETCH_DEFAULT || options->fetch > BROWSER_FETCH_BACKGROUND)
		return EINVAL;

	/* The view, empty. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* The fonts the program names, and the system's for the ones it leaves out. */
	made->fonts.sans = TEXT_DEFAULT_SANS;
	made->fonts.mono = TEXT_DEFAULT_MONO;
	made->fonts.fallback = TEXT_DEFAULT_FALLBACK;
	if (options->fonts != NULL) {
		if (options->fonts->sans != NULL)
			made->fonts.sans = options->fonts->sans;
		if (options->fonts->mono != NULL)
			made->fonts.mono = options->fonts->mono;
		if (options->fonts->fallback != NULL)
			made->fonts.fallback = options->fonts->fallback;
	}

	/* The rest of the options. */
	made->stack_base = options->stack_base;
	made->width = options->width;
	made->height = options->height;
	made->fetch = options->fetch;
	made->press_button = -1;
	made->has_focus = 1;
	wb_buffer_init(&made->title);
	wb_buffer_init(&made->arrived);
	wb_vector_init(&made->prefetches, sizeof(struct view_prefetch_script *));
	if (options->callbacks != NULL)
		made->callbacks = *options->callbacks;

	/* The caller's GPU, when it lends one now. */
	if (options->gpu != NULL) {
		made->device = *options->gpu;
		made->has_device = 1;
	}

	/* A view that reads every page at once needs no loader. */
	if (made->fetch == BROWSER_FETCH_AT_ONCE) {
		*view = made;
		return 0;
	}

	/* The loader of the http and https pages and images. */
	error = page_net_create(&made->loader);
	if (error != 0) {
		free(made);
		return error;
	}

	/* Succeeded: the view has no page yet. */
	*view = made;
	return 0;
}

/* Ends a view: its objects on the caller's GPU, a load under way, the page, the history and the loader, in that order. */
void
browser_view_destroy(
	struct browser_view *view)
{
	size_t index;

	/* No view. */
	if (view == NULL)
		return;

	/* The renderer and the framebuffers on the caller's device. */
	view_gpu_close(view);

	/* A load under way, then the page and its path (its image requests go with it). */
	view_stop_load(view, 0);
	page_destroy(view->page);
	free(view->path);

	/* The history's paths. */
	for (index = 0; index < view->history_count; index++)
		free(view->history[index]);

	/* The loader, once no page uses it, then the view. */
	if (view->loader != NULL)
		page_net_destroy(view->loader);
	wb_buffer_release(&view->title);
	wb_buffer_release(&view->arrived);
	view_prefetch_free(&view->prefetches);
	free(view);
}

/*
 * Loads a location as a new step of the history: a path (relative ones
 * from the working directory) or a URL.  The first page is read at once
 * whatever it is; later http and https pages are fetched without blocking.
 */
int
browser_view_load(
	struct browser_view *view,
	const char *location)
{
	char directory[1024];
	struct wb_buffer path;
	char *found;
	int error;

	/* The working directory, which a relative path starts from, with a slash to make it a base. */
	found = getcwd(directory, sizeof(directory) - 1U);
	if (found == NULL)
		return errno;
	strcat(directory, "/");

	/* The location resolved against it, as a link would be. */
	wb_buffer_init(&path);
	error = page_resolve_location(directory, location, &path);
	if (error == 0)
		error = view_navigate(view, wb_buffer_string(&path), VIEW_STEP_NEW, view->history_index);
	wb_buffer_release(&path);
	if (error != 0)
		return error;

	/* Succeeded: the page is shown, or loading. */
	return 0;
}

/* Opens a link's or a typed location's target, resolved against the page shown, as a new step. */
int
browser_view_follow(
	struct browser_view *view,
	const char *target)
{
	struct wb_buffer path;
	int error;

	/* The target's location. */
	wb_buffer_init(&path);
	error = page_resolve_location(view->path, target, &path);
	if (error != 0) {
		wb_buffer_release(&path);
		return error;
	}

	/* Its page, as a new step of the history. */
	error = view_navigate(view, wb_buffer_string(&path), VIEW_STEP_NEW, view->history_index);
	wb_buffer_release(&path);
	if (error != 0)
		return error;

	/* Succeeded: the page is shown, or loading. */
	return 0;
}

/*
 * Tells whether the history has a step that far from the one shown (0 is the page shown, which can be reloaded).
 */
int
browser_view_can_go(
	const struct browser_view *view,
	int steps)
{
	/* Widens before negation so INT_MIN remains a defined history query. */
	if (steps < 0) {
		if (view->history_index < (size_t)-(int64_t)steps)
			return 0;

		/* Succeeded: the requested older step exists. */
		return 1;
	}

	/* A forward step must stay within the committed history. */
	if (steps > 0) {
		if (view->history_index + (size_t)steps >= view->history_count)
			return 0;

		/* Succeeded: the requested newer step exists. */
		return 1;
	}

	/* A reload requires a committed page. */
	if (view->history_count == 0)
		return 0;

	/* Succeeded: the current step can be reloaded. */
	return 1;
}

/*
 * Shows the page some steps back (negative), forward (positive), or the
 * same page again (0, a reload); a step outside the history does nothing.
 */
int
browser_view_go(
	struct browser_view *view,
	int steps)
{
	size_t index;
	int possible;
	int error;

	/* No step before the first or after the last. */
	possible = browser_view_can_go(view, steps);
	if (!possible)
		return 0;

	/* The step to show. */
	index = view->history_index;
	if (steps < 0)
		index -= (size_t)-(int64_t)steps;
	else
		index += (size_t)steps;

	/* Loads the destination without publishing its history index before success. */
	error = view_navigate(view, view->history[index], VIEW_STEP_KEEP, index);
	if (error != 0)
		return error;

	/* Succeeded: the page is shown, or loading. */
	return 0;
}

/* Stops the page being fetched, if any (the load callback hears BROWSER_LOAD_STOPPED). */
void
browser_view_stop(
	struct browser_view *view)
{
	/* The load, reported. */
	view_stop_load(view, 1);
}

/*
 * Gives the view a new size: the page is laid out at it again.
 */
int
browser_view_resize(
	struct browser_view *view,
	unsigned width,
	unsigned height)
{
	int error;

	/* A refused resize leaves both the view and its page at the previous size. */
	error = view_dimensions(width, height);
	if (error != 0)
		return error;

	/* The size, and the page's scripts see it. */
	view->width = width;
	view->height = height;
	if (view->page == NULL)
		return 0;
	page_set_viewport(view->page, (int)width, (int)height);

	/* The page is laid out at the new size when it is drawn next. */
	view->resized = 1;
	view_redraw(view);

	/* Succeeded: the page will fit the new size. */
	return 0;
}

/* The page shown's location (NULL before the first page). */
const char *
browser_view_url(
	const struct browser_view *view)
{
	/* The location kept for it. */
	return view->path;
}

/* The page shown's title, or its location when it has none. */
const char *
browser_view_title(
	const struct browser_view *view)
{
	/* A page without a title shows its location. */
	if (view->title.length == 0)
		return view->path;

	/* The title. */
	return wb_buffer_string(&view->title);
}

/* The height of the page shown's document in pixels (the page is laid out first when it changed). */
double
browser_view_document_height(
	struct browser_view *view)
{
	int error;

	/* No page is no height. */
	if (view->page == NULL)
		return 0.0;

	/* A page that cannot be laid out has no height either. */
	error = view_update(view);
	if (error != 0)
		return 0.0;

	/* The layout's. */
	return (double)layout_to_px(view->page->layout.document_height);
}

/* Lists the descriptors the view waits on and the events it waits for; returns how many. */
size_t
browser_view_poll_fds(
	const struct browser_view *view,
	struct pollfd *fds,
	size_t capacity)
{
	size_t count;

	/* The loader's (none without one). */
	count = 0;
	if (view->loader != NULL)
		count = page_net_poll_fds(view->loader, fds, capacity);

	/* The page's media engines' wakes after them (ws121-p004). */
	if (view->page != NULL && count < capacity)
		count += page_media_poll_fds(view->page, fds + count, capacity - count);
	return count;
}

/* Reports how long the caller may wait before calling browser_view_process (-1: until a descriptor is ready). */
int
browser_view_timeout(
	const struct browser_view *view)
{
	double due;
	double page_now;
	double wait;
	int timeout;
	int media;
	int found;

	/* The network's earliest time out (none without a loader). */
	timeout = -1;
	if (view->loader != NULL)
		timeout = page_net_timeout(view->loader);
	if (view->page == NULL)
		return timeout;

	/* A playing video's next picture (ws121-p004). */
	media = page_media_timeout(view->page);
	if (media >= 0 && (timeout < 0 || media < timeout))
		timeout = media;

	/* The page's next timer, in the page's own time. */
	found = page_next_timer(view->page, &due);
	if (!found)
		return timeout;
	page_now = (double)(view_clock() - view->page_epoch);
	wait = due - page_now;
	if (wait < 0.0)
		wait = 0.0;
	if (wait > VIEW_WAIT_MAX)
		wait = VIEW_WAIT_MAX;

	/* The sooner of the two. */
	if (timeout < 0 || (int)wait < timeout)
		timeout = (int)wait;
	return timeout;
}

/*
 * Does the work due after the caller's poll: the network (pages and
 * images that arrived), the page's timers, and the redraw and the new
 * title when its scripts or its images changed it.
 */
void
browser_view_process(
	struct browser_view *view,
	const struct pollfd *fds,
	size_t count)
{
	int changed;
	int error;

	/* The loader's work: requests that moved on, their callbacks. */
	if (view->loader != NULL)
		page_net_process(view->loader, fds, count);
	if (view->page == NULL)
		return;

	/* The timers due by now, in the page's own time. */
	error = page_set_time(view->page, (double)(view_clock() - view->page_epoch));
	if (error != 0 && view->callbacks.script_error != NULL)
		view->callbacks.script_error(view->callbacks.context, view, error);

	/* The media's engines: their states and the pictures due (ws121-p004). */
	page_media_process(view->page);

	/* A page the scripts, the images and the media left as it was needs nothing more. */
	changed = page_needs_layout(view->page);
	if (!changed)
		changed = page_needs_paint(view->page);
	if (!changed)
		return;

	/* The redraw, which lays the page out again, and the title the scripts may have set. */
	view_redraw(view);
	view_update_title(view);
}

/* How far the page is scrolled, in pixels. */
double
browser_view_scroll_y(
	const struct browser_view *view)
{
	/* The scroll in pixels. */
	return (double)layout_to_px(view->scroll_y);
}

/*
 * Places the scroll at a point of the document, in pixels, kept inside the
 * document (the page does not scroll sideways yet: x is not used).  The
 * page gets no wheel event; a change is drawn again.  Reports ENOENT when
 * no page is shown, or the layout's error.
 */
int
browser_view_scroll_to(
	struct browser_view *view,
	double x,
	double y)
{
	layout_unit before;
	int error;

	UNUSED_PARAMETER(x);

	/* No page, no scroll. */
	if (view->page == NULL)
		return ENOENT;

	/* The document as it is laid out now, which the scroll stays inside. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* The new place, within the document. */
	before = view->scroll_y;
	view->scroll_y = layout_from_px((float)y);
	view_clamp_scroll(view);

	/* A change is drawn again. */
	if (view->scroll_y != before)
		view_redraw(view);

	/* Succeeded: the scroll is placed. */
	return 0;
}

/*
 * Reports how far the page scrolls at most, in pixels: the document as it
 * is laid out now less the view (0 when it fits; sideways always 0, the
 * page does not scroll sideways yet).  Reports ENOENT when no page is
 * shown, or the layout's error.
 */
int
browser_view_scroll_range(
	struct browser_view *view,
	double *largest_x,
	double *largest_y)
{
	layout_unit limit;
	int error;

	/* Nothing scrolls without a page. */
	*largest_x = 0.0;
	*largest_y = 0.0;
	if (view->page == NULL)
		return ENOENT;

	/* The document as it is laid out now. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* The document's height less the view's. */
	limit = view->page->layout.document_height - (layout_unit)view->height * LAYOUT_UNIT;
	if (limit < 0)
		limit = 0;
	*largest_y = (double)layout_to_px(limit);

	/* Succeeded: the range is reported. */
	return 0;
}

/*
 * Draws the content shifted by a distance in pixels (positive down and
 * right) without moving the scroll: a rubber band past an end of the
 * document, the gap showing the page's canvas.  The shift is at most the
 * view's height either way (the page does not scroll sideways yet: dx is
 * not used), is the drawing's alone (the pointer and the page's events
 * are placed without it), and goes with the page (a new page starts
 * without it).  A change is drawn again.
 */
int
browser_view_set_overscroll(
	struct browser_view *view,
	double dx,
	double dy)
{
	layout_unit shift;
	float limit;
	float distance;

	UNUSED_PARAMETER(dx);

	/* At most the view's height either way. */
	limit = (float)view->height * VIEW_OVERSCROLL_MAX;
	distance = (float)dy;
	if (distance > limit)
		distance = limit;
	if (distance < -limit)
		distance = -limit;
	shift = layout_from_px(distance);

	/* The same shift needs nothing. */
	if (shift == view->overscroll_y)
		return 0;

	/* The new shift, drawn. */
	view->overscroll_y = shift;
	if (view->page != NULL)
		view_redraw(view);

	/* Succeeded: the content is drawn shifted. */
	return 0;
}

/* The pointer moves over the view: its place is kept for the wheel and the buttons. */
int
browser_view_pointer_move(
	struct browser_view *view,
	float x,
	float y,
	uint32_t modifiers)
{
	UNUSED_PARAMETER(modifiers);

	/* The place, and the pointer is over the view. */
	view->pointer_x = x;
	view->pointer_y = y;
	view->pointer_inside = 1;

	/* Succeeded: the place is kept. */
	return 0;
}

/*
 * A pointer button is pressed or let go at a place in the view: the page
 * gets mousedown or mouseup there.  Unless they are canceled, a press of
 * the main button moves the focus to what it lands on; a release of the
 * button pressed near its press is a click (the main button's click
 * follows the link under it), or a step through the history (the back and
 * forward buttons).
 */
int
browser_view_pointer_button(
	struct browser_view *view,
	float x,
	float y,
	int button,
	int pressed,
	uint32_t modifiers)
{
	struct page_pointer pointer;
	float distance_x;
	float distance_y;
	int clicked;
	int canceled;
	int error;

	/* The place, which the pointer is at now. */
	view->pointer_x = x;
	view->pointer_y = y;
	view->pointer_inside = 1;

	/* A release is a click when the same button went down nearly there. */
	clicked = 0;
	if (!pressed && view->press_button == button) {
		distance_x = fabsf(x - view->press_x);
		distance_y = fabsf(y - view->press_y);
		if (distance_x <= VIEW_CLICK_SLOP && distance_y <= VIEW_CLICK_SLOP)
			clicked = 1;
	}

	/* A press remembers its button and place; any release ends the press. */
	view->press_button = -1;
	if (pressed) {
		view->press_button = button;
		view->press_x = x;
		view->press_y = y;
	}

	/* The page gets mousedown or mouseup (there is nothing to press without a page). */
	canceled = 0;
	if (view->page != NULL) {
		error = view_update(view);
		if (error != 0)
			return error;
		view_pointer_at(view, x, y, button, modifiers, &pointer);
		if (pressed)
			error = page_mouse_event(view->page, "mousedown", &pointer, &canceled);
		else
			error = page_mouse_event(view->page, "mouseup", &pointer, &canceled);
		if (error != 0)
			return error;
	}

	/* The press of the main button moves the focus, unless mousedown was canceled. */
	if (pressed &&
	    !canceled &&
	    button == BROWSER_BUTTON_PRIMARY &&
	    view->page != NULL) {
		error = page_focus_at(view->page, pointer.x, pointer.y);
		if (error != 0)
			return error;
	}

	/* The page as the listeners and the focus left it is drawn again. */
	view_changed(view);

	/* A release that is not a click, or whose mouseup was canceled, does nothing more. */
	if (!clicked || canceled)
		return 0;

	/* The main button's click. */
	if (button == BROWSER_BUTTON_PRIMARY) {
		error = view_click(view, x, y, modifiers);
		if (error != 0)
			return error;
		return 0;
	}

	/* The back and forward buttons step through the history (a failure goes to the load callback). */
	if (button == BROWSER_BUTTON_BACK)
		(void)browser_view_go(view, -1);
	if (button == BROWSER_BUTTON_FORWARD)
		(void)browser_view_go(view, 1);

	/* Succeeded: the button is carried out. */
	return 0;
}

/* The pointer leaves the view: a press under way is no longer a click. */
int
browser_view_pointer_leave(
	struct browser_view *view)
{
	/* Nothing is pressed or pointed at any more. */
	view->pointer_inside = 0;
	view->press_button = -1;

	/* Succeeded: the pointer is gone. */
	return 0;
}

/*
 * The wheel turns over a place in the view: the page gets a wheel event
 * there, and unless it is canceled the page scrolls by the vertical
 * distance.
 */
int
browser_view_wheel(
	struct browser_view *view,
	float x,
	float y,
	float delta_x,
	float delta_y,
	uint32_t modifiers)
{
	struct page_pointer pointer;
	int canceled;
	int error;

	/* The place, which the pointer is at now. */
	view->pointer_x = x;
	view->pointer_y = y;
	view->pointer_inside = 1;

	/* No page, nothing to scroll. */
	if (view->page == NULL)
		return 0;

	/* The boxes the wheel is over are the page's as it is now. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* The page's wheel event. */
	view_pointer_at(view, x, y, BROWSER_BUTTON_PRIMARY, modifiers, &pointer);
	pointer.button = 0;
	pointer.delta_x = (double)delta_x;
	pointer.delta_y = (double)delta_y;
	error = page_wheel_event(view->page, &pointer, &canceled);
	if (error != 0)
		return error;
	view_changed(view);

	/* A canceled wheel does not scroll. */
	if (canceled)
		return 0;

	/* The scroll, by the vertical distance (the page does not scroll sideways yet). */
	view_scroll(view, layout_from_px(delta_y));

	/* Succeeded: the wheel is carried out. */
	return 0;
}

/*
 * A key is pressed (repeat when held) or let go: the page gets keydown or
 * keyup at its focused element, and keypress for a key that types text;
 * unless keydown is canceled, the view does the key's default action.
 */
int
browser_view_key(
	struct browser_view *view,
	const char *key,
	const char *code,
	const char *text,
	int pressed,
	int repeat,
	uint32_t modifiers)
{
	struct bind_key event;
	struct bind_key typed;
	int canceled;
	int handled;
	int error;

	/* The key as the page sees it (a missing name is empty). */
	memset(&event, 0, sizeof(event));
	event.key = "";
	if (key != NULL)
		event.key = key;
	event.code = "";
	if (code != NULL)
		event.code = code;
	event.repeat = repeat;
	event.modifiers = view_bind_modifiers(modifiers);

	/* keydown or keyup, at the focused element (there is no page to tell before the first). */
	canceled = 0;
	if (view->page != NULL) {
		if (pressed)
			error = page_key_event(view->page, "keydown", &event, &canceled);
		else
			error = page_key_event(view->page, "keyup", &event, &canceled);
		if (error != 0)
			return error;
		view_changed(view);
	}

	/* A release, or a canceled keydown, has no default action. */
	if (!pressed || canceled)
		return 0;

	/* A key that types text without Control, Alt or Meta gets keypress too, with the text as its key. */
	if (view->page != NULL &&
	    text != NULL &&
	    text[0] != '\0' &&
	    (modifiers & (BROWSER_MOD_CTRL | BROWSER_MOD_ALT | BROWSER_MOD_META)) == 0U) {
		typed = event;
		typed.key = text;
		error = page_key_event(view->page, "keypress", &typed, &canceled);
		if (error != 0)
			return error;
		view_changed(view);

		/* A canceled keypress types nothing, and has no default action either. */
		if (canceled)
			return 0;
	}

	/* A focused text control takes the text and its editing keys. */
	if (view->page != NULL) {
		error = page_edit_key(view->page, event.key, text, event.modifiers, &handled);
		if (error != 0)
			return error;
		if (handled) {
			view_changed(view);
			return 0;
		}
	}

	/* The key's default action. */
	error = view_key_default(view, event.key, modifiers);
	if (error != 0)
		return error;

	/* Succeeded: the key is carried out. */
	return 0;
}

/*
 * The caller's program gains or loses the focus: the page's focused
 * element hears of it, and its ring shows only while the view has it.
 */
int
browser_view_focus(
	struct browser_view *view,
	int focused)
{
	int error;

	/* The view's own record, which a new page starts from. */
	view->has_focus = focused;
	if (view->page == NULL)
		return 0;

	/* The page's. */
	error = page_window_focus(view->page, focused);
	if (error != 0)
		return error;
	view_changed(view);

	/* Succeeded: the focus is told. */
	return 0;
}

/*
 * Moves the focus to the first control that takes text and whose
 * autocomplete attribute holds a token ("one-time-code"), with its ring,
 * and scrolls it into view.  Returns 0 when one has the focus, ENOENT
 * when the page has none, or an errno value when the page's scripts or
 * its layout failed.
 */
int
browser_view_focus_field(
	struct browser_view *view,
	const char *autocomplete)
{
	int focused;
	int error;

	/* No page has no field. */
	if (view->page == NULL)
		return ENOENT;

	/* The page laid out as it is now (only drawn controls are taken). */
	error = view_update(view);
	if (error != 0)
		return error;

	/* The control, focused. */
	focused = page_focus_field(view->page, autocomplete);
	if (focused < 0)
		return -focused;
	if (focused == 0)
		return ENOENT;
	view_changed(view);

	/* In view. */
	error = view_scroll_into_view(view);
	if (error != 0)
		return error;

	/* Succeeded: the control has the focus. */
	return 0;
}

/*
 * Gives what an input method reads around the caret: the focused
 * control's value (cut around the caret to the room), the caret's byte
 * offset, its purpose and its hints.  Returns 1 with them, 0 when the
 * focus takes no text or there was no memory.
 */
int
browser_view_text_context(
	struct browser_view *view,
	char *text,
	size_t size,
	size_t *cursor,
	int *purpose,
	unsigned *hints)
{
	struct wb_buffer value;
	size_t caret;
	size_t start;
	size_t length;
	int page_purpose;
	unsigned page_hints;
	int found;

	/* Nothing yet. */
	*cursor = 0;
	*purpose = BROWSER_TEXT_PURPOSE_NORMAL;
	*hints = 0U;
	if (size != 0U)
		text[0] = '\0';
	if (view->page == NULL || size == 0U)
		return 0;

	/* The page's control. */
	wb_buffer_init(&value);
	found = page_compose_context(view->page, &value, &caret, &page_purpose, &page_hints);
	if (found != 1) {
		wb_buffer_release(&value);
		return 0;
	}

	/* The part around the caret that fits: from up to half the room before it, at a character's start. */
	start = 0;
	length = value.length;
	if (length > size - 1U) {
		if (caret > (size - 1U) / 2U)
			start = caret - (size - 1U) / 2U;
		if (start + size - 1U > length)
			start = length - (size - 1U);
		while (start < caret && (value.data[start] & 0xc0U) == 0x80U)
			start++;
		length = start + size - 1U;
		if (length > value.length)
			length = value.length;
		while (length > caret && length < value.length && (value.data[length] & 0xc0U) == 0x80U)
			length--;
		length -= start;
	}

	/* Given to the program. */
	if (length != 0U)
		memcpy(text, value.data + start, length);
	text[length] = '\0';
	*cursor = caret - start;
	*purpose = page_purpose;
	*hints = page_hints;
	wb_buffer_release(&value);

	/* Succeeded: the focus takes text. */
	return 1;
}

/*
 * Tells whether the focused element takes an input method's text: a text
 * field or a textarea that takes typing (a password field does not).
 * caret gets the caret's rectangle in the view's pixels as the last
 * drawing placed it (x, y, width, height; zero before the control is
 * drawn).
 */
int
browser_view_text_target(
	struct browser_view *view,
	float caret[4])
{
	struct dom_element *element;
	struct dom_control *control;
	layout_unit top;

	/* No caret until one is found. */
	caret[0] = 0.0f;
	caret[1] = 0.0f;
	caret[2] = 0.0f;
	caret[3] = 0.0f;
	if (view->page == NULL)
		return 0;

	/* The control an input method composes in, if the focus is in one. */
	element = page_compose_element(view->page);
	if (element == NULL)
		return 0;

	/* Its caret where the display list drew it, below the scroll the page is drawn at. */
	control = element->control;
	if (control != NULL && control->drawn) {
		top = control->caret_top - view_drawn_scroll(view);
		caret[0] = layout_to_px(control->caret_x);
		caret[1] = layout_to_px(top);
		caret[2] = 1.0f;
		caret[3] = layout_to_px(control->caret_height);
	}

	/* The focus takes an input method's text. */
	return 1;
}

/*
 * Shows the text an input method is composing at the focused control's
 * caret, underlined, without changing its value (an empty text ends it):
 * begin and end are its cursor's byte offsets, -1 when hidden (the caret
 * is drawn at end, or at the text's end when hidden).
 */
int
browser_view_compose(
	struct browser_view *view,
	const char *preedit,
	int begin,
	int end)
{
	int cursor;
	int error;

	/* No page has nothing to compose in. */
	if (view->page == NULL)
		return 0;

	/* The cursor: its end, its begin, or none (the text's end); the chosen part runs from begin to end when both are given. */
	cursor = end;
	if (cursor < 0) {
		cursor = begin;
		begin = -1;
	}

	/* A missing text ends the composing. */
	if (preedit == NULL)
		preedit = "";

	/* The control shows it, drawn again. */
	error = page_compose(view->page, preedit, begin, cursor);
	if (error != 0)
		return error;
	view_changed(view);

	/* Succeeded: the composed text shows. */
	return 0;
}

/*
 * Commits an input method's text to the focused control: what it was
 * composing goes, delete_before and delete_after bytes of UTF-8 are deleted
 * before and after the caret, and the text goes in at the caret, which
 * fires input.  Reports 0, or why the page's scripts failed.
 */
int
browser_view_commit_text(
	struct browser_view *view,
	const char *text,
	uint32_t delete_before,
	uint32_t delete_after)
{
	int error;

	/* No page has nothing to commit to. */
	if (view->page == NULL)
		return 0;

	/* A missing text commits only the deletion. */
	if (text == NULL)
		text = "";

	/* The control takes it, drawn again with what its input listeners did. */
	error = page_commit_text(view->page, text, delete_before, delete_after);
	view_changed(view);
	if (error != 0)
		return error;

	/* Succeeded: the text is committed. */
	return 0;
}

/*
 * Brings the page to rest for a headless caller: the page being fetched
 * arrives, the page's timers run on a virtual clock up to budget
 * milliseconds, and with BROWSER_SETTLE_LAYOUT the page is laid out, the
 * images its layout asked for arrive, and it is laid out again with them.
 * Returns ENOENT when no page is shown (its load failed, which the load
 * callback heard of), or why the scripts or the layout failed.
 */
int
browser_view_settle(
	struct browser_view *view,
	double budget,
	unsigned flags)
{
	int error;

	/* The page being fetched, if any, arrives or fails. */
	view_settle_network(view);
	if (view->page == NULL)
		return ENOENT;

	/* The page's timers, on the virtual clock. */
	error = page_settle(view->page, budget);
	if (error != 0)
		return error;

	/* The DOM and the style need no layout. */
	if ((flags & BROWSER_SETTLE_LAYOUT) == 0U)
		return 0;

	/* The layout, which asks for the page's images. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* The images that arrive, and the layout again with them. */
	view_settle_network(view);
	error = view_update(view);
	if (error != 0)
		return error;

	/* Succeeded: the page is at rest. */
	return 0;
}

/*
 * Draws the page shown with the CPU renderer into the caller's pixels:
 * width by height pixels of 0xAARRGGBB, rows stride bytes apart, the page
 * scrolled as the view is.
 */
int
browser_view_draw_pixels(
	struct browser_view *view,
	uint32_t *pixels,
	unsigned width,
	unsigned height,
	size_t stride)
{
	struct paint_bitmap bitmap;
	unsigned char *row_start;
	unsigned row;
	int error;

	/* Refuses an output whose rows overlap or wrap the address space. */
	error = view_pixels(pixels, width, height, stride);
	if (error != 0)
		return error;

	/* No page has nothing to draw. */
	if (view->page == NULL)
		return ENOENT;

	/* The page as it is now. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* Rows packed one after another are drawn in place. */
	if (stride == (size_t)width * sizeof(uint32_t)) {
		bitmap.pixels = pixels;
		bitmap.width = (int)width;
		bitmap.height = (int)height;
		error = paint_software(&view->page->paint, &view->page->text, view_drawn_scroll(view), &bitmap);
		if (error != 0)
			return error;

		/* Succeeded: the pixels hold the page. */
		return 0;
	}

	/* Rows further apart are drawn into a packed bitmap first. */
	error = paint_bitmap_create(&bitmap, (int)width, (int)height);
	if (error != 0)
		return error;
	error = paint_software(&view->page->paint, &view->page->text, view_drawn_scroll(view), &bitmap);
	if (error != 0) {
		paint_bitmap_release(&bitmap);
		return error;
	}

	/* Then copied into the caller's rows. */
	for (row = 0; row < height; row++) {
		row_start = (unsigned char *)pixels + (size_t)row * stride;
		memcpy(row_start, bitmap.pixels + (size_t)row * width, (size_t)width * sizeof(uint32_t));
	}

	/* The packed bitmap is no longer needed. */
	paint_bitmap_release(&bitmap);

	/* Succeeded: the pixels hold the page. */
	return 0;
}

/*
 * Writes one of the page shown's text dumps (its DOM, its computed style,
 * its layout or its display list) into memory the caller frees.
 */
int
browser_view_dump(
	struct browser_view *view,
	enum browser_dump kind,
	char **text,
	size_t *length)
{
	struct wb_buffer out;
	int error;

	/* No page has nothing to dump. */
	*text = NULL;
	*length = 0;
	if (view->page == NULL)
		return ENOENT;

	/* The layout and the display list are the page's as it is now. */
	if (kind == BROWSER_DUMP_LAYOUT || kind == BROWSER_DUMP_PAINT) {
		error = view_update(view);
		if (error != 0)
			return error;
	}

	/* The dump, as the kind asks. */
	wb_buffer_init(&out);
	if (kind == BROWSER_DUMP_DOM) {
		error = page_dump_dom(view->page, &out);
	} else if (kind == BROWSER_DUMP_STYLE) {
		error = page_dump_style(view->page, &out);
	} else if (kind == BROWSER_DUMP_LAYOUT) {
		error = layout_dump(&view->page->layout, &out);
	} else {
		error = paint_dump(&view->page->paint, &out);
	}

	/* A dump that could not be written. */
	if (error != 0) {
		wb_buffer_release(&out);
		return error;
	}

	/* An empty dump still gives the caller a string. */
	if (out.data == NULL) {
		error = wb_buffer_append_string(&out, "");
		if (error != 0)
			return error;
	}

	/* Succeeded: the caller owns the text. */
	*text = (char *)out.data;
	*length = out.length;
	return 0;
}

/*
 * Gives the view the caller's GPU to draw with from now on, or none (NULL):
 * what the view made on the old device is released first, so the caller
 * may destroy that device afterwards.
 */
void
browser_view_set_gpu(
	struct browser_view *view,
	const struct browser_gpu *gpu)
{
	/* The renderer and the framebuffers on the old device. */
	view_gpu_close(view);

	/* No GPU from now on. */
	if (gpu == NULL) {
		view->has_device = 0;
		return;
	}

	/* The new one; the renderer is made on it at the next drawing. */
	view->device = *gpu;
	view->has_device = 1;
}

/*
 * Forgets the framebuffers the view made for the caller's image views; the
 * caller calls it before it destroys those views, once the drawing into
 * them has finished.
 */
void
browser_view_release_targets(
	struct browser_view *view)
{
	size_t index;

	/* Each framebuffer, oldest first. */
	for (index = 0; index < view->framebuffer_count; index++)
		vkDestroyFramebuffer(view->device.device, view->framebuffers[index].framebuffer, NULL);
	view->framebuffer_count = 0;
}

/* Lays the page out now when it changed, so that a frame the caller begins next cannot fail on the layout. */
int
browser_view_prepare(
	struct browser_view *view)
{
	int error;

	/* No page has nothing to lay out. */
	if (view->page == NULL)
		return ENOENT;

	/* The page as it is now. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* Succeeded: the page can be drawn. */
	return 0;
}

/*
 * Draws the page shown into the caller's image on the caller's GPU and
 * waits for it: submitted after wait and signalling signal
 * (VK_NULL_HANDLE for none).
 */
int
browser_view_draw(
	struct browser_view *view,
	const struct browser_target *target,
	VkSemaphore wait,
	VkSemaphore signal)
{
	VkFramebuffer framebuffer;
	VkExtent2D extent;
	VkResult result;
	int error;

	/* Refuses an absent or empty target before issuing any Vulkan command. */
	error = view_target(target);
	if (error != 0)
		return error;

	/* No page has nothing to draw. */
	if (view->page == NULL)
		return ENOENT;

	/* The page as it is now. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* The renderer for the target's format and layout, and the framebuffer over the target. */
	error = view_gpu_ready(view, target);
	if (error != 0)
		return error;
	error = view_framebuffer(view, target, &framebuffer);
	if (error != 0)
		return error;

	/* The drawing, submitted and waited for. */
	extent.width = target->width;
	extent.height = target->height;
	result = paint_gpu_draw(&view->gpu, &view->page->paint, &view->page->text, view_drawn_scroll(view), framebuffer, extent, wait, signal);
	if (result != VK_SUCCESS) {
		error = view_gpu_failed(view, view->gpu.operation, result);
		return error;
	}

	/* Succeeded: the image holds the page. */
	return 0;
}

/*
 * Records the drawing of the page shown into the caller's image, render
 * pass and all, into a command buffer the caller began and submits.  The
 * drawing recorded before must have finished.
 */
int
browser_view_record(
	struct browser_view *view,
	const struct browser_target *target,
	VkCommandBuffer commands)
{
	VkFramebuffer framebuffer;
	VkExtent2D extent;
	VkResult result;
	int error;

	/* Refuses an absent or empty target before issuing any Vulkan command. */
	error = view_target(target);
	if (error != 0)
		return error;

	/* No page has nothing to draw. */
	if (view->page == NULL)
		return ENOENT;

	/* The page as it is now. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* The renderer for the target's format and layout, and the framebuffer over the target. */
	error = view_gpu_ready(view, target);
	if (error != 0)
		return error;
	error = view_framebuffer(view, target, &framebuffer);
	if (error != 0)
		return error;

	/* The frame's instances and atlas, written by the host now. */
	extent.width = target->width;
	extent.height = target->height;
	result = paint_gpu_prepare(&view->gpu, &view->page->paint, &view->page->text, view_drawn_scroll(view), extent);
	if (result != VK_SUCCESS) {
		error = view_gpu_failed(view, view->gpu.operation, result);
		return error;
	}

	/* The pass, into the caller's command buffer. */
	paint_gpu_record(&view->gpu, commands, &view->page->paint, framebuffer, extent);

	/* Succeeded: the caller's commands draw the page. */
	return 0;
}

/*
 * Finds the image shown at a place of the view (ws189-p003: a picture
 * dragged out of the browser): a copy of its pixels, premultiplied
 * 0xAARRGGBB words (the decoder's are straight), and the absolute URL of
 * its source.  browser_view_image_release frees them.  Returns 0, ENOENT
 * when no image is there, or ENOMEM.
 */
int
browser_view_image_at(
	struct browser_view *view,
	float x,
	float y,
	struct browser_image *image)
{
	const struct img_bitmap *bitmap;
	struct page_pointer pointer;
	struct wb_buffer source;
	const char *url;
	uint32_t pixel;
	uint32_t alpha;
	size_t count;
	size_t index;
	int error;

	/* Nothing yet, and a page. */
	memset(image, 0, sizeof(*image));
	if (view->page == NULL)
		return ENOENT;

	/* The image at the place in the document, and its source. */
	view_pointer_at(view, x, y, BROWSER_BUTTON_PRIMARY, 0U, &pointer);
	wb_buffer_init(&source);
	error = page_image_at(view->page, pointer.x, pointer.y, &bitmap, &source);
	if (error != 0 || bitmap == NULL || bitmap->pixels == NULL) {
		wb_buffer_release(&source);
		if (error != 0)
			return error;
		return ENOENT;
	}

	/* The pixels, copied and multiplied by their alpha. */
	count = (size_t)bitmap->width * (size_t)bitmap->height;
	image->pixels = malloc(count * sizeof(*image->pixels));
	if (image->pixels == NULL) {
		wb_buffer_release(&source);
		return ENOMEM;
	}

	/* Each pixel's colours times its alpha. */
	for (index = 0; index < count; index++) {
		pixel = bitmap->pixels[index];
		alpha = pixel >> 24;
		image->pixels[index] = (alpha << 24) |
				       ((((pixel >> 16) & 0xffU) * alpha + 127U) / 255U) << 16 |
				       ((((pixel >> 8) & 0xffU) * alpha + 127U) / 255U) << 8 |
				       (((pixel & 0xffU) * alpha + 127U) / 255U);
	}

	/* Its size. */
	image->width = bitmap->width;
	image->height = bitmap->height;

	/* The source's URL, copied (empty when the element has none). */
	url = wb_buffer_string(&source);
	if (url == NULL)
		url = "";
	image->url = strdup(url);
	wb_buffer_release(&source);
	if (image->url == NULL) {
		free(image->pixels);
		image->pixels = NULL;
		return ENOMEM;
	}

	/* Succeeded: the image. */
	return 0;
}

/*
 * Frees what browser_view_image_at gave.
 */
void
browser_view_image_release(
	struct browser_image *image)
{
	/* The pixels and the URL. */
	free(image->pixels);
	free(image->url);
	memset(image, 0, sizeof(*image));
}

/* Tells which Vulkan call (or step) failed last in a drawing, and what it returned. */
void
browser_view_gpu_failure(
	const struct browser_view *view,
	struct browser_gpu_failure *failure)
{
	/* The one kept. */
	*failure = view->failure;
}

/*
 * Makes an offscreen image of the engine's own, width by height, on a
 * device of its own; a failure is EIO with the Vulkan call in *failure.
 */
int
browser_offscreen_create(
	unsigned width,
	unsigned height,
	struct browser_offscreen **offscreen,
	struct browser_gpu_failure *failure)
{
	struct browser_offscreen *made;
	VkResult result;
	int error;

	/* Refuses missing result slots without dereferencing them. */
	if (offscreen == NULL || failure == NULL)
		return EINVAL;

	/* A refused creation leaves no owned image and no stale Vulkan error. */
	*offscreen = NULL;
	failure->operation = NULL;
	failure->result = VK_SUCCESS;

	/* Rejects empty images before the driver can receive invalid dimensions. */
	error = view_dimensions(width, height);
	if (error != 0)
		return error;

	/* Allocates the holder of the device and image. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* The device and the image. */
	result = paint_offscreen_open(&made->offscreen, (uint32_t)width, (uint32_t)height);
	if (result != VK_SUCCESS) {
		failure->operation = made->offscreen.operation;
		failure->result = result;
		paint_offscreen_close(&made->offscreen);
		free(made);
		return EIO;
	}

	/* Succeeded: a view can draw into it. */
	*offscreen = made;
	return 0;
}

/* Gives the GPU and the target a view draws into the offscreen image with (left ready to be read back). */
void
browser_offscreen_target(
	const struct browser_offscreen *offscreen,
	struct browser_gpu *gpu,
	struct browser_target *target)
{
	/* The device. */
	gpu->instance = offscreen->offscreen.instance;
	gpu->physical = offscreen->offscreen.physical;
	gpu->device = offscreen->offscreen.device;
	gpu->queue_family = offscreen->offscreen.family;

	/* The image, left as the copy that reads it back needs. */
	target->image = offscreen->offscreen.image;
	target->view = offscreen->offscreen.view;
	target->format = offscreen->offscreen.format;
	target->new_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	target->width = offscreen->offscreen.extent.width;
	target->height = offscreen->offscreen.extent.height;
}

/*
 * Reads the offscreen image, drawn by a view, into 0xAARRGGBB pixels whose rows are stride bytes apart.
 */
int
browser_offscreen_read(
	struct browser_offscreen *offscreen,
	uint32_t *pixels,
	size_t stride,
	struct browser_gpu_failure *failure)
{
	VkResult result;
	int error;

	/* Refuses missing objects and result storage before accessing their fields. */
	if (offscreen == NULL || failure == NULL)
		return EINVAL;

	/* Starts this read without a stale driver failure from an earlier operation. */
	failure->operation = NULL;
	failure->result = VK_SUCCESS;

	/* Refuses an output span that cannot hold the complete image. */
	error = view_pixels(pixels, offscreen->offscreen.extent.width, offscreen->offscreen.extent.height, stride);
	if (error != 0)
		return error;

	/* The copy out of the image. */
	failure->operation = NULL;
	failure->result = VK_SUCCESS;
	result = paint_offscreen_read(&offscreen->offscreen, pixels, stride);
	if (result != VK_SUCCESS) {
		failure->operation = offscreen->offscreen.operation;
		failure->result = result;
		return EIO;
	}

	/* Succeeded: the pixels hold the picture. */
	return 0;
}

/* Ends an offscreen image and its device (no view may draw with it any more). */
void
browser_offscreen_destroy(
	struct browser_offscreen *offscreen)
{
	/* No offscreen image. */
	if (offscreen == NULL)
		return;

	/* The image, the device, the instance, then the holder. */
	paint_offscreen_close(&offscreen->offscreen);
	free(offscreen);
}

/* Trusts the CA certificates of a PEM file for https, besides the system's roots, in every view. */
int
browser_add_ca_file(
	const char *path)
{
	int error;

	/* The TLS layer's list. */
	error = net_tls_add_ca_file(path);
	if (error != 0)
		return error;

	/* Succeeded: the file's authorities are trusted. */
	return 0;
}

/* The monotonic clock in milliseconds. */
static uint64_t
view_clock(void)
{
	struct timespec now;
	int status;

	/* The clock. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0U;

	/* In milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/*
 * Makes the page of a location the one shown, as a new step of the history
 * or one already in it: a file or data: page at once, an http or https
 * page as the view fetches (the first at once, or every one at once, or
 * every one without blocking).  Returns 0, or an errno value when the page
 * could not be opened (the page shown stays).
 */
static int
view_navigate(
    struct browser_view *view,
    const char *path,
    int step,
    size_t history_index)
{
	struct page *page;
	int background;
	int remote;
	int error;

	/* Whether an http or https page is fetched without blocking: never without a loader, the first one only when asked. */
	remote = page_net_is_remote(path);
	background = 0;
	if (remote && view->loader != NULL) {
		if (view->page != NULL || view->fetch == BROWSER_FETCH_BACKGROUND)
			background = 1;
	}

	/* Such a page is fetched; the page shown stays until it arrives. */
	if (background) {
		error = view_start_load(view, path, step, history_index);
		if (error != 0)
			return error;

		/* Succeeded: the destination is pending and the current page remains shown. */
		return 0;
	}

	/* Any other page is read at once, and shown. */
	view_stop_load(view, 0);
	error = view_open_page(view, path, &page);
	if (error != 0)
		return error;
	error = view_show_page(view, page, path, step, history_index);
	if (error != 0) {
		view_failed(view, path, error, "");
		return error;
	}

	/* Succeeded: the page is the one shown. */
	return 0;
}

/*
 * Makes an empty page: its heap scanning the stack up to the view's frame,
 * the view's size for its scripts, its console through the view, its
 * clock starting now, and the loader for its images.
 */
static int
view_make_page(
	struct browser_view *view,
	struct page **page)
{
	struct page *made;
	int error;

	/* The page. */
	*page = NULL;
	error = page_create(&made, view->stack_base);
	if (error != 0)
		return error;

	/* Its scripts see the view's size and its fonts, write their console through the view, and count time from now. */
	page_set_viewport(made, (int)view->width, (int)view->height);
	page_set_fonts(made, &view->fonts);
	page_set_console(made, view_console, view);
	view->open_epoch = view_clock();

	/* Its http and https images come through the loader (without one, they are read at once). */
	page_set_loader(made, view->loader);

	/* Succeeded: the page is empty. */
	*page = made;
	return 0;
}

/* Makes a page and reads a location into it; a failure goes to the load callback. */
static int
view_open_page(
	struct browser_view *view,
	const char *path,
	struct page **page)
{
	struct page *loaded;
	int error;

	/* The page. */
	*page = NULL;
	error = view_make_page(view, &loaded);
	if (error != 0) {
		view_failed(view, path, error, "");
		return error;
	}

	/* The location. */
	error = page_load_location(loaded, path);
	if (error != 0) {
		view_failed(view, path, error, page_failure_reason());
		page_destroy(loaded);
		return error;
	}

	/* Succeeded: the page is loaded. */
	*page = loaded;
	return 0;
}

/*
 * Makes a loaded page the one shown, from its top, as a new step of the
 * history or one already in it; the committed callback hears of it.
 */
static int
view_show_page(
    struct browser_view *view,
    struct page *page,
    const char *path,
    int step,
    size_t history_index)
{
	char *copy;
	char *history_copy;
	size_t index;

	/* The page's own location (a URL's after its redirects) is kept for the history and for resolving links. */
	if (page->base != NULL)
		path = page->base;
	copy = strdup(path);
	if (copy == NULL) {
		page_destroy(page);
		return ENOMEM;
	}

	/* Reserves the new history entry while every old page and forward step is still owned. */
	history_copy = NULL;
	if (step == VIEW_STEP_NEW) {
		history_copy = strdup(path);
		if (history_copy == NULL) {
			free(copy);
			page_destroy(page);
			return ENOMEM;
		}
	}

	/*
	 * The atlas's glyphs are the old page's (its text system goes with it,
	 * and the memory of their bitmaps may be used again by the new one's).
	 */
	if (view->gpu_open)
		paint_gpu_forget_glyphs(&view->gpu);

	/* The new page has the focus of the view's program as the view has it (it has no focused element to tell yet). */
	(void)page_window_focus(page, view->has_focus);

	/* The new page replaces the old one, from its top, with its own clock. */
	page_destroy(view->page);
	view->page = page;
	view->page_epoch = view->open_epoch;
	free(view->path);
	view->path = copy;
	view->scroll_y = 0;
	view->overscroll_y = 0;

	/* A new step drops the steps after the one shown, and the oldest when the history is full. */
	if (step == VIEW_STEP_NEW) {
		for (index = view->history_index + 1U; index < view->history_count; index++)
			free(view->history[index]);
		if (view->history_count != 0)
			view->history_count = view->history_index + 1U;
		if (view->history_count == VIEW_HISTORY_MAX) {
			free(view->history[0]);
			memmove(view->history, view->history + 1, (VIEW_HISTORY_MAX - 1U) * sizeof(view->history[0]));
			view->history_count--;
		}

		/* Records the new step. */
		view->history[view->history_count] = history_copy;
		view->history_index = view->history_count;
		view->history_count++;
	} else {
		/* Publishes the destination only after its page has replaced the old one. */
		view->history_index = history_index;
	}

	/* Its title (it is laid out at the view's size when it is drawn). */
	wb_buffer_clear(&view->title);
	(void)page_title(view->page, &view->title);

	/* The caller hears of it and draws it. */
	if (view->callbacks.committed != NULL)
		view->callbacks.committed(view->callbacks.context, view);
	view_redraw(view);

	/* Succeeded: the page is the one shown. */
	return 0;
}

/* Starts fetching an http or https page (any load under way is stopped); the page shown stays until it arrives. */
static int
view_start_load(
    struct browser_view *view,
    const char *path,
    int step,
    size_t history_index)
{
	char *copy;
	int error;

	/* One load at a time. */
	view_stop_load(view, 0);

	/* The location, kept for the arrival. */
	copy = strdup(path);
	if (copy == NULL)
		return ENOMEM;

	/* The request. */
	error = page_net_fetch(view->loader, path, view_document_arrived, view, &view->pending);
	if (error != 0) {
		view_failed(view, path, error, "");
		free(copy);
		return error;
	}

	/* Succeeded: the page is loading. */
	view->pending_path = copy;
	view->pending_step = step;
	view->pending_index = history_index;
	if (view->callbacks.load != NULL)
		view->callbacks.load(view->callbacks.context, view, BROWSER_LOAD_STARTED, path, 0, "");
	return 0;
}

/*
 * The loader's callback for the page being fetched: a response becomes the
 * page shown (at its final URL, after redirects), once the parser-blocking
 * scripts it names were fetched (BUG-207); a failure is reported and the
 * page shown stays.
 */
static void
view_document_arrived(
	void *context,
	struct net_request *request)
{
	struct browser_view *view;
	const unsigned char *bytes;
	const char *url;
	char *path;
	size_t length;
	int error;

	/* The document's request is over. */
	view = context;
	view->pending = NULL;

	/* A request that failed leaves the page shown. */
	error = page_net_result(request, &bytes, &length, &url);
	if (error != 0) {
		path = view->pending_path;
		view->pending_path = NULL;
		view_failed(view, path, error, page_failure_reason());
		free(path);
		return;
	}

	/* The scripts it names are fetched first; the document waits for them without blocking. */
	error = view_prefetch_start(view, bytes, length, url);
	if (error == 0 && view->prefetch_waiting != 0)
		return;

	/* No script to wait for (or no memory to wait with: the parser reads them at once): the page now. */
	view_prefetch_clear(view);
	view_commit_document(view, bytes, length, url, NULL);
}

/*
 * Makes the document that arrived the page shown, in a new page at its
 * final URL, with the scripts fetched ahead for its parser (NULL for
 * none); a page that could not be made is reported and the page shown
 * stays.  The load's location goes.
 */
static void
view_commit_document(
	struct browser_view *view,
	const unsigned char *bytes,
	size_t length,
	const char *url,
	struct wb_vector *scripts)
{
	const struct view_prefetch_script *script;
	struct page *page;
	char *path;
	size_t history_index;
	size_t index;
	int step;
	int error;

	/* The load's location and how it joins the history, taken from the view. */
	path = view->pending_path;
	step = view->pending_step;
	history_index = view->pending_index;
	view->pending_path = NULL;

	/* A new page. */
	error = view_make_page(view, &page);

	/* The scripts fetched ahead, for its parser. */
	if (error == 0 && scripts != NULL) {
		for (index = 0; index < scripts->count; index++) {
			script = *(const struct view_prefetch_script **)wb_vector_at(scripts, index);
			error = page_add_prefetched(page, script->location, script->error, script->bytes.data, script->bytes.length);
			if (error != 0)
				break;
		}
		if (error != 0)
			page_destroy(page);
	}

	/* The document, at its final URL. */
	if (error == 0) {
		error = page_load_bytes(page, bytes, length, url);
		if (error != 0)
			page_destroy(page);
	}

	/* The new page is shown; a page that could not be made is reported. */
	if (error == 0)
		error = view_show_page(view, page, url, step, history_index);

	/* Allocation failures during commit also reach the caller's failure callback. */
	if (error != 0)
		view_failed(view, url, error, "");
	free(path);
}

/*
 * Starts fetching the parser-blocking web scripts a document names
 * (BUG-207), keeping the document until they end.  prefetch_waiting says
 * how many were started (none: the document need not wait).  Returns 0,
 * or ENOMEM.
 */
static int
view_prefetch_start(
	struct browser_view *view,
	const unsigned char *bytes,
	size_t length,
	const char *url)
{
	struct view_prefetch_script *script;
	struct wb_vector locations;
	char *location;
	size_t index;
	int error;

	/* Nothing waits yet. */
	view_prefetch_clear(view);

	/* The scripts the document names (a failure to scan leaves the parser to read them). */
	wb_vector_init(&locations, sizeof(char *));
	error = page_scan_scripts(url, bytes, length, &locations);

	/* None: the document need not wait. */
	if (error != 0 || locations.count == 0) {
		for (index = 0; index < locations.count; index++) {
			location = *(char **)wb_vector_at(&locations, index);
			free(location);
		}
		wb_vector_release(&locations);
		return error;
	}

	/* The document and its final URL, kept while it waits. */
	error = wb_buffer_append(&view->arrived, bytes, length);
	if (error == 0) {
		view->arrived_url = strdup(url);
		if (view->arrived_url == NULL)
			error = ENOMEM;
	}

	/* A fetch for each script; one that cannot start is left to the parser. */
	for (index = 0; index < locations.count; index++) {
		location = *(char **)wb_vector_at(&locations, index);

		/* Without memory for the document the scripts only go. */
		if (error != 0) {
			free(location);
			continue;
		}

		/* The script's entry, which owns its location from here. */
		script = calloc(1, sizeof(*script));
		if (script == NULL) {
			free(location);
			error = ENOMEM;
			continue;
		}
		script->view = view;
		script->location = location;
		wb_buffer_init(&script->bytes);

		/* The view's list keeps it. */
		error = wb_vector_push(&view->prefetches, &script);
		if (error != 0) {
			free(location);
			free(script);
			continue;
		}

		/* Its request; one that does not start leaves the parser to read it (the entry is dropped). */
		error = page_net_fetch(view->loader, location, view_prefetch_arrived, script, &script->request);
		if (error != 0) {
			script->request = NULL;
			view->prefetches.count--;
			free(location);
			free(script);
			error = 0;
			continue;
		}

		/*
		 * One more script the document waits for; the count reaching zero
		 * in view_prefetch_arrived shows the page.
		 */
		view->prefetch_waiting++;
	}
	wb_vector_release(&locations);

	/* Without memory the waiting ends: the scripts started are cancelled and the parser reads them. */
	if (error != 0) {
		view_prefetch_clear(view);
		return error;
	}

	/* Succeeded: the document waits for its scripts (or for none). */
	return 0;
}

/* The loader's callback for a script fetched ahead: its result is kept, and the last one shows the page. */
static void
view_prefetch_arrived(
	void *context,
	struct net_request *request)
{
	struct view_prefetch_script *script;
	const unsigned char *bytes;
	const char *url;
	size_t length;
	int error;

	/* The script's request is over: its error, or its body (of any status, as reading it at once gives). */
	script = context;
	script->request = NULL;
	error = page_net_result(request, &bytes, &length, &url);
	if (error == 0)
		error = wb_buffer_append(&script->bytes, bytes, length);
	script->error = error;

	/* One fewer to wait for; the last one makes the page. */
	script->view->prefetch_waiting--;
	if (script->view->prefetch_waiting == 0)
		view_prefetch_commit(script->view);
}

/* Makes the document that waited for its scripts the page shown, then lets the scripts and the document go. */
static void
view_prefetch_commit(
	struct browser_view *view)
{
	struct wb_buffer document;
	struct wb_vector scripts;
	char *url;

	/*
	 * The document, its URL and its scripts are taken from the view
	 * first: the new page may start another load, which clears the view's.
	 */
	document = view->arrived;
	wb_buffer_init(&view->arrived);
	url = view->arrived_url;
	view->arrived_url = NULL;
	scripts = view->prefetches;
	wb_vector_init(&view->prefetches, sizeof(struct view_prefetch_script *));

	/* The page. */
	view_commit_document(view, document.data, document.length, url, &scripts);

	/* What it was made from goes (not the view's, which may hold a new load's). */
	view_prefetch_free(&scripts);
	wb_buffer_release(&document);
	free(url);
}

/* Lets a waiting document and its scripts go, cancelling the scripts' requests still running. */
static void
view_prefetch_clear(
	struct browser_view *view)
{
	/* The scripts, with their requests cancelled; the list stays for the next load. */
	view_prefetch_free(&view->prefetches);
	wb_vector_init(&view->prefetches, sizeof(struct view_prefetch_script *));

	/* No script and no document wait. */
	view->prefetch_waiting = 0;
	wb_buffer_release(&view->arrived);
	wb_buffer_init(&view->arrived);
	free(view->arrived_url);
	view->arrived_url = NULL;
}

/* Frees a list of scripts fetched ahead and the list's storage, cancelling their requests still running (their callbacks are not called). */
static void
view_prefetch_free(
	struct wb_vector *scripts)
{
	struct view_prefetch_script *script;
	size_t index;

	/* Each script: its request, its location and bytes. */
	for (index = 0; index < scripts->count; index++) {
		script = *(struct view_prefetch_script **)wb_vector_at(scripts, index);
		if (script->request != NULL)
			page_net_cancel(script->request);
		free(script->location);
		wb_buffer_release(&script->bytes);
		free(script);
	}

	/* The list's storage. */
	wb_vector_release(scripts);
}

/* Stops the page being fetched, if any; report tells the load callback. */
static void
view_stop_load(
	struct browser_view *view,
	int report)
{
	/* Nothing loads: no request runs and no document waits for its scripts. */
	if (view->pending == NULL && view->arrived_url == NULL)
		return;

	/* The request, or the scripts a document that arrived waits for. */
	if (view->pending != NULL)
		page_net_cancel(view->pending);
	view->pending = NULL;
	view_prefetch_clear(view);
	if (report && view->callbacks.load != NULL)
		view->callbacks.load(view->callbacks.context, view, BROWSER_LOAD_STOPPED, view->pending_path, 0, "");

	/* The location kept for it goes too. */
	free(view->pending_path);
	view->pending_path = NULL;
}

/*
 * Lays the page out at the view's size and paints it when it changed or
 * the view was resized since it was last laid out (opening the fonts the
 * first time), and keeps the scroll inside the new document.
 */
static int
view_update(
	struct browser_view *view)
{
	int changed;
	int repaint;
	int error;

	/* No page has nothing to lay out. */
	if (view->page == NULL)
		return 0;

	/* A page laid out at the view's size and unchanged since needs at most its focus painted again. */
	changed = page_needs_layout(view->page);
	if (!changed && !view->resized) {
		repaint = page_needs_paint(view->page);
		if (!repaint)
			return 0;
		error = page_paint(view->page);
		if (error != 0)
			return error;
		return 0;
	}

	/* The fonts, opened once. */
	error = page_open_fonts(view->page, &view->fonts);
	if (error != 0)
		return error;

	/* The layout at the view's size. */
	error = page_layout(view->page, (int)view->width, (int)view->height);
	if (error != 0)
		return error;

	/* The display list. */
	error = page_paint(view->page);
	if (error != 0)
		return error;

	/* The page fits the view now, and the scroll stays inside the new document. */
	view->resized = 0;
	view_clamp_scroll(view);

	/* Succeeded: the page is ready to draw. */
	return 0;
}

/*
 * Runs the loader until it has no request left (the headless modes): polls
 * its descriptors until one is ready or its earliest time out, then lets
 * it work.
 */
static void
view_settle_network(
	struct browser_view *view)
{
	struct pollfd fds[VIEW_POLL_MAX];
	size_t count;
	int timeout;
	int ready;

	/* A view without a loader read everything at once. */
	if (view->loader == NULL)
		return;

	/* Each round, while a request runs (the loader has no time out when it has none). */
	for (;;) {
		timeout = page_net_timeout(view->loader);
		if (timeout < 0)
			break;

		/* The descriptors, waited for. */
		count = page_net_poll_fds(view->loader, fds, VIEW_POLL_MAX);
		ready = poll(fds, (nfds_t)count, timeout);
		if (ready < 0 && errno != EINTR)
			break;

		/* The loader's work, and the callbacks of the requests that ended. */
		page_net_process(view->loader, fds, count);
	}
}

/* Keeps the scroll between the top and the last view's worth of the document as it is laid out. */
static void
view_clamp_scroll(
	struct browser_view *view)
{
	layout_unit limit;

	/* The furthest the page scrolls: the document's height less the view's. */
	limit = view->page->layout.document_height - (layout_unit)view->height * LAYOUT_UNIT;
	if (limit < 0)
		limit = 0;

	/* The place, within the limits. */
	if (view->scroll_y > limit)
		view->scroll_y = limit;
	if (view->scroll_y < 0)
		view->scroll_y = 0;

	/* The page's scripts see where the view is (ws074-p031). */
	page_set_scroll(view->page, 0.0, layout_to_px(view->scroll_y));
}

/* Takes the page's title again, and tells the caller when it changed. */
static void
view_update_title(
	struct browser_view *view)
{
	struct wb_buffer title;
	int differs;
	int error;

	/* The title now. */
	wb_buffer_init(&title);
	error = page_title(view->page, &title);
	if (error != 0 || title.length == 0) {
		wb_buffer_release(&title);
		return;
	}

	/* The same title is no change. */
	differs = 1;
	if (title.length == view->title.length)
		differs = memcmp(title.data, view->title.data, title.length);
	if (differs == 0) {
		wb_buffer_release(&title);
		return;
	}

	/* The new title is kept and told. */
	wb_buffer_release(&view->title);
	view->title = title;
	if (view->callbacks.title != NULL)
		view->callbacks.title(view->callbacks.context, view, wb_buffer_string(&view->title));
}

/* Moves the scroll by a distance, kept between the top and the last view's worth of the document. */
static void
view_scroll(
	struct browser_view *view,
	layout_unit distance)
{
	layout_unit before;
	int error;

	/* No page, no scroll. */
	if (view->page == NULL)
		return;

	/* The document as it is laid out now, which the scroll stays inside. */
	error = view_update(view);
	if (error != 0)
		return;

	/* The new place, within the document. */
	before = view->scroll_y;
	view->scroll_y += distance;
	view_clamp_scroll(view);

	/* A change is drawn again. */
	if (view->scroll_y != before)
		view_redraw(view);
}

/* Asks the caller to draw the view again. */
static void
view_redraw(
	struct browser_view *view)
{
	/* The callback, when there is one. */
	if (view->callbacks.redraw != NULL)
		view->callbacks.redraw(view->callbacks.context, view);
}

/* Tells the caller a load failed. */
static void
view_failed(
	struct browser_view *view,
	const char *url,
	int error,
	const char *reason)
{
	/* The callback, when there is one. */
	if (view->callbacks.load != NULL)
		view->callbacks.load(view->callbacks.context, view, BROWSER_LOAD_FAILED, url, error, reason);
}

/* Passes a page's console line to the caller. */
static void
view_console(
	void *context,
	int level,
	const char *text,
	size_t length)
{
	struct browser_view *view;

	/* The callback, when there is one. */
	view = context;
	if (view->callbacks.console != NULL)
		view->callbacks.console(view->callbacks.context, view, level, text, length);
}

/*
 * Makes the renderer ready for a target on the caller's device: made at
 * the first drawing, and made again when the target's format or final
 * layout differs from the one its pass was made for.
 */
static int
view_gpu_ready(
	struct browser_view *view,
	const struct browser_target *target)
{
	VkResult result;
	int error;

	/* Without the caller's GPU there is nothing to draw with. */
	if (!view->has_device)
		return ENODEV;

	/* A renderer made for another kind of target goes. */
	if (view->gpu_open) {
		if (view->gpu_format != target->format || view->gpu_layout != target->new_layout)
			view_gpu_close(view);
	}

	/* A renderer for this kind of target is there. */
	if (view->gpu_open)
		return 0;

	/* The renderer on the caller's device, its pass for the target's format and final layout. */
	result = paint_gpu_open(
		&view->gpu,
		view->device.instance,
		view->device.physical,
		view->device.queue_family,
		view->device.device,
		target->format,
		target->new_layout);
	if (result != VK_SUCCESS) {
		error = view_gpu_failed(view, view->gpu.operation, result);
		paint_gpu_close(&view->gpu);
		return error;
	}

	/* Succeeded: the renderer draws this kind of target. */
	view->gpu_open = 1;
	view->gpu_format = target->format;
	view->gpu_layout = target->new_layout;
	return 0;
}

/* Releases the renderer and the framebuffers on the caller's device, once the device has finished with them. */
static void
view_gpu_close(
	struct browser_view *view)
{
	/* Nothing was made. */
	if (!view->gpu_open)
		return;

	/* The device's work that may still use them. */
	(void)vkDeviceWaitIdle(view->device.device);

	/* The framebuffers (made in the renderer's pass), then the renderer. */
	browser_view_release_targets(view);
	paint_gpu_close(&view->gpu);
	view->gpu_open = 0;
}

/*
 * Finds the framebuffer over a target's image view at its size, or makes
 * one in the renderer's pass (the oldest goes when the view keeps as many
 * as it can; the drawing into it has finished, as the caller promises).
 */
static int
view_framebuffer(
	struct browser_view *view,
	const struct browser_target *target,
	VkFramebuffer *framebuffer)
{
	struct view_framebuffer *kept;
	VkFramebufferCreateInfo create;
	VkResult result;
	size_t index;
	int error;

	/* One made for this image view at this size. */
	for (index = 0; index < view->framebuffer_count; index++) {
		kept = &view->framebuffers[index];
		if (kept->image_view != target->view)
			continue;
		if (kept->width != target->width || kept->height != target->height)
			continue;

		/* Found: the one to draw through. */
		*framebuffer = kept->framebuffer;
		return 0;
	}

	/* A full table lets its oldest go. */
	if (view->framebuffer_count == VIEW_FRAMEBUFFERS_MAX) {
		vkDestroyFramebuffer(view->device.device, view->framebuffers[0].framebuffer, NULL);
		memmove(view->framebuffers, view->framebuffers + 1, (VIEW_FRAMEBUFFERS_MAX - 1U) * sizeof(view->framebuffers[0]));
		view->framebuffer_count--;
	}

	/* The framebuffer over the image view, in the renderer's pass. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	create.renderPass = view->gpu.pass;
	create.attachmentCount = 1U;
	create.pAttachments = &target->view;
	create.width = target->width;
	create.height = target->height;
	create.layers = 1U;
	kept = &view->framebuffers[view->framebuffer_count];
	result = vkCreateFramebuffer(view->device.device, &create, NULL, &kept->framebuffer);
	if (result != VK_SUCCESS) {
		error = view_gpu_failed(view, "vkCreateFramebuffer", result);
		return error;
	}

	/* Succeeded: kept for the next frames into the same image. */
	kept->image_view = target->view;
	kept->width = target->width;
	kept->height = target->height;
	view->framebuffer_count++;
	*framebuffer = kept->framebuffer;
	return 0;
}

/* Keeps which Vulkan call failed and what it returned, for browser_view_gpu_failure; reports EIO. */
static int
view_gpu_failed(
	struct browser_view *view,
	const char *operation,
	VkResult result)
{
	/* The failure, for the caller's message. */
	view->failure.operation = operation;
	view->failure.result = result;

	/* The error the drawing reports. */
	return EIO;
}

/* Describes a place of the view to the page: in the document (scrolled) and in the viewport, whole pixels. */
static void
view_pointer_at(
	const struct browser_view *view,
	float x,
	float y,
	int button,
	uint32_t modifiers,
	struct page_pointer *pointer)
{
	/* The viewport's place, and the document's below the scroll. */
	memset(pointer, 0, sizeof(*pointer));
	pointer->client_x = (int)floorf(x);
	pointer->client_y = (int)floorf(y);
	pointer->x = pointer->client_x;
	pointer->y = pointer->client_y + (int)(view->scroll_y / LAYOUT_UNIT);

	/* The button and the modifiers. */
	pointer->button = button;
	pointer->modifiers = view_bind_modifiers(modifiers);
}

/* Turns the view's modifier bits into the binding's. */
static unsigned
view_bind_modifiers(
	uint32_t modifiers)
{
	unsigned bits;

	/* Each modifier held. */
	bits = 0;
	if ((modifiers & BROWSER_MOD_SHIFT) != 0U)
		bits |= BIND_MOD_SHIFT;
	if ((modifiers & BROWSER_MOD_CTRL) != 0U)
		bits |= BIND_MOD_CTRL;
	if ((modifiers & BROWSER_MOD_ALT) != 0U)
		bits |= BIND_MOD_ALT;
	if ((modifiers & BROWSER_MOD_META) != 0U)
		bits |= BIND_MOD_META;

	/* The binding's bits. */
	return bits;
}

/*
 * Asks the caller to draw the view again when the page's scripts changed
 * its document, its focus or its scroll, and tells it of a new title.
 */
static void
view_changed(
	struct browser_view *view)
{
	int layout;
	int paint;

	/* No page, no change. */
	if (view->page == NULL)
		return;

	/* A scroll the scripts asked for (ws074-p082) is the view's, drawn again. */
	if (view->page->scroll_requested) {
		view->page->scroll_requested = 0;
		view->scroll_y = layout_from_px((float)view->page->scroll_y);
		view_clamp_scroll(view);
		view_redraw(view);
	}

	/* A document the scripts changed: drawn again, and its title may be new. */
	layout = page_needs_layout(view->page);
	if (layout) {
		view_redraw(view);
		view_update_title(view);
		return;
	}

	/* A focus that moved: drawn again with its ring. */
	paint = page_needs_paint(view->page);
	if (paint)
		view_redraw(view);
}

/*
 * The click of the main button at a place in the view: the page's scripts
 * get it first; unless they cancel it, the link under it is followed.
 */
static int
view_click(
	struct browser_view *view,
	float x,
	float y,
	uint32_t modifiers)
{
	struct page_pointer pointer;
	struct wb_buffer href;
	int canceled;
	int found;
	int error;

	/* No page, nothing to click. */
	if (view->page == NULL)
		return 0;

	/* The boxes the click lands on are the page's as it is now. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* The page's scripts get the click first; a canceled click opens no link. */
	view_pointer_at(view, x, y, BROWSER_BUTTON_PRIMARY, modifiers, &pointer);
	error = page_mouse_event(view->page, "click", &pointer, &canceled);
	if (error != 0)
		return error;
	view_changed(view);
	if (canceled)
		return 0;

	/* A page the listeners changed is laid out again before its link is looked for. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* A form control under the click does what it does (a submit button's form goes where it submits). */
	wb_buffer_init(&href);
	error = page_click_control(view->page, pointer.x, pointer.y, &href, &found);
	if (error == 0 && found) {
		view_changed(view);
		view_follow_link(view, wb_buffer_string(&href));
		wb_buffer_release(&href);
		return 0;
	}

	/* A control that changed (a checkbox) is drawn again. */
	view_changed(view);

	/* The link under the click, in the document's coordinates. */
	if (error == 0)
		error = page_link_at(view->page, pointer.x, pointer.y, &href, &found);
	if (error != 0) {
		wb_buffer_release(&href);
		return error;
	}

	/* The link, when there is one, is followed. */
	if (found)
		view_follow_link(view, wb_buffer_string(&href));
	wb_buffer_release(&href);

	/* Succeeded: the click was carried out. */
	return 0;
}

/* Follows a link a click or Enter opened, when the link callback allows it (a load that fails goes to the load callback). */
static void
view_follow_link(
	struct browser_view *view,
	const char *href)
{
	enum browser_policy policy;

	/* The caller decides. */
	policy = BROWSER_POLICY_ALLOW;
	if (view->callbacks.link != NULL)
		policy = view->callbacks.link(view->callbacks.context, view, href);

	/* An allowed link is followed. */
	if (policy == BROWSER_POLICY_ALLOW)
		(void)browser_view_follow(view, href);
}

/*
 * Does the default action of a key the page's scripts did not cancel:
 * the history (Alt+Left, Alt+Right, the back and forward keys), reloading
 * (F5, Ctrl+R) and stopping (Escape), the focus (Tab, Shift+Tab) and a
 * link's activation (Enter), and scrolling (the arrows, Page Up and Down,
 * Space, Home, End).
 */
static int
view_key_default(
	struct browser_view *view,
	const char *key,
	uint32_t modifiers)
{
	int named;
	int alt;
	int ctrl;
	int meta;
	int shift;
	int pressable;
	int error;

	/* The key among the ones with an action, and the modifiers held. */
	named = view_key_named(key);
	alt = 0;
	if ((modifiers & BROWSER_MOD_ALT) != 0U)
		alt = 1;
	ctrl = 0;
	if ((modifiers & BROWSER_MOD_CTRL) != 0U)
		ctrl = 1;
	meta = 0;
	if ((modifiers & BROWSER_MOD_META) != 0U)
		meta = 1;
	shift = 0;
	if ((modifiers & BROWSER_MOD_SHIFT) != 0U)
		shift = 1;

	/* The history, reloading and stopping, with or without a page (a failure goes to the load callback). */
	switch (named) {
	case VIEW_KEY_BACK:
		(void)browser_view_go(view, -1);
		return 0;
	case VIEW_KEY_FORWARD:
		(void)browser_view_go(view, 1);
		return 0;
	case VIEW_KEY_LEFT:
		if (alt)
			(void)browser_view_go(view, -1);
		return 0;
	case VIEW_KEY_RIGHT:
		if (alt)
			(void)browser_view_go(view, 1);
		return 0;
	case VIEW_KEY_REFRESH:
		(void)browser_view_go(view, 0);
		return 0;
	case VIEW_KEY_R:
		if (ctrl)
			(void)browser_view_go(view, 0);
		return 0;
	case VIEW_KEY_STOP:
		view_stop_load(view, 1);
		return 0;
	default:
		break;
	}

	/* The rest need a page, and neither Alt nor Meta. */
	if (view->page == NULL)
		return 0;
	if (alt || meta)
		return 0;

	/* The focus, the activation and scrolling. */
	error = 0;
	switch (named) {
	case VIEW_KEY_TAB:
		/* Ctrl+Tab belongs to the program around the view (its tabs). */
		if (!ctrl)
			error = view_move_focus(view, shift);
		break;
	case VIEW_KEY_ENTER:
		error = view_activate(view);
		break;
	case VIEW_KEY_DOWN:
		view_scroll(view, (layout_unit)VIEW_LINE_SCROLL * LAYOUT_UNIT);
		break;
	case VIEW_KEY_UP:
		view_scroll(view, -(layout_unit)VIEW_LINE_SCROLL * LAYOUT_UNIT);
		break;
	case VIEW_KEY_PAGE_DOWN:
		view_scroll_pages(view, 1);
		break;
	case VIEW_KEY_PAGE_UP:
		view_scroll_pages(view, -1);
		break;
	case VIEW_KEY_SPACE:
		/* Space presses a focused button or checkbox; otherwise it goes down a page, and up with Shift. */
		pressable = page_focus_pressable(view->page);
		if (pressable)
			error = view_activate(view);
		else if (shift)
			view_scroll_pages(view, -1);
		else
			view_scroll_pages(view, 1);
		break;
	case VIEW_KEY_HOME:
		view_scroll_to(view, VIEW_SCROLL_TOP);
		break;
	case VIEW_KEY_END:
		view_scroll_to(view, VIEW_SCROLL_BOTTOM);
		break;
	default:
		break;
	}

	/* Reports why the focus or the activation failed. */
	if (error != 0)
		return error;

	/* Succeeded: the key's default action is done (most keys have none). */
	return 0;
}

/* Finds which of the keys with a default action a DOM key name is (VIEW_KEY_OTHER for the rest). */
static int
view_key_named(
	const char *key)
{
	size_t index;
	int differs;

	/* The table's names. */
	for (index = 0; view_key_names[index].name != NULL; index++) {
		differs = strcmp(key, view_key_names[index].name);
		if (differs == 0)
			return view_key_names[index].key;
	}

	/* A key without a default action. */
	return VIEW_KEY_OTHER;
}

/* Moves the focus forward or backward (Tab), and scrolls the focused element into view. */
static int
view_move_focus(
	struct browser_view *view,
	int backward)
{
	int error;

	/* The order of the elements is the page's as it is laid out now. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* The move. */
	error = page_focus_move(view->page, backward);
	if (error != 0)
		return error;
	view_changed(view);

	/* The focused element in view. */
	error = view_scroll_into_view(view);
	if (error != 0)
		return error;

	/* Succeeded: the focus moved. */
	return 0;
}

/* Activates the focused element (Enter): its click, and the link it is in is followed unless the click was canceled. */
static int
view_activate(
	struct browser_view *view)
{
	struct wb_buffer href;
	int found;
	int error;

	/* The click at the focused element, and its link. */
	wb_buffer_init(&href);
	error = page_activate_focused(view->page, &href, &found);
	if (error != 0) {
		wb_buffer_release(&href);
		return error;
	}

	/* The page as the click's listeners left it is drawn again. */
	view_changed(view);

	/* The link, when there is one, is followed. */
	if (found)
		view_follow_link(view, wb_buffer_string(&href));
	wb_buffer_release(&href);

	/* Succeeded: the element is activated. */
	return 0;
}

/*
 * Scrolls the least that brings the focused element into view, with a
 * little room around it (its top when it is taller than the view).
 */
static int
view_scroll_into_view(
	struct browser_view *view)
{
	struct layout_rect rect;
	layout_unit margin;
	layout_unit height;
	layout_unit target;
	int found;
	int error;

	/* The page laid out and painted with the focus as it is. */
	error = view_update(view);
	if (error != 0)
		return error;

	/* The focused element's rectangle; nothing focused scrolls nothing. */
	found = page_focus_rect(view->page, &rect);
	if (!found)
		return 0;

	/* The view's height and the room kept around the element. */
	margin = (layout_unit)VIEW_FOCUS_MARGIN * LAYOUT_UNIT;
	height = (layout_unit)view->height * LAYOUT_UNIT;
	target = view->scroll_y;

	/* Below the view: its bottom at the view's bottom; above it (or taller than it): its top at the view's top. */
	if (rect.y + rect.height + margin > target + height)
		target = rect.y + rect.height + margin - height;
	if (rect.y - margin < target)
		target = rect.y - margin;

	/* The scroll, kept inside the document. */
	view_scroll(view, target - view->scroll_y);

	/* Succeeded: the element is in view. */
	return 0;
}

/* Scrolls by pages: the view's height less a little kept in view, each. */
static void
view_scroll_pages(
	struct browser_view *view,
	int pages)
{
	layout_unit step;

	/* A page's step. */
	step = ((layout_unit)view->height - VIEW_PAGE_OVERLAP) * LAYOUT_UNIT;
	view_scroll(view, step * pages);
}

/* Scrolls to the document's top or bottom. */
static void
view_scroll_to(
	struct browser_view *view,
	int place)
{
	int error;

	/* No page, no scroll. */
	if (view->page == NULL)
		return;

	/* The document's height is the one laid out now. */
	error = view_update(view);
	if (error != 0)
		return;

	/* The top, or the bottom (the scroll stops at the last view's worth). */
	if (place == VIEW_SCROLL_TOP)
		view_scroll(view, -view->scroll_y);
	else
		view_scroll(view, view->page->layout.document_height);
}

/* Reports the scroll the page is drawn at: the scroll less the caller's shift past an end (a shift down shows the document from higher up). */
static layout_unit
view_drawn_scroll(
	const struct browser_view *view)
{
	/* The scroll with the shift. */
	return view->scroll_y - view->overscroll_y;
}

/* Rejects sizes the signed layout coordinates cannot represent. */
static int
view_dimensions(
    unsigned width,
    unsigned height)
{
	/* Both axes must contain pixels and fit the layout's signed dimensions. */
	if (width == 0 || width > INT_MAX)
		return EINVAL;

	/* The height has the same representation as the width. */
	if (height == 0 || height > INT_MAX)
		return EINVAL;

	/* Succeeded: both dimensions are usable by the layout engine. */
	return 0;
}

/* Validates the complete byte span before a renderer writes the caller's rows. */
static int
view_pixels(
    const uint32_t *pixels,
    unsigned width,
    unsigned height,
    size_t stride)
{
	size_t row_bytes;
	int error;

	/* Refuses an absent output before any renderer can write through it. */
	if (pixels == NULL)
		return EINVAL;

	/* Rejects empty or unrepresentable dimensions before calculating byte counts. */
	error = view_dimensions(width, height);
	if (error != 0)
		return error;

	/* Detects packed-row multiplication wrap on either client word size. */
	row_bytes = (size_t)width * sizeof(uint32_t);
	if (row_bytes / sizeof(uint32_t) != (size_t)width)
		return EINVAL;

	/* Every row needs its complete pixels, even when there is padding after it. */
	if (stride < row_bytes)
		return EINVAL;

	/* The last row's start and complete contents must not wrap the byte span. */
	if (height > 1U) {
		if (stride > (SIZE_MAX - row_bytes) / (size_t)(height - 1U))
			return EINVAL;
	}

	/* Succeeded: each output row has a distinct, representable byte span. */
	return 0;
}

/* Refuses targets that cannot name a nonempty Vulkan color attachment. */
static int
view_target(
    const struct browser_target *target)
{
	int error;

	/* The caller must supply a target before its fields can be inspected. */
	if (target == NULL)
		return EINVAL;

	/* Both handles belong to the caller and must survive the submitted drawing. */
	if (target->image == VK_NULL_HANDLE || target->view == VK_NULL_HANDLE)
		return EINVAL;

	/* The target must have a usable color format. */
	if (target->format == VK_FORMAT_UNDEFINED)
		return EINVAL;

	/* Validates dimensions before framebuffer creation or viewport recording. */
	error = view_dimensions(target->width, target->height);
	if (error != 0)
		return error;

	/* Succeeded: the target describes a complete attachment. */
	return 0;
}
