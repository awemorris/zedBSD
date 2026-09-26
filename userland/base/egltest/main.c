/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest: clears a window with OpenGL ES and presents it with EGL (WS068).
 *
 * On Wayland (the default when a compositor's socket is given or
 * WAYLAND_DISPLAY is set) the window is an xdg-shell toplevel through
 * wl_egl_window; with --platform=display the whole screen is drawn
 * directly, without a compositor; with --platform=pbuffer it draws into an
 * EGL pbuffer that nothing shows.  Each frame clears to one colour
 * (--color) or to a colour that changes with the frame, and swaps; with
 * --scene=draw each frame draws scene.c's shapes instead, and the first
 * frame reads its colours back (EGLTEST PIXEL and EGLTEST CHECK lines).
 *
 * Every outcome is one line: EGLTEST DONE on a clean end, EGLTEST FAILED
 * naming what failed otherwise.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <wayland-egl.h>
#include <xdg-shell-client-protocol.h>

#include "scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * What the command line asked for.
 */
struct egltest_options {
	const char *display;
	const char *token;
	int direct;
	int pbuffer;
	int width;
	int height;
	unsigned frames;
	unsigned delay_ms;
	int fixed;
	float color[3];
	int scene;
};

/*
 * The Wayland side: the connection, the globals and the toplevel window.
 */
struct egltest_window {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *shell;
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	struct wl_egl_window *egl_window;
	int width;
	int height;
	int resized;
	int configured;
	int closed;
};

/*
 * The EGL objects of a run, and what failed when something did.
 */
struct egltest_egl {
	EGLDisplay display;
	EGLConfig config;
	EGLSurface surface;
	EGLContext context;
	EGLint major;
	EGLint minor;
	const char *operation;
	int failures;
};

static int egltest_parse(int argc, char **argv, struct egltest_options *options);
static int egltest_start(const struct egltest_options *options, struct egltest_window *window, struct egltest_egl *egl);
static int egltest_frames(const struct egltest_options *options, struct egltest_window *window, struct egltest_egl *egl, unsigned *presented);
static const char *egltest_value(const char *argument, const char *name);
static int egltest_number(const char *text, unsigned long maximum, unsigned long *value);
static int egltest_window_open(struct egltest_window *window, const struct egltest_options *options);
static void egltest_window_close(struct egltest_window *window);
static void egltest_frame_color(const struct egltest_options *options, unsigned frame, float *color);
static void egltest_sleep(unsigned milliseconds);
static void egltest_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void egltest_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void egltest_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void egltest_configure(void *data, struct xdg_surface *surface, uint32_t serial);
static void egltest_toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void egltest_toplevel_close(void *data, struct xdg_toplevel *toplevel);

/* The registry's callbacks. */
static const struct wl_registry_listener egltest_registry_listener = {
	egltest_global, egltest_global_remove
};

/* The shell's liveness check. */
static const struct xdg_wm_base_listener egltest_shell_listener = {
	egltest_ping
};

/* The role's configure. */
static const struct xdg_surface_listener egltest_surface_listener = {
	egltest_configure
};

/* The toplevel's size and close request. */
static const struct xdg_toplevel_listener egltest_toplevel_listener = {
	egltest_toplevel_configure, egltest_toplevel_close
};

/*
 * Runs the test.
 */
int
main(
	int argc,
	char **argv)
{
	struct egltest_options options;
	struct egltest_window window;
	struct egltest_egl egl;
	unsigned presented;
	int status;

	/* The command line. */
	status = egltest_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: egltest [--display=NAME] [--platform=wayland|display|pbuffer] [--size=WxH] [--frames=N] [--delay-ms=N] [--color=RRGGBB] [--scene=draw] [--token=NAME]\n");
		return 2;
	}

	/* The window, EGL and a current context; then the frames. */
	memset(&window, 0, sizeof(window));
	memset(&egl, 0, sizeof(egl));
	presented = 0U;
	status = egltest_start(&options, &window, &egl);
	if (status == 0)
		status = egltest_frames(&options, &window, &egl, &presented);

	/* A failure names the step and EGL's error. */
	if (status != 0) {
		printf("EGLTEST FAILED run=%s operation=%s eglerror=0x%x\n", options.token, egl.operation, (unsigned)eglGetError());
		egltest_window_close(&window);
		return 1;
	}

	/* A clean end releases EGL, then the window. */
	(void)eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	(void)eglDestroyContext(egl.display, egl.context);
	(void)eglDestroySurface(egl.display, egl.surface);
	(void)eglTerminate(egl.display);
	egltest_window_close(&window);

	/* Succeeded: how many frames were presented, and how many of the scene's colours differed. */
	printf("EGLTEST DONE run=%s frames=%u glerror=0x%x failures=%d\n", options.token, presented, 0U, egl.failures);
	return 0;
}

/* Opens the window (on Wayland), EGL, a config, a window surface and a current OpenGL ES 2 context; nonzero on failure. */
static int
egltest_start(
	const struct egltest_options *options,
	struct egltest_window *window,
	struct egltest_egl *egl)
{
	EGLint config_attributes[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_DEPTH_SIZE, 0,
		EGL_NONE
	};
	static const EGLint context_attributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	EGLint pbuffer_attributes[5];
	const char *platform;
	EGLint count;
	EGLint width;
	EGLint height;
	EGLBoolean done;
	int status;

	/* The display: Wayland's through the window's connection, or the screen itself. */
	egl->operation = "wayland";
	platform = "display";
	if (!options->direct) {
		platform = "wayland";
		status = egltest_window_open(window, options);
		if (status != 0)
			return -1;
		egl->display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, window->display, NULL);
	} else {
		egl->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	}

	/* EGL itself. */
	egl->operation = "eglInitialize";
	if (egl->display == EGL_NO_DISPLAY)
		return -1;
	done = eglInitialize(egl->display, &egl->major, &egl->minor);
	if (!done)
		return -1;

	/* An 8-bit RGB config for windows and OpenGL ES 2, with a depth buffer for the scene. */
	egl->operation = "eglChooseConfig";
	if (options->scene)
		config_attributes[11] = 24;
	if (options->pbuffer)
		config_attributes[1] = EGL_PBUFFER_BIT;
	done = eglChooseConfig(egl->display, config_attributes, &egl->config, 1, &count);
	if (!done || count < 1)
		return -1;

	/* The surface: over the Wayland EGL window, the whole screen, or a pbuffer of the size asked for. */
	egl->operation = "eglCreateWindowSurface";
	pbuffer_attributes[0] = EGL_WIDTH;
	pbuffer_attributes[1] = options->width;
	pbuffer_attributes[2] = EGL_HEIGHT;
	pbuffer_attributes[3] = options->height;
	pbuffer_attributes[4] = EGL_NONE;
	if (options->pbuffer) {
		platform = "pbuffer";
		egl->operation = "eglCreatePbufferSurface";
		egl->surface = eglCreatePbufferSurface(egl->display, egl->config, pbuffer_attributes);
	} else if (!options->direct) {
		egl->surface = eglCreatePlatformWindowSurface(egl->display, egl->config, window->egl_window, NULL);
	} else {
		egl->surface = eglCreateWindowSurface(egl->display, egl->config, (EGLNativeWindowType)0, NULL);
	}

	/* A surface that could not be made ends the run. */
	if (egl->surface == EGL_NO_SURFACE)
		return -1;

	/* An OpenGL ES 2 context. */
	egl->operation = "eglCreateContext";
	egl->context = eglCreateContext(egl->display, egl->config, EGL_NO_CONTEXT, context_attributes);
	if (egl->context == EGL_NO_CONTEXT)
		return -1;

	/* Current on the surface. */
	egl->operation = "eglMakeCurrent";
	done = eglMakeCurrent(egl->display, egl->surface, egl->surface, egl->context);
	if (!done)
		return -1;

	/* Succeeded: what EGL and GLES say they are. */
	(void)eglQuerySurface(egl->display, egl->surface, EGL_WIDTH, &width);
	(void)eglQuerySurface(egl->display, egl->surface, EGL_HEIGHT, &height);
	printf("EGLTEST START run=%s platform=%s egl=%d.%d vendor=\"%s\" renderer=\"%s\" version=\"%s\" size=%dx%d\n",
	       options->token, platform, egl->major, egl->minor,
	       (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
	       (const char *)glGetString(GL_VERSION), width, height);
	fflush(stdout);

	/* The scene's program, buffers and texture. */
	egl->operation = "scene";
	if (options->scene) {
		status = egltest_scene_start();
		if (status != 0)
			return -1;
	}

	/* Succeeded: ready for the frames. */
	return 0;
}

/* Clears and presents the frames, following the window's size; nonzero on failure. */
static int
egltest_frames(
	const struct egltest_options *options,
	struct egltest_window *window,
	struct egltest_egl *egl,
	unsigned *presented)
{
	EGLBoolean done;
	EGLint width;
	EGLint height;
	float color[3];
	unsigned frame;
	int status;

	/* Each frame, until the count or the compositor's close. */
	for (frame = 1U; frame <= options->frames; frame++) {
		/* The compositor's events: a new size, or a request to close. */
		if (!options->direct) {
			egl->operation = "wl_display_dispatch_pending";
			status = wl_display_dispatch_pending(window->display);
			if (status < 0)
				return -1;

			/* The requests go out; a close ends the frames. */
			(void)wl_display_flush(window->display);
			if (window->closed)
				break;

			/* A new size is given to the EGL window, which EGL follows at the swap. */
			if (window->resized) {
				wl_egl_window_resize(window->egl_window, window->width, window->height, 0, 0);
				window->resized = 0;
			}
		}

		/* The scene over the window's size, its colours read back at the first frame. */
		if (options->scene) {
			width = window->width;
			height = window->height;
			if (options->direct) {
				(void)eglQuerySurface(egl->display, egl->surface, EGL_WIDTH, &width);
				(void)eglQuerySurface(egl->display, egl->surface, EGL_HEIGHT, &height);
			}

			/* The scene. */
			egltest_scene_draw(width, height);
			if (frame == 1U)
				egl->failures = egltest_scene_check(width, height, options->token);
		}

		/* Without the scene: cleared to the frame's colour. */
		egltest_frame_color(options, frame, color);
		if (!options->scene) {
			glClearColor(color[0], color[1], color[2], 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		}

		/* Presented. */
		egl->operation = "eglSwapBuffers";
		done = eglSwapBuffers(egl->display, egl->surface);
		if (!done)
			return -1;

		/* The first frame is logged (the scene logs its own colours). */
		*presented = frame;
		if (frame == 1U && !options->scene) {
			printf("EGLTEST FRAME run=%s frame=1 color=%02x%02x%02x\n", options->token,
			       (unsigned)(color[0] * 255.0f + 0.5f), (unsigned)(color[1] * 255.0f + 0.5f), (unsigned)(color[2] * 255.0f + 0.5f));
			fflush(stdout);
		}

		/* The delay between frames. */
		egltest_sleep(options->delay_ms);
	}

	/* Succeeded: every frame was presented. */
	return 0;
}

/* Reads the command line; returns nonzero for a malformed one. */
static int
egltest_parse(
	int argc,
	char **argv,
	struct egltest_options *options)
{
	const char *value;
	unsigned long number;
	size_t length;
	int index;
	int status;
	int scanned;
	int differs;

	/* The defaults: Wayland, 640x400, 600 frames about 30 ms apart, changing colours. */
	memset(options, 0, sizeof(*options));
	options->token = "egl";
	options->width = 640;
	options->height = 400;
	options->frames = 600U;
	options->delay_ms = 30U;

	/* Each option; the first name that matches takes it. */
	for (index = 1; index < argc; index++) {
		/* The compositor's socket. */
		value = egltest_value(argv[index], "--display=");
		if (value != NULL) {
			options->display = value;
			continue;
		}

		/* The platform: Wayland, the screen directly, or a pbuffer (on the screen's display). */
		value = egltest_value(argv[index], "--platform=");
		if (value != NULL) {
			options->direct = 0;
			options->pbuffer = 0;
			if (value[0] == 'd')
				options->direct = 1;
			if (value[0] == 'p') {
				options->direct = 1;
				options->pbuffer = 1;
			}

			/* The next argument. */
			continue;
		}

		/* The window's size. */
		value = egltest_value(argv[index], "--size=");
		if (value != NULL) {
			scanned = sscanf(value, "%dx%d", &options->width, &options->height);
			if (scanned != 2)
				return -1;
			continue;
		}

		/* How many frames. */
		value = egltest_value(argv[index], "--frames=");
		if (value != NULL) {
			status = egltest_number(value, 1000000UL, &number);
			if (status != 0 || number == 0UL)
				return -1;
			options->frames = (unsigned)number;
			continue;
		}

		/* The delay between frames. */
		value = egltest_value(argv[index], "--delay-ms=");
		if (value != NULL) {
			status = egltest_number(value, 10000UL, &number);
			if (status != 0)
				return -1;
			options->delay_ms = (unsigned)number;
			continue;
		}

		/* One colour for every frame, RRGGBB. */
		value = egltest_value(argv[index], "--color=");
		if (value != NULL) {
			length = strlen(value);
			number = strtoul(value, NULL, 16);
			if (length != 6U)
				return -1;
			options->fixed = 1;
			options->color[0] = (float)((number >> 16) & 0xffUL) / 255.0f;
			options->color[1] = (float)((number >> 8) & 0xffUL) / 255.0f;
			options->color[2] = (float)(number & 0xffUL) / 255.0f;
			continue;
		}

		/* The drawing scene. */
		value = egltest_value(argv[index], "--scene=");
		if (value != NULL) {
			differs = strcmp(value, "draw");
			if (differs != 0)
				return -1;
			options->scene = 1;
			continue;
		}

		/* The name of the run in the log lines. */
		value = egltest_value(argv[index], "--token=");
		if (value != NULL) {
			options->token = value;
			continue;
		}

		/* An unknown option refuses the command line. */
		return -1;
	}

	/* A size that is not positive is refused. */
	if (options->width <= 0 || options->height <= 0)
		return -1;

	/* Succeeded: the options. */
	return 0;
}

/* Returns what follows an option's name in an argument, or NULL when the argument is another option. */
static const char *
egltest_value(
	const char *argument,
	const char *name)
{
	size_t length;
	int differs;

	/* The argument must start with the name. */
	length = strlen(name);
	differs = strncmp(argument, name, length);
	if (differs != 0)
		return NULL;

	/* Reports the value after the name. */
	return argument + length;
}

/* Reads a decimal number no larger than a maximum; nonzero when it is not one. */
static int
egltest_number(
	const char *text,
	unsigned long maximum,
	unsigned long *value)
{
	char *end;

	/* Digits and nothing after them. */
	*value = strtoul(text, &end, 10);
	if (end == text || *end != '\0' || *value > maximum)
		return -1;

	/* Succeeded: the value. */
	return 0;
}

/* Connects to the compositor and makes a toplevel window with its wl_egl_window; returns nonzero on failure. */
static int
egltest_window_open(
	struct egltest_window *window,
	const struct egltest_options *options)
{
	int status;

	/* The connection and the globals. */
	window->width = options->width;
	window->height = options->height;
	window->display = wl_display_connect(options->display);
	if (window->display == NULL)
		return -1;
	window->registry = wl_display_get_registry(window->display);
	if (window->registry == NULL)
		return -1;
	(void)wl_registry_add_listener(window->registry, &egltest_registry_listener, window);
	status = wl_display_roundtrip(window->display);
	if (status < 0 || window->compositor == NULL || window->shell == NULL)
		return -1;

	/* The surface as a toplevel window. */
	window->surface = wl_compositor_create_surface(window->compositor);
	if (window->surface == NULL)
		return -1;
	window->role = xdg_wm_base_get_xdg_surface(window->shell, window->surface);
	if (window->role == NULL)
		return -1;
	(void)xdg_surface_add_listener(window->role, &egltest_surface_listener, window);
	window->toplevel = xdg_surface_get_toplevel(window->role);
	if (window->toplevel == NULL)
		return -1;
	(void)xdg_toplevel_add_listener(window->toplevel, &egltest_toplevel_listener, window);
	xdg_toplevel_set_title(window->toplevel, "EGL test");
	xdg_toplevel_set_app_id(window->toplevel, "egltest");
	wl_surface_commit(window->surface);

	/* The first configure before anything is drawn. */
	status = wl_display_roundtrip(window->display);
	if (status < 0 || !window->configured)
		return -1;

	/* The EGL window at the size the compositor gave. */
	window->resized = 0;
	window->egl_window = wl_egl_window_create(window->surface, window->width, window->height);
	if (window->egl_window == NULL)
		return -1;

	/* Succeeded: the window EGL draws into. */
	return 0;
}

/* Destroys the window and disconnects. */
static void
egltest_window_close(
	struct egltest_window *window)
{
	/* The EGL window, the roles, the surface, the globals, the connection. */
	if (window->egl_window != NULL)
		wl_egl_window_destroy(window->egl_window);
	if (window->toplevel != NULL)
		xdg_toplevel_destroy(window->toplevel);
	if (window->role != NULL)
		xdg_surface_destroy(window->role);
	if (window->surface != NULL)
		wl_surface_destroy(window->surface);
	if (window->shell != NULL)
		xdg_wm_base_destroy(window->shell);
	if (window->compositor != NULL)
		wl_compositor_destroy(window->compositor);
	if (window->registry != NULL)
		wl_registry_destroy(window->registry);
	if (window->display != NULL)
		wl_display_disconnect(window->display);
	memset(window, 0, sizeof(*window));
}

/* The colour of a frame: the one asked for, or a slow cycle through a few. */
static void
egltest_frame_color(
	const struct egltest_options *options,
	unsigned frame,
	float *color)
{
	static const float cycle[4][3] = {
		{ 0.85f, 0.30f, 0.25f },
		{ 0.25f, 0.70f, 0.40f },
		{ 0.25f, 0.45f, 0.90f },
		{ 0.95f, 0.80f, 0.30f }
	};
	unsigned step;

	/* The fixed colour. */
	if (options->fixed) {
		memcpy(color, options->color, 3U * sizeof(float));
		return;
	}

	/* A new colour every 30 frames. */
	step = ((frame - 1U) / 30U) % 4U;
	memcpy(color, cycle[step], 3U * sizeof(float));
}

/* Waits a number of milliseconds. */
static void
egltest_sleep(
	unsigned milliseconds)
{
	struct timespec delay;

	/* Nothing for no delay. */
	if (milliseconds == 0U)
		return;

	/* The wait. */
	delay.tv_sec = (time_t)(milliseconds / 1000U);
	delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&delay, NULL);
}

/* Binds the compositor and the shell. */
static void
egltest_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct egltest_window *window;
	int compositor;
	int shell;

	/* The two globals a window needs. */
	window = data;
	(void)version;
	compositor = strcmp(interface, "wl_compositor");
	shell = strcmp(interface, "xdg_wm_base");
	if (compositor == 0 && window->compositor == NULL) {
		window->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4U);
	} else if (shell == 0 && window->shell == NULL) {
		window->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1U);
		if (window->shell != NULL)
			(void)xdg_wm_base_add_listener(window->shell, &egltest_shell_listener, window);
	}
}

/* A global going away does not matter here. */
static void
egltest_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	/* Nothing to do. */
	(void)data;
	(void)registry;
	(void)name;
}

/* Answers the compositor's liveness check. */
static void
egltest_ping(
	void *data,
	struct xdg_wm_base *shell,
	uint32_t serial)
{
	/* The same serial back. */
	(void)data;
	xdg_wm_base_pong(shell, serial);
}

/* Acknowledges a configure. */
static void
egltest_configure(
	void *data,
	struct xdg_surface *surface,
	uint32_t serial)
{
	struct egltest_window *window;

	/* The next frame is drawn at the size it gave. */
	window = data;
	xdg_surface_ack_configure(surface, serial);
	window->configured = 1;
}

/* Takes the size the compositor gives. */
static void
egltest_toplevel_configure(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height,
	struct wl_array *states)
{
	struct egltest_window *window;

	/* A positive size that differs resizes the window. */
	(void)toplevel;
	(void)states;
	window = data;
	if (width > 0 && height > 0 && (width != window->width || height != window->height)) {
		window->width = width;
		window->height = height;
		window->resized = 1;
	}
}

/* The compositor asks the window to close. */
static void
egltest_toplevel_close(
	void *data,
	struct xdg_toplevel *toplevel)
{
	struct egltest_window *window;

	/* The frame loop ends. */
	(void)toplevel;
	window = data;
	window->closed = 1;
}
