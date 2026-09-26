/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's GLX (WS069 p004): OpenGL contexts for Xzed's windows.
 *
 * Rendering is direct.  A GLX context is an EGL context (OpenGL ES 2,
 * libGLESv2's translation to Vulkan, built into this library, with the
 * fixed-function OpenGL 1.x of fixed.c and immediate.c) on a
 * surfaceless EGL display, current on a pbuffer the size of the window.
 * glXSwapBuffers reads the pbuffer back and puts it into the window with
 * XzedPutImageRGB24; a window that changed size gets a new pbuffer.
 * Xzed's GLX extension answers the version and the strings (Xzed's
 * glx.c).
 *
 * Xzed's visuals for GL are two TrueColor 24-bit, double-buffered RGBA
 * ones: 0x21 without and 0x22 with a 24-bit depth and 8-bit stencil
 * buffer; the GLX 1.3 configs are the same two.
 */

#include "fixed.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glx.h>
#include "userland/X11/libX11/Xzed.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* GLX's minor opcodes this library sends. */
#define GLX_REQUEST_QUERY_VERSION	7U
#define GLX_REQUEST_QUERY_SERVER_STRING	19U
#define GLX_REQUEST_CLIENT_INFO		20U

/* The version this library speaks. */
#define GLX_CLIENT_MAJOR		1
#define GLX_CLIENT_MINOR		4

/* The two visuals' ids. */
#define GLX_VISUAL_PLAIN		0x21U
#define GLX_VISUAL_DEPTH		0x22U

/*
 * A GLX context: the EGL context, the config it was made with, and the
 * pbuffer it draws into for its window, with the buffers a swap uses.
 */
struct __GLXcontextRec {
	/* The display and whether the visual has a depth buffer. */
	Display *display;
	int depth;

	/* The EGL context and config. */
	EGLContext context;
	EGLConfig config;

	/* The window it is current on, the pbuffer of the window's size, and that size. */
	GLXDrawable drawable;
	EGLSurface pbuffer;
	unsigned width;
	unsigned height;

	/* The RGBA pixels read back and the RGB rows sent, and their size in pixels. */
	unsigned char *pixels;
	unsigned char *rows;
	size_t capacity;
};

/*
 * A GLX 1.3 config: one of the two visuals.
 */
struct __GLXFBConfigRec {
	int id;
	int depth;
};

/* The EGL display every context uses (surfaceless: pbuffers only), made at the first context. */
static EGLDisplay glx_egl = EGL_NO_DISPLAY;

/* GLX's major opcode on the server (0 until asked). */
static int glx_major;

/* The server's strings (vendor, version, extensions), asked once each. */
static char *glx_server_strings[3];

/* The two visuals and the two configs. */
static Visual glx_visuals[2] = {
	{ NULL, GLX_VISUAL_PLAIN, TrueColor, 0xff0000UL, 0x00ff00UL, 0x0000ffUL, 8, 256 },
	{ NULL, GLX_VISUAL_DEPTH, TrueColor, 0xff0000UL, 0x00ff00UL, 0x0000ffUL, 8, 256 }
};
static struct __GLXFBConfigRec glx_configs[2] = {
	{ 1, 0 },
	{ 2, 1 }
};

/*
 * The step glXSwapBuffers is at (zedBSD's own, for a watchdog in the
 * application to say where a swap stopped, WS069 p007): 0 outside, 1 the
 * readback, 2 the conversion, 3 the image to the server, 4 the EGL swap,
 * 5 the window's size, 6 a new pbuffer, 7 current again.
 */
volatile int zglx_swap_step;

/* The key of each thread's current context, made once. */
static pthread_key_t glx_current_key;
static pthread_once_t glx_current_once = PTHREAD_ONCE_INIT;

static int glx_setup(Display *dpy);
static XVisualInfo *glx_visual_info(int depth);
static GLXContext glx_context(Display *dpy, int depth);
static int glx_pbuffer(GLXContext ctx, GLXDrawable drawable);
static GLXContext glx_current(void);
static void glx_set_current(GLXContext ctx);
static void glx_key_make(void);
static void glx_put32(unsigned char *bytes, uint32_t value);
static uint32_t glx_get32(const unsigned char *bytes);

/*
 * Reports whether the server has GLX, with its first error and event.
 */
Bool
glXQueryExtension(
	Display *dpy,
	int *errorBase,
	int *eventBase)
{
	Bool present;
	int major;
	int event;
	int error;

	/* The server's answer. */
	present = XQueryExtension(dpy, "GLX", &major, &event, &error);
	if (!present)
		return False;

	/* Its numbers. */
	glx_major = major;
	if (errorBase != NULL)
		*errorBase = error;
	if (eventBase != NULL)
		*eventBase = event;
	return True;
}

/*
 * Reports the GLX version the server and this library both speak.
 */
Bool
glXQueryVersion(
	Display *dpy,
	int *major,
	int *minor)
{
	unsigned char request[12];
	unsigned char reply[32];
	int server_major;
	int server_minor;
	int status;

	/* GLX on the server. */
	status = glx_setup(dpy);
	if (status != 0)
		return False;

	/* QueryVersion with this library's version. */
	memset(request, 0, sizeof(request));
	request[0] = (unsigned char)glx_major;
	request[1] = GLX_REQUEST_QUERY_VERSION;
	glx_put32(request + 4, GLX_CLIENT_MAJOR);
	glx_put32(request + 8, GLX_CLIENT_MINOR);
	status = XzedExtensionRequest(dpy, request, sizeof(request), reply, NULL, NULL);
	if (status != 0)
		return False;

	/* The lower of the two. */
	server_major = (int)glx_get32(reply + 8);
	server_minor = (int)glx_get32(reply + 12);
	if (server_major > GLX_CLIENT_MAJOR || (server_major == GLX_CLIENT_MAJOR && server_minor > GLX_CLIENT_MINOR)) {
		server_major = GLX_CLIENT_MAJOR;
		server_minor = GLX_CLIENT_MINOR;
	}

	/* Reported. */
	if (major != NULL)
		*major = server_major;
	if (minor != NULL)
		*minor = server_minor;
	return True;
}

/*
 * Returns one of this library's strings.
 */
const char *
glXGetClientString(
	Display *dpy,
	int name)
{
	/* Vendor, version, extensions. */
	(void)dpy;
	switch (name) {
	case GLX_VENDOR:
		return "zedBSD";
	case GLX_VERSION:
		return "1.4";
	case GLX_EXTENSIONS:
		return "GLX_ARB_get_proc_address";
	default:
		break;
	}

	/* Not a name. */
	return NULL;
}

/*
 * Returns one of the server's strings (asked once, kept for the process).
 */
const char *
glXQueryServerString(
	Display *dpy,
	int screen,
	int name)
{
	unsigned char request[12];
	unsigned char reply[32];
	unsigned char *extra;
	size_t length;
	int status;

	/* A name, and its string when it was asked before. */
	if (name < GLX_VENDOR || name > GLX_EXTENSIONS)
		return NULL;
	if (glx_server_strings[name - 1] != NULL)
		return glx_server_strings[name - 1];

	/* GLX on the server. */
	status = glx_setup(dpy);
	if (status != 0)
		return NULL;

	/* QueryServerString. */
	memset(request, 0, sizeof(request));
	request[0] = (unsigned char)glx_major;
	request[1] = GLX_REQUEST_QUERY_SERVER_STRING;
	glx_put32(request + 4, (uint32_t)screen);
	glx_put32(request + 8, (uint32_t)name);
	extra = NULL;
	status = XzedExtensionRequest(dpy, request, sizeof(request), reply, &extra, &length);
	if (status != 0 || extra == NULL)
		return NULL;

	/* Kept (the reply's bytes end with a terminator). */
	glx_server_strings[name - 1] = (char *)extra;
	return glx_server_strings[name - 1];
}

/*
 * Returns the GLX extensions both sides have: this library's.
 */
const char *
glXQueryExtensionsString(
	Display *dpy,
	int screen)
{
	/* The server's extensions are this library's. */
	(void)screen;
	return glXGetClientString(dpy, GLX_EXTENSIONS);
}

/*
 * Returns a visual meeting a list of attributes: RGBA is required, a depth
 * or stencil size picks the visual with a depth buffer.
 */
XVisualInfo *
glXChooseVisual(
	Display *dpy,
	int screen,
	int *attribList)
{
	unsigned index;
	int rgba;
	int depth;

	/* Each attribute: the booleans alone, the rest with a value. */
	(void)dpy;
	(void)screen;
	rgba = 0;
	depth = 0;
	for (index = 0U; attribList != NULL && attribList[index] != None; index++) {
		switch (attribList[index]) {
		case GLX_RGBA:
			rgba = 1;
			break;
		case GLX_USE_GL:
		case GLX_DOUBLEBUFFER:
		case GLX_STEREO:
			break;
		case GLX_DEPTH_SIZE:
		case GLX_STENCIL_SIZE:
			index++;
			if (attribList[index] > 0)
				depth = 1;
			break;
		default:
			index++;
			break;
		}
	}

	/* Colour index visuals are not there. */
	if (!rgba)
		return NULL;
	return glx_visual_info(depth);
}

/*
 * Reports an attribute of a visual.
 */
int
glXGetConfig(
	Display *dpy,
	XVisualInfo *vis,
	int attrib,
	int *value)
{
	int depth;

	/* One of the two visuals. */
	(void)dpy;
	if (vis == NULL || (vis->visualid != GLX_VISUAL_PLAIN && vis->visualid != GLX_VISUAL_DEPTH))
		return GLX_BAD_VISUAL;
	depth = 0;
	if (vis->visualid == GLX_VISUAL_DEPTH)
		depth = 1;

	/* The attribute. */
	switch (attrib) {
	case GLX_USE_GL:
	case GLX_RGBA:
	case GLX_DOUBLEBUFFER:
		*value = 1;
		return 0;
	case GLX_BUFFER_SIZE:
		*value = 32;
		return 0;
	case GLX_RED_SIZE:
	case GLX_GREEN_SIZE:
	case GLX_BLUE_SIZE:
	case GLX_ALPHA_SIZE:
		*value = 8;
		return 0;
	case GLX_DEPTH_SIZE:
		*value = 24 * depth;
		return 0;
	case GLX_STENCIL_SIZE:
		*value = 8 * depth;
		return 0;
	case GLX_LEVEL:
	case GLX_STEREO:
	case GLX_AUX_BUFFERS:
	case GLX_ACCUM_RED_SIZE:
	case GLX_ACCUM_GREEN_SIZE:
	case GLX_ACCUM_BLUE_SIZE:
	case GLX_ACCUM_ALPHA_SIZE:
		*value = 0;
		return 0;
	default:
		break;
	}

	/* Not an attribute. */
	return GLX_BAD_ATTRIBUTE;
}

/*
 * Makes a context for a visual (always direct; sharing is not there yet).
 */
GLXContext
glXCreateContext(
	Display *dpy,
	XVisualInfo *vis,
	GLXContext shareList,
	Bool direct)
{
	GLXContext ctx;
	int depth;

	/* One of the two visuals. */
	(void)shareList;
	(void)direct;
	if (vis == NULL)
		return NULL;
	depth = 0;
	if (vis->visualid == GLX_VISUAL_DEPTH)
		depth = 1;

	/* The context. */
	ctx = glx_context(dpy, depth);
	return ctx;
}

/*
 * Destroys a context (made not current first when it is current).
 */
void
glXDestroyContext(
	Display *dpy,
	GLXContext ctx)
{
	GLXContext current;

	/* Nothing to destroy. */
	(void)dpy;
	if (ctx == NULL)
		return;

	/* Not current any more. */
	current = glx_current();
	if (current == ctx) {
		(void)eglMakeCurrent(glx_egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		glx_set_current(NULL);
	}

	/* The pbuffer, the EGL context and the buffers. */
	if (ctx->pbuffer != EGL_NO_SURFACE)
		(void)eglDestroySurface(glx_egl, ctx->pbuffer);
	(void)eglDestroyContext(glx_egl, ctx->context);
	free(ctx->pixels);
	free(ctx->rows);
	free(ctx);
}

/*
 * Makes a context current on a window (or none current).
 */
Bool
glXMakeCurrent(
	Display *dpy,
	GLXDrawable drawable,
	GLXContext ctx)
{
	EGLBoolean done;
	int status;

	/* Releasing. */
	if (ctx == NULL || drawable == None) {
		(void)eglMakeCurrent(glx_egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		glx_set_current(NULL);
		return True;
	}

	/* A pbuffer of the window's size. */
	ctx->display = dpy;
	status = glx_pbuffer(ctx, drawable);
	if (status != 0)
		return False;

	/* Current on it. */
	done = eglMakeCurrent(glx_egl, ctx->pbuffer, ctx->pbuffer, ctx->context);
	if (!done)
		return False;

	/* Succeeded: the thread's current context. */
	glx_set_current(ctx);
	return True;
}

/*
 * Shows what the current context drew into its window: the pbuffer read
 * back and put into the window; then a window of a new size gets a new
 * pbuffer.
 */
void
glXSwapBuffers(
	Display *dpy,
	GLXDrawable drawable)
{
	GLXContext ctx;
	unsigned char *source;
	unsigned char *target;
	unsigned row;
	unsigned column;
	int status;
	EGLBoolean done;

	/* The current context, on this window. */
	ctx = glx_current();
	if (ctx == NULL || ctx->drawable != drawable)
		return;

	/* The pixels, from the bottom row up. */
	zglx_swap_step = 1;
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, (GLsizei)ctx->width, (GLsizei)ctx->height, GL_RGBA, GL_UNSIGNED_BYTE, ctx->pixels);

	/* As RGB rows from the top down. */
	zglx_swap_step = 2;
	for (row = 0U; row < ctx->height; row++) {
		source = ctx->pixels + (size_t)(ctx->height - 1U - row) * ctx->width * 4U;
		target = ctx->rows + (size_t)row * ctx->width * 3U;
		for (column = 0U; column < ctx->width; column++) {
			target[0] = source[0];
			target[1] = source[1];
			target[2] = source[2];
			source += 4;
			target += 3;
		}
	}

	/* Into the window, and the frame's resources freed. */
	zglx_swap_step = 3;
	(void)XzedPutImageRGB24(dpy, drawable, 0, 0, ctx->width, ctx->height, ctx->rows, ctx->width * 3U);
	zglx_swap_step = 4;
	(void)eglSwapBuffers(glx_egl, ctx->pbuffer);

	/* A window of a new size: a new pbuffer, current again. */
	zglx_swap_step = 5;
	status = glx_pbuffer(ctx, drawable);
	if (status == 0) {
		zglx_swap_step = 7;
		done = eglMakeCurrent(glx_egl, ctx->pbuffer, ctx->pbuffer, ctx->context);
		(void)done;
	}

	/* Done. */
	zglx_swap_step = 0;
}

/*
 * Reports whether a context renders directly: always.
 */
Bool
glXIsDirect(
	Display *dpy,
	GLXContext ctx)
{
	/* Every context. */
	(void)dpy;
	(void)ctx;
	return True;
}

/*
 * Returns the calling thread's current context.
 */
GLXContext
glXGetCurrentContext(void)
{
	GLXContext ctx;

	/* The thread's. */
	ctx = glx_current();
	return ctx;
}

/*
 * Returns the window the calling thread's context is current on.
 */
GLXDrawable
glXGetCurrentDrawable(void)
{
	GLXContext ctx;

	/* None without a context. */
	ctx = glx_current();
	if (ctx == NULL)
		return None;
	return ctx->drawable;
}

/*
 * Returns the display of the calling thread's current context.
 */
Display *
glXGetCurrentDisplay(void)
{
	GLXContext ctx;

	/* NULL without a context. */
	ctx = glx_current();
	if (ctx == NULL)
		return NULL;
	return ctx->display;
}

/*
 * Waits for GL: nothing is shown before glXSwapBuffers, which waits.
 */
void
glXWaitGL(void)
{
	/* Nothing to wait for. */
	return;
}

/*
 * Waits for X: this library's requests are synchronous.
 */
void
glXWaitX(void)
{
	/* Nothing to wait for. */
	return;
}

/*
 * Returns a GL or GLX function by its name.
 */
__GLXextFuncPtr
glXGetProcAddress(
	const GLubyte *procName)
{
	__eglMustCastToProperFunctionPointerType address;

	/* EGL finds it in the program's global scope, where this library's functions are. */
	address = eglGetProcAddress((const char *)procName);
	return (__GLXextFuncPtr)address;
}

/*
 * The ARB spelling of glXGetProcAddress.
 */
__GLXextFuncPtr
glXGetProcAddressARB(
	const GLubyte *procName)
{
	__GLXextFuncPtr address;

	/* The same. */
	address = glXGetProcAddress(procName);
	return address;
}

/*
 * Returns the two configs (freed with XFree).
 */
GLXFBConfig *
glXGetFBConfigs(
	Display *dpy,
	int screen,
	int *nelements)
{
	GLXFBConfig *configs;

	/* The list. */
	(void)dpy;
	(void)screen;
	configs = malloc(2U * sizeof(*configs));
	if (configs == NULL) {
		*nelements = 0;
		return NULL;
	}

	/* Both. */
	configs[0] = &glx_configs[0];
	configs[1] = &glx_configs[1];
	*nelements = 2;
	return configs;
}

/*
 * Returns the configs meeting a list of attributes, the smallest first:
 * a depth or stencil size needs the one with a depth buffer.
 */
GLXFBConfig *
glXChooseFBConfig(
	Display *dpy,
	int screen,
	const int *attribList,
	int *nitems)
{
	GLXFBConfig *configs;
	unsigned index;
	int depth;

	/* Whether a depth buffer is needed. */
	depth = 0;
	for (index = 0U; attribList != NULL && attribList[index] != None; index += 2U) {
		if ((attribList[index] == GLX_DEPTH_SIZE || attribList[index] == GLX_STENCIL_SIZE) && attribList[index + 1U] > 0)
			depth = 1;
	}

	/* Both configs, or the one with a depth buffer. */
	configs = glXGetFBConfigs(dpy, screen, nitems);
	if (configs != NULL && depth) {
		configs[0] = &glx_configs[1];
		*nitems = 1;
	}

	/* The list (freed with XFree). */
	return configs;
}

/*
 * Reports an attribute of a config.
 */
int
glXGetFBConfigAttrib(
	Display *dpy,
	GLXFBConfig config,
	int attribute,
	int *value)
{
	XVisualInfo info;
	int status;

	/* The config's own attributes. */
	switch (attribute) {
	case GLX_FBCONFIG_ID:
		*value = config->id;
		return 0;
	case GLX_VISUAL_ID:
		*value = (int)glx_visuals[config->depth].visualid;
		return 0;
	case GLX_DRAWABLE_TYPE:
		*value = GLX_WINDOW_BIT;
		return 0;
	case GLX_RENDER_TYPE:
		*value = GLX_RGBA_BIT;
		return 0;
	case GLX_X_RENDERABLE:
		*value = True;
		return 0;
	case GLX_X_VISUAL_TYPE:
		*value = GLX_TRUE_COLOR;
		return 0;
	case GLX_CONFIG_CAVEAT:
	case GLX_TRANSPARENT_TYPE:
		*value = GLX_NONE;
		return 0;
	default:
		break;
	}

	/* The rest as its visual's. */
	memset(&info, 0, sizeof(info));
	info.visualid = glx_visuals[config->depth].visualid;
	status = glXGetConfig(dpy, &info, attribute, value);
	return status;
}

/*
 * Returns a config's visual (freed with XFree).
 */
XVisualInfo *
glXGetVisualFromFBConfig(
	Display *dpy,
	GLXFBConfig config)
{
	XVisualInfo *info;

	/* The visual of the same depth buffer. */
	(void)dpy;
	info = glx_visual_info(config->depth);
	return info;
}

/*
 * Makes a context for a config.
 */
GLXContext
glXCreateNewContext(
	Display *dpy,
	GLXFBConfig config,
	int renderType,
	GLXContext shareList,
	Bool direct)
{
	GLXContext ctx;

	/* RGBA contexts only. */
	(void)shareList;
	(void)direct;
	if (renderType != GLX_RGBA_TYPE || config == NULL)
		return NULL;
	ctx = glx_context(dpy, config->depth);
	return ctx;
}

/*
 * Makes a context current on a window for drawing (reading is the same
 * window).
 */
Bool
glXMakeContextCurrent(
	Display *dpy,
	GLXDrawable draw,
	GLXDrawable read,
	GLXContext ctx)
{
	Bool done;

	/* The draw window. */
	(void)read;
	done = glXMakeCurrent(dpy, draw, ctx);
	return done;
}

/*
 * Makes a GLX window of an X window: the X window itself.
 */
GLXWindow
glXCreateWindow(
	Display *dpy,
	GLXFBConfig config,
	Window win,
	const int *attribList)
{
	/* Nothing to make. */
	(void)dpy;
	(void)config;
	(void)attribList;
	return win;
}

/*
 * Destroys a GLX window: nothing was made.
 */
void
glXDestroyWindow(
	Display *dpy,
	GLXWindow window)
{
	/* Nothing to destroy. */
	(void)dpy;
	(void)window;
}

/* Learns GLX's opcode from the server and introduces this library, once; nonzero when the server has no GLX. */
static int
glx_setup(
	Display *dpy)
{
	unsigned char request[16];
	Bool present;
	int status;

	/* Once. */
	if (glx_major != 0)
		return 0;
	present = glXQueryExtension(dpy, NULL, NULL);
	if (!present)
		return -1;

	/* ClientInfo: this library's version, no extensions. */
	memset(request, 0, sizeof(request));
	request[0] = (unsigned char)glx_major;
	request[1] = GLX_REQUEST_CLIENT_INFO;
	glx_put32(request + 4, GLX_CLIENT_MAJOR);
	glx_put32(request + 8, GLX_CLIENT_MINOR);
	status = XzedExtensionRequest(dpy, request, sizeof(request), NULL, NULL, NULL);
	return status;
}

/* Returns a new description of a visual (freed with XFree); NULL when there is no memory. */
static XVisualInfo *
glx_visual_info(
	int depth)
{
	XVisualInfo *info;

	/* The description. */
	info = calloc(1U, sizeof(*info));
	if (info == NULL)
		return NULL;

	/* Succeeded: TrueColor, 24 bits. */
	info->visual = &glx_visuals[depth];
	info->visualid = glx_visuals[depth].visualid;
	info->depth = 24;
	info->class = TrueColor;
	info->red_mask = 0xff0000UL;
	info->green_mask = 0x00ff00UL;
	info->blue_mask = 0x0000ffUL;
	info->colormap_size = 256;
	info->bits_per_rgb = 8;
	return info;
}

/* Makes a context: the EGL display at the first one, a config with or without a depth buffer, an OpenGL ES 2 context. */
static GLXContext
glx_context(
	Display *dpy,
	int depth)
{
	static const EGLint context_attributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	EGLint config_attributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 0,
		EGL_NONE
	};
	GLXContext ctx;
	EGLBoolean done;
	EGLint count;
	int status;

	/* GLX on the server, the fixed-function layer's hooks, and the EGL display. */
	status = glx_setup(dpy);
	if (status != 0)
		return NULL;
	fixed_install();
	if (glx_egl == EGL_NO_DISPLAY) {
		glx_egl = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
		done = eglInitialize(glx_egl, NULL, NULL);
		if (!done) {
			glx_egl = EGL_NO_DISPLAY;
			return NULL;
		}
	}

	/* The context's record. */
	ctx = calloc(1U, sizeof(*ctx));
	if (ctx == NULL)
		return NULL;
	ctx->display = dpy;
	ctx->depth = depth;
	ctx->pbuffer = EGL_NO_SURFACE;

	/* The config, with a depth buffer when the visual has one. */
	if (depth)
		config_attributes[13] = 24;
	done = eglChooseConfig(glx_egl, config_attributes, &ctx->config, 1, &count);
	if (!done || count < 1) {
		free(ctx);
		return NULL;
	}

	/* The EGL context. */
	ctx->context = eglCreateContext(glx_egl, ctx->config, EGL_NO_CONTEXT, context_attributes);
	if (ctx->context == EGL_NO_CONTEXT) {
		free(ctx);
		return NULL;
	}

	/* Succeeded: the context. */
	return ctx;
}

/*
 * Gives a context a pbuffer of its window's size (and the buffers a swap
 * uses), making it again when the window or its size changed.  Returns 0
 * (the pbuffer is new or was kept), or -1.  The caller makes it current.
 */
static int
glx_pbuffer(
	GLXContext ctx,
	GLXDrawable drawable)
{
	EGLint attributes[5];
	unsigned char *pixels;
	unsigned char *rows;
	unsigned int width;
	unsigned int height;
	size_t size;
	int got;

	/* The window's size. */
	got = XGetGeometry(ctx->display, drawable, NULL, NULL, NULL, &width, &height, NULL, NULL);
	if (!got)
		return -1;
	if (width == 0U)
		width = 1U;
	if (height == 0U)
		height = 1U;

	/* The same window at the same size keeps its pbuffer. */
	if (ctx->pbuffer != EGL_NO_SURFACE && ctx->drawable == drawable && ctx->width == width && ctx->height == height)
		return 0;

	/* The buffers a swap uses, when they are too small. */
	size = (size_t)width * height;
	if (size > ctx->capacity) {
		pixels = realloc(ctx->pixels, size * 4U);
		if (pixels == NULL)
			return -1;
		ctx->pixels = pixels;
		rows = realloc(ctx->rows, size * 3U);
		if (rows == NULL)
			return -1;
		ctx->rows = rows;
		ctx->capacity = size;
	}

	/* The old pbuffer goes (not current while it goes). */
	zglx_swap_step = 6;
	if (ctx->pbuffer != EGL_NO_SURFACE) {
		(void)eglMakeCurrent(glx_egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		(void)eglDestroySurface(glx_egl, ctx->pbuffer);
		ctx->pbuffer = EGL_NO_SURFACE;
	}

	/* The new one. */
	attributes[0] = EGL_WIDTH;
	attributes[1] = (EGLint)width;
	attributes[2] = EGL_HEIGHT;
	attributes[3] = (EGLint)height;
	attributes[4] = EGL_NONE;
	ctx->pbuffer = eglCreatePbufferSurface(glx_egl, ctx->config, attributes);
	if (ctx->pbuffer == EGL_NO_SURFACE)
		return -1;

	/* Succeeded: the window and its size. */
	ctx->drawable = drawable;
	ctx->width = width;
	ctx->height = height;
	return 0;
}

/* Returns the calling thread's current context, or NULL. */
static GLXContext
glx_current(void)
{
	GLXContext ctx;

	/* The key, then the thread's value. */
	(void)pthread_once(&glx_current_once, glx_key_make);
	ctx = pthread_getspecific(glx_current_key);
	return ctx;
}

/* Sets the calling thread's current context. */
static void
glx_set_current(
	GLXContext ctx)
{
	/* The key, then the thread's value. */
	(void)pthread_once(&glx_current_once, glx_key_make);
	(void)pthread_setspecific(glx_current_key, ctx);
}

/* Makes the key of the current context (once). */
static void
glx_key_make(void)
{
	/* No destructor: contexts are destroyed with glXDestroyContext. */
	(void)pthread_key_create(&glx_current_key, NULL);
}

/* Writes a 32-bit value least significant byte first (libX11 speaks that order). */
static void
glx_put32(
	unsigned char *bytes,
	uint32_t value)
{
	/* Four bytes. */
	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);
}

/* Reads a 32-bit value least significant byte first. */
static uint32_t
glx_get32(
	const unsigned char *bytes)
{
	/* Four bytes. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}
