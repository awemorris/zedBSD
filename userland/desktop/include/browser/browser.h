/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Web browser engine as a component: libbrowser (ws074-p057,
 * plan/ws074/design.md §19).
 *
 * The engine holds HTML, CSS, the layout, JavaScript, the DOM, the
 * network, the images and the drawing.  A program uses it through a view:
 * one browsing context (a tab's content), an opaque handle.  The program
 * owns the window (if it has one), the Vulkan device, the main loop and
 * the user interface around the content; the engine knows nothing of
 * Wayland or of windows.  /bin/browser is one such program (its window and
 * titlebar); a System Settings window or a widget can be another.
 *
 * A view owns the page shown, the page being fetched, the session history,
 * the scroll, the page's clock and the loader of the network.  The program
 * makes it, gives it a size, loads a location, polls the descriptors it
 * lists, calls browser_view_process, and hears of what happened through
 * callbacks.
 *
 * The page is laid out when something needs its boxes (a drawing, a dump
 * of the layout, the scroll, a click), not when it changes, so a view that
 * is only asked for its DOM never opens its fonts.  A program without a
 * window settles a view (browser_view_settle), then draws it with the CPU
 * (browser_view_draw_pixels) or dumps it (browser_view_dump).
 *
 * With the GPU, the program lends the view its Vulkan device
 * (browser_view_set_gpu) and names an image of its own to draw into (a
 * browser_target: a swapchain image of a window, an offscreen image).  The
 * view either submits the drawing itself and waits for it
 * (browser_view_draw), or records it into the program's command buffer
 * (browser_view_record), which the program submits.  The view makes and
 * keeps a framebuffer for each image view it is given; the program tells
 * it to forget them (browser_view_release_targets) before it destroys the
 * images.  A program without a window can make an offscreen image of the
 * engine's (browser_offscreen) and read the drawing back.
 *
 * Input: the program sends the pointer, the wheel, the keys and its own
 * focus in the view's pixels and the DOM's names of the keys
 * (browser_view_pointer_*, _wheel, _key, _focus); it turns its device's
 * codes into those names and keeps only its own shortcuts.  The page's
 * scripts get the events first; unless they cancel them, the view does
 * the default actions: the wheel and the keys scroll (the arrows, Page Up
 * and Down, Space, Home, End), a click or Enter on a link follows it, Tab
 * moves the focus with its ring and scrolls it into view, Alt+Left and
 * Alt+Right (and the back and forward keys and buttons) step through the
 * history, F5 and Ctrl+R reload, and Escape stops a load.
 *
 * A view is used from the thread that made it.  Callbacks run synchronously
 * inside the call that caused them.  A callback may inspect browser_view_url,
 * browser_view_title, browser_view_can_go, browser_view_scroll_y,
 * browser_view_gpu_failure, browser_view_poll_fds and browser_view_timeout.
 * It may operate on another view.  Changing, drawing, laying out, processing
 * or destroying the same view must wait until the outer call has returned;
 * browser_view_document_height and browser_view_scroll_range may lay it out.
 * Callback strings and query strings are borrowed: copy them before keeping
 * them beyond the callback or a later state-changing call.  Fonts' paths and
 * stack_base outlive the view; callback/options structures are copied.
 *
 * Dimensions must be nonzero and fit INT_MAX.  Creation refuses missing
 * output/options or an unknown fetch policy with EINVAL and leaves a valid
 * output slot NULL.  Pixel drawing/readback require non-NULL storage, a stride
 * large enough for a full row and a representable complete row span (EINVAL
 * otherwise); the caller owns enough bytes for that span.  GPU targets require
 * non-NULL image/image-view handles, a defined format and valid dimensions.
 * Complete recorded GPU work before changing pages, releasing targets or
 * reusing a view's renderer.  Destroy/set_gpu wait for the old device's work.
 *
 * Every call that can fail
 * reports 0 or an errno value.
 */

#ifndef _BROWSER_H_
#define _BROWSER_H_

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The version of this interface.  A program puts it in the options of
 * each view it makes; a library of another version refuses the view
 * (ENOTSUP) rather than read options laid out differently.
 */
#define BROWSER_API_VERSION		2

/* A view, opaque to the program. */
struct browser_view;
struct pollfd;

/* An offscreen image of the engine's own, opaque to the program. */
struct browser_offscreen;

/* How a load went, for the load callback. */
enum browser_load_state {
	BROWSER_LOAD_STARTED,
	BROWSER_LOAD_STOPPED,
	BROWSER_LOAD_FAILED
};

/* What a link callback decides. */
enum browser_policy {
	BROWSER_POLICY_ALLOW,
	BROWSER_POLICY_DENY
};

/*
 * How a view fetches its http and https pages and images: the first page
 * at once and the rest without blocking (the default, a window's), every
 * one at once (the program waits in each load and each layout), or every
 * one without blocking, the first page too (the program settles the view).
 */
enum browser_fetch {
	BROWSER_FETCH_DEFAULT,
	BROWSER_FETCH_AT_ONCE,
	BROWSER_FETCH_BACKGROUND
};

/* What browser_view_settle does besides the network and the timers: lay the page out and wait for its images. */
#define BROWSER_SETTLE_LAYOUT		0x01U

/* The text dumps of the page shown (the golden files of the tests are made of them). */
enum browser_dump {
	BROWSER_DUMP_DOM,
	BROWSER_DUMP_STYLE,
	BROWSER_DUMP_LAYOUT,
	BROWSER_DUMP_PAINT
};

/* The modifier keys an input says were held. */
#define BROWSER_MOD_SHIFT		0x01U
#define BROWSER_MOD_CTRL		0x02U
#define BROWSER_MOD_ALT			0x04U
#define BROWSER_MOD_META		0x08U

/* The DOM's numbers of the pointer's buttons (the back and forward buttons step through the history). */
#define BROWSER_BUTTON_PRIMARY		0
#define BROWSER_BUTTON_MIDDLE		1
#define BROWSER_BUTTON_SECONDARY	2
#define BROWSER_BUTTON_BACK		3
#define BROWSER_BUTTON_FORWARD		4

/*
 * The fonts a view draws with: paths of TrueType files for the sans-serif
 * text, the monospace text and the characters the two lack (NULL for the
 * system's defaults).  The strings must outlive the view.
 */
struct browser_fonts {
	const char *sans;
	const char *mono;
	const char *fallback;
};

/*
 * The program's callbacks (any may be NULL) and the pointer they get back.
 *
 * - redraw: the content or the scroll changed; draw it again.
 * - title: the document's title changed (after a script set it).
 * - committed: a page became the one shown (a navigation, the history or
 *   a reload); browser_view_url, _title and _can_go tell its state.
 * - load: a load of an http or https page started (it is fetched without
 *   blocking), was stopped, or failed (error is an errno value, reason a
 *   text for a TLS failure, or "").  url is the location loaded.
 * - link: a click or Enter opens a link (href as the document has it);
 *   the program allows the view to follow it, or denies it (and may open
 *   it some other way).
 * - console: a script wrote to the console (level 0 log, 1 warn, 2 error).
 * - script_error: a timer's script failed (error is an errno value).
 */
struct browser_callbacks {
	void (*redraw)(void *context, struct browser_view *view);
	void (*title)(void *context, struct browser_view *view, const char *title);
	void (*committed)(void *context, struct browser_view *view);
	void (*load)(void *context, struct browser_view *view, enum browser_load_state state, const char *url, int error,
	    const char *reason);
	enum browser_policy (*link)(void *context, struct browser_view *view, const char *href);
	void (*console)(void *context, struct browser_view *view, int level, const char *text, size_t length);
	void (*script_error)(void *context, struct browser_view *view, int error);
	void *context;
};

/*
 * The program's GPU a view draws with: its instance, its device and a
 * queue family of the device that draws (the view submits on queue 0 of it
 * when it draws by itself).
 */
struct browser_gpu {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	uint32_t queue_family;
};

/*
 * An image of the program's the view draws into: the image, a 2D view of
 * it as a color attachment, its format (8-bit UNORM; the renderer blends
 * the stored values), the layout the drawing leaves it in, and its size in
 * pixels.  The view clears the whole image first, so whatever layout it
 * was in will do.
 */
struct browser_target {
	VkImage image;
	VkImageView view;
	VkFormat format;
	VkImageLayout new_layout;
	uint32_t width;
	uint32_t height;
};

/* Why a Vulkan call of the engine failed: the call (or the step) and what it returned. */
struct browser_gpu_failure {
	const char *operation;
	VkResult result;
};

/*
 * What a view is made with: the interface's version (BROWSER_API_VERSION),
 * the fonts (NULL, or NULL fields, for the defaults), the callbacks, the
 * outermost stack frame of the thread that uses it (the heaps of its pages
 * scan the stack up to it; the program's calls into the view are made from
 * frames inside it), its size in pixels, how it fetches (zero is
 * BROWSER_FETCH_DEFAULT), and the GPU it draws with (NULL for none yet;
 * browser_view_set_gpu gives it one later).
 */
struct browser_view_options {
	int version;
	const struct browser_fonts *fonts;
	const struct browser_callbacks *callbacks;
	const void *stack_base;
	unsigned width;
	unsigned height;
	enum browser_fetch fetch;
	const struct browser_gpu *gpu;
};

/* Making and ending a view. */
int browser_view_create(const struct browser_view_options *options, struct browser_view **view);
void browser_view_destroy(struct browser_view *view);

/* Loading: a location (a path, file:, data:, http:, https:) as a new step, a link's target, the history, and stopping. */
int browser_view_load(struct browser_view *view, const char *location);
int browser_view_follow(struct browser_view *view, const char *target);
int browser_view_go(struct browser_view *view, int steps);
int browser_view_can_go(const struct browser_view *view, int steps);
void browser_view_stop(struct browser_view *view);

/* The size in pixels. */
int browser_view_resize(struct browser_view *view, unsigned width, unsigned height);

/* The page shown: its location, its title (its location when it has none), and its document's height in pixels. */
const char *browser_view_url(const struct browser_view *view);
const char *browser_view_title(const struct browser_view *view);
double browser_view_document_height(struct browser_view *view);

/* The main loop's side: the descriptors to poll, how long to wait at most (-1: no limit), and the work due. */
size_t browser_view_poll_fds(const struct browser_view *view, struct pollfd *fds, size_t capacity);
int browser_view_timeout(const struct browser_view *view);
void browser_view_process(struct browser_view *view, const struct pollfd *fds, size_t count);

/* How far the page is scrolled, in pixels. */
double browser_view_scroll_y(const struct browser_view *view);

/*
 * The scroll placed by the program (version 2, ws081-p006: a touch
 * screen's scroller that glides after a flick and stretches past an end).
 * browser_view_scroll_to places the scroll at a point of the document in
 * pixels, kept inside the document, with no wheel event to the page (the
 * page gets no scroll event yet, as for the wheel and the keys);
 * browser_view_scroll_range reports how far the page scrolls at most, the
 * document as it is laid out now less the view; browser_view_set_overscroll
 * draws the content shifted by a distance (positive down) without moving
 * the scroll, the gap showing the page's canvas color, at most the view's
 * height either way; the pointer's places do not follow the shift, and a
 * new page starts without it.  The page does not scroll sideways yet: x
 * and dx are not used and the sideways range is 0.  A change calls the
 * redraw callback.  The first two report ENOENT when no page is shown.
 */
int browser_view_scroll_to(struct browser_view *view, double x, double y);
int browser_view_scroll_range(struct browser_view *view, double *largest_x, double *largest_y);
int browser_view_set_overscroll(struct browser_view *view, double dx, double dy);

/*
 * Input, in the view's pixels from its top left, with the modifiers held
 * (BROWSER_MOD_*).  The pointer moves, a button (BROWSER_BUTTON_*) is
 * pressed or let go (a press and a release at nearly the same place are a
 * click), the pointer leaves the view, and the wheel turns (distances in
 * pixels, positive is down and right).  A key is pressed (repeat when it
 * is held) or let go: key and code as the DOM names them ("a" and "KeyA",
 * "Enter", "ArrowDown", " " and "Space"), and the UTF-8 text it types
 * ("" when none).  The program gains or loses the focus.  Each reports 0,
 * or an errno value when the page's scripts or its layout failed.
 */
int browser_view_pointer_move(struct browser_view *view, float x, float y, uint32_t modifiers);
int browser_view_pointer_button(struct browser_view *view, float x, float y, int button, int pressed, uint32_t modifiers);
int browser_view_pointer_leave(struct browser_view *view);
int browser_view_wheel(struct browser_view *view, float x, float y, float delta_x, float delta_y, uint32_t modifiers);
int browser_view_key(struct browser_view *view, const char *key, const char *code, const char *text, int pressed, int repeat, uint32_t modifiers);
int browser_view_focus(struct browser_view *view, int focused);

/*
 * An input method (ws090-p025): whether the focused element takes its text
 * (a text field or a textarea; a password field takes keys only), with the
 * caret's rectangle in the view's pixels (x, y, width, height) for the
 * program's text input; the text it is composing, shown at the caret
 * underlined without changing the value (empty when it goes; begin and
 * end are its cursor's byte offsets, -1 when hidden); and the text it
 * commits, after delete_before and delete_after bytes of UTF-8 around the
 * caret are deleted, which fires input.  Each reports 0, or an errno value
 * when the page's scripts failed.
 */
int browser_view_text_target(struct browser_view *view, float caret[4]);

/*
 * A field by its purpose (ws177-p017): the focus moves, with its ring, to
 * the first control in the document's order that takes text (a text or
 * password field, a textarea), can be focused, is drawn, and whose
 * autocomplete attribute holds the token (ASCII, any case; for example
 * "one-time-code"), and the view scrolls it into view, as Tab would.  The
 * page's focus events fire.  Reports 0 when one has the focus, ENOENT
 * when there is none (the focus stays where it was), or an errno value
 * when the page's scripts or its layout failed.  It is a mutation of the
 * view, not to be called from within one of its callbacks.
 */
int browser_view_focus_field(struct browser_view *view, const char *autocomplete);

/*
 * What an input method reads around the caret (ws177-p019): when the
 * focused element takes its text (as browser_view_text_target says), its
 * value as UTF-8 into text (a NUL-ended room of size bytes; a longer value
 * is cut to a part around the caret, at characters' starts), the caret's
 * byte offset in it, what the control is for (BROWSER_TEXT_PURPOSE_*: by
 * its inputmode, else its type) and its hints (BROWSER_TEXT_HINT_*).  The
 * numbers are text-input-v3's.  Returns 1 with them, 0 when the focus
 * takes no text (or there was no memory).
 */
#define BROWSER_TEXT_PURPOSE_NORMAL	0
#define BROWSER_TEXT_PURPOSE_DIGITS	2
#define BROWSER_TEXT_PURPOSE_NUMBER	3
#define BROWSER_TEXT_PURPOSE_PHONE	4
#define BROWSER_TEXT_PURPOSE_URL	5
#define BROWSER_TEXT_PURPOSE_EMAIL	6
#define BROWSER_TEXT_HINT_MULTILINE	0x200U
int browser_view_text_context(struct browser_view *view, char *text, size_t size, size_t *cursor, int *purpose, unsigned *hints);
int browser_view_compose(struct browser_view *view, const char *preedit, int begin, int end);
int browser_view_commit_text(struct browser_view *view, const char *text, uint32_t delete_before, uint32_t delete_after);

/*
 * The text input's session (ws177-p019): a number that changes when the
 * focus moves to another element, when another page is shown, and when
 * the page itself ended what an input method was composing (a click puts
 * it into the value, a script's value drops it).  The program starts its
 * input method's text input again when it changes, so that the input
 * method drops what it was composing too.  A pure query.
 */
uint64_t browser_view_text_session(const struct browser_view *view);

/*
 * The side without a window: the page brought to rest (the page being
 * fetched has arrived, its timers have run on a virtual clock up to budget
 * milliseconds, and with BROWSER_SETTLE_LAYOUT it is laid out with the
 * images its layout asked for), drawn with the CPU into 0xAARRGGBB pixels
 * whose rows are stride bytes apart, or dumped as text (*text is the
 * program's to free).
 */
int browser_view_settle(struct browser_view *view, double budget, unsigned flags);
int browser_view_draw_pixels(struct browser_view *view, uint32_t *pixels, unsigned width, unsigned height, size_t stride);
int browser_view_dump(struct browser_view *view, enum browser_dump kind, char **text, size_t *length);

/*
 * The GPU side: the device the view draws with from now on (NULL to let
 * the old one go: the view releases what it made on it, and the program
 * may then destroy it), the framebuffers the view keeps for the program's
 * images forgotten (before the program destroys those images, once the
 * drawing into them has finished), and the page laid out now (so that a
 * frame begun cannot fail on the layout).
 *
 * Drawing into a target, the view scrolled as it is: submitted by the view
 * after wait and signalling signal (VK_NULL_HANDLE for none), waited for;
 * or recorded, render pass and all, into a command buffer the program
 * began, which the program submits.  Before the view draws or records
 * again, the work it recorded must have finished (the program waits for
 * its fence).  Both report 0, ENOENT when no page is shown, ENODEV without
 * a GPU, a layout's error, or EIO when a Vulkan call failed
 * (browser_view_gpu_failure says which).
 */
void browser_view_set_gpu(struct browser_view *view, const struct browser_gpu *gpu);
void browser_view_release_targets(struct browser_view *view);
int browser_view_prepare(struct browser_view *view);
int browser_view_draw(struct browser_view *view, const struct browser_target *target, VkSemaphore wait, VkSemaphore signal);
int browser_view_record(struct browser_view *view, const struct browser_target *target, VkCommandBuffer commands);
void browser_view_gpu_failure(const struct browser_view *view, struct browser_gpu_failure *failure);

/*
 * An offscreen image of the engine's own, for a program without a window:
 * a device of its own with one image of a size, the GPU and the target to
 * give a view, and the image read back into 0xAARRGGBB pixels (rows stride
 * bytes apart) once a view has drawn into it.  The view lets the
 * offscreen's GPU go before the offscreen is destroyed.  A failure is EIO
 * with the Vulkan call in *failure.
 */
int browser_offscreen_create(unsigned width, unsigned height, struct browser_offscreen **offscreen, struct browser_gpu_failure *failure);
void browser_offscreen_target(const struct browser_offscreen *offscreen, struct browser_gpu *gpu, struct browser_target *target);
int browser_offscreen_read(struct browser_offscreen *offscreen, uint32_t *pixels, size_t stride, struct browser_gpu_failure *failure);
void browser_offscreen_destroy(struct browser_offscreen *offscreen);

/* The process-wide settings of the engine: the certificate authorities trusted besides the system's (https). */
int browser_add_ca_file(const char *path);

/*
 * The script tools of the test suites (the /bin/browser modes --js,
 * --dump=ast and --dump=code): a JavaScript file run in a realm of its own
 * with print, or its syntax tree or its compiled code written to standard
 * output, as strict code or a module when the flags say so.  A syntax
 * error, what the engine does not support yet, or an uncaught exception
 * is written to standard error.  stack_base is the caller's outermost
 * frame, as for a view.  Returns the program's exit status: 0, 1 for a
 * script that failed, 2 for a misuse.
 */
#define BROWSER_SCRIPT_STRICT		0x01U
#define BROWSER_SCRIPT_MODULE		0x02U

/* What a script tool does with the file: runs it, or writes its syntax tree or its code. */
enum browser_script_tool {
	BROWSER_SCRIPT_RUN,
	BROWSER_SCRIPT_DUMP_AST,
	BROWSER_SCRIPT_DUMP_CODE
};

/* Runs a script tool on a file. */
int browser_script_tool(enum browser_script_tool tool, const char *path, unsigned flags, const void *stack_base);

#ifdef __cplusplus
}
#endif

#endif
