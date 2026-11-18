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

#include "include/kern/os-name.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * The extensions this library offers (glGetString's list;
 * gles_extension_names has the same names one by one): every device's,
 * then GL_EXT_color_buffer_float on a device that draws into a float
 * format (gles_state's color_buffer_float).
 */
#define GLES_EXTENSIONS_EVERY \
	"GL_OES_element_index_uint GL_OES_texture_npot GL_EXT_texture_format_BGRA8888 GL_EXT_blend_minmax"
#define GLES_EXTENSIONS_FLOAT	GLES_EXTENSIONS_EVERY " GL_EXT_color_buffer_float"

/* The largest uniform block a program may read, in bytes (OpenGL ES 3's minimum). */
#define GLES_UNIFORM_BLOCK_SIZE	16384

/*
 * The extensions this library offers, one name each, for glGetStringi.
 * They are the names of GLES_EXTENSIONS_FLOAT in the same order, and
 * never change; a device without float targets has all but the last.
 */
static const char *const gles_extension_names[] = {
	"GL_OES_element_index_uint",
	"GL_OES_texture_npot",
	"GL_EXT_texture_format_BGRA8888",
	"GL_EXT_blend_minmax",
	"GL_EXT_color_buffer_float"
};

/* The fixed-function layer, NULL without one (libGL sets it before its first context). */
const struct gles_fixed_hooks *gles_fixed;

/* The failures already reported (by the text naming them), so each is said once. */
static const char *gles_reported[16];

static void gles_frame_done(struct zegl_context *context, int finished);
static void gles_frame_closing(struct zegl_context *context);
static void gles_release(struct zegl_context *context);
static int gles_capability(struct gles_state *state, GLenum cap, int **flag);
static unsigned gles_integers(struct zegl_context *context, struct gles_state *state, GLenum pname, GLint *values);
static unsigned gles_integers_es3(struct gles_state *state, GLenum pname, GLint *values);
static unsigned gles_integers_compute(struct gles_state *state, GLenum pname, GLint *values);
static GLint gles_buffer_name(const struct gles_buffer *buffer);
static GLenum gles_draw_buffer(struct gles_state *state, GLenum index);
static GLenum gles_read_buffer(struct gles_state *state);
static GLenum gles_read_pair(struct gles_state *state, GLenum pname);
static void gles_blend_buffer(GLenum target, GLuint index, int on);
static int gles_blend_factors_check(struct zegl_context *context, const struct gles_state *state, const GLenum *factors);
static int gles_blend_equations_check(struct zegl_context *context, GLenum rgb, GLenum alpha);
static void gles_blend_functions_split(struct gles_state *state);
static int gles_version_three(struct zegl_context *context);
static int gles_color_buffer_float(struct gles_state *state);
static unsigned gles_extension_count(const struct gles_state *state);
static int gles_compute_offered(struct zegl_context *context, const VkPhysicalDeviceLimits *limits);
static unsigned gles_desktop_integers(struct gles_state *state, GLenum pname, GLint *values);
static int gles_tess_integers(struct gles_state *state, GLenum pname, GLint *values);
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

	/* Patches of three vertices, default levels of 1 (desktop GL 4.0). */
	state->patch_vertices = 3;
	for (index = 0U; index < 4U; index++)
		state->patch_outer[index] = 1.0f;
	state->patch_inner[0] = 1.0f;
	state->patch_inner[1] = 1.0f;

	/* GL's own clip space: y = 1 at the top, depths from -1 (desktop GL's glClipControl). */
	state->clip_origin = GL_LOWER_LEFT;
	state->clip_depth = GL_NEGATIVE_ONE_TO_ONE;

	/* Lines one wide, dithering on, rows aligned to 4. */
	state->line_width = 1.0f;
	state->dither = 1;
	state->sample_coverage_value = 1.0f;
	state->unpack_alignment = 4;
	state->pack_alignment = 4;
	state->mipmap_hint = GL_DONT_CARE;

	/* The surfaces' framebuffer draws into and reads its one colour buffer. */
	state->default_draw_buffer = GL_BACK;
	state->default_read_buffer = GL_BACK;

	/* The default transform feedback object is bound. */
	state->default_feedback.bound = 1;
	state->feedback = &state->default_feedback;

	/* Whether the context offers OpenGL ES 3.1's compute (ws101-p009), and its image units. */
	state->compute = gles_compute_offered(context, &state->limits);
	state->image_units = gles_image_units_offered(state);

	/* The colour attachments and draw buffers: the device's, at most the library's, at least one. */
	state->color_attachments = GLES_COLOR_ATTACHMENTS;
	if (state->limits.maxColorAttachments < state->color_attachments)
		state->color_attachments = state->limits.maxColorAttachments;
	if (state->color_attachments == 0U)
		state->color_attachments = 1U;

	/* Float targets, and buffer textures with and without their RGB32 formats. */
	state->color_buffer_float = gles_color_buffer_float(state);
	state->texel_buffers = gles_texel_buffers(state->display, 0);
	state->texel_rgb32 = 0;
	if (state->texel_buffers)
		state->texel_rgb32 = gles_texel_buffers(state->display, 1);

	/* Every array four floats, disabled and on its own binding point, every current value the floats (0, 0, 0, 1). */
	for (index = 0U; index < GLES_ATTRIBS; index++) {
		state->attribs[index].size = 4;
		state->attribs[index].type = GL_FLOAT;
		state->attribs[index].value[3] = 1.0f;
		state->attribs[index].value_type = GL_FLOAT;
		state->attribs[index].binding = (GLuint)index;
	}

	/* Succeeded: the first frame, and the callbacks libEGL makes. */
	state->frame = 1U;
	context->gles.state = state;
	context->gles.frame_done = gles_frame_done;
	context->gles.release = gles_release;
	context->gles.frame_closing = gles_frame_closing;
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

	/* Desktop GL's debug output hears every one (libGL). */
	if (gles_fixed != NULL && gles_fixed->error != NULL)
		gles_fixed->error(context, error);
}

/*
 * Says on stderr, once for each kind, that a step of the translation
 * failed (a Vulkan call's result, or -1): what an application sees is only
 * GL_OUT_OF_MEMORY, which does not say which step.
 */
void
gles_report(
	const char *what,
	int code)
{
	unsigned index;

	/* Each kind once. */
	for (index = 0U; index < sizeof(gles_reported) / sizeof(gles_reported[0]); index++) {
		if (gles_reported[index] == what)
			return;
		if (gles_reported[index] == NULL)
			break;
	}

	/* Remembered while there is room. */
	if (index < sizeof(gles_reported) / sizeof(gles_reported[0]))
		gles_reported[index] = what;

	/* The line. */
	fprintf(stderr, "GLES: %s failed (%d)\n", what, code);
}

/*
 * Whether the calls of a compute run are timed (ZEDBSD_GLES_COMPUTE_TRACE=2,
 * ws101-p016): -1 until the environment is read.
 */
static int gles_timing = -1;

/*
 * Starts timing a step: the time in microseconds when steps are timed,
 * else 0 (nothing is timed).
 */
uint64_t
gles_time_begin(void)
{
	const char *setting;
	struct timespec now;
	int level;

	/* The environment, read once: level 2 or more times the steps. */
	if (gles_timing < 0) {
		setting = getenv("ZEDBSD_GLES_COMPUTE_TRACE");
		level = 0;
		if (setting != NULL)
			level = atoi(setting);
		gles_timing = 0;
		if (level >= 2)
			gles_timing = 1;
	}

	/* Nothing timed. */
	if (!gles_timing)
		return 0U;

	/* The monotonic time. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/*
 * Ends timing a step begun by gles_time_begin: one line on stderr with its
 * name, its microseconds and the bytes it moved (0 when none).
 */
void
gles_time_end(
	const char *step,
	uint64_t started,
	size_t bytes)
{
	uint64_t ended;

	/* Nothing was timed. */
	if (started == 0U)
		return;

	/* The line. */
	ended = gles_time_begin();
	fprintf(stderr, "gles: time step=%s us=%llu bytes=%lu\n", step, (unsigned long long)(ended - started), (unsigned long)bytes);
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

	/* On (blending: on every draw buffer again). */
	*flag = 1;
	if (cap == GL_BLEND)
		state->blend_indexed = 0;
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

	/* Off (blending: on every draw buffer again). */
	*flag = 0;
	if (cap == GL_BLEND)
		state->blend_indexed = 0;
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
 * Sets the blend factors of colour and alpha (of every draw buffer).
 */
GL_APICALL void GL_APIENTRY
glBlendFuncSeparate(
	GLenum sfactorRGB,
	GLenum dfactorRGB,
	GLenum sfactorAlpha,
	GLenum dfactorAlpha)
{
	struct zegl_context *context;
	struct gles_state *state;
	GLenum factors[4];
	unsigned index;
	int status;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* Factors a draw may blend with. */
	factors[0] = sfactorRGB;
	factors[1] = dfactorRGB;
	factors[2] = sfactorAlpha;
	factors[3] = dfactorAlpha;
	status = gles_blend_factors_check(context, state, factors);
	if (status != 0)
		return;

	/* The four factors. */
	state->blend_src_rgb = sfactorRGB;
	state->blend_dst_rgb = dfactorRGB;
	state->blend_src_alpha = sfactorAlpha;
	state->blend_dst_alpha = dfactorAlpha;

	/* Each draw buffer's own, once they were set apart (their equations stay). */
	for (index = 0U; state->functions_indexed && index < GLES_DRAW_BUFFERS; index++) {
		state->indexed_functions[index].src_rgb = sfactorRGB;
		state->indexed_functions[index].dst_rgb = dfactorRGB;
		state->indexed_functions[index].src_alpha = sfactorAlpha;
		state->indexed_functions[index].dst_alpha = dfactorAlpha;
	}
}

/*
 * Sets the blend factors of colour and alpha for one draw buffer (desktop
 * GL 4.0).
 */
GL_APICALL void GL_APIENTRY
glBlendFuncSeparatei(
	GLuint buf,
	GLenum srcRGB,
	GLenum dstRGB,
	GLenum srcAlpha,
	GLenum dstAlpha)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_blend_function *function;
	GLenum factors[4];
	int status;

	/* A context with its state, and a draw buffer. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (buf >= state->color_attachments) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Factors a draw may blend with. */
	factors[0] = srcRGB;
	factors[1] = dstRGB;
	factors[2] = srcAlpha;
	factors[3] = dstAlpha;
	status = gles_blend_factors_check(context, state, factors);
	if (status != 0)
		return;

	/* The buffer's own factors (buffer 0's are the shared ones glGet reports). */
	gles_blend_functions_split(state);
	function = &state->indexed_functions[buf];
	function->src_rgb = srcRGB;
	function->dst_rgb = dstRGB;
	function->src_alpha = srcAlpha;
	function->dst_alpha = dstAlpha;
	if (buf == 0U) {
		state->blend_src_rgb = srcRGB;
		state->blend_dst_rgb = dstRGB;
		state->blend_src_alpha = srcAlpha;
		state->blend_dst_alpha = dstAlpha;
	}
}

/*
 * Sets the blend factors of colour and alpha together for one draw buffer
 * (desktop GL 4.0).
 */
GL_APICALL void GL_APIENTRY
glBlendFunci(
	GLuint buf,
	GLenum src,
	GLenum dst)
{
	/* The same for both. */
	glBlendFuncSeparatei(buf, src, dst, src, dst);
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
 * Sets the blend equations of colour and alpha (of every draw buffer).
 */
GL_APICALL void GL_APIENTRY
glBlendEquationSeparate(
	GLenum modeRGB,
	GLenum modeAlpha)
{
	struct zegl_context *context;
	struct gles_state *state;
	unsigned index;
	int status;

	/* A context with its state, and equations there are. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	status = gles_blend_equations_check(context, modeRGB, modeAlpha);
	if (status != 0)
		return;

	/* The two equations. */
	state->blend_equation_rgb = modeRGB;
	state->blend_equation_alpha = modeAlpha;

	/* Each draw buffer's own, once they were set apart (their factors stay). */
	for (index = 0U; state->functions_indexed && index < GLES_DRAW_BUFFERS; index++) {
		state->indexed_functions[index].equation_rgb = modeRGB;
		state->indexed_functions[index].equation_alpha = modeAlpha;
	}
}

/*
 * Sets the blend equations of colour and alpha for one draw buffer
 * (desktop GL 4.0).
 */
GL_APICALL void GL_APIENTRY
glBlendEquationSeparatei(
	GLuint buf,
	GLenum modeRGB,
	GLenum modeAlpha)
{
	struct zegl_context *context;
	struct gles_state *state;
	int status;

	/* A context with its state, a draw buffer, and equations there are. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (buf >= state->color_attachments) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}
	status = gles_blend_equations_check(context, modeRGB, modeAlpha);
	if (status != 0)
		return;

	/* The buffer's own equations (buffer 0's are the shared ones glGet reports). */
	gles_blend_functions_split(state);
	state->indexed_functions[buf].equation_rgb = modeRGB;
	state->indexed_functions[buf].equation_alpha = modeAlpha;
	if (buf == 0U) {
		state->blend_equation_rgb = modeRGB;
		state->blend_equation_alpha = modeAlpha;
	}
}

/*
 * Sets the blend equation of colour and alpha together for one draw
 * buffer (desktop GL 4.0).
 */
GL_APICALL void GL_APIENTRY
glBlendEquationi(
	GLuint buf,
	GLenum mode)
{
	/* The same for both. */
	glBlendEquationSeparatei(buf, mode, mode);
}

/*
 * Reports a draw buffer's blend factor or equation (glGetIntegeri_v of
 * desktop GL 4.0): 1 with *value for a blend name, 0 for another name,
 * -1 with the error recorded for a draw buffer there is not.
 */
int
gles_blend_indexed(
	struct zegl_context *context,
	const struct gles_state *state,
	GLenum target,
	GLuint index,
	GLint64 *value)
{
	struct gles_blend_function function;

	/* Desktop GL's only. */
	if (gles_fixed == NULL)
		return 0;

	/* A blend name, of a draw buffer there is. */
	switch (target) {
	case GL_BLEND_SRC_RGB:
	case GL_BLEND_DST_RGB:
	case GL_BLEND_SRC_ALPHA:
	case GL_BLEND_DST_ALPHA:
	case GL_BLEND_EQUATION_RGB:
	case GL_BLEND_EQUATION_ALPHA:
		break;
	default:
		return 0;
	}
	if (index >= state->color_attachments) {
		gles_error(context, GL_INVALID_VALUE);
		return -1;
	}

	/* The buffer's factors and equations. */
	gles_blend_function_of(state, index, &function);

	/* The one asked for. */
	switch (target) {
	case GL_BLEND_SRC_RGB:
		*value = (GLint64)function.src_rgb;
		break;
	case GL_BLEND_DST_RGB:
		*value = (GLint64)function.dst_rgb;
		break;
	case GL_BLEND_SRC_ALPHA:
		*value = (GLint64)function.src_alpha;
		break;
	case GL_BLEND_DST_ALPHA:
		*value = (GLint64)function.dst_alpha;
		break;
	case GL_BLEND_EQUATION_RGB:
		*value = (GLint64)function.equation_rgb;
		break;
	default:
		*value = (GLint64)function.equation_alpha;
		break;
	}

	/* Succeeded: a blend name's value. */
	return 1;
}

/*
 * Gives a draw buffer's blend factors and equations: its own once they
 * were set apart, else the shared ones.
 */
void
gles_blend_function_of(
	const struct gles_state *state,
	unsigned index,
	struct gles_blend_function *function)
{
	/* The buffer's own. */
	if (state->functions_indexed && index < GLES_DRAW_BUFFERS) {
		*function = state->indexed_functions[index];
		return;
	}

	/* Succeeded: the shared ones. */
	function->src_rgb = state->blend_src_rgb;
	function->dst_rgb = state->blend_dst_rgb;
	function->src_alpha = state->blend_src_alpha;
	function->dst_alpha = state->blend_dst_alpha;
	function->equation_rgb = state->blend_equation_rgb;
	function->equation_alpha = state->blend_equation_alpha;
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

	/* Every draw buffer alike again. */
	state->indexed_masked = 0;
}

/*
 * Sets which colour channels draws write into one draw buffer.
 */
GL_APICALL void GL_APIENTRY
glColorMaski(
	GLuint buf,
	GLboolean red,
	GLboolean green,
	GLboolean blue,
	GLboolean alpha)
{
	struct zegl_context *context;
	struct gles_state *state;
	unsigned index;

	/* A context with its state, and a draw buffer. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (buf >= state->color_attachments) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each buffer starts from the shared mask the first time. */
	if (!state->indexed_masked) {
		for (index = 0U; index < GLES_DRAW_BUFFERS; index++)
			memcpy(state->indexed_masks[index], state->color_mask, sizeof(state->color_mask));
		state->indexed_masked = 1;
	}

	/* The buffer's channels (buffer 0's are the shared mask glGet reports). */
	state->indexed_masks[buf][0] = red;
	state->indexed_masks[buf][1] = green;
	state->indexed_masks[buf][2] = blue;
	state->indexed_masks[buf][3] = alpha;
	if (buf == 0U)
		memcpy(state->color_mask, state->indexed_masks[0], sizeof(state->color_mask));
}

/*
 * Turns blending on for one draw buffer.
 */
GL_APICALL void GL_APIENTRY
glEnablei(
	GLenum target,
	GLuint index)
{
	/* Blending on. */
	gles_blend_buffer(target, index, 1);
}

/*
 * Turns blending off for one draw buffer.
 */
GL_APICALL void GL_APIENTRY
glDisablei(
	GLenum target,
	GLuint index)
{
	/* Blending off. */
	gles_blend_buffer(target, index, 0);
}

/*
 * Reports whether blending is on for one draw buffer.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsEnabledi(
	GLenum target,
	GLuint index)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, and blending. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return GL_FALSE;
	if (target != GL_BLEND) {
		gles_error(context, GL_INVALID_ENUM);
		return GL_FALSE;
	}

	/* A draw buffer there is. */
	if (index >= state->color_attachments) {
		gles_error(context, GL_INVALID_VALUE);
		return GL_FALSE;
	}

	/* The buffer's own, once one was set. */
	if (state->blend_indexed) {
		if ((state->blend_buffers & (1U << index)) != 0U)
			return GL_TRUE;
		return GL_FALSE;
	}

	/* The shared one. */
	if (state->blend)
		return GL_TRUE;

	/* Off. */
	return GL_FALSE;
}

/*
 * Reports an indexed boolean state: a draw buffer's colour mask, or an
 * indexed integer state (a buffer binding) as a boolean.
 */
GL_APICALL void GL_APIENTRY
glGetBooleani_v(
	GLenum target,
	GLuint index,
	GLboolean *data)
{
	struct zegl_context *context;
	struct gles_state *state;
	GLint value;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* An indexed integer state (glGetIntegeri_v reports a name that is not one). */
	if (target != GL_COLOR_WRITEMASK) {
		value = 0;
		glGetIntegeri_v(target, index, &value);
		data[0] = GL_FALSE;
		if (value != 0)
			data[0] = GL_TRUE;
		return;
	}

	/* A draw buffer. */
	if (index >= state->color_attachments) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The buffer's own, or the shared one. */
	if (state->indexed_masked) {
		memcpy(data, state->indexed_masks[index], 4U);
		return;
	}

	/* Succeeded: the shared mask. */
	memcpy(data, state->color_mask, 4U);
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
	GLint *value;
	int alignment;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* The parameter's place: the alignments, and OpenGL ES 3's lengths and skips. */
	alignment = 0;
	switch (pname) {
	case GL_UNPACK_ALIGNMENT:
		value = &state->unpack_alignment;
		alignment = 1;
		break;
	case GL_PACK_ALIGNMENT:
		value = &state->pack_alignment;
		alignment = 1;
		break;
	case GL_UNPACK_ROW_LENGTH:
		value = &state->unpack_row_length;
		break;
	case GL_UNPACK_SKIP_ROWS:
		value = &state->unpack_skip_rows;
		break;
	case GL_UNPACK_SKIP_PIXELS:
		value = &state->unpack_skip_pixels;
		break;
	case GL_UNPACK_IMAGE_HEIGHT:
		value = &state->unpack_image_height;
		break;
	case GL_UNPACK_SKIP_IMAGES:
		value = &state->unpack_skip_images;
		break;
	case GL_PACK_ROW_LENGTH:
		value = &state->pack_row_length;
		break;
	case GL_PACK_SKIP_ROWS:
		value = &state->pack_skip_rows;
		break;
	case GL_PACK_SKIP_PIXELS:
		value = &state->pack_skip_pixels;
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* An alignment of 1, 2, 4 or 8. */
	if (alignment && param != 1 && param != 2 && param != 4 && param != 8) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A length or a skip that is not negative. */
	if (param < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the parameter. */
	*value = param;
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
 * Reports integer states as 64-bit integers.
 */
GL_APICALL void GL_APIENTRY
glGetInteger64v(
	GLenum pname,
	GLint64 *data)
{
	struct zegl_context *context;
	struct gles_state *state;
	GLint integers[16];
	GLfloat floats[16];
	unsigned count;
	unsigned index;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL || data == NULL)
		return;

	/* The states only 64 bits hold: the largest index and wait. */
	if (pname == GL_MAX_ELEMENT_INDEX) {
		data[0] = (GLint64)state->limits.maxDrawIndexedIndexValue;
		return;
	}

	/* The GPU's time (desktop GL 3.3's timer queries, libGL), of a queue that writes timestamps. */
	if (pname == GL_TIMESTAMP &&
	    gles_fixed != NULL &&
	    state->display->timestamp_bits != 0U) {
		(void)gles_query_timestamp(context, state, data);
		return;
	}

	/* A server wait (glClientWaitSync) is never refused for its length. */
	if (pname == GL_MAX_SERVER_WAIT_TIMEOUT) {
		data[0] = INT64_MAX;
		return;
	}

	/* The largest shader storage block, the device's largest storage buffer range (ws101-p009). */
	if (pname == GL_MAX_SHADER_STORAGE_BLOCK_SIZE && state->compute) {
		data[0] = (GLint64)state->limits.maxStorageBufferRange;
		return;
	}

	/* An integer state. */
	count = gles_integers(context, state, pname, integers);
	for (index = 0U; index < count; index++)
		data[index] = integers[index];
	if (count != 0U)
		return;

	/* A float state, rounded. */
	count = gles_floats(context, state, pname, floats);
	for (index = 0U; index < count; index++)
		data[index] = (GLint64)(floats[index] + 0.5f);
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
	struct gles_state *state;
	const GLubyte *layer;
	int three;
	int compute;

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

	/*
	 * The string asked for: OpenGL ES 3.0's when the device captures
	 * outputs, 3.1's when the context offers compute too (ws101-p009: the
	 * compute part of 3.1; the rest of 3.1's calls record errors).
	 */
	three = gles_version_three(context);
	state = gles_state(context);
	compute = 0;
	if (state != NULL)
		compute = state->compute;
	switch (name) {
	case GL_VENDOR:
		return (const GLubyte *)ZEDBSD_OS_NAME;
	case GL_RENDERER:
		return (const GLubyte *)ZEDBSD_OS_NAME " OpenGL ES on Vulkan";
	case GL_VERSION:
		if (compute)
			return (const GLubyte *)"OpenGL ES 3.1 " ZEDBSD_OS_NAME;
		if (three)
			return (const GLubyte *)"OpenGL ES 3.0 " ZEDBSD_OS_NAME;
		return (const GLubyte *)"OpenGL ES 2.0 " ZEDBSD_OS_NAME;
	case GL_SHADING_LANGUAGE_VERSION:
		if (compute)
			return (const GLubyte *)"OpenGL ES GLSL ES 3.10";
		if (three)
			return (const GLubyte *)"OpenGL ES GLSL ES 3.00";
		return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
	case GL_EXTENSIONS:
		if (state != NULL && state->color_buffer_float)
			return (const GLubyte *)GLES_EXTENSIONS_FLOAT;
		return (const GLubyte *)GLES_EXTENSIONS_EVERY;
	default:
		break;
	}

	/* Any other name is an error. */
	gles_error(context, GL_INVALID_ENUM);
	return NULL;
}

/*
 * Returns one of the extensions' names (the only indexed string).
 */
GL_APICALL const GLubyte *GL_APIENTRY
glGetStringi(
	GLenum name,
	GLuint index)
{
	struct zegl_context *context;
	struct gles_state *state;
	const char *extension;
	unsigned names;
	int count;

	/* Without a current context there are no strings. */
	context = gles_context();
	if (context == NULL)
		return NULL;

	/* The extensions are the only list. */
	if (name != GL_EXTENSIONS) {
		gles_error(context, GL_INVALID_ENUM);
		return NULL;
	}

	/* Desktop GL's own list (libGL), when the context has one. */
	if (gles_fixed != NULL) {
		count = gles_fixed->extension_count();
		if (count >= 0 && index >= (GLuint)count) {
			gles_error(context, GL_INVALID_VALUE);
			return NULL;
		}

		/* The name at the index. */
		if (count >= 0) {
			extension = gles_fixed->extension(index);
			return (const GLubyte *)extension;
		}
	}

	/* An index inside the list the device has. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;
	names = gles_extension_count(state);
	if (index >= names) {
		gles_error(context, GL_INVALID_VALUE);
		return NULL;
	}

	/* Succeeded: the name. */
	return (const GLubyte *)gles_extension_names[index];
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
	struct zegl_context *context;
	struct gles_state *state;

	/* Without a context or its state nothing was submitted. */
	context = zegl_current_context();
	if (context == NULL || context->gles.state == NULL)
		return;

	/* The frames a window presented may still run (ws068-p009): they are waited for. */
	state = context->gles.state;
	(void)gles_frame_retire(context, state);
}

/*
 * Counts the next frame once eglSwapBuffers ended one (finished: that
 * frame is done too) and frees what the frames done held.  A window's
 * presented frame may still run (ws068-p009); a surface's slots cover
 * only its own frames (and a pbuffer's empty frame submits nothing), so a
 * frame another surface left in flight is waited for first.
 */
static void
gles_frame_done(
	struct zegl_context *context,
	int finished)
{
	struct gles_state *state;
	EGLint error;

	/* The state, when there is one. */
	state = context->gles.state;
	if (state == NULL)
		return;

	/* Another surface's frame in flight: everything submitted is waited for. */
	if (state->flight != NULL && state->flight != context->draw) {
		error = zegl_retire(state->display);
		if (error == EGL_SUCCESS)
			finished = 1;
	}

	/* The next frame; the memory of the frames done is free. */
	gles_frame_finished(state, finished);
	if (!finished)
		state->flight = context->draw;
}

/* Ends a framebuffer object's render pass left open in the frame, before libEGL submits the frame or the context stops being current. */
static void
gles_frame_closing(
	struct zegl_context *context)
{
	struct gles_state *state;

	/* The state, when there is one. */
	state = context->gles.state;
	if (state == NULL)
		return;

	/* An occlusion query's segment, and the pass, if one is open. */
	gles_queries_suspend(state);
	gles_target_close(state);
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
	unsigned kind;
	unsigned shape;

	/* The state, when there is one; nothing may still run. */
	state = context->gles.state;
	if (state == NULL)
		return;
	(void)vkDeviceWaitIdle(state->device);

	/* The fixed-function layer's state (its programs are among the objects freed below). */
	if (gles_fixed != NULL)
		gles_fixed->release(state);

	/* The program pipeline objects, the programs, then the shaders left. */
	state->program = NULL;
	gles_pipelines_release(state);
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

	/* The black textures of each shape and kind, and the black cube map arrays. */
	for (shape = 0U; shape < GLES_SHAPES; shape++) {
		for (kind = 0U; kind < GLES_BLACK_KINDS; kind++) {
			if (state->blacks[shape][kind] != NULL)
				gles_texture_free(state, state->blacks[shape][kind]);
		}
	}
	for (kind = 0U; kind < GLES_BLACK_KINDS; kind++) {
		if (state->black_cube_arrays[kind] != NULL)
			gles_texture_free(state, state->black_cube_arrays[kind]);
	}

	/* The black multisample textures. */
	for (kind = 0U; kind < 3U; kind++) {
		if (state->black_ms[kind] != NULL)
			gles_texture_free(state, state->black_ms[kind]);
	}

	/* The 1x1 textures image units are read as (image load and store). */
	for (shape = 0U; shape < GLES_SHAPES; shape++) {
		for (kind = 0U; kind < GLES_IMAGE_FORMATS; kind++) {
			if (state->image_blanks[shape][kind] != NULL)
				gles_texture_free(state, state->image_blanks[shape][kind]);
		}
	}

	/* The black texel buffer's views (buffer textures; nothing runs any more). */
	for (kind = 0U; kind < 3U; kind++) {
		if (state->black_buffer_views[kind] != VK_NULL_HANDLE)
			vkDestroyBufferView(state->device, state->black_buffer_views[kind], NULL);
	}

	/* The buffer, after its views. */
	if (state->black_buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(state->device, state->black_buffer, NULL);
		vkFreeMemory(state->device, state->black_buffer_memory, NULL);
	}

	/* The sampler objects, the query objects and fence syncs, and the transform feedback objects. */
	gles_samplers_release(state);
	gles_queries_release(state);
	gles_feedbacks_release(state);

	/* The framebuffer objects and renderbuffers, and the vertex array objects. */
	gles_framebuffers_release(state);
	gles_vertex_arrays_release(state);

	/* The garbage, which the frees above added to, and the spare device copies it left (ws101-p017); nothing runs. */
	state->done = state->frame;
	gles_collect(state);
	gles_spares_release(state);

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
	case GL_PRIMITIVE_RESTART_FIXED_INDEX:
		*flag = &state->primitive_restart;
		return 0;
	case GL_RASTERIZER_DISCARD:
		*flag = &state->rasterizer_discard;
		return 0;
	case GL_SAMPLE_MASK:
		/* Desktop GL 3.2's and OpenGL ES 3.1's (a context that offers compute). */
		if (gles_fixed == NULL && !state->compute)
			return -1;
		*flag = &state->sample_mask;
		return 0;
	default:
		break;
	}

	/* Without the fixed-function layer there are no more (OpenGL ES). */
	if (gles_fixed == NULL)
		return -1;

	/* Desktop GL's primitive restart, depth clamping and seamless cube maps (libGL). */
	switch (cap) {
	case GL_PRIMITIVE_RESTART:
		*flag = &state->primitive_restart_any;
		return 0;
	case GL_DEPTH_CLAMP:
		/* Only a device that clamps (GL_ARB_depth_clamp, desktop GL 3.2): not taken and ignored. */
		if (!state->display->features.depthClamp)
			return -1;
		*flag = &state->depth_clamp;
		return 0;
	case GL_TEXTURE_CUBE_MAP_SEAMLESS:
		*flag = &state->cube_seamless;
		return 0;
	default:
		break;
	}

	/* One of the fixed-function layer's. */
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
	uint32_t samples;
	int *flag;
	unsigned count;
	int status;
	int three;

	/* A capability is an integer 0 or 1. */
	status = gles_capability(state, pname, &flag);
	if (status == 0) {
		values[0] = *flag;
		return 1U;
	}

	/* Desktop GL's version, context flags and profile are the fixed-function layer's (libGL). */
	if (gles_fixed != NULL &&
	    (pname == GL_MAJOR_VERSION ||
	     pname == GL_MINOR_VERSION ||
	     pname == GL_CONTEXT_FLAGS ||
	     pname == GL_CONTEXT_PROFILE_MASK))
		return 0U;

	/* Desktop GL's own integer states (libGL). */
	if (gles_fixed != NULL) {
		count = gles_desktop_integers(state, pname, values);
		if (count != 0U)
			return count;
	}

	/* The rest, one by one. */
	config = context->config;
	switch (pname) {
	case GL_MAJOR_VERSION:
		three = gles_version_three(context);
		values[0] = 2;
		if (three)
			values[0] = 3;
		return 1U;
	case GL_MINOR_VERSION:
		values[0] = state->compute;
		return 1U;
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
	case GL_TEXTURE_BINDING_CUBE_MAP:
		texture = state->cube_units[state->active_unit];
		values[0] = 0;
		if (texture != NULL)
			values[0] = (GLint)texture->name;
		return 1U;
	case GL_TEXTURE_BINDING_3D:
		texture = state->volume_units[state->active_unit];
		values[0] = 0;
		if (texture != NULL)
			values[0] = (GLint)texture->name;
		return 1U;
	case GL_TEXTURE_BINDING_2D_ARRAY:
		texture = state->array_units[state->active_unit];
		values[0] = 0;
		if (texture != NULL)
			values[0] = (GLint)texture->name;
		return 1U;
	case GL_MAX_3D_TEXTURE_SIZE:
		values[0] = (GLint)state->limits.maxImageDimension3D;
		if (values[0] > GLES_MAX_3D_SIZE)
			values[0] = GLES_MAX_3D_SIZE;
		return 1U;
	case GL_MAX_ARRAY_TEXTURE_LAYERS:
		values[0] = (GLint)state->limits.maxImageArrayLayers;
		if (values[0] > GLES_MAX_LAYERS)
			values[0] = GLES_MAX_LAYERS;
		return 1U;
	case GL_FRAMEBUFFER_BINDING:
		values[0] = (GLint)state->framebuffer;
		return 1U;
	case GL_READ_FRAMEBUFFER_BINDING:
		values[0] = (GLint)state->read_framebuffer;
		return 1U;
	case GL_MAX_COLOR_ATTACHMENTS:
	case GL_MAX_DRAW_BUFFERS:
		/* The device's (gles_state), at most the library's. */
		values[0] = (GLint)state->color_attachments;
		return 1U;
	case GL_MAX_DUAL_SOURCE_DRAW_BUFFERS:
		/* Desktop GL 3.3's dual-source blending: one draw buffer when the device blends so. */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = 0;
		if (state->display->features.dualSrcBlend)
			values[0] = 1;
		return 1U;
	case GL_DRAW_BUFFER0:
	case GL_DRAW_BUFFER1:
	case GL_DRAW_BUFFER2:
	case GL_DRAW_BUFFER3:
		values[0] = (GLint)gles_draw_buffer(state, pname - GL_DRAW_BUFFER0);
		return 1U;
	case GL_READ_BUFFER:
		values[0] = (GLint)gles_read_buffer(state);
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
	case GL_SAMPLES:
		values[0] = (GLint)gles_framebuffer_samples(state, state->framebuffer);
		return 1U;
	case GL_SAMPLE_BUFFERS:
		samples = gles_framebuffer_samples(state, state->framebuffer);
		values[0] = 0;
		if (samples > 1U)
			values[0] = 1;
		return 1U;
	case GL_MAX_SAMPLES:
		values[0] = (GLint)gles_samples_max(state);
		return 1U;
	case GL_NUM_COMPRESSED_TEXTURE_FORMATS:
		/* OpenGL ES 3.0's ETC2 and EAC formats (decoded on the CPU). */
		values[0] = (GLint)gles_compressed_formats(NULL);
		return 1U;
	case GL_COMPRESSED_TEXTURE_FORMATS:
		return gles_compressed_formats(values);
	case GL_UNPACK_ALIGNMENT:
		values[0] = state->unpack_alignment;
		return 1U;
	case GL_PACK_ALIGNMENT:
		values[0] = state->pack_alignment;
		return 1U;
	case GL_UNPACK_ROW_LENGTH:
		values[0] = state->unpack_row_length;
		return 1U;
	case GL_UNPACK_SKIP_ROWS:
		values[0] = state->unpack_skip_rows;
		return 1U;
	case GL_UNPACK_SKIP_PIXELS:
		values[0] = state->unpack_skip_pixels;
		return 1U;
	case GL_UNPACK_IMAGE_HEIGHT:
		values[0] = state->unpack_image_height;
		return 1U;
	case GL_UNPACK_SKIP_IMAGES:
		values[0] = state->unpack_skip_images;
		return 1U;
	case GL_PACK_ROW_LENGTH:
		values[0] = state->pack_row_length;
		return 1U;
	case GL_PACK_SKIP_ROWS:
		values[0] = state->pack_skip_rows;
		return 1U;
	case GL_PACK_SKIP_PIXELS:
		values[0] = state->pack_skip_pixels;
		return 1U;
	case GL_NUM_SHADER_BINARY_FORMATS:
		values[0] = 1;
		return 1U;
	case GL_NUM_PROGRAM_BINARY_FORMATS:
		values[0] = 0;
		return 1U;
	case GL_SHADER_BINARY_FORMATS:
		values[0] = GL_SHADER_BINARY_FORMAT_SPIR_V;
		return 1U;
	case GL_SHADER_COMPILER:
		values[0] = GL_TRUE;
		return 1U;
	case GL_IMPLEMENTATION_COLOR_READ_FORMAT:
	case GL_IMPLEMENTATION_COLOR_READ_TYPE:
		values[0] = (GLint)gles_read_pair(state, pname);
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

	/* One of OpenGL ES 3's, or not an integer state. */
	count = gles_integers_es3(state, pname, values);
	return count;
}

/* Writes one of OpenGL ES 3's integer states (buffers, vertex arrays, uniform blocks, limits); returns how many, 0 when the name is not one. */
static unsigned
gles_integers_es3(
	struct gles_state *state,
	GLenum pname,
	GLint *values)
{
	/* The states, one by one. */
	switch (pname) {
	case GL_VERTEX_ARRAY_BINDING:
		values[0] = (GLint)state->vertex_array;
		return 1U;
	case GL_SAMPLER_BINDING:
		values[0] = 0;
		if (state->unit_samplers[state->active_unit] != NULL)
			values[0] = (GLint)state->unit_samplers[state->active_unit]->name;
		return 1U;
	case GL_COPY_READ_BUFFER_BINDING:
		values[0] = gles_buffer_name(state->copy_read_buffer);
		return 1U;
	case GL_COPY_WRITE_BUFFER_BINDING:
		values[0] = gles_buffer_name(state->copy_write_buffer);
		return 1U;
	case GL_UNIFORM_BUFFER_BINDING:
		values[0] = gles_buffer_name(state->uniform_buffer);
		return 1U;
	case GL_PIXEL_PACK_BUFFER_BINDING:
		values[0] = gles_buffer_name(state->pixel_pack_buffer);
		return 1U;
	case GL_PIXEL_UNPACK_BUFFER_BINDING:
		values[0] = gles_buffer_name(state->pixel_unpack_buffer);
		return 1U;
	case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
		values[0] = gles_buffer_name(state->feedback_buffer);
		return 1U;
	case GL_TRANSFORM_FEEDBACK_BINDING:
		values[0] = (GLint)state->feedback->name;
		return 1U;
	case GL_TRANSFORM_FEEDBACK_ACTIVE:
		values[0] = state->feedback->active;
		return 1U;
	case GL_TRANSFORM_FEEDBACK_PAUSED:
		values[0] = state->feedback->paused;
		return 1U;
	case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS:
		values[0] = (GLint)GLES_CAPTURE_COMPONENTS;
		return 1U;
	case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS:
		values[0] = (GLint)GLES_FEEDBACK_BINDINGS;
		return 1U;
	case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS:
		values[0] = 4;
		return 1U;
	case GL_MAX_UNIFORM_BUFFER_BINDINGS:
		values[0] = (GLint)GLES_UNIFORM_BINDINGS;
		return 1U;
	case GL_MAX_UNIFORM_BLOCK_SIZE:
		values[0] = GLES_UNIFORM_BLOCK_SIZE;
		if ((GLint64)state->limits.maxUniformBufferRange > GLES_UNIFORM_BLOCK_SIZE)
			values[0] = (GLint)state->limits.maxUniformBufferRange;
		return 1U;
	case GL_MAX_VERTEX_UNIFORM_BLOCKS:
	case GL_MAX_FRAGMENT_UNIFORM_BLOCKS:
		values[0] = (GLint)GLES_STAGE_BLOCKS;
		return 1U;
	case GL_MAX_COMBINED_UNIFORM_BLOCKS:
		values[0] = (GLint)GLES_NAMED_BLOCKS;
		return 1U;
	case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT:
		values[0] = (GLint)state->limits.minUniformBufferOffsetAlignment;
		return 1U;
	case GL_MAX_VERTEX_UNIFORM_COMPONENTS:
	case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:
		values[0] = 1024;
		return 1U;
	case GL_MAX_VERTEX_OUTPUT_COMPONENTS:
	case GL_MAX_FRAGMENT_INPUT_COMPONENTS:
		values[0] = 64;
		return 1U;
	case GL_MAX_VARYING_COMPONENTS:
		values[0] = 60;
		return 1U;
	case GL_MAX_ELEMENTS_VERTICES:
	case GL_MAX_ELEMENTS_INDICES:
		values[0] = 1048576;
		return 1U;
	case GL_MAX_ELEMENT_INDEX:
		values[0] = INT32_MAX;
		if (state->limits.maxDrawIndexedIndexValue < (uint32_t)INT32_MAX)
			values[0] = (GLint)state->limits.maxDrawIndexedIndexValue;
		return 1U;
	case GL_NUM_EXTENSIONS:
		values[0] = (GLint)gles_extension_count(state);
		return 1U;
	default:
		break;
	}

	/* OpenGL ES 3.1's compute states, in a context that offers it. */
	if (state->compute)
		return gles_integers_compute(state, pname, values);

	/* Not an integer state. */
	return 0U;
}

/*
 * Writes one of OpenGL ES 3.1's states (ws101-p009: the compute bindings
 * and limits, the device's where it has one; the image units the context
 * has, which only compute shaders read; atomic counter limits are 0, as
 * they are not offered; and the states of 3.1's other calls); returns how
 * many, 0 when the name is not one.
 */
static unsigned
gles_integers_compute(
	struct gles_state *state,
	GLenum pname,
	GLint *values)
{
	/* The states, one by one. */
	switch (pname) {
	case GL_SHADER_STORAGE_BUFFER_BINDING:
		values[0] = gles_buffer_name(state->storage_buffer);
		return 1U;
	case GL_DISPATCH_INDIRECT_BUFFER_BINDING:
		values[0] = gles_buffer_name(state->dispatch_buffer);
		return 1U;
	case GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS:
		values[0] = (GLint)state->limits.maxComputeWorkGroupInvocations;
		return 1U;
	case GL_MAX_COMPUTE_SHARED_MEMORY_SIZE:
		values[0] = (GLint)state->limits.maxComputeSharedMemorySize;
		return 1U;
	case GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS:
	case GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS:
	case GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS:
		values[0] = (GLint)GLES_STORAGE_BINDINGS;
		return 1U;
	case GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS:
	case GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS:
		values[0] = 0;
		return 1U;
	case GL_MAX_SHADER_STORAGE_BLOCK_SIZE:
		values[0] = INT32_MAX;
		if (state->limits.maxStorageBufferRange < (uint32_t)INT32_MAX)
			values[0] = (GLint)state->limits.maxStorageBufferRange;
		return 1U;
	case GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT:
		values[0] = (GLint)state->limits.minStorageBufferOffsetAlignment;
		return 1U;
	case GL_MAX_COMPUTE_UNIFORM_BLOCKS:
		values[0] = (GLint)GLES_STAGE_BLOCKS;
		return 1U;
	case GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS:
		values[0] = (GLint)GLES_UNITS;
		return 1U;
	case GL_MAX_COMPUTE_UNIFORM_COMPONENTS:
		values[0] = 1024;
		return 1U;
	case GL_MAX_COMBINED_COMPUTE_UNIFORM_COMPONENTS:
		values[0] = 1024 + (GLint)GLES_STAGE_BLOCKS * GLES_UNIFORM_BLOCK_SIZE / 4;
		return 1U;
	case GL_MAX_COMPUTE_ATOMIC_COUNTERS:
	case GL_MAX_COMPUTE_ATOMIC_COUNTER_BUFFERS:
	case GL_MAX_VERTEX_IMAGE_UNIFORMS:
	case GL_MAX_FRAGMENT_IMAGE_UNIFORMS:
		values[0] = 0;
		return 1U;
	case GL_TEXTURE_BINDING_2D_MULTISAMPLE:
		/* OpenGL ES 3.1's multisample textures (desktop GL's answer first, gles_desktop_integers). */
		values[0] = 0;
		if (state->ms_units[state->active_unit] != NULL)
			values[0] = (GLint)state->ms_units[state->active_unit]->name;
		return 1U;
	case GL_MAX_SAMPLE_MASK_WORDS:
		values[0] = 1;
		return 1U;
	case GL_MAX_COLOR_TEXTURE_SAMPLES:
		values[0] = (GLint)gles_color_texture_samples_max(state);
		return 1U;
	case GL_MAX_DEPTH_TEXTURE_SAMPLES:
		values[0] = (GLint)gles_depth_texture_samples_max(state);
		return 1U;
	case GL_MAX_INTEGER_SAMPLES:
		values[0] = (GLint)gles_integer_samples_max(state);
		return 1U;
	case GL_MAX_IMAGE_UNITS:
	case GL_MAX_COMPUTE_IMAGE_UNIFORMS:
	case GL_MAX_COMBINED_IMAGE_UNIFORMS:
		values[0] = (GLint)state->image_units;
		return 1U;
	case GL_MAX_COMBINED_SHADER_OUTPUT_RESOURCES:
		values[0] = (GLint)(GLES_STORAGE_BINDINGS + state->image_units + state->color_attachments);
		return 1U;
	case GL_DRAW_INDIRECT_BUFFER_BINDING:
		/* The buffer the indirect draws read. */
		values[0] = gles_buffer_name(state->draw_indirect_buffer);
		return 1U;
	case GL_PROGRAM_PIPELINE_BINDING:
		/* The program pipeline object bound. */
		values[0] = 0;
		if (state->pipeline != NULL)
			values[0] = (GLint)state->pipeline->name;
		return 1U;
	case GL_MAX_VERTEX_ATTRIB_BINDINGS:
		/* The vertex buffer binding points: one per attribute. */
		values[0] = (GLint)GLES_ATTRIBS;
		return 1U;
	case GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET:
		/* An attribute's largest offset in its binding point's elements (OpenGL ES 3.1's minimum). */
		values[0] = 2047;
		return 1U;
	case GL_MAX_FRAMEBUFFER_WIDTH:
		/* A framebuffer object's largest size and samples with nothing attached: the device's. */
		values[0] = (GLint)state->limits.maxFramebufferWidth;
		return 1U;
	case GL_MAX_FRAMEBUFFER_HEIGHT:
		/* Its height, the device's. */
		values[0] = (GLint)state->limits.maxFramebufferHeight;
		return 1U;
	case GL_MAX_FRAMEBUFFER_SAMPLES:
		/* Its samples, the device's framebufferNoAttachmentsSampleCounts (0: one only). */
		values[0] = (GLint)gles_framebuffer_default_samples_max(state);
		return 1U;
	case GL_MAX_VERTEX_ATTRIB_STRIDE:
		/* The largest stride (OpenGL ES 3.1's minimum; one past the device's is spread into the stream). */
		if (gles_fixed != NULL)
			return 0U;
		values[0] = 2048;
		return 1U;
	default:
		break;
	}

	/* Not a state of OpenGL ES 3.1's. */
	return 0U;
}

/* Returns what draw buffer i of the draw framebuffer writes (GL_BACK, GL_COLOR_ATTACHMENTi or GL_NONE). */
static GLenum
gles_draw_buffer(
	struct gles_state *state,
	GLenum index)
{
	struct gles_framebuffer *fbo;

	/* The surfaces' one buffer. */
	if (state->framebuffer == 0U) {
		if (index == 0U)
			return state->default_draw_buffer;
		return GL_NONE;
	}

	/* A framebuffer object's buffer. */
	fbo = gles_names_get(&state->framebuffers, state->framebuffer);
	if (fbo == NULL || index >= GLES_DRAW_BUFFERS)
		return GL_NONE;

	/* Succeeded: the attachment it writes. */
	return fbo->draw_buffers[index];
}

/* Returns what the read framebuffer's reads read (GL_BACK, GL_COLOR_ATTACHMENTi or GL_NONE). */
static GLenum
gles_read_buffer(
	struct gles_state *state)
{
	struct gles_framebuffer *fbo;

	/* The surfaces' buffer. */
	if (state->read_framebuffer == 0U)
		return state->default_read_buffer;

	/* A framebuffer object's. */
	fbo = gles_names_get(&state->framebuffers, state->read_framebuffer);
	if (fbo == NULL)
		return GL_NONE;

	/* Succeeded: the attachment it reads. */
	return fbo->read_buffer;
}

/* Returns the format (GL_IMPLEMENTATION_COLOR_READ_FORMAT) or the type glReadPixels reads the read buffer as besides RGBA bytes. */
static GLenum
gles_read_pair(
	struct gles_state *state,
	GLenum pname)
{
	const struct gles_format *format;
	GLenum read_format;
	GLenum read_type;

	/* The read buffer's format's pair (RGBA bytes without one). */
	format = gles_read_buffer_format(state);
	read_format = GL_RGBA;
	read_type = GL_UNSIGNED_BYTE;
	if (format != NULL)
		gles_read_format(format, &read_format, &read_type);

	/* The format asked for. */
	if (pname == GL_IMPLEMENTATION_COLOR_READ_FORMAT)
		return read_format;

	/* Succeeded: the type. */
	return read_type;
}

/*
 * Reports whether the context offers OpenGL ES 3.0: its device can capture
 * outputs (transform feedback writes from vertex shaders) and has the 3D
 * and array textures.
 */
static int
gles_version_three(
	struct zegl_context *context)
{
	struct gles_state *state;

	/* A context's state with the device's features. */
	state = gles_state(context);
	if (state == NULL)
		return 0;
	if (!state->display->features.vertexPipelineStoresAndAtomics)
		return 0;
	if (!state->display->volume_and_array)
		return 0;

	/* Succeeded: OpenGL ES 3.0. */
	return 1;
}

/*
 * Reports whether a device draws into a float format (OpenGL ES 3.0's
 * GL_EXT_color_buffer_float): a device of OpenGL ES 3.0 (the extension
 * needs it) whose R16F, RG16F, RGBA16F, R32F, RG32F, RGBA32F or
 * R11F_G11F_B10F images may be colour attachments.  A framebuffer object
 * with one of the others attached is unsupported (framebuffer.c).
 */
static int
gles_color_buffer_float(
	struct gles_state *state)
{
	static const VkFormat formats[] = {
		VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT,
		VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32
	};
	uint32_t features;
	unsigned index;

	/* A device of OpenGL ES 3.0. */
	if (!state->display->features.vertexPipelineStoresAndAtomics)
		return 0;
	if (!state->display->volume_and_array)
		return 0;

	/* One of the formats drawn into. */
	for (index = 0U; index < sizeof(formats) / sizeof(formats[0]); index++) {
		features = gles_image_features(state, formats[index]);
		if ((features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0U)
			return 1;
	}

	/* None of them. */
	return 0;
}

/* Returns how many of gles_extension_names a context's device has: all, or all but GL_EXT_color_buffer_float. */
static unsigned
gles_extension_count(
	const struct gles_state *state)
{
	unsigned count;

	/* Every name, the last only with float targets. */
	count = sizeof(gles_extension_names) / sizeof(gles_extension_names[0]);
	if (!state->color_buffer_float)
		count--;
	return count;
}

/*
 * Reports whether a context offers OpenGL ES 3.1's compute (ws101-p009):
 * an OpenGL ES 3 context of OpenGL ES 3.0 (or libGL's desktop GL of 4.3
 * on, whose GLSL 4.30 has compute shaders), whose
 * queue runs compute, on a device whose limits are at least OpenGL ES
 * 3.1's minimums (workgroups, shared memory, four storage buffers a
 * stage, 2^27 bytes of a storage buffer).
 */
static int
gles_compute_offered(
	struct zegl_context *context,
	const VkPhysicalDeviceLimits *limits)
{
	VkQueueFamilyProperties *families;
	uint32_t count;
	unsigned glsl_latest;
	int compute;

	/*
	 * An OpenGL ES 3 context of OpenGL ES 3.0 (the device's feature
	 * gles_version_three asks for; the state is being made), or desktop
	 * GL's (libGL) of a version whose GLSL has compute shaders (4.30, 4.3).
	 */
	if (!context->display->features.vertexPipelineStoresAndAtomics || context->version < 3)
		return 0;
	if (gles_fixed != NULL) {
		glsl_latest = gles_fixed->glsl_version();
		if (glsl_latest < 430U)
			return 0;
	}

	/* OpenGL ES 3.1's minimums of the compute limits. */
	if (limits->maxComputeWorkGroupInvocations < 128U ||
	    limits->maxComputeWorkGroupSize[0] < 128U ||
	    limits->maxComputeWorkGroupSize[1] < 128U ||
	    limits->maxComputeWorkGroupSize[2] < 64U ||
	    limits->maxComputeWorkGroupCount[0] < 65535U ||
	    limits->maxComputeWorkGroupCount[1] < 65535U ||
	    limits->maxComputeWorkGroupCount[2] < 65535U ||
	    limits->maxComputeSharedMemorySize < 16384U)
		return 0;

	/* And of the storage buffers. */
	if (limits->maxPerStageDescriptorStorageBuffers < GLES_STORAGE_BINDINGS ||
	    limits->maxDescriptorSetStorageBuffers < GLES_STORAGE_BINDINGS ||
	    limits->maxStorageBufferRange < (1U << 27))
		return 0;

	/* The display's queue family, which must run compute too. */
	count = 0U;
	vkGetPhysicalDeviceQueueFamilyProperties(context->display->physical, &count, NULL);
	if (context->display->family >= count)
		return 0;
	families = calloc(count, sizeof(*families));
	if (families == NULL)
		return 0;
	vkGetPhysicalDeviceQueueFamilyProperties(context->display->physical, &count, families);
	compute = 0;
	if (context->display->family < count && (families[context->display->family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U)
		compute = 1;
	free(families);

	/* Succeeded: whether it runs compute. */
	return compute;
}

/* Writes one of desktop GL's integer states (libGL); returns how many values, 0 when the name is not one. */
static unsigned
gles_desktop_integers(
	struct gles_state *state,
	GLenum pname,
	GLint *values)
{
	struct gles_texture *texture;
	int count;

	/* The context's own list of extensions (-1: OpenGL ES's). */
	if (pname == GL_NUM_EXTENSIONS) {
		count = gles_fixed->extension_count();
		if (count < 0)
			return 0U;
		values[0] = count;
		return 1U;
	}

	/* The state asked for. */
	switch (pname) {
	case GL_PRIMITIVE_RESTART_INDEX:
		values[0] = (GLint)state->restart_index;
		return 1U;
	case GL_PROVOKING_VERTEX:
		values[0] = GL_LAST_VERTEX_CONVENTION;
		if (state->provoking_vertex == GL_FIRST_VERTEX_CONVENTION)
			values[0] = GL_FIRST_VERTEX_CONVENTION;
		return 1U;
	case GL_TEXTURE_BINDING_RECTANGLE:
		texture = state->rect_units[state->active_unit];
		values[0] = 0;
		if (texture != NULL)
			values[0] = (GLint)texture->name;
		return 1U;
	case GL_TEXTURE_BINDING_BUFFER:
		texture = state->buffer_units[state->active_unit];
		values[0] = 0;
		if (texture != NULL)
			values[0] = (GLint)texture->name;
		return 1U;
	case GL_TEXTURE_BUFFER:
		values[0] = gles_buffer_name(state->texture_buffer);
		return 1U;
	case GL_MAX_RECTANGLE_TEXTURE_SIZE:
		values[0] = (GLint)state->limits.maxImageDimension2D;
		return 1U;
	case GL_MAX_TEXTURE_BUFFER_SIZE:
		values[0] = (GLint)state->limits.maxTexelBufferElements;
		return 1U;
	case GL_TEXTURE_BINDING_2D_MULTISAMPLE:
		texture = state->ms_units[state->active_unit];
		values[0] = 0;
		if (texture != NULL)
			values[0] = (GLint)texture->name;
		return 1U;
	case GL_TEXTURE_BINDING_CUBE_MAP_ARRAY:
		/* Desktop GL 4.0's cube map arrays (libGL). */
		if (gles_fixed == NULL)
			return 0U;
		texture = state->cube_array_units[state->active_unit];
		values[0] = 0;
		if (texture != NULL)
			values[0] = (GLint)texture->name;
		return 1U;
	case GL_MAX_SAMPLE_MASK_WORDS:
		values[0] = 1;
		return 1U;
	case GL_MAX_COLOR_TEXTURE_SAMPLES:
		values[0] = (GLint)gles_color_texture_samples_max(state);
		return 1U;
	case GL_MAX_DEPTH_TEXTURE_SAMPLES:
		values[0] = (GLint)gles_depth_texture_samples_max(state);
		return 1U;
	case GL_MAX_INTEGER_SAMPLES:
		values[0] = (GLint)gles_integer_samples_max(state);
		return 1U;
	case GL_MAX_GEOMETRY_OUTPUT_VERTICES:
		values[0] = (GLint)state->limits.maxGeometryOutputVertices;
		return 1U;
	case GL_DRAW_INDIRECT_BUFFER_BINDING:
		/* The buffer indirect draws read (GL_ARB_draw_indirect). */
		values[0] = gles_buffer_name(state->draw_indirect_buffer);
		return 1U;
	case GL_MIN_PROGRAM_TEXTURE_GATHER_OFFSET:
		/* Desktop GL's texture gathers' offsets (libGL), the device's. */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = (GLint)state->limits.minTexelGatherOffset;
		return 1U;
	case GL_MAX_PROGRAM_TEXTURE_GATHER_OFFSET:
		/* Desktop GL's texture gathers' offsets (libGL), the device's. */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = (GLint)state->limits.maxTexelGatherOffset;
		return 1U;
	case GL_MAX_GEOMETRY_SHADER_INVOCATIONS:
		/* Desktop GL's geometry shader invocations (libGL, GL_ARB_gpu_shader5), the shaders' most: 32. */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = 32;
		if (state->limits.maxGeometryShaderInvocations < 32U)
			values[0] = (GLint)state->limits.maxGeometryShaderInvocations;
		return 1U;
	case GL_FRAGMENT_INTERPOLATION_OFFSET_BITS:
		/* Desktop GL's interpolation offsets' fraction bits (libGL, GL_ARB_gpu_shader5), the device's. */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = (GLint)state->limits.subPixelInterpolationOffsetBits;
		return 1U;
	case GL_MAX_PROGRAM_TEXTURE_GATHER_COMPONENTS_ARB:
		/* Desktop GL's texture gathers read every format's first component (libGL). */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = 4;
		return 1U;
	case GL_MAX_VERTEX_ATTRIB_BINDINGS:
		/* Desktop GL 4.3's vertex buffer binding points (libGL): one per attribute. */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = (GLint)GLES_ATTRIBS;
		return 1U;
	case GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET:
		/* Desktop GL 4.3's largest offset of an attribute in its binding point's elements (libGL). */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = 2047;
		return 1U;
	case GL_MAX_TRANSFORM_FEEDBACK_BUFFERS:
		/* Desktop GL 4.0's transform feedback binding points interleaved names may write (libGL, GL_ARB_transform_feedback3). */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = (GLint)GLES_FEEDBACK_BINDINGS;
		return 1U;
	case GL_MAX_VERTEX_STREAMS:
		/* Desktop GL 4.0's vertex streams (libGL, GL_ARB_transform_feedback3 and GL_ARB_gpu_shader5). */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = (GLint)GLES_VERTEX_STREAMS;
		return 1U;
	case GL_MAX_SUBROUTINES:
		/* Desktop GL 4.0's subroutines of a stage and their uniforms' locations (libGL, GL_ARB_shader_subroutine). */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = (GLint)GLES_SUBROUTINES;
		return 1U;
	case GL_MAX_SUBROUTINE_UNIFORM_LOCATIONS:
		if (gles_fixed == NULL)
			return 0U;
		values[0] = (GLint)GLES_SUBROUTINE_LOCATIONS;
		return 1U;
	case GL_CLIP_ORIGIN:
		/* Desktop GL's clip control (libGL). */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = (GLint)state->clip_origin;
		return 1U;
	case GL_CLIP_DEPTH_MODE:
		/* Desktop GL's clip control (libGL). */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = (GLint)state->clip_depth;
		return 1U;
	case GL_MAX_GEOMETRY_UNIFORM_COMPONENTS:
		values[0] = 1024;
		return 1U;
	case GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS:
		values[0] = (GLint)GLES_UNITS;
		return 1U;
	case GL_MAX_GEOMETRY_INPUT_COMPONENTS:
		values[0] = (GLint)state->limits.maxGeometryInputComponents;
		return 1U;
	case GL_MAX_GEOMETRY_OUTPUT_COMPONENTS:
		values[0] = (GLint)state->limits.maxGeometryOutputComponents;
		return 1U;
	default:
		break;
	}

	/* Desktop GL 4.0's tessellation, on a device that has it. */
	if (state->display->features.tessellationShader) {
		count = gles_tess_integers(state, pname, values);
		return (unsigned)count;
	}

	/* Not one of desktop GL's. */
	return 0U;
}

/* Writes one of desktop GL 4.0's tessellation states and limits (libGL); returns how many values, 0 when the name is not one. */
static int
gles_tess_integers(
	struct gles_state *state,
	GLenum pname,
	GLint *values)
{
	/* The state asked for, or the device's limit (gl_MaxPatchVertices at most). */
	switch (pname) {
	case GL_PATCH_VERTICES:
		values[0] = state->patch_vertices;
		return 1;
	case GL_MAX_PATCH_VERTICES:
		values[0] = (GLint)state->limits.maxTessellationPatchSize;
		if (values[0] > GLES_MAX_PATCH_VERTICES)
			values[0] = GLES_MAX_PATCH_VERTICES;
		return 1;
	case GL_MAX_TESS_GEN_LEVEL:
		values[0] = (GLint)state->limits.maxTessellationGenerationLevel;
		return 1;
	case GL_MAX_TESS_CONTROL_UNIFORM_COMPONENTS:
	case GL_MAX_TESS_EVALUATION_UNIFORM_COMPONENTS:
		values[0] = 1024;
		return 1;
	case GL_MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS:
	case GL_MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS:
		values[0] = (GLint)GLES_UNITS;
		return 1;
	case GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS:
	case GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS:
		values[0] = (GLint)GLES_STAGE_BLOCKS;
		return 1;
	case GL_MAX_TESS_CONTROL_OUTPUT_COMPONENTS:
		values[0] = (GLint)state->limits.maxTessellationControlPerVertexOutputComponents;
		return 1;
	case GL_MAX_TESS_PATCH_COMPONENTS:
		values[0] = (GLint)state->limits.maxTessellationControlPerPatchOutputComponents;
		return 1;
	case GL_MAX_TESS_CONTROL_TOTAL_OUTPUT_COMPONENTS:
		values[0] = (GLint)state->limits.maxTessellationControlTotalOutputComponents;
		return 1;
	case GL_MAX_TESS_EVALUATION_OUTPUT_COMPONENTS:
		values[0] = (GLint)state->limits.maxTessellationEvaluationOutputComponents;
		return 1;
	case GL_MAX_TESS_CONTROL_INPUT_COMPONENTS:
		values[0] = (GLint)state->limits.maxTessellationControlPerVertexInputComponents;
		return 1;
	case GL_MAX_TESS_EVALUATION_INPUT_COMPONENTS:
		values[0] = (GLint)state->limits.maxTessellationEvaluationInputComponents;
		return 1;
	default:
		break;
	}

	/* Not one of tessellation's. */
	return 0;
}

/*
 * Sets the index that restarts primitives while GL_PRIMITIVE_RESTART is
 * on (desktop GL 3.1, libGL).
 */
GL_APICALL void GL_APIENTRY
glPrimitiveRestartIndex(
	GLuint index)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* Succeeded: the index draws compare with. */
	state->restart_index = index;
}

/*
 * Sets a patch's vertices (GL_PATCH_VERTICES) for the draws of patches
 * (desktop GL 4.0, libGL), at most GL_MAX_PATCH_VERTICES.
 */
GL_APICALL void GL_APIENTRY
glPatchParameteri(
	GLenum pname,
	GLint value)
{
	struct zegl_context *context;
	struct gles_state *state;
	GLint largest;

	/* A context with its state, on a device with tessellation, and the one parameter. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (gles_fixed == NULL || !state->display->features.tessellationShader || pname != GL_PATCH_VERTICES) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Between 1 and the most a patch has. */
	largest = (GLint)state->limits.maxTessellationPatchSize;
	if (largest > GLES_MAX_PATCH_VERTICES)
		largest = GLES_MAX_PATCH_VERTICES;
	if (value <= 0 || value > largest) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the vertices of each patch drawn. */
	state->patch_vertices = value;
}

/*
 * Sets the default outer or inner levels (desktop GL 4.0, libGL): kept
 * for glGet; a program without a control shader, which would take them,
 * does not link here.
 */
GL_APICALL void GL_APIENTRY
glPatchParameterfv(
	GLenum pname,
	const GLfloat *values)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, on a device with tessellation. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (gles_fixed == NULL || !state->display->features.tessellationShader) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The four outer levels, or the two inner ones. */
	if (pname == GL_PATCH_DEFAULT_OUTER_LEVEL) {
		memcpy(state->patch_outer, values, sizeof(state->patch_outer));
	} else if (pname == GL_PATCH_DEFAULT_INNER_LEVEL) {
		memcpy(state->patch_inner, values, sizeof(state->patch_inner));
	} else {
		gles_error(context, GL_INVALID_ENUM);
	}
}

/*
 * Sets the least fraction of a pixel's samples shaded on their own while
 * GL_SAMPLE_SHADING is on (desktop GL 4.0's GL_ARB_sample_shading, libGL),
 * clamped to [0, 1].
 */
GL_APICALL void GL_APIENTRY
glMinSampleShading(
	GLfloat value)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, on a device that shades per sample. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (gles_fixed == NULL || !state->display->features.sampleRateShading) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Clamped. */
	if (value < 0.0f)
		value = 0.0f;
	if (value > 1.0f)
		value = 1.0f;

	/* Succeeded: the fraction draws take. */
	state->min_sample_shading = value;
}

/*
 * Sets the word of the sample mask (desktop GL 3.2, libGL): which samples
 * a fragment may cover while GL_SAMPLE_MASK is on; there is one word.
 */
GL_APICALL void GL_APIENTRY
glSampleMaski(
	GLuint maskNumber,
	GLbitfield mask)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, and word 0. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (maskNumber != 0U) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the word draws use. */
	state->sample_mask_value = mask;
}

/*
 * Reports a sample's position in its pixel (desktop GL 3.2, libGL): the
 * draw framebuffer's samples are at Vulkan's standard positions.
 */
GL_APICALL void GL_APIENTRY
glGetMultisamplefv(
	GLenum pname,
	GLuint index,
	GLfloat *val)
{
	static const GLfloat two[2][2] = { { 0.75f, 0.75f }, { 0.25f, 0.25f } };
	static const GLfloat four[4][2] = { { 0.375f, 0.125f }, { 0.875f, 0.375f }, { 0.125f, 0.625f }, { 0.625f, 0.875f } };
	static const GLfloat eight[8][2] = {
		{ 0.5625f, 0.3125f }, { 0.4375f, 0.6875f }, { 0.8125f, 0.5625f }, { 0.3125f, 0.1875f },
		{ 0.1875f, 0.8125f }, { 0.0625f, 0.4375f }, { 0.6875f, 0.9375f }, { 0.9375f, 0.0625f }
	};
	struct zegl_context *context;
	struct gles_state *state;
	uint32_t samples;

	/* A context with its state, and the one name. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (pname != GL_SAMPLE_POSITION) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A sample the draw framebuffer has (the window's one is at the centre). */
	samples = gles_framebuffer_samples(state, state->framebuffer);
	if (samples == 0U)
		samples = 1U;
	if (index >= samples) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the standard position of the count. */
	val[0] = 0.5f;
	val[1] = 0.5f;
	if (samples == 2U) {
		val[0] = two[index][0];
		val[1] = two[index][1];
	} else if (samples == 4U) {
		val[0] = four[index][0];
		val[1] = four[index][1];
	} else if (samples == 8U) {
		val[0] = eight[index][0];
		val[1] = eight[index][1];
	}
}

/*
 * Chooses the vertex of each primitive whose values flat inputs take
 * (desktop GL 3.2, libGL): the first, or the last as GL starts with.
 */
GL_APICALL void GL_APIENTRY
glProvokingVertex(
	GLenum mode)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, and one of the two conventions. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (mode != GL_FIRST_VERTEX_CONVENTION && mode != GL_LAST_VERTEX_CONVENTION) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Succeeded: the convention the draws follow. */
	state->provoking_vertex = mode;
}

/* Turns blending on or off for one draw buffer (glEnablei, glDisablei). */
static void
gles_blend_buffer(
	GLenum target,
	GLuint index,
	int on)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, and blending. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (target != GL_BLEND) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A draw buffer there is. */
	if (index >= state->color_attachments) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each buffer starts from the shared state the first time. */
	if (!state->blend_indexed) {
		state->blend_buffers = 0U;
		if (state->blend)
			state->blend_buffers = (1U << GLES_DRAW_BUFFERS) - 1U;
		state->blend_indexed = 1;
	}

	/* The buffer's bit. */
	if (on) {
		state->blend_buffers |= 1U << index;
	} else {
		state->blend_buffers &= ~(1U << index);
	}

	/* The shared flag (glIsEnabled) says whether any buffer blends. */
	state->blend = 0;
	if (state->blend_buffers != 0U)
		state->blend = 1;
}

/*
 * Checks four blend factors: the second colour's only in desktop GL 3.3
 * on a device that blends with it.  Returns 0, or -1 with the error
 * recorded.
 */
static int
gles_blend_factors_check(
	struct zegl_context *context,
	const struct gles_state *state,
	const GLenum *factors)
{
	unsigned index;

	/* Each factor that is one of the second colour's. */
	for (index = 0U; index < 4U; index++) {
		if (factors[index] != GL_SRC1_COLOR &&
		    factors[index] != GL_ONE_MINUS_SRC1_COLOR &&
		    factors[index] != GL_SRC1_ALPHA &&
		    factors[index] != GL_ONE_MINUS_SRC1_ALPHA)
			continue;
		if (gles_fixed == NULL || !state->display->features.dualSrcBlend) {
			gles_error(context, GL_INVALID_ENUM);
			return -1;
		}
	}

	/* Succeeded: factors a draw may blend with. */
	return 0;
}

/*
 * Checks the blend equations of colour and alpha: those of OpenGL ES 2
 * and the minimum and maximum.  Returns 0, or -1 with GL_INVALID_ENUM
 * recorded.
 */
static int
gles_blend_equations_check(
	struct zegl_context *context,
	GLenum rgb,
	GLenum alpha)
{
	GLenum modes[2];
	unsigned index;

	/* Each of the two. */
	modes[0] = rgb;
	modes[1] = alpha;
	for (index = 0U; index < 2U; index++) {
		switch (modes[index]) {
		case GL_FUNC_ADD:
		case GL_FUNC_SUBTRACT:
		case GL_FUNC_REVERSE_SUBTRACT:
		case GL_MIN_EXT:
		case GL_MAX_EXT:
			break;
		default:
			gles_error(context, GL_INVALID_ENUM);
			return -1;
		}
	}

	/* Succeeded: equations there are. */
	return 0;
}

/* Sets each draw buffer's blend factors and equations apart, from the shared ones, the first time one buffer's are set. */
static void
gles_blend_functions_split(
	struct gles_state *state)
{
	unsigned index;

	/* Set apart already. */
	if (state->functions_indexed)
		return;

	/* Every buffer starts from the shared ones. */
	for (index = 0U; index < GLES_DRAW_BUFFERS; index++)
		gles_blend_function_of(state, index, &state->indexed_functions[index]);
	state->functions_indexed = 1;
}

/* Returns a buffer's name, 0 for none. */
static GLint
gles_buffer_name(
	const struct gles_buffer *buffer)
{
	/* No buffer. */
	if (buffer == NULL)
		return 0;

	/* The buffer's name. */
	return (GLint)buffer->name;
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
	case GL_MIN_SAMPLE_SHADING_VALUE:
		/* Desktop GL's per-sample shading's fraction (libGL). */
		if (gles_fixed == NULL || !state->display->features.sampleRateShading)
			return 0U;
		values[0] = state->min_sample_shading;
		return 1U;
	case GL_MIN_FRAGMENT_INTERPOLATION_OFFSET:
		/* Desktop GL's interpolation offsets (libGL, GL_ARB_gpu_shader5), the device's. */
		if (gles_fixed == NULL)
			return 0U;
		values[0] = state->limits.minInterpolationOffset;
		return 1U;
	case GL_MAX_FRAGMENT_INTERPOLATION_OFFSET:
		if (gles_fixed == NULL)
			return 0U;
		values[0] = state->limits.maxInterpolationOffset;
		return 1U;
	case GL_PATCH_DEFAULT_OUTER_LEVEL:
		/* Desktop GL 4.0's default levels (libGL). */
		if (gles_fixed == NULL || !state->display->features.tessellationShader)
			return 0U;
		memcpy(values, state->patch_outer, sizeof(state->patch_outer));
		return 4U;
	case GL_PATCH_DEFAULT_INNER_LEVEL:
		if (gles_fixed == NULL || !state->display->features.tessellationShader)
			return 0U;
		memcpy(values, state->patch_inner, sizeof(state->patch_inner));
		return 2U;
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
