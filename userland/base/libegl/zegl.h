/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of zedBSD's EGL (WS068, plan/ws068/design.md), shared by
 * libEGL and libGLESv2.
 *
 * An EGL display owns a Vulkan instance and device; a window surface owns
 * a Vulkan surface, its swapchain, a depth buffer when its config has one,
 * (a pbuffer: one offscreen colour image instead of the swapchain)
 * one framebuffer per image, and the frame being recorded; a context
 * holds the GLES state.  libGLESv2 finds the calling thread's context with
 * zegl_current_context(), opens its draw surface's frame with
 * zegl_frame_begin() and records into it inside zegl_frame_pass();
 * eglSwapBuffers submits and presents the frame and then tells libGLESv2
 * that its per-frame resources are free again.
 */

#ifndef ZEGL_H
#define ZEGL_H

#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <wayland-egl-core.h>

#include "../libwayland-egl/wayland-egl-backend.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <stdint.h>

/* The platforms a display can be on. */
#define ZEGL_PLATFORM_WAYLAND		1
#define ZEGL_PLATFORM_DISPLAY		2
#define ZEGL_PLATFORM_SURFACELESS	3

/* How many configs a display offers, and the images a swapchain may have. */
#define ZEGL_CONFIGS		8U
#define ZEGL_IMAGES		8U

/*
 * One framebuffer configuration an application may choose.
 */
struct zegl_config {
	/* The config's EGL_CONFIG_ID (1 up). */
	EGLint id;

	/* The bits of each channel of the colour, depth and stencil buffers. */
	EGLint red;
	EGLint green;
	EGLint blue;
	EGLint alpha;
	EGLint depth;
	EGLint stencil;

	/* The surfaces (EGL_WINDOW_BIT, ...) and client APIs (EGL_OPENGL_ES2_BIT, ...) it serves. */
	EGLint surface_type;
	EGLint renderable;
};

/*
 * One EGL display: a platform, its native display, and the Vulkan objects
 * every surface and context of the display shares.
 *
 * Displays live for the process (EGL has no way to free one); terminate
 * releases the Vulkan objects and initialize makes them again.
 */
struct zegl_display {
	/* The platform and its native display (the wl_display for Wayland). */
	int platform;
	void *native;

	/* Nonzero between eglInitialize and eglTerminate. */
	int initialized;

	/* The Vulkan instance, the device and its one graphics queue. */
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;

	/* The configs offered. */
	struct zegl_config configs[ZEGL_CONFIGS];
	unsigned config_count;

	/* The next display in the process's list. */
	struct zegl_display *next;
};

/*
 * One EGL surface: for a window, the Vulkan surface, its swapchain and a
 * framebuffer per image, and the frame being recorded.
 */
struct zegl_surface {
	/* The display and config it was made with, and EGL_WINDOW_BIT or EGL_PBUFFER_BIT. */
	struct zegl_display *display;
	struct zegl_config *config;
	EGLint kind;

	/* The Wayland EGL window (NULL for display direct). */
	struct wl_egl_window *window;
	unsigned window_generation;

	/* The Vulkan surface, the swapchain, its format and size, and whether it must be made again. */
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkFormat format;
	VkExtent2D extent;
	int stale;

	/*
	 * The render pass that starts a frame by clearing it, and the one that
	 * goes on with what an earlier pass of the frame left (both leave the
	 * colour image ready to present); pipelines are made with the first,
	 * which the second is compatible with.
	 */
	VkRenderPass pass;
	VkRenderPass pass_load;

	/* The depth and stencil buffer (VK_FORMAT_UNDEFINED when the config has none), and the aspects its format has. */
	VkFormat depth_format;
	VkImageAspectFlags depth_aspects;
	VkImage depth_image;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;

	/* Nonzero when the swapchain's images can be copied from (glReadPixels). */
	int readable;

	/*
	 * The layout the colour image rests in outside a pass: ready to
	 * present for a window, a colour attachment for a pbuffer.
	 */
	VkImageLayout rest_layout;

	/* A pbuffer's colour image and its memory, and whether its images were given their resting layouts yet. */
	VkImage pbuffer_image;
	VkDeviceMemory pbuffer_memory;
	int pbuffer_ready;

	/* Each image's view and framebuffer. */
	VkImage images[ZEGL_IMAGES];
	VkImageView views[ZEGL_IMAGES];
	VkFramebuffer framebuffers[ZEGL_IMAGES];
	uint32_t image_count;

	/* The frame's command buffer, the fence of its submission, and the acquire and render semaphores. */
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkSemaphore acquired;
	VkSemaphore rendered;

	/*
	 * The frame being recorded: whether an image is acquired and the
	 * command buffer recording, whether a render pass is open, how many
	 * passes the frame has had, whether a submission already waited for
	 * the acquire, and the image.
	 */
	int frame_open;
	int in_pass;
	unsigned passes;
	int acquire_waited;
	uint32_t image;

	/* Nonzero when commands were recorded since the frame's last submission. */
	int recorded;

	/* The swap interval (eglSwapInterval), and how many frames were presented. */
	EGLint interval;
	uint64_t frames;
};

/*
 * The GLES state of a context that libEGL also sees: the clear colour,
 * the viewport and the error.  The rest of the state is libGLESv2's own
 * (state), which libGLESv2 makes at the context's first GLES call and
 * which libEGL gives back through the two callbacks.
 */
struct zegl_context;

struct zegl_gles {
	/* The colour glClear clears to, and whether a clear was asked for since the last frame. */
	float clear_color[4];
	int clear_pending;

	/* The viewport (x, y, width, height), and whether the application set it. */
	int viewport[4];
	int viewport_set;

	/* The first error since glGetError last read it (GL_NO_ERROR when none). */
	unsigned error;

	/* libGLESv2's state, NULL until its first call. */
	void *state;

	/* Called by eglSwapBuffers once a frame is done, and by eglDestroyContext; NULL until libGLESv2 sets them. */
	void (*frame_done)(struct zegl_context *context);
	void (*release)(struct zegl_context *context);
};

/*
 * One EGL context: the client API's version and state, and what it is
 * current with.
 */
struct zegl_context {
	/* The display and config it was made with, and the GLES version (2 or 3). */
	struct zegl_display *display;
	struct zegl_config *config;
	EGLint version;

	/* The surfaces it draws to and reads from while current (EGL_NO_SURFACE when none). */
	struct zegl_surface *draw;
	struct zegl_surface *read;

	/* The GLES state. */
	struct zegl_gles gles;
};

/* The calling thread's current context, or NULL (libEGL; libGLESv2 calls it). */
struct zegl_context *zegl_current_context(void);

/* The Vulkan side (vulkan.c). */
EGLint zegl_vulkan_open(struct zegl_display *display);
void zegl_vulkan_close(struct zegl_display *display);
EGLint zegl_surface_open(struct zegl_surface *surface);
EGLint zegl_pbuffer_open(struct zegl_surface *surface, uint32_t width, uint32_t height);
void zegl_surface_close(struct zegl_surface *surface);
EGLint zegl_surface_present(struct zegl_surface *surface, struct zegl_context *context);

/* The frame, for libGLESv2 (vulkan.c): opened, a pass entered or left, and the recording submitted and waited for. */
EGLint zegl_frame_begin(struct zegl_surface *surface);
void zegl_frame_pass(struct zegl_surface *surface, const VkClearValue *clear);
void zegl_frame_leave_pass(struct zegl_surface *surface);
EGLint zegl_frame_flush(struct zegl_surface *surface);

#endif
