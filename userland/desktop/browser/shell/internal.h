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

/* The Wayland platform's parts of Vulkan (the instance's surface extension), declared before anything includes vulkan.h. */
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <poll.h>

#include "shell/shell.h"
#include "shell/touch.h"

#include <keiland/keiland.h>

/*
 * How many descriptors of the network the main loop polls at most besides
 * the compositor's (ws177-p019: more than the application watches, the
 * rest polled by the shell itself), and how many of them the application
 * watches at most (libkeiland's own limit).
 */
#define SHELL_NET_FDS		256U
#define SHELL_WATCHED_MAX	KL_APP_FDS_MAX

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
	SHELL_EVENT_TEXT_DELETE
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
 * held.  An input method's text (ws090-p025) is the text it commits, the
 * text it composes with its cursor's byte offsets (begin and end, -1 when
 * hidden), or the bytes it deletes before and after the caret.
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
};

/*
 * A key as the view names it (keys.c): its DOM code and key, and the text
 * it types ("" when none).  The strings live for the life of the program.
 */
struct shell_key_names {
	const char *code;
	const char *key;
	const char *text;
};

struct shell_titlebar;

/*
 * The window: libkeiland's application and its one window (shown with
 * KL_PRESENT_NONE; the presenter draws on its surface), the size the
 * compositor gave it, the network's descriptors the application watches,
 * and the input waiting for the main loop.  What is done with the titlebar
 * goes to the titlebar's queue (titlebar.c).
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

	/* The modifiers held (SHELL_MOD_*), and the pointer's place in the window. */
	uint32_t modifiers;
	int pointer_x;
	int pointer_y;

	/*
	 * The network's descriptors the application watches now, and how many
	 * of the last round's it could not watch (polled by the shell itself;
	 * the log tells when that number changes).
	 */
	int watched[SHELL_WATCHED_MAX];
	unsigned watched_count;
	size_t unwatched_count;

	/* The titlebar that hears its controls' inputs (NULL until it opens). */
	struct shell_titlebar *titlebar;

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

	/* The Vulkan call that failed last, for the error line. */
	const char *operation;
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
};

/*
 * The sign-in codes of mail (mail.c, WS169 p005): the application's system
 * (NULL when the compositor tells no arrivals), the code offered (empty
 * for none) and the label of its titlebar control, until when, the request that posted its notification and the
 * notification's number (0 before it comes or after it closed).
 */
struct shell_mail {
	struct kl_system *system;
	char code[KL_MAIL_CODE_MAX];
	char label[KL_MAIL_CODE_MAX + 8U];
	uint64_t until_ms;
	uint32_t request;
	uint32_t notification;
};

/* The window (window.c). */
int shell_window_open(struct shell_window *window, const char *display, uint32_t width, uint32_t height, const char *title);
int shell_window_dispatch(struct shell_window *window, int timeout, struct pollfd *extra, size_t extra_count);
int shell_window_take(struct shell_window *window, struct shell_event *event);
void shell_window_title(struct shell_window *window, const char *title);
void shell_window_close(struct shell_window *window);
uint64_t shell_clock(void);

/* The keys (keys.c). */
void shell_key_names(uint32_t evdev, uint32_t modifiers, struct shell_key_names *names);
uint32_t shell_key_modifiers(uint32_t modifiers);
int shell_key_button(uint32_t button);

/* The titlebar (titlebar.c). */
int shell_titlebar_open(struct shell_titlebar *titlebar, struct shell_window *window);
int shell_titlebar_show(struct shell_titlebar *titlebar, int can_back, int can_forward, const char *path);
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
void shell_mail_close(struct shell_mail *mail, struct shell_titlebar *titlebar);

/* The presenter (present.c). */
VkResult shell_present_open(struct shell_present *present, struct shell_window *window, struct browser_view *view);
VkResult shell_present_resize(struct shell_present *present, struct browser_view *view, uint32_t width, uint32_t height);
VkResult shell_present_frame(struct shell_present *present, struct browser_view *view);
void shell_present_close(struct shell_present *present, struct browser_view *view);

#endif
