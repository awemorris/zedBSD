/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the shell that speak to the desktop and to Vulkan: the
 * window (window.c, a window of libkeiland's application, WS131 p025), the
 * names of its keys for the view (keys.c) and the presenter that shows the
 * view's frames in it (present.c).  The host build leaves the whole
 * directory out.
 */

#ifndef KEILAND_BROWSER_SHELL_INTERNAL_H
#define KEILAND_BROWSER_SHELL_INTERNAL_H

#include <vulkan/vulkan.h>

#include "shell/shell.h"
#include "shell/touch.h"
#include "shell/unresponsive.h"

#include <keiland/keiland.h>

/* The code of the event the view's wake posts to the application (KL_APP_POST): the view has work. */
#define SHELL_POST_VIEW		1U

/* The code of the diagnostic clock's tick (KL_APP_POST, latency.c): how long the window's thread took to take it. */
#define SHELL_POST_TICK		2U

/*
 * The code of the window's input (KL_WINDOW_POST) by which the drawing
 * thread asks the window's thread to acknowledge the configure it is about
 * to show a frame of (render.c); its data is NULL.
 */
#define SHELL_POST_ACK		0x4F474D32U

/*
 * The code of the window's input (KL_WINDOW_POST) that brings the URLs a
 * later start of the program hands over (Windows' single start): its data
 * is the URLs in UTF-8, each ended by a NUL, the list by an empty one,
 * for the program to free.
 */
#define SHELL_POST_OPEN		0x4F474D31U

/* How many inputs wait for the main loop at most. */
#define SHELL_WINDOW_EVENTS	256U

/* How many touch inputs wait for the main loop at most (ws081-p006). */
#define SHELL_WINDOW_TOUCHES	256U

/* The kinds of input the window queues. */
enum shell_event_type {
	SHELL_EVENT_KEY,
	SHELL_EVENT_SCROLL,
	SHELL_EVENT_BUTTON,
	SHELL_EVENT_MOTION,
	SHELL_EVENT_LEAVE,
	SHELL_EVENT_FOCUS,
	SHELL_EVENT_TEXT_COMMIT,
	SHELL_EVENT_TEXT_PREEDIT,
	SHELL_EVENT_TEXT_DELETE,
	SHELL_EVENT_OPEN
};

/* The modifier bits of an input. */
#define SHELL_MOD_SHIFT		0x01U
#define SHELL_MOD_CTRL		0x02U
#define SHELL_MOD_ALT		0x04U
#define SHELL_MOD_META		0x08U

/* The evdev codes of the pointer's buttons the window names. */
#define SHELL_BUTTON_LEFT	0x110U
#define SHELL_BUTTON_RIGHT	0x111U
#define SHELL_BUTTON_MIDDLE	0x112U
#define SHELL_BUTTON_SIDE	0x113U
#define SHELL_BUTTON_EXTRA	0x114U

/*
 * One input for the main loop: a key pressed, repeated or let go (its
 * evdev code), a scroll of some pixels (scroll down and scroll_x right are
 * positive), a pointer button pressed or let go (its evdev code), the
 * pointer moving or leaving, or the keyboard's focus gained or lost
 * (pressed), with the pointer's place in the window and the modifiers
 * held.  A key pressed carries in text the character it types, in UTF-8,
 * when the window's backend knows it (empty otherwise).  An input method's
 * text (ws090-p025) is the text it commits, the
 * text it composes with its cursor's byte offsets (begin and end, -1 when
 * hidden), or the bytes it deletes before and after the caret.  An open
 * (SHELL_EVENT_OPEN) carries the URLs a later start handed over in data
 * (SHELL_POST_OPEN's list, which the one who takes the event frees).
 */
struct shell_event {
	int type;
	uint32_t key;
	int repeat;
	int scroll;
	int scroll_x;
	uint32_t button;
	int pressed;
	int x;
	int y;
	uint32_t modifiers;
	char text[KL_WINDOW_TEXT_MAX];
	int32_t begin;
	int32_t end;
	uint32_t before;
	uint32_t after;
	char *data;
};

/*
 * A key as the view names it (keys.c): its DOM code and key, and the text
 * it types ("" when none).  The strings live for the life of the program,
 * except that a key or text the backend named is in typed.
 */
struct shell_key_names {
	const char *code;
	const char *key;
	const char *text;
	char typed[KL_WINDOW_TEXT_MAX];
};

struct shell_titlebar;
struct shell_render;
struct shell_latency;

/*
 * The window: libkeiland's application and its one window (shown with
 * KL_PRESENT_NONE, the presenter drawing on its surface, or with
 * KL_PRESENT_SHM, the CPU presenter's memory shown), the size the
 * compositor gave it, and the input waiting for the main loop.  What is
 * done with the titlebar goes to the titlebar's queue (titlebar.c).
 *
 * One lives for the whole run.
 */
struct shell_window {
	/* The application (the connection to the compositor) and its window. */
	struct kl_app *app;
	struct kl_window *kui;

	/* The size the compositor asked for, and whether it changed since it was last taken. */
	uint32_t width;
	uint32_t height;
	int resized;

	/* Whether the compositor asked to close. */
	int closed;

	/* Whether the desktop's appearance (light or dark) changed since it was last taken. */
	int themed;

	/* Whether the desktop's language changed since it was last taken, and the one it changed to ("en", "ja", ...). */
	int languaged;
	char language[KL_TR_NAME_MAX + 1U];

	/* The modifiers held (SHELL_MOD_*), and the pointer's place in the window. */
	uint32_t modifiers;
	int pointer_x;
	int pointer_y;

	/* The titlebar that hears its controls' inputs (NULL until it opens). */
	struct shell_titlebar *titlebar;

	/* The drawing thread whose acknowledgements the window carries out (NULL until it starts). */
	struct shell_render *render;

	/* The diagnostic clock whose ticks the window times (NULL when it is not running). */
	struct shell_latency *latency;

	/* The inputs waiting, a ring: the oldest's slot and how many. */
	struct shell_event events[SHELL_WINDOW_EVENTS];
	unsigned event_first;
	unsigned event_count;

	/* The touch inputs not yet taken by the main loop, oldest first (ws081-p006; a full queue drops the newest). */
	struct shell_touch_event touches[SHELL_WINDOW_TOUCHES];
	unsigned touch_count;
};

/*
 * One swapchain image the view draws into: the image (the swapchain's),
 * the view of it the view's framebuffer is made over, and the semaphore
 * its present waits for.
 */
struct shell_target {
	VkImage image;
	VkImageView view;
	VkSemaphore rendered;
};

/*
 * The Vulkan objects that show the page in the window: the instance with
 * the window's surface, the device (lent to the view, which draws with
 * it), the swapchain with its images, and the one command buffer each
 * frame is recorded into with the fence its submission signals.  The
 * presenter keeps only the swapchain and the synchronization; the drawing
 * is the view's (browser_view_record).
 */
struct shell_present {
	/* The instance, the surface of the window, the device and its queue family. */
	VkInstance instance;
	VkSurfaceKHR surface;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;

	/* The swapchain, its format and extent, and one target per image. */
	VkSwapchainKHR swapchain;
	VkFormat format;
	VkExtent2D extent;
	struct shell_target *targets;
	uint32_t count;

	/* The semaphore the acquire signals. */
	VkSemaphore acquired;

	/* The frame's command buffer and its fence, and whether a submitted frame may still run. */
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	int in_flight;

	/* Whether the swapchain's images take copies (the question's card, copied over the page), and the card's host memory. */
	int copy;
	VkBuffer overlay;
	VkDeviceMemory overlay_memory;
	void *overlay_mapped;
	VkDeviceSize overlay_size;

	/* The Vulkan call that failed last, for the error line. */
	const char *operation;
};

/*
 * A picture copied over the page's frame in its middle (the question
 * about a page that does not respond, prompt.c): 0xAARRGGBB words, width
 * by height, packed rows (width 0: none), and how many words the memory
 * holds.
 */
struct shell_overlay {
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	size_t capacity;
};

/*
 * The CPU presenter (present-cpu.c): the size libkeiland shows frames at in
 * the window (the drawing thread draws the frames, render.c).
 */
struct shell_cpu {
	uint32_t width;
	uint32_t height;
};

/*
 * One frame of the CPU's way (render.c): the memory the drawing thread
 * drew the page into (0xAARRGGBB words, width by height, packed rows), and
 * its size.
 */
struct shell_cpu_frame {
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
};

/* How many frames the CPU's way keeps: one being drawn, one drawn last, one being shown. */
#define SHELL_CPU_FRAMES	3U

/*
 * The drawing thread (render.c): it draws the view's latest committed
 * frame whenever the view commits one (the view's frame_wake), and at
 * every new size the window gives it, with the GPU into the swapchain of
 * the presenter (present.c) or with the CPU into frames the window's
 * thread shows (present-cpu.c).
 *
 * view, kui, present (NULL for the CPU's way), wake and wake_context are
 * set before the thread starts and only read after.  thread is the
 * window's thread's.  Everything from stop on is under lock; changed is
 * broadcast whenever one of them changes:
 *
 * - stop: the window's thread asked the drawing thread to end; ended: it did.
 * - committed: the view committed a frame since the last drawing;
 *   has_commit: it committed one (nothing is drawn before).
 * - width and height: the size to draw at (zero: nothing is drawn, a
 *   window minimized); sized: it changed since the drawing thread took it.
 * - configure and configure_serial: a configure of the window
 *   (kl_window_configure_take) that waits to be acknowledged before the
 *   first frame of its size is shown.
 * - ack_state, ack_serial and ack_error: the acknowledgement the drawing
 *   thread asked the window's thread for (SHELL_POST_ACK) and its answer.
 * - failed: the drawing ended on an error (said in a line); the window closes.
 * - shown, shown_width and shown_height: how many frames the GPU's way has
 *   presented, and the size of the last.
 * - frames, back, ready, front and fresh: the CPU's frames, the one the
 *   drawing thread draws into (back, its own between the swaps), the one
 *   it drew last (ready, new for the window when fresh), and the one the
 *   window shows (front, the window's own).
 *
 * - overlay and overlay_changed: the picture the window's thread gives to
 *   copy over every frame (shell_render_overlay; width 0: none), and
 *   whether it changed since the drawing thread took it; the drawing
 *   thread copies it into drawn_overlay, its own, before it draws.
 *
 * told is set (atomically) when the drawing thread woke the window's
 * thread and cleared by the window's thread before it reads the state, so
 * that a run of frames wakes it once.
 */
struct shell_render {
	struct browser_view *view;
	struct kl_window *kui;
	struct shell_present *present;
	void (*wake)(void *context);
	void *wake_context;
	struct kl_thread *thread;
	int made;
	struct kl_mutex lock;
	struct kl_cond changed;
	int stop;
	int ended;
	int committed;
	int has_commit;
	uint32_t width;
	uint32_t height;
	int sized;
	int configure;
	uint32_t configure_serial;
	int ack_state;
	uint32_t ack_serial;
	int ack_error;
	int failed;
	unsigned long shown;
	uint32_t shown_width;
	uint32_t shown_height;
	struct shell_cpu_frame frames[SHELL_CPU_FRAMES];
	unsigned back;
	unsigned ready;
	unsigned front;
	int fresh;
	struct shell_overlay overlay;
	int overlay_changed;
	struct shell_overlay drawn_overlay;
	int told;
};

/*
 * The question about a page that does not respond (prompt.c), all the
 * window's thread's: the rule of when (unresponsive.c), the widgets' input
 * and the text it is drawn with (ui NULL: never asked), whether the card
 * shows and for which task, how long it had run then in seconds, the
 * page's name and the card's words, the card's memory (pixel_count words)
 * and its size and place in the window (whose size it was laid out in),
 * the lines of its explanation, its buttons in the card, and whether they
 * still move (drawn again soon).
 */
struct shell_prompt {
	struct shell_unresponsive rule;
	struct kl_ui *ui;
	struct kl_text text;
	int text_open;
	int shown;
	uint32_t task;
	uint32_t seconds;
	char name[128];
	char title[256];
	char body[1024];
	char wait_label[64];
	char stop_label[64];
	uint32_t *pixels;
	size_t pixel_count;
	int width;
	int height;
	int x;
	int y;
	uint32_t window_width;
	uint32_t window_height;
	int lines;
	struct kl_rect wait;
	struct kl_rect stop;
	int moving;
};

/*
 * The diagnostic clock of the window's thread (latency.c, run when the
 * environment has BROWSER_LATENCY): a thread of its own posts a tick to
 * the application (SHELL_POST_TICK) every few milliseconds, when the last
 * was taken, and the window's thread times how long each waited to be
 * taken; the longest and how many waited long are said in one line when
 * the window closes, and each wait over 100 ms as it is taken.  thread,
 * app and started_us are set before the thread starts;
 * stop, waiting and sent_us are atomic; the counts are the window's.
 */
struct shell_latency {
	struct kl_app *app;
	struct kl_thread *thread;
	int stop;
	int waiting;
	_Alignas(8) uint64_t sent_us;
	uint64_t started_us;
	uint64_t longest_us;
	uint64_t longest_at_us;
	unsigned long samples;
	unsigned long over_20ms;
	unsigned long over_100ms;
};

/* The titlebar's controls. */
#define SHELL_CONTROL_BACK	1U
#define SHELL_CONTROL_FORWARD	2U
#define SHELL_CONTROL_RELOAD	3U
#define SHELL_CONTROL_LOCATION	4U
#define SHELL_CONTROL_CODE	5U	/* a sign-in code offered (WS169 p005), only while there is one */

/* How many things done with the titlebar wait for the main loop at most, and the longest text kept. */
#define SHELL_TITLEBAR_EVENTS	16U
#define SHELL_TITLEBAR_TEXT	1024U

/* The kinds of thing done with the titlebar. */
enum shell_titlebar_kind {
	SHELL_TITLEBAR_ACTIVATED,
	SHELL_TITLEBAR_DONE
};

/*
 * One thing done with the titlebar: a control chosen (detail is a
 * breadcrumb's part), or a text control's editing ended (detail is how,
 * KL_TEXT_*, and text is its text).
 */
struct shell_titlebar_event {
	int kind;
	uint32_t id;
	uint32_t detail;
	char text[SHELL_TITLEBAR_TEXT];
};

/*
 * The window's titlebar controls (titlebar.c): the window they are
 * declared on (NULL when the compositor has no titlebar for them) and what
 * the user did with them and the main loop has not yet carried out, oldest
 * first.
 */
struct shell_titlebar {
	struct kl_window *kui;
	struct shell_titlebar_event events[SHELL_TITLEBAR_EVENTS];
	unsigned event_count;
	/* The load the location's box shows, in thousandths (-1 for none). */
	int progress;
};

/*
 * The sign-in codes of mail (mail.c, WS169 p005): the application's system
 * (NULL when the compositor tells no arrivals), the code offered (empty
 * for none) and the label of its titlebar control, until when, the request that posted its notification and the
 * notification's number (0 before it comes or after it closed).  asked says the view was asked for the page's
 * field for one-time codes and its answer (the view's field callback, shell_mail_field) has not come.
 */
struct shell_mail {
	struct kl_system *system;
	char code[KL_MAIL_CODE_MAX];
	char label[KL_MAIL_CODE_MAX + 8U];
	uint64_t until_ms;
	uint32_t request;
	uint32_t notification;
	int asked;
};

/* The window (window.c). */
int shell_window_connect(struct shell_window *window, const char *display);
int shell_window_open(struct shell_window *window, uint32_t width, uint32_t height, const char *title, unsigned present);
void shell_window_drop(struct shell_window *window);
int shell_window_dispatch(struct shell_window *window, int timeout);
int shell_window_take(struct shell_window *window, struct shell_event *event);
void shell_window_title(struct shell_window *window, const char *title);
void shell_window_close(struct shell_window *window);
uint64_t shell_clock(void);

/* The keys (keys.c). */
void shell_key_names(uint32_t evdev, uint32_t modifiers, const char *typed, struct shell_key_names *names);
uint32_t shell_key_modifiers(uint32_t modifiers);
int shell_key_button(uint32_t button);

/* The titlebar (titlebar.c). */
int shell_titlebar_open(struct shell_titlebar *titlebar, struct shell_window *window);
int shell_titlebar_show(struct shell_titlebar *titlebar, int can_back, int can_forward, const char *path);
void shell_titlebar_progress(struct shell_titlebar *titlebar, int progress);
int shell_titlebar_edit_location(struct shell_titlebar *titlebar);
int shell_titlebar_offer_code(struct shell_titlebar *titlebar, const char *label);
int shell_titlebar_take(struct shell_titlebar *titlebar, struct shell_titlebar_event *event);
void shell_titlebar_post(struct shell_titlebar *titlebar, const struct kl_window_event *event);
void shell_titlebar_close(struct shell_titlebar *titlebar);

/* The sign-in codes of mail (mail.c). */
void shell_mail_open(struct shell_mail *mail, struct kl_app *app);
int shell_mail_round(struct shell_mail *mail, struct browser_view *view, struct shell_titlebar *titlebar, uint64_t now_ms);
int shell_mail_timeout(const struct shell_mail *mail, uint64_t now_ms);
void shell_mail_fill(struct shell_mail *mail, struct browser_view *view, struct shell_titlebar *titlebar);
void shell_mail_field(struct shell_mail *mail, struct browser_view *view, struct shell_titlebar *titlebar, int error);
void shell_mail_close(struct shell_mail *mail, struct shell_titlebar *titlebar);

/* The CPU presenter (present-cpu.c). */
int shell_cpu_open(struct shell_cpu *cpu, struct shell_window *window);
int shell_cpu_resize(struct shell_cpu *cpu, struct shell_window *window);
int shell_cpu_show(struct shell_cpu *cpu, struct shell_window *window, const struct shell_cpu_frame *frame, int *dropped);
void shell_cpu_close(struct shell_cpu *cpu);

/* The presenter (present.c). */
VkResult shell_present_open(struct shell_present *present, struct shell_window *window, struct browser_view *view);
VkResult shell_present_resize(struct shell_present *present, struct browser_view *view, uint32_t width, uint32_t height);
VkResult shell_present_frame(struct shell_present *present, struct browser_view *view, const struct shell_overlay *overlay, uint64_t *serial);
void shell_present_close(struct shell_present *present, struct browser_view *view);

/* The drawing thread (render.c). */
int shell_render_init(struct shell_render *render);
void shell_render_frame_wake(void *context);
int shell_render_start(struct shell_render *render, struct browser_view *view, struct kl_window *kui, struct shell_present *present,
    void (*wake)(void *context), void *wake_context);
void shell_render_resize(struct shell_render *render, uint32_t width, uint32_t height, int configure, uint32_t serial);
void shell_render_ack(struct shell_render *render);
const struct shell_cpu_frame *shell_render_cpu_frame(struct shell_render *render, int *fresh);
int shell_render_status(struct shell_render *render, unsigned long *shown, uint32_t *width, uint32_t *height);
void shell_render_overlay(struct shell_render *render, const uint32_t *pixels, uint32_t width, uint32_t height);
void shell_render_place_overlay(const struct shell_overlay *overlay, uint32_t width, uint32_t height, struct kl_rect *place);
void shell_render_stop(struct shell_render *render, struct shell_window *window);
void shell_render_close(struct shell_render *render);

/* The question about a page that does not respond (prompt.c). */
void shell_prompt_open(struct shell_prompt *prompt, const struct browser_fonts *fonts, const char *language);
int shell_prompt_round(struct shell_prompt *prompt, struct browser_view *view, struct shell_render *render, uint64_t now_ms);
int shell_prompt_input(struct shell_prompt *prompt, struct browser_view *view, struct shell_render *render, const struct shell_event *event,
    uint64_t now_ms);
void shell_prompt_resize(struct shell_prompt *prompt, struct shell_render *render, uint32_t width, uint32_t height, uint64_t now_ms);
void shell_prompt_redraw(struct shell_prompt *prompt, struct shell_render *render, uint64_t now_ms);
void shell_prompt_language(struct shell_prompt *prompt, struct shell_render *render, const char *language);
void shell_prompt_close(struct shell_prompt *prompt);

/* The diagnostic clock of the window's thread (latency.c). */
int shell_latency_start(struct shell_latency *latency, struct kl_app *app);
void shell_latency_take(struct shell_latency *latency);
void shell_latency_stop(struct shell_latency *latency);

#endif
