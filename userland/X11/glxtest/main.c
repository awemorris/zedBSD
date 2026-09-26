/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * glxtest: an X window drawn with OpenGL through GLX (WS069 p004).
 *
 * It asks the server for GLX and its version and strings, makes a window
 * with a GL visual and a context, and draws egltest's scene (strip,
 * texture, blend, depth and culling; SPIR-V shaders) each frame, following
 * the window's size.  The first frame's colours are read back.
 *
 * Every outcome is one line: GLXTEST DONE on a clean end, GLXTEST FAILED
 * naming what failed otherwise.
 */

#include <GL/glx.h>

#include "../../base/egltest/scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * What the command line asked for.
 */
struct glxtest_options {
	const char *token;
	unsigned width;
	unsigned height;
	unsigned frames;
	unsigned delay_ms;
};

static int glxtest_parse(int argc, char **argv, struct glxtest_options *options);
static int glxtest_number(const char *text, const char *name, unsigned long maximum, unsigned long *value);
static void glxtest_sleep(unsigned milliseconds);

/*
 * Runs the test.
 */
int
main(
	int argc,
	char **argv)
{
	static int visual_attributes[] = {
		GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_DEPTH_SIZE, 24, None
	};
	struct glxtest_options options;
	XVisualInfo *visual;
	GLXContext context;
	Display *display;
	Window window;
	Window root;
	XEvent event;
	const char *server_vendor;
	const char *server_version;
	unsigned int width;
	unsigned int height;
	unsigned int border;
	unsigned int depth;
	unsigned frame;
	int x;
	int y;
	int major;
	int minor;
	int failures;
	int status;
	int pending;
	Bool done;

	/* The command line. */
	status = glxtest_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: glxtest [--size=WxH] [--frames=N] [--delay-ms=N] [--token=NAME]\n");
		return 2;
	}

	/* The server, with GLX. */
	display = XOpenDisplay(NULL);
	if (display == NULL) {
		printf("GLXTEST FAILED run=%s operation=XOpenDisplay\n", options.token);
		return 1;
	}

	/* GLX on it. */
	done = glXQueryExtension(display, NULL, NULL);
	if (!done) {
		printf("GLXTEST FAILED run=%s operation=glXQueryExtension\n", options.token);
		return 1;
	}

	/* Its version and strings. */
	major = 0;
	minor = 0;
	done = glXQueryVersion(display, &major, &minor);
	server_vendor = glXQueryServerString(display, 0, GLX_VENDOR);
	server_version = glXQueryServerString(display, 0, GLX_VERSION);
	if (!done || server_vendor == NULL || server_version == NULL) {
		printf("GLXTEST FAILED run=%s operation=glXQueryVersion\n", options.token);
		return 1;
	}

	/* One line with them. */
	printf("GLXTEST GLX run=%s version=%d.%d server_vendor=\"%s\" server_version=\"%s\" client_vendor=\"%s\"\n",
	       options.token, major, minor, server_vendor, server_version, glXGetClientString(display, GLX_VENDOR));

	/* A visual with a depth buffer, and a window of it. */
	visual = glXChooseVisual(display, DefaultScreen(display), visual_attributes);
	if (visual == NULL) {
		printf("GLXTEST FAILED run=%s operation=glXChooseVisual\n", options.token);
		return 1;
	}

	/* The window, shown. */
	window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, options.width, options.height, 0,
				     BlackPixel(display, DefaultScreen(display)), BlackPixel(display, DefaultScreen(display)));
	(void)XStoreName(display, window, "GLX test");
	(void)XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask);
	(void)XMapWindow(display, window);

	/* A context, current on the window. */
	context = glXCreateContext(display, visual, NULL, True);
	XFree(visual);
	if (context == NULL) {
		printf("GLXTEST FAILED run=%s operation=glXCreateContext\n", options.token);
		return 1;
	}

	/* Current on the window. */
	done = glXMakeCurrent(display, window, context);
	if (!done) {
		printf("GLXTEST FAILED run=%s operation=glXMakeCurrent\n", options.token);
		return 1;
	}

	/* What GL says it is. */
	printf("GLXTEST START run=%s vendor=\"%s\" renderer=\"%s\" version=\"%s\" direct=%d\n", options.token,
	       (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
	       (const char *)glGetString(GL_VERSION), (int)glXIsDirect(display, context));
	fflush(stdout);

	/* The scene's program, buffers and texture. */
	status = egltest_scene_start();
	if (status != 0) {
		printf("GLXTEST FAILED run=%s operation=scene\n", options.token);
		return 1;
	}

	/* Each frame at the window's size. */
	failures = 0;
	for (frame = 1U; frame <= options.frames; frame++) {
		/* The events waiting (a closed connection ends the program in XPending). */
		pending = XPending(display);
		while (pending > 0) {
			(void)XNextEvent(display, &event);
			pending = XPending(display);
		}

		/* The window's size now. */
		width = options.width;
		height = options.height;
		(void)XGetGeometry(display, window, &root, &x, &y, &width, &height, &border, &depth);

		/* The scene, its colours read back at the first frame, and shown. */
		egltest_scene_draw((int)width, (int)height);
		if (frame == 1U)
			failures = egltest_scene_check((int)width, (int)height, options.token);
		glXSwapBuffers(display, window);

		/* The delay between frames. */
		glxtest_sleep(options.delay_ms);
	}

	/* Succeeded: the context and window go. */
	(void)glXMakeCurrent(display, None, NULL);
	glXDestroyContext(display, context);
	(void)XDestroyWindow(display, window);
	(void)XCloseDisplay(display);
	printf("GLXTEST DONE run=%s frames=%u failures=%d\n", options.token, options.frames, failures);
	return 0;
}

/* Reads the command line; returns nonzero for a malformed one. */
static int
glxtest_parse(
	int argc,
	char **argv,
	struct glxtest_options *options)
{
	unsigned long number;
	int index;
	int status;
	int scanned;
	int differs;

	/* The defaults: 640x400, 600 frames about 30 ms apart. */
	options->token = "glx";
	options->width = 640U;
	options->height = 400U;
	options->frames = 600U;
	options->delay_ms = 30U;

	/* Each option. */
	for (index = 1; index < argc; index++) {
		/* The window's size. */
		differs = strncmp(argv[index], "--size=", 7U);
		if (differs == 0) {
			scanned = sscanf(argv[index] + 7, "%ux%u", &options->width, &options->height);
			if (scanned != 2 || options->width == 0U || options->height == 0U)
				return -1;
			continue;
		}

		/* How many frames. */
		status = glxtest_number(argv[index], "--frames=", 1000000UL, &number);
		if (status == 0) {
			options->frames = (unsigned)number;
			continue;
		}

		/* The delay between frames. */
		status = glxtest_number(argv[index], "--delay-ms=", 10000UL, &number);
		if (status == 0) {
			options->delay_ms = (unsigned)number;
			continue;
		}

		/* The name of the run in the log lines. */
		differs = strncmp(argv[index], "--token=", 8U);
		if (differs == 0) {
			options->token = argv[index] + 8;
			continue;
		}

		/* An unknown option refuses the command line. */
		return -1;
	}

	/* Succeeded: the options. */
	return 0;
}

/* Reads "NAME<decimal>" no larger than a maximum; nonzero when the argument is not that. */
static int
glxtest_number(
	const char *text,
	const char *name,
	unsigned long maximum,
	unsigned long *value)
{
	char *end;
	size_t length;
	int differs;

	/* The name first. */
	length = strlen(name);
	differs = strncmp(text, name, length);
	if (differs != 0)
		return -1;

	/* Digits and nothing after them. */
	*value = strtoul(text + length, &end, 10);
	if (end == text + length || *end != '\0' || *value > maximum)
		return -1;

	/* Succeeded: the value. */
	return 0;
}

/* Sleeps some milliseconds. */
static void
glxtest_sleep(
	unsigned milliseconds)
{
	struct timespec delay;

	/* Nothing to wait. */
	if (milliseconds == 0U)
		return;

	/* The delay. */
	delay.tv_sec = (time_t)(milliseconds / 1000U);
	delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&delay, NULL);
}
