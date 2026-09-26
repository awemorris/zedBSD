/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's OpenGL ES 2.0 over EGL and Vulkan (WS068): a context's state,
 * its fixed-function settings, and the queries.
 *
 * A context's libGLESv2 state is made at its first GLES call and handed
 * to libEGL's context with two callbacks: one when eglSwapBuffers has
 * finished a frame (the frame's stream, descriptors and garbage are free
 * again), one when the context is destroyed.  Buffers (buffer.c),
 * textures (texture.c), shaders and programs (program.c, spirv.c) and
 * the draws (draw.c) translate into Vulkan; see gles.h.
 */

#include "gles.h"

#include <stdlib.h>
#include <string.h>

/* The extensions this library offers. */
#define GLES_EXTENSIONS \
	"GL_OES_element_index_uint GL_OES_texture_npot GL_EXT_texture_format_BGRA8888 GL_EXT_blend_minmax"

/* The fixed-function layer, NULL without one (libGL sets it before its first context). */
const struct gles_fixed_hooks *gles_fixed;

static void gles_frame_done(struct zegl_context *context);
static void gles_release(struct zegl_context *context);
static int gles_capability(struct gles_state *state, GLenum cap, int **flag);
static unsigned gles_integers(struct zegl_context *context, struct gles_state *state, GLenum pname, GLint *values);
static unsigned gles_floats(struct zegl_context *context, struct gles_state *state, GLenum pname, GLfloat *values);

/*
 * Returns the calling thread's current context, or NULL.
 */
struct zegl_context *
gles_context(void)
{
	struct zegl_context *context;

	/* libEGL keeps it per thread. */
	context = zegl_current_context();
	return context;
}

/*
 * Returns a context's libGLESv2 state, making it at the first call;
 * NULL without a context, or when there is no memory.
 */
struct gles_state *
gles_state(
	struct zegl_context *context)
{
	struct gles_state *state;
	VkPhysicalDeviceProperties properties;
	unsigned index;

	/* A context, and its state when made already. */
	if (context == NULL)
		return NULL;
	if (context->gles.state != NULL)
		return context->gles.state;

	/* The state, with the device's memory types and limits. */
	state = calloc(1U, sizeof(*state));
	if (state == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return NULL;
	}

	/* The display and its device. */
	state->context = context;
	state->display = context->display;
	state->device = context->display->device;
	vkGetPhysicalDeviceMemoryProperties(context->display->physical, &state->memory);
	vkGetPhysicalDeviceProperties(context->display->physical, &properties);
	state->limits = properties.limits;

	/* GL's initial state: blending off with ONE and ZERO, every channel written. */
	state->blend_src_rgb = GL_ONE;
	state->blend_dst_rgb = GL_ZERO;
	state->blend_src_alpha = GL_ONE;
	state->blend_dst_alpha = GL_ZERO;
	state->blend_equation_rgb = GL_FUNC_ADD;
	state->blend_equation_alpha = GL_FUNC_ADD;
	for (index = 0U; index < 4U; index++)
		state->color_mask[index] = GL_TRUE;

	/* The depth test off, LESS, writing, clearing to 1 over [0, 1]. */
	state->depth_func = GL_LESS;
	state->depth_mask = GL_TRUE;
	state->clear_depth = 1.0f;
	state->depth_near = 0.0f;
	state->depth_far = 1.0f;

	/* The stencil test off, ALWAYS with every bit, KEEP. */
	for (index = 0U; index < 2U; index++) {
		state->stencil_func[index] = GL_ALWAYS;
		state->stencil_value_mask[index] = 0xffffffffU;
		state->stencil_write_mask[index] = 0xffffffffU;
		state->stencil_fail[index] = GL_KEEP;
		state->stencil_zfail[index] = GL_KEEP;
		state->stencil_zpass[index] = GL_KEEP;
	}

	/* Culling off (back faces, counter-clockwise front), the scissor box the viewport. */
	state->cull_mode = GL_BACK;
	state->front_face = GL_CCW;
	memcpy(state->scissor, context->gles.viewport, sizeof(state->scissor));

	/* Lines one wide, dithering on, rows aligned to 4, attributes (0, 0, 0, 1). */
	state->line_width = 1.0f;
	state->dither = 1;
	state->sample_coverage_value = 1.0f;
	state->unpack_alignment = 4;
	state->pack_alignment = 4;
	state->mipmap_hint = GL_DONT_CARE;
	for (index = 0U; index < GLES_ATTRIBS; index++)
		state->attribs[index].value[3] = 1.0f;

	/* Succeeded: the first frame, and the callbacks libEGL makes. */
	state->frame = 1U;
	context->gles.state = state;
	context->gles.frame_done = gles_frame_done;
	context->gles.release = gles_release;
	return state;
}

/*
 * Records an error, keeping the first one until glGetError reads it.
 */
void
gles_error(
	struct zegl_context *context,
	GLenum error)
{
	/* Only the first counts. */
	if (context->gles.error == GL_NO_ERROR)
		context->gles.error = error;
}

/*
 * Puts an object under a name, growing the table.  Returns 0, or -1 when
 * there is no memory.
 */
int
gles_names_add(
	struct gles_names *names,
	GLuint name,
	void *object)
{
	void **grown;
	GLuint capacity;

	/* The table grows by doubling until the name fits. */
	if (name >= names->capacity) {
		capacity = names->capacity;
		if (capacity == 0U)
			capacity = 64U;
		while (capacity <= name)
			capacity *= 2U;
		grown = realloc(names->objects, capacity * sizeof(*grown));
		if (grown == NULL)
			return -1;
		memset(grown + names->capacity, 0, (capacity - names->capacity) * sizeof(*grown));
		names->objects = grown;
		names->capacity = capacity;
	}

	/* Succeeded: the object under its name. */
	names->objects[name] = object;
	return 0;
}

/*
 * Returns the lowest name no object has.
 */
GLuint
gles_names_free(
	struct gles_names *names)
{
	GLuint name;

	/* The first empty slot after 0, else the first past the table. */
	for (name = 1U; name < names->capacity; name++) {
		if (names->objects[name] == NULL)
			return name;
	}

	/* Past the table (at least 1). */
	if (names->capacity == 0U)
		return 1U;
	return names->capacity;
}

/*
 * Returns the object under a name, or NULL.
 */
void *
gles_names_get(
	struct gles_names *names,
	GLuint name)
{
	/* Names outside the table have no object. */
	if (name == 0U || name >= names->capacity)
		return NULL;
	return names->objects[name];
}

/*
 * Takes the object away from a name.
 */
void
gles_names_remove(
	struct gles_names *names,
	GLuint name)
{
	/* Only a name inside the table has one. */
	if (name != 0U && name < names->capacity)
		names->objects[name] = NULL;
}

/*
 * Sets the colour glClear clears the colour buffer to (each channel clamped to [0, 1]).
 */
GL_APICALL void GL_APIENTRY
glClearColor(
	GLfloat red,
	GLfloat green,
	GLfloat blue,
	GLfloat alpha)
{
	struct zegl_context *context;
	GLfloat channels[4];
	unsigned index;

	/* Without a current context the call does nothing. */
	context = gles_context();
	if (context == NULL)
		return;

	/* Each channel, clamped. */
	channels[0] = red;
	channels[1] = green;
	channels[2] = blue;
	channels[3] = alpha;
	for (index = 0U; index < 4U; index++) {
		if (channels[index] < 0.0f)
			channels[index] = 0.0f;
		if (channels[index] > 1.0f)
			channels[index] = 1.0f;
		context->gles.clear_color[index] = channels[index];
	}
}

/*
 * Sets the depth glClear clears the depth buffer to.
 */
GL_APICALL void GL_APIENTRY
glClearDepthf(
	GLfloat d)
{
	struct gles_state *state;

	/* Clamped to [0, 1]. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	if (d < 0.0f)
		d = 0.0f;
	if (d > 1.0f)
		d = 1.0f;
	state->clear_depth = d;
}

/*
 * Sets the value glClear clears the stencil buffer to.
 */
GL_APICALL void GL_APIENTRY
glClearStencil(
	GLint s)
{
	struct gles_state *state;

	/* The value. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	state->clear_stencil = s;
}

/*
 * Sets the viewport.
 */
GL_APICALL void GL_APIENTRY
glViewport(
	GLint x,
	GLint y,
	GLsizei width,
	GLsizei height)
{
	struct zegl_context *context;

	/* Without a current context the call does nothing. */
	context = gles_context();
	if (context == NULL)
		return;

	/* A negative size is an error. */
	if (width < 0 || height < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The viewport, for the draws to come. */
	context->gles.viewport[0] = x;
	context->gles.viewport[1] = y;
	context->gles.viewport[2] = width;
	context->gles.viewport[3] = height;
	context->gles.viewport_set = 1;
}

/*
 * Sets the depth range the viewport maps to.
 */
GL_APICALL void GL_APIENTRY
glDepthRangef(
	GLfloat n,
	GLfloat f)
{
	struct gles_state *state;

	/* Each end clamped to [0, 1]. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	if (n < 0.0f)
		n = 0.0f;
	if (n > 1.0f)
		n = 1.0f;
	if (f < 0.0f)
		f = 0.0f;
	if (f > 1.0f)
		f = 1.0f;
	state->depth_near = n;
	state->depth_far = f;
}

/*
 * Turns a capability on.
 */
GL_APICALL void GL_APIENTRY
glEnable(
	GLenum cap)
{
	struct zegl_context *context;
	struct gles_state *state;
	int *flag;
	int status;

	/* The capability's flag. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	status = gles_capability(state, cap, &flag);
	if (status != 0) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* On. */
	*flag = 1;
}

/*
 * Turns a capability off.
 */
GL_APICALL void GL_APIENTRY
glDisable(
	GLenum cap)
{
	struct zegl_context *context;
	struct gles_state *state;
	int *flag;
	int status;

	/* The capability's flag. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	status = gles_capability(state, cap, &flag);
	if (status != 0) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Off. */
	*flag = 0;
}

/*
 * Reports whether a capability is on.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsEnabled(
	GLenum cap)
{
	struct zegl_context *context;
	struct gles_state *state;
	int *flag;
	int status;

	/* The capability's flag. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return GL_FALSE;
	status = gles_capability(state, cap, &flag);
	if (status != 0) {
		gles_error(context, GL_INVALID_ENUM);
		return GL_FALSE;
	}

	/* Its value. */
	if (*flag)
		return GL_TRUE;
	return GL_FALSE;
}

/*
 * Sets the blend factors of colour and alpha together.
 */
GL_APICALL void GL_APIENTRY
glBlendFunc(
	GLenum sfactor,
	GLenum dfactor)
{
	/* The same for both. */
	glBlendFuncSeparate(sfactor, dfactor, sfactor, dfactor);
}

/*
 * Sets the blend factors of colour and alpha.
 */
GL_APICALL void GL_APIENTRY
glBlendFuncSeparate(
	GLenum sfactorRGB,
	GLenum dfactorRGB,
	GLenum sfactorAlpha,
	GLenum dfactorAlpha)
{
	struct gles_state *state;

	/* The four factors. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	state->blend_src_rgb = sfactorRGB;
	state->blend_dst_rgb = dfactorRGB;
	state->blend_src_alpha = sfactorAlpha;
	state->blend_dst_alpha = dfactorAlpha;
}

/*
 * Sets the blend equation of colour and alpha together.
 */
GL_APICALL void GL_APIENTRY
glBlendEquation(
	GLenum mode)
{
	/* The same for both. */
	glBlendEquationSeparate(mode, mode);
}

/*
 * Sets the blend equations of colour and alpha.
 */
GL_APICALL void GL_APIENTRY
glBlendEquationSeparate(
	GLenum modeRGB,
	GLenum modeAlpha)
{
	struct gles_state *state;

	/* The two equations. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	state->blend_equation_rgb = modeRGB;
	state->blend_equation_alpha = modeAlpha;
}

/*
 * Sets the constant blend colour.
 */
GL_APICALL void GL_APIENTRY
glBlendColor(
	GLfloat red,
	GLfloat green,
	GLfloat blue,
	GLfloat alpha)
{
	struct gles_state *state;

	/* The colour. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	state->blend_color[0] = red;
	state->blend_color[1] = green;
	state->blend_color[2] = blue;
	state->blend_color[3] = alpha;
}

/*
 * Sets which colour channels draws write.
 */
GL_APICALL void GL_APIENTRY
glColorMask(
	GLboolean red,
	GLboolean green,
	GLboolean blue,
	GLboolean alpha)
{
	struct gles_state *state;

	/* The four channels. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	state->color_mask[0] = red;
	state->color_mask[1] = green;
	state->color_mask[2] = blue;
	state->color_mask[3] = alpha;
}

/*
 * Sets the depth test's comparison.
 */
GL_APICALL void GL_APIENTRY
glDepthFunc(
	GLenum func)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* One of the eight comparisons. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (func < GL_NEVER || func > GL_ALWAYS) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The comparison. */
	state->depth_func = func;
}

/*
 * Sets whether draws write the depth buffer.
 */
GL_APICALL void GL_APIENTRY
glDepthMask(
	GLboolean flag)
{
	struct gles_state *state;

	/* The flag. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	state->depth_mask = flag;
}

/*
 * Sets the stencil test of both faces.
 */
GL_APICALL void GL_APIENTRY
glStencilFunc(
	GLenum func,
	GLint ref,
	GLuint mask)
{
	/* Front and back. */
	glStencilFuncSeparate(GL_FRONT_AND_BACK, func, ref, mask);
}

/*
 * Sets the stencil test of one face or both.
 */
GL_APICALL void GL_APIENTRY
glStencilFuncSeparate(
	GLenum face,
	GLenum func,
	GLint ref,
	GLuint mask)
{
	struct zegl_context *context;
	struct gles_state *state;
	unsigned index;

	/* One of the eight comparisons. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (func < GL_NEVER || func > GL_ALWAYS) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Each face named: 0 front, 1 back. */
	for (index = 0U; index < 2U; index++) {
		if (face == GL_FRONT && index == 1U)
			continue;
		if (face == GL_BACK && index == 0U)
			continue;
		state->stencil_func[index] = func;
		state->stencil_ref[index] = ref;
		state->stencil_value_mask[index] = mask;
	}
}

/*
 * Sets the stencil operations of both faces.
 */
GL_APICALL void GL_APIENTRY
glStencilOp(
	GLenum fail,
	GLenum zfail,
	GLenum zpass)
{
	/* Front and back. */
	glStencilOpSeparate(GL_FRONT_AND_BACK, fail, zfail, zpass);
}

/*
 * Sets the stencil operations of one face or both.
 */
GL_APICALL void GL_APIENTRY
glStencilOpSeparate(
	GLenum face,
	GLenum sfail,
	GLenum dpfail,
	GLenum dppass)
{
	struct gles_state *state;
	unsigned index;

	/* Each face named: 0 front, 1 back. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	for (index = 0U; index < 2U; index++) {
		if (face == GL_FRONT && index == 1U)
			continue;
		if (face == GL_BACK && index == 0U)
			continue;
		state->stencil_fail[index] = sfail;
		state->stencil_zfail[index] = dpfail;
		state->stencil_zpass[index] = dppass;
	}
}

/*
 * Sets the stencil bits draws write, both faces.
 */
GL_APICALL void GL_APIENTRY
glStencilMask(
	GLuint mask)
{
	/* Front and back. */
	glStencilMaskSeparate(GL_FRONT_AND_BACK, mask);
}

/*
 * Sets the stencil bits draws write, one face or both.
 */
GL_APICALL void GL_APIENTRY
glStencilMaskSeparate(
	GLenum face,
	GLuint mask)
{
	struct gles_state *state;
	unsigned index;

	/* Each face named: 0 front, 1 back. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	for (index = 0U; index < 2U; index++) {
		if (face == GL_FRONT && index == 1U)
			continue;
		if (face == GL_BACK && index == 0U)
			continue;
		state->stencil_write_mask[index] = mask;
	}
}

/*
 * Sets which faces culling removes.
 */
GL_APICALL void GL_APIENTRY
glCullFace(
	GLenum mode)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* Front, back or both. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The mode. */
	state->cull_mode = mode;
}

/*
 * Sets which winding is a front face.
 */
GL_APICALL void GL_APIENTRY
glFrontFace(
	GLenum mode)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* Clockwise or counter-clockwise. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (mode != GL_CW && mode != GL_CCW) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The winding. */
	state->front_face = mode;
}

/*
 * Sets the scissor box.
 */
GL_APICALL void GL_APIENTRY
glScissor(
	GLint x,
	GLint y,
	GLsizei width,
	GLsizei height)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A size that is not negative. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (width < 0 || height < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The box. */
	state->scissor[0] = x;
	state->scissor[1] = y;
	state->scissor[2] = width;
	state->scissor[3] = height;
}

/*
 * Sets the width of lines (drawn one pixel wide whatever it is).
 */
GL_APICALL void GL_APIENTRY
glLineWidth(
	GLfloat width)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A positive width. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (width <= 0.0f) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Kept for glGet. */
	state->line_width = width;
}

/*
 * Sets the polygon offset.
 */
GL_APICALL void GL_APIENTRY
glPolygonOffset(
	GLfloat factor,
	GLfloat units)
{
	struct gles_state *state;

	/* The factor and the units. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	state->polygon_factor = factor;
	state->polygon_units = units;
}

/*
 * Sets the sample coverage (kept; surfaces have one sample).
 */
GL_APICALL void GL_APIENTRY
glSampleCoverage(
	GLfloat value,
	GLboolean invert)
{
	struct gles_state *state;

	/* Kept for glGet. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;
	state->sample_coverage_value = value;
	state->sample_coverage_invert = invert;
}

/*
 * Sets a hint: only the mipmap hint, which glGenerateMipmap does not need.
 */
GL_APICALL void GL_APIENTRY
glHint(
	GLenum target,
	GLenum mode)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* The one target. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (target != GL_GENERATE_MIPMAP_HINT) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Kept for glGet. */
	state->mipmap_hint = mode;
}

/*
 * Sets the row alignment of the pixels glTexImage2D reads or glReadPixels
 * writes.
 */
GL_APICALL void GL_APIENTRY
glPixelStorei(
	GLenum pname,
	GLint param)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* An alignment of 1, 2, 4 or 8. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (param != 1 && param != 2 && param != 4 && param != 8) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The parameter. */
	switch (pname) {
	case GL_UNPACK_ALIGNMENT:
		state->unpack_alignment = param;
		return;
	case GL_PACK_ALIGNMENT:
		state->pack_alignment = param;
		return;
	default:
		break;
	}

	/* Any other is an error. */
	gles_error(context, GL_INVALID_ENUM);
}

/*
 * Reports integer states.
 */
GL_APICALL void GL_APIENTRY
glGetIntegerv(
	GLenum pname,
	GLint *data)
{
	struct zegl_context *context;
	struct gles_state *state;
	GLfloat floats[16];
	unsigned count;
	unsigned index;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL || data == NULL)
		return;

	/* An integer state. */
	count = gles_integers(context, state, pname, data);
	if (count != 0U)
		return;

	/* A float state (the fixed-function layer's too), rounded. */
	count = gles_floats(context, state, pname, floats);
	for (index = 0U; index < count; index++)
		data[index] = (GLint)(floats[index] + 0.5f);
	if (count == 0U)
		gles_error(context, GL_INVALID_ENUM);
}

/*
 * Reports float states.
 */
GL_APICALL void GL_APIENTRY
glGetFloatv(
	GLenum pname,
	GLfloat *data)
{
	struct zegl_context *context;
	struct gles_state *state;
	GLint integers[16];
	unsigned count;
	unsigned index;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL || data == NULL)
		return;

	/* A float state. */
	count = gles_floats(context, state, pname, data);
	if (count != 0U)
		return;

	/* An integer state, converted. */
	count = gles_integers(context, state, pname, integers);
	for (index = 0U; index < count; index++)
		data[index] = (GLfloat)integers[index];
	if (count == 0U)
		gles_error(context, GL_INVALID_ENUM);
}

/*
 * Reports states as booleans.
 */
GL_APICALL void GL_APIENTRY
glGetBooleanv(
	GLenum pname,
	GLboolean *data)
{
	struct zegl_context *context;
	struct gles_state *state;
	GLfloat floats[16];
	GLint integers[16];
	unsigned count;
	unsigned index;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL || data == NULL)
		return;

	/* The colour mask and the depth mask are booleans already. */
	if (pname == GL_COLOR_WRITEMASK) {
		memcpy(data, state->color_mask, 4U);
		return;
	}

	/* The depth mask is one. */
	if (pname == GL_DEPTH_WRITEMASK) {
		data[0] = state->depth_mask;
		return;
	}

	/* An integer state, else a float state: nonzero is true. */
	count = gles_integers(context, state, pname, integers);
	for (index = 0U; index < count; index++)
		data[index] = (GLboolean)(integers[index] != 0);
	if (count != 0U)
		return;
	count = gles_floats(context, state, pname, floats);
	for (index = 0U; index < count; index++)
		data[index] = (GLboolean)(floats[index] != 0.0f);
	if (count == 0U)
		gles_error(context, GL_INVALID_ENUM);
}

/*
 * Returns one of GLES's strings.
 */
GL_APICALL const GLubyte *GL_APIENTRY
glGetString(
	GLenum name)
{
	struct zegl_context *context;
	const GLubyte *layer;

	/* Without a current context there are no strings. */
	context = gles_context();
	if (context == NULL)
		return NULL;

	/* The fixed-function layer's own (desktop GL's version). */
	if (gles_fixed != NULL) {
		layer = gles_fixed->string(name);
		if (layer != NULL)
			return layer;
	}

	/* The string asked for. */
	switch (name) {
	case GL_VENDOR:
		return (const GLubyte *)"zedBSD";
	case GL_RENDERER:
		return (const GLubyte *)"zedBSD OpenGL ES on Vulkan";
	case GL_VERSION:
		return (const GLubyte *)"OpenGL ES 2.0 zedBSD";
	case GL_SHADING_LANGUAGE_VERSION:
		return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
	case GL_EXTENSIONS:
		return (const GLubyte *)GLES_EXTENSIONS;
	default:
		break;
	}

	/* Any other name is an error. */
	gles_error(context, GL_INVALID_ENUM);
	return NULL;
}

/*
 * Returns the first error since the last call, and clears it.
 */
GL_APICALL GLenum GL_APIENTRY
glGetError(void)
{
	struct zegl_context *context;
	GLenum error;

	/* Without a current context there is no error. */
	context = gles_context();
	if (context == NULL)
		return GL_NO_ERROR;

	/* Reading it clears it. */
	error = (GLenum)context->gles.error;
	context->gles.error = GL_NO_ERROR;
	return error;
}

/*
 * Sends the recorded commands on: eglSwapBuffers sends each frame, so there is nothing to send.
 */
GL_APICALL void GL_APIENTRY
glFlush(void)
{
	/* Nothing is waiting. */
	return;
}

/*
 * Waits for the recorded commands: nothing becomes visible before
 * eglSwapBuffers or glReadPixels, which wait themselves.
 */
GL_APICALL void GL_APIENTRY
glFinish(void)
{
	/* Nothing is running. */
	return;
}

/* Frees what the frame that eglSwapBuffers just finished held, and counts the next frame. */
static void
gles_frame_done(
	struct zegl_context *context)
{
	struct gles_state *state;

	/* The state, when there is one. */
	state = context->gles.state;
	if (state == NULL)
		return;

	/* The next frame; the finished one's memory is free. */
	state->frame++;
	gles_collect(state);
}

/* Frees a context's libGLESv2 state and every object and Vulkan object in it. */
static void
gles_release(
	struct zegl_context *context)
{
	struct gles_state *state;
	struct gles_pipeline *pipeline;
	struct gles_sampler *sampler;
	struct gles_pool *pool;
	struct gles_chunk *chunk;
	struct gles_program *program;
	struct gles_shader *shader;
	GLuint name;

	/* The state, when there is one; nothing may still run. */
	state = context->gles.state;
	if (state == NULL)
		return;
	(void)vkDeviceWaitIdle(state->device);

	/* The fixed-function layer's state (its programs are among the objects freed below). */
	if (gles_fixed != NULL)
		gles_fixed->release(state);

	/* The programs, then the shaders left. */
	state->program = NULL;
	for (name = 1U; name < state->objects.capacity; name++) {
		program = state->objects.objects[name];
		if (program != NULL && program->kind == GLES_KIND_PROGRAM) {
			state->objects.objects[name] = NULL;
			gles_program_release(state, program);
		}
	}

	/* The shaders no program took with it. */
	for (name = 1U; name < state->objects.capacity; name++) {
		shader = state->objects.objects[name];
		if (shader != NULL)
			gles_shader_release(shader);
	}

	/* The buffers and the textures. */
	for (name = 1U; name < state->buffers.capacity; name++) {
		if (state->buffers.objects[name] != NULL)
			gles_buffer_free(state, state->buffers.objects[name]);
	}

	/* The textures. */
	for (name = 1U; name < state->textures.capacity; name++) {
		if (state->textures.objects[name] != NULL)
			gles_texture_free(state, state->textures.objects[name]);
	}

	/* The black texture. */
	if (state->black != NULL)
		gles_texture_free(state, state->black);

	/* The garbage, which the frees above added to. */
	gles_collect(state);

	/* The pipelines, samplers, descriptor pools and stream. */
	while (state->pipelines != NULL) {
		pipeline = state->pipelines;
		state->pipelines = pipeline->next;
		vkDestroyPipeline(state->device, pipeline->pipeline, NULL);
		free(pipeline);
	}

	/* The samplers. */
	while (state->samplers != NULL) {
		sampler = state->samplers;
		state->samplers = sampler->next;
		vkDestroySampler(state->device, sampler->sampler, NULL);
		free(sampler);
	}
	while (state->pools != NULL) {
		pool = state->pools;
		state->pools = pool->next;
		vkDestroyDescriptorPool(state->device, pool->pool, NULL);
		free(pool);
	}
	while (state->chunks != NULL) {
		chunk = state->chunks;
		state->chunks = chunk->next;
		vkDestroyBuffer(state->device, chunk->buffer, NULL);
		vkFreeMemory(state->device, chunk->memory, NULL);
		free(chunk);
	}

	/* The upload's objects. */
	if (state->upload_fence != VK_NULL_HANDLE)
		vkDestroyFence(state->device, state->upload_fence, NULL);
	if (state->upload_pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(state->device, state->upload_pool, NULL);

	/* The tables and the state. */
	free(state->objects.objects);
	free(state->buffers.objects);
	free(state->textures.objects);
	free(state);
	context->gles.state = NULL;
}

/* Returns the flag of a capability glEnable takes; nonzero for one that is not. */
static int
gles_capability(
	struct gles_state *state,
	GLenum cap,
	int **flag)
{
	int status;

	/* The capabilities of OpenGL ES 2. */
	switch (cap) {
	case GL_BLEND:
		*flag = &state->blend;
		return 0;
	case GL_CULL_FACE:
		*flag = &state->cull;
		return 0;
	case GL_DEPTH_TEST:
		*flag = &state->depth_test;
		return 0;
	case GL_DITHER:
		*flag = &state->dither;
		return 0;
	case GL_POLYGON_OFFSET_FILL:
		*flag = &state->polygon_offset;
		return 0;
	case GL_SAMPLE_ALPHA_TO_COVERAGE:
		*flag = &state->sample_alpha_to_coverage;
		return 0;
	case GL_SAMPLE_COVERAGE:
		*flag = &state->sample_coverage;
		return 0;
	case GL_SCISSOR_TEST:
		*flag = &state->scissor_test;
		return 0;
	case GL_STENCIL_TEST:
		*flag = &state->stencil_test;
		return 0;
	default:
		break;
	}

	/* One of the fixed-function layer's, when there is the layer. */
	if (gles_fixed == NULL)
		return -1;
	status = gles_fixed->capability(state->context, cap, flag);
	return status;
}

/* Writes an integer state's values; returns how many, 0 when the name is not an integer state. */
static unsigned
gles_integers(
	struct zegl_context *context,
	struct gles_state *state,
	GLenum pname,
	GLint *values)
{
	struct zegl_config *config;
	struct gles_texture *texture;
	int *flag;
	int status;

	/* A capability is an integer 0 or 1. */
	status = gles_capability(state, pname, &flag);
	if (status == 0) {
		values[0] = *flag;
		return 1U;
	}

	/* The rest, one by one. */
	config = context->config;
	switch (pname) {
	case GL_VIEWPORT:
		memcpy(values, context->gles.viewport, 4U * sizeof(GLint));
		return 4U;
	case GL_SCISSOR_BOX:
		memcpy(values, state->scissor, 4U * sizeof(GLint));
		return 4U;
	case GL_MAX_VIEWPORT_DIMS:
		values[0] = (GLint)state->limits.maxViewportDimensions[0];
		values[1] = (GLint)state->limits.maxViewportDimensions[1];
		return 2U;
	case GL_MAX_TEXTURE_SIZE:
	case GL_MAX_RENDERBUFFER_SIZE:
	case GL_MAX_CUBE_MAP_TEXTURE_SIZE:
		values[0] = (GLint)state->limits.maxImageDimension2D;
		if (values[0] > 16384)
			values[0] = 16384;
		return 1U;
	case GL_MAX_VERTEX_ATTRIBS:
		values[0] = (GLint)GLES_ATTRIBS;
		return 1U;
	case GL_MAX_TEXTURE_IMAGE_UNITS:
	case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
	case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
		values[0] = (GLint)GLES_UNITS;
		return 1U;
	case GL_MAX_VERTEX_UNIFORM_VECTORS:
	case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
		values[0] = 256;
		return 1U;
	case GL_MAX_VARYING_VECTORS:
		values[0] = 15;
		return 1U;
	case GL_ARRAY_BUFFER_BINDING:
		values[0] = 0;
		if (state->array_buffer != NULL)
			values[0] = (GLint)state->array_buffer->name;
		return 1U;
	case GL_ELEMENT_ARRAY_BUFFER_BINDING:
		values[0] = 0;
		if (state->element_buffer != NULL)
			values[0] = (GLint)state->element_buffer->name;
		return 1U;
	case GL_CURRENT_PROGRAM:
		values[0] = 0;
		if (state->program != NULL)
			values[0] = (GLint)state->program->name;
		return 1U;
	case GL_ACTIVE_TEXTURE:
		values[0] = (GLint)(GL_TEXTURE0 + state->active_unit);
		return 1U;
	case GL_TEXTURE_BINDING_2D:
		texture = state->units[state->active_unit];
		values[0] = 0;
		if (texture != NULL)
			values[0] = (GLint)texture->name;
		return 1U;
	case GL_FRAMEBUFFER_BINDING:
		values[0] = (GLint)state->framebuffer;
		return 1U;
	case GL_RENDERBUFFER_BINDING:
		values[0] = (GLint)state->renderbuffer;
		return 1U;
	case GL_RED_BITS:
	case GL_GREEN_BITS:
	case GL_BLUE_BITS:
		values[0] = 8;
		return 1U;
	case GL_ALPHA_BITS:
		values[0] = 0;
		if (config != NULL)
			values[0] = config->alpha;
		return 1U;
	case GL_DEPTH_BITS:
		values[0] = 0;
		if (config != NULL)
			values[0] = config->depth;
		return 1U;
	case GL_STENCIL_BITS:
		values[0] = 0;
		if (config != NULL)
			values[0] = config->stencil;
		return 1U;
	case GL_SUBPIXEL_BITS:
		values[0] = 4;
		return 1U;
	case GL_SAMPLE_BUFFERS:
	case GL_SAMPLES:
	case GL_NUM_COMPRESSED_TEXTURE_FORMATS:
	case GL_COMPRESSED_TEXTURE_FORMATS:
		values[0] = 0;
		return 1U;
	case GL_UNPACK_ALIGNMENT:
		values[0] = state->unpack_alignment;
		return 1U;
	case GL_PACK_ALIGNMENT:
		values[0] = state->pack_alignment;
		return 1U;
	case GL_NUM_SHADER_BINARY_FORMATS:
		values[0] = 1;
		return 1U;
	case GL_SHADER_BINARY_FORMATS:
		values[0] = GL_SHADER_BINARY_FORMAT_SPIR_V;
		return 1U;
	case GL_SHADER_COMPILER:
		values[0] = GL_FALSE;
		return 1U;
	case GL_IMPLEMENTATION_COLOR_READ_FORMAT:
		values[0] = GL_RGBA;
		return 1U;
	case GL_IMPLEMENTATION_COLOR_READ_TYPE:
		values[0] = GL_UNSIGNED_BYTE;
		return 1U;
	case GL_BLEND_SRC_RGB:
		values[0] = (GLint)state->blend_src_rgb;
		return 1U;
	case GL_BLEND_DST_RGB:
		values[0] = (GLint)state->blend_dst_rgb;
		return 1U;
	case GL_BLEND_SRC_ALPHA:
		values[0] = (GLint)state->blend_src_alpha;
		return 1U;
	case GL_BLEND_DST_ALPHA:
		values[0] = (GLint)state->blend_dst_alpha;
		return 1U;
	case GL_BLEND_EQUATION_RGB:
		values[0] = (GLint)state->blend_equation_rgb;
		return 1U;
	case GL_BLEND_EQUATION_ALPHA:
		values[0] = (GLint)state->blend_equation_alpha;
		return 1U;
	case GL_DEPTH_FUNC:
		values[0] = (GLint)state->depth_func;
		return 1U;
	case GL_STENCIL_FUNC:
		values[0] = (GLint)state->stencil_func[0];
		return 1U;
	case GL_STENCIL_REF:
		values[0] = state->stencil_ref[0];
		return 1U;
	case GL_STENCIL_VALUE_MASK:
		values[0] = (GLint)state->stencil_value_mask[0];
		return 1U;
	case GL_STENCIL_WRITEMASK:
		values[0] = (GLint)state->stencil_write_mask[0];
		return 1U;
	case GL_STENCIL_FAIL:
		values[0] = (GLint)state->stencil_fail[0];
		return 1U;
	case GL_STENCIL_PASS_DEPTH_FAIL:
		values[0] = (GLint)state->stencil_zfail[0];
		return 1U;
	case GL_STENCIL_PASS_DEPTH_PASS:
		values[0] = (GLint)state->stencil_zpass[0];
		return 1U;
	case GL_STENCIL_BACK_FUNC:
		values[0] = (GLint)state->stencil_func[1];
		return 1U;
	case GL_STENCIL_BACK_REF:
		values[0] = state->stencil_ref[1];
		return 1U;
	case GL_STENCIL_BACK_VALUE_MASK:
		values[0] = (GLint)state->stencil_value_mask[1];
		return 1U;
	case GL_STENCIL_BACK_WRITEMASK:
		values[0] = (GLint)state->stencil_write_mask[1];
		return 1U;
	case GL_STENCIL_BACK_FAIL:
		values[0] = (GLint)state->stencil_fail[1];
		return 1U;
	case GL_STENCIL_BACK_PASS_DEPTH_FAIL:
		values[0] = (GLint)state->stencil_zfail[1];
		return 1U;
	case GL_STENCIL_BACK_PASS_DEPTH_PASS:
		values[0] = (GLint)state->stencil_zpass[1];
		return 1U;
	case GL_STENCIL_CLEAR_VALUE:
		values[0] = state->clear_stencil;
		return 1U;
	case GL_CULL_FACE_MODE:
		values[0] = (GLint)state->cull_mode;
		return 1U;
	case GL_FRONT_FACE:
		values[0] = (GLint)state->front_face;
		return 1U;
	case GL_GENERATE_MIPMAP_HINT:
		values[0] = (GLint)state->mipmap_hint;
		return 1U;
	default:
		break;
	}

	/* Not an integer state. */
	return 0U;
}

/* Writes a float state's values; returns how many, 0 when the name is not a float state. */
static unsigned
gles_floats(
	struct zegl_context *context,
	struct gles_state *state,
	GLenum pname,
	GLfloat *values)
{
	unsigned count;

	/* The float states, one by one. */
	switch (pname) {
	case GL_COLOR_CLEAR_VALUE:
		memcpy(values, context->gles.clear_color, 4U * sizeof(GLfloat));
		return 4U;
	case GL_BLEND_COLOR:
		memcpy(values, state->blend_color, 4U * sizeof(GLfloat));
		return 4U;
	case GL_DEPTH_CLEAR_VALUE:
		values[0] = state->clear_depth;
		return 1U;
	case GL_DEPTH_RANGE:
		values[0] = state->depth_near;
		values[1] = state->depth_far;
		return 2U;
	case GL_LINE_WIDTH:
		values[0] = state->line_width;
		return 1U;
	case GL_POLYGON_OFFSET_FACTOR:
		values[0] = state->polygon_factor;
		return 1U;
	case GL_POLYGON_OFFSET_UNITS:
		values[0] = state->polygon_units;
		return 1U;
	case GL_SAMPLE_COVERAGE_VALUE:
		values[0] = state->sample_coverage_value;
		return 1U;
	case GL_SAMPLE_COVERAGE_INVERT:
		values[0] = (GLfloat)state->sample_coverage_invert;
		return 1U;
	case GL_ALIASED_LINE_WIDTH_RANGE:
		values[0] = 1.0f;
		values[1] = 1.0f;
		return 2U;
	case GL_ALIASED_POINT_SIZE_RANGE:
		values[0] = 1.0f;
		values[1] = state->limits.pointSizeRange[1];
		return 2U;
	default:
		break;
	}

	/* The fixed-function layer's states (matrices, lights, ...), when there is the layer. */
	if (gles_fixed == NULL)
		return 0U;
	count = gles_fixed->get(context, pname, values);
	return count;
}
