/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libGL's fixed-function OpenGL 1.x (WS069 p005): matrices, lights,
 * material, the other fixed-function state, and the draws that use it.
 *
 * The translation (libGLESv2's sources, built into libGL) reaches this
 * layer through its hooks (gles.h): a draw without a program takes one of
 * the two fixed-function programs (shaders/, SPIR-V in shaders.h; flat or
 * smooth) with its uniforms written from this state; glEnable takes the
 * layer's capabilities; glGet reads its state; GL_VERSION is desktop GL's.
 * The vertex colour, normal and texture coordinates are generic
 * attributes 1 to 3 (the position is 0), as GL's compatibility profile
 * aliases them.
 */

#include "fixed.h"
#include "shaders.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int fixed_capability(struct zegl_context *context, GLenum cap, int **flag);
static struct gles_program *fixed_program(struct zegl_context *context, int *flat);
static unsigned fixed_get(struct zegl_context *context, GLenum pname, GLfloat *values);
static const GLubyte *fixed_string(GLenum name);
static void fixed_release(struct gles_state *state);
static struct fixed_state *fixed_new(struct gles_state *state);
static GLuint fixed_make_program(unsigned which, struct fixed_uniforms *uniforms);
static GLuint fixed_shader(GLenum type, const uint32_t *code, size_t size);
static void fixed_identity(GLfloat *matrix);
static void fixed_multiply(GLfloat *result, const GLfloat *a, const GLfloat *b);
static void fixed_apply(const GLfloat *matrix);
static void fixed_normal_matrix(const GLfloat *modelview, GLfloat *normal);
static void fixed_transform(const GLfloat *matrix, const GLfloat *in, GLfloat *out);
static struct fixed_light *fixed_light_of(struct zegl_context *context, struct fixed_state *fixed, GLenum light);
static void fixed_material(struct fixed_state *fixed, GLenum pname, const GLfloat *params);

/* The hooks the translation calls. */
static const struct gles_fixed_hooks fixed_hooks = {
	fixed_capability,
	fixed_program,
	fixed_get,
	fixed_string,
	fixed_release
};

/*
 * Returns the calling thread's context (in *context) and its
 * fixed-function state, making the state at the first call; NULL without
 * a context.
 */
struct fixed_state *
fixed_current(
	struct zegl_context **context)
{
	struct gles_state *state;

	/* The context and its translation's state. */
	*context = gles_context();
	state = gles_state(*context);
	if (state == NULL)
		return NULL;

	/* The fixed-function state, made once. */
	if (state->fixed == NULL)
		state->fixed = fixed_new(state);
	return state->fixed;
}

/*
 * Hands the translation this layer's hooks (before libGL's first context).
 */
void
fixed_install(void)
{
	/* The translation draws without a program through them from now on. */
	gles_fixed = &fixed_hooks;
}

/*
 * Returns the top of the current matrix stack.
 */
GLfloat *
fixed_matrix(
	struct fixed_state *fixed)
{
	/* The stack of the matrix mode. */
	switch (fixed->matrix_mode) {
	case GL_PROJECTION:
		return fixed->projection[fixed->projection_top];
	case GL_TEXTURE:
		return fixed->texture[fixed->texture_top];
	default:
		break;
	}

	/* GL_MODELVIEW. */
	return fixed->modelview[fixed->modelview_top];
}

/*
 * Selects the matrix stack the matrix calls work on.
 */
GL_APICALL void GL_APIENTRY
glMatrixMode(
	GLenum mode)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	recorded = fixed_record(fixed, FIXED_OP_MATRIX_MODE, mode, 0U, NULL, 0U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* One of the three stacks. */
	if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Selected. */
	fixed->matrix_mode = mode;
}

/*
 * Makes the current matrix the identity.
 */
GL_APICALL void GL_APIENTRY
glLoadIdentity(void)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	recorded = fixed_record(fixed, FIXED_OP_LOAD_IDENTITY, 0U, 0U, NULL, 0U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* The identity. */
	fixed_identity(fixed_matrix(fixed));
}

/*
 * Replaces the current matrix (column-major).
 */
GL_APICALL void GL_APIENTRY
glLoadMatrixf(
	const GLfloat *m)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	recorded = fixed_record(fixed, FIXED_OP_LOAD_MATRIX, 0U, 0U, m, 16U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* The sixteen values. */
	memcpy(fixed_matrix(fixed), m, 16U * sizeof(GLfloat));
}

/*
 * Replaces the current matrix from doubles.
 */
GL_APICALL void GL_APIENTRY
glLoadMatrixd(
	const GLdouble *m)
{
	GLfloat matrix[16];
	unsigned index;

	/* As floats. */
	for (index = 0U; index < 16U; index++)
		matrix[index] = (GLfloat)m[index];
	glLoadMatrixf(matrix);
}

/*
 * Multiplies the current matrix by one on the right.
 */
GL_APICALL void GL_APIENTRY
glMultMatrixf(
	const GLfloat *m)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	recorded = fixed_record(fixed, FIXED_OP_MULT_MATRIX, 0U, 0U, m, 16U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* Current = current x m. */
	fixed_apply(m);
}

/*
 * Multiplies the current matrix by one of doubles.
 */
GL_APICALL void GL_APIENTRY
glMultMatrixd(
	const GLdouble *m)
{
	GLfloat matrix[16];
	unsigned index;

	/* As floats. */
	for (index = 0U; index < 16U; index++)
		matrix[index] = (GLfloat)m[index];
	glMultMatrixf(matrix);
}

/*
 * Multiplies the current matrix by a translation.
 */
GL_APICALL void GL_APIENTRY
glTranslatef(
	GLfloat x,
	GLfloat y,
	GLfloat z)
{
	GLfloat matrix[16];

	/* The identity with the move in the last column. */
	fixed_identity(matrix);
	matrix[12] = x;
	matrix[13] = y;
	matrix[14] = z;
	glMultMatrixf(matrix);
}

/*
 * Multiplies the current matrix by a translation of doubles.
 */
GL_APICALL void GL_APIENTRY
glTranslated(
	GLdouble x,
	GLdouble y,
	GLdouble z)
{
	/* As floats. */
	glTranslatef((GLfloat)x, (GLfloat)y, (GLfloat)z);
}

/*
 * Multiplies the current matrix by a rotation of angle degrees about an
 * axis.
 */
GL_APICALL void GL_APIENTRY
glRotatef(
	GLfloat angle,
	GLfloat x,
	GLfloat y,
	GLfloat z)
{
	GLfloat matrix[16];
	GLfloat length;
	GLfloat c;
	GLfloat s;
	GLfloat t;

	/* The unit axis (a zero axis rotates about nothing). */
	length = sqrtf(x * x + y * y + z * z);
	if (length == 0.0f)
		return;
	x /= length;
	y /= length;
	z /= length;

	/* The rotation matrix, column-major. */
	c = cosf(angle * 3.14159265358979f / 180.0f);
	s = sinf(angle * 3.14159265358979f / 180.0f);
	t = 1.0f - c;
	fixed_identity(matrix);
	matrix[0] = x * x * t + c;
	matrix[1] = y * x * t + z * s;
	matrix[2] = x * z * t - y * s;
	matrix[4] = x * y * t - z * s;
	matrix[5] = y * y * t + c;
	matrix[6] = y * z * t + x * s;
	matrix[8] = x * z * t + y * s;
	matrix[9] = y * z * t - x * s;
	matrix[10] = z * z * t + c;
	glMultMatrixf(matrix);
}

/*
 * Multiplies the current matrix by a rotation given in doubles.
 */
GL_APICALL void GL_APIENTRY
glRotated(
	GLdouble angle,
	GLdouble x,
	GLdouble y,
	GLdouble z)
{
	/* As floats. */
	glRotatef((GLfloat)angle, (GLfloat)x, (GLfloat)y, (GLfloat)z);
}

/*
 * Multiplies the current matrix by a scale.
 */
GL_APICALL void GL_APIENTRY
glScalef(
	GLfloat x,
	GLfloat y,
	GLfloat z)
{
	GLfloat matrix[16];

	/* The scale on the diagonal. */
	fixed_identity(matrix);
	matrix[0] = x;
	matrix[5] = y;
	matrix[10] = z;
	glMultMatrixf(matrix);
}

/*
 * Multiplies the current matrix by a scale of doubles.
 */
GL_APICALL void GL_APIENTRY
glScaled(
	GLdouble x,
	GLdouble y,
	GLdouble z)
{
	/* As floats. */
	glScalef((GLfloat)x, (GLfloat)y, (GLfloat)z);
}

/*
 * Multiplies the current matrix by a perspective projection.
 */
GL_APICALL void GL_APIENTRY
glFrustum(
	GLdouble left,
	GLdouble right,
	GLdouble bottom,
	GLdouble top,
	GLdouble zNear,
	GLdouble zFar)
{
	GLfloat matrix[16];

	/* A frustum that has a volume. */
	if (zNear <= 0.0 || zFar <= 0.0 || left == right || bottom == top || zNear == zFar) {
		gles_error(gles_context(), GL_INVALID_VALUE);
		return;
	}

	/* glFrustum's matrix. */
	memset(matrix, 0, sizeof(matrix));
	matrix[0] = (GLfloat)(2.0 * zNear / (right - left));
	matrix[5] = (GLfloat)(2.0 * zNear / (top - bottom));
	matrix[8] = (GLfloat)((right + left) / (right - left));
	matrix[9] = (GLfloat)((top + bottom) / (top - bottom));
	matrix[10] = (GLfloat)(-(zFar + zNear) / (zFar - zNear));
	matrix[11] = -1.0f;
	matrix[14] = (GLfloat)(-2.0 * zFar * zNear / (zFar - zNear));
	glMultMatrixf(matrix);
}

/*
 * Multiplies the current matrix by an orthographic projection.
 */
GL_APICALL void GL_APIENTRY
glOrtho(
	GLdouble left,
	GLdouble right,
	GLdouble bottom,
	GLdouble top,
	GLdouble zNear,
	GLdouble zFar)
{
	GLfloat matrix[16];

	/* A box that has a volume. */
	if (left == right || bottom == top || zNear == zFar) {
		gles_error(gles_context(), GL_INVALID_VALUE);
		return;
	}

	/* glOrtho's matrix. */
	fixed_identity(matrix);
	matrix[0] = (GLfloat)(2.0 / (right - left));
	matrix[5] = (GLfloat)(2.0 / (top - bottom));
	matrix[10] = (GLfloat)(-2.0 / (zFar - zNear));
	matrix[12] = (GLfloat)(-(right + left) / (right - left));
	matrix[13] = (GLfloat)(-(top + bottom) / (top - bottom));
	matrix[14] = (GLfloat)(-(zFar + zNear) / (zFar - zNear));
	glMultMatrixf(matrix);
}

/*
 * Pushes a copy of the current matrix on its stack.
 */
GL_APICALL void GL_APIENTRY
glPushMatrix(void)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	GLfloat *top;
	unsigned *index;
	unsigned depth;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	recorded = fixed_record(fixed, FIXED_OP_PUSH_MATRIX, 0U, 0U, NULL, 0U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* The stack's top and depth. */
	top = fixed_matrix(fixed);
	index = &fixed->modelview_top;
	depth = FIXED_MODELVIEW_DEPTH;
	if (fixed->matrix_mode == GL_PROJECTION)
		index = &fixed->projection_top;
	if (fixed->matrix_mode == GL_TEXTURE)
		index = &fixed->texture_top;
	if (fixed->matrix_mode != GL_MODELVIEW)
		depth = FIXED_OTHER_DEPTH;

	/* A full stack overflows. */
	if (*index + 1U >= depth) {
		gles_error(context, GL_STACK_OVERFLOW);
		return;
	}

	/* The copy on top. */
	(*index)++;
	memcpy(fixed_matrix(fixed), top, 16U * sizeof(GLfloat));
}

/*
 * Pops the current matrix stack.
 */
GL_APICALL void GL_APIENTRY
glPopMatrix(void)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	unsigned *index;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	recorded = fixed_record(fixed, FIXED_OP_POP_MATRIX, 0U, 0U, NULL, 0U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* The stack's depth. */
	index = &fixed->modelview_top;
	if (fixed->matrix_mode == GL_PROJECTION)
		index = &fixed->projection_top;
	if (fixed->matrix_mode == GL_TEXTURE)
		index = &fixed->texture_top;

	/* An empty stack underflows. */
	if (*index == 0U) {
		gles_error(context, GL_STACK_UNDERFLOW);
		return;
	}

	/* The one below is current again. */
	(*index)--;
}

/*
 * Sets the shading: GL_FLAT or GL_SMOOTH.
 */
GL_APICALL void GL_APIENTRY
glShadeModel(
	GLenum mode)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	recorded = fixed_record(fixed, FIXED_OP_SHADE_MODEL, mode, 0U, NULL, 0U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* One of the two. */
	if (mode != GL_FLAT && mode != GL_SMOOTH) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Set. */
	fixed->shade_model = mode;
}

/*
 * Sets a parameter of a light from an array; the position (and spot
 * direction) are taken into eye space by the current modelview matrix.
 */
GL_APICALL void GL_APIENTRY
glLightfv(
	GLenum light,
	GLenum pname,
	const GLfloat *params)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	struct fixed_light *target;
	GLfloat values[4];
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	memset(values, 0, sizeof(values));
	memcpy(values, params, sizeof(GLfloat));
	if (pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR || pname == GL_POSITION)
		memcpy(values, params, 4U * sizeof(GLfloat));
	recorded = fixed_record(fixed, FIXED_OP_LIGHT, light, pname, values, 4U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* The light. */
	target = fixed_light_of(context, fixed, light);
	if (target == NULL)
		return;

	/* The parameter. */
	switch (pname) {
	case GL_AMBIENT:
		memcpy(target->ambient, values, sizeof(values));
		return;
	case GL_DIFFUSE:
		memcpy(target->diffuse, values, sizeof(values));
		return;
	case GL_SPECULAR:
		memcpy(target->specular, values, sizeof(values));
		return;
	case GL_POSITION:
		fixed_transform(fixed->modelview[fixed->modelview_top], values, target->position);
		return;
	case GL_CONSTANT_ATTENUATION:
		target->attenuation[0] = values[0];
		return;
	case GL_LINEAR_ATTENUATION:
		target->attenuation[1] = values[0];
		return;
	case GL_QUADRATIC_ATTENUATION:
		target->attenuation[2] = values[0];
		return;
	case GL_SPOT_DIRECTION:
	case GL_SPOT_EXPONENT:
	case GL_SPOT_CUTOFF:
		return;
	default:
		break;
	}

	/* Not a parameter. */
	gles_error(context, GL_INVALID_ENUM);
}

/*
 * Sets a scalar parameter of a light.
 */
GL_APICALL void GL_APIENTRY
glLightf(
	GLenum light,
	GLenum pname,
	GLfloat param)
{
	/* As an array of one. */
	glLightfv(light, pname, &param);
}

/*
 * Sets a parameter of a light from integers (colours map to [-1, 1]).
 */
GL_APICALL void GL_APIENTRY
glLightiv(
	GLenum light,
	GLenum pname,
	const GLint *params)
{
	GLfloat values[4];
	unsigned index;

	/* Colours as fractions of the largest integer, the rest as they are. */
	memset(values, 0, sizeof(values));
	for (index = 0U; index < 4U; index++) {
		if (pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR)
			values[index] = (GLfloat)params[index] / 2147483647.0f;
		else if (pname == GL_POSITION || index == 0U)
			values[index] = (GLfloat)params[index];
	}

	/* As floats. */
	glLightfv(light, pname, values);
}

/*
 * Sets a scalar parameter of a light from an integer.
 */
GL_APICALL void GL_APIENTRY
glLighti(
	GLenum light,
	GLenum pname,
	GLint param)
{
	GLfloat value;

	/* As a float. */
	value = (GLfloat)param;
	glLightfv(light, pname, &value);
}

/*
 * Sets a parameter of the light model: its ambient colour (the viewer and
 * two-sided lighting are taken and not used).
 */
GL_APICALL void GL_APIENTRY
glLightModelfv(
	GLenum pname,
	const GLfloat *params)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	GLfloat values[4];
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	memset(values, 0, sizeof(values));
	memcpy(values, params, sizeof(GLfloat));
	if (pname == GL_LIGHT_MODEL_AMBIENT)
		memcpy(values, params, 4U * sizeof(GLfloat));
	recorded = fixed_record(fixed, FIXED_OP_LIGHT_MODEL, pname, 0U, values, 4U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* The parameter. */
	switch (pname) {
	case GL_LIGHT_MODEL_AMBIENT:
		memcpy(fixed->scene_ambient, values, sizeof(values));
		return;
	case GL_LIGHT_MODEL_LOCAL_VIEWER:
	case GL_LIGHT_MODEL_TWO_SIDE:
		return;
	default:
		break;
	}

	/* Not a parameter. */
	gles_error(context, GL_INVALID_ENUM);
}

/*
 * Sets a scalar parameter of the light model.
 */
GL_APICALL void GL_APIENTRY
glLightModelf(
	GLenum pname,
	GLfloat param)
{
	/* As an array of one. */
	glLightModelfv(pname, &param);
}

/*
 * Sets a scalar parameter of the light model from an integer.
 */
GL_APICALL void GL_APIENTRY
glLightModeli(
	GLenum pname,
	GLint param)
{
	GLfloat value;

	/* As a float. */
	value = (GLfloat)param;
	glLightModelfv(pname, &value);
}

/*
 * Sets a parameter of the light model from integers.
 */
GL_APICALL void GL_APIENTRY
glLightModeliv(
	GLenum pname,
	const GLint *params)
{
	GLfloat values[4];
	unsigned index;

	/* The ambient colour as fractions of the largest integer. */
	memset(values, 0, sizeof(values));
	values[0] = (GLfloat)params[0];
	for (index = 0U; pname == GL_LIGHT_MODEL_AMBIENT && index < 4U; index++)
		values[index] = (GLfloat)params[index] / 2147483647.0f;
	glLightModelfv(pname, values);
}

/*
 * Sets a parameter of the front material (the back one is not kept).
 */
GL_APICALL void GL_APIENTRY
glMaterialfv(
	GLenum face,
	GLenum pname,
	const GLfloat *params)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	GLfloat values[4];
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	memset(values, 0, sizeof(values));
	memcpy(values, params, sizeof(GLfloat));
	if (pname != GL_SHININESS)
		memcpy(values, params, 4U * sizeof(GLfloat));
	recorded = fixed_record(fixed, FIXED_OP_MATERIAL, face, pname, values, 4U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* A face. */
	if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The front's. */
	if (face != GL_BACK)
		fixed_material(fixed, pname, values);
}

/*
 * Sets a scalar parameter of the material (the shininess).
 */
GL_APICALL void GL_APIENTRY
glMaterialf(
	GLenum face,
	GLenum pname,
	GLfloat param)
{
	/* As an array of one. */
	glMaterialfv(face, pname, &param);
}

/*
 * Sets a parameter of the material from integers.
 */
GL_APICALL void GL_APIENTRY
glMaterialiv(
	GLenum face,
	GLenum pname,
	const GLint *params)
{
	GLfloat values[4];
	unsigned index;

	/* Colours as fractions of the largest integer, the shininess as it is. */
	memset(values, 0, sizeof(values));
	values[0] = (GLfloat)params[0];
	for (index = 0U; pname != GL_SHININESS && index < 4U; index++)
		values[index] = (GLfloat)params[index] / 2147483647.0f;
	glMaterialfv(face, pname, values);
}

/*
 * Sets a scalar parameter of the material from an integer.
 */
GL_APICALL void GL_APIENTRY
glMateriali(
	GLenum face,
	GLenum pname,
	GLint param)
{
	GLfloat value;

	/* As a float. */
	value = (GLfloat)param;
	glMaterialfv(face, pname, &value);
}

/*
 * Sets which material the vertex colour stands for under GL_COLOR_MATERIAL.
 */
GL_APICALL void GL_APIENTRY
glColorMaterial(
	GLenum face,
	GLenum mode)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	recorded = fixed_record(fixed, FIXED_OP_COLOR_MATERIAL, face, mode, NULL, 0U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* The mode (the face is always the front here). */
	fixed->color_material_mode = mode;
}

/*
 * Sets the alpha test.
 */
GL_APICALL void GL_APIENTRY
glAlphaFunc(
	GLenum func,
	GLfloat ref)
{
	struct zegl_context *context;
	struct fixed_state *fixed;

	/* A comparison. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	if (func < GL_NEVER || func > GL_ALWAYS) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The comparison and the reference, clamped. */
	fixed->alpha_func = func;
	fixed->alpha_ref = ref;
	if (ref < 0.0f)
		fixed->alpha_ref = 0.0f;
	if (ref > 1.0f)
		fixed->alpha_ref = 1.0f;
}

/*
 * Sets the texture environment's mode (GL_MODULATE, or GL_REPLACE; GL_DECAL
 * is taken as GL_REPLACE).
 */
GL_APICALL void GL_APIENTRY
glTexEnvi(
	GLenum target,
	GLenum pname,
	GLint param)
{
	struct zegl_context *context;
	struct fixed_state *fixed;

	/* The mode of the environment. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	if (target != GL_TEXTURE_ENV || pname != GL_TEXTURE_ENV_MODE)
		return;

	/* Set. */
	fixed->texture_env_mode = (GLenum)param;
}

/*
 * Sets the texture environment's mode from a float.
 */
GL_APICALL void GL_APIENTRY
glTexEnvf(
	GLenum target,
	GLenum pname,
	GLfloat param)
{
	/* As an integer. */
	glTexEnvi(target, pname, (GLint)param);
}

/*
 * Sets the texture environment from an array (the colour is not used).
 */
GL_APICALL void GL_APIENTRY
glTexEnvfv(
	GLenum target,
	GLenum pname,
	const GLfloat *params)
{
	/* The first value. */
	glTexEnvi(target, pname, (GLint)params[0]);
}

/*
 * Sets the texture environment from an array of integers.
 */
GL_APICALL void GL_APIENTRY
glTexEnviv(
	GLenum target,
	GLenum pname,
	const GLint *params)
{
	/* The first value. */
	glTexEnvi(target, pname, params[0]);
}

/*
 * Sets the size of points.
 */
GL_APICALL void GL_APIENTRY
glPointSize(
	GLfloat size)
{
	struct zegl_context *context;
	struct fixed_state *fixed;

	/* A positive size. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	if (size <= 0.0f) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Set. */
	fixed->point_size = size;
}

/*
 * Sets the depth glClear clears to, from a double.
 */
GL_APICALL void GL_APIENTRY
glClearDepth(
	GLclampd depth)
{
	/* As a float. */
	glClearDepthf((GLfloat)depth);
}

/*
 * Sets the depth range, from doubles.
 */
GL_APICALL void GL_APIENTRY
glDepthRange(
	GLclampd zNear,
	GLclampd zFar)
{
	/* As floats. */
	glDepthRangef((GLfloat)zNear, (GLfloat)zFar);
}

/*
 * Sets how polygons are drawn: filled only.
 */
GL_APICALL void GL_APIENTRY
glPolygonMode(
	GLenum face,
	GLenum mode)
{
	/* Lines and points of polygons are not there. */
	(void)face;
	if (mode != GL_FILL)
		gles_error(gles_context(), GL_INVALID_ENUM);
}

/*
 * Selects the buffer draws go to: the window's back buffer always.
 */
GL_APICALL void GL_APIENTRY
glDrawBuffer(
	GLenum mode)
{
	/* There is one colour buffer. */
	(void)mode;
}

/*
 * Selects the buffer glReadPixels reads: the window's back buffer always.
 */
GL_APICALL void GL_APIENTRY
glReadBuffer(
	GLenum mode)
{
	/* There is one colour buffer. */
	(void)mode;
}

/*
 * Pushes state groups (not kept: glPopAttrib restores nothing).
 */
GL_APICALL void GL_APIENTRY
glPushAttrib(
	GLbitfield mask)
{
	/* Nothing is saved. */
	(void)mask;
}

/*
 * Pops state groups (nothing was saved).
 */
GL_APICALL void GL_APIENTRY
glPopAttrib(void)
{
	/* Nothing to restore. */
	return;
}

/*
 * Reports states as doubles.
 */
GL_APICALL void GL_APIENTRY
glGetDoublev(
	GLenum pname,
	GLdouble *params)
{
	GLfloat values[16];
	unsigned index;

	/* The floats (at most sixteen of them), converted. */
	memset(values, 0, sizeof(values));
	glGetFloatv(pname, values);
	for (index = 0U; index < 16U; index++)
		params[index] = (GLdouble)values[index];
}

/* Returns the flag of one of this layer's capabilities; nonzero when it is not one. */
static int
fixed_capability(
	struct zegl_context *context,
	GLenum cap,
	int **flag)
{
	struct fixed_state *fixed;
	struct zegl_context *current;

	/* The state. */
	(void)context;
	fixed = fixed_current(&current);
	if (fixed == NULL)
		return -1;

	/* A light. */
	if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + FIXED_LIGHTS) {
		*flag = &fixed->light_enabled[cap - GL_LIGHT0];
		return 0;
	}

	/* The rest. */
	switch (cap) {
	case GL_LIGHTING:
		*flag = &fixed->lighting;
		return 0;
	case GL_COLOR_MATERIAL:
		*flag = &fixed->color_material;
		return 0;
	case GL_NORMALIZE:
		*flag = &fixed->normalize;
		return 0;
	case GL_RESCALE_NORMAL:
		*flag = &fixed->rescale_normal;
		return 0;
	case GL_TEXTURE_2D:
		*flag = &fixed->texture_2d;
		return 0;
	case GL_ALPHA_TEST:
		*flag = &fixed->alpha_test;
		return 0;
	case GL_FOG:
		*flag = &fixed->fog;
		return 0;
	case GL_LINE_SMOOTH:
		*flag = &fixed->line_smooth;
		return 0;
	case GL_POINT_SMOOTH:
		*flag = &fixed->point_smooth;
		return 0;
	case GL_POLYGON_SMOOTH:
		*flag = &fixed->polygon_smooth;
		return 0;
	case GL_LINE_STIPPLE:
		*flag = &fixed->line_stipple;
		return 0;
	case GL_POLYGON_STIPPLE:
		*flag = &fixed->polygon_stipple;
		return 0;
	case GL_AUTO_NORMAL:
		*flag = &fixed->auto_normal;
		return 0;
	default:
		break;
	}

	/* Not one. */
	return -1;
}

/*
 * Returns the program for a draw without one, current, with its uniforms
 * written from the state: the smooth program for as many lights as are on
 * (1, 2, 4 or 8, the lights on packed first, so small programs fit the i915
 * compiler's kernels), or the flat one.  NULL when it cannot be made.
 */
static struct gles_program *
fixed_program(
	struct zegl_context *context,
	int *flat)
{
	struct zegl_context *current;
	struct fixed_state *fixed;
	struct gles_state *state;
	struct fixed_uniforms *uniforms;
	struct gles_program *program;
	GLfloat mvp[16];
	GLfloat normal[16];
	GLfloat material[5 * 4];
	GLfloat lights[FIXED_LIGHTS * 5U * 4U];
	GLfloat flags[4];
	GLfloat flags2[4];
	GLfloat params[4];
	GLfloat *slot;
	unsigned light;
	unsigned count;
	unsigned which;
	int complete;

	/* The state. */
	state = gles_state(context);
	fixed = fixed_current(&current);
	if (state == NULL || fixed == NULL)
		return NULL;

	/* The lights on, packed first (5 vectors each; the last one's w says it is on). */
	memset(lights, 0, sizeof(lights));
	count = 0U;
	for (light = 0U; light < FIXED_LIGHTS; light++) {
		if (!fixed->light_enabled[light])
			continue;
		slot = &lights[count * 20U];
		memcpy(slot, fixed->lights[light].position, 4U * sizeof(GLfloat));
		memcpy(slot + 4, fixed->lights[light].ambient, 4U * sizeof(GLfloat));
		memcpy(slot + 8, fixed->lights[light].diffuse, 4U * sizeof(GLfloat));
		memcpy(slot + 12, fixed->lights[light].specular, 4U * sizeof(GLfloat));
		memcpy(slot + 16, fixed->lights[light].attenuation, 3U * sizeof(GLfloat));
		slot[19] = 1.0f;
		count++;
	}

	/* The program: flat, or the smallest smooth one for the lights on. */
	which = 3U;
	if (count <= 4U)
		which = 2U;
	if (count <= 2U)
		which = 1U;
	if (count <= 1U)
		which = 0U;
	*flat = 0;
	if (fixed->shade_model == GL_FLAT) {
		which = FIXED_PROGRAM_FLAT;
		*flat = 1;
	}

	/* Made the first time, and current so its uniforms can be written. */
	if (fixed->programs[which] == 0U)
		fixed->programs[which] = fixed_make_program(which, &fixed->uniforms[which]);
	program = gles_names_get(&state->objects, fixed->programs[which]);
	if (program == NULL || program->kind != GLES_KIND_PROGRAM || !program->linked)
		return NULL;
	state->program = program;
	uniforms = &fixed->uniforms[which];

	/* The matrices: projection x modelview, the modelview, its normal matrix, the texture's. */
	fixed_multiply(mvp, fixed->projection[fixed->projection_top], fixed->modelview[fixed->modelview_top]);
	fixed_normal_matrix(fixed->modelview[fixed->modelview_top], normal);
	glUniformMatrix4fv(uniforms->mvp, 1, GL_FALSE, mvp);
	glUniformMatrix4fv(uniforms->modelview, 1, GL_FALSE, fixed->modelview[fixed->modelview_top]);
	glUniformMatrix4fv(uniforms->normal_matrix, 1, GL_FALSE, normal);
	glUniformMatrix4fv(uniforms->texture_matrix, 1, GL_FALSE, fixed->texture[fixed->texture_top]);

	/* The material: the scene's ambient, ambient, diffuse, specular, emission; and the lights. */
	memcpy(material, fixed->scene_ambient, 4U * sizeof(GLfloat));
	memcpy(material + 4, fixed->material_ambient, 4U * sizeof(GLfloat));
	memcpy(material + 8, fixed->material_diffuse, 4U * sizeof(GLfloat));
	memcpy(material + 12, fixed->material_specular, 4U * sizeof(GLfloat));
	memcpy(material + 16, fixed->material_emission, 4U * sizeof(GLfloat));
	glUniform4fv(uniforms->material, 5, material);
	glUniform4fv(uniforms->lights, (GLsizei)(FIXED_LIGHTS * 5U), lights);

	/* The flags: lighting, the colour material (for the ambient, for the diffuse), normalizing. */
	memset(flags, 0, sizeof(flags));
	if (fixed->lighting)
		flags[0] = 1.0f;
	if (fixed->color_material && (fixed->color_material_mode == GL_AMBIENT_AND_DIFFUSE || fixed->color_material_mode == GL_AMBIENT))
		flags[1] = 1.0f;
	if (fixed->color_material && (fixed->color_material_mode == GL_AMBIENT_AND_DIFFUSE || fixed->color_material_mode == GL_DIFFUSE))
		flags[2] = 1.0f;
	if (fixed->normalize || fixed->rescale_normal)
		flags[3] = 1.0f;
	glUniform4fv(uniforms->flags, 1, flags);

	/*
	 * Texturing when GL_TEXTURE_2D is on and unit 0 has a texture that can
	 * be sampled (GL leaves it off otherwise), whether the texture replaces
	 * the colour, and the alpha test (GL_NEVER .. GL_ALWAYS as 1 .. 8, 0 off).
	 */
	memset(flags2, 0, sizeof(flags2));
	complete = gles_texture_complete(state->units[0]);
	if (fixed->texture_2d && complete)
		flags2[0] = 1.0f;
	if (fixed->texture_env_mode == GL_REPLACE || fixed->texture_env_mode == GL_DECAL)
		flags2[1] = 1.0f;
	if (fixed->alpha_test)
		flags2[2] = (GLfloat)(fixed->alpha_func - GL_NEVER + 1U);
	flags2[3] = fixed->alpha_ref;
	glUniform4fv(uniforms->flags2, 1, flags2);
	glUniform1i(uniforms->texture, 0);

	/* The shininess and the point size. */
	memset(params, 0, sizeof(params));
	params[0] = fixed->shininess;
	params[1] = fixed->point_size;
	glUniform4fv(uniforms->params, 1, params);

	/* Succeeded: the program, current. */
	return program;
}

/* Writes one of this layer's states as floats; returns how many, 0 when the name is not one. */
static unsigned
fixed_get(
	struct zegl_context *context,
	GLenum pname,
	GLfloat *values)
{
	struct zegl_context *current;
	struct fixed_state *fixed;
	struct gles_state *state;

	/* The state. */
	state = gles_state(context);
	fixed = fixed_current(&current);
	if (state == NULL || fixed == NULL)
		return 0U;

	/* The state asked for. */
	switch (pname) {
	case GL_MODELVIEW_MATRIX:
		memcpy(values, fixed->modelview[fixed->modelview_top], 16U * sizeof(GLfloat));
		return 16U;
	case GL_PROJECTION_MATRIX:
		memcpy(values, fixed->projection[fixed->projection_top], 16U * sizeof(GLfloat));
		return 16U;
	case GL_TEXTURE_MATRIX:
		memcpy(values, fixed->texture[fixed->texture_top], 16U * sizeof(GLfloat));
		return 16U;
	case GL_MATRIX_MODE:
		values[0] = (GLfloat)fixed->matrix_mode;
		return 1U;
	case GL_MODELVIEW_STACK_DEPTH:
		values[0] = (GLfloat)(fixed->modelview_top + 1U);
		return 1U;
	case GL_PROJECTION_STACK_DEPTH:
		values[0] = (GLfloat)(fixed->projection_top + 1U);
		return 1U;
	case GL_TEXTURE_STACK_DEPTH:
		values[0] = (GLfloat)(fixed->texture_top + 1U);
		return 1U;
	case GL_MAX_MODELVIEW_STACK_DEPTH:
		values[0] = (GLfloat)FIXED_MODELVIEW_DEPTH;
		return 1U;
	case GL_MAX_PROJECTION_STACK_DEPTH:
	case GL_MAX_TEXTURE_STACK_DEPTH:
		values[0] = (GLfloat)FIXED_OTHER_DEPTH;
		return 1U;
	case GL_MAX_LIGHTS:
		values[0] = (GLfloat)FIXED_LIGHTS;
		return 1U;
	case GL_MAX_LIST_NESTING:
		values[0] = (GLfloat)FIXED_LIST_NESTING;
		return 1U;
	case GL_CURRENT_COLOR:
		memcpy(values, state->attribs[FIXED_COLOR].value, 4U * sizeof(GLfloat));
		return 4U;
	case GL_CURRENT_NORMAL:
		memcpy(values, state->attribs[FIXED_NORMAL].value, 3U * sizeof(GLfloat));
		return 3U;
	case GL_CURRENT_TEXTURE_COORDS:
		memcpy(values, state->attribs[FIXED_TEXCOORD].value, 4U * sizeof(GLfloat));
		return 4U;
	case GL_SHADE_MODEL:
		values[0] = (GLfloat)fixed->shade_model;
		return 1U;
	case GL_LIGHT_MODEL_AMBIENT:
		memcpy(values, fixed->scene_ambient, 4U * sizeof(GLfloat));
		return 4U;
	case GL_POINT_SIZE:
		values[0] = fixed->point_size;
		return 1U;
	case GL_ALPHA_TEST_FUNC:
		values[0] = (GLfloat)fixed->alpha_func;
		return 1U;
	case GL_ALPHA_TEST_REF:
		values[0] = fixed->alpha_ref;
		return 1U;
	case GL_LIST_INDEX:
		values[0] = 0.0f;
		if (fixed->compiling != NULL)
			values[0] = (GLfloat)fixed->compiling->name;
		return 1U;
	case GL_LIST_MODE:
		values[0] = (GLfloat)fixed->compile_mode;
		return 1U;
	default:
		break;
	}

	/* Not one of this layer's. */
	return 0U;
}

/* Returns the strings that are desktop GL's rather than OpenGL ES's: the version. */
static const GLubyte *
fixed_string(
	GLenum name)
{
	/* The version, in desktop GL's form; the rest are OpenGL ES's. */
	if (name == GL_VERSION)
		return (const GLubyte *)"1.4 zedBSD (fixed function on OpenGL ES 2.0 on Vulkan)";
	if (name == GL_SHADING_LANGUAGE_VERSION)
		return (const GLubyte *)"";
	return NULL;
}

/* Frees a context's fixed-function state (its programs go with the context's objects). */
static void
fixed_release(
	struct gles_state *state)
{
	struct fixed_state *fixed;

	/* The state, when there is one. */
	fixed = state->fixed;
	if (fixed == NULL)
		return;

	/* The lists, the vertices and the state. */
	fixed_lists_free(fixed);
	free(fixed->vertices);
	free(fixed);
	state->fixed = NULL;
}

/* Makes a context's fixed-function state with GL's initial values; NULL when there is no memory. */
static struct fixed_state *
fixed_new(
	struct gles_state *state)
{
	static const GLfloat scene_ambient[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
	static const GLfloat material_ambient[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
	static const GLfloat material_diffuse[4] = { 0.8f, 0.8f, 0.8f, 1.0f };
	static const GLfloat black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	static const GLfloat white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const GLfloat position[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
	struct fixed_state *fixed;
	unsigned light;

	/* The state. */
	fixed = calloc(1U, sizeof(*fixed));
	if (fixed == NULL)
		return NULL;

	/* The matrices are identities; the modelview is current. */
	fixed->matrix_mode = GL_MODELVIEW;
	fixed_identity(fixed->modelview[0]);
	fixed_identity(fixed->projection[0]);
	fixed_identity(fixed->texture[0]);

	/* The lights: black ambient, light 0 white and the others black, at +z far away, no attenuation. */
	for (light = 0U; light < FIXED_LIGHTS; light++) {
		memcpy(fixed->lights[light].ambient, black, sizeof(black));
		memcpy(fixed->lights[light].diffuse, black, sizeof(black));
		memcpy(fixed->lights[light].specular, black, sizeof(black));
		memcpy(fixed->lights[light].position, position, sizeof(position));
		fixed->lights[light].attenuation[0] = 1.0f;
	}

	/* Light 0 is white. */
	memcpy(fixed->lights[0].diffuse, white, sizeof(white));
	memcpy(fixed->lights[0].specular, white, sizeof(white));
	memcpy(fixed->scene_ambient, scene_ambient, sizeof(scene_ambient));

	/* The material. */
	memcpy(fixed->material_ambient, material_ambient, sizeof(material_ambient));
	memcpy(fixed->material_diffuse, material_diffuse, sizeof(material_diffuse));
	memcpy(fixed->material_specular, black, sizeof(black));
	memcpy(fixed->material_emission, black, sizeof(black));
	fixed->color_material_mode = GL_AMBIENT_AND_DIFFUSE;

	/* Smooth shading, ALWAYS, modulate, points of 1. */
	fixed->shade_model = GL_SMOOTH;
	fixed->alpha_func = GL_ALWAYS;
	fixed->texture_env_mode = GL_MODULATE;
	fixed->point_size = 1.0f;
	fixed->next_list = 1U;

	/* The current colour is white and the current normal +z. */
	memcpy(state->attribs[FIXED_COLOR].value, white, sizeof(white));
	state->attribs[FIXED_NORMAL].value[0] = 0.0f;
	state->attribs[FIXED_NORMAL].value[1] = 0.0f;
	state->attribs[FIXED_NORMAL].value[2] = 1.0f;
	return fixed;
}

/* Makes one of the programs (smooth for 1, 2, 4 or 8 lights, or flat) and finds its uniforms; returns its name, 0 on failure. */
static GLuint
fixed_make_program(
	unsigned which,
	struct fixed_uniforms *uniforms)
{
	GLuint vertex;
	GLuint fragment;
	GLuint program;
	GLint linked;

	/* The two shaders of the variant. */
	switch (which) {
	case 0U:
		vertex = fixed_shader(GL_VERTEX_SHADER, fixed_vert_1, sizeof(fixed_vert_1));
		break;
	case 1U:
		vertex = fixed_shader(GL_VERTEX_SHADER, fixed_vert_2, sizeof(fixed_vert_2));
		break;
	case 2U:
		vertex = fixed_shader(GL_VERTEX_SHADER, fixed_vert_4, sizeof(fixed_vert_4));
		break;
	case 3U:
		vertex = fixed_shader(GL_VERTEX_SHADER, fixed_vert_8, sizeof(fixed_vert_8));
		break;
	default:
		vertex = fixed_shader(GL_VERTEX_SHADER, fixed_flat_vert, sizeof(fixed_flat_vert));
		break;
	}

	/* The smooth fragment shader, or the flat one. */
	fragment = fixed_shader(GL_FRAGMENT_SHADER, fixed_frag, sizeof(fixed_frag));
	if (which == FIXED_PROGRAM_FLAT) {
		glDeleteShader(fragment);
		fragment = fixed_shader(GL_FRAGMENT_SHADER, fixed_flat_frag, sizeof(fixed_flat_frag));
	}

	/* The program, the attributes where the fixed-function arrays are. */
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glBindAttribLocation(program, FIXED_POSITION, "a_position");
	glBindAttribLocation(program, FIXED_COLOR, "a_color");
	glBindAttribLocation(program, FIXED_NORMAL, "a_normal");
	glBindAttribLocation(program, FIXED_TEXCOORD, "a_texcoord");
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked)
		return 0U;

	/* Its uniforms. */
	uniforms->mvp = glGetUniformLocation(program, "u_mvp");
	uniforms->modelview = glGetUniformLocation(program, "u_modelview");
	uniforms->normal_matrix = glGetUniformLocation(program, "u_normal_matrix");
	uniforms->texture_matrix = glGetUniformLocation(program, "u_texture_matrix");
	uniforms->material = glGetUniformLocation(program, "u_material");
	uniforms->lights = glGetUniformLocation(program, "u_lights");
	uniforms->flags = glGetUniformLocation(program, "u_flags");
	uniforms->flags2 = glGetUniformLocation(program, "u_flags2");
	uniforms->params = glGetUniformLocation(program, "u_params");
	uniforms->texture = glGetUniformLocation(program, "u_texture");

	/* Succeeded: the program. */
	return program;
}

/* Makes a shader from SPIR-V; 0 when the translation refuses it. */
static GLuint
fixed_shader(
	GLenum type,
	const uint32_t *code,
	size_t size)
{
	GLuint shader;

	/* The shader with the binary (SPIR-V's format, GL 4.6's value). */
	shader = glCreateShader(type);
	glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, code, (GLsizei)size);
	return shader;
}

/* Makes a matrix the identity. */
static void
fixed_identity(
	GLfloat *matrix)
{
	/* Ones on the diagonal. */
	memset(matrix, 0, 16U * sizeof(GLfloat));
	matrix[0] = 1.0f;
	matrix[5] = 1.0f;
	matrix[10] = 1.0f;
	matrix[15] = 1.0f;
}

/* Multiplies two column-major matrices: result = a x b (result may not be a or b). */
static void
fixed_multiply(
	GLfloat *result,
	const GLfloat *a,
	const GLfloat *b)
{
	unsigned row;
	unsigned column;
	unsigned k;
	GLfloat sum;

	/* Each element: a's row by b's column. */
	for (column = 0U; column < 4U; column++) {
		for (row = 0U; row < 4U; row++) {
			sum = 0.0f;
			for (k = 0U; k < 4U; k++)
				sum += a[k * 4U + row] * b[column * 4U + k];
			result[column * 4U + row] = sum;
		}
	}
}

/* Multiplies the current matrix by one on the right. */
static void
fixed_apply(
	const GLfloat *matrix)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	GLfloat result[16];
	GLfloat *current;

	/* The current matrix. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	current = fixed_matrix(fixed);

	/* Current = current x matrix. */
	fixed_multiply(result, current, matrix);
	memcpy(current, result, sizeof(result));
}

/* Makes the normal matrix (the inverse transpose of the modelview's upper 3x3) as the upper 3x3 of a 4x4. */
static void
fixed_normal_matrix(
	const GLfloat *m,
	GLfloat *normal)
{
	GLfloat determinant;
	GLfloat magnitude;
	GLfloat inverse;

	/* The determinant of the upper 3x3 (a zero one keeps the identity). */
	fixed_identity(normal);
	determinant = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
	magnitude = fabsf(determinant);
	if (magnitude < 1e-12f)
		return;
	inverse = 1.0f / determinant;

	/* The cofactors over the determinant: the inverse's transpose, column-major. */
	normal[0] = (m[5] * m[10] - m[6] * m[9]) * inverse;
	normal[1] = (m[6] * m[8] - m[4] * m[10]) * inverse;
	normal[2] = (m[4] * m[9] - m[5] * m[8]) * inverse;
	normal[4] = (m[2] * m[9] - m[1] * m[10]) * inverse;
	normal[5] = (m[0] * m[10] - m[2] * m[8]) * inverse;
	normal[6] = (m[1] * m[8] - m[0] * m[9]) * inverse;
	normal[8] = (m[1] * m[6] - m[2] * m[5]) * inverse;
	normal[9] = (m[2] * m[4] - m[0] * m[6]) * inverse;
	normal[10] = (m[0] * m[5] - m[1] * m[4]) * inverse;
}

/* Transforms a point by a column-major matrix. */
static void
fixed_transform(
	const GLfloat *matrix,
	const GLfloat *in,
	GLfloat *out)
{
	unsigned row;

	/* Each output component: the matrix's row by the point. */
	for (row = 0U; row < 4U; row++)
		out[row] = matrix[row] * in[0] + matrix[4U + row] * in[1] + matrix[8U + row] * in[2] + matrix[12U + row] * in[3];
}

/* Returns a light of the state, recording the error when the name is not a light. */
static struct fixed_light *
fixed_light_of(
	struct zegl_context *context,
	struct fixed_state *fixed,
	GLenum light)
{
	/* GL_LIGHT0 to GL_LIGHT7. */
	if (light < GL_LIGHT0 || light >= GL_LIGHT0 + FIXED_LIGHTS) {
		gles_error(context, GL_INVALID_ENUM);
		return NULL;
	}

	/* Its record. */
	return &fixed->lights[light - GL_LIGHT0];
}

/* Sets one parameter of the front material. */
static void
fixed_material(
	struct fixed_state *fixed,
	GLenum pname,
	const GLfloat *params)
{
	/* The parameter. */
	switch (pname) {
	case GL_AMBIENT:
		memcpy(fixed->material_ambient, params, 4U * sizeof(GLfloat));
		break;
	case GL_DIFFUSE:
		memcpy(fixed->material_diffuse, params, 4U * sizeof(GLfloat));
		break;
	case GL_AMBIENT_AND_DIFFUSE:
		memcpy(fixed->material_ambient, params, 4U * sizeof(GLfloat));
		memcpy(fixed->material_diffuse, params, 4U * sizeof(GLfloat));
		break;
	case GL_SPECULAR:
		memcpy(fixed->material_specular, params, 4U * sizeof(GLfloat));
		break;
	case GL_EMISSION:
		memcpy(fixed->material_emission, params, 4U * sizeof(GLfloat));
		break;
	case GL_SHININESS:
		fixed->shininess = params[0];
		break;
	default:
		break;
	}
}
