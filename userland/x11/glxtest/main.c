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
 * With --gl3 (WS068 p013) the context is an OpenGL 3.0 one from
 * glXCreateContextAttribsARB, after checking that a later version is
 * refused, that an OpenGL 1.4 context from glXCreateNewContext reports
 * its own version, and that a forward-compatible 3.0 context reports its
 * flag; the scene is gl3.c's.  With --gl31 (WS068 p031) the context is
 * an OpenGL 3.1 one after the same checks, and the scene is gl31.c's.
 * With --gl32 (WS068 p033) it is an OpenGL 3.2 compatibility one, after
 * checking a core profile's context too, and the scene is gl32.c's.
 * With --gl33 it is an OpenGL 3.3 compatibility one, and the scene is
 * gl33.c's.  With --gl40 it is an OpenGL 4.0 compatibility one, and the
 * scene is gl33.c's with OpenGL 4.0's outcomes after 3.3's.
 *
 * Every outcome is one line: GLXTEST DONE on a clean end, GLXTEST SKIP
 * (exit status 3) when the display gives an OpenGL 1.4 context but not
 * one of the version asked for (its device has not the version's
 * features), GLXTEST FAILED naming what failed otherwise.
 */

#include <GL/glx.h>

#include "../../tests/egltest/scene.h"
#include "gl3.h"
#include "gl31.h"
#include "gl32.h"
#include "gl33.h"

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
	int gl3;
	int gl3_minor;
	int gl4;
};

static int glxtest_parse(int argc, char **argv, struct glxtest_options *options);
static int glxtest_number(const char *text, const char *name, unsigned long maximum, unsigned long *value);
static void glxtest_sleep(unsigned milliseconds);
static GLXContext glxtest_gl3_context(Display *display, Window window, int major, int minor, int *checks, int *refused);
static int glxtest_core_context(Display *display, Window window, GLXFBConfig config, PFNGLXCREATECONTEXTATTRIBSARBPROC create);

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
	int checks;
	int refused;
	int status;
	int pending;
	Bool done;

	/* The command line. */
	status = glxtest_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: glxtest [--size=WxH] [--frames=N] [--delay-ms=N] [--token=NAME] [--gl3|--gl31|--gl32|--gl33|--gl40]\n");
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
	if (!done ||
	    server_vendor == NULL ||
	    server_version == NULL) {
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

	/* An OpenGL 3.0 to 3.3 or 4.0 context current on the window, the contexts' checks done. */
	checks = 0;
	refused = 0;
	if (options.gl4 || options.gl3) {
		XFree(visual);
		major = 3;
		minor = options.gl3_minor;
		if (options.gl4) {
			major = 4;
			minor = 0;
		}
		context = glxtest_gl3_context(display, window, major, minor, &checks, &refused);
		if (context == NULL && refused) {
			printf("GLXTEST SKIP run=%s: the display gives no OpenGL %d.%d context (an OpenGL 1.4 one only or a version before)\n",
			       options.token, major, minor);
			return 3;
		}
		if (context == NULL) {
			printf("GLXTEST FAILED run=%s operation=glXCreateContextAttribsARB\n", options.token);
			return 1;
		}
	} else {
		context = glXCreateContext(display, visual, NULL, True);
		XFree(visual);
		if (context == NULL) {
			printf("GLXTEST FAILED run=%s operation=glXCreateContext\n", options.token);
			return 1;
		}
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

	/* The scene's program, buffers and texture (OpenGL 3.0's to 4.0's checks). */
	if (options.gl4) {
		status = glxtest_gl40_start();
	} else if (options.gl3 && options.gl3_minor == 3) {
		status = glxtest_gl33_start();
	} else if (options.gl3 && options.gl3_minor == 2) {
		status = glxtest_gl32_start();
	} else if (options.gl3 && options.gl3_minor == 1) {
		status = glxtest_gl31_start();
	} else if (options.gl3) {
		status = glxtest_gl3_start();
	} else {
		status = egltest_scene_start();
	}

	/* A scene that could not start ends the run. */
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

		/* The scene. */
		if (options.gl3 && options.gl3_minor == 3) {
			glxtest_gl33_draw((int)width, (int)height);
		} else if (options.gl3 && options.gl3_minor == 2) {
			glxtest_gl32_draw((int)width, (int)height);
		} else if (options.gl3 && options.gl3_minor == 1) {
			glxtest_gl31_draw((int)width, (int)height);
		} else if (options.gl3) {
			glxtest_gl3_draw((int)width, (int)height);
		} else {
			egltest_scene_draw((int)width, (int)height);
		}

		/* The first frame's colours read back. */
		if (frame == 1U && options.gl3 && options.gl3_minor == 3)
			failures = glxtest_gl33_check((int)width, (int)height, options.token) + checks;
		if (frame == 1U && options.gl3 && options.gl3_minor == 2)
			failures = glxtest_gl32_check((int)width, (int)height, options.token) + checks;
		if (frame == 1U && options.gl3 && options.gl3_minor == 1)
			failures = glxtest_gl31_check((int)width, (int)height, options.token) + checks;
		if (frame == 1U && options.gl3 && options.gl3_minor == 0)
			failures = glxtest_gl3_check((int)width, (int)height, options.token) + checks;
		if (frame == 1U && !options.gl3)
			failures = egltest_scene_check((int)width, (int)height, options.token);

		/* Shown. */
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
	options->gl3 = 0;
	options->gl3_minor = 0;
	options->gl4 = 0;

	/* Each option. */
	for (index = 1; index < argc; index++) {
		/* The window's size. */
		differs = strncmp(argv[index], "--size=", 7U);
		if (differs == 0) {
			scanned = sscanf(argv[index] + 7, "%ux%u", &options->width, &options->height);
			if (scanned != 2 ||
			    options->width == 0U ||
			    options->height == 0U)
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

		/* An OpenGL 3.0 context and its scene. */
		differs = strcmp(argv[index], "--gl3");
		if (differs == 0) {
			options->gl3 = 1;
			continue;
		}

		/* An OpenGL 3.1 context and its scene. */
		differs = strcmp(argv[index], "--gl31");
		if (differs == 0) {
			options->gl3 = 1;
			options->gl3_minor = 1;
			continue;
		}

		/* An OpenGL 3.2 context and its scene. */
		differs = strcmp(argv[index], "--gl32");
		if (differs == 0) {
			options->gl3 = 1;
			options->gl3_minor = 2;
			continue;
		}

		/* An OpenGL 3.3 context and its scene. */
		differs = strcmp(argv[index], "--gl33");
		if (differs == 0) {
			options->gl3 = 1;
			options->gl3_minor = 3;
			continue;
		}

		/* An OpenGL 4.0 context and its scene (3.3's with 4.0's outcomes). */
		differs = strcmp(argv[index], "--gl40");
		if (differs == 0) {
			options->gl3 = 1;
			options->gl3_minor = 3;
			options->gl4 = 1;
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
	if (end == text + length ||
	    *end != '\0' ||
	    *value > maximum)
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

/*
 * Makes the OpenGL 3.0 context of --gl3, or the one of --gl31 to --gl33
 * and --gl40 (a major and minor version; compatibility profile) with
 * glXCreateContextAttribsARB,
 * found by glXGetProcAddress, after checking
 * the other contexts: OpenGL 4.7 (no such version) is refused, an OpenGL 1.4 context from
 * glXCreateNewContext reports 1.4, a forward-compatible 3.0 context
 * reports its flag.  Adds the checks that failed to *checks; NULL when
 * the context cannot be made, with *refused set when the display made an
 * OpenGL 1.4 context but refused the version asked for.
 */
static GLXContext
glxtest_gl3_context(
	Display *display,
	Window window,
	int major,
	int minor,
	int *checks,
	int *refused)
{
	static const int config_attributes[] = { GLX_DEPTH_SIZE, 24, None };
	static const int later_attributes[] = {
		GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, 7,
		GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None
	};
	static const int forward_attributes[] = {
		GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 0,
		GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, None
	};
	int attributes[] = {
		GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 0,
		GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, None
	};
	PFNGLXCREATECONTEXTATTRIBSARBPROC create;
	GLXFBConfig *configs;
	GLXFBConfig config;
	GLXContext context;
	GLXContext other;
	const char *legacy_version;
	GLint legacy_major;
	GLint forward_flags;
	int later_refused;
	int count;
	int differs;

	/* The config with a depth buffer, and the call, found as applications find it. */
	count = 0;
	configs = glXChooseFBConfig(display, DefaultScreen(display), config_attributes, &count);
	if (configs == NULL || count < 1)
		return NULL;
	config = configs[0];
	XFree(configs);
	create = (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddress((const GLubyte *)"glXCreateContextAttribsARB");
	if (create == NULL)
		return NULL;

	/* OpenGL 4.7, which there is not, is refused. */
	other = create(display, config, NULL, True, later_attributes);
	later_refused = 0;
	if (other == NULL)
		later_refused = 1;
	if (other != NULL)
		glXDestroyContext(display, other);

	/* An OpenGL 1.4 context reports its version. */
	legacy_version = "";
	legacy_major = 0;
	other = glXCreateNewContext(display, config, GLX_RGBA_TYPE, NULL, True);
	if (other != NULL) {
		(void)glXMakeCurrent(display, window, other);
		legacy_version = (const char *)glGetString(GL_VERSION);
		glGetIntegerv(GL_MAJOR_VERSION, &legacy_major);
		(void)glXMakeCurrent(display, None, NULL);
		glXDestroyContext(display, other);
	}

	/* A forward-compatible OpenGL 3.0 context reports its flag. */
	forward_flags = -1;
	other = create(display, config, NULL, True, forward_attributes);
	if (other != NULL) {
		(void)glXMakeCurrent(display, window, other);
		glGetIntegerv(GL_CONTEXT_FLAGS, &forward_flags);
		(void)glXMakeCurrent(display, None, NULL);
		glXDestroyContext(display, other);
	}

	/* The checks' line; each that failed counts. */
	printf("GLXTEST GL3 contexts later-refused=%d legacy-version=\"%s\" legacy-major=%d forward-flags=%d\n",
	       later_refused, legacy_version, (int)legacy_major, (int)forward_flags);
	differs = strncmp(legacy_version, "1.4 ", 4U);
	if (!later_refused)
		(*checks)++;
	if (differs != 0)
		(*checks)++;
	if (legacy_major != 1)
		(*checks)++;
	if (forward_flags != GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT)
		(*checks)++;

	/* For 3.2, a core profile's context: its profile, no GL_ARB_compatibility, no fixed function, no draw without a vertex array object. */
	if (major == 3 && minor == 2)
		*checks += glxtest_core_context(display, window, config, create);

	/* The context of the scene, of the version asked for. */
	attributes[1] = major;
	attributes[3] = minor;
	context = create(display, config, NULL, True, attributes);
	if (context == NULL && legacy_version[0] != '\0')
		*refused = 1;
	if (context == NULL)
		return NULL;

	/* Succeeded: the context of the version asked for. */
	return context;
}

/*
 * Checks an OpenGL 3.2 core profile's context: GL_CONTEXT_PROFILE_MASK
 * says core, GL_EXTENSIONS has no GL_ARB_compatibility, glBegin and a
 * draw without a vertex array object (or a program) are refused.  Prints
 * a line; returns how many checks failed.
 */
static int
glxtest_core_context(
	Display *display,
	Window window,
	GLXFBConfig config,
	PFNGLXCREATECONTEXTATTRIBSARBPROC create)
{
	static const int attributes[] = {
		GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 2,
		GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None
	};
	GLXContext context;
	const char *extensions;
	const char *found;
	GLint profile;
	GLenum begin_error;
	GLenum draw_error;
	int failed;

	/* The context, current on the window. */
	context = create(display, config, NULL, True, attributes);
	if (context == NULL) {
		printf("GLXTEST GL32 core context refused\n");
		return 1;
	}

	/* Current on the window. */
	(void)glXMakeCurrent(display, window, context);

	/* Its profile and extensions. */
	profile = 0;
	glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
	extensions = (const char *)glGetString(GL_EXTENSIONS);
	found = NULL;
	if (extensions != NULL)
		found = strstr(extensions, "GL_ARB_compatibility");

	/* The fixed function and a draw without a vertex array object, refused. */
	(void)glGetError();
	glBegin(GL_TRIANGLES);
	begin_error = glGetError();
	glDrawArrays(GL_POINTS, 0, 1);
	draw_error = glGetError();

	/* The context goes. */
	(void)glXMakeCurrent(display, None, NULL);
	glXDestroyContext(display, context);

	/* The line; each check that failed counts. */
	printf("GLXTEST GL32 core profile=%d compatibility=%d begin=0x%x draw=0x%x\n", (int)profile, (int)(found != NULL),
	       (unsigned)begin_error, (unsigned)draw_error);
	failed = 0;
	if (profile != GL_CONTEXT_CORE_PROFILE_BIT)
		failed++;
	if (found != NULL)
		failed++;
	if (begin_error != GL_INVALID_OPERATION)
		failed++;
	if (draw_error != GL_INVALID_OPERATION)
		failed++;

	/* Succeeded: the failures counted. */
	return failed;
}
