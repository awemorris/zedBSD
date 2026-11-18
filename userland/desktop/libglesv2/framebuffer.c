/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Framebuffer and renderbuffer objects of zedBSD's OpenGL ES (WS068
 * p022, p026), and the target every draw, clear and read goes to.
 *
 * A framebuffer object draws into up to four colour attachments and one
 * depth and stencil attachment: a level of a 2D texture, a face of a cube
 * map, a layer of a 2D array texture, or a renderbuffer, each of any
 * format a framebuffer may draw into.  Its render pass is recorded into
 * the draw surface's frame, as the surface's own passes are, and stays
 * open until something else records into the frame or the frame is
 * submitted (libEGL's frame_closing).  Its images keep GL's rows from the
 * bottom up, as textures do, so its draws do not turn y over.  A texture
 * drawn into this way is newer on the device than on the CPU until
 * gles_texture_fetch reads it back.
 *
 * The draw framebuffer and the read framebuffer are bound apart (OpenGL
 * ES 3); draws and clears go to the draw framebuffer's draw buffers,
 * reads come from the read framebuffer's read buffer.  Pipelines are made
 * with one render pass per set of attachment formats (compatible with
 * every framebuffer object's pass of those formats).
 *
 * Fragment output i goes to colour slot i of the pass, which holds the
 * attachment draw buffer i names.  OpenGL ES names attachment i or none
 * there (an attachment no draw buffer names keeps its own slot, unwritten);
 * desktop GL (libGL) may name any attachment in any draw buffer.
 */

#include "gles.h"
#include "glsl/glsl.h"

#include <stdlib.h>
#include <string.h>

/* The size a renderbuffer may have (GL_MAX_RENDERBUFFER_SIZE is at most this). */
#define FRAMEBUFFER_SIZE_MAX	16384

/*
 * What one attachment point of a complete framebuffer object names: the
 * texture (with the face, the level GL named and the image's level and
 * layer) or the renderbuffer, the format and size, and the layout the
 * image rests in between passes.
 */
struct framebuffer_image {
	struct gles_texture *texture;
	struct gles_renderbuffer *renderbuffer;
	const struct gles_format *format;
	VkFormat vk;
	VkImageLayout layout;
	unsigned face;
	GLint gl_level;
	uint32_t level;
	uint32_t layer;
	int width;
	int height;

	/* The samples per pixel, and nonzero for a 3D texture's slice (layer is the slice, drawn through a gles_slice_image). */
	uint32_t samples;
	int volume;

	/* A layered attachment (desktop GL): nonzero, with its layers (layer is 0). */
	int layered;
	uint32_t layers;
};

/*
 * What every attachment point of a complete framebuffer object names, and
 * how many colour attachments its pass has (the last one attached, plus
 * one).
 */
struct framebuffer_images {
	struct framebuffer_image colors[GLES_COLOR_ATTACHMENTS];
	unsigned color_count;
	struct framebuffer_image depth;
	int has_depth;
	uint32_t samples;

	/* The layers the framebuffer has: the fewest of its layered attachments' (every attachment is layered then), 1 otherwise. */
	uint32_t layers;
};

/*
 * One side of a blit: the image read or written with its level and layer
 * (and a 3D texture's slice as a depth offset), the layout it rests in,
 * its format, aspects, size (the framebuffer's, and the attachment's
 * own) and samples, whether its rows go down (a surface's), the frame's
 * command buffer the copy is recorded in, and the object it belongs to
 * (to mark it used or drawn into).
 */
struct framebuffer_side {
	VkImage image;
	VkImageUsageFlags usage;
	uint32_t level;
	uint32_t layer;
	int32_t slice;
	VkImageLayout layout;
	VkFormat vk;
	const struct gles_format *format;
	VkImageAspectFlags aspects;
	VkExtent2D extent;
	VkExtent2D size;
	uint32_t samples;
	int flip;
	VkCommandBuffer command;
	struct gles_texture *texture;
	struct gles_renderbuffer *renderbuffer;
	unsigned face;
	GLint gl_level;
};

/*
 * The render pass and pipelines that resolve a multisampled depth and
 * stencil buffer of one format into an image of one sample: its depth
 * pipeline and its stencil pipeline (VK_NULL_HANDLE where the format has
 * no such aspect).
 */
struct framebuffer_resolver {
	VkFormat vk;
	VkRenderPass pass;
	VkPipeline pipelines[2];
	struct framebuffer_resolver *next;
};

/*
 * What a blit resolves multisampled depth and stencil buffers with, made
 * at the first such blit: the shader modules (the depth draw's vertex and
 * fragment shader, then the stencil draw's), the set layout of the one
 * sampled image and the pipeline layout, the sampler, and the render pass
 * and pipelines of each format; failed when the shaders could not be
 * made (no blit resolves depth then).
 */
struct gles_depth_resolve {
	int failed;
	VkShaderModule modules[4];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkSampler sampler;
	struct framebuffer_resolver *resolvers;
};

/*
 * The shaders of a depth and stencil resolve.  The vertex shader draws a
 * triangle over the whole viewport (the multisampled image's size) and
 * passes the place in it, and the draw's bit of stencil (its first
 * vertex over 3).  The depth shader writes each pixel's first sample's
 * depth; the stencil shader keeps the pixels whose first sample has the
 * bit set, which the pipeline writes into the bit alone.
 */
static const char framebuffer_resolve_vertex[] =
	"#version 310 es\n"
	"out highp vec2 place;\n"
	"flat out highp int bit;\n"
	"void main()\n"
	"{\n"
	"\tint corner = gl_VertexID % 3;\n"
	"\tvec2 position = vec2(corner == 1 ? 3.0 : -1.0, corner == 2 ? 3.0 : -1.0);\n"
	"\tplace = position * 0.5 + 0.5;\n"
	"\tbit = gl_VertexID / 3;\n"
	"\tgl_Position = vec4(position, 0.0, 1.0);\n"
	"}\n";
static const char framebuffer_resolve_depth_fragment[] =
	"#version 310 es\n"
	"precision highp float;\n"
	"uniform highp sampler2DMS depth;\n"
	"in highp vec2 place;\n"
	"flat in highp int bit;\n"
	"void main()\n"
	"{\n"
	"\tgl_FragDepth = texelFetch(depth, ivec2(place * vec2(textureSize(depth))), 0).r;\n"
	"}\n";
static const char framebuffer_resolve_stencil_fragment[] =
	"#version 310 es\n"
	"precision highp float;\n"
	"uniform highp usampler2DMS stencil;\n"
	"in highp vec2 place;\n"
	"flat in highp int bit;\n"
	"void main()\n"
	"{\n"
	"\tuint value = texelFetch(stencil, ivec2(place * vec2(textureSize(stencil))), 0).r;\n"
	"\tif ((value & (1u << uint(bit))) == 0u)\n"
	"\t\tdiscard;\n"
	"}\n";

/*
 * The format a stencil-only renderbuffer (GL_STENCIL_INDEX8) is kept in:
 * an attachment of stencil and no colour, whose image has the Vulkan
 * format the renderbuffer chose.  It never changes.
 */
static const struct gles_format framebuffer_stencil = {
	GL_STENCIL_INDEX8, GL_STENCIL_INDEX8, VK_FORMAT_S8_UINT, 1U, 1U, GLES_TEXEL_DEPTH, 0, 0, 1, 1
};

static struct gles_framebuffer *framebuffer_named(struct gles_state *state, GLuint name);
static int framebuffer_target(struct zegl_context *context, GLenum target, int *read);
static struct gles_framebuffer *framebuffer_for(struct zegl_context *context, struct gles_state *state, GLenum target, int bound, GLuint *name);
static struct gles_framebuffer *framebuffer_new(GLuint name);
static GLenum framebuffer_image_of(struct gles_state *state, const struct gles_attachment *point, int depth, struct framebuffer_image *image);
static GLenum framebuffer_status(struct gles_state *state, struct gles_framebuffer *fbo, struct framebuffer_images *images);
static GLenum framebuffer_build(struct gles_state *state, struct gles_framebuffer *fbo, struct framebuffer_images *images);
static unsigned framebuffer_slot_attachment(const struct gles_framebuffer *fbo, unsigned slot);
static void framebuffer_mark(struct gles_state *state, struct framebuffer_images *images);
static GLenum framebuffer_layered_of(struct gles_texture *texture, const struct gles_attachment *point, struct framebuffer_image *image);
static GLenum framebuffer_layers(struct framebuffer_images *images);
static void framebuffer_forget_views(struct gles_state *state, struct gles_framebuffer *fbo);
static void framebuffer_leave(struct gles_state *state, struct zegl_surface *surface);
static VkImageView framebuffer_slice(struct gles_state *state, struct gles_slice_image *slice, const struct framebuffer_image *image);
static void framebuffer_slices_copy(struct gles_state *state, struct gles_framebuffer *fbo, VkCommandBuffer command, int back);
static void framebuffer_slices_free(struct gles_state *state, struct gles_framebuffer *fbo);
static void framebuffer_buffer_barrier(VkCommandBuffer command, VkBuffer buffer);
static VkResult framebuffer_pass(struct gles_state *state, const struct gles_pass_format *format, const VkImageLayout *color_layouts, VkImageLayout depth_layout, VkRenderPass *pass);
static VkRenderPass framebuffer_compatible(struct gles_state *state, const struct gles_pass_format *format);
static void framebuffer_attach(struct zegl_context *context, GLenum target, GLenum attachment, int kind, GLuint name, unsigned face, GLint level, GLint layer);
static int framebuffer_point(const struct gles_state *state, GLenum attachment, struct gles_attachment **point, struct gles_framebuffer *fbo);
static GLint framebuffer_bits(const struct gles_format *format, VkFormat vk, GLenum pname);
static GLint framebuffer_attachment_value(struct gles_state *state, const struct gles_attachment *point, GLenum pname, int *known);
static int framebuffer_sides(struct zegl_context *context, struct gles_state *state, struct framebuffer_side *read_color, struct framebuffer_side *read_depth, struct framebuffer_side *draw_colors, unsigned *count, struct framebuffer_side *draw_depth);
static void framebuffer_side_surface(struct zegl_surface *surface, int depth, struct framebuffer_side *side);
static void framebuffer_side_image(const struct framebuffer_image *image, VkExtent2D extent, struct zegl_surface *surface, struct framebuffer_side *side);
static int framebuffer_side_integer(const struct framebuffer_side *side);
static int framebuffer_resolve(struct zegl_context *context, struct gles_state *state, struct framebuffer_side *side);
static int framebuffer_resolve_depth(struct zegl_context *context, struct gles_state *state, struct framebuffer_side *side, VkImageAspectFlags aspects);
static struct gles_depth_resolve *framebuffer_resolve_objects(struct gles_state *state);
static struct framebuffer_resolver *framebuffer_resolver(struct gles_state *state, struct gles_depth_resolve *resolve, VkFormat vk, VkImageAspectFlags aspects);
static VkPipeline framebuffer_resolve_pipeline(struct gles_state *state, struct gles_depth_resolve *resolve, VkRenderPass pass, int stencil);
static void framebuffer_resolve_free(struct gles_state *state);
static void framebuffer_blit(struct gles_state *state, const struct framebuffer_side *source, const struct framebuffer_side *target, const GLint *rectangles, VkImageAspectFlags aspects, GLenum filter);
static int framebuffer_blit_axis(GLint a0, GLint a1, GLint b0, GLint b1, int32_t limit, int32_t low, int32_t high, int32_t *out);
static uint32_t framebuffer_samples_mask(struct gles_state *state);
static uint32_t framebuffer_integer_mask(struct gles_state *state);
static uint32_t framebuffer_color_texture_mask(struct gles_state *state);
static uint32_t framebuffer_depth_texture_mask(struct gles_state *state);
static VkImageUsageFlags framebuffer_texture_usage(struct gles_state *state, const struct gles_format *format);
static int framebuffer_multisample_textures(const struct gles_state *state);
static int framebuffer_integer_multisampled(const struct gles_state *state);
static uint32_t framebuffer_samples_most(uint32_t mask);
static uint32_t framebuffer_samples_for(uint32_t mask, uint32_t samples);
static uint32_t framebuffer_default_samples(struct gles_state *state, uint32_t samples);
static void framebuffer_join_stencil(unsigned char *kept, const unsigned char *copied, size_t texels);
static struct gles_renderbuffer *renderbuffer_bound(struct zegl_context *context, GLenum target);
static int renderbuffer_format(struct gles_state *state, GLenum internal, struct gles_renderbuffer *renderbuffer);
static VkImageUsageFlags renderbuffer_usage(struct gles_state *state, const struct gles_renderbuffer *renderbuffer);
static int renderbuffer_image(struct gles_state *state, struct gles_renderbuffer *renderbuffer);
static void renderbuffer_discard(struct gles_state *state, struct gles_renderbuffer *renderbuffer);
static void renderbuffer_free(struct gles_state *state, struct gles_renderbuffer *renderbuffer);

/*
 * Opens where a draw, clear or read goes: the draw surface's frame, and
 * in it the surface's render pass (clearing with clear when it is the
 * frame's first) or the draw framebuffer object's.  Returns 0, or -1 with
 * the error recorded (no draw surface, an incomplete framebuffer, no
 * memory).
 */
int
gles_target_open(
	struct zegl_context *context,
	struct gles_state *state,
	const VkClearValue *clear,
	struct gles_target *target)
{
	struct framebuffer_images images;
	struct zegl_surface *surface;
	struct gles_framebuffer *fbo;
	VkRenderPassBeginInfo begin;
	const struct gles_format *format;
	uint32_t features;
	unsigned index;
	GLenum status;
	EGLint error;

	/* A draw surface, whose frame records everything. */
	surface = context->draw;
	if (surface == NULL) {
		gles_error(context, GL_INVALID_FRAMEBUFFER_OPERATION);
		return -1;
	}

	/* The frame, opened when this is its first command. */
	error = zegl_frame_begin(surface);
	if (error != EGL_SUCCESS) {
		gles_report("the frame", error);
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* The target starts as the surface's own images, of one layer. */
	memset(target, 0, sizeof(*target));
	target->surface = surface;
	target->command = surface->command;
	target->layers = 1U;

	/* Framebuffer 0: the surface's pass, after any framebuffer object's, with its one colour image. */
	if (state->framebuffer == 0U) {
		gles_target_close(state);
		zegl_frame_pass(surface, clear);
		target->pass = surface->pass;
		target->extent = surface->extent;
		target->flip = 1;
		target->samples = 1U;
		target->color_count = 1U;
		if (state->default_draw_buffer == GL_BACK)
			target->draw_mask = 1U;
		if (surface->depth_format != VK_FORMAT_UNDEFINED)
			target->depth_aspects = surface->depth_aspects;
		return 0;
	}

	/* A framebuffer object: complete, with its pass and framebuffer made for what is attached now. */
	fbo = framebuffer_named(state, state->framebuffer);
	status = GL_FRAMEBUFFER_UNSUPPORTED;
	if (fbo != NULL)
		status = framebuffer_build(state, fbo, &images);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		gles_error(context, GL_INVALID_FRAMEBUFFER_OPERATION);
		return -1;
	}

	/* Its pass entered, unless it is the one open: the surface's pass or another object's ends first, the 3D slices it draws are copied in. */
	if (state->open_fbo != fbo) {
		gles_target_close(state);
		framebuffer_leave(state, surface);
		framebuffer_slices_copy(state, fbo, surface->command, 0);
		memset(&begin, 0, sizeof(begin));
		begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		begin.renderPass = fbo->pass;
		begin.framebuffer = fbo->framebuffer;
		begin.renderArea.extent = fbo->extent;
		vkCmdBeginRenderPass(surface->command, &begin, VK_SUBPASS_CONTENTS_INLINE);

		/*
		 * The pass is open in this surface's frame until gles_target_close;
		 * recorded makes a pbuffer's frame be submitted even when nothing
		 * else was drawn.
		 */
		state->open_fbo = fbo;
		state->open_surface = surface;
		surface->recorded = 1;
	}

	/* The attached objects are drawn into by this frame. */
	framebuffer_mark(state, &images);

	/* The framebuffer object, with GL's rows and the pass its pipelines are made with. */
	target->fbo = fbo;
	target->pass = fbo->compatible;
	target->extent = fbo->extent;
	target->color_count = fbo->format.color_count;
	target->depth_aspects = fbo->depth_aspects;
	target->samples = fbo->format.samples;
	target->layers = fbo->layers;

	/* Each colour attachment a draw buffer writes, and those holding integers or that cannot blend. */
	for (index = 0U; index < fbo->format.color_count; index++) {
		format = fbo->color_formats[index];
		if (format == NULL)
			continue;

		/* Written when the draw buffer of its slot names an attachment. */
		if (index < GLES_DRAW_BUFFERS && fbo->draw_buffers[index] != GL_NONE)
			target->draw_mask |= 1U << index;

		/* Integers are neither blended nor cleared with floats. */
		if (format->kind == GLES_TEXEL_INT || format->kind == GLES_TEXEL_UINT)
			target->integer_mask |= 1U << index;

		/* A format the device does not blend is written as it comes. */
		features = gles_image_features(state, format->vk);
		if ((features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) == 0U)
			target->opaque_mask |= 1U << index;
	}

	/* Succeeded: the framebuffer object's target. */
	return 0;
}

/*
 * Ends the framebuffer object's render pass open in a frame, if there is
 * one, so the frame can go on with something else or be submitted.
 */
void
gles_target_close(
	struct gles_state *state)
{
	/* Nothing is open. */
	if (state->open_fbo == NULL)
		return;

	/* An occlusion query's segment in the pass ends first. */
	gles_queries_suspend(state);

	/* The pass ends in the frame it was opened in; the images rest in their layouts again, the 3D slices drawn are copied back. */
	vkCmdEndRenderPass(state->open_surface->command);
	framebuffer_slices_copy(state, state->open_fbo, state->open_surface->command, 1);
	state->open_fbo = NULL;
	state->open_surface = NULL;
}

/*
 * Finds what a read of the read framebuffer's read buffer copies: the
 * read surface's image, or the read framebuffer object's colour
 * attachment, in a frame with no pass open (the copy is recorded there).
 * Returns 0, or -1 with the error recorded (no surface, no read buffer,
 * an incomplete framebuffer).
 */
int
gles_read_source(
	struct zegl_context *context,
	struct gles_state *state,
	struct gles_read *read)
{
	struct framebuffer_images images;
	struct framebuffer_image *image;
	struct zegl_surface *surface;
	struct gles_framebuffer *fbo;
	unsigned index;
	GLenum status;
	EGLint error;

	/* Nothing found yet. */
	memset(read, 0, sizeof(*read));

	/* The read surface, whose images must be copyable and whose read buffer is its image. */
	if (state->read_framebuffer == 0U) {
		surface = context->read;
		if (surface == NULL) {
			gles_error(context, GL_INVALID_FRAMEBUFFER_OPERATION);
			return -1;
		}

		/* A read buffer, and images that can be copied. */
		if (state->default_read_buffer == GL_NONE || !surface->readable) {
			gles_error(context, GL_INVALID_OPERATION);
			return -1;
		}

		/* The frame, with its image drawn by at least one pass (a first pass clears it); no framebuffer object's pass open. */
		gles_target_close(state);
		error = zegl_frame_begin(surface);
		if (error != EGL_SUCCESS) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return -1;
		}

		/* The pass ends so the image can be copied. */
		zegl_frame_pass(surface, NULL);
		framebuffer_leave(state, surface);

		/* Succeeded: the surface's image, with rows from the top, as RGBA8 (or BGRA8). */
		read->surface = surface;
		read->image = surface->images[surface->image];
		read->layout = surface->rest_layout;
		read->format = gles_format_rgba8();
		read->extent = surface->extent;
		read->flip = 1;
		if (surface->format == VK_FORMAT_B8G8R8A8_UNORM)
			read->swizzle = 1;
		return 0;
	}

	/* The read framebuffer object, complete, with its views made. */
	fbo = framebuffer_named(state, state->read_framebuffer);
	status = GL_FRAMEBUFFER_UNSUPPORTED;
	if (fbo != NULL)
		status = framebuffer_build(state, fbo, &images);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		gles_error(context, GL_INVALID_FRAMEBUFFER_OPERATION);
		return -1;
	}

	/* A read buffer, of one sample per pixel (a multisampled image is read through a blit). */
	if (fbo->read_buffer == GL_NONE || images.samples > 1U) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* The attachment the read buffer names, which must have something attached. */
	index = fbo->read_buffer - GL_COLOR_ATTACHMENT0;
	image = &images.colors[index];
	if (index >= images.color_count || image->format == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* The draw surface's frame records the copy, with no pass open in it. */
	surface = context->draw;
	if (surface == NULL) {
		gles_error(context, GL_INVALID_FRAMEBUFFER_OPERATION);
		return -1;
	}

	/* The frame, opened when this is its first command. */
	error = zegl_frame_begin(surface);
	if (error != EGL_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* No pass open in it. */
	gles_target_close(state);
	framebuffer_leave(state, surface);

	/* Succeeded: the attachment's image (a renderbuffer's or a texture's), level and layer, with GL's rows. */
	read->surface = surface;
	if (image->renderbuffer != NULL) {
		read->image = image->renderbuffer->image;
	} else {
		read->image = image->texture->image;
	}

	/* The copy's layer. */
	read->layer = image->layer;
	read->level = image->level;
	read->layout = image->layout;
	read->format = image->format;
	read->extent = fbo->extent;
	if (image->volume) {
		read->layer = 0U;
		read->slice = image->layer;
		read->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	/* Succeeded: the read comes from there. */
	return 0;
}

/*
 * Returns the format of the read framebuffer's read buffer: a framebuffer
 * object's colour attachment's (NULL when it has none there), or the
 * surfaces' RGBA8.
 */
const struct gles_format *
gles_read_buffer_format(
	struct gles_state *state)
{
	struct framebuffer_image image;
	struct gles_framebuffer *fbo;
	GLenum status;

	/* The surfaces' image is RGBA8 (or BGRA8, read as RGBA8). */
	if (state->read_framebuffer == 0U)
		return gles_format_rgba8();

	/* The object's read buffer names an attachment with something attached. */
	fbo = framebuffer_named(state, state->read_framebuffer);
	if (fbo == NULL || fbo->read_buffer == GL_NONE)
		return NULL;
	status = framebuffer_image_of(state, &fbo->colors[fbo->read_buffer - GL_COLOR_ATTACHMENT0], 0, &image);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		return NULL;

	/* Succeeded: its format (NULL when nothing is attached). */
	return image.format;
}

/*
 * Detaches a deleted texture or renderbuffer from the bound draw and read
 * framebuffer objects (GL detaches it only there; elsewhere its name is
 * looked up again and finds nothing).
 */
void
gles_framebuffers_forget(
	struct gles_state *state,
	int kind,
	GLuint name)
{
	struct gles_framebuffer *bound[2];
	struct gles_framebuffer *fbo;
	unsigned which;
	unsigned index;

	/* The draw and the read framebuffer objects. */
	bound[0] = framebuffer_named(state, state->framebuffer);
	bound[1] = framebuffer_named(state, state->read_framebuffer);
	for (which = 0U; which < 2U; which++) {
		fbo = bound[which];
		if (fbo == NULL)
			continue;

		/* Each colour attachment point that names the object. */
		for (index = 0U; index < GLES_COLOR_ATTACHMENTS; index++) {
			if (fbo->colors[index].kind == kind && fbo->colors[index].name == name)
				fbo->colors[index].kind = GLES_ATTACH_NONE;
		}

		/* The depth and stencil points. */
		if (fbo->depth.kind == kind && fbo->depth.name == name)
			fbo->depth.kind = GLES_ATTACH_NONE;
		if (fbo->stencil.kind == kind && fbo->stencil.name == name)
			fbo->stencil.kind = GLES_ATTACH_NONE;
	}
}

/*
 * Frees every framebuffer object and renderbuffer of a context, and the
 * render passes its pipelines were made with (nothing may still run).
 */
void
gles_framebuffers_release(
	struct gles_state *state)
{
	struct gles_framebuffer *fbo;
	struct gles_renderbuffer *renderbuffer;
	struct gles_compatible_pass *compatible;
	GLuint name;

	/* The framebuffer objects, whose passes and framebuffers wait for the garbage. */
	state->open_fbo = NULL;
	state->open_surface = NULL;
	for (name = 1U; name < state->framebuffers.capacity; name++) {
		fbo = state->framebuffers.objects[name];
		if (fbo == NULL)
			continue;

		/* Its pass and framebuffer and slice images, then the object. */
		framebuffer_forget_views(state, fbo);
		framebuffer_slices_free(state, fbo);
		state->framebuffers.objects[name] = NULL;
		free(fbo);
	}

	/* The renderbuffers. */
	for (name = 1U; name < state->renderbuffers.capacity; name++) {
		renderbuffer = state->renderbuffers.objects[name];
		if (renderbuffer == NULL)
			continue;

		/* Its image, then the object. */
		state->renderbuffers.objects[name] = NULL;
		renderbuffer_free(state, renderbuffer);
	}

	/* What blits resolved depth and stencil with. */
	framebuffer_resolve_free(state);

	/* The passes pipelines were made with. */
	while (state->compatible_passes != NULL) {
		compatible = state->compatible_passes;
		state->compatible_passes = compatible->next;
		vkDestroyRenderPass(state->device, compatible->pass, NULL);
		free(compatible);
	}

	/* The name tables. */
	free(state->framebuffers.objects);
	free(state->renderbuffers.objects);
	memset(&state->framebuffers, 0, sizeof(state->framebuffers));
	memset(&state->renderbuffers, 0, sizeof(state->renderbuffers));
}

/*
 * Brings the levels on the CPU that framebuffer objects drew into (every
 * layer of each) up to date with the texture's image, before the CPU
 * changes the texture.  Returns 0, or -1 with the error recorded.
 */
int
gles_texture_fetch(
	struct zegl_context *context,
	struct gles_texture *texture)
{
	struct gles_state *state;
	struct zegl_surface *surface;
	VkImageMemoryBarrier barrier;
	VkBufferImageCopy copy;
	VkBuffer buffer;
	VkDeviceSize offset;
	VkImageAspectFlags copy_aspect;
	VkImageAspectFlags whole_aspect;
	const struct gles_format *format;
	struct gles_level *level;
	unsigned char *mapped[GLES_FACES * GLES_LEVELS];
	unsigned char *pixels;
	size_t bytes;
	uint32_t layers;
	uint32_t slices;
	uint32_t base_layer;
	unsigned face;
	unsigned index;
	GLint image_level;
	int written;
	EGLint error;

	/* A context with its state. */
	state = gles_state(context);
	if (state == NULL)
		return -1;

	/* Nothing drawn into since the last read, or no image. */
	written = 0;
	for (face = 0U; face < GLES_FACES; face++) {
		if (texture->gpu_levels[face] != 0U)
			written = 1;
	}

	/* Nothing to read back (a multisample texture's samples stay on the device). */
	if (!written || texture->image == VK_NULL_HANDLE || texture->samples > 1U) {
		memset(texture->gpu_levels, 0, sizeof(texture->gpu_levels));
		return 0;
	}

	/* The frame of the draw surface, which drew into it, with no pass open. */
	surface = context->draw;
	if (surface == NULL) {
		memset(texture->gpu_levels, 0, sizeof(texture->gpu_levels));
		return 0;
	}

	/* No pass open in the frame. */
	gles_target_close(state);
	error = zegl_frame_begin(surface);
	if (error != EGL_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* Nor the surface's. */
	framebuffer_leave(state, surface);

	/* A depth format is copied from its depth aspect; a barrier names every aspect it has. */
	format = texture->image_format;
	copy_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	whole_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	if (format->kind == GLES_TEXEL_DEPTH) {
		copy_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		whole_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (format->stencil)
			whole_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}

	/* Each level drawn into, copied out with every layer between two layout changes (its rows are GL's already). */
	memset(mapped, 0, sizeof(mapped));
	for (index = 0U; index < GLES_FACES * GLES_LEVELS; index++) {
		face = index / GLES_LEVELS;
		if ((texture->gpu_levels[face] & (1U << (index % GLES_LEVELS))) == 0U)
			continue;

		/* The level, which the image has from the base level on. */
		level = &texture->levels[index];
		image_level = (GLint)(index % GLES_LEVELS) - texture->base_level;
		if (image_level < 0 || (uint32_t)image_level >= texture->level_count || level->format != format)
			continue;

		/* A cube map's face is a layer; a 2D array's level has all its layers, a 3D texture's all its slices. */
		layers = 1U;
		slices = 1U;
		base_layer = 0U;
		if (texture->target == GL_TEXTURE_CUBE_MAP)
			base_layer = face;
		if (texture->target == GL_TEXTURE_2D_ARRAY || texture->target == GL_TEXTURE_CUBE_MAP_ARRAY)
			layers = (uint32_t)level->depth;
		if (texture->target == GL_TEXTURE_3D)
			slices = (uint32_t)level->depth;

		/* Room in the stream for the level. */
		bytes = (size_t)level->width * (size_t)level->height * (size_t)layers * (size_t)slices * format->bytes;
		mapped[index] = gles_stream(state, bytes, 16U, &buffer, &offset);
		if (mapped[index] == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return -1;
		}

		/* Ready to be copied. */
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = texture->image;
		barrier.subresourceRange.aspectMask = whole_aspect;
		barrier.subresourceRange.baseMipLevel = (uint32_t)image_level;
		barrier.subresourceRange.levelCount = 1U;
		barrier.subresourceRange.baseArrayLayer = base_layer;
		barrier.subresourceRange.layerCount = layers;
		vkCmdPipelineBarrier(surface->command,
				     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				     VK_PIPELINE_STAGE_TRANSFER_BIT,
				     0U,
				     0U,
				     NULL,
				     0U,
				     NULL,
				     1U,
				     &barrier);

		/* The copy of the level's layers. */
		memset(&copy, 0, sizeof(copy));
		copy.bufferOffset = offset;
		copy.imageSubresource.aspectMask = copy_aspect;
		copy.imageSubresource.mipLevel = (uint32_t)image_level;
		copy.imageSubresource.baseArrayLayer = base_layer;
		copy.imageSubresource.layerCount = layers;
		copy.imageExtent.width = (uint32_t)level->width;
		copy.imageExtent.height = (uint32_t)level->height;
		copy.imageExtent.depth = slices;
		vkCmdCopyImageToBuffer(surface->command, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1U, &copy);

		/* A depth and stencil format's stencil indices after the depths (4 bytes a texel; the level keeps 8). */
		if (format->stencil) {
			copy.bufferOffset = offset + bytes / format->bytes * 4U;
			copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
			vkCmdCopyImageToBuffer(surface->command, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1U, &copy);
		}

		/* Back to be sampled. */
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(surface->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
				     0U, 0U, NULL, 0U, NULL, 1U, &barrier);
	}

	/* Done before the bytes are read. */
	surface->recorded = 1;
	error = zegl_frame_flush(surface);
	if (error != EGL_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* Each level's bytes into place (a level made without data has no pixels yet). */
	for (index = 0U; index < GLES_FACES * GLES_LEVELS; index++) {
		if (mapped[index] == NULL)
			continue;

		/* The level's pixels. */
		level = &texture->levels[index];
		bytes = (size_t)level->width * (size_t)level->height * (size_t)level->depth * format->bytes;
		pixels = level->pixels;
		if (pixels == NULL) {
			pixels = malloc(bytes + 1U);
			if (pixels == NULL) {
				gles_error(context, GL_OUT_OF_MEMORY);
				return -1;
			}

			/* The level keeps them. */
			level->pixels = pixels;
		}

		/* The bytes (a depth and stencil format's two aspects put together again). */
		if (format->stencil) {
			framebuffer_join_stencil(pixels, mapped[index], bytes / format->bytes);
		} else {
			memcpy(pixels, mapped[index], bytes);
		}
	}

	/* The CPU's copy is the newest again. */
	memset(texture->gpu_levels, 0, sizeof(texture->gpu_levels));

	/* Everything recorded before is done: the frame's resources are free again. */
	gles_frame_finished(state, 1);

	/* Succeeded: each level drawn into is what the device drew. */
	return 0;
}

/*
 * Makes framebuffer names.
 */
GL_APICALL void GL_APIENTRY
glGenFramebuffers(
	GLsizei n,
	GLuint *framebuffers)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_framebuffer *fbo;
	GLsizei index;
	GLuint name;
	int status;

	/* A context with its state and a count. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name gets its object now, so the next name is another; it becomes a framebuffer at its first bind. */
	for (index = 0; index < n; index++) {
		name = gles_names_free(&state->framebuffers);
		fbo = framebuffer_new(name);
		if (fbo == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* Under the name. */
		status = gles_names_add(&state->framebuffers, name, fbo);
		if (status != 0) {
			free(fbo);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name goes back to the application. */
		framebuffers[index] = name;
	}
}

/*
 * Deletes framebuffer objects, binding 0 in place of a deleted one.
 */
GL_APICALL void GL_APIENTRY
glDeleteFramebuffers(
	GLsizei n,
	const GLuint *framebuffers)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_framebuffer *fbo;
	GLsizei index;

	/* A context with its state and a count. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name that is a framebuffer object. */
	for (index = 0; index < n; index++) {
		fbo = gles_names_get(&state->framebuffers, framebuffers[index]);
		if (fbo == NULL)
			continue;

		/* Its pass ends if it is open; a binding of it goes back to 0. */
		if (state->open_fbo == fbo)
			gles_target_close(state);
		if (state->framebuffer == framebuffers[index])
			state->framebuffer = 0U;
		if (state->read_framebuffer == framebuffers[index])
			state->read_framebuffer = 0U;

		/* The name, the pass, framebuffer and slice images (they wait for the frame), and the object go. */
		gles_names_remove(&state->framebuffers, framebuffers[index]);
		framebuffer_forget_views(state, fbo);
		framebuffer_slices_free(state, fbo);
		free(fbo);
	}
}

/*
 * Binds a framebuffer (0: the surfaces') for drawing, reading or both,
 * making the object when the name is new.
 */
GL_APICALL void GL_APIENTRY
glBindFramebuffer(
	GLenum target,
	GLuint framebuffer)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_framebuffer *fbo;
	int read;
	int status;

	/* A context with its state and a framebuffer target. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	status = framebuffer_target(context, target, &read);
	if (status != 0)
		return;

	/* A name seen for the first time gets its object, with nothing attached. */
	fbo = NULL;
	if (framebuffer != 0U)
		fbo = gles_names_get(&state->framebuffers, framebuffer);
	if (framebuffer != 0U && fbo == NULL) {
		fbo = framebuffer_new(framebuffer);
		if (fbo == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name finds it from now on. */
		status = gles_names_add(&state->framebuffers, framebuffer, fbo);
		if (status != 0) {
			free(fbo);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}
	}

	/* Bound for reading, for drawing, or for both (GL_FRAMEBUFFER); a named object is a framebuffer from now on. */
	if (fbo != NULL)
		fbo->bound = 1;
	if (read != 0)
		state->read_framebuffer = framebuffer;
	if (read != 1)
		state->framebuffer = framebuffer;
}

/*
 * Reports whether a name is a framebuffer object.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsFramebuffer(
	GLuint framebuffer)
{
	struct gles_state *state;
	struct gles_framebuffer *fbo;

	/* A context with its state. */
	state = gles_state(gles_context());
	if (state == NULL)
		return GL_FALSE;

	/* A name that was bound once. */
	fbo = gles_names_get(&state->framebuffers, framebuffer);
	if (fbo == NULL || !fbo->bound)
		return GL_FALSE;

	/* Succeeded: it is one. */
	return GL_TRUE;
}

/*
 * Reports whether a bound framebuffer can be drawn into or read from, and
 * when not, why.
 */
GL_APICALL GLenum GL_APIENTRY
glCheckFramebufferStatus(
	GLenum target)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_framebuffer *fbo;
	struct framebuffer_images images;
	GLuint name;
	GLenum status;

	/* A context with its state, and the framebuffer bound to the target. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return 0U;
	name = 0U;
	fbo = framebuffer_for(context, state, target, 0, &name);
	if (fbo == NULL && name == (GLuint)-1)
		return 0U;

	/* The surfaces' framebuffer is complete. */
	if (name == 0U)
		return GL_FRAMEBUFFER_COMPLETE;

	/* A framebuffer object's attachments decide. */
	status = GL_FRAMEBUFFER_UNSUPPORTED;
	if (fbo != NULL)
		status = framebuffer_status(state, fbo, &images);

	/* Succeeded: the status. */
	return status;
}

/*
 * Attaches a level of a 2D texture or of a cube map's face to an
 * attachment point of a bound framebuffer object (texture 0 detaches).
 */
GL_APICALL void GL_APIENTRY
glFramebufferTexture2D(
	GLenum target,
	GLenum attachment,
	GLenum textarget,
	GLuint texture,
	GLint level)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *object;
	unsigned face;
	int rectangle;
	GLenum kind;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* Texture 0 detaches. */
	if (texture == 0U) {
		framebuffer_attach(context, target, attachment, GLES_ATTACH_NONE, 0U, 0U, 0, 0);
		return;
	}

	/* A 2D texture, a face of a cube map, or (desktop GL, libGL) a rectangle or (and OpenGL ES 3.1) multisample texture's one level. */
	rectangle = 0;
	if (textarget == GL_TEXTURE_RECTANGLE && gles_fixed != NULL)
		rectangle = 1;
	if (textarget == GL_TEXTURE_2D_MULTISAMPLE && (gles_fixed != NULL || state->compute))
		rectangle = 1;
	if (textarget != GL_TEXTURE_2D &&
	    !rectangle &&
	    (textarget < GL_TEXTURE_CUBE_MAP_POSITIVE_X || textarget > GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A rectangle texture has level 0 only. */
	if (rectangle && level != 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A level a texture has. */
	if (level < 0 || level >= (GLint)GLES_LEVELS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A name that is a texture. */
	object = gles_names_get(&state->textures, texture);
	if (object == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A face needs a cube map, GL_TEXTURE_2D a 2D texture, GL_TEXTURE_RECTANGLE a rectangle texture. */
	face = 0U;
	kind = GL_TEXTURE_2D;
	if (rectangle) {
		kind = textarget;
	} else if (textarget != GL_TEXTURE_2D) {
		face = textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
		kind = GL_TEXTURE_CUBE_MAP;
	}

	/* The texture must be of that kind. */
	if (object->target != kind) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Attached by name. */
	framebuffer_attach(context, target, attachment, GLES_ATTACH_TEXTURE, texture, face, level, 0);
}

/*
 * Attaches a level of a texture to an attachment point of a bound
 * framebuffer object (desktop GL 3.2, libGL): a 2D or rectangle texture's
 * level as glFramebufferTexture2D does, every layer of a 2D array
 * texture's or every face of a cube map's level as a layered attachment
 * (a geometry shader's gl_Layer chooses the layer); texture 0 detaches.
 */
GL_APICALL void GL_APIENTRY
glFramebufferTexture(
	GLenum target,
	GLenum attachment,
	GLuint texture,
	GLint level)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *object;

	/* A context with its state (desktop GL). */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (gles_fixed == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Texture 0 detaches. */
	if (texture == 0U) {
		framebuffer_attach(context, target, attachment, GLES_ATTACH_NONE, 0U, 0U, 0, 0);
		return;
	}

	/* A level a texture has, of a name that is a texture. */
	if (level < 0 || level >= (GLint)GLES_LEVELS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The texture of the name. */
	object = gles_names_get(&state->textures, texture);
	if (object == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A 2D or rectangle texture's level, as itself. */
	if (object->target == GL_TEXTURE_2D || object->target == GL_TEXTURE_RECTANGLE) {
		framebuffer_attach(context, target, attachment, GLES_ATTACH_TEXTURE, texture, 0U, level, 0);
		return;
	}

	/* Layered: a 2D array texture's, a cube map's or a cube map array's level (a 3D texture's slices are not). */
	if (object->target != GL_TEXTURE_2D_ARRAY &&
	    object->target != GL_TEXTURE_CUBE_MAP &&
	    object->target != GL_TEXTURE_CUBE_MAP_ARRAY) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Attached with every layer. */
	framebuffer_attach(context, target, attachment, GLES_ATTACH_TEXTURE, texture, 0U, level, (GLint)GLES_LAYER_ALL);
}

/*
 * Attaches a layer of a level of a 2D array texture, or a slice of a 3D
 * texture's, to an attachment point of a bound framebuffer object
 * (texture 0 detaches).
 */
GL_APICALL void GL_APIENTRY
glFramebufferTextureLayer(
	GLenum target,
	GLenum attachment,
	GLuint texture,
	GLint level,
	GLint layer)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *object;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* Texture 0 detaches. */
	if (texture == 0U) {
		framebuffer_attach(context, target, attachment, GLES_ATTACH_NONE, 0U, 0U, 0, 0);
		return;
	}

	/* A level and a layer a texture has. */
	if (level < 0 || level >= (GLint)GLES_LEVELS || layer < 0 || layer >= GLES_MAX_LAYERS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A name that is a 2D array, cube map array (its layer-faces) or 3D texture. */
	object = gles_names_get(&state->textures, texture);
	if (object == NULL ||
	    (object->target != GL_TEXTURE_2D_ARRAY && object->target != GL_TEXTURE_3D && object->target != GL_TEXTURE_CUBE_MAP_ARRAY)) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Attached by name. */
	framebuffer_attach(context, target, attachment, GLES_ATTACH_TEXTURE, texture, 0U, level, layer);
}

/*
 * Attaches a renderbuffer to an attachment point of a bound framebuffer
 * object (renderbuffer 0 detaches).
 */
GL_APICALL void GL_APIENTRY
glFramebufferRenderbuffer(
	GLenum target,
	GLenum attachment,
	GLenum renderbuffertarget,
	GLuint renderbuffer)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_renderbuffer *object;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* Renderbuffer 0 detaches. */
	if (renderbuffer == 0U) {
		framebuffer_attach(context, target, attachment, GLES_ATTACH_NONE, 0U, 0U, 0, 0);
		return;
	}

	/* The renderbuffer target. */
	if (renderbuffertarget != GL_RENDERBUFFER) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A name that is a renderbuffer. */
	object = gles_names_get(&state->renderbuffers, renderbuffer);
	if (object == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Attached by name. */
	framebuffer_attach(context, target, attachment, GLES_ATTACH_RENDERBUFFER, renderbuffer, 0U, 0, 0);
}

/*
 * Reports what an attachment point of a bound framebuffer has: an object
 * attached to a framebuffer object's, or the surfaces' own buffers.
 */
GL_APICALL void GL_APIENTRY
glGetFramebufferAttachmentParameteriv(
	GLenum target,
	GLenum attachment,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_framebuffer *fbo;
	struct gles_attachment *point;
	struct gles_attachment *other;
	const struct gles_format *format;
	GLuint name;
	int known;
	int status;

	/* A context with its state, and the framebuffer bound to the target. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	name = 0U;
	fbo = framebuffer_for(context, state, target, 0, &name);
	if (fbo == NULL && name == (GLuint)-1)
		return;

	/* The surfaces' framebuffer: its colour buffer (GL_BACK) and depth and stencil buffers, RGBA8 and the depth format. */
	if (name == 0U) {
		if (attachment != GL_BACK && attachment != GL_DEPTH && attachment != GL_STENCIL) {
			gles_error(context, GL_INVALID_ENUM);
			return;
		}

		/* The parameter. */
		format = gles_format_rgba8();
		switch (pname) {
		case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
			*params = GL_FRAMEBUFFER_DEFAULT;
			break;
		case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
			*params = GL_UNSIGNED_NORMALIZED;
			break;
		case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
			*params = GL_LINEAR;
			break;
		case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
		case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
		case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
		case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
			*params = 0;
			if (attachment == GL_BACK)
				*params = framebuffer_bits(format, format->vk, pname);
			break;
		case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
			*params = 0;
			if (attachment == GL_DEPTH && context->config != NULL)
				*params = context->config->depth;
			break;
		case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
			*params = 0;
			if (attachment == GL_STENCIL && context->config != NULL)
				*params = context->config->stencil;
			break;
		default:
			gles_error(context, GL_INVALID_ENUM);
			break;
		}

		/* The surfaces' buffers are done. */
		return;
	}

	/* An attachment point of the framebuffer object (depth and stencil must name the same object to be asked together). */
	if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
		point = &fbo->depth;
		other = &fbo->stencil;
		if (point->kind != other->kind || point->name != other->name) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}
	} else {
		status = framebuffer_point(state, attachment, &point, fbo);
		if (status != 0) {
			gles_error(context, (GLenum)status);
			return;
		}
	}

	/* The parameter, when the point has something attached or asks what it has. */
	known = 1;
	*params = framebuffer_attachment_value(state, point, pname, &known);
	if (!known)
		gles_error(context, GL_INVALID_ENUM);
}

/*
 * Chooses the colour attachments the draw buffers of the draw framebuffer
 * write (draw buffer i writes GL_COLOR_ATTACHMENTi or nothing; the
 * surfaces' one writes GL_BACK or nothing).
 */
GL_APICALL void GL_APIENTRY
glDrawBuffers(
	GLsizei n,
	const GLenum *bufs)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_framebuffer *fbo;
	GLenum buffers[GLES_DRAW_BUFFERS];
	GLsizei index;
	unsigned named;
	unsigned bit;

	/* A context with its state, and no more buffers than there are. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0 || n > (GLsizei)state->color_attachments) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The surfaces' framebuffer: one buffer, GL_BACK or GL_NONE. */
	if (state->framebuffer == 0U) {
		if (n != 1 || (bufs[0] != GL_BACK && bufs[0] != GL_NONE)) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* Succeeded: the surface's buffer. */
		state->default_draw_buffer = bufs[0];
		return;
	}

	/*
	 * Each buffer of a framebuffer object: its own attachment or none
	 * (desktop GL, libGL: any attachment, each named once).
	 */
	named = 0U;
	for (index = 0; index < (GLsizei)GLES_DRAW_BUFFERS; index++) {
		buffers[index] = GL_NONE;
		if (index >= n)
			continue;

		/* None. */
		if (bufs[index] == GL_NONE)
			continue;

		/* GL_BACK and the other attachments are not this buffer's in OpenGL ES. */
		if (gles_fixed == NULL && bufs[index] != GL_COLOR_ATTACHMENT0 + (GLenum)index) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* An attachment there is. */
		if (bufs[index] < GL_COLOR_ATTACHMENT0 || bufs[index] >= GL_COLOR_ATTACHMENT0 + state->color_attachments) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* Not named by an earlier buffer. */
		bit = 1U << (bufs[index] - GL_COLOR_ATTACHMENT0);
		if ((named & bit) != 0U) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* The buffer. */
		named |= bit;
		buffers[index] = bufs[index];
	}

	/* Succeeded: the object's draw buffers (pipelines follow them). */
	fbo = framebuffer_named(state, state->framebuffer);
	if (fbo != NULL)
		memcpy(fbo->draw_buffers, buffers, sizeof(buffers));
}

/*
 * Chooses the colour buffer reads of the read framebuffer read.
 */
GL_APICALL void GL_APIENTRY
glReadBuffer(
	GLenum src)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_framebuffer *fbo;

	/* A context with its state; desktop GL's front buffer (libGL's) is the one colour buffer, GL_BACK. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (gles_fixed != NULL && src == GL_FRONT)
		src = GL_BACK;

	/* A name of a colour buffer. */
	if (src != GL_NONE &&
	    src != GL_BACK &&
	    (src < GL_COLOR_ATTACHMENT0 || src > GL_COLOR_ATTACHMENT15)) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The surfaces' framebuffer reads GL_BACK or nothing. */
	if (state->read_framebuffer == 0U) {
		if (src != GL_BACK && src != GL_NONE) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* Succeeded: the surface's read buffer. */
		state->default_read_buffer = src;
		return;
	}

	/* A framebuffer object reads one of its colour attachments or nothing. */
	if (src == GL_BACK || src >= GL_COLOR_ATTACHMENT0 + state->color_attachments) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Succeeded: the object's read buffer. */
	fbo = framebuffer_named(state, state->read_framebuffer);
	if (fbo != NULL)
		fbo->read_buffer = src;
}

/*
 * Lets the contents of a framebuffer's attachments become undefined;
 * they are kept as they are.
 */
GL_APICALL void GL_APIENTRY
glInvalidateFramebuffer(
	GLenum target,
	GLsizei numAttachments,
	const GLenum *attachments)
{
	struct zegl_context *context;
	int read;
	int status;

	/* A framebuffer target and a count. */
	(void)attachments;
	context = gles_context();
	if (context == NULL)
		return;
	status = framebuffer_target(context, target, &read);
	if (status != 0)
		return;
	if (numAttachments < 0)
		gles_error(context, GL_INVALID_VALUE);
}

/*
 * Lets a rectangle of a framebuffer's attachments become undefined; it is
 * kept as it is.
 */
GL_APICALL void GL_APIENTRY
glInvalidateSubFramebuffer(
	GLenum target,
	GLsizei numAttachments,
	const GLenum *attachments,
	GLint x,
	GLint y,
	GLsizei width,
	GLsizei height)
{
	struct zegl_context *context;
	int read;
	int status;

	/* A framebuffer target, a count and a size. */
	(void)attachments;
	(void)x;
	(void)y;
	context = gles_context();
	if (context == NULL)
		return;
	status = framebuffer_target(context, target, &read);
	if (status != 0)
		return;
	if (numAttachments < 0 || width < 0 || height < 0)
		gles_error(context, GL_INVALID_VALUE);
}

/*
 * Makes renderbuffer names.
 */
GL_APICALL void GL_APIENTRY
glGenRenderbuffers(
	GLsizei n,
	GLuint *renderbuffers)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_renderbuffer *object;
	GLsizei index;
	GLuint name;
	int status;

	/* A context with its state and a count. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name gets its object now, so the next name is another; it becomes a renderbuffer at its first bind. */
	for (index = 0; index < n; index++) {
		name = gles_names_free(&state->renderbuffers);
		object = calloc(1U, sizeof(*object));
		if (object == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* Under the name. */
		object->name = name;
		status = gles_names_add(&state->renderbuffers, name, object);
		if (status != 0) {
			free(object);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name goes back to the application. */
		renderbuffers[index] = name;
	}
}

/*
 * Deletes renderbuffers, detaching them from the bound framebuffer
 * objects.
 */
GL_APICALL void GL_APIENTRY
glDeleteRenderbuffers(
	GLsizei n,
	const GLuint *renderbuffers)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_renderbuffer *renderbuffer;
	GLsizei index;

	/* A context with its state and a count. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name that is a renderbuffer. */
	for (index = 0; index < n; index++) {
		renderbuffer = gles_names_get(&state->renderbuffers, renderbuffers[index]);
		if (renderbuffer == NULL)
			continue;

		/* An open pass may draw into it: it ends; the bound one goes back to 0. */
		gles_target_close(state);
		if (state->renderbuffer == renderbuffers[index])
			state->renderbuffer = 0U;

		/* Detached, and the name and the image (it waits for the frame) go. */
		gles_framebuffers_forget(state, GLES_ATTACH_RENDERBUFFER, renderbuffers[index]);
		gles_names_remove(&state->renderbuffers, renderbuffers[index]);
		renderbuffer_free(state, renderbuffer);
	}
}

/*
 * Binds a renderbuffer, making the object when the name is new.
 */
GL_APICALL void GL_APIENTRY
glBindRenderbuffer(
	GLenum target,
	GLuint renderbuffer)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_renderbuffer *object;
	int status;

	/* A context with its state and the one target. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (target != GL_RENDERBUFFER) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A name seen for the first time gets its object, with no storage. */
	object = NULL;
	if (renderbuffer != 0U)
		object = gles_names_get(&state->renderbuffers, renderbuffer);
	if (renderbuffer != 0U && object == NULL) {
		object = calloc(1U, sizeof(*object));
		if (object == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name finds it from now on. */
		object->name = renderbuffer;
		status = gles_names_add(&state->renderbuffers, renderbuffer, object);
		if (status != 0) {
			free(object);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}
	}

	/* Bound (a named object is a renderbuffer from now on); glRenderbufferStorage gives it storage. */
	if (object != NULL)
		object->bound = 1;
	state->renderbuffer = renderbuffer;
}

/*
 * Reports whether a name is a renderbuffer.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsRenderbuffer(
	GLuint renderbuffer)
{
	struct gles_state *state;
	struct gles_renderbuffer *object;

	/* A context with its state. */
	state = gles_state(gles_context());
	if (state == NULL)
		return GL_FALSE;

	/* A name that was bound once. */
	object = gles_names_get(&state->renderbuffers, renderbuffer);
	if (object == NULL || !object->bound)
		return GL_FALSE;

	/* Succeeded: it is one. */
	return GL_TRUE;
}

/*
 * Gives the bound renderbuffer storage of a format and size, one sample
 * per pixel: a new device image (the old one waits for the frame).
 */
GL_APICALL void GL_APIENTRY
glRenderbufferStorage(
	GLenum target,
	GLenum internalformat,
	GLsizei width,
	GLsizei height)
{
	/* No multisampling. */
	glRenderbufferStorageMultisample(target, 0, internalformat, width, height);
}

/*
 * Gives the bound renderbuffer storage of a format and size with at least
 * a number of samples per pixel (0: one): a new device image (the old one
 * waits for the frame).  Every error is found before the old storage
 * goes, so that a refused call leaves it.
 */
GL_APICALL void GL_APIENTRY
glRenderbufferStorageMultisample(
	GLenum target,
	GLsizei samples,
	GLenum internalformat,
	GLsizei width,
	GLsizei height)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_renderbuffer *renderbuffer;
	struct gles_renderbuffer chosen;
	uint32_t count;
	uint32_t most;
	uint32_t mask;
	int multisampled;
	int integer;
	int status;

	/* The bound renderbuffer. */
	context = gles_context();
	state = gles_state(context);
	renderbuffer = renderbuffer_bound(context, target);
	if (renderbuffer == NULL)
		return;

	/* A size that fits, and no more samples than the most. */
	if (width < 0 ||
	    height < 0 ||
	    samples < 0 ||
	    width > FRAMEBUFFER_SIZE_MAX ||
	    height > FRAMEBUFFER_SIZE_MAX) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* No more samples than the device's most. */
	most = gles_samples_max(state);
	if ((uint32_t)samples > most) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A format a framebuffer draws into: colour, or depth and stencil. */
	memset(&chosen, 0, sizeof(chosen));
	status = renderbuffer_format(state, internalformat, &chosen);
	if (status != 0) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* An integer format's samples: none in OpenGL ES 3.0 (stencil only holds no integers). */
	integer = 0;
	if (chosen.kept->kind == GLES_TEXEL_INT || chosen.kept->kind == GLES_TEXEL_UINT)
		integer = 1;
	multisampled = framebuffer_integer_multisampled(state);
	if (integer && samples > 0 && !multisampled) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/*
	 * No more samples than the format's most (its largest count of
	 * glGetInternalformativ: an integer format's are those of integers,
	 * GL_MAX_INTEGER_SAMPLES, and every format's those the device makes
	 * an image of it with).
	 */
	mask = gles_format_samples(state, GL_RENDERBUFFER, internalformat);
	most = framebuffer_samples_most(mask);
	if ((uint32_t)samples > most) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A pass that draws into the old image ends; the image waits for the frame. */
	gles_target_close(state);
	renderbuffer_discard(state, renderbuffer);

	/* The format chosen. */
	renderbuffer->kept = chosen.kept;
	renderbuffer->vk = chosen.vk;
	renderbuffer->depth = chosen.depth;
	renderbuffer->aspects = chosen.aspects;

	/* The fewest samples the format has of at least as many as asked for (none asked for: one). */
	count = 1U;
	if (samples > 0)
		count = framebuffer_samples_for(mask, (uint32_t)samples);

	/* The new storage (a zero size has no image and leaves a framebuffer incomplete). */
	renderbuffer->format = internalformat;
	renderbuffer->width = width;
	renderbuffer->height = height;
	renderbuffer->samples = count;
	if (width == 0 || height == 0)
		return;

	/* Its image. */
	status = renderbuffer_image(state, renderbuffer);
	if (status < 0)
		gles_error(context, GL_OUT_OF_MEMORY);

	/*
	 * Storage the device does not make fails as storage the GL cannot
	 * create (GL_OUT_OF_MEMORY, as glRenderbufferStorage defines it), is
	 * reported once, and leaves a framebuffer with it unsupported.
	 */
	if (status > 0) {
		gles_image_unmade(renderbuffer->vk, "a renderbuffer");
		gles_error(context, GL_OUT_OF_MEMORY);
	}
}

/*
 * Reports a parameter of the bound renderbuffer.
 */
GL_APICALL void GL_APIENTRY
glGetRenderbufferParameteriv(
	GLenum target,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_renderbuffer *renderbuffer;

	/* The bound renderbuffer. */
	context = gles_context();
	renderbuffer = renderbuffer_bound(context, target);
	if (renderbuffer == NULL)
		return;

	/* The parameter. */
	switch (pname) {
	case GL_RENDERBUFFER_WIDTH:
		*params = renderbuffer->width;
		break;
	case GL_RENDERBUFFER_HEIGHT:
		*params = renderbuffer->height;
		break;
	case GL_RENDERBUFFER_INTERNAL_FORMAT:
		*params = GL_RGBA4;
		if (renderbuffer->format != 0U)
			*params = (GLint)renderbuffer->format;
		break;
	case GL_RENDERBUFFER_SAMPLES:
		*params = 0;
		if (renderbuffer->samples > 1U)
			*params = (GLint)renderbuffer->samples;
		break;
	case GL_RENDERBUFFER_RED_SIZE:
	case GL_RENDERBUFFER_GREEN_SIZE:
	case GL_RENDERBUFFER_BLUE_SIZE:
	case GL_RENDERBUFFER_ALPHA_SIZE:
	case GL_RENDERBUFFER_DEPTH_SIZE:
	case GL_RENDERBUFFER_STENCIL_SIZE:
		*params = framebuffer_bits(renderbuffer->kept, renderbuffer->vk, pname);
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		break;
	}
}

/*
 * Sets a parameter of the framebuffer object bound to a target that it
 * has with nothing attached (OpenGL ES 3.1): its width, height, samples
 * or whether its sample locations are fixed.
 */
GL_APICALL void GL_APIENTRY
glFramebufferParameteri(
	GLenum target,
	GLenum pname,
	GLint param)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_framebuffer *fbo;
	GLuint name;
	GLint most;

	/* A context of OpenGL ES 3.1 with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (gles_fixed == NULL && !state->compute) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The object bound to the target (framebuffer 0 has no parameters). */
	fbo = framebuffer_for(context, state, target, 1, &name);
	if (fbo == NULL)
		return;

	/* The parameter, at most its limit. */
	switch (pname) {
	case GL_FRAMEBUFFER_DEFAULT_WIDTH:
		most = (GLint)state->limits.maxFramebufferWidth;
		break;
	case GL_FRAMEBUFFER_DEFAULT_HEIGHT:
		most = (GLint)state->limits.maxFramebufferHeight;
		break;
	case GL_FRAMEBUFFER_DEFAULT_SAMPLES:
		most = (GLint)gles_framebuffer_default_samples_max(state);
		break;
	case GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS:
		most = INT32_MAX;
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return;
	}
	if (param < 0 || param > most) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the parameter, which the next completeness check and draw take. */
	if (pname == GL_FRAMEBUFFER_DEFAULT_WIDTH)
		fbo->default_width = param;
	if (pname == GL_FRAMEBUFFER_DEFAULT_HEIGHT)
		fbo->default_height = param;
	if (pname == GL_FRAMEBUFFER_DEFAULT_SAMPLES)
		fbo->default_samples = param;
	if (pname == GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS)
		fbo->default_fixed_locations = param;
}

/*
 * Reads a parameter glFramebufferParameteri sets of the framebuffer
 * object bound to a target (OpenGL ES 3.1).
 */
GL_APICALL void GL_APIENTRY
glGetFramebufferParameteriv(
	GLenum target,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_framebuffer *fbo;
	GLuint name;

	/* A context of OpenGL ES 3.1 with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (gles_fixed == NULL && !state->compute) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The object bound to the target (framebuffer 0 has no parameters). */
	fbo = framebuffer_for(context, state, target, 1, &name);
	if (fbo == NULL)
		return;

	/* The parameter (a fixed sample location pattern reads as GL_TRUE or GL_FALSE). */
	switch (pname) {
	case GL_FRAMEBUFFER_DEFAULT_WIDTH:
		*params = fbo->default_width;
		return;
	case GL_FRAMEBUFFER_DEFAULT_HEIGHT:
		*params = fbo->default_height;
		return;
	case GL_FRAMEBUFFER_DEFAULT_SAMPLES:
		*params = fbo->default_samples;
		return;
	case GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS:
		*params = GL_FALSE;
		if (fbo->default_fixed_locations != 0)
			*params = GL_TRUE;
		return;
	default:
		break;
	}

	/* Not a parameter. */
	gles_error(context, GL_INVALID_ENUM);
}

/*
 * Returns the most samples a framebuffer object with nothing attached may
 * be asked for (GL_MAX_FRAMEBUFFER_SAMPLES): the device's most of its
 * framebufferNoAttachmentsSampleCounts, 0 when it has only one.
 */
uint32_t
gles_framebuffer_default_samples_max(
	struct gles_state *state)
{
	uint32_t count;

	/* The largest count of the device's. */
	for (count = 64U; count >= 2U; count /= 2U) {
		if ((state->limits.framebufferNoAttachmentsSampleCounts & count) != 0U)
			return count;
	}

	/* Succeeded: no multisampling without attachments. */
	return 0U;
}

/*
 * Returns the most samples per pixel a renderbuffer may have
 * (GL_MAX_SAMPLES): the device's most for every kind of attachment, 1
 * when it multisamples nothing.
 */
uint32_t
gles_samples_max(
	struct gles_state *state)
{
	uint32_t mask;
	uint32_t count;

	/* The largest count of the device's. */
	mask = framebuffer_samples_mask(state);
	count = framebuffer_samples_most(mask);

	/* Succeeded: the count. */
	return count;
}

/*
 * Returns the most samples per pixel a renderbuffer or multisample texture
 * of an integer format may have (GL_MAX_INTEGER_SAMPLES): the device's most
 * for every kind of attachment that it also samples of integers, 1 when it
 * multisamples no integers.
 */
uint32_t
gles_integer_samples_max(
	struct gles_state *state)
{
	uint32_t mask;
	uint32_t count;

	/* The largest count of the device's of integers. */
	mask = framebuffer_integer_mask(state);
	count = framebuffer_samples_most(mask);

	/* Succeeded: the count. */
	return count;
}

/*
 * Returns the most samples per pixel a multisample texture of a colour
 * format may have (GL_MAX_COLOR_TEXTURE_SAMPLES): the device's most for
 * every kind of attachment that it also samples of colours, 1 when it
 * samples no multisampled colour.
 */
uint32_t
gles_color_texture_samples_max(
	struct gles_state *state)
{
	uint32_t mask;
	uint32_t count;

	/* The largest count of the device's of sampled colours. */
	mask = framebuffer_color_texture_mask(state);
	count = framebuffer_samples_most(mask);

	/* Succeeded: the count. */
	return count;
}

/*
 * Returns the most samples per pixel a multisample texture of a depth or
 * depth and stencil format may have (GL_MAX_DEPTH_TEXTURE_SAMPLES): the
 * device's most for every kind of attachment that it also samples of
 * depth and of stencil, 1 when it samples no multisampled depth.
 */
uint32_t
gles_depth_texture_samples_max(
	struct gles_state *state)
{
	uint32_t mask;
	uint32_t count;

	/* The largest count of the device's of sampled depth and stencil. */
	mask = framebuffer_depth_texture_mask(state);
	count = framebuffer_samples_most(mask);

	/* Succeeded: the count. */
	return count;
}

/*
 * Returns the sample counts above one an internal format may have as a
 * renderbuffer (GL_RENDERBUFFER) or as a multisample texture
 * (GL_TEXTURE_2D_MULTISAMPLE), as Vulkan's bits: those of the target's
 * limit (a renderbuffer's are every attachment's, a texture's those the
 * device also samples of the format's kind; an integer format's those of
 * integers, none for a renderbuffer in OpenGL ES 3.0) that the device
 * makes an image of the format with, of the usage the GL makes it with.
 * A format no framebuffer draws into has none, and so has a texture of
 * stencil only.
 */
uint32_t
gles_format_samples(
	struct gles_state *state,
	GLenum target,
	GLenum internalformat)
{
	struct gles_renderbuffer probe;
	VkImageFormatProperties properties;
	const struct gles_format *format;
	VkImageUsageFlags usage;
	VkFormat vk;
	VkResult result;
	uint32_t mask;
	int integer;
	int status;

	/* The format: one of the table, or a renderbuffer's stencil only (no texture is of stencil only). */
	format = gles_format_renderable(state, internalformat);
	if (format == NULL &&
	    (target != GL_RENDERBUFFER ||
	     internalformat != GL_STENCIL_INDEX8))
		return 0U;

	/* Whether it holds integers (stencil only holds none). */
	integer = 0;
	if (format != NULL &&
	    (format->kind == GLES_TEXEL_INT ||
	     format->kind == GLES_TEXEL_UINT))
		integer = 1;

	/* A renderbuffer's limit: every attachment's, or of integers where the context multisamples them. */
	if (target == GL_RENDERBUFFER) {
		mask = framebuffer_samples_mask(state);
		if (integer) {
			mask = 0U;
			if (framebuffer_integer_multisampled(state))
				mask = framebuffer_integer_mask(state);
		}

		/* Its Vulkan format and usage, as its image is made. */
		memset(&probe, 0, sizeof(probe));
		status = renderbuffer_format(state, internalformat, &probe);
		if (status != 0)
			return 0U;
		vk = probe.vk;
		usage = renderbuffer_usage(state, &probe);
	} else {
		/* A texture's: the device's of sampled colours, depth or integers. */
		mask = framebuffer_color_texture_mask(state);
		if (format->kind == GLES_TEXEL_DEPTH)
			mask = framebuffer_depth_texture_mask(state);
		if (integer)
			mask = framebuffer_integer_mask(state);

		/* Its Vulkan format and usage, as texture syncing makes its image. */
		vk = format->vk;
		usage = framebuffer_texture_usage(state, format);
	}
	if (mask == 0U)
		return 0U;

	/* The counts the device makes such an image with. */
	memset(&properties, 0, sizeof(properties));
	result = vkGetPhysicalDeviceImageFormatProperties(state->display->physical, vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
							  usage, 0U, &properties);
	if (result != VK_SUCCESS)
		return 0U;

	/* Succeeded: the counts of both. */
	return mask & (uint32_t)properties.sampleCounts;
}

/*
 * Returns the most samples per pixel an internal format may have as a
 * renderbuffer or a multisample texture (the largest count
 * glGetInternalformativ reports, 1 when it reports none).
 */
uint32_t
gles_format_samples_max(
	struct gles_state *state,
	GLenum target,
	GLenum internalformat)
{
	uint32_t mask;
	uint32_t count;

	/* The largest of the format's counts. */
	mask = gles_format_samples(state, target, internalformat);
	count = framebuffer_samples_most(mask);

	/* Succeeded: the count. */
	return count;
}

/*
 * Returns the fewest samples per pixel an internal format may have as a
 * renderbuffer or a multisample texture of at least a count asked for (1
 * when it has none that many).
 */
uint32_t
gles_format_samples_for(
	struct gles_state *state,
	GLenum target,
	GLenum internalformat,
	uint32_t samples)
{
	uint32_t mask;
	uint32_t count;

	/* The fewest of the format's counts. */
	mask = gles_format_samples(state, target, internalformat);
	count = framebuffer_samples_for(mask, samples);

	/* Succeeded: the count. */
	return count;
}

/*
 * Returns the samples per pixel of a framebuffer (GL_SAMPLES): 0 for the
 * surfaces' and for an object with one sample per pixel or that is not
 * complete.
 */
uint32_t
gles_framebuffer_samples(
	struct gles_state *state,
	GLuint name)
{
	struct framebuffer_images images;
	struct gles_framebuffer *fbo;
	GLenum status;

	/* The surfaces' framebuffer is not multisampled. */
	fbo = framebuffer_named(state, name);
	if (fbo == NULL)
		return 0U;

	/* A complete object's images decide. */
	status = framebuffer_status(state, fbo, &images);
	if (status != GL_FRAMEBUFFER_COMPLETE || images.samples <= 1U)
		return 0U;

	/* Succeeded: its samples. */
	return images.samples;
}

/*
 * Reports what an internal format offers as a renderbuffer's storage or a
 * multisample texture's (OpenGL ES 3.1 and desktop GL): the sample counts
 * it may have above one (GL_NUM_SAMPLE_COUNTS, and GL_SAMPLES largest
 * first), those of the target's limit the device makes an image of the
 * format with (gles_format_samples).
 */
GL_APICALL void GL_APIENTRY
glGetInternalformativ(
	GLenum target,
	GLenum internalformat,
	GLenum pname,
	GLsizei bufSize,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_state *state;
	const struct gles_format *format;
	GLint counts[3];
	GLint number;
	GLsizei index;
	uint32_t mask;
	uint32_t count;
	int textures;

	/* A context with its state, and a target of the context's: renderbuffers, and multisample textures where it has them. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	textures = framebuffer_multisample_textures(state);
	if (target != GL_RENDERBUFFER &&
	    (target != GL_TEXTURE_2D_MULTISAMPLE ||
	     !textures)) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Room for the answer. */
	if (bufSize < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A format a framebuffer draws into. */
	format = gles_format_renderable(state, internalformat);
	if (format == NULL && internalformat != GL_STENCIL_INDEX8) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The counts above one, largest first. */
	mask = gles_format_samples(state, target, internalformat);
	number = 0;
	for (count = 8U; count >= 2U; count /= 2U) {
		if ((mask & count) == 0U)
			continue;
		counts[number] = (GLint)count;
		number++;
	}

	/* The parameter. */
	switch (pname) {
	case GL_NUM_SAMPLE_COUNTS:
		if (bufSize > 0)
			params[0] = number;
		break;
	case GL_SAMPLES:
		for (index = 0; index < bufSize && index < number; index++)
			params[index] = counts[index];
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		break;
	}
}

/*
 * Copies a rectangle of the read framebuffer's read buffer, and of its
 * depth and stencil buffer, into a rectangle of the draw framebuffer's
 * draw buffers and depth and stencil buffer, scaled with a filter (and
 * mirrored when a rectangle's corners are swapped), inside the scissor
 * box; a multisampled read framebuffer is resolved.
 */
GL_APICALL void GL_APIENTRY
glBlitFramebuffer(
	GLint srcX0,
	GLint srcY0,
	GLint srcX1,
	GLint srcY1,
	GLint dstX0,
	GLint dstY0,
	GLint dstX1,
	GLint dstY1,
	GLbitfield mask,
	GLenum filter)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct framebuffer_side read_color;
	struct framebuffer_side read_depth;
	struct framebuffer_side draw_colors[GLES_DRAW_BUFFERS];
	struct framebuffer_side draw_depth;
	GLint rectangles[8];
	VkImageAspectFlags depth_aspects;
	unsigned count;
	unsigned index;
	int integer;
	int draw_integer;
	int multisampled;
	int status;

	/* A context with its state, a mask of the three buffers, and a filter. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if ((mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0U) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A filter there is. */
	if (filter != GL_NEAREST && filter != GL_LINEAR) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Depth and stencil are copied texel by texel. */
	if (filter == GL_LINEAR && (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0U) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* What each framebuffer has: the read buffer and the draw buffers, the depth and stencil buffers (the frame begun, no pass open). */
	status = framebuffer_sides(context, state, &read_color, &read_depth, draw_colors, &count, &draw_depth);
	if (status != 0)
		return;

	/* A multisampled framebuffer is only read, into a rectangle of its size and a format of its own. */
	if (draw_depth.samples > 1U || (count > 0U && draw_colors[0].samples > 1U)) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/*
	 * A multisampled read framebuffer is resolved into a rectangle of the
	 * same size, and in OpenGL ES of the same corners too.
	 */
	multisampled = 0;
	if (read_color.samples > 1U || read_depth.samples > 1U)
		multisampled = 1;
	if (multisampled &&
	    (srcX1 - srcX0 != dstX1 - dstX0 ||
	     srcY1 - srcY0 != dstY1 - dstY0)) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* OpenGL ES's corners. */
	if (multisampled &&
	    gles_fixed == NULL &&
	    (srcX0 != dstX0 ||
	     srcY0 != dstY0)) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* And into buffers of its formats. */
	if (read_color.image != VK_NULL_HANDLE && read_color.samples > 1U) {
		/* Each draw buffer of the read buffer's format. */
		for (index = 0U; index < count; index++) {
			if (draw_colors[index].vk != read_color.vk) {
				gles_error(context, GL_INVALID_OPERATION);
				return;
			}
		}
	}

	/* Integers only into integers, and never filtered. */
	if ((mask & GL_COLOR_BUFFER_BIT) != 0U && read_color.image != VK_NULL_HANDLE) {
		integer = framebuffer_side_integer(&read_color);
		for (index = 0U; index < count; index++) {
			draw_integer = framebuffer_side_integer(&draw_colors[index]);
			if (draw_integer != integer) {
				gles_error(context, GL_INVALID_OPERATION);
				return;
			}
		}

		/* Integers are not filtered. */
		if (integer && filter == GL_LINEAR) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}
	}

	/* Depth and stencil between buffers of one format. */
	depth_aspects = 0U;
	if ((mask & GL_DEPTH_BUFFER_BIT) != 0U)
		depth_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
	if ((mask & GL_STENCIL_BUFFER_BIT) != 0U)
		depth_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
	depth_aspects &= read_depth.aspects & draw_depth.aspects;
	if (depth_aspects != 0U && read_depth.vk != draw_depth.vk) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/*
	 * Copied out of and into images whose format lets them be (the device
	 * copies no depth of some formats; a multisampled one is copied out
	 * of the image it is resolved into, framebuffer_resolve_depth).
	 */
	if (depth_aspects != 0U &&
	    ((read_depth.samples <= 1U && (read_depth.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0U) ||
	     (draw_depth.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0U)) {
		gles_report("a blit of a depth buffer whose format the device does not copy", -1);
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/*
	 * A multisampled depth and stencil buffer resolved first (the first
	 * sample of each pixel: OpenGL ES's stencil takes one sample's value,
	 * and its depth one between the pixel's least and greatest), before
	 * anything is copied, so that a device that cannot resolve it leaves
	 * the draw framebuffer as it was.
	 */
	if (depth_aspects != 0U && read_depth.samples > 1U) {
		status = framebuffer_resolve_depth(context, state, &read_depth, depth_aspects);
		if (status != 0)
			return;
	}

	/* The rectangles, GL's corners of the read one then the draw one. */
	rectangles[0] = srcX0;
	rectangles[1] = srcY0;
	rectangles[2] = srcX1;
	rectangles[3] = srcY1;
	rectangles[4] = dstX0;
	rectangles[5] = dstY0;
	rectangles[6] = dstX1;
	rectangles[7] = dstY1;

	/* The colour into each draw buffer (a multisampled read buffer resolved first). */
	if ((mask & GL_COLOR_BUFFER_BIT) != 0U && read_color.image != VK_NULL_HANDLE) {
		if (read_color.samples > 1U) {
			status = framebuffer_resolve(context, state, &read_color);
			if (status != 0)
				return;
		}

		/* Each draw buffer. */
		for (index = 0U; index < count; index++) {
			framebuffer_blit(state, &read_color, &draw_colors[index], rectangles, VK_IMAGE_ASPECT_COLOR_BIT, filter);
		}
	}

	/* The depth and stencil. */
	if (depth_aspects != 0U)
		framebuffer_blit(state, &read_depth, &draw_depth, rectangles, depth_aspects, GL_NEAREST);

	/* Succeeded: the frame has the copies. */
	context->draw->recorded = 1;
}

/* Returns the framebuffer object of a name, or NULL for name 0 or a name that has none. */
static struct gles_framebuffer *
framebuffer_named(
	struct gles_state *state,
	GLuint name)
{
	struct gles_framebuffer *fbo;

	/* Name 0 is the surfaces'. */
	if (name == 0U)
		return NULL;

	/* The object of the name. */
	fbo = gles_names_get(&state->framebuffers, name);
	if (fbo == NULL)
		return NULL;

	/* Succeeded: the object. */
	return fbo;
}

/* Reads a framebuffer target: *read is 1 for GL_READ_FRAMEBUFFER, 0 for GL_DRAW_FRAMEBUFFER, 2 for GL_FRAMEBUFFER (both); nonzero with the error recorded for any other. */
static int
framebuffer_target(
	struct zegl_context *context,
	GLenum target,
	int *read)
{
	/* The three targets. */
	switch (target) {
	case GL_READ_FRAMEBUFFER:
		*read = 1;
		return 0;
	case GL_DRAW_FRAMEBUFFER:
		*read = 0;
		return 0;
	case GL_FRAMEBUFFER:
		*read = 2;
		return 0;
	default:
		break;
	}

	/* Not a framebuffer target. */
	*read = 0;
	gles_error(context, GL_INVALID_ENUM);
	return -1;
}

/*
 * Returns the framebuffer object bound to a target (GL_FRAMEBUFFER is the
 * draw one) and its name in *name; NULL for the surfaces' framebuffer
 * (name 0).  With bound nonzero the call needs an object: framebuffer 0
 * records GL_INVALID_OPERATION.  A wrong target records GL_INVALID_ENUM
 * and sets *name to (GLuint)-1.
 */
static struct gles_framebuffer *
framebuffer_for(
	struct zegl_context *context,
	struct gles_state *state,
	GLenum target,
	int bound,
	GLuint *name)
{
	struct gles_framebuffer *fbo;
	int read;
	int status;

	/* The target. */
	status = framebuffer_target(context, target, &read);
	if (status != 0) {
		*name = (GLuint)-1;
		return NULL;
	}

	/* The name bound to it and its object. */
	*name = state->framebuffer;
	if (read == 1)
		*name = state->read_framebuffer;
	fbo = framebuffer_named(state, *name);
	if (fbo == NULL && bound) {
		gles_error(context, GL_INVALID_OPERATION);
		*name = (GLuint)-1;
		return NULL;
	}

	/* Succeeded: the object, or NULL for the surfaces'. */
	return fbo;
}

/* Makes a framebuffer object of a name with nothing attached, drawing and reading its first colour attachment; NULL without memory. */
static struct gles_framebuffer *
framebuffer_new(
	GLuint name)
{
	struct gles_framebuffer *fbo;
	unsigned index;

	/* The object. */
	fbo = calloc(1U, sizeof(*fbo));
	if (fbo == NULL)
		return NULL;

	/* GL's initial draw and read buffers. */
	fbo->name = name;
	for (index = 0U; index < GLES_DRAW_BUFFERS; index++)
		fbo->draw_buffers[index] = GL_NONE;
	fbo->draw_buffers[0] = GL_COLOR_ATTACHMENT0;
	fbo->read_buffer = GL_COLOR_ATTACHMENT0;

	/* Succeeded: the object. */
	return fbo;
}

/*
 * Finds what an attachment point names, for a colour point or (depth
 * nonzero) the depth or stencil point: the texture's level and layer or
 * the renderbuffer, which must have storage of a format a framebuffer
 * draws into of the point's kind.  Returns GL_FRAMEBUFFER_COMPLETE (with
 * image->format NULL when nothing is attached), or why not.
 */
static GLenum
framebuffer_image_of(
	struct gles_state *state,
	const struct gles_attachment *point,
	int depth,
	struct framebuffer_image *image)
{
	const struct gles_level *level;
	uint32_t features;
	uint32_t wanted;
	GLenum status;
	int is_depth;

	/* Nothing found yet. */
	memset(image, 0, sizeof(*image));
	if (point->kind == GLES_ATTACH_NONE)
		return GL_FRAMEBUFFER_COMPLETE;

	/* A renderbuffer with storage of the point's kind. */
	if (point->kind == GLES_ATTACH_RENDERBUFFER) {
		image->renderbuffer = gles_names_get(&state->renderbuffers, point->name);
		if (image->renderbuffer == NULL)
			return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;

		/* Storage of a size but no image: one the device does not make. */
		if (image->renderbuffer->image == VK_NULL_HANDLE &&
		    image->renderbuffer->width > 0 &&
		    image->renderbuffer->height > 0)
			return GL_FRAMEBUFFER_UNSUPPORTED;
		if (image->renderbuffer->image == VK_NULL_HANDLE)
			return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
		if ((image->renderbuffer->depth != 0) != (depth != 0))
			return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;

		/* Succeeded: the renderbuffer, resting as an attachment. */
		image->samples = image->renderbuffer->samples;
		image->format = image->renderbuffer->kept;
		image->vk = image->renderbuffer->vk;
		image->width = image->renderbuffer->width;
		image->height = image->renderbuffer->height;
		image->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		if (depth)
			image->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		return GL_FRAMEBUFFER_COMPLETE;
	}

	/* A texture. */
	image->texture = gles_names_get(&state->textures, point->name);
	if (image->texture == NULL)
		return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;

	/* The level is specified, with the layer or the slice (a layered one: every face of a cube map's level). */
	level = &image->texture->levels[point->face * GLES_LEVELS + (unsigned)point->level];
	if (level->width <= 0 || level->height <= 0 || level->format == NULL)
		return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
	if ((uint32_t)point->layer == GLES_LAYER_ALL) {
		status = framebuffer_layered_of(image->texture, point, image);
		if (status != GL_FRAMEBUFFER_COMPLETE)
			return status;
	} else if ((image->texture->target == GL_TEXTURE_2D_ARRAY || image->texture->target == GL_TEXTURE_CUBE_MAP_ARRAY) &&
		   point->layer >= level->depth) {
		return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
	} else if (image->texture->target == GL_TEXTURE_3D && point->layer >= level->depth) {
		return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
	}

	/* A format a framebuffer draws into, of the point's kind. */
	is_depth = 0;
	if (level->format->kind == GLES_TEXEL_DEPTH)
		is_depth = 1;
	if (!level->format->renderable || is_depth != (depth != 0))
		return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;

	/* The image starts at the base level. */
	if (point->level < image->texture->base_level)
		return GL_FRAMEBUFFER_UNSUPPORTED;

	/* A format the device draws into. */
	wanted = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
	if (depth)
		wanted = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
	features = gles_image_features(state, level->format->vk);
	if ((features & wanted) == 0U)
		return GL_FRAMEBUFFER_UNSUPPORTED;

	/* Succeeded: the level and layer (a cube map's face is its layer), resting to be sampled (a 3D slice's image as an attachment; a multisample texture's samples). */
	image->samples = 1U;
	if (image->texture->samples > 1U)
		image->samples = image->texture->samples;
	image->format = level->format;
	image->vk = level->format->vk;
	image->face = point->face;
	image->gl_level = point->level;
	image->level = (uint32_t)(point->level - image->texture->base_level);
	image->layer = point->face;
	if (image->texture->target == GL_TEXTURE_2D_ARRAY || image->texture->target == GL_TEXTURE_CUBE_MAP_ARRAY)
		image->layer = (uint32_t)point->layer;
	if (image->layered)
		image->layer = 0U;
	image->width = level->width;
	image->height = level->height;
	image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (image->texture->target == GL_TEXTURE_3D && !image->layered) {
		image->volume = 1;
		image->layer = (uint32_t)point->layer;
		image->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	/* Succeeded: the image found. */
	return GL_FRAMEBUFFER_COMPLETE;
}

/*
 * Reports whether a framebuffer object can be drawn into, finding what
 * its attachment points name: the colour images and the one depth and
 * stencil image.  Returns GL_FRAMEBUFFER_COMPLETE or why not.
 */
static GLenum
framebuffer_status(
	struct gles_state *state,
	struct gles_framebuffer *fbo,
	struct framebuffer_images *images)
{
	struct framebuffer_image stencil;
	struct framebuffer_image *image;
	uint32_t samples;
	unsigned index;
	int attached;
	GLenum status;

	/* Nothing found yet. */
	memset(images, 0, sizeof(*images));
	attached = 0;

	/* Each colour attachment. */
	for (index = 0U; index < GLES_COLOR_ATTACHMENTS; index++) {
		status = framebuffer_image_of(state, &fbo->colors[index], 0, &images->colors[index]);
		if (status != GL_FRAMEBUFFER_COMPLETE)
			return status;

		/* Attached: the pass has it and every one before. */
		if (images->colors[index].format != NULL) {
			images->color_count = index + 1U;
			attached = 1;
		}
	}

	/* The depth and the stencil attachments. */
	status = framebuffer_image_of(state, &fbo->depth, 1, &images->depth);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		return status;
	status = framebuffer_image_of(state, &fbo->stencil, 1, &stencil);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		return status;

	/* A depth attachment's format has depth, a stencil attachment's stencil. */
	if (images->depth.format != NULL && images->depth.renderbuffer != NULL &&
	    (images->depth.renderbuffer->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) == 0U)
		return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
	if (stencil.format != NULL && stencil.renderbuffer != NULL &&
	    (stencil.renderbuffer->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) == 0U)
		return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
	if (stencil.format != NULL && stencil.texture != NULL && !stencil.format->stencil)
		return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;

	/* Separate depth and stencil images cannot be one Vulkan attachment. */
	if (images->depth.format != NULL && stencil.format != NULL) {
		if (fbo->depth.kind != fbo->stencil.kind ||
		    fbo->depth.name != fbo->stencil.name ||
		    fbo->depth.level != fbo->stencil.level ||
		    fbo->depth.layer != fbo->stencil.layer ||
		    fbo->depth.face != fbo->stencil.face)
			return GL_FRAMEBUFFER_UNSUPPORTED;
	}

	/* The one depth and stencil image. */
	if (images->depth.format == NULL)
		images->depth = stencil;
	if (images->depth.format != NULL) {
		images->has_depth = 1;
		attached = 1;
	}

	/* Nothing attached at all: complete with the object's default size (OpenGL ES 3.1), one layer and its default samples. */
	if (!attached) {
		if (fbo->default_width == 0 || fbo->default_height == 0)
			return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
		images->layers = 1U;
		images->samples = framebuffer_default_samples(state, (uint32_t)fbo->default_samples);
		return GL_FRAMEBUFFER_COMPLETE;
	}

	/* Every attachment layered, or none; the framebuffer has the fewest layers. */
	status = framebuffer_layers(images);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		return status;

	/* Every image has as many samples per pixel. */
	samples = 0U;
	for (index = 0U; index <= GLES_COLOR_ATTACHMENTS; index++) {
		image = &images->depth;
		if (index < GLES_COLOR_ATTACHMENTS)
			image = &images->colors[index];
		if (image->format == NULL)
			continue;

		/* The first one's count, which the others must have. */
		if (samples == 0U)
			samples = image->samples;
		if (image->samples != samples)
			return GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;
	}

	/* The count, for the pass and the pipelines. */
	images->samples = samples;

	/* Succeeded: it can be drawn into. */
	return GL_FRAMEBUFFER_COMPLETE;
}

/*
 * Makes a complete framebuffer object's render pass and framebuffer for
 * the views its attachments have now, unless they were made for them,
 * and finds the compatible pass pipelines are made with.  Returns
 * GL_FRAMEBUFFER_COMPLETE, why it is not, or GL_FRAMEBUFFER_UNSUPPORTED
 * when the device refused.
 */
static GLenum
framebuffer_build(
	struct gles_state *state,
	struct gles_framebuffer *fbo,
	struct framebuffer_images *images)
{
	struct framebuffer_image *image;
	struct gles_pass_format format;
	VkFramebufferCreateInfo create;
	VkImageView views[GLES_COLOR_ATTACHMENTS + 1U];
	VkImageView color_views[GLES_COLOR_ATTACHMENTS];
	VkImageView depth_view;
	VkImageLayout color_layouts[GLES_COLOR_ATTACHMENTS];
	VkImageLayout depth_layout;
	VkExtent2D extent;
	VkRenderPass compatible;
	uint32_t count;
	unsigned index;
	unsigned attachment;
	GLenum status;
	VkResult result;
	int synced;
	int differs;

	/* Complete. */
	status = framebuffer_status(state, fbo, images);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		return status;

	/* Each attachment's view (a texture's image made or brought up to date from its levels first), format and size. */
	memset(&format, 0, sizeof(format));
	memset(color_views, 0, sizeof(color_views));
	memset(color_layouts, 0, sizeof(color_layouts));
	depth_view = VK_NULL_HANDLE;
	depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	extent.width = UINT32_MAX;
	extent.height = UINT32_MAX;
	format.color_count = 0U;
	format.samples = images->samples;
	for (index = 0U; index <= GLES_COLOR_ATTACHMENTS; index++) {
		/* The depth attachment, or the attachment in colour slot index (none: the slot is empty). */
		image = &images->depth;
		attachment = GLES_COLOR_ATTACHMENTS;
		if (index < GLES_COLOR_ATTACHMENTS) {
			attachment = framebuffer_slot_attachment(fbo, index);
			if (attachment == GLES_COLOR_ATTACHMENTS)
				continue;
			image = &images->colors[attachment];
		}

		/* Nothing attached there. */
		if (image->format == NULL)
			continue;

		/* A texture's image, with the level. */
		if (image->texture != NULL) {
			synced = gles_texture_sync(state, image->texture);
			if (synced != 0)
				return GL_FRAMEBUFFER_UNSUPPORTED;
		}

		/*
		 * The view of the level and layer (a layered attachment's of every
		 * layer: Vulkan's framebuffer of several layers needs views of as
		 * many), the image a 3D slice is drawn through, or the
		 * renderbuffer's.
		 */
		if (image->volume && index < GLES_COLOR_ATTACHMENTS) {
			views[0] = framebuffer_slice(state, &fbo->slices[attachment], image);
		} else if (image->texture != NULL && image->layered) {
			views[0] = gles_texture_attach_view(state, image->texture, image->level, GLES_LAYER_ALL);
		} else if (image->texture != NULL) {
			views[0] = gles_texture_attach_view(state, image->texture, image->level, image->layer);
		} else {
			views[0] = image->renderbuffer->view;
		}

		/* A view the device made. */
		if (views[0] == VK_NULL_HANDLE)
			return GL_FRAMEBUFFER_UNSUPPORTED;

		/* The colour or the depth attachment, its format and resting layout. */
		if (index < GLES_COLOR_ATTACHMENTS) {
			color_views[index] = views[0];
			color_layouts[index] = image->layout;
			format.colors[index] = (uint32_t)image->vk;
			format.color_count = index + 1U;
		} else {
			depth_view = views[0];
			depth_layout = image->layout;
			format.depth = (uint32_t)image->vk;
		}

		/* The framebuffer is as large as the smallest attachment. */
		if ((uint32_t)image->width < extent.width)
			extent.width = (uint32_t)image->width;
		if ((uint32_t)image->height < extent.height)
			extent.height = (uint32_t)image->height;
	}

	/* Nothing attached: the object's default size. */
	if (extent.width == UINT32_MAX) {
		extent.width = (uint32_t)fbo->default_width;
		extent.height = (uint32_t)fbo->default_height;
	}

	/* The pass pipelines are made with. */
	compatible = framebuffer_compatible(state, &format);
	if (compatible == VK_NULL_HANDLE)
		return GL_FRAMEBUFFER_UNSUPPORTED;

	/* Made for these views, this size and these samples already. */
	differs = 1;
	if (fbo->framebuffer != VK_NULL_HANDLE && fbo->built_depth == depth_view)
		differs = memcmp(fbo->built_colors, color_views, sizeof(color_views));
	if (differs == 0 &&
	    extent.width == fbo->extent.width &&
	    extent.height == fbo->extent.height &&
	    format.samples == fbo->format.samples)
		return GL_FRAMEBUFFER_COMPLETE;

	/* The old ones go (an open pass over them ends first); they wait for the frame. */
	if (state->open_fbo == fbo)
		gles_target_close(state);
	framebuffer_forget_views(state, fbo);

	/* The render pass, which loads and keeps what is there. */
	result = framebuffer_pass(state, &format, color_layouts, depth_layout, &fbo->pass);
	if (result != VK_SUCCESS) {
		gles_report("vkCreateRenderPass", (int)result);
		return GL_FRAMEBUFFER_UNSUPPORTED;
	}

	/* The framebuffer over the views, the colour ones in order, then the depth one. */
	count = 0U;
	for (index = 0U; index < GLES_COLOR_ATTACHMENTS; index++) {
		if (color_views[index] != VK_NULL_HANDLE)
			views[count++] = color_views[index];
	}

	/* The depth view last. */
	if (depth_view != VK_NULL_HANDLE)
		views[count++] = depth_view;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	create.renderPass = fbo->pass;
	create.attachmentCount = count;
	create.pAttachments = views;
	create.width = extent.width;
	create.height = extent.height;
	create.layers = images->layers;
	result = vkCreateFramebuffer(state->device, &create, NULL, &fbo->framebuffer);
	if (result != VK_SUCCESS) {
		gles_report("vkCreateFramebuffer", (int)result);
		framebuffer_forget_views(state, fbo);
		return GL_FRAMEBUFFER_UNSUPPORTED;
	}

	/* Made for these views, formats, size and layers. */
	memcpy(fbo->built_colors, color_views, sizeof(color_views));
	fbo->built_depth = depth_view;
	fbo->extent = extent;
	fbo->layers = images->layers;
	fbo->format = format;
	fbo->compatible = compatible;
	for (index = 0U; index < GLES_COLOR_ATTACHMENTS; index++) {
		fbo->color_formats[index] = NULL;
		attachment = framebuffer_slot_attachment(fbo, index);
		if (attachment < GLES_COLOR_ATTACHMENTS)
			fbo->color_formats[index] = images->colors[attachment].format;
	}

	/* The depth image's aspects: depth, and stencil when its format has it. */
	fbo->depth_aspects = 0U;
	if (depth_view != VK_NULL_HANDLE) {
		fbo->depth_aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (images->depth.renderbuffer != NULL)
			fbo->depth_aspects = images->depth.renderbuffer->aspects;
		if (images->depth.texture != NULL && images->depth.format->stencil)
			fbo->depth_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}

	/* Succeeded: the pass and framebuffer are made. */
	return GL_FRAMEBUFFER_COMPLETE;
}

/*
 * Finds a layered attachment's layers: a 2D array texture's level's
 * layers, or a cube map's six faces (each of the level specified at the
 * same size).  Returns GL_FRAMEBUFFER_COMPLETE, or why not.
 */
static GLenum
framebuffer_layered_of(
	struct gles_texture *texture,
	const struct gles_attachment *point,
	struct framebuffer_image *image)
{
	const struct gles_level *level;
	const struct gles_level *face_level;
	unsigned face;

	/* A 2D array texture: its level's layers (a cube map array's layer-faces). */
	level = &texture->levels[(unsigned)point->level];
	image->layered = 1;
	if (texture->target == GL_TEXTURE_2D_ARRAY || texture->target == GL_TEXTURE_CUBE_MAP_ARRAY) {
		image->layers = (uint32_t)level->depth;
		return GL_FRAMEBUFFER_COMPLETE;
	}

	/* A cube map: every face of the level, alike. */
	for (face = 1U; face < GLES_FACES; face++) {
		face_level = &texture->levels[face * GLES_LEVELS + (unsigned)point->level];
		if (face_level->width != level->width ||
		    face_level->height != level->height ||
		    face_level->format != level->format)
			return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
	}

	/* Succeeded: six layers. */
	image->layers = GLES_FACES;
	return GL_FRAMEBUFFER_COMPLETE;
}

/*
 * Finds the layers of a framebuffer object's framebuffer: 1 when no
 * attachment is layered, the fewest of theirs when every one is.  Returns
 * GL_FRAMEBUFFER_COMPLETE, or GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS when
 * some are layered and some not.
 */
static GLenum
framebuffer_layers(
	struct framebuffer_images *images)
{
	struct framebuffer_image *image;
	unsigned index;
	unsigned layered;
	unsigned flat;

	/* Each attachment, layered or not. */
	images->layers = 0U;
	layered = 0U;
	flat = 0U;
	for (index = 0U; index <= GLES_COLOR_ATTACHMENTS; index++) {
		image = &images->depth;
		if (index < GLES_COLOR_ATTACHMENTS)
			image = &images->colors[index];
		if (image->format == NULL)
			continue;
		if (!image->layered) {
			flat++;
			continue;
		}

		/* A layered one: the fewest layers. */
		layered++;
		if (images->layers == 0U || image->layers < images->layers)
			images->layers = image->layers;
	}

	/* Both kinds together. */
	if (layered != 0U && flat != 0U)
		return GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS;

	/* Succeeded: one layer when none is layered. */
	if (layered == 0U)
		images->layers = 1U;
	return GL_FRAMEBUFFER_COMPLETE;
}

/*
 * Returns the colour attachment in a slot of a framebuffer object's pass:
 * the one the slot's draw buffer names, or with none named, the slot's
 * own attachment unless another draw buffer names it (an attachment is
 * in one slot only); GLES_COLOR_ATTACHMENTS for an empty slot.
 */
static unsigned
framebuffer_slot_attachment(
	const struct gles_framebuffer *fbo,
	unsigned slot)
{
	unsigned other;

	/* The attachment the draw buffer names. */
	if (fbo->draw_buffers[slot] != GL_NONE)
		return fbo->draw_buffers[slot] - GL_COLOR_ATTACHMENT0;

	/* The slot's own, unless a draw buffer puts it in another slot. */
	for (other = 0U; other < GLES_DRAW_BUFFERS; other++) {
		if (fbo->draw_buffers[other] == GL_COLOR_ATTACHMENT0 + slot)
			return GLES_COLOR_ATTACHMENTS;
	}

	/* Succeeded: its own attachment, unwritten. */
	return slot;
}

/* Marks what a framebuffer object's pass draws into as used by this frame, and the textures' levels as newer on the device. */
static void
framebuffer_mark(
	struct gles_state *state,
	struct framebuffer_images *images)
{
	struct framebuffer_image *image;
	unsigned index;
	unsigned face;

	/* Each attachment, the colour ones and the depth one. */
	for (index = 0U; index <= GLES_COLOR_ATTACHMENTS; index++) {
		image = &images->depth;
		if (index < GLES_COLOR_ATTACHMENTS)
			image = &images->colors[index];
		if (image->format == NULL)
			continue;

		/* A texture's level on the CPU is older than the image from now on (a 3D slice's once it is copied back; every face of a layered cube map's). */
		if (image->texture != NULL) {
			image->texture->used = state->frame;
			image->texture->gpu_levels[image->face] |= 1U << (unsigned)image->gl_level;
			for (face = 0U; image->layered && image->texture->target == GL_TEXTURE_CUBE_MAP && face < GLES_FACES; face++)
				image->texture->gpu_levels[face] |= 1U << (unsigned)image->gl_level;
		}

		/* A renderbuffer waits for the frame before it may go. */
		if (image->renderbuffer != NULL)
			image->renderbuffer->used = state->frame;
	}
}

/* Ends the surface's own render pass, if one is open, an occlusion query's segment in it first. */
static void
framebuffer_leave(
	struct gles_state *state,
	struct zegl_surface *surface)
{
	/* The segment, then the pass. */
	gles_queries_suspend(state);
	zegl_frame_leave_pass(surface);
}

/* Lets a framebuffer object's render pass and framebuffer go (they wait for the frame), and forgets what they were made for. */
static void
framebuffer_forget_views(
	struct gles_state *state,
	struct gles_framebuffer *fbo)
{
	struct gles_garbage objects;

	/* The pass and framebuffer wait for the frame. */
	memset(&objects, 0, sizeof(objects));
	objects.pass = fbo->pass;
	objects.framebuffer = fbo->framebuffer;
	gles_garbage_keep(state, &objects);

	/* Nothing is made for any views now. */
	fbo->pass = VK_NULL_HANDLE;
	fbo->framebuffer = VK_NULL_HANDLE;
	memset(fbo->built_colors, 0, sizeof(fbo->built_colors));
	fbo->built_depth = VK_NULL_HANDLE;
}

/*
 * Returns the view of the 2D image a framebuffer object draws a 3D
 * texture's slice through, making the image (resting as a colour
 * attachment) when the slice or its format or size changed;
 * VK_NULL_HANDLE when the device refused.
 */
static VkImageView
framebuffer_slice(
	struct gles_state *state,
	struct gles_slice_image *slice,
	const struct framebuffer_image *image)
{
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkImageMemoryBarrier barrier;
	uint32_t type;
	VkResult result;
	int status;

	/* The image made for this slice already. */
	if (slice->image != VK_NULL_HANDLE &&
	    slice->texture == image->texture->name &&
	    slice->level == image->gl_level &&
	    slice->slice == image->layer &&
	    slice->format == image->vk &&
	    slice->width == image->width &&
	    slice->height == image->height)
		return slice->view;

	/* The old one waits for the frame. */
	gles_throw_away(state, VK_NULL_HANDLE, slice->image, slice->view, slice->memory);
	memset(slice, 0, sizeof(*slice));

	/* A 2D image of the slice's format and size, drawn into and copied both ways. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = image->vk;
	create.extent.width = (uint32_t)image->width;
	create.extent.height = (uint32_t)image->height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	/* A slice image the device does not make leaves the framebuffer unsupported, reported once. */
	status = gles_image_supported(state, &create);
	if (!status) {
		gles_image_unmade(create.format, "a 3D texture's slice drawn into");
		return VK_NULL_HANDLE;
	}
	result = vkCreateImage(state->device, &create, NULL, &slice->image);
	if (result != VK_SUCCESS) {
		slice->image = VK_NULL_HANDLE;
		return VK_NULL_HANDLE;
	}

	/* Its memory on the device, bound. */
	vkGetImageMemoryRequirements(state->device, slice->image, &requirements);
	type = gles_memory_type(state, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX)
		type = gles_memory_type(state, requirements.memoryTypeBits, 0U);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	result = vkAllocateMemory(state->device, &allocate, NULL, &slice->memory);
	if (result == VK_SUCCESS)
		result = vkBindImageMemory(state->device, slice->image, slice->memory, 0U);
	if (result != VK_SUCCESS) {
		gles_throw_away(state, VK_NULL_HANDLE, slice->image, VK_NULL_HANDLE, slice->memory);
		memset(slice, 0, sizeof(*slice));
		return VK_NULL_HANDLE;
	}

	/* A view of it. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = slice->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = image->vk;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	result = vkCreateImageView(state->device, &view, NULL, &slice->view);
	if (result != VK_SUCCESS) {
		gles_throw_away(state, VK_NULL_HANDLE, slice->image, VK_NULL_HANDLE, slice->memory);
		memset(slice, 0, sizeof(*slice));
		return VK_NULL_HANDLE;
	}

	/* Its resting layout, set by the upload queue before any pass uses it. */
	status = gles_upload_begin(state);
	if (status == 0) {
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = slice->image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1U;
		barrier.subresourceRange.layerCount = 1U;
		vkCmdPipelineBarrier(state->upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
				     0U, 0U, NULL, 0U, NULL, 1U, &barrier);
		status = gles_upload_end(state);
	}

	/* Without its layout the image goes. */
	if (status != 0) {
		gles_throw_away(state, VK_NULL_HANDLE, slice->image, slice->view, slice->memory);
		memset(slice, 0, sizeof(*slice));
		return VK_NULL_HANDLE;
	}

	/* Succeeded: the image stands for the slice. */
	slice->texture = image->texture->name;
	slice->level = image->gl_level;
	slice->slice = image->layer;
	slice->format = image->vk;
	slice->width = image->width;
	slice->height = image->height;
	return slice->view;
}

/*
 * Copies the 3D slices a framebuffer object draws into its 2D images
 * (back zero: before its pass) or its 2D images back into the slices
 * (back nonzero: after its pass), through the stream (Vulkan 1.0 copies
 * no image of one type into one of another).
 */
static void
framebuffer_slices_copy(
	struct gles_state *state,
	struct gles_framebuffer *fbo,
	VkCommandBuffer command,
	int back)
{
	struct gles_slice_image *slice;
	struct gles_texture *texture;
	VkImageMemoryBarrier barriers[2];
	VkBufferImageCopy volume_copy;
	VkBufferImageCopy flat_copy;
	VkImageLayout volume_layouts[2];
	VkImageLayout flat_layouts[2];
	const struct gles_format *format;
	VkBuffer buffer;
	VkDeviceSize offset;
	void *room;
	unsigned index;
	GLint image_level;

	/* Each colour attachment drawn through a slice image. */
	for (index = 0U; index < GLES_COLOR_ATTACHMENTS; index++) {
		slice = &fbo->slices[index];
		if (slice->image == VK_NULL_HANDLE || slice->texture == 0U)
			continue;

		/* The 3D texture's image, which has the level. */
		texture = gles_names_get(&state->textures, slice->texture);
		if (texture == NULL || texture->image == VK_NULL_HANDLE || texture->target != GL_TEXTURE_3D)
			continue;
		image_level = slice->level - texture->base_level;
		format = texture->image_format;
		if (image_level < 0 || (uint32_t)image_level >= texture->level_count || format->vk != slice->format)
			continue;

		/* Room in the stream for the slice. */
		room = gles_stream(state, (size_t)slice->width * (size_t)slice->height * format->bytes, 16U, &buffer, &offset);
		if (room == NULL)
			continue;

		/* The copies: the slice of the 3D image, and the whole 2D image, through the same bytes. */
		memset(&volume_copy, 0, sizeof(volume_copy));
		volume_copy.bufferOffset = offset;
		volume_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		volume_copy.imageSubresource.mipLevel = (uint32_t)image_level;
		volume_copy.imageSubresource.layerCount = 1U;
		volume_copy.imageOffset.z = (int32_t)slice->slice;
		volume_copy.imageExtent.width = (uint32_t)slice->width;
		volume_copy.imageExtent.height = (uint32_t)slice->height;
		volume_copy.imageExtent.depth = 1U;
		flat_copy = volume_copy;
		flat_copy.imageSubresource.mipLevel = 0U;
		flat_copy.imageOffset.z = 0;

		/* The layouts: the 3D image from and back to being sampled, the 2D one from and back to being attached. */
		volume_layouts[0] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		flat_layouts[0] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		if (back) {
			volume_layouts[0] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			flat_layouts[0] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		}

		/* Both rest there again after the copy. */
		volume_layouts[1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		flat_layouts[1] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		/* Both ready to be copied. */
		memset(barriers, 0, sizeof(barriers));
		barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[0].oldLayout = volume_layouts[1];
		barriers[0].newLayout = volume_layouts[0];
		barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].image = texture->image;
		barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[0].subresourceRange.baseMipLevel = (uint32_t)image_level;
		barriers[0].subresourceRange.levelCount = 1U;
		barriers[0].subresourceRange.layerCount = 1U;
		barriers[1] = barriers[0];
		barriers[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barriers[1].oldLayout = flat_layouts[1];
		barriers[1].newLayout = flat_layouts[0];
		barriers[1].image = slice->image;
		barriers[1].subresourceRange.baseMipLevel = 0U;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				     0U, 0U, NULL, 0U, NULL, 2U, barriers);

		/* Into the stream and out of it, one way or the other. */
		if (back) {
			vkCmdCopyImageToBuffer(command, slice->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1U, &flat_copy);
		} else {
			vkCmdCopyImageToBuffer(command, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1U, &volume_copy);
		}

		/* The bytes written are read after. */
		framebuffer_buffer_barrier(command, buffer);
		if (back) {
			vkCmdCopyBufferToImage(command, buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &volume_copy);
		} else {
			vkCmdCopyBufferToImage(command, buffer, slice->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &flat_copy);
		}

		/* Both back where they rest. */
		barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
		barriers[0].oldLayout = volume_layouts[0];
		barriers[0].newLayout = volume_layouts[1];
		barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barriers[1].oldLayout = flat_layouts[0];
		barriers[1].newLayout = flat_layouts[1];
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				     0U, 0U, NULL, 0U, NULL, 2U, barriers);
		texture->used = state->frame;
	}
}

/* Makes the stream's bytes a copy wrote visible to the next copy that reads them. */
static void
framebuffer_buffer_barrier(
	VkCommandBuffer command,
	VkBuffer buffer)
{
	VkBufferMemoryBarrier barrier;

	/* The whole buffer, from the write to the read. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = buffer;
	barrier.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0U, 0U, NULL, 1U, &barrier, 0U, NULL);
}

/* Lets the 2D images a framebuffer object drew 3D slices through go (they wait for the frame). */
static void
framebuffer_slices_free(
	struct gles_state *state,
	struct gles_framebuffer *fbo)
{
	unsigned index;

	/* Each one. */
	for (index = 0U; index < GLES_COLOR_ATTACHMENTS; index++) {
		gles_throw_away(state, VK_NULL_HANDLE, fbo->slices[index].image, fbo->slices[index].view, fbo->slices[index].memory);
		memset(&fbo->slices[index], 0, sizeof(fbo->slices[index]));
	}
}

/*
 * Finds what a blit reads and writes: the read framebuffer's read buffer
 * and depth and stencil buffer, and the draw framebuffer's draw buffers
 * and depth and stencil buffer (an image of none is VK_NULL_HANDLE), in
 * the draw surface's frame with no pass open.  Returns 0, or -1 with the
 * error recorded.
 */
static int
framebuffer_sides(
	struct zegl_context *context,
	struct gles_state *state,
	struct framebuffer_side *read_color,
	struct framebuffer_side *read_depth,
	struct framebuffer_side *draw_colors,
	unsigned *count,
	struct framebuffer_side *draw_depth)
{
	struct framebuffer_images read_images;
	struct framebuffer_images draw_images;
	struct gles_framebuffer *read_fbo;
	struct gles_framebuffer *draw_fbo;
	struct zegl_surface *surface;
	unsigned index;
	unsigned attachment;
	GLenum status;
	EGLint error;

	/* Nothing found yet, and a draw surface whose frame records the copies. */
	memset(read_color, 0, sizeof(*read_color));
	memset(read_depth, 0, sizeof(*read_depth));
	memset(draw_colors, 0, GLES_DRAW_BUFFERS * sizeof(*draw_colors));
	memset(draw_depth, 0, sizeof(*draw_depth));
	*count = 0U;
	surface = context->draw;
	if (surface == NULL) {
		gles_error(context, GL_INVALID_FRAMEBUFFER_OPERATION);
		return -1;
	}

	/* Both framebuffer objects complete, with their images made. */
	read_fbo = framebuffer_named(state, state->read_framebuffer);
	draw_fbo = framebuffer_named(state, state->framebuffer);
	status = GL_FRAMEBUFFER_COMPLETE;
	if (read_fbo != NULL)
		status = framebuffer_build(state, read_fbo, &read_images);
	if (status == GL_FRAMEBUFFER_COMPLETE && draw_fbo != NULL)
		status = framebuffer_build(state, draw_fbo, &draw_images);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		gles_error(context, GL_INVALID_FRAMEBUFFER_OPERATION);
		return -1;
	}

	/* The surfaces' framebuffer is read from the draw surface only (its image is this frame's). */
	if (read_fbo == NULL && context->read != surface && state->default_read_buffer != GL_NONE) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* The frame, with the surface's image drawn by at least one pass, and no pass open. */
	error = zegl_frame_begin(surface);
	if (error != EGL_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* No pass open in the frame. */
	gles_target_close(state);
	if (read_fbo == NULL || draw_fbo == NULL)
		zegl_frame_pass(surface, NULL);
	framebuffer_leave(state, surface);

	/* The read side: the surface's image and depth buffer, or the object's read buffer and depth attachment. */
	if (read_fbo == NULL) {
		if (state->default_read_buffer == GL_BACK && surface->readable)
			framebuffer_side_surface(surface, 0, read_color);
		framebuffer_side_surface(surface, 1, read_depth);
	} else {
		index = GLES_COLOR_ATTACHMENTS;
		if (read_fbo->read_buffer != GL_NONE)
			index = read_fbo->read_buffer - GL_COLOR_ATTACHMENT0;
		if (index < GLES_COLOR_ATTACHMENTS && read_images.colors[index].format != NULL)
			framebuffer_side_image(&read_images.colors[index], read_fbo->extent, surface, read_color);
		if (read_images.has_depth)
			framebuffer_side_image(&read_images.depth, read_fbo->extent, surface, read_depth);
	}

	/* The draw side: the surface's image (when a blit may write it) and depth buffer. */
	if (draw_fbo == NULL) {
		if (state->default_draw_buffer == GL_BACK && !surface->writable) {
			gles_error(context, GL_INVALID_OPERATION);
			return -1;
		}

		/* The image, when a draw buffer names it. */
		if (state->default_draw_buffer == GL_BACK) {
			framebuffer_side_surface(surface, 0, &draw_colors[0]);
			*count = 1U;
		}

		/* The depth and stencil image. */
		framebuffer_side_surface(surface, 1, draw_depth);
		return 0;
	}

	/* Or the object's attachments its draw buffers name, and its depth attachment. */
	for (index = 0U; index < GLES_DRAW_BUFFERS; index++) {
		if (draw_fbo->draw_buffers[index] == GL_NONE)
			continue;
		attachment = draw_fbo->draw_buffers[index] - GL_COLOR_ATTACHMENT0;
		if (draw_images.colors[attachment].format == NULL)
			continue;
		framebuffer_side_image(&draw_images.colors[attachment], draw_fbo->extent, surface, &draw_colors[*count]);
		(*count)++;
	}

	/* The depth attachment. */
	if (draw_images.has_depth)
		framebuffer_side_image(&draw_images.depth, draw_fbo->extent, surface, draw_depth);

	/* Succeeded: both sides are found. */
	return 0;
}

/* Describes a surface's colour image (depth zero) or its depth and stencil image, when it has one, as a blit's side. */
static void
framebuffer_side_surface(
	struct zegl_surface *surface,
	int depth,
	struct framebuffer_side *side)
{
	/* The frame's command buffer, the size, rows from the top, one sample. */
	side->command = surface->command;
	side->extent = surface->extent;
	side->size = surface->extent;
	side->flip = 1;
	side->samples = 1U;

	/* The depth and stencil image, resting as an attachment (none: nothing). */
	if (depth) {
		if (surface->depth_format == VK_FORMAT_UNDEFINED)
			return;
		side->image = surface->depth_image;
		side->usage = surface->depth_usage;
		side->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		side->vk = surface->depth_format;
		side->aspects = surface->depth_aspects;
		return;
	}

	/* The colour image of this frame, RGBA8 (or BGRA8), copied as the surface allows. */
	side->image = surface->images[surface->image];
	side->usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	side->layout = surface->rest_layout;
	side->vk = surface->format;
	side->format = gles_format_rgba8();
	side->aspects = VK_IMAGE_ASPECT_COLOR_BIT;
}

/* Describes an attachment of a framebuffer object of a size as a blit's side: a renderbuffer's image, or a texture's level and layer or slice. */
static void
framebuffer_side_image(
	const struct framebuffer_image *image,
	VkExtent2D extent,
	struct zegl_surface *surface,
	struct framebuffer_side *side)
{
	/* The frame's command buffer, the attachment's format and size, GL's rows. */
	side->command = surface->command;
	side->extent = extent;
	side->size.width = (uint32_t)image->width;
	side->size.height = (uint32_t)image->height;
	side->format = image->format;
	side->vk = image->vk;
	side->samples = image->samples;

	/* A renderbuffer: its image, resting as an attachment. */
	if (image->renderbuffer != NULL) {
		side->image = image->renderbuffer->image;
		side->usage = image->renderbuffer->usage;
		side->layout = image->layout;
		side->aspects = image->renderbuffer->aspects;
		side->renderbuffer = image->renderbuffer;
		return;
	}

	/* A texture: the level and layer (a 3D texture's slice as a depth offset), resting to be sampled, copied either way. */
	side->image = image->texture->image;
	side->usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	side->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	side->level = image->level;
	side->layer = image->layer;
	if (image->volume) {
		side->layer = 0U;
		side->slice = (int32_t)image->layer;
	}

	/* Colour, or depth with the stencil its format has (a depth texture's image is sampled). */
	side->aspects = VK_IMAGE_ASPECT_COLOR_BIT;
	if (image->format->kind == GLES_TEXEL_DEPTH) {
		side->aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (image->format->stencil)
			side->aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
		side->usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}

	/* What it belongs to, marked after the copy. */
	side->texture = image->texture;
	side->face = image->face;
	side->gl_level = image->gl_level;
}

/* Reports whether a blit's side holds integers. */
static int
framebuffer_side_integer(
	const struct framebuffer_side *side)
{
	/* A format of signed or unsigned integers. */
	if (side->format == NULL)
		return 0;
	if (side->format->kind == GLES_TEXEL_INT)
		return 1;
	if (side->format->kind == GLES_TEXEL_UINT)
		return 1;

	/* Succeeded: normalized or float. */
	return 0;
}

/*
 * Resolves a multisampled side into a new image of one sample per pixel
 * (waiting in the garbage for the frame), which the side then names,
 * resting to be copied from.  Returns 0, or -1 with the error recorded.
 */
static int
framebuffer_resolve(
	struct zegl_context *context,
	struct gles_state *state,
	struct framebuffer_side *side)
{
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageMemoryBarrier barriers[2];
	VkImageResolve region;
	VkImage image;
	VkDeviceMemory memory;
	uint32_t type;
	VkResult result;
	int supported;

	/* An image of the side's format and size, one sample per pixel. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = side->vk;
	create.extent.width = side->extent.width;
	create.extent.height = side->extent.height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	/*
	 * A format the device makes no such image of cannot be resolved for
	 * the blit: the blit fails as one between formats that cannot be
	 * blitted (GL_INVALID_OPERATION), reported once.
	 */
	supported = gles_image_supported(state, &create);
	if (!supported) {
		gles_image_unmade(create.format, "a resolved blit");
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}
	result = vkCreateImage(state->device, &create, NULL, &image);
	if (result != VK_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* Its memory, bound; both wait for the frame from now on. */
	vkGetImageMemoryRequirements(state->device, image, &requirements);
	type = gles_memory_type(state, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX)
		type = gles_memory_type(state, requirements.memoryTypeBits, 0U);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	memory = VK_NULL_HANDLE;
	result = vkAllocateMemory(state->device, &allocate, NULL, &memory);
	if (result == VK_SUCCESS)
		result = vkBindImageMemory(state->device, image, memory, 0U);
	gles_throw_away(state, VK_NULL_HANDLE, image, VK_NULL_HANDLE, memory);
	if (result != VK_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* The multisampled image to be read, the new one to be written. */
	memset(barriers, 0, sizeof(barriers));
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].oldLayout = side->layout;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = side->image;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].subresourceRange.levelCount = 1U;
	barriers[0].subresourceRange.layerCount = 1U;
	barriers[1] = barriers[0];
	barriers[1].srcAccessMask = 0U;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].image = image;
	vkCmdPipelineBarrier(side->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0U, 0U, NULL, 0U, NULL, 2U, barriers);

	/* The whole image resolved. */
	memset(&region, 0, sizeof(region));
	region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.srcSubresource.layerCount = 1U;
	region.dstSubresource = region.srcSubresource;
	region.extent.width = side->extent.width;
	region.extent.height = side->extent.height;
	region.extent.depth = 1U;
	vkCmdResolveImage(side->command, side->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			  1U, &region);

	/* The multisampled image back where it rests, the new one to be copied from. */
	barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].newLayout = side->layout;
	barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(side->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			     0U, 0U, NULL, 0U, NULL, 2U, barriers);

	/* The renderbuffer waits for the frame. */
	if (side->renderbuffer != NULL)
		side->renderbuffer->used = state->frame;

	/* Succeeded: the side is the resolved image. */
	side->image = image;
	side->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	side->samples = 1U;
	side->level = 0U;
	side->layer = 0U;
	side->renderbuffer = NULL;
	side->texture = NULL;
	return 0;
}

/*
 * Resolves a multisampled depth and stencil side (its aspects asked for)
 * into a new image of one sample per pixel (waiting in the garbage for
 * the frame), which the side then names, resting to be copied from: a
 * render pass draws each pixel's first sample's depth, and its first
 * sample's stencil one bit at a time, read from the side's image as a
 * multisample texture.  Vulkan's vkCmdResolveImage resolves colour only,
 * and the depth and stencil resolve of a subpass is not in Vulkan 1.0
 * (VK_KHR_depth_stencil_resolve), so the draw is the way every device
 * that samples the image's format of its samples has.  On a device that
 * does not, the blit fails with GL_INVALID_OPERATION (as one of formats
 * the device does not copy), reported once.  Returns 0, or -1 with the
 * error recorded.
 */
static int
framebuffer_resolve_depth(
	struct zegl_context *context,
	struct gles_state *state,
	struct framebuffer_side *side,
	VkImageAspectFlags aspects)
{
	/* Whether a device that cannot resolve has been reported (once a process). */
	static int reported;
	struct gles_depth_resolve *resolve;
	struct framebuffer_resolver *resolver;
	struct gles_garbage garbage;
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view_create;
	VkFramebufferCreateInfo framebuffer_create;
	VkRenderPassBeginInfo begin;
	VkClearValue clear;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	VkImageMemoryBarrier barrier;
	VkViewport viewport;
	VkRect2D scissor;
	VkImageView views[2];
	VkImageView target_view;
	VkDescriptorSet sets[2];
	VkFramebuffer framebuffer;
	VkImage image;
	VkDeviceMemory memory;
	VkImageAspectFlags kinds[2];
	uint32_t type;
	uint32_t bit;
	unsigned kind;
	VkResult result;
	int supported;

	/*
	 * A device that samples the image (its format, of its samples), and
	 * the shaders, render pass and pipelines of the format; else the blit
	 * fails as one the device cannot do.
	 */
	resolve = NULL;
	resolver = NULL;
	if ((side->usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U)
		resolve = framebuffer_resolve_objects(state);
	if (resolve != NULL)
		resolver = framebuffer_resolver(state, resolve, side->vk, side->aspects);
	if (resolver == NULL) {
		if (!reported)
			gles_report("a blit that resolves a multisampled depth or stencil buffer the device does not sample", -1);
		reported = 1;
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* An image of the side's format and size, one sample per pixel, drawn into and copied from. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = side->vk;
	create.extent.width = side->extent.width;
	create.extent.height = side->extent.height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	/* A format the device makes no such image of cannot be resolved for the blit (reported once). */
	supported = gles_image_supported(state, &create);
	if (!supported) {
		gles_image_unmade(create.format, "a resolved blit");
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}
	result = vkCreateImage(state->device, &create, NULL, &image);
	if (result != VK_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* Its memory, bound; both wait for the frame from now on. */
	vkGetImageMemoryRequirements(state->device, image, &requirements);
	type = gles_memory_type(state, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX)
		type = gles_memory_type(state, requirements.memoryTypeBits, 0U);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	memory = VK_NULL_HANDLE;
	result = vkAllocateMemory(state->device, &allocate, NULL, &memory);
	if (result == VK_SUCCESS)
		result = vkBindImageMemory(state->device, image, memory, 0U);
	gles_throw_away(state, VK_NULL_HANDLE, image, VK_NULL_HANDLE, memory);
	if (result != VK_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* The new image's view, every aspect of it, and the framebuffer of the pass; they wait for the frame too. */
	memset(&view_create, 0, sizeof(view_create));
	view_create.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_create.image = image;
	view_create.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_create.format = side->vk;
	view_create.subresourceRange.aspectMask = side->aspects;
	view_create.subresourceRange.levelCount = 1U;
	view_create.subresourceRange.layerCount = 1U;
	target_view = VK_NULL_HANDLE;
	result = vkCreateImageView(state->device, &view_create, NULL, &target_view);
	if (result != VK_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}
	gles_throw_away(state, VK_NULL_HANDLE, VK_NULL_HANDLE, target_view, VK_NULL_HANDLE);
	memset(&framebuffer_create, 0, sizeof(framebuffer_create));
	framebuffer_create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_create.renderPass = resolver->pass;
	framebuffer_create.attachmentCount = 1U;
	framebuffer_create.pAttachments = &target_view;
	framebuffer_create.width = side->extent.width;
	framebuffer_create.height = side->extent.height;
	framebuffer_create.layers = 1U;
	framebuffer = VK_NULL_HANDLE;
	result = vkCreateFramebuffer(state->device, &framebuffer_create, NULL, &framebuffer);
	if (result != VK_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}
	memset(&garbage, 0, sizeof(garbage));
	garbage.framebuffer = framebuffer;
	gles_garbage_keep(state, &garbage);

	/* The aspects drawn: depth, then stencil, each read through a view of its aspect of the multisampled image. */
	kinds[0] = VK_IMAGE_ASPECT_DEPTH_BIT;
	kinds[1] = VK_IMAGE_ASPECT_STENCIL_BIT;
	for (kind = 0U; kind < 2U; kind++) {
		views[kind] = VK_NULL_HANDLE;
		sets[kind] = VK_NULL_HANDLE;
		if ((aspects & kinds[kind]) == 0U)
			continue;

		/* The view of the aspect, as a multisample texture's. */
		view_create.image = side->image;
		view_create.subresourceRange.aspectMask = kinds[kind];
		view_create.subresourceRange.baseMipLevel = side->level;
		view_create.subresourceRange.baseArrayLayer = side->layer;
		result = vkCreateImageView(state->device, &view_create, NULL, &views[kind]);
		if (result != VK_SUCCESS) {
			views[kind] = VK_NULL_HANDLE;
			gles_error(context, GL_OUT_OF_MEMORY);
			return -1;
		}
		gles_throw_away(state, VK_NULL_HANDLE, VK_NULL_HANDLE, views[kind], VK_NULL_HANDLE);

		/* Its descriptor set, from the frame's pools. */
		sets[kind] = gles_descriptor_set(state, resolve->set_layout);
		if (sets[kind] == VK_NULL_HANDLE) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return -1;
		}
		memset(&image_info, 0, sizeof(image_info));
		image_info.sampler = resolve->sampler;
		image_info.imageView = views[kind];
		image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		memset(&write, 0, sizeof(write));
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = sets[kind];
		write.dstBinding = 1U;
		write.descriptorCount = 1U;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &image_info;
		vkUpdateDescriptorSets(state->device, 1U, &write, 0U, NULL);
	}

	/* The multisampled image to be sampled. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = side->layout;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = side->image;
	barrier.subresourceRange.aspectMask = side->aspects;
	barrier.subresourceRange.baseMipLevel = side->level;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.baseArrayLayer = side->layer;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(side->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			     0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* The pass over the framebuffer's size, the new image cleared to 0 first. */
	memset(&clear, 0, sizeof(clear));
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass = resolver->pass;
	begin.framebuffer = framebuffer;
	begin.renderArea.extent = side->extent;
	begin.clearValueCount = 1U;
	begin.pClearValues = &clear;
	vkCmdBeginRenderPass(side->command, &begin, VK_SUBPASS_CONTENTS_INLINE);

	/* The viewport of the multisampled image's own size (texel for pixel), drawn inside the framebuffer's. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)side->size.width;
	viewport.height = (float)side->size.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(side->command, 0U, 1U, &viewport);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent = side->extent;
	vkCmdSetScissor(side->command, 0U, 1U, &scissor);

	/* The depth: one triangle. */
	if (sets[0] != VK_NULL_HANDLE) {
		vkCmdBindPipeline(side->command, VK_PIPELINE_BIND_POINT_GRAPHICS, resolver->pipelines[0]);
		vkCmdBindDescriptorSets(side->command, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve->layout, 0U, 1U, &sets[0], 0U, NULL);
		vkCmdDraw(side->command, 3U, 1U, 0U, 0U);
	}

	/* The stencil: a triangle a bit, which it writes alone (all ones where the first sample has it). */
	if (sets[1] != VK_NULL_HANDLE) {
		vkCmdBindPipeline(side->command, VK_PIPELINE_BIND_POINT_GRAPHICS, resolver->pipelines[1]);
		vkCmdBindDescriptorSets(side->command, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve->layout, 0U, 1U, &sets[1], 0U, NULL);
		vkCmdSetStencilReference(side->command, VK_STENCIL_FACE_FRONT_AND_BACK, 0xffU);
		vkCmdSetStencilCompareMask(side->command, VK_STENCIL_FACE_FRONT_AND_BACK, 0xffU);
		for (bit = 0U; bit < 8U; bit++) {
			vkCmdSetStencilWriteMask(side->command, VK_STENCIL_FACE_FRONT_AND_BACK, 1U << bit);
			vkCmdDraw(side->command, 3U, 1U, 3U * bit, 0U);
		}
	}

	/* The pass leaves the new image to be copied from. */
	vkCmdEndRenderPass(side->command);

	/* The multisampled image back where it rests. */
	barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.newLayout = side->layout;
	vkCmdPipelineBarrier(side->command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			     0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* What was read waits for the frame. */
	if (side->renderbuffer != NULL)
		side->renderbuffer->used = state->frame;
	if (side->texture != NULL)
		side->texture->used = state->frame;

	/* Succeeded: the side is the resolved image. */
	side->image = image;
	side->usage = create.usage;
	side->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	side->samples = 1U;
	side->level = 0U;
	side->layer = 0U;
	side->size = side->extent;
	side->renderbuffer = NULL;
	side->texture = NULL;
	return 0;
}

/*
 * Returns what depth and stencil resolves are made with, making it at the
 * first use: the GLSL shaders compiled and linked into SPIR-V modules,
 * the set layout of one sampled image (the sampler's binding, 1), the
 * pipeline layout and a sampler.  NULL when it could not be made (it is
 * not tried again).
 */
static struct gles_depth_resolve *
framebuffer_resolve_objects(
	struct gles_state *state)
{
	static const char *const fragments[2] = { framebuffer_resolve_depth_fragment, framebuffer_resolve_stencil_fragment };
	struct gles_depth_resolve *resolve;
	struct glsl_shader *vertex;
	struct glsl_shader *fragment;
	struct glsl_program program;
	VkShaderModuleCreateInfo module;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set;
	VkPipelineLayoutCreateInfo layout;
	VkSamplerCreateInfo sampler;
	VkResult result;
	unsigned kind;
	unsigned stage;
	char *log;
	int status;

	/* Made once, failed or not. */
	if (state->depth_resolve != NULL) {
		if (state->depth_resolve->failed)
			return NULL;
		return state->depth_resolve;
	}
	resolve = calloc(1U, sizeof(*resolve));
	if (resolve == NULL)
		return NULL;
	state->depth_resolve = resolve;
	resolve->failed = 1;

	/* The vertex shader, linked with each fragment shader into its stages' modules. */
	log = NULL;
	vertex = glsl_compile(GLSL_STAGE_VERTEX, framebuffer_resolve_vertex, 100U, &log);
	free(log);
	if (vertex == NULL) {
		gles_report("the depth resolve's vertex shader", -1);
		return NULL;
	}
	for (kind = 0U; kind < 2U; kind++) {
		log = NULL;
		fragment = glsl_compile(GLSL_STAGE_FRAGMENT, fragments[kind], 100U, &log);
		free(log);
		if (fragment == NULL) {
			gles_report("the depth resolve's fragment shader", (int)kind);
			glsl_shader_free(vertex);
			return NULL;
		}

		/* Linked: one sampler (binding 1) and nothing in a uniform block. */
		memset(&program, 0, sizeof(program));
		log = NULL;
		status = glsl_link(vertex, fragment, NULL, 0U, &program, &log);
		free(log);
		glsl_shader_free(fragment);
		if (status != 0 ||
		    program.uniform_count != 1U ||
		    program.uniforms[0].sampler != GLSL_SAMPLER_MS) {
			gles_report("the depth resolve's link", (int)kind);
			glsl_program_free(&program);
			glsl_shader_free(vertex);
			return NULL;
		}

		/* The two stages' modules. */
		for (stage = 0U; stage < 2U; stage++) {
			memset(&module, 0, sizeof(module));
			module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			module.codeSize = program.words[stage] * sizeof(uint32_t);
			module.pCode = program.code[stage];
			result = vkCreateShaderModule(state->device, &module, NULL, &resolve->modules[kind * 2U + stage]);
			if (result != VK_SUCCESS) {
				resolve->modules[kind * 2U + stage] = VK_NULL_HANDLE;
				gles_report("vkCreateShaderModule", (int)result);
				glsl_program_free(&program);
				glsl_shader_free(vertex);
				return NULL;
			}
		}
		glsl_program_free(&program);
	}
	glsl_shader_free(vertex);

	/* The set layout of the one sampled image, read by the fragment shader. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 1U;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1U;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&set, 0, sizeof(set));
	set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set.bindingCount = 1U;
	set.pBindings = &binding;
	result = vkCreateDescriptorSetLayout(state->device, &set, NULL, &resolve->set_layout);
	if (result != VK_SUCCESS) {
		resolve->set_layout = VK_NULL_HANDLE;
		return NULL;
	}

	/* The pipeline layout of that set. */
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1U;
	layout.pSetLayouts = &resolve->set_layout;
	result = vkCreatePipelineLayout(state->device, &layout, NULL, &resolve->layout);
	if (result != VK_SUCCESS) {
		resolve->layout = VK_NULL_HANDLE;
		return NULL;
	}

	/* A sampler (texelFetch filters nothing). */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxAnisotropy = 1.0f;
	sampler.compareOp = VK_COMPARE_OP_NEVER;
	result = vkCreateSampler(state->device, &sampler, NULL, &resolve->sampler);
	if (result != VK_SUCCESS) {
		resolve->sampler = VK_NULL_HANDLE;
		return NULL;
	}

	/* Succeeded: made. */
	resolve->failed = 0;
	return resolve;
}

/*
 * Returns the render pass and pipelines that resolve a depth and stencil
 * format of some aspects, making them at the first use: one depth and
 * stencil attachment of one sample, cleared to 0, left to be copied from;
 * a pipeline for each aspect.  NULL when they cannot be made.
 */
static struct framebuffer_resolver *
framebuffer_resolver(
	struct gles_state *state,
	struct gles_depth_resolve *resolve,
	VkFormat vk,
	VkImageAspectFlags aspects)
{
	struct framebuffer_resolver *resolver;
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo create;
	VkResult result;

	/* One made already (with the pipelines, or not: they are not tried again). */
	for (resolver = resolve->resolvers; resolver != NULL; resolver = resolver->next) {
		if (resolver->vk != vk)
			continue;
		if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U && resolver->pipelines[0] == VK_NULL_HANDLE)
			return NULL;
		if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U && resolver->pipelines[1] == VK_NULL_HANDLE)
			return NULL;
		return resolver;
	}

	/* The attachment: cleared, kept, then copied from. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = vk;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.pDepthStencilAttachment = &reference;

	/* Its writes done before the copy reads them. */
	memset(&dependency, 0, sizeof(dependency));
	dependency.srcSubpass = 0U;
	dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
	dependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	/* The resolver, with its pass. */
	resolver = calloc(1U, sizeof(*resolver));
	if (resolver == NULL)
		return NULL;
	resolver->vk = vk;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = 1U;
	create.pAttachments = &attachment;
	create.subpassCount = 1U;
	create.pSubpasses = &subpass;
	create.dependencyCount = 1U;
	create.pDependencies = &dependency;
	result = vkCreateRenderPass(state->device, &create, NULL, &resolver->pass);
	if (result != VK_SUCCESS) {
		gles_report("vkCreateRenderPass", (int)result);
		free(resolver);
		return NULL;
	}

	/* Kept with the others (freed with them, even when a pipeline is not made). */
	resolver->next = resolve->resolvers;
	resolve->resolvers = resolver;

	/* A pipeline for each aspect the format has. */
	if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U) {
		resolver->pipelines[0] = framebuffer_resolve_pipeline(state, resolve, resolver->pass, 0);
		if (resolver->pipelines[0] == VK_NULL_HANDLE)
			return NULL;
	}
	if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U) {
		resolver->pipelines[1] = framebuffer_resolve_pipeline(state, resolve, resolver->pass, 1);
		if (resolver->pipelines[1] == VK_NULL_HANDLE)
			return NULL;
	}

	/* Succeeded: the pass and pipelines. */
	return resolver;
}

/*
 * Makes the pipeline that resolves depth (stencil zero: the depth
 * written whatever it was) or stencil (nonzero: all ones written into the
 * bits the write mask leaves, where the fragment is kept) in a pass.
 * VK_NULL_HANDLE when it cannot be made.
 */
static VkPipeline
framebuffer_resolve_pipeline(
	struct gles_state *state,
	struct gles_depth_resolve *resolve,
	VkRenderPass pass,
	int stencil)
{
	static const VkDynamicState dynamics[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
		VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
		VK_DYNAMIC_STATE_STENCIL_REFERENCE
	};
	VkPipelineShaderStageCreateInfo stages[2];
	VkPipelineVertexInputStateCreateInfo input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depth;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic;
	VkGraphicsPipelineCreateInfo create;
	VkPipeline pipeline;
	VkResult result;
	unsigned first;

	/* The kind's vertex and fragment shader. */
	first = 0U;
	if (stencil)
		first = 2U;
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = resolve->modules[first];
	stages[0].pName = "main";
	stages[1] = stages[0];
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = resolve->modules[first + 1U];

	/* No vertices read: triangles, one viewport and scissor, filled, one sample. */
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1U;
	viewport.scissorCount = 1U;
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	/* The depth written always, or the stencil's reference (all ones) written into the write mask's bits. */
	memset(&depth, 0, sizeof(depth));
	depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	if (!stencil) {
		depth.depthTestEnable = VK_TRUE;
		depth.depthWriteEnable = VK_TRUE;
		depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	} else {
		depth.stencilTestEnable = VK_TRUE;
		depth.front.failOp = VK_STENCIL_OP_KEEP;
		depth.front.passOp = VK_STENCIL_OP_REPLACE;
		depth.front.depthFailOp = VK_STENCIL_OP_REPLACE;
		depth.front.compareOp = VK_COMPARE_OP_ALWAYS;
		depth.front.compareMask = 0xffU;
		depth.front.writeMask = 0xffU;
		depth.front.reference = 0xffU;
		depth.back = depth.front;
	}

	/* No colour. */
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = (uint32_t)(sizeof(dynamics) / sizeof(dynamics[0]));
	dynamic.pDynamicStates = dynamics;

	/* The pipeline. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	create.stageCount = 2U;
	create.pStages = stages;
	create.pVertexInputState = &input;
	create.pInputAssemblyState = &assembly;
	create.pViewportState = &viewport;
	create.pRasterizationState = &raster;
	create.pMultisampleState = &multisample;
	create.pDepthStencilState = &depth;
	create.pColorBlendState = &blend;
	create.pDynamicState = &dynamic;
	create.layout = resolve->layout;
	create.renderPass = pass;
	pipeline = VK_NULL_HANDLE;
	result = vkCreateGraphicsPipelines(state->device, VK_NULL_HANDLE, 1U, &create, NULL, &pipeline);
	if (result != VK_SUCCESS) {
		gles_report("vkCreateGraphicsPipelines", (int)result);
		return VK_NULL_HANDLE;
	}

	/* Succeeded: the pipeline. */
	return pipeline;
}

/* Frees what depth and stencil resolves were made with (nothing may still run). */
static void
framebuffer_resolve_free(
	struct gles_state *state)
{
	struct gles_depth_resolve *resolve;
	struct framebuffer_resolver *resolver;
	unsigned index;

	/* Nothing made. */
	resolve = state->depth_resolve;
	if (resolve == NULL)
		return;
	state->depth_resolve = NULL;

	/* Each format's pipelines and pass. */
	while (resolve->resolvers != NULL) {
		resolver = resolve->resolvers;
		resolve->resolvers = resolver->next;
		for (index = 0U; index < 2U; index++) {
			if (resolver->pipelines[index] != VK_NULL_HANDLE)
				vkDestroyPipeline(state->device, resolver->pipelines[index], NULL);
		}
		vkDestroyRenderPass(state->device, resolver->pass, NULL);
		free(resolver);
	}

	/* The sampler, the layouts and the modules. */
	if (resolve->sampler != VK_NULL_HANDLE)
		vkDestroySampler(state->device, resolve->sampler, NULL);
	if (resolve->layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(state->device, resolve->layout, NULL);
	if (resolve->set_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(state->device, resolve->set_layout, NULL);
	for (index = 0U; index < 4U; index++) {
		if (resolve->modules[index] != VK_NULL_HANDLE)
			vkDestroyShaderModule(state->device, resolve->modules[index], NULL);
	}
	free(resolve);
}

/*
 * Copies a rectangle of one side into a rectangle of another with a
 * filter (GL's corners, rectangles[0..3] the read one's, [4..7] the draw
 * one's), inside the draw side's size and the scissor box, and marks what
 * it drew into.
 */
static void
framebuffer_blit(
	struct gles_state *state,
	const struct framebuffer_side *source,
	const struct framebuffer_side *target,
	const GLint *rectangles,
	VkImageAspectFlags aspects,
	GLenum filter)
{
	VkImageMemoryBarrier barriers[2];
	VkImageBlit region;
	VkImageLayout source_layout;
	VkImageLayout target_layout;
	VkFilter scale;
	int32_t x[4];
	int32_t y[4];
	int32_t low_x;
	int32_t low_y;
	int32_t high_x;
	int32_t high_y;
	int inside;

	/* Where the draw side may be written: its size, and the scissor box. */
	low_x = 0;
	low_y = 0;
	high_x = (int32_t)target->extent.width;
	high_y = (int32_t)target->extent.height;
	if (state->scissor_test) {
		if (state->scissor[0] > low_x)
			low_x = state->scissor[0];
		if (state->scissor[1] > low_y)
			low_y = state->scissor[1];
		if (state->scissor[0] + state->scissor[2] < high_x)
			high_x = state->scissor[0] + state->scissor[2];
		if (state->scissor[1] + state->scissor[3] < high_y)
			high_y = state->scissor[1] + state->scissor[3];
	}

	/* Each axis clipped to the read side's size and to where the draw side may be written. */
	inside = framebuffer_blit_axis(rectangles[0], rectangles[2], rectangles[4], rectangles[6],
				       (int32_t)source->extent.width, low_x, high_x, x);
	if (!inside)
		return;
	inside = framebuffer_blit_axis(rectangles[1], rectangles[3], rectangles[5], rectangles[7],
				       (int32_t)source->extent.height, low_y, high_y, y);
	if (!inside)
		return;

	/* The region, rows turned over on a side whose rows go down, a 3D slice as one layer of depth. */
	memset(&region, 0, sizeof(region));
	region.srcSubresource.aspectMask = aspects;
	region.srcSubresource.mipLevel = source->level;
	region.srcSubresource.baseArrayLayer = source->layer;
	region.srcSubresource.layerCount = 1U;
	region.srcOffsets[0].x = x[0];
	region.srcOffsets[1].x = x[1];
	region.srcOffsets[0].y = y[0];
	region.srcOffsets[1].y = y[1];
	if (source->flip) {
		region.srcOffsets[0].y = (int32_t)source->extent.height - y[0];
		region.srcOffsets[1].y = (int32_t)source->extent.height - y[1];
	}

	/* The read side's slice. */
	region.srcOffsets[0].z = source->slice;
	region.srcOffsets[1].z = source->slice + 1;
	region.dstSubresource.aspectMask = aspects;
	region.dstSubresource.mipLevel = target->level;
	region.dstSubresource.baseArrayLayer = target->layer;
	region.dstSubresource.layerCount = 1U;
	region.dstOffsets[0].x = x[2];
	region.dstOffsets[1].x = x[3];
	region.dstOffsets[0].y = y[2];
	region.dstOffsets[1].y = y[3];
	if (target->flip) {
		region.dstOffsets[0].y = (int32_t)target->extent.height - y[2];
		region.dstOffsets[1].y = (int32_t)target->extent.height - y[3];
	}

	/* The draw side's slice. */
	region.dstOffsets[0].z = target->slice;
	region.dstOffsets[1].z = target->slice + 1;

	/* The layouts of the copy (one layout both ways when a side is the other's own level and layer). */
	source_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	target_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	if (source->image == target->image && source->level == target->level && source->layer == target->layer) {
		source_layout = VK_IMAGE_LAYOUT_GENERAL;
		target_layout = VK_IMAGE_LAYOUT_GENERAL;
	}

	/* Both ready to be copied. */
	memset(barriers, 0, sizeof(barriers));
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
				    VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].oldLayout = source->layout;
	barriers[0].newLayout = source_layout;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = source->image;
	barriers[0].subresourceRange.aspectMask = source->aspects;
	barriers[0].subresourceRange.baseMipLevel = source->level;
	barriers[0].subresourceRange.levelCount = 1U;
	barriers[0].subresourceRange.baseArrayLayer = source->layer;
	barriers[0].subresourceRange.layerCount = 1U;
	barriers[1] = barriers[0];
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].oldLayout = target->layout;
	barriers[1].newLayout = target_layout;
	barriers[1].image = target->image;
	barriers[1].subresourceRange.aspectMask = target->aspects;
	barriers[1].subresourceRange.baseMipLevel = target->level;
	barriers[1].subresourceRange.baseArrayLayer = target->layer;
	vkCmdPipelineBarrier(target->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0U, 0U, NULL, 0U, NULL, 2U, barriers);

	/* The copy, scaled with the filter. */
	scale = VK_FILTER_NEAREST;
	if (filter == GL_LINEAR)
		scale = VK_FILTER_LINEAR;
	vkCmdBlitImage(target->command, source->image, source_layout, target->image, target_layout, 1U, &region, scale);

	/* Both back where they rest. */
	barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].oldLayout = source_layout;
	barriers[0].newLayout = source->layout;
	barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
				    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
	barriers[1].oldLayout = target_layout;
	barriers[1].newLayout = target->layout;
	vkCmdPipelineBarrier(target->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			     0U, 0U, NULL, 0U, NULL, 2U, barriers);

	/* What was read and written waits for the frame; a texture's level on the CPU is older than its image. */
	if (source->texture != NULL)
		source->texture->used = state->frame;
	if (source->renderbuffer != NULL)
		source->renderbuffer->used = state->frame;
	if (target->renderbuffer != NULL)
		target->renderbuffer->used = state->frame;
	if (target->texture != NULL) {
		target->texture->used = state->frame;
		target->texture->gpu_levels[target->face] |= 1U << (unsigned)target->gl_level;
	}
}

/*
 * Clips one axis of a blit: the read side's run from a0 to a1 and the
 * draw side's from b0 to b1 (either reversed to mirror), kept to where
 * the read side has texels (0 to limit) and the draw side may be written
 * (low to high), in step.  Writes the clipped a0, a1, b0, b1 into out;
 * returns 0 when nothing is left.
 */
static int
framebuffer_blit_axis(
	GLint a0,
	GLint a1,
	GLint b0,
	GLint b1,
	int32_t limit,
	int32_t low,
	int32_t high,
	int32_t *out)
{
	double first;
	double last;
	double enter;
	double leave;
	double swap;

	/* An empty run copies nothing. */
	if (a0 == a1 || b0 == b1)
		return 0;

	/* The part of the run (0 to 1) whose draw side lies between low and high. */
	first = 0.0;
	last = 1.0;
	enter = ((double)low - (double)b0) / ((double)b1 - (double)b0);
	leave = ((double)high - (double)b0) / ((double)b1 - (double)b0);
	if (enter > leave) {
		swap = enter;
		enter = leave;
		leave = swap;
	}

	/* The later of the two starts, the earlier of the two ends. */
	if (enter > first)
		first = enter;
	if (leave < last)
		last = leave;

	/* And whose read side lies between 0 and the limit. */
	enter = (0.0 - (double)a0) / ((double)a1 - (double)a0);
	leave = ((double)limit - (double)a0) / ((double)a1 - (double)a0);
	if (enter > leave) {
		swap = enter;
		enter = leave;
		leave = swap;
	}

	/* The later of the starts, the earlier of the ends. */
	if (enter > first)
		first = enter;
	if (leave < last)
		last = leave;

	/* Nothing left. */
	if (first >= last)
		return 0;

	/* The ends, rounded to whole texels. */
	out[0] = (int32_t)((double)a0 + first * ((double)a1 - (double)a0) + 0.5);
	out[1] = (int32_t)((double)a0 + last * ((double)a1 - (double)a0) + 0.5);
	out[2] = (int32_t)((double)b0 + first * ((double)b1 - (double)b0) + 0.5);
	out[3] = (int32_t)((double)b0 + last * ((double)b1 - (double)b0) + 0.5);
	if (out[0] == out[1] || out[2] == out[3])
		return 0;

	/* Succeeded: the clipped run. */
	return 1;
}

/*
 * Makes a render pass over colour images of formats (a slot of
 * VK_FORMAT_UNDEFINED is an unused attachment) and a depth and stencil
 * image, each resting in its layout, that loads and keeps them, and waits
 * for (and makes later work wait for) the draws, copies and samples
 * around it.
 */
static VkResult
framebuffer_pass(
	struct gles_state *state,
	const struct gles_pass_format *format,
	const VkImageLayout *color_layouts,
	VkImageLayout depth_layout,
	VkRenderPass *pass)
{
	VkAttachmentDescription attachments[GLES_COLOR_ATTACHMENTS + 1U];
	VkAttachmentReference color_references[GLES_COLOR_ATTACHMENTS];
	VkAttachmentReference depth_reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo create;
	uint32_t count;
	unsigned index;
	VkResult result;

	/* Each colour attachment, loaded and kept in its resting layout (an unused slot has none). */
	memset(attachments, 0, sizeof(attachments));
	memset(&depth_reference, 0, sizeof(depth_reference));
	count = 0U;
	for (index = 0U; index < format->color_count; index++) {
		color_references[index].attachment = VK_ATTACHMENT_UNUSED;
		color_references[index].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		if (format->colors[index] == (uint32_t)VK_FORMAT_UNDEFINED)
			continue;

		/* The attachment. */
		attachments[count].format = (VkFormat)format->colors[index];
		attachments[count].samples = (VkSampleCountFlagBits)format->samples;
		attachments[count].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[count].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[count].initialLayout = color_layouts[index];
		attachments[count].finalLayout = color_layouts[index];
		color_references[index].attachment = count;
		count++;
	}

	/* The depth and stencil attachment, loaded and kept. */
	if (format->depth != (uint32_t)VK_FORMAT_UNDEFINED) {
		attachments[count].format = (VkFormat)format->depth;
		attachments[count].samples = (VkSampleCountFlagBits)format->samples;
		attachments[count].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[count].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[count].initialLayout = depth_layout;
		attachments[count].finalLayout = depth_layout;
		depth_reference.attachment = count;
		depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		count++;
	}

	/* One subpass over them, fragment output i into colour slot i. */
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = format->color_count;
	subpass.pColorAttachments = color_references;
	if (format->depth != (uint32_t)VK_FORMAT_UNDEFINED)
		subpass.pDepthStencilAttachment = &depth_reference;

	/* Earlier draws, copies and samples of the images finish before the pass writes them. */
	memset(dependencies, 0, sizeof(dependencies));
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0U;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
				       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
				       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
					VK_ACCESS_TRANSFER_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
					VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	/* Later samples, copies and draws wait for what the pass wrote. */
	dependencies[1].srcSubpass = 0U;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
				       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
					VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	/* The pass. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = count;
	create.pAttachments = attachments;
	create.subpassCount = 1U;
	create.pSubpasses = &subpass;
	create.dependencyCount = 2U;
	create.pDependencies = dependencies;

	/* Made, or refused. */
	*pass = VK_NULL_HANDLE;
	result = vkCreateRenderPass(state->device, &create, NULL, pass);
	if (result != VK_SUCCESS) {
		*pass = VK_NULL_HANDLE;
		return result;
	}

	/* Succeeded: the pass. */
	return VK_SUCCESS;
}

/*
 * Returns the render pass framebuffer objects' pipelines of a set of
 * formats are made with (every framebuffer object's pass of the formats
 * is compatible with it), making it the first time; VK_NULL_HANDLE when
 * the device refused.
 */
static VkRenderPass
framebuffer_compatible(
	struct gles_state *state,
	const struct gles_pass_format *format)
{
	VkImageLayout color_layouts[GLES_COLOR_ATTACHMENTS];
	struct gles_compatible_pass *compatible;
	unsigned index;
	VkResult result;
	int differs;

	/* Made before. */
	for (compatible = state->compatible_passes; compatible != NULL; compatible = compatible->next) {
		differs = memcmp(&compatible->format, format, sizeof(*format));
		if (differs == 0)
			return compatible->pass;
	}

	/* A new entry. */
	compatible = calloc(1U, sizeof(*compatible));
	if (compatible == NULL)
		return VK_NULL_HANDLE;

	/* Made now (the layouts do not matter to compatibility). */
	for (index = 0U; index < GLES_COLOR_ATTACHMENTS; index++)
		color_layouts[index] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	result = framebuffer_pass(state, format, color_layouts, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, &compatible->pass);
	if (result != VK_SUCCESS) {
		gles_report("vkCreateRenderPass", (int)result);
		free(compatible);
		return VK_NULL_HANDLE;
	}

	/* Succeeded: the pass, kept. */
	compatible->format = *format;
	compatible->next = state->compatible_passes;
	state->compatible_passes = compatible;
	return compatible->pass;
}

/*
 * Attaches (or with kind GLES_ATTACH_NONE detaches) an object to an
 * attachment point of the framebuffer object bound to a target;
 * GL_DEPTH_STENCIL_ATTACHMENT is both the depth and the stencil point.
 */
static void
framebuffer_attach(
	struct zegl_context *context,
	GLenum target,
	GLenum attachment,
	int kind,
	GLuint name,
	unsigned face,
	GLint level,
	GLint layer)
{
	struct gles_state *state;
	struct gles_framebuffer *fbo;
	struct gles_attachment *points[2];
	struct gles_attachment made;
	GLuint bound;
	unsigned index;
	int status;

	/* A framebuffer object bound to the target (the surfaces' takes no attachments). */
	state = gles_state(context);
	bound = 0U;
	fbo = framebuffer_for(context, state, target, 1, &bound);
	if (fbo == NULL)
		return;

	/* The point, or both depth and stencil. */
	points[1] = NULL;
	if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
		points[0] = &fbo->depth;
		points[1] = &fbo->stencil;
	} else {
		status = framebuffer_point(state, attachment, &points[0], fbo);
		if (status != 0) {
			gles_error(context, (GLenum)status);
			return;
		}
	}

	/* The attachment, by name. */
	memset(&made, 0, sizeof(made));
	made.kind = kind;
	made.name = name;
	made.face = face;
	made.level = level;
	made.layer = layer;

	/* An open pass over the object ends; the pass is made again at the next draw. */
	if (state->open_fbo == fbo)
		gles_target_close(state);
	for (index = 0U; index < 2U; index++) {
		if (points[index] != NULL)
			*points[index] = made;
	}
}

/*
 * Finds a framebuffer object's attachment point for GL's name of it.
 * Returns 0, GL_INVALID_OPERATION for a colour attachment past the
 * context's GL_MAX_COLOR_ATTACHMENTS, or GL_INVALID_ENUM for a name that
 * is not a point (GL_DEPTH_STENCIL_ATTACHMENT is its callers').
 */
static int
framebuffer_point(
	const struct gles_state *state,
	GLenum attachment,
	struct gles_attachment **point,
	struct gles_framebuffer *fbo)
{
	/* The colour attachments the device has. */
	if (attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + state->color_attachments) {
		*point = &fbo->colors[attachment - GL_COLOR_ATTACHMENT0];
		return 0;
	}

	/* Those GL names that the device does not have. */
	if (attachment >= GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT31)
		return GL_INVALID_OPERATION;

	/* The depth and the stencil points. */
	switch (attachment) {
	case GL_DEPTH_ATTACHMENT:
		*point = &fbo->depth;
		return 0;
	case GL_STENCIL_ATTACHMENT:
		*point = &fbo->stencil;
		return 0;
	default:
		break;
	}

	/* Not one. */
	*point = NULL;
	return -1;
}

/*
 * Returns the bits of a component a format has (GL_*_SIZE of a
 * framebuffer attachment or a renderbuffer): the kept Vulkan format's, 0
 * for a component GL's format does not have.
 */
static GLint
framebuffer_bits(
	const struct gles_format *format,
	VkFormat vk,
	GLenum pname)
{
	GLint sizes[6];
	unsigned components;
	unsigned index;
	GLint each;

	/* Nothing without a format. */
	memset(sizes, 0, sizeof(sizes));
	if (format == NULL && vk == VK_FORMAT_UNDEFINED)
		return 0;

	/* The kept format's component sizes: red, green, blue, alpha, depth, stencil. */
	switch (vk) {
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
	case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		sizes[0] = 10;
		sizes[1] = 10;
		sizes[2] = 10;
		sizes[3] = 2;
		break;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		sizes[0] = 11;
		sizes[1] = 11;
		sizes[2] = 10;
		break;
	case VK_FORMAT_D16_UNORM:
		sizes[4] = 16;
		break;
	case VK_FORMAT_X8_D24_UNORM_PACK32:
		sizes[4] = 24;
		break;
	case VK_FORMAT_D32_SFLOAT:
		sizes[4] = 32;
		break;
	case VK_FORMAT_D24_UNORM_S8_UINT:
		sizes[4] = 24;
		sizes[5] = 8;
		break;
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		sizes[4] = 32;
		sizes[5] = 8;
		break;
	case VK_FORMAT_S8_UINT:
		sizes[5] = 8;
		break;
	default:
		/* Components of equal size, as many as GL's format has (the fourth of an RGB format is not GL's). */
		if (format == NULL || format->components == 0U)
			break;
		each = (GLint)(format->bytes * 8U / format->components);
		components = format->components;
		if (format->base == GL_RGB || format->base == GL_RGB_INTEGER)
			components = 3U;
		for (index = 0U; index < components; index++)
			sizes[index] = each;
		break;
	}

	/* The component asked for. */
	switch (pname) {
	case GL_RENDERBUFFER_RED_SIZE:
	case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
		return sizes[0];
	case GL_RENDERBUFFER_GREEN_SIZE:
	case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
		return sizes[1];
	case GL_RENDERBUFFER_BLUE_SIZE:
	case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
		return sizes[2];
	case GL_RENDERBUFFER_ALPHA_SIZE:
	case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
		return sizes[3];
	case GL_RENDERBUFFER_DEPTH_SIZE:
	case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
		return sizes[4];
	default:
		break;
	}

	/* Succeeded: the stencil bits. */
	return sizes[5];
}

/*
 * Returns a parameter of a framebuffer object's attachment point (GL's
 * glGetFramebufferAttachmentParameteriv); *known is 0 when the name is
 * not a parameter the point has.
 */
static GLint
framebuffer_attachment_value(
	struct gles_state *state,
	const struct gles_attachment *point,
	GLenum pname,
	int *known)
{
	struct gles_texture *texture;
	struct gles_renderbuffer *renderbuffer;
	const struct gles_format *format;
	VkFormat vk;

	/* What is attached: its kind first. */
	*known = 1;
	if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) {
		if (point->kind == GLES_ATTACH_TEXTURE)
			return GL_TEXTURE;
		if (point->kind == GLES_ATTACH_RENDERBUFFER)
			return GL_RENDERBUFFER;
		return GL_NONE;
	}

	/* Nothing attached has a name and nothing else. */
	if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) {
		if (point->kind == GLES_ATTACH_NONE)
			return 0;
		return (GLint)point->name;
	}

	/* Nothing attached has nothing else. */
	if (point->kind == GLES_ATTACH_NONE) {
		*known = 0;
		return 0;
	}

	/* The format of the object, and the texture's level and layer. */
	texture = NULL;
	renderbuffer = NULL;
	format = NULL;
	vk = VK_FORMAT_UNDEFINED;
	if (point->kind == GLES_ATTACH_TEXTURE)
		texture = gles_names_get(&state->textures, point->name);
	if (point->kind == GLES_ATTACH_RENDERBUFFER)
		renderbuffer = gles_names_get(&state->renderbuffers, point->name);
	if (texture != NULL)
		format = texture->levels[point->face * GLES_LEVELS + (unsigned)point->level].format;
	if (renderbuffer != NULL) {
		format = renderbuffer->kept;
		vk = renderbuffer->vk;
	}

	/* A texture's format is its Vulkan one. */
	if (format != NULL && vk == VK_FORMAT_UNDEFINED)
		vk = format->vk;

	/* The parameter. */
	switch (pname) {
	case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
		return point->level;
	case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
		if (texture != NULL && texture->target == GL_TEXTURE_CUBE_MAP)
			return (GLint)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + point->face);
		return 0;
	case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
		return point->layer;
	case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
		if (vk == VK_FORMAT_R8G8B8A8_SRGB)
			return GL_SRGB;
		return GL_LINEAR;
	case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
		if (format == NULL)
			return GL_NONE;
		if (format->kind == GLES_TEXEL_INT)
			return GL_INT;
		if (format->kind == GLES_TEXEL_UINT)
			return GL_UNSIGNED_INT;
		if (format->kind == GLES_TEXEL_FLOAT || vk == VK_FORMAT_D32_SFLOAT || vk == VK_FORMAT_D32_SFLOAT_S8_UINT)
			return GL_FLOAT;
		return GL_UNSIGNED_NORMALIZED;
	case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
	case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
	case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
	case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
	case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
	case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
		return framebuffer_bits(format, vk, pname);
	default:
		break;
	}

	/* Not a parameter. */
	*known = 0;
	return 0;
}

/* Returns the sample counts above one every framebuffer attachment of the device may have, as Vulkan's bits (at most 8). */
static uint32_t
framebuffer_samples_mask(
	struct gles_state *state)
{
	uint32_t mask;

	/* The counts colour, depth and stencil attachments all have. */
	mask = state->limits.framebufferColorSampleCounts;
	mask &= state->limits.framebufferDepthSampleCounts;
	mask &= state->limits.framebufferStencilSampleCounts;

	/* Succeeded: those from 2 to 8. */
	return mask & (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT);
}

/*
 * Returns the sample counts above one an image of integers may have, as
 * Vulkan's bits: those of every framebuffer attachment that the device
 * also samples of integers (sampledImageIntegerSampleCounts), since one
 * limit (GL_MAX_INTEGER_SAMPLES) holds for an integer renderbuffer and an
 * integer multisample texture read by isampler2DMS and usampler2DMS.
 */
static uint32_t
framebuffer_integer_mask(
	struct gles_state *state)
{
	uint32_t mask;

	/* The attachments' counts the device samples integers of. */
	mask = framebuffer_samples_mask(state);
	mask &= state->limits.sampledImageIntegerSampleCounts;

	/* Succeeded: the counts. */
	return mask;
}

/* Returns the sample counts above one a multisample texture of a colour format may have: every attachment's that the device samples of colours. */
static uint32_t
framebuffer_color_texture_mask(
	struct gles_state *state)
{
	uint32_t mask;

	/* The attachments' counts the device samples colours of. */
	mask = framebuffer_samples_mask(state);
	mask &= state->limits.sampledImageColorSampleCounts;

	/* Succeeded: the counts. */
	return mask;
}

/*
 * Returns the sample counts above one a multisample texture of a depth or
 * depth and stencil format may have: every attachment's that the device
 * samples of depth and of stencil (a depth and stencil texture's stencil
 * is sampled too, GL_DEPTH_STENCIL_TEXTURE_MODE).
 */
static uint32_t
framebuffer_depth_texture_mask(
	struct gles_state *state)
{
	uint32_t mask;

	/* The attachments' counts the device samples depth and stencil of. */
	mask = framebuffer_samples_mask(state);
	mask &= state->limits.sampledImageDepthSampleCounts;
	mask &= state->limits.sampledImageStencilSampleCounts;

	/* Succeeded: the counts. */
	return mask;
}

/*
 * Returns the usage a multisample texture's image of a format is made
 * with (texture.c's of a 2D texture): sampled and copied, and drawn into
 * where the format can be.
 */
static VkImageUsageFlags
framebuffer_texture_usage(
	struct gles_state *state,
	const struct gles_format *format)
{
	VkImageUsageFlags usage;
	uint32_t features;

	/* Sampled and copied. */
	usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	/* Drawn into as a colour or a depth attachment, where the format's features say so. */
	features = gles_image_features(state, format->vk);
	if (format->kind != GLES_TEXEL_DEPTH && (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0U)
		usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (format->kind == GLES_TEXEL_DEPTH && (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U)
		usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	/* Succeeded: the usage. */
	return usage;
}

/* Reports whether the context multisamples integer formats: OpenGL ES 3.1's and desktop GL's do, OpenGL ES 3.0's does not. */
static int
framebuffer_integer_multisampled(
	const struct gles_state *state)
{
	/* Desktop GL (libGL). */
	if (gles_fixed != NULL)
		return 1;

	/* OpenGL ES 3.1 (a context that offers compute). */
	if (state->compute)
		return 1;

	/* Succeeded: OpenGL ES 3.0 and before. */
	return 0;
}

/* Reports whether the context has multisample textures: desktop GL's does, and OpenGL ES 3.1's (a context that offers compute). */
static int
framebuffer_multisample_textures(
	const struct gles_state *state)
{
	/* Desktop GL (libGL). */
	if (gles_fixed != NULL)
		return 1;

	/* OpenGL ES 3.1. */
	if (state->compute)
		return 1;

	/* Succeeded: OpenGL ES 3.0 and before. */
	return 0;
}

/* Returns the largest count of a mask of sample counts (Vulkan's bits, at most 8), 1 for none. */
static uint32_t
framebuffer_samples_most(
	uint32_t mask)
{
	uint32_t count;

	/* The counts from the largest. */
	for (count = 8U; count >= 2U; count /= 2U) {
		if ((mask & count) != 0U)
			return count;
	}

	/* Succeeded: no multisampling. */
	return 1U;
}

/* Returns the fewest samples a framebuffer with nothing attached has of at least a count (1 for a count of 0 or 1, or past the device's). */
static uint32_t
framebuffer_default_samples(
	struct gles_state *state,
	uint32_t samples)
{
	uint32_t count;

	/* Not multisampled. */
	if (samples <= 1U)
		return 1U;

	/* The counts in order, from 2. */
	for (count = 2U; count <= 64U; count *= 2U) {
		if (count >= samples && (state->limits.framebufferNoAttachmentsSampleCounts & count) != 0U)
			return count;
	}

	/* None: one. */
	return 1U;
}

/* Returns the fewest samples of a mask of the device's counts of at least a count (the caller checked it is at most the most). */
static uint32_t
framebuffer_samples_for(
	uint32_t mask,
	uint32_t samples)
{
	uint32_t count;

	/* The counts in order, from 2. */
	for (count = 2U; count <= 8U; count *= 2U) {
		if (count >= samples && (mask & count) != 0U)
			return count;
	}

	/* None (the most is below the count): one. */
	return 1U;
}

/* Returns the bound renderbuffer, recording the error when the target is wrong or none is bound. */
static struct gles_renderbuffer *
renderbuffer_bound(
	struct zegl_context *context,
	GLenum target)
{
	struct gles_state *state;
	struct gles_renderbuffer *renderbuffer;

	/* A context with its state and the one target. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;
	if (target != GL_RENDERBUFFER) {
		gles_error(context, GL_INVALID_ENUM);
		return NULL;
	}

	/* The bound one (renderbuffer 0 is not one). */
	renderbuffer = NULL;
	if (state->renderbuffer != 0U)
		renderbuffer = gles_names_get(&state->renderbuffers, state->renderbuffer);
	if (renderbuffer == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the renderbuffer. */
	return renderbuffer;
}

/*
 * Chooses the image format of a renderbuffer of an internal format: a
 * colour or depth format a framebuffer draws into (format.c), or for
 * GL_STENCIL_INDEX8 the device's stencil format.  Returns 0, or -1 when
 * the internal format is not one or the device has none.
 */
static int
renderbuffer_format(
	struct gles_state *state,
	GLenum internal,
	struct gles_renderbuffer *renderbuffer)
{
	const struct gles_format *kept;
	uint32_t features;

	/* Stencil only: an 8-bit stencil format, else the device's depth and stencil one. */
	if (internal == GL_STENCIL_INDEX8) {
		features = gles_image_features(state, VK_FORMAT_S8_UINT);
		renderbuffer->kept = &framebuffer_stencil;
		renderbuffer->depth = 1;
		renderbuffer->vk = VK_FORMAT_S8_UINT;
		renderbuffer->aspects = VK_IMAGE_ASPECT_STENCIL_BIT;
		if ((features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U)
			return 0;

		/* The device's depth and stencil format, asked for once, with stencil. */
		if (state->depth_format == VK_FORMAT_UNDEFINED)
			state->depth_format = zegl_depth_format(state->display, &state->depth_aspects);
		if ((state->depth_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) == 0U)
			return -1;
		renderbuffer->vk = state->depth_format;
		renderbuffer->aspects = state->depth_aspects;
		return 0;
	}

	/* A format of the table a framebuffer draws into. */
	kept = gles_format_renderable(state, internal);
	if (kept == NULL || kept->legacy)
		return -1;

	/* Succeeded: its Vulkan format and aspects. */
	renderbuffer->kept = kept;
	renderbuffer->vk = kept->vk;
	renderbuffer->depth = 0;
	renderbuffer->aspects = VK_IMAGE_ASPECT_COLOR_BIT;
	if (kept->kind == GLES_TEXEL_DEPTH) {
		renderbuffer->depth = 1;
		renderbuffer->aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (kept->stencil)
			renderbuffer->aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}

	/* Succeeded: the format is chosen. */
	return 0;
}

/*
 * Returns the usage a renderbuffer's image is made with: drawn into as a
 * colour or a depth and stencil attachment, and copied from and into (a
 * depth format only when its features say it may be: a blit of the
 * depth).
 */
static VkImageUsageFlags
renderbuffer_usage(
	struct gles_state *state,
	const struct gles_renderbuffer *renderbuffer)
{
	VkImageUsageFlags usage;
	uint32_t features;

	/* A colour attachment, copied either way. */
	if (!renderbuffer->depth)
		return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	/* A depth and stencil attachment, copied as its format's features allow. */
	features = gles_image_features(state, renderbuffer->vk);
	usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if ((features & (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT)) != 0U)
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if ((features & (VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) != 0U)
		usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	/* Succeeded: the usage. */
	return usage;
}

/*
 * Makes a renderbuffer's device image and view, in the layout it rests in
 * as an attachment.  Returns 0, 1 when the device does not make such an
 * image (the renderbuffer is left without one), or -1 when it refused.
 */
static int
renderbuffer_image(
	struct gles_state *state,
	struct gles_renderbuffer *renderbuffer)
{
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkImageMemoryBarrier barrier;
	uint32_t features;
	uint32_t type;
	VkResult result;
	int status;

	/* Drawn into and copied. */
	renderbuffer->usage = renderbuffer_usage(state, renderbuffer);

	/* The image, drawn into (and copied). */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = renderbuffer->vk;
	create.extent.width = (uint32_t)renderbuffer->width;
	create.extent.height = (uint32_t)renderbuffer->height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = 1U;
	create.samples = (VkSampleCountFlagBits)renderbuffer->samples;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = renderbuffer->usage;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	/*
	 * A multisampled depth or stencil buffer is sampled too, where the
	 * device samples its format of its samples: a blit resolves it with a
	 * draw that reads each pixel's first sample (framebuffer_resolve_depth).
	 */
	features = gles_image_features(state, renderbuffer->vk);
	if (renderbuffer->depth && renderbuffer->samples > 1U && (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0U) {
		create.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		status = gles_image_supported(state, &create);
		if (!status)
			create.usage &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_SAMPLED_BIT;
		renderbuffer->usage = create.usage;
	}

	/* Storage the device does not make has no image: a framebuffer with it is unsupported. */
	status = gles_image_supported(state, &create);
	if (!status) {
		renderbuffer->image = VK_NULL_HANDLE;
		return 1;
	}
	result = vkCreateImage(state->device, &create, NULL, &renderbuffer->image);
	if (result != VK_SUCCESS) {
		renderbuffer->image = VK_NULL_HANDLE;
		return -1;
	}

	/* Its memory on the device. */
	vkGetImageMemoryRequirements(state->device, renderbuffer->image, &requirements);
	type = gles_memory_type(state, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX)
		type = gles_memory_type(state, requirements.memoryTypeBits, 0U);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	result = vkAllocateMemory(state->device, &allocate, NULL, &renderbuffer->memory);
	if (result != VK_SUCCESS) {
		renderbuffer->memory = VK_NULL_HANDLE;
		renderbuffer_discard(state, renderbuffer);
		return -1;
	}

	/* Bound. */
	result = vkBindImageMemory(state->device, renderbuffer->image, renderbuffer->memory, 0U);
	if (result != VK_SUCCESS) {
		renderbuffer_discard(state, renderbuffer);
		return -1;
	}

	/* A view of the whole image. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = renderbuffer->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = renderbuffer->vk;
	view.subresourceRange.aspectMask = renderbuffer->aspects;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	result = vkCreateImageView(state->device, &view, NULL, &renderbuffer->view);
	if (result != VK_SUCCESS) {
		renderbuffer->view = VK_NULL_HANDLE;
		renderbuffer_discard(state, renderbuffer);
		return -1;
	}

	/* Its resting layout, set by the upload queue before any pass uses it. */
	status = gles_upload_begin(state);
	if (status != 0) {
		renderbuffer_discard(state, renderbuffer);
		return -1;
	}

	/* The layout change. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	if (renderbuffer->depth) {
		barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}

	/* The whole image, on the one queue. */
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = renderbuffer->image;
	barrier.subresourceRange.aspectMask = renderbuffer->aspects;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(state->upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
			     0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* Submitted and waited for. */
	status = gles_upload_end(state);
	if (status != 0) {
		renderbuffer_discard(state, renderbuffer);
		return -1;
	}

	/* Succeeded: the renderbuffer can be attached. */
	return 0;
}

/* Lets a renderbuffer's image, view and memory go (they wait for the frame); the object stays, with no storage made. */
static void
renderbuffer_discard(
	struct gles_state *state,
	struct gles_renderbuffer *renderbuffer)
{
	/* They wait for the frame. */
	gles_throw_away(state, VK_NULL_HANDLE, renderbuffer->image, renderbuffer->view, renderbuffer->memory);

	/* Nothing made now. */
	renderbuffer->image = VK_NULL_HANDLE;
	renderbuffer->memory = VK_NULL_HANDLE;
	renderbuffer->view = VK_NULL_HANDLE;
}

/* Frees a renderbuffer: its image waits for the frame, the object goes now. */
static void
renderbuffer_free(
	struct gles_state *state,
	struct gles_renderbuffer *renderbuffer)
{
	/* The image, its view and memory, then the object. */
	renderbuffer_discard(state, renderbuffer);
	free(renderbuffer);
}

/*
 * Puts a depth and stencil format's two aspects as copies wrote them
 * (every depth, 4 bytes, then every stencil index, 1 byte) into the kept
 * form: 8 bytes a texel, the depth, the stencil index and 3 of 0.
 */
static void
framebuffer_join_stencil(
	unsigned char *kept,
	const unsigned char *copied,
	size_t texels)
{
	size_t index;

	/* Each texel. */
	for (index = 0U; index < texels; index++) {
		memcpy(kept + index * 8U, copied + index * 4U, 4U);
		kept[index * 8U + 4U] = copied[texels * 4U + index];
		memset(kept + index * 8U + 5U, 0, 3U);
	}
}
