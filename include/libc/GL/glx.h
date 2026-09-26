/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's GLX 1.4 (WS069 p004): OpenGL contexts for Xzed's windows.
 * Rendering is direct (libGL draws with EGL and OpenGL ES on Vulkan and
 * puts each frame into the window).
 */

#ifndef LIBC_GL_GLX_H
#define LIBC_GL_GLX_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GLX_VERSION_1_1 1
#define GLX_VERSION_1_2 1
#define GLX_VERSION_1_3 1
#define GLX_VERSION_1_4 1
#define GLX_ARB_get_proc_address 1

/* glXChooseVisual's and glXGetConfig's attributes. */
#define GLX_USE_GL		1
#define GLX_BUFFER_SIZE		2
#define GLX_LEVEL		3
#define GLX_RGBA		4
#define GLX_DOUBLEBUFFER	5
#define GLX_STEREO		6
#define GLX_AUX_BUFFERS		7
#define GLX_RED_SIZE		8
#define GLX_GREEN_SIZE		9
#define GLX_BLUE_SIZE		10
#define GLX_ALPHA_SIZE		11
#define GLX_DEPTH_SIZE		12
#define GLX_STENCIL_SIZE	13
#define GLX_ACCUM_RED_SIZE	14
#define GLX_ACCUM_GREEN_SIZE	15
#define GLX_ACCUM_BLUE_SIZE	16
#define GLX_ACCUM_ALPHA_SIZE	17

/* glXGetConfig's errors. */
#define GLX_BAD_SCREEN		1
#define GLX_BAD_ATTRIBUTE	2
#define GLX_NO_EXTENSION	3
#define GLX_BAD_VISUAL		4
#define GLX_BAD_CONTEXT		5
#define GLX_BAD_VALUE		6
#define GLX_BAD_ENUM		7

/* The strings. */
#define GLX_VENDOR		1
#define GLX_VERSION		2
#define GLX_EXTENSIONS		3

/* GLX 1.3's config attributes and values. */
#define GLX_CONFIG_CAVEAT	0x20
#define GLX_DONT_CARE		0xFFFFFFFF
#define GLX_X_VISUAL_TYPE	0x22
#define GLX_TRANSPARENT_TYPE	0x23
#define GLX_VISUAL_ID		0x800B
#define GLX_SCREEN		0x800C
#define GLX_NONE		0x8000
#define GLX_TRUE_COLOR		0x8002
#define GLX_DRAWABLE_TYPE	0x8010
#define GLX_RENDER_TYPE		0x8011
#define GLX_X_RENDERABLE	0x8012
#define GLX_FBCONFIG_ID		0x8013
#define GLX_RGBA_TYPE		0x8014
#define GLX_MAX_PBUFFER_WIDTH	0x8016
#define GLX_MAX_PBUFFER_HEIGHT	0x8017
#define GLX_WINDOW_BIT		0x00000001
#define GLX_PIXMAP_BIT		0x00000002
#define GLX_PBUFFER_BIT		0x00000004
#define GLX_RGBA_BIT		0x00000001
#define GLX_WIDTH		0x801D
#define GLX_HEIGHT		0x801E

typedef struct __GLXcontextRec *GLXContext;
typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef XID GLXDrawable;
typedef XID GLXPixmap;
typedef XID GLXWindow;
typedef XID GLXPbuffer;
typedef void (*__GLXextFuncPtr)(void);

Bool glXQueryExtension(Display *dpy, int *errorBase, int *eventBase);
Bool glXQueryVersion(Display *dpy, int *major, int *minor);
const char *glXGetClientString(Display *dpy, int name);
const char *glXQueryServerString(Display *dpy, int screen, int name);
const char *glXQueryExtensionsString(Display *dpy, int screen);
XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attribList);
int glXGetConfig(Display *dpy, XVisualInfo *vis, int attrib, int *value);
GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct);
void glXDestroyContext(Display *dpy, GLXContext ctx);
Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx);
void glXSwapBuffers(Display *dpy, GLXDrawable drawable);
Bool glXIsDirect(Display *dpy, GLXContext ctx);
GLXContext glXGetCurrentContext(void);
GLXDrawable glXGetCurrentDrawable(void);
Display *glXGetCurrentDisplay(void);
void glXWaitGL(void);
void glXWaitX(void);
__GLXextFuncPtr glXGetProcAddress(const GLubyte *procName);
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *procName);
GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements);
GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen, const int *attribList, int *nitems);
int glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config, int attribute, int *value);
XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config);
GLXContext glXCreateNewContext(Display *dpy, GLXFBConfig config, int renderType, GLXContext shareList, Bool direct);
Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx);
GLXWindow glXCreateWindow(Display *dpy, GLXFBConfig config, Window win, const int *attribList);
void glXDestroyWindow(Display *dpy, GLXWindow window);

#ifdef __cplusplus
}
#endif

#endif
