/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's desktop OpenGL header (WS069): libGL.so has the OpenGL ES 3.0
 * functions (their declarations and types come from GLES3/gl3.h), the
 * fixed-function part of OpenGL 1.x (WS069 p005) and the calls of
 * OpenGL 3.0 that OpenGL ES 3.0 does not have (WS068 p013) declared here.
 */

#ifndef LIBC_GL_GL_H
#define LIBC_GL_GL_H

#include <GLES3/gl3.h>

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
#define GL_LEFT				0x0406

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

/* OpenGL 1.5 to 3.0's names that OpenGL ES 3.0 does not have. */
#define GL_SAMPLES_PASSED		0x8914
#define GL_QUERY_COUNTER_BITS		0x8864
#define GL_READ_ONLY			0x88B8
#define GL_WRITE_ONLY			0x88B9
#define GL_READ_WRITE			0x88BA
#define GL_BUFFER_ACCESS		0x88BB
#define GL_TEXTURE_BORDER_COLOR		0x1004
#define GL_CLAMP_TO_BORDER		0x812D
#define GL_CONTEXT_FLAGS		0x821E
#define GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT 0x00000001
#define GL_CLAMP_VERTEX_COLOR		0x891A
#define GL_CLAMP_FRAGMENT_COLOR		0x891B
#define GL_CLAMP_READ_COLOR		0x891C
#define GL_FIXED_ONLY			0x891D
#define GL_QUERY_WAIT			0x8E13
#define GL_QUERY_NO_WAIT		0x8E14
#define GL_QUERY_BY_REGION_WAIT		0x8E15
#define GL_QUERY_BY_REGION_NO_WAIT	0x8E16
#define GL_PRIMITIVES_GENERATED		0x8C87

/* OpenGL 3.1 and 3.2's names that OpenGL ES 3.0 does not have. */
#define GL_TEXTURE_RECTANGLE		0x84F5
#define GL_TEXTURE_BINDING_RECTANGLE	0x84F6
#define GL_MAX_RECTANGLE_TEXTURE_SIZE	0x84F8
#define GL_SAMPLER_2D_RECT		0x8B63
#define GL_SAMPLER_2D_RECT_SHADOW	0x8B64
#define GL_INT_SAMPLER_2D_RECT		0x8DCD
#define GL_UNSIGNED_INT_SAMPLER_2D_RECT	0x8DD5
#define GL_TEXTURE_BUFFER		0x8C2A
#define GL_MAX_TEXTURE_BUFFER_SIZE	0x8C2B
#define GL_TEXTURE_BINDING_BUFFER	0x8C2C
#define GL_TEXTURE_BUFFER_DATA_STORE_BINDING 0x8C2D
#define GL_SAMPLER_BUFFER		0x8DC2
#define GL_INT_SAMPLER_BUFFER		0x8DD0
#define GL_UNSIGNED_INT_SAMPLER_BUFFER	0x8DD8
#define GL_R16				0x822A
#define GL_RG16				0x822C
#define GL_RGBA16			0x805B
#define GL_PRIMITIVE_RESTART		0x8F9D
#define GL_PRIMITIVE_RESTART_INDEX	0x8F9E
#define GL_DEPTH_CLAMP			0x864F
#define GL_TEXTURE_CUBE_MAP_SEAMLESS	0x884F
#define GL_FIRST_VERTEX_CONVENTION	0x8E4D
#define GL_LAST_VERTEX_CONVENTION	0x8E4E
#define GL_PROVOKING_VERTEX		0x8E4F

/* OpenGL 3.2's and 1.0's level parameters that OpenGL ES 3.0 does not have. */
#define GL_GEOMETRY_SHADER		0x8DD9
#define GL_GEOMETRY_VERTICES_OUT	0x8916
#define GL_GEOMETRY_INPUT_TYPE		0x8917
#define GL_GEOMETRY_OUTPUT_TYPE		0x8918
#define GL_LINES_ADJACENCY		0x000A
#define GL_LINE_STRIP_ADJACENCY		0x000B
#define GL_TRIANGLES_ADJACENCY		0x000C
#define GL_TRIANGLE_STRIP_ADJACENCY	0x000D
#define GL_MAX_GEOMETRY_OUTPUT_VERTICES	0x8DE0
#define GL_FRAMEBUFFER_ATTACHMENT_LAYERED 0x8DA7
#define GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS 0x8DA8
#define GL_SAMPLE_POSITION		0x8E50
#define GL_SAMPLE_MASK			0x8E51
#define GL_SAMPLE_MASK_VALUE		0x8E52
#define GL_MAX_SAMPLE_MASK_WORDS	0x8E59
#define GL_TEXTURE_2D_MULTISAMPLE	0x9100
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE 0x9104
#define GL_TEXTURE_SAMPLES		0x9106
#define GL_TEXTURE_FIXED_SAMPLE_LOCATIONS 0x9107
#define GL_SAMPLER_2D_MULTISAMPLE	0x9108
#define GL_INT_SAMPLER_2D_MULTISAMPLE	0x9109
#define GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE 0x910A
#define GL_MAX_COLOR_TEXTURE_SAMPLES	0x910E
#define GL_MAX_DEPTH_TEXTURE_SAMPLES	0x910F
#define GL_MAX_INTEGER_SAMPLES		0x9110
#define GL_CONTEXT_PROFILE_MASK		0x9126
#define GL_CONTEXT_CORE_PROFILE_BIT	0x00000001
#define GL_CONTEXT_COMPATIBILITY_PROFILE_BIT 0x00000002
#define GL_TEXTURE_WIDTH		0x1000
#define GL_TEXTURE_HEIGHT		0x1001
#define GL_TEXTURE_INTERNAL_FORMAT	0x1003
#define GL_TEXTURE_DEPTH		0x8071

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

/* OpenGL 1.5 to 3.0's calls that OpenGL ES 3.0 does not have. */
GLAPI void *APIENTRY glMapBuffer(GLenum target, GLenum access);
GLAPI void APIENTRY glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void *data);
GLAPI void APIENTRY glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params);
GLAPI void APIENTRY glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void *pixels);
GLAPI void APIENTRY glColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a);
GLAPI void APIENTRY glEnablei(GLenum target, GLuint index);
GLAPI void APIENTRY glDisablei(GLenum target, GLuint index);
GLAPI GLboolean APIENTRY glIsEnabledi(GLenum target, GLuint index);
GLAPI void APIENTRY glGetBooleani_v(GLenum target, GLuint index, GLboolean *data);
GLAPI void APIENTRY glBindFragDataLocation(GLuint program, GLuint color, const GLchar *name);
GLAPI void APIENTRY glBeginConditionalRender(GLuint id, GLenum mode);
GLAPI void APIENTRY glEndConditionalRender(void);
GLAPI void APIENTRY glClampColor(GLenum target, GLenum clamp);
GLAPI void APIENTRY glTexParameterIiv(GLenum target, GLenum pname, const GLint *params);
GLAPI void APIENTRY glTexParameterIuiv(GLenum target, GLenum pname, const GLuint *params);
GLAPI void APIENTRY glGetTexParameterIiv(GLenum target, GLenum pname, GLint *params);
GLAPI void APIENTRY glGetTexParameterIuiv(GLenum target, GLenum pname, GLuint *params);
GLAPI void APIENTRY glFramebufferTexture1D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
GLAPI void APIENTRY glFramebufferTexture3D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLint zoffset);
GLAPI void APIENTRY glVertexAttribI1i(GLuint index, GLint x);
GLAPI void APIENTRY glVertexAttribI2i(GLuint index, GLint x, GLint y);
GLAPI void APIENTRY glVertexAttribI3i(GLuint index, GLint x, GLint y, GLint z);
GLAPI void APIENTRY glVertexAttribI1ui(GLuint index, GLuint x);
GLAPI void APIENTRY glVertexAttribI2ui(GLuint index, GLuint x, GLuint y);
GLAPI void APIENTRY glVertexAttribI3ui(GLuint index, GLuint x, GLuint y, GLuint z);
GLAPI void APIENTRY glVertexAttribI1iv(GLuint index, const GLint *v);
GLAPI void APIENTRY glVertexAttribI2iv(GLuint index, const GLint *v);
GLAPI void APIENTRY glVertexAttribI3iv(GLuint index, const GLint *v);
GLAPI void APIENTRY glVertexAttribI1uiv(GLuint index, const GLuint *v);
GLAPI void APIENTRY glVertexAttribI2uiv(GLuint index, const GLuint *v);
GLAPI void APIENTRY glVertexAttribI3uiv(GLuint index, const GLuint *v);
GLAPI void APIENTRY glVertexAttribI4bv(GLuint index, const GLbyte *v);
GLAPI void APIENTRY glVertexAttribI4sv(GLuint index, const GLshort *v);
GLAPI void APIENTRY glVertexAttribI4ubv(GLuint index, const GLubyte *v);
GLAPI void APIENTRY glVertexAttribI4usv(GLuint index, const GLushort *v);

/* OpenGL 3.1 and 3.2's calls that OpenGL ES 3.0 does not have. */
GLAPI void APIENTRY glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer);
GLAPI void APIENTRY glPrimitiveRestartIndex(GLuint index);
GLAPI void APIENTRY glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei *length, GLchar *uniformName);
GLAPI void APIENTRY glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex);
GLAPI void APIENTRY glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint basevertex);
GLAPI void APIENTRY glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount, GLint basevertex);
GLAPI void APIENTRY glProvokingVertex(GLenum mode);

/* OpenGL 3.2's calls that OpenGL ES 3.0 does not have, and 1.0's level parameters. */
GLAPI void APIENTRY glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level);
GLAPI void APIENTRY glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations);
GLAPI void APIENTRY glTexStorage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations);
GLAPI void APIENTRY glSampleMaski(GLuint maskNumber, GLbitfield mask);
GLAPI void APIENTRY glGetMultisamplefv(GLenum pname, GLuint index, GLfloat *val);
GLAPI void APIENTRY glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
GLAPI void APIENTRY glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params);

/* OpenGL 3.3's timer queries, dual-source blending and packed current attribute values. */
#define GL_TIME_ELAPSED			0x88BF
#define GL_TIMESTAMP			0x8E28
#define GL_QUERY_COUNTER_BITS		0x8864
#define GL_SRC1_COLOR			0x88F9
#define GL_SRC1_ALPHA			0x8589
#define GL_ONE_MINUS_SRC1_COLOR		0x88FA
#define GL_ONE_MINUS_SRC1_ALPHA		0x88FB
#define GL_MAX_DUAL_SOURCE_DRAW_BUFFERS	0x88FC
GLAPI void APIENTRY glQueryCounter(GLuint id, GLenum target);
GLAPI void APIENTRY glGetQueryObjecti64v(GLuint id, GLenum pname, GLint64 *params);
GLAPI void APIENTRY glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64 *params);
GLAPI void APIENTRY glBindFragDataLocationIndexed(GLuint program, GLuint colorNumber, GLuint index, const GLchar *name);
GLAPI GLint APIENTRY glGetFragDataIndex(GLuint program, const GLchar *name);
GLAPI void APIENTRY glVertexAttribP1ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
GLAPI void APIENTRY glVertexAttribP2ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
GLAPI void APIENTRY glVertexAttribP3ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
GLAPI void APIENTRY glVertexAttribP4ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
GLAPI void APIENTRY glVertexAttribP1uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint *value);
GLAPI void APIENTRY glVertexAttribP2uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint *value);
GLAPI void APIENTRY glVertexAttribP3uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint *value);
GLAPI void APIENTRY glVertexAttribP4uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint *value);

/* OpenGL 4.0's tessellation (GL_ARB_tessellation_shader on 3.2 and 3.3 contexts of a device that has it). */
#define GL_PATCHES			0x000E
#define GL_PATCH_VERTICES		0x8E72
#define GL_PATCH_DEFAULT_INNER_LEVEL	0x8E73
#define GL_PATCH_DEFAULT_OUTER_LEVEL	0x8E74
#define GL_TESS_CONTROL_OUTPUT_VERTICES	0x8E75
#define GL_TESS_GEN_MODE		0x8E76
#define GL_TESS_GEN_SPACING		0x8E77
#define GL_TESS_GEN_VERTEX_ORDER	0x8E78
#define GL_TESS_GEN_POINT_MODE		0x8E79
#define GL_ISOLINES			0x8E7A
#define GL_FRACTIONAL_ODD		0x8E7B
#define GL_FRACTIONAL_EVEN		0x8E7C
#define GL_MAX_PATCH_VERTICES		0x8E7D
#define GL_MAX_TESS_GEN_LEVEL		0x8E7E
#define GL_TESS_EVALUATION_SHADER	0x8E87
#define GL_TESS_CONTROL_SHADER		0x8E88
GLAPI void APIENTRY glPatchParameteri(GLenum pname, GLint value);
GLAPI void APIENTRY glPatchParameterfv(GLenum pname, const GLfloat *values);

/* OpenGL 4.3's compute shaders and shader storage buffers (contexts of 4.3 on; the constants are OpenGL ES 3.1's). */
#define GL_COMPUTE_SHADER		0x91B9
#define GL_SHADER_STORAGE_BUFFER	0x90D2
#define GL_SHADER_STORAGE_BUFFER_BINDING 0x90D3
#define GL_DISPATCH_INDIRECT_BUFFER	0x90EE
#define GL_COMPUTE_WORK_GROUP_SIZE	0x8267
#define GL_MAX_COMPUTE_WORK_GROUP_COUNT	0x91BE
#define GL_MAX_COMPUTE_WORK_GROUP_SIZE	0x91BF
#define GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS 0x90EB
#define GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS 0x90DD
#define GL_SHADER_STORAGE_BARRIER_BIT	0x00002000
#define GL_ALL_BARRIER_BITS		0xFFFFFFFF
GLAPI void APIENTRY glDispatchCompute(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z);
GLAPI void APIENTRY glDispatchComputeIndirect(GLintptr indirect);
GLAPI void APIENTRY glMemoryBarrier(GLbitfield barriers);

/* OpenGL 4.0's indirect draws (GL_ARB_draw_indirect on 3.1 to 3.3 contexts). */
#define GL_DRAW_INDIRECT_BUFFER		0x8F3F
#define GL_DRAW_INDIRECT_BUFFER_BINDING	0x8F43
GLAPI void APIENTRY glDrawArraysIndirect(GLenum mode, const void *indirect);
GLAPI void APIENTRY glDrawElementsIndirect(GLenum mode, GLenum type, const void *indirect);

/* OpenGL 4.3's debug output (GL_KHR_debug on every 3.x context). */
typedef void (APIENTRY *GLDEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *userParam);
#define GL_DEBUG_OUTPUT_SYNCHRONOUS	0x8242
#define GL_DEBUG_NEXT_LOGGED_MESSAGE_LENGTH 0x8243
#define GL_DEBUG_CALLBACK_FUNCTION	0x8244
#define GL_DEBUG_CALLBACK_USER_PARAM	0x8245
#define GL_DEBUG_SOURCE_API		0x8246
#define GL_DEBUG_SOURCE_WINDOW_SYSTEM	0x8247
#define GL_DEBUG_SOURCE_SHADER_COMPILER	0x8248
#define GL_DEBUG_SOURCE_THIRD_PARTY	0x8249
#define GL_DEBUG_SOURCE_APPLICATION	0x824A
#define GL_DEBUG_SOURCE_OTHER		0x824B
#define GL_DEBUG_TYPE_ERROR		0x824C
#define GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR 0x824D
#define GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR 0x824E
#define GL_DEBUG_TYPE_PORTABILITY	0x824F
#define GL_DEBUG_TYPE_PERFORMANCE	0x8250
#define GL_DEBUG_TYPE_OTHER		0x8251
#define GL_DEBUG_TYPE_MARKER		0x8268
#define GL_DEBUG_TYPE_PUSH_GROUP	0x8269
#define GL_DEBUG_TYPE_POP_GROUP		0x826A
#define GL_DEBUG_SEVERITY_NOTIFICATION	0x826B
#define GL_MAX_DEBUG_GROUP_STACK_DEPTH	0x826C
#define GL_DEBUG_GROUP_STACK_DEPTH	0x826D
#define GL_BUFFER			0x82E0
#define GL_SHADER			0x82E1
#define GL_PROGRAM			0x82E2
#define GL_QUERY			0x82E3
#define GL_PROGRAM_PIPELINE		0x82E4
#define GL_SAMPLER			0x82E6
#define GL_MAX_LABEL_LENGTH		0x82E8
#define GL_MAX_DEBUG_MESSAGE_LENGTH	0x9143
#define GL_MAX_DEBUG_LOGGED_MESSAGES	0x9144
#define GL_DEBUG_LOGGED_MESSAGES	0x9145
#define GL_DEBUG_SEVERITY_HIGH		0x9146
#define GL_DEBUG_SEVERITY_MEDIUM	0x9147
#define GL_DEBUG_SEVERITY_LOW		0x9148
#define GL_DEBUG_OUTPUT			0x92E0
#define GL_CONTEXT_FLAG_DEBUG_BIT	0x00000002
GLAPI void APIENTRY glDebugMessageControl(GLenum source, GLenum type, GLenum severity, GLsizei count, const GLuint *ids, GLboolean enabled);
GLAPI void APIENTRY glDebugMessageInsert(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *buf);
GLAPI void APIENTRY glDebugMessageCallback(GLDEBUGPROC callback, const void *userParam);
GLAPI GLuint APIENTRY glGetDebugMessageLog(GLuint count, GLsizei bufSize, GLenum *sources, GLenum *types, GLuint *ids, GLenum *severities, GLsizei *lengths, GLchar *messageLog);
GLAPI void APIENTRY glPushDebugGroup(GLenum source, GLuint id, GLsizei length, const GLchar *message);
GLAPI void APIENTRY glPopDebugGroup(void);
GLAPI void APIENTRY glObjectLabel(GLenum identifier, GLuint name, GLsizei length, const GLchar *label);
GLAPI void APIENTRY glGetObjectLabel(GLenum identifier, GLuint name, GLsizei bufSize, GLsizei *length, GLchar *label);
GLAPI void APIENTRY glObjectPtrLabel(const void *ptr, GLsizei length, const GLchar *label);
GLAPI void APIENTRY glGetObjectPtrLabel(const void *ptr, GLsizei bufSize, GLsizei *length, GLchar *label);
GLAPI void APIENTRY glGetPointerv(GLenum pname, void **params);

/*
 * OpenGL 4.0's transform feedback objects drawn, indexed queries and the
 * buffers interleaved names write (GL_ARB_transform_feedback2 and 3 on
 * every 3.x context), and 4.2's instanced draws of them.
 */
#define GL_MAX_TRANSFORM_FEEDBACK_BUFFERS 0x8E70
#define GL_MAX_VERTEX_STREAMS		0x8E71
GLAPI void APIENTRY glDrawTransformFeedback(GLenum mode, GLuint id);
GLAPI void APIENTRY glDrawTransformFeedbackStream(GLenum mode, GLuint id, GLuint stream);
GLAPI void APIENTRY glDrawTransformFeedbackInstanced(GLenum mode, GLuint id, GLsizei instancecount);
GLAPI void APIENTRY glDrawTransformFeedbackStreamInstanced(GLenum mode, GLuint id, GLuint stream, GLsizei instancecount);
GLAPI void APIENTRY glBeginQueryIndexed(GLenum target, GLuint index, GLuint id);
GLAPI void APIENTRY glEndQueryIndexed(GLenum target, GLuint index);
GLAPI void APIENTRY glGetQueryIndexediv(GLenum target, GLuint index, GLenum pname, GLint *params);

/* OpenGL 4.0's subroutines (GL_ARB_shader_subroutine on 3.2 and 3.3 contexts). */
#define GL_ACTIVE_SUBROUTINES		0x8DE5
#define GL_ACTIVE_SUBROUTINE_UNIFORMS	0x8DE6
#define GL_MAX_SUBROUTINES		0x8DE7
#define GL_MAX_SUBROUTINE_UNIFORM_LOCATIONS 0x8DE8
#define GL_ACTIVE_SUBROUTINE_UNIFORM_LOCATIONS 0x8E47
#define GL_ACTIVE_SUBROUTINE_MAX_LENGTH	0x8E48
#define GL_ACTIVE_SUBROUTINE_UNIFORM_MAX_LENGTH 0x8E49
#define GL_NUM_COMPATIBLE_SUBROUTINES	0x8E4A
#define GL_COMPATIBLE_SUBROUTINES	0x8E4B
GLAPI GLint APIENTRY glGetSubroutineUniformLocation(GLuint program, GLenum shadertype, const GLchar *name);
GLAPI GLuint APIENTRY glGetSubroutineIndex(GLuint program, GLenum shadertype, const GLchar *name);
GLAPI void APIENTRY glGetActiveSubroutineUniformiv(GLuint program, GLenum shadertype, GLuint index, GLenum pname, GLint *values);
GLAPI void APIENTRY glGetActiveSubroutineUniformName(GLuint program, GLenum shadertype, GLuint index, GLsizei bufSize, GLsizei *length, GLchar *name);
GLAPI void APIENTRY glGetActiveSubroutineName(GLuint program, GLenum shadertype, GLuint index, GLsizei bufSize, GLsizei *length, GLchar *name);
GLAPI void APIENTRY glUniformSubroutinesuiv(GLenum shadertype, GLsizei count, const GLuint *indices);
GLAPI void APIENTRY glGetUniformSubroutineuiv(GLenum shadertype, GLint location, GLuint *params);
GLAPI void APIENTRY glGetProgramStageiv(GLuint program, GLenum shadertype, GLenum pname, GLint *values);

/* OpenGL 4.0's per-sample shading (GL_ARB_sample_shading on a device that shades per sample). */
#define GL_SAMPLE_SHADING		0x8C36
#define GL_MIN_SAMPLE_SHADING_VALUE	0x8C37
GLAPI void APIENTRY glMinSampleShading(GLfloat value);
GLAPI void APIENTRY glMinSampleShadingARB(GLfloat value);

/* OpenGL 4.0's blending of each draw buffer (GL_ARB_draw_buffers_blend on a 3.x context of a device that has it). */
GLAPI void APIENTRY glBlendFunci(GLuint buf, GLenum src, GLenum dst);
GLAPI void APIENTRY glBlendFuncSeparatei(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
GLAPI void APIENTRY glBlendEquationi(GLuint buf, GLenum mode);
GLAPI void APIENTRY glBlendEquationSeparatei(GLuint buf, GLenum modeRGB, GLenum modeAlpha);
GLAPI void APIENTRY glBlendFunciARB(GLuint buf, GLenum src, GLenum dst);
GLAPI void APIENTRY glBlendFuncSeparateiARB(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
GLAPI void APIENTRY glBlendEquationiARB(GLuint buf, GLenum mode);
GLAPI void APIENTRY glBlendEquationSeparateiARB(GLuint buf, GLenum modeRGB, GLenum modeAlpha);

/* OpenGL 4.0's doubles in shaders (GL_ARB_gpu_shader_fp64 on a 3.2 or 3.3 context of a device that has them). */
#define GL_DOUBLE_VEC2		0x8FFC
#define GL_DOUBLE_VEC3		0x8FFD
#define GL_DOUBLE_VEC4		0x8FFE
#define GL_DOUBLE_MAT2		0x8F46
#define GL_DOUBLE_MAT3		0x8F47
#define GL_DOUBLE_MAT4		0x8F48
#define GL_DOUBLE_MAT2x3	0x8F49
#define GL_DOUBLE_MAT2x4	0x8F4A
#define GL_DOUBLE_MAT3x2	0x8F4B
#define GL_DOUBLE_MAT3x4	0x8F4C
#define GL_DOUBLE_MAT4x2	0x8F4D
#define GL_DOUBLE_MAT4x3	0x8F4E
GLAPI void APIENTRY glUniform1d(GLint location, GLdouble x);
GLAPI void APIENTRY glUniform2d(GLint location, GLdouble x, GLdouble y);
GLAPI void APIENTRY glUniform3d(GLint location, GLdouble x, GLdouble y, GLdouble z);
GLAPI void APIENTRY glUniform4d(GLint location, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
GLAPI void APIENTRY glUniform1dv(GLint location, GLsizei count, const GLdouble *value);
GLAPI void APIENTRY glUniform2dv(GLint location, GLsizei count, const GLdouble *value);
GLAPI void APIENTRY glUniform3dv(GLint location, GLsizei count, const GLdouble *value);
GLAPI void APIENTRY glUniform4dv(GLint location, GLsizei count, const GLdouble *value);
GLAPI void APIENTRY glUniformMatrix2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble *value);
GLAPI void APIENTRY glUniformMatrix3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble *value);
GLAPI void APIENTRY glUniformMatrix4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble *value);
GLAPI void APIENTRY glUniformMatrix2x3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble *value);
GLAPI void APIENTRY glUniformMatrix2x4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble *value);
GLAPI void APIENTRY glUniformMatrix3x2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble *value);
GLAPI void APIENTRY glUniformMatrix3x4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble *value);
GLAPI void APIENTRY glUniformMatrix4x2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble *value);
GLAPI void APIENTRY glUniformMatrix4x3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble *value);
GLAPI void APIENTRY glGetUniformdv(GLuint program, GLint location, GLdouble *params);

/* OpenGL 4.0's texture gathers (GL_ARB_texture_gather on every 3.x context). */
#define GL_MIN_PROGRAM_TEXTURE_GATHER_OFFSET 0x8E5E
#define GL_MAX_PROGRAM_TEXTURE_GATHER_OFFSET 0x8E5F
#define GL_MAX_PROGRAM_TEXTURE_GATHER_COMPONENTS_ARB 0x8F9F

/* OpenGL 4.0's cube map arrays (GL_ARB_texture_cube_map_array on a 3.x context of a device that has them). */
#define GL_TEXTURE_CUBE_MAP_ARRAY	0x9009
#define GL_TEXTURE_BINDING_CUBE_MAP_ARRAY 0x900A
#define GL_PROXY_TEXTURE_CUBE_MAP_ARRAY	0x900B
#define GL_SAMPLER_CUBE_MAP_ARRAY	0x900C
#define GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW 0x900D
#define GL_INT_SAMPLER_CUBE_MAP_ARRAY	0x900E
#define GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY 0x900F

/* OpenGL 4.3's vertex attribute binding (GL_ARB_vertex_attrib_binding on every 3.x context). */
#define GL_VERTEX_ATTRIB_BINDING	0x82D4
#define GL_VERTEX_ATTRIB_RELATIVE_OFFSET 0x82D5
#define GL_VERTEX_BINDING_DIVISOR	0x82D6
#define GL_VERTEX_BINDING_OFFSET	0x82D7
#define GL_VERTEX_BINDING_STRIDE	0x82D8
#define GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET 0x82D9
#define GL_MAX_VERTEX_ATTRIB_BINDINGS	0x82DA
#define GL_VERTEX_BINDING_BUFFER	0x8F4F
GLAPI void APIENTRY glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);
GLAPI void APIENTRY glVertexAttribFormat(GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset);
GLAPI void APIENTRY glVertexAttribIFormat(GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset);
GLAPI void APIENTRY glVertexAttribBinding(GLuint attribindex, GLuint bindingindex);
GLAPI void APIENTRY glVertexBindingDivisor(GLuint bindingindex, GLuint divisor);

/* OpenGL 4.5's clip control (GL_ARB_clip_control on every 3.x context). */
#define GL_LOWER_LEFT			0x8CA1
#define GL_UPPER_LEFT			0x8CA2
#define GL_CLIP_ORIGIN			0x935C
#define GL_CLIP_DEPTH_MODE		0x935D
#define GL_NEGATIVE_ONE_TO_ONE		0x935E
#define GL_ZERO_TO_ONE			0x935F
GLAPI void APIENTRY glClipControl(GLenum origin, GLenum depth);

#ifdef __cplusplus
}
#endif

#endif
