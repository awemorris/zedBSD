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
 * a Vulkan surface, its swapchain and one framebuffer per image; a context
 * holds the GLES state.  libGLESv2 finds the calling thread's context with
 * zegl_current_context() and draws into its surface's frame; libEGL
 * submits and presents the frame in eglSwapBuffers.
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

	/* The render pass that starts a frame by clearing it, and each image's view and framebuffer. */
	VkRenderPass pass;
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

	/* The swap interval (eglSwapInterval), and how many frames were presented. */
	EGLint interval;
	uint64_t frames;
};

/*
 * The GLES state of a context that libGLESv2 keeps (WS068 p002: the clear
 * colour, the viewport and the error).
 */
struct zegl_gles {
	/* The colour glClear clears to, and whether a clear was asked for since the last frame. */
	float clear_color[4];
	int clear_pending;

	/* The viewport (x, y, width, height), and whether the application set it. */
	int viewport[4];
	int viewport_set;

	/* The first error since glGetError last read it (GL_NO_ERROR when none). */
	unsigned error;
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
void zegl_surface_close(struct zegl_surface *surface);
EGLint zegl_surface_present(struct zegl_surface *surface, struct zegl_context *context);

#endif
