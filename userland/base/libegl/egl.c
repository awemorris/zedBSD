/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's EGL 1.5 (WS068 p002, plan/ws068/design.md): displays, configs,
 * window surfaces, OpenGL ES contexts and the calling thread's current
 * context, over Vulkan (vulkan.c).
 *
 * A display is on Wayland (eglGetPlatformDisplay with
 * EGL_PLATFORM_WAYLAND_KHR, or eglGetDisplay with a wl_display), or drives
 * the screen directly (EGL_DEFAULT_DISPLAY: the first display's native mode
 * through VK_KHR_display, for a program run without a compositor), or is
 * surfaceless.  Pbuffers, pixmaps, EGLImage and fence syncs are not there
 * yet.
 */

#include "zegl.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* The client extensions, which eglQueryString reports for EGL_NO_DISPLAY. */
#define EGL_CLIENT_EXTENSIONS_STRING \
	"EGL_EXT_client_extensions EGL_EXT_platform_base EGL_KHR_platform_wayland " \
	"EGL_EXT_platform_wayland EGL_MESA_platform_surfaceless"

/* The display extensions. */
#define EGL_DISPLAY_EXTENSIONS_STRING \
	"EGL_KHR_create_context EGL_KHR_surfaceless_context EGL_KHR_no_config_context"

/* The number of the surfaceless platform (EGL_MESA_platform_surfaceless). */
#define ZEGL_PLATFORM_SURFACELESS_MESA	0x31DD

/*
 * One EGL entry point eglGetProcAddress can hand out.
 */
struct egl_proc {
	const char *name;
	void (*address)(void);
};

/*
 * Every display made so far.  EGL has no call that frees a display, so the
 * list only grows; it is not locked (applications make their displays on
 * one thread in practice).
 */
static struct zegl_display *egl_displays;

/*
 * One thread's EGL state: its last error, its current context and display
 * (NULL when none) and its bound client API.  Made on the thread's first
 * EGL call and freed when the thread ends.
 */
struct egl_thread {
	EGLint error;
	struct zegl_context *current;
	struct zegl_display *display;
	EGLenum api;
};

/*
 * The key of each thread's struct egl_thread, made once (egl_key_once), and
 * the state used when a thread's own cannot be allocated.
 */
static pthread_key_t egl_key;
static pthread_once_t egl_key_once = PTHREAD_ONCE_INIT;
static struct egl_thread egl_fallback = { EGL_SUCCESS, NULL, NULL, EGL_OPENGL_ES_API };

/*
 * The program's global scope (dlopen of NULL), where eglGetProcAddress
 * finds the functions of libGLESv2 and the rest; opened on first use and
 * kept for the process.
 */
static void *egl_program;

static struct egl_thread *egl_thread(void);
static void egl_key_make(void);
static void egl_thread_free(void *state);
static EGLBoolean egl_fail(EGLint error);
static EGLBoolean egl_succeed(void);
static struct zegl_display *egl_display_get(int platform, void *native);
static struct zegl_display *egl_display_valid(EGLDisplay dpy);
static void egl_configs(struct zegl_display *display);
static int egl_config_matches(const struct zegl_config *config, const EGLint *attributes);
static EGLint egl_config_value(const struct zegl_config *config, EGLint attribute, EGLint *value);
static struct zegl_config *egl_config_valid(struct zegl_display *display, EGLConfig config);
static EGLSurface egl_window_surface(struct zegl_display *display, struct zegl_config *config, void *native);

/*
 * Returns the calling thread's current context, or NULL (libGLESv2's way
 * to its state).
 */
struct zegl_context *
zegl_current_context(void)
{
	/* The thread's own. */
	return egl_thread()->current;
}

/*
 * Returns the calling thread's last error and clears it.
 */
EGLint EGLAPIENTRY
eglGetError(void)
{
	EGLint error;

	/* Reading the error resets it. */
	error = egl_thread()->error;
	egl_thread()->error = EGL_SUCCESS;
	return error;
}

/*
 * Returns the display of a native display: a wl_display is a Wayland
 * display, EGL_DEFAULT_DISPLAY drives the screen directly.
 */
EGLDisplay EGLAPIENTRY
eglGetDisplay(
	EGLNativeDisplayType display_id)
{
	struct zegl_display *display;

	/* The default display is the screen itself; anything else is taken to be a wl_display. */
	if (display_id == EGL_DEFAULT_DISPLAY) {
		display = egl_display_get(ZEGL_PLATFORM_DISPLAY, NULL);
	} else {
		display = egl_display_get(ZEGL_PLATFORM_WAYLAND, (void *)display_id);
	}

	/* A display that could not be made. */
	if (display == NULL) {
		(void)egl_fail(EGL_BAD_ALLOC);
		return EGL_NO_DISPLAY;
	}

	/* Succeeded: the display. */
	(void)egl_succeed();
	return (EGLDisplay)display;
}

/*
 * Returns the display of a native display on a named platform: Wayland or
 * surfaceless.
 */
EGLDisplay EGLAPIENTRY
eglGetPlatformDisplay(
	EGLenum platform,
	void *native_display,
	const EGLAttrib *attrib_list)
{
	struct zegl_display *display;

	/* No attributes are taken. */
	(void)attrib_list;

	/* The platform named. */
	if (platform == EGL_PLATFORM_WAYLAND_KHR) {
		display = egl_display_get(ZEGL_PLATFORM_WAYLAND, native_display);
	} else if (platform == ZEGL_PLATFORM_SURFACELESS_MESA) {
		display = egl_display_get(ZEGL_PLATFORM_SURFACELESS, NULL);
	} else {
		(void)egl_fail(EGL_BAD_PARAMETER);
		return EGL_NO_DISPLAY;
	}

	/* A display that could not be made. */
	if (display == NULL) {
		(void)egl_fail(EGL_BAD_ALLOC);
		return EGL_NO_DISPLAY;
	}

	/* Succeeded: the display. */
	(void)egl_succeed();
	return (EGLDisplay)display;
}

/*
 * The EXT spelling of eglGetPlatformDisplay (EGL_EXT_platform_base).
 */
EGLDisplay EGLAPIENTRY
eglGetPlatformDisplayEXT(
	EGLenum platform,
	void *native_display,
	const EGLint *attrib_list)
{
	EGLDisplay display;

	/* No attributes are taken, so the two attribute types need not be converted. */
	(void)attrib_list;
	display = eglGetPlatformDisplay(platform, native_display, NULL);
	return display;
}

/*
 * Makes a display ready: its Vulkan device and its configs.
 */
EGLBoolean EGLAPIENTRY
eglInitialize(
	EGLDisplay dpy,
	EGLint *major,
	EGLint *minor)
{
	struct zegl_display *display;
	EGLint error;

	/* A display this library made. */
	display = egl_display_valid(dpy);
	if (display == NULL)
		return egl_fail(EGL_BAD_DISPLAY);

	/* The device, once. */
	if (!display->initialized) {
		error = zegl_vulkan_open(display);
		if (error != EGL_SUCCESS)
			return egl_fail(error);
		egl_configs(display);
		display->initialized = 1;
	}

	/* Succeeded: EGL 1.5. */
	if (major != NULL)
		*major = 1;
	if (minor != NULL)
		*minor = 5;
	return egl_succeed();
}

/*
 * Releases a display's device (its surfaces and contexts must be gone).
 */
EGLBoolean EGLAPIENTRY
eglTerminate(
	EGLDisplay dpy)
{
	struct zegl_display *display;

	/* A display this library made. */
	display = egl_display_valid(dpy);
	if (display == NULL)
		return egl_fail(EGL_BAD_DISPLAY);

	/* The device goes; initialize makes it again. */
	if (display->initialized) {
		zegl_vulkan_close(display);
		display->initialized = 0;
	}

	/* Succeeded. */
	return egl_succeed();
}

/*
 * Returns one of EGL's strings: the vendor, the version, the client APIs
 * and the extensions (the client extensions for EGL_NO_DISPLAY).
 */
const char * EGLAPIENTRY
eglQueryString(
	EGLDisplay dpy,
	EGLint name)
{
	struct zegl_display *display;

	/* Without a display only the client extensions are asked for. */
	if (dpy == EGL_NO_DISPLAY) {
		if (name != EGL_EXTENSIONS) {
			(void)egl_fail(EGL_BAD_DISPLAY);
			return NULL;
		}

		/* Reports the client extensions. */
		(void)egl_succeed();
		return EGL_CLIENT_EXTENSIONS_STRING;
	}

	/* An initialized display. */
	display = egl_display_valid(dpy);
	if (display == NULL || !display->initialized) {
		(void)egl_fail(EGL_NOT_INITIALIZED);
		return NULL;
	}

	/* The string asked for. */
	(void)egl_succeed();
	switch (name) {
	case EGL_VENDOR:
		return "zedBSD";
	case EGL_VERSION:
		return "1.5 zedBSD";
	case EGL_CLIENT_APIS:
		return "OpenGL_ES";
	case EGL_EXTENSIONS:
		return EGL_DISPLAY_EXTENSIONS_STRING;
	default:
		break;
	}

	/* Any other name is not an EGL string. */
	(void)egl_fail(EGL_BAD_PARAMETER);
	return NULL;
}

/*
 * Lists a display's configs.
 */
EGLBoolean EGLAPIENTRY
eglGetConfigs(
	EGLDisplay dpy,
	EGLConfig *configs,
	EGLint config_size,
	EGLint *num_config)
{
	struct zegl_display *display;
	EGLint index;

	/* An initialized display and a place for the count. */
	display = egl_display_valid(dpy);
	if (display == NULL || !display->initialized)
		return egl_fail(EGL_NOT_INITIALIZED);
	if (num_config == NULL)
		return egl_fail(EGL_BAD_PARAMETER);

	/* Only the count, when no array is given. */
	if (configs == NULL) {
		*num_config = (EGLint)display->config_count;
		return egl_succeed();
	}

	/* As many configs as fit. */
	for (index = 0; index < config_size && index < (EGLint)display->config_count; index++)
		configs[index] = (EGLConfig)&display->configs[index];

	/* Succeeded: the configs given. */
	*num_config = index;
	return egl_succeed();
}

/*
 * Lists the configs that meet a list of attributes, in the display's order.
 */
EGLBoolean EGLAPIENTRY
eglChooseConfig(
	EGLDisplay dpy,
	const EGLint *attrib_list,
	EGLConfig *configs,
	EGLint config_size,
	EGLint *num_config)
{
	struct zegl_display *display;
	unsigned index;
	EGLint count;
	int matches;

	/* An initialized display and a place for the count. */
	display = egl_display_valid(dpy);
	if (display == NULL || !display->initialized)
		return egl_fail(EGL_NOT_INITIALIZED);
	if (num_config == NULL)
		return egl_fail(EGL_BAD_PARAMETER);

	/* Each config that meets every attribute. */
	count = 0;
	for (index = 0U; index < display->config_count; index++) {
		matches = egl_config_matches(&display->configs[index], attrib_list);
		if (!matches)
			continue;

		/* The config goes in the array while there is room; it counts either way without an array. */
		if (configs != NULL) {
			if (count >= config_size)
				break;
			configs[count] = (EGLConfig)&display->configs[index];
		}

		/* One more found. */
		count++;
	}

	/* Succeeded: how many were found. */
	*num_config = count;
	return egl_succeed();
}

/*
 * Reports one attribute of a config.
 */
EGLBoolean EGLAPIENTRY
eglGetConfigAttrib(
	EGLDisplay dpy,
	EGLConfig config,
	EGLint attribute,
	EGLint *value)
{
	struct zegl_display *display;
	struct zegl_config *chosen;
	EGLint error;

	/* An initialized display and one of its configs. */
	display = egl_display_valid(dpy);
	if (display == NULL || !display->initialized)
		return egl_fail(EGL_NOT_INITIALIZED);
	chosen = egl_config_valid(display, config);
	if (chosen == NULL)
		return egl_fail(EGL_BAD_CONFIG);
	if (value == NULL)
		return egl_fail(EGL_BAD_PARAMETER);

	/* The attribute's value. */
	error = egl_config_value(chosen, attribute, value);
	if (error != EGL_SUCCESS)
		return egl_fail(error);

	/* Succeeded: the value. */
	return egl_succeed();
}

/*
 * Makes a window surface over a native window: a wl_egl_window on Wayland,
 * the whole screen (any window value) when driving it directly.
 */
EGLSurface EGLAPIENTRY
eglCreateWindowSurface(
	EGLDisplay dpy,
	EGLConfig config,
	EGLNativeWindowType win,
	const EGLint *attrib_list)
{
	struct zegl_display *display;
	struct zegl_config *chosen;
	EGLSurface surface;

	/* No attributes are taken (the render buffer is always the back one). */
	(void)attrib_list;

	/* An initialized display and one of its configs. */
	display = egl_display_valid(dpy);
	if (display == NULL || !display->initialized) {
		(void)egl_fail(EGL_NOT_INITIALIZED);
		return EGL_NO_SURFACE;
	}

	/* The config must be one of the display's. */
	chosen = egl_config_valid(display, config);
	if (chosen == NULL) {
		(void)egl_fail(EGL_BAD_CONFIG);
		return EGL_NO_SURFACE;
	}

	/* The surface. */
	surface = egl_window_surface(display, chosen, (void *)win);
	return surface;
}

/*
 * The EGL 1.5 spelling of eglCreateWindowSurface, whose native window is a
 * pointer (a wl_egl_window * on Wayland).
 */
EGLSurface EGLAPIENTRY
eglCreatePlatformWindowSurface(
	EGLDisplay dpy,
	EGLConfig config,
	void *native_window,
	const EGLAttrib *attrib_list)
{
	struct zegl_display *display;
	struct zegl_config *chosen;
	EGLSurface surface;

	/* No attributes are taken. */
	(void)attrib_list;

	/* An initialized display and one of its configs. */
	display = egl_display_valid(dpy);
	if (display == NULL || !display->initialized) {
		(void)egl_fail(EGL_NOT_INITIALIZED);
		return EGL_NO_SURFACE;
	}

	/* The config must be one of the display's. */
	chosen = egl_config_valid(display, config);
	if (chosen == NULL) {
		(void)egl_fail(EGL_BAD_CONFIG);
		return EGL_NO_SURFACE;
	}

	/* The surface. */
	surface = egl_window_surface(display, chosen, native_window);
	return surface;
}

/*
 * The EXT spelling of eglCreatePlatformWindowSurface.
 */
EGLSurface EGLAPIENTRY
eglCreatePlatformWindowSurfaceEXT(
	EGLDisplay dpy,
	EGLConfig config,
	void *native_window,
	const EGLint *attrib_list)
{
	EGLSurface surface;

	/* No attributes are taken, so the two attribute types need not be converted. */
	(void)attrib_list;
	surface = eglCreatePlatformWindowSurface(dpy, config, native_window, NULL);
	return surface;
}

/*
 * Makes a pbuffer: an offscreen colour image (and depth buffer) of
 * EGL_WIDTH x EGL_HEIGHT (at least 1 x 1).
 */
EGLSurface EGLAPIENTRY
eglCreatePbufferSurface(
	EGLDisplay dpy,
	EGLConfig config,
	const EGLint *attrib_list)
{
	struct zegl_display *display;
	struct zegl_config *chosen;
	struct zegl_surface *surface;
	EGLint width;
	EGLint height;
	EGLint error;
	unsigned index;

	/* An initialized display and one of its configs that makes pbuffers. */
	display = egl_display_valid(dpy);
	if (display == NULL || !display->initialized) {
		(void)egl_fail(EGL_NOT_INITIALIZED);
		return EGL_NO_SURFACE;
	}

	/* One of its configs. */
	chosen = egl_config_valid(display, config);
	if (chosen == NULL) {
		(void)egl_fail(EGL_BAD_CONFIG);
		return EGL_NO_SURFACE;
	}

	/* A config that makes pbuffers. */
	if ((chosen->surface_type & EGL_PBUFFER_BIT) == 0) {
		(void)egl_fail(EGL_BAD_MATCH);
		return EGL_NO_SURFACE;
	}

	/* The size asked for. */
	width = 0;
	height = 0;
	for (index = 0U; attrib_list != NULL && attrib_list[index] != EGL_NONE; index += 2U) {
		if (attrib_list[index] == EGL_WIDTH)
			width = attrib_list[index + 1U];
		if (attrib_list[index] == EGL_HEIGHT)
			height = attrib_list[index + 1U];
	}

	/* A negative or too large size is refused. */
	if (width < 0 || height < 0 || width > 16384 || height > 16384) {
		(void)egl_fail(EGL_BAD_PARAMETER);
		return EGL_NO_SURFACE;
	}

	/* 0 is 1. */
	if (width == 0)
		width = 1;
	if (height == 0)
		height = 1;

	/* The surface. */
	surface = calloc(1U, sizeof(*surface));
	if (surface == NULL) {
		(void)egl_fail(EGL_BAD_ALLOC);
		return EGL_NO_SURFACE;
	}

	/* What it was made with. */
	surface->display = display;
	surface->config = chosen;
	surface->kind = EGL_PBUFFER_BIT;
	surface->interval = 1;

	/* Its image and frame. */
	error = zegl_pbuffer_open(surface, (uint32_t)width, (uint32_t)height);
	if (error != EGL_SUCCESS) {
		zegl_surface_close(surface);
		free(surface);
		(void)egl_fail(error);
		return EGL_NO_SURFACE;
	}

	/* Succeeded: the pbuffer. */
	(void)egl_succeed();
	return (EGLSurface)surface;
}

/*
 * Destroys a surface.
 */
EGLBoolean EGLAPIENTRY
eglDestroySurface(
	EGLDisplay dpy,
	EGLSurface surface)
{
	struct zegl_display *display;
	struct zegl_surface *destroyed;
	struct egl_thread *thread;

	/* The calling thread's EGL state. */
	thread = egl_thread();

	/* A display and a surface. */
	display = egl_display_valid(dpy);
	if (display == NULL)
		return egl_fail(EGL_BAD_DISPLAY);
	if (surface == EGL_NO_SURFACE)
		return egl_fail(EGL_BAD_SURFACE);

	/* A surface current somewhere is not destroyed under its context (EGL defers it; here it is refused). */
	destroyed = (struct zegl_surface *)surface;
	if (thread->current != NULL && (thread->current->draw == destroyed || thread->current->read == destroyed))
		return egl_fail(EGL_BAD_ACCESS);

	/* Succeeded: its Vulkan objects and the surface go. */
	zegl_surface_close(destroyed);
	free(destroyed);
	return egl_succeed();
}

/*
 * Reports one attribute of a surface.
 */
EGLBoolean EGLAPIENTRY
eglQuerySurface(
	EGLDisplay dpy,
	EGLSurface surface,
	EGLint attribute,
	EGLint *value)
{
	struct zegl_surface *queried;
	struct zegl_display *display;

	/* A display, a surface and a place for the value. */
	display = egl_display_valid(dpy);
	if (display == NULL)
		return egl_fail(EGL_BAD_DISPLAY);
	if (surface == EGL_NO_SURFACE)
		return egl_fail(EGL_BAD_SURFACE);
	if (value == NULL)
		return egl_fail(EGL_BAD_PARAMETER);

	/* The attribute. */
	queried = (struct zegl_surface *)surface;
	switch (attribute) {
	case EGL_WIDTH:
		*value = (EGLint)queried->extent.width;
		break;
	case EGL_HEIGHT:
		*value = (EGLint)queried->extent.height;
		break;
	case EGL_CONFIG_ID:
		*value = queried->config->id;
		break;
	case EGL_RENDER_BUFFER:
		*value = EGL_BACK_BUFFER;
		break;
	case EGL_SWAP_BEHAVIOR:
		*value = EGL_BUFFER_DESTROYED;
		if (queried->kind == EGL_PBUFFER_BIT)
			*value = EGL_BUFFER_PRESERVED;
		break;
	case EGL_MULTISAMPLE_RESOLVE:
		*value = EGL_MULTISAMPLE_RESOLVE_DEFAULT;
		break;
	default:
		return egl_fail(EGL_BAD_ATTRIBUTE);
	}

	/* Succeeded: the value. */
	return egl_succeed();
}

/*
 * Sets a surface attribute: only the swap behaviour, and only to what it is.
 */
EGLBoolean EGLAPIENTRY
eglSurfaceAttrib(
	EGLDisplay dpy,
	EGLSurface surface,
	EGLint attribute,
	EGLint value)
{
	struct zegl_display *display;

	/* A display and a surface. */
	display = egl_display_valid(dpy);
	if (display == NULL)
		return egl_fail(EGL_BAD_DISPLAY);
	if (surface == EGL_NO_SURFACE)
		return egl_fail(EGL_BAD_SURFACE);

	/* The contents after a swap are never kept. */
	if (attribute == EGL_SWAP_BEHAVIOR && value == EGL_BUFFER_DESTROYED)
		return egl_succeed();

	/* Anything else is not supported. */
	return egl_fail(EGL_BAD_MATCH);
}

/*
 * Binds a client API to the calling thread: only OpenGL ES.
 */
EGLBoolean EGLAPIENTRY
eglBindAPI(
	EGLenum api)
{
	struct egl_thread *thread;

	/* The calling thread's EGL state. */
	thread = egl_thread();

	/* OpenGL ES is the only one. */
	if (api != EGL_OPENGL_ES_API)
		return egl_fail(EGL_BAD_PARAMETER);

	/* Succeeded. */
	thread->api = api;
	return egl_succeed();
}

/*
 * Reports the calling thread's client API.
 */
EGLenum EGLAPIENTRY
eglQueryAPI(void)
{
	/* The one bound. */
	return egl_thread()->api;
}

/*
 * Makes an OpenGL ES 2 or 3 context.
 */
EGLContext EGLAPIENTRY
eglCreateContext(
	EGLDisplay dpy,
	EGLConfig config,
	EGLContext share_context,
	const EGLint *attrib_list)
{
	struct zegl_display *display;
	struct zegl_context *context;
	EGLint version;
	unsigned index;

	/* Sharing object names between contexts is not there yet. */
	(void)share_context;

	/* An initialized display. */
	display = egl_display_valid(dpy);
	if (display == NULL || !display->initialized) {
		(void)egl_fail(EGL_NOT_INITIALIZED);
		return EGL_NO_CONTEXT;
	}

	/* The version asked for (2 by default); 2 and 3 are made. */
	version = 1;
	for (index = 0U; attrib_list != NULL && attrib_list[index] != EGL_NONE; index += 2U) {
		if (attrib_list[index] == EGL_CONTEXT_CLIENT_VERSION)
			version = attrib_list[index + 1U];
	}

	/* No version given is 2. */
	if (version == 1)
		version = 2;
	if (version != 2 && version != 3) {
		(void)egl_fail(EGL_BAD_MATCH);
		return EGL_NO_CONTEXT;
	}

	/* The context with GLES's initial state (black, no error). */
	context = calloc(1U, sizeof(*context));
	if (context == NULL) {
		(void)egl_fail(EGL_BAD_ALLOC);
		return EGL_NO_CONTEXT;
	}

	/* Succeeded: the context. */
	context->display = display;
	context->config = egl_config_valid(display, config);
	context->version = version;
	(void)egl_succeed();
	return (EGLContext)context;
}

/*
 * Destroys a context that is not current.
 */
EGLBoolean EGLAPIENTRY
eglDestroyContext(
	EGLDisplay dpy,
	EGLContext ctx)
{
	struct egl_thread *thread;
	struct zegl_display *display;
	struct zegl_context *context;

	/* The calling thread's EGL state. */
	thread = egl_thread();

	/* A display and a context. */
	display = egl_display_valid(dpy);
	if (display == NULL)
		return egl_fail(EGL_BAD_DISPLAY);
	if (ctx == EGL_NO_CONTEXT)
		return egl_fail(EGL_BAD_CONTEXT);

	/* A current context is refused (EGL defers its destruction). */
	if ((struct zegl_context *)ctx == thread->current)
		return egl_fail(EGL_BAD_ACCESS);

	/* libGLESv2's state goes first, then the context. */
	context = (struct zegl_context *)ctx;
	if (context->gles.release != NULL)
		context->gles.release(context);
	free(context);
	return egl_succeed();
}

/*
 * Makes a context current on the calling thread with its draw and read
 * surfaces, or releases the current one.
 */
EGLBoolean EGLAPIENTRY
eglMakeCurrent(
	EGLDisplay dpy,
	EGLSurface draw,
	EGLSurface read,
	EGLContext ctx)
{
	struct zegl_display *display;
	struct zegl_context *context;
	struct zegl_surface *surface;
	struct egl_thread *thread;

	/* The calling thread's EGL state. */
	thread = egl_thread();

	/* Releasing: no context, no surfaces. */
	if (ctx == EGL_NO_CONTEXT) {
		if (thread->current != NULL) {
			thread->current->draw = NULL;
			thread->current->read = NULL;
		}

		/* The thread has no context and no display. */
		thread->current = NULL;
		thread->display = NULL;
		return egl_succeed();
	}

	/* An initialized display. */
	display = egl_display_valid(dpy);
	if (display == NULL || !display->initialized)
		return egl_fail(EGL_NOT_INITIALIZED);

	/* The context with its surfaces (both, or neither for a surfaceless context). */
	context = (struct zegl_context *)ctx;
	if ((draw == EGL_NO_SURFACE) != (read == EGL_NO_SURFACE))
		return egl_fail(EGL_BAD_MATCH);
	context->draw = (struct zegl_surface *)draw;
	context->read = (struct zegl_surface *)read;

	/* A context used for the first time gets its draw surface's size as its viewport. */
	surface = context->draw;
	if (surface != NULL && !context->gles.viewport_set) {
		context->gles.viewport[0] = 0;
		context->gles.viewport[1] = 0;
		context->gles.viewport[2] = (int)surface->extent.width;
		context->gles.viewport[3] = (int)surface->extent.height;
		context->gles.viewport_set = 1;
	}

	/* Succeeded: the thread's current context. */
	thread->current = context;
	thread->display = display;
	return egl_succeed();
}

/*
 * Returns the calling thread's current context.
 */
EGLContext EGLAPIENTRY
eglGetCurrentContext(void)
{
	struct egl_thread *thread;

	/* EGL_NO_CONTEXT when none. */
	thread = egl_thread();
	if (thread->current == NULL)
		return EGL_NO_CONTEXT;

	/* Reports the context. */
	return (EGLContext)thread->current;
}

/*
 * Returns the calling thread's current draw or read surface.
 */
EGLSurface EGLAPIENTRY
eglGetCurrentSurface(
	EGLint readdraw)
{
	struct egl_thread *thread;

	/* The calling thread's EGL state. */
	thread = egl_thread();

	/* Without a context there is no surface. */
	if (thread->current == NULL)
		return EGL_NO_SURFACE;

	/* The one asked for. */
	if (readdraw == EGL_READ)
		return (EGLSurface)thread->current->read;
	if (readdraw == EGL_DRAW)
		return (EGLSurface)thread->current->draw;

	/* Anything else is a bad parameter. */
	(void)egl_fail(EGL_BAD_PARAMETER);
	return EGL_NO_SURFACE;
}

/*
 * Returns the calling thread's current display.
 */
EGLDisplay EGLAPIENTRY
eglGetCurrentDisplay(void)
{
	struct egl_thread *thread;

	/* EGL_NO_DISPLAY when none. */
	thread = egl_thread();
	if (thread->display == NULL)
		return EGL_NO_DISPLAY;

	/* Reports the display. */
	return (EGLDisplay)thread->display;
}

/*
 * Reports one attribute of a context.
 */
EGLBoolean EGLAPIENTRY
eglQueryContext(
	EGLDisplay dpy,
	EGLContext ctx,
	EGLint attribute,
	EGLint *value)
{
	struct zegl_context *context;
	struct zegl_display *display;

	/* A display, a context and a place for the value. */
	display = egl_display_valid(dpy);
	if (display == NULL)
		return egl_fail(EGL_BAD_DISPLAY);
	if (ctx == EGL_NO_CONTEXT)
		return egl_fail(EGL_BAD_CONTEXT);
	if (value == NULL)
		return egl_fail(EGL_BAD_PARAMETER);

	/* The attribute. */
	context = (struct zegl_context *)ctx;
	switch (attribute) {
	case EGL_CONFIG_ID:
		*value = 0;
		if (context->config != NULL)
			*value = context->config->id;
		break;
	case EGL_CONTEXT_CLIENT_TYPE:
		*value = EGL_OPENGL_ES_API;
		break;
	case EGL_CONTEXT_CLIENT_VERSION:
		*value = context->version;
		break;
	case EGL_RENDER_BUFFER:
		*value = EGL_BACK_BUFFER;
		break;
	default:
		return egl_fail(EGL_BAD_ATTRIBUTE);
	}

	/* Succeeded: the value. */
	return egl_succeed();
}

/*
 * Presents what the current context drew to a window surface.
 */
EGLBoolean EGLAPIENTRY
eglSwapBuffers(
	EGLDisplay dpy,
	EGLSurface surface)
{
	struct zegl_surface *swapped;
	EGLint error;
	struct egl_thread *thread;
	struct zegl_display *display;

	/* The calling thread's EGL state. */
	thread = egl_thread();

	/* A display, a surface, and a context current with it on this thread. */
	display = egl_display_valid(dpy);
	if (display == NULL)
		return egl_fail(EGL_BAD_DISPLAY);
	if (surface == EGL_NO_SURFACE)
		return egl_fail(EGL_BAD_SURFACE);
	swapped = (struct zegl_surface *)surface;
	if (thread->current == NULL || thread->current->draw != swapped)
		return egl_fail(EGL_BAD_SURFACE);

	/* The frame goes to the window. */
	error = zegl_surface_present(swapped, thread->current);
	if (error != EGL_SUCCESS)
		return egl_fail(error);

	/* Succeeded: the frame is shown. */
	return egl_succeed();
}

/*
 * Sets how many vertical blanks a swap waits for: 0 (no waiting, MAILBOX
 * when there is one) or 1 (FIFO); more are taken as 1.
 */
EGLBoolean EGLAPIENTRY
eglSwapInterval(
	EGLDisplay dpy,
	EGLint interval)
{
	struct zegl_surface *surface;
	struct egl_thread *thread;
	struct zegl_display *display;

	/* The calling thread's EGL state. */
	thread = egl_thread();

	/* A display and a current context with a surface. */
	display = egl_display_valid(dpy);
	if (display == NULL)
		return egl_fail(EGL_BAD_DISPLAY);
	if (thread->current == NULL)
		return egl_fail(EGL_BAD_CONTEXT);
	surface = thread->current->draw;
	if (surface == NULL)
		return egl_fail(EGL_BAD_SURFACE);

	/* The interval, 0 or 1; a change makes the swapchain again. */
	if (interval > 1)
		interval = 1;
	if (interval < 0)
		interval = 0;
	if (interval != surface->interval) {
		surface->interval = interval;
		surface->stale = 1;
	}

	/* Succeeded. */
	return egl_succeed();
}

/*
 * Waits for the client API's rendering: every frame is waited for already.
 */
EGLBoolean EGLAPIENTRY
eglWaitClient(void)
{
	/* Nothing is left running. */
	return egl_succeed();
}

/*
 * The EGL 1.0 spelling of eglWaitClient.
 */
EGLBoolean EGLAPIENTRY
eglWaitGL(void)
{
	/* Nothing is left running. */
	return egl_succeed();
}

/*
 * Waits for native rendering: there is none to wait for.
 */
EGLBoolean EGLAPIENTRY
eglWaitNative(
	EGLint engine)
{
	/* Nothing to wait for. */
	(void)engine;
	return egl_succeed();
}

/*
 * Releases the calling thread's current context.
 */
EGLBoolean EGLAPIENTRY
eglReleaseThread(void)
{
	EGLBoolean released;

	/* As eglMakeCurrent with no context. */
	released = eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	return released;
}

/*
 * Returns the address of an EGL or OpenGL ES function by its name, or NULL.
 */
__eglMustCastToProperFunctionPointerType EGLAPIENTRY
eglGetProcAddress(
	const char *procname)
{
	static const struct egl_proc procs[] = {
		{ "eglGetPlatformDisplay", (void (*)(void))eglGetPlatformDisplay },
		{ "eglGetPlatformDisplayEXT", (void (*)(void))eglGetPlatformDisplayEXT },
		{ "eglCreatePlatformWindowSurface", (void (*)(void))eglCreatePlatformWindowSurface },
		{ "eglCreatePlatformWindowSurfaceEXT", (void (*)(void))eglCreatePlatformWindowSurfaceEXT },
		{ "eglSwapInterval", (void (*)(void))eglSwapInterval }
	};
	void *address;
	size_t index;
	int differs;

	/* A name is needed. */
	if (procname == NULL)
		return NULL;

	/* The EGL extension functions this library has. */
	for (index = 0U; index < sizeof(procs) / sizeof(procs[0]); index++) {
		differs = strcmp(procname, procs[index].name);
		if (differs == 0)
			return procs[index].address;
	}

	/* Any other function (GLES's among them) in the program's global scope. */
	if (egl_program == NULL)
		egl_program = dlopen(NULL, RTLD_NOW);
	if (egl_program == NULL)
		return NULL;
	address = dlsym(egl_program, procname);
	return (__eglMustCastToProperFunctionPointerType)address;
}

/* Returns the calling thread's EGL state, making it on its first call. */
static struct egl_thread *
egl_thread(void)
{
	struct egl_thread *state;
	int error;

	/* The key, once for the process. */
	(void)pthread_once(&egl_key_once, egl_key_make);

	/* The thread's own state, if it has one. */
	state = pthread_getspecific(egl_key);
	if (state != NULL)
		return state;

	/* A new one: no error, no context, OpenGL ES; the shared fallback when there is no memory. */
	state = calloc(1U, sizeof(*state));
	if (state == NULL)
		return &egl_fallback;
	state->error = EGL_SUCCESS;
	state->api = EGL_OPENGL_ES_API;
	error = pthread_setspecific(egl_key, state);
	if (error != 0) {
		free(state);
		return &egl_fallback;
	}

	/* Succeeded: the thread's state. */
	return state;
}

/* Makes the key of the threads' states (once). */
static void
egl_key_make(void)
{
	/* A thread's state is freed when the thread ends. */
	(void)pthread_key_create(&egl_key, egl_thread_free);
}

/* Frees an ended thread's state. */
static void
egl_thread_free(
	void *state)
{
	/* The memory only; its context stays the application's. */
	free(state);
}

/* Sets the thread's error and reports failure. */
static EGLBoolean
egl_fail(
	EGLint error)
{
	/* The first cause is what eglGetError reports. */
	egl_thread()->error = error;
	return EGL_FALSE;
}

/* Clears the thread's error and reports success. */
static EGLBoolean
egl_succeed(void)
{
	/* A successful call leaves EGL_SUCCESS. */
	egl_thread()->error = EGL_SUCCESS;
	return EGL_TRUE;
}

/* Returns the display of a platform and native display, making it the first time. */
static struct zegl_display *
egl_display_get(
	int platform,
	void *native)
{
	struct zegl_display *display;

	/* The same platform and native display give the same EGL display. */
	for (display = egl_displays; display != NULL; display = display->next) {
		if (display->platform == platform && display->native == native)
			return display;
	}

	/* A new one, at the head of the list. */
	display = calloc(1U, sizeof(*display));
	if (display == NULL)
		return NULL;

	/* Succeeded: the display, not initialized. */
	display->platform = platform;
	display->native = native;
	display->next = egl_displays;
	egl_displays = display;
	return display;
}

/* Returns a display handle as a display this library made, or NULL. */
static struct zegl_display *
egl_display_valid(
	EGLDisplay dpy)
{
	struct zegl_display *display;

	/* One of the list's. */
	for (display = egl_displays; display != NULL; display = display->next) {
		if ((EGLDisplay)display == dpy)
			return display;
	}

	/* Not one of ours. */
	return NULL;
}

/* Fills a display's configs: 8-bit RGBA or RGB, with or without a 24-bit depth and 8-bit stencil buffer. */
static void
egl_configs(
	struct zegl_display *display)
{
	struct zegl_config *config;
	unsigned index;

	/* Four configs: alpha and none, each without and with depth and stencil. */
	display->config_count = 4U;
	for (index = 0U; index < display->config_count; index++) {
		config = &display->configs[index];
		memset(config, 0, sizeof(*config));
		config->id = (EGLint)index + 1;
		config->red = 8;
		config->green = 8;
		config->blue = 8;
		config->alpha = 8;
		if (index >= 2U)
			config->alpha = 0;
		if ((index & 1U) != 0U) {
			config->depth = 24;
			config->stencil = 8;
		}

		/* Every config draws windows and pbuffers with OpenGL ES 2 and 3; a surfaceless display only pbuffers. */
		config->surface_type = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
		if (display->platform == ZEGL_PLATFORM_SURFACELESS)
			config->surface_type = EGL_PBUFFER_BIT;
		config->renderable = EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT;
	}
}

/* Tells whether a config meets every attribute of a list (sizes at least, masks all bits, the ID exactly). */
static int
egl_config_matches(
	const struct zegl_config *config,
	const EGLint *attributes)
{
	unsigned index;
	EGLint wanted;
	EGLint value;
	EGLint error;

	/* No list matches every config. */
	if (attributes == NULL)
		return 1;

	/* Each attribute and its wanted value. */
	for (index = 0U; attributes[index] != EGL_NONE; index += 2U) {
		wanted = attributes[index + 1U];

		/* EGL_DONT_CARE matches anything. */
		if (wanted == EGL_DONT_CARE)
			continue;

		/* Routes the attribute by how it is compared. */
		switch (attributes[index]) {
		case EGL_RED_SIZE:
		case EGL_GREEN_SIZE:
		case EGL_BLUE_SIZE:
		case EGL_ALPHA_SIZE:
		case EGL_DEPTH_SIZE:
		case EGL_STENCIL_SIZE:
		case EGL_BUFFER_SIZE:
			/* A size is met by one at least as large. */
			error = egl_config_value(config, attributes[index], &value);
			if (error != EGL_SUCCESS || value < wanted)
				return 0;
			break;
		case EGL_SURFACE_TYPE:
		case EGL_RENDERABLE_TYPE:
		case EGL_CONFORMANT:
			/* A mask is met when every bit asked for is there. */
			error = egl_config_value(config, attributes[index], &value);
			if (error != EGL_SUCCESS || (value & wanted) != wanted)
				return 0;
			break;
		case EGL_CONFIG_ID:
			/* The ID is met exactly. */
			if (config->id != wanted)
				return 0;
			break;
		case EGL_SAMPLES:
		case EGL_SAMPLE_BUFFERS:
			/* There is no multisampling. */
			if (wanted > 0)
				return 0;
			break;
		default:
			/* The other attributes do not narrow the choice here. */
			break;
		}
	}

	/* Every attribute is met. */
	return 1;
}

/* Reports a config's value of an attribute. */
static EGLint
egl_config_value(
	const struct zegl_config *config,
	EGLint attribute,
	EGLint *value)
{
	/* Routes the attribute. */
	switch (attribute) {
	case EGL_BUFFER_SIZE:
		*value = config->red + config->green + config->blue + config->alpha;
		break;
	case EGL_RED_SIZE:
		*value = config->red;
		break;
	case EGL_GREEN_SIZE:
		*value = config->green;
		break;
	case EGL_BLUE_SIZE:
		*value = config->blue;
		break;
	case EGL_ALPHA_SIZE:
		*value = config->alpha;
		break;
	case EGL_DEPTH_SIZE:
		*value = config->depth;
		break;
	case EGL_STENCIL_SIZE:
		*value = config->stencil;
		break;
	case EGL_CONFIG_ID:
		*value = config->id;
		break;
	case EGL_SURFACE_TYPE:
		*value = config->surface_type;
		break;
	case EGL_RENDERABLE_TYPE:
	case EGL_CONFORMANT:
		*value = config->renderable;
		break;
	case EGL_COLOR_BUFFER_TYPE:
		*value = EGL_RGB_BUFFER;
		break;
	case EGL_CONFIG_CAVEAT:
	case EGL_NATIVE_VISUAL_TYPE:
	case EGL_TRANSPARENT_TYPE:
		*value = EGL_NONE;
		break;
	case EGL_MIN_SWAP_INTERVAL:
		*value = 0;
		break;
	case EGL_MAX_SWAP_INTERVAL:
		*value = 1;
		break;
	case EGL_NATIVE_RENDERABLE:
	case EGL_BIND_TO_TEXTURE_RGB:
	case EGL_BIND_TO_TEXTURE_RGBA:
		*value = EGL_FALSE;
		break;
	case EGL_LEVEL:
	case EGL_SAMPLES:
	case EGL_SAMPLE_BUFFERS:
	case EGL_NATIVE_VISUAL_ID:
	case EGL_LUMINANCE_SIZE:
	case EGL_ALPHA_MASK_SIZE:
	case EGL_MAX_PBUFFER_WIDTH:
	case EGL_MAX_PBUFFER_HEIGHT:
	case EGL_MAX_PBUFFER_PIXELS:
	case EGL_TRANSPARENT_RED_VALUE:
	case EGL_TRANSPARENT_GREEN_VALUE:
	case EGL_TRANSPARENT_BLUE_VALUE:
		*value = 0;
		break;
	default:
		return EGL_BAD_ATTRIBUTE;
	}

	/* Succeeded: the value. */
	return EGL_SUCCESS;
}

/* Returns a config handle as one of a display's configs, or NULL. */
static struct zegl_config *
egl_config_valid(
	struct zegl_display *display,
	EGLConfig config)
{
	unsigned index;

	/* One of the display's. */
	for (index = 0U; index < display->config_count; index++) {
		if ((EGLConfig)&display->configs[index] == config)
			return &display->configs[index];
	}

	/* Not one of them. */
	return NULL;
}

/* Makes a window surface on a display: over a wl_egl_window on Wayland, or over the whole screen. */
static EGLSurface
egl_window_surface(
	struct zegl_display *display,
	struct zegl_config *config,
	void *native)
{
	struct zegl_surface *surface;
	EGLint error;

	/* The config must draw windows, and a Wayland window must be given on Wayland. */
	if ((config->surface_type & EGL_WINDOW_BIT) == 0) {
		(void)egl_fail(EGL_BAD_MATCH);
		return EGL_NO_SURFACE;
	}

	/* A Wayland window must be given on Wayland. */
	if (display->platform == ZEGL_PLATFORM_WAYLAND && native == NULL) {
		(void)egl_fail(EGL_BAD_NATIVE_WINDOW);
		return EGL_NO_SURFACE;
	}

	/* The surface. */
	surface = calloc(1U, sizeof(*surface));
	if (surface == NULL) {
		(void)egl_fail(EGL_BAD_ALLOC);
		return EGL_NO_SURFACE;
	}

	/* What it was made with; FIFO presenting (an interval of 1) to begin with. */
	surface->display = display;
	surface->config = config;
	surface->kind = EGL_WINDOW_BIT;
	surface->interval = 1;
	if (display->platform == ZEGL_PLATFORM_WAYLAND)
		surface->window = native;

	/* Its Vulkan surface and swapchain. */
	error = zegl_surface_open(surface);
	if (error != EGL_SUCCESS) {
		zegl_surface_close(surface);
		free(surface);
		(void)egl_fail(error);
		return EGL_NO_SURFACE;
	}

	/* Succeeded: the surface. */
	(void)egl_succeed();
	return (EGLSurface)surface;
}
