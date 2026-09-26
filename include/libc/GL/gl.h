/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's desktop OpenGL header (WS069): libGL.so has the OpenGL ES 2.0
 * functions (their declarations and types come from GLES2/gl2.h) and the
 * fixed-function part of OpenGL 1.x (WS069 p005) declared here.
 */

#ifndef LIBC_GL_GL_H
#define LIBC_GL_GL_H

#include <GLES2/gl2.h>

#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif

#ifndef GLAPI
#define GLAPI GL_APICALL
#endif

typedef double GLdouble;
typedef double GLclampd;

#define GL_VERSION_1_1 1

#ifdef __cplusplus
extern "C" {
#endif

/* Primitives. */
#define GL_QUADS			0x0007
#define GL_QUAD_STRIP			0x0008
#define GL_POLYGON			0x0009

/* Matrices. */
#define GL_MATRIX_MODE			0x0BA0
#define GL_MODELVIEW			0x1700
#define GL_PROJECTION			0x1701
#define GL_TEXTURE			0x1702
#define GL_MODELVIEW_STACK_DEPTH	0x0BA3
#define GL_PROJECTION_STACK_DEPTH	0x0BA4
#define GL_TEXTURE_STACK_DEPTH		0x0BA5
#define GL_MODELVIEW_MATRIX		0x0BA6
#define GL_PROJECTION_MATRIX		0x0BA7
#define GL_TEXTURE_MATRIX		0x0BA8
#define GL_MAX_MODELVIEW_STACK_DEPTH	0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH	0x0D38
#define GL_MAX_TEXTURE_STACK_DEPTH	0x0D39
#define GL_STACK_OVERFLOW		0x0503
#define GL_STACK_UNDERFLOW		0x0504

/* Lighting and materials. */
#define GL_LIGHTING			0x0B50
#define GL_LIGHT_MODEL_LOCAL_VIEWER	0x0B51
#define GL_LIGHT_MODEL_TWO_SIDE		0x0B52
#define GL_LIGHT_MODEL_AMBIENT		0x0B53
#define GL_SHADE_MODEL			0x0B54
#define GL_COLOR_MATERIAL		0x0B57
#define GL_NORMALIZE			0x0BA1
#define GL_RESCALE_NORMAL		0x803A
#define GL_MAX_LIGHTS			0x0D31
#define GL_LIGHT0			0x4000
#define GL_LIGHT1			0x4001
#define GL_LIGHT2			0x4002
#define GL_LIGHT3			0x4003
#define GL_LIGHT4			0x4004
#define GL_LIGHT5			0x4005
#define GL_LIGHT6			0x4006
#define GL_LIGHT7			0x4007
#define GL_AMBIENT			0x1200
#define GL_DIFFUSE			0x1201
#define GL_SPECULAR			0x1202
#define GL_POSITION			0x1203
#define GL_SPOT_DIRECTION		0x1204
#define GL_SPOT_EXPONENT		0x1205
#define GL_SPOT_CUTOFF			0x1206
#define GL_CONSTANT_ATTENUATION		0x1207
#define GL_LINEAR_ATTENUATION		0x1208
#define GL_QUADRATIC_ATTENUATION	0x1209
#define GL_EMISSION			0x1600
#define GL_SHININESS			0x1601
#define GL_AMBIENT_AND_DIFFUSE		0x1602
#define GL_FLAT				0x1D00
#define GL_SMOOTH			0x1D01

/* The current vertex values. */
#define GL_CURRENT_COLOR		0x0B00
#define GL_CURRENT_NORMAL		0x0B02
#define GL_CURRENT_TEXTURE_COORDS	0x0B03

/* Display lists. */
#define GL_COMPILE			0x1300
#define GL_COMPILE_AND_EXECUTE		0x1301
#define GL_LIST_MODE			0x0B30
#define GL_MAX_LIST_NESTING		0x0B31
#define GL_LIST_INDEX			0x0B33

/* Vertex arrays. */
#define GL_VERTEX_ARRAY			0x8074
#define GL_NORMAL_ARRAY			0x8075
#define GL_COLOR_ARRAY			0x8076
#define GL_TEXTURE_COORD_ARRAY		0x8078
#define GL_DOUBLE			0x140A

/* Capabilities and their states. */
#define GL_POINT_SMOOTH			0x0B10
#define GL_POINT_SIZE			0x0B11
#define GL_LINE_SMOOTH			0x0B20
#define GL_LINE_STIPPLE			0x0B24
#define GL_POLYGON_SMOOTH		0x0B41
#define GL_POLYGON_STIPPLE		0x0B42
#define GL_FOG				0x0B60
#define GL_ALPHA_TEST			0x0BC0
#define GL_ALPHA_TEST_FUNC		0x0BC1
#define GL_ALPHA_TEST_REF		0x0BC2
#define GL_AUTO_NORMAL			0x0D80
#define GL_TEXTURE_1D			0x0DE0

/* Polygon modes and buffers. */
#define GL_POINT			0x1B00
#define GL_LINE				0x1B01
#define GL_FILL				0x1B02
#define GL_FRONT_LEFT			0x0400
#define GL_BACK_LEFT			0x0402

/* The texture environment. */
#define GL_TEXTURE_ENV			0x2300
#define GL_TEXTURE_ENV_MODE		0x2200
#define GL_MODULATE			0x2100
#define GL_DECAL			0x2101
#define GL_CLAMP			0x2900

/* Formats. */
#define GL_BGR				0x80E0
#define GL_BGRA				0x80E1
#define GL_RGB8				0x8051
#define GL_RGBA8			0x8058

/* glPushAttrib's groups. */
#define GL_CURRENT_BIT			0x00000001
#define GL_LIGHTING_BIT			0x00000040
#define GL_TRANSFORM_BIT		0x00001000
#define GL_ENABLE_BIT			0x00002000
#define GL_ALL_ATTRIB_BITS		0x000FFFFF

/* Matrices. */
GLAPI void APIENTRY glMatrixMode(GLenum mode);
GLAPI void APIENTRY glLoadIdentity(void);
GLAPI void APIENTRY glLoadMatrixf(const GLfloat *m);
GLAPI void APIENTRY glLoadMatrixd(const GLdouble *m);
GLAPI void APIENTRY glMultMatrixf(const GLfloat *m);
GLAPI void APIENTRY glMultMatrixd(const GLdouble *m);
GLAPI void APIENTRY glTranslatef(GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glTranslated(GLdouble x, GLdouble y, GLdouble z);
GLAPI void APIENTRY glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z);
GLAPI void APIENTRY glScalef(GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glScaled(GLdouble x, GLdouble y, GLdouble z);
GLAPI void APIENTRY glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
GLAPI void APIENTRY glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
GLAPI void APIENTRY glPushMatrix(void);
GLAPI void APIENTRY glPopMatrix(void);

/* Immediate mode. */
GLAPI void APIENTRY glBegin(GLenum mode);
GLAPI void APIENTRY glEnd(void);
GLAPI void APIENTRY glVertex2f(GLfloat x, GLfloat y);
GLAPI void APIENTRY glVertex2fv(const GLfloat *v);
GLAPI void APIENTRY glVertex2d(GLdouble x, GLdouble y);
GLAPI void APIENTRY glVertex2i(GLint x, GLint y);
GLAPI void APIENTRY glVertex2s(GLshort x, GLshort y);
GLAPI void APIENTRY glVertex3f(GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glVertex3fv(const GLfloat *v);
GLAPI void APIENTRY glVertex3d(GLdouble x, GLdouble y, GLdouble z);
GLAPI void APIENTRY glVertex3dv(const GLdouble *v);
GLAPI void APIENTRY glVertex3i(GLint x, GLint y, GLint z);
GLAPI void APIENTRY glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
GLAPI void APIENTRY glVertex4fv(const GLfloat *v);
GLAPI void APIENTRY glColor3f(GLfloat red, GLfloat green, GLfloat blue);
GLAPI void APIENTRY glColor3fv(const GLfloat *v);
GLAPI void APIENTRY glColor3d(GLdouble red, GLdouble green, GLdouble blue);
GLAPI void APIENTRY glColor3ub(GLubyte red, GLubyte green, GLubyte blue);
GLAPI void APIENTRY glColor3ubv(const GLubyte *v);
GLAPI void APIENTRY glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
GLAPI void APIENTRY glColor4fv(const GLfloat *v);
GLAPI void APIENTRY glColor4d(GLdouble red, GLdouble green, GLdouble blue, GLdouble alpha);
GLAPI void APIENTRY glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
GLAPI void APIENTRY glColor4ubv(const GLubyte *v);
GLAPI void APIENTRY glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);
GLAPI void APIENTRY glNormal3fv(const GLfloat *v);
GLAPI void APIENTRY glNormal3d(GLdouble nx, GLdouble ny, GLdouble nz);
GLAPI void APIENTRY glTexCoord1f(GLfloat s);
GLAPI void APIENTRY glTexCoord2f(GLfloat s, GLfloat t);
GLAPI void APIENTRY glTexCoord2fv(const GLfloat *v);
GLAPI void APIENTRY glTexCoord3f(GLfloat s, GLfloat t, GLfloat r);
GLAPI void APIENTRY glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q);
GLAPI void APIENTRY glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2);

/* Vertex arrays. */
GLAPI void APIENTRY glEnableClientState(GLenum array);
GLAPI void APIENTRY glDisableClientState(GLenum array);
GLAPI void APIENTRY glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *pointer);
GLAPI void APIENTRY glColorPointer(GLint size, GLenum type, GLsizei stride, const void *pointer);
GLAPI void APIENTRY glNormalPointer(GLenum type, GLsizei stride, const void *pointer);
GLAPI void APIENTRY glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *pointer);

/* Lighting and materials. */
GLAPI void APIENTRY glShadeModel(GLenum mode);
GLAPI void APIENTRY glLightf(GLenum light, GLenum pname, GLfloat param);
GLAPI void APIENTRY glLightfv(GLenum light, GLenum pname, const GLfloat *params);
GLAPI void APIENTRY glLighti(GLenum light, GLenum pname, GLint param);
GLAPI void APIENTRY glLightiv(GLenum light, GLenum pname, const GLint *params);
GLAPI void APIENTRY glLightModelf(GLenum pname, GLfloat param);
GLAPI void APIENTRY glLightModelfv(GLenum pname, const GLfloat *params);
GLAPI void APIENTRY glLightModeli(GLenum pname, GLint param);
GLAPI void APIENTRY glLightModeliv(GLenum pname, const GLint *params);
GLAPI void APIENTRY glMaterialf(GLenum face, GLenum pname, GLfloat param);
GLAPI void APIENTRY glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
GLAPI void APIENTRY glMateriali(GLenum face, GLenum pname, GLint param);
GLAPI void APIENTRY glMaterialiv(GLenum face, GLenum pname, const GLint *params);
GLAPI void APIENTRY glColorMaterial(GLenum face, GLenum mode);

/* Display lists. */
GLAPI GLuint APIENTRY glGenLists(GLsizei range);
GLAPI void APIENTRY glNewList(GLuint list, GLenum mode);
GLAPI void APIENTRY glEndList(void);
GLAPI void APIENTRY glCallList(GLuint list);
GLAPI void APIENTRY glCallLists(GLsizei n, GLenum type, const void *lists);
GLAPI void APIENTRY glDeleteLists(GLuint list, GLsizei range);
GLAPI GLboolean APIENTRY glIsList(GLuint list);

/* The rest of the fixed-function state. */
GLAPI void APIENTRY glAlphaFunc(GLenum func, GLfloat ref);
GLAPI void APIENTRY glTexEnvf(GLenum target, GLenum pname, GLfloat param);
GLAPI void APIENTRY glTexEnvi(GLenum target, GLenum pname, GLint param);
GLAPI void APIENTRY glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params);
GLAPI void APIENTRY glTexEnviv(GLenum target, GLenum pname, const GLint *params);
GLAPI void APIENTRY glPointSize(GLfloat size);
GLAPI void APIENTRY glClearDepth(GLclampd depth);
GLAPI void APIENTRY glDepthRange(GLclampd zNear, GLclampd zFar);
GLAPI void APIENTRY glPolygonMode(GLenum face, GLenum mode);
GLAPI void APIENTRY glDrawBuffer(GLenum mode);
GLAPI void APIENTRY glReadBuffer(GLenum mode);
GLAPI void APIENTRY glPushAttrib(GLbitfield mask);
GLAPI void APIENTRY glPopAttrib(void);
GLAPI void APIENTRY glGetDoublev(GLenum pname, GLdouble *params);

#ifdef __cplusplus
}
#endif

#endif
