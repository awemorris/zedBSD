/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of Image Viewer that speak Wayland, Vulkan and the compositor's
 * extensions: the window (libkeiland's kl_window since ws090-p008: the
 * toplevel and the seat's input; its surface is left to the presenter),
 * the presenter of the image and the drawn canvas (present.c), the menus
 * (menu.c), the titlebar's controls (titlebar.c) and the window's glass
 * (glass.c).  The host tests build the rest of the program without them.
 */

#ifndef IMAGEVIEW_WINDOW_H
#define IMAGEVIEW_WINDOW_H

/* The Vulkan header first, so that <keiland/keiland.h> declares kl_window_vulkan_surface. */
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>

#include "imageview.h"
#include "touch.h"

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <keiland/keiland.h>

/*
 * The window: libkeiland's window, which queues the input (the menus' and
 * the titlebar's actions among it, posted in the order they came).  The
 * viewer's own presenter draws on its surface (KL_PRESENT_NONE): the
 * image is a texture under the canvas, which libkeiland's presenter does not
 * have.
 *
 * One lives for the whole run.
 */
struct iv_window {
	struct kl_window *kui;
};

/*
 * One swapchain image the presenter draws into; the image is the
 * swapchain's.
 */
struct iv_present_target {
	VkImage image;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkSemaphore rendered;
};

/*
 * One level of the image as a texture: a linear, host-written image with
 * its memory (mapped for good), view, and the sets that bind it smoothly
 * and to the nearest texel; its size, and whether it left its
 * preinitialized layout.
 */
struct iv_present_level {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkDescriptorSet smooth_set;
	VkDescriptorSet nearest_set;
	unsigned char *map;
	size_t pitch;
	int width;
	int height;
	int ready;
};

/*
 * The Vulkan objects that show the frames in the window: a swapchain; the
 * image's levels as textures, one of which a quad draws where the view
 * places it; and a host-written canvas image the size of the window that
 * a second quad lays over it by its premultiplied alpha.
 */
struct iv_present {
	/* The instance, the surface of the window, the device and its queue. */
	VkInstance instance;
	VkSurfaceKHR surface;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;

	/* The swapchain, its format and extent, and one target per image. */
	VkSwapchainKHR swapchain;
	VkFormat format;
	/* Whether the swapchain is see-through: the compositor blends the frame by its premultiplied alpha. */
	int premultiplied;
	VkExtent2D extent;
	struct iv_present_target *targets;
	uint32_t count;

	/* The pass and the pipeline that draw the canvas, and what the pipeline binds. */
	VkRenderPass pass;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet set;
	VkSampler sampler;
	VkSampler smooth_sampler;

	/* The image's levels, the image they are of (image_serial), and whether they can be sampled smoothly and how large one may be. */
	struct iv_present_level levels[IV_LEVELS_MAX];
	size_t level_count;
	/* Oversized textures use CPU sampling into the window-sized canvas. */
	int cpu_image;
	unsigned image_serial;
	int has_image;
	int smooth;
	int max_dimension;

	/* The canvas image (linear, host-written), its memory, view and map, and whether it was made ready. */
	VkImage canvas;
	VkDeviceMemory canvas_memory;
	VkImageView canvas_view;
	unsigned char *canvas_map;
	size_t canvas_pitch;
	int canvas_ready;

	/* The two quads' vertices (the canvas's, then the image's), in host-visible memory mapped for good. */
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	float *vertex_map;

	/* One command buffer, the fence that says it finished and the acquire semaphore. */
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkSemaphore acquired;

	/* The Vulkan call that failed last, for the error line; the last frame's copy, acquire, present and wait times (milliseconds). */
	const char *operation;
	unsigned copy_ms;
	unsigned acquire_ms;
	unsigned present_ms;
	unsigned wait_ms;
};

/*
 * What the menus and the titlebar show of the viewer: whether an image is
 * shown (and can be), its place and the folder's count, whether it is
 * fitted, animated (and playing) and fullscreen, and whether a slideshow
 * runs (ws128-p005).
 */
struct iv_state {
	int has_image;
	int can_show;
	size_t index;
	size_t count;
	int fit;
	int animated;
	int playing;
	int fullscreen;
	int slideshow;
};

/*
 * The window's glass in the compositor (glass.c, WS131 p017): the window, and
 * whether it is glass (libkeiland sends the panels only when they change).
 */
struct iv_glass {
	struct iv_window *window;
	int on;
	size_t count;
	int sent;
};

/* The action of File > Open With's submenu itself (greyed without an image; nothing is done when it is chosen). */
#define IV_ACTION_OPEN_WITH_MENU	97U

/* The action of the titlebar's place control ("3 / 12"), which does nothing but show its state. */
#define IV_ACTION_PLACE_INFO	98U

/*
 * The window's menus as given to libkeiland (menu.c, WS131 p017): the
 * window, whether the menu was given (not without the compositor's System
 * Menu), the state the actions last showed and whether it was sent, and
 * the names of Open With's applications for the image shown (ws128-p005;
 * opener_count -1 before the first).
 */
struct iv_menu {
	struct iv_window *window;
	int shown_once;
	struct iv_state shown;
	int sent;
	char openers[IV_OPENERS][IV_OPENER_NAME];
	int opener_count;
};

/*
 * The window's titlebar in the compositor (titlebar.c, WS131 p017): the window,
 * and whether its controls are shown (not without the compositor's
 * titlebar).  The controls' state is their actions' (menu.c).
 */
struct iv_titlebar {
	struct iv_window *window;
	int shown;
};

/* The menus (menu.c). */
int iv_menu_open(struct iv_menu *menu, struct iv_window *window, const struct iv_state *state);
void iv_menu_refresh(struct iv_menu *menu, const struct iv_state *state);
void iv_menu_close(struct iv_menu *menu);
void iv_menu_context(struct iv_menu *menu, const struct iv_state *state, int x, int y);
void iv_menu_openers(struct iv_menu *menu, char names[][IV_OPENER_NAME], int count);

/* The titlebar's controls (titlebar.c). */
int iv_titlebar_open(struct iv_titlebar *titlebar, struct iv_window *window, const struct iv_state *state);
void iv_titlebar_refresh(struct iv_titlebar *titlebar, const struct iv_state *state);
void iv_titlebar_close(struct iv_titlebar *titlebar);

/* The presenter (present.c). */
VkResult iv_present_open(struct iv_present *present, struct iv_window *window);
VkResult iv_present_resize(struct iv_present *present, uint32_t width, uint32_t height);
VkResult iv_present_frame(struct iv_present *present, const uint32_t *pixels, size_t stride, int canvas_changed, const struct iv_quad *quad, uint32_t ground);
VkResult iv_present_set_image(struct iv_present *present, const struct iv_image *image, unsigned serial);
void iv_present_set_frame(struct iv_present *present, const uint32_t *pixels);
void iv_present_close(struct iv_present *present);

/* The glass (glass.c). */
int iv_glass_open(struct iv_glass *glass, struct iv_window *window, const struct iv_present *present, const struct iv_app *app);
void iv_glass_update(struct iv_glass *glass, const struct iv_app *app);
void iv_glass_close(struct iv_glass *glass);

#endif
