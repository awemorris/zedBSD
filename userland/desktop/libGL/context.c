/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libGL's view of the current context's desktop GL version (WS178): the
 * window-system binding (libGLX.so) attaches its answers through the
 * private interface of zgl-glx.h, and GL (fixed.c, immediate.c) asks
 * here.  Without a binding, a context is OpenGL 1.4 with no flags and no
 * profile.
 */

#include "fixed.h"
#include "zgl-glx.h"

#include <stddef.h>

/*
 * The latest desktop GL version a context is given (zgl_glx_version_limit
 * gives it only on a device that has what it needs).
 */
#define CONTEXT_LATEST		40U

static unsigned context_three(const struct zegl_display *device);
static int context_multisample_textures(const struct zegl_display *device);
static int context_four(const struct zegl_display *device);

/*
 * The binding's answers, NULL until libGLX attaches them.
 *
 * libGLX attaches the same instance before each context it makes, from
 * any thread, and the instance lives as long as the process; the atomic
 * store and load make the pointer whole to a GL call on another thread.
 */
static const struct zgl_glx_query *context_query;

/*
 * Takes a window-system binding's answers and turns on the fixed-function
 * layer.
 */
void
zgl_glx_attach(
	const struct zgl_glx_query *query)
{
	/* The answers GL asks from now on. */
	__atomic_store_n(&context_query, query, __ATOMIC_RELEASE);

	/* The translation draws without a program through the fixed-function hooks from now on. */
	fixed_install();
}

/*
 * Returns the latest desktop GL version a context on an initialized EGL
 * display may have, up to CONTEXT_LATEST: what the display's device has
 * decides it, a version's features being there (context_three), its
 * limits being the device's whatever they are.  4.0 needs more of the
 * device (context_four); 1.4 needs nothing.
 */
unsigned
zgl_glx_version_limit(
	EGLDisplay display)
{
	const struct zegl_display *device;
	unsigned limit;
	int four;

	/* libEGL's display is its zegl_display; one not initialized has no device yet. */
	device = (const struct zegl_display *)display;
	if (device == NULL || !device->initialized)
		return ZGL_GL_LEGACY;

	/* OpenGL 1.4 or 3.0 to 3.3 by the features. */
	limit = context_three(device);
	if (limit < 33U)
		return limit;

	/* 4.0 with what it needs. */
	four = context_four(device);
	if (four)
		limit = 40U;

	/* Succeeded: no later than the library gives. */
	if (limit > CONTEXT_LATEST)
		limit = CONTEXT_LATEST;
	return limit;
}

/*
 * Returns the latest of OpenGL 1.4 and 3.0 to 3.3 a device has the
 * features of: 3.0 its transform feedback (stores from the vertex stage,
 * which OpenGL ES 3.0 needs too and the contexts are made on) and its
 * 3D and array textures (OpenGL ES 3.0's too), 3.1 its
 * buffer textures, 3.2 its geometry shaders, depth clamping and
 * multisample textures (context_multisample_textures), 3.3 its
 * dual-source blending (MAX_DUAL_SOURCE_DRAW_BUFFERS at least 1) and
 * timer queries (timestamps on the queue).  Each version needs the
 * earlier ones'; a device without 3.0's has 1.4 only.
 */
static unsigned
context_three(
	const struct zegl_display *device)
{
	int multisample;
	int texel;

	/* OpenGL 3.0's transform feedback, and its 3D and array textures. */
	if (!device->features.vertexPipelineStoresAndAtomics)
		return ZGL_GL_LEGACY;
	if (!device->volume_and_array)
		return ZGL_GL_LEGACY;

	/* 3.1's buffer textures. */
	texel = gles_texel_buffers(device, 0);
	if (!texel)
		return 30U;

	/* 3.2's geometry shaders, depth clamping and multisample textures. */
	if (!device->features.geometryShader)
		return 31U;
	if (!device->features.depthClamp)
		return 31U;
	multisample = context_multisample_textures(device);
	if (!multisample)
		return 31U;

	/* 3.3's dual-source blending and timer queries. */
	if (!device->features.dualSrcBlend)
		return 32U;
	if (device->timestamp_bits == 0U)
		return 32U;

	/* Succeeded: OpenGL 3.3's features are there. */
	return 33U;
}

/*
 * Reports whether a device reads multisample colour textures (OpenGL 3.2's
 * GL_ARB_texture_multisample, sampler2DMS): it samples colour images of a
 * count above one that every framebuffer attachment may have, so
 * GL_MAX_COLOR_TEXTURE_SAMPLES is above one (libGLESv2's
 * gles_color_texture_samples_max, whose programs of sampler2DMS a device
 * of one sample refuses: Vulkan samples such a sampler only from an image
 * of several samples).
 */
static int
context_multisample_textures(
	const struct zegl_display *device)
{
	VkPhysicalDeviceProperties properties;
	VkSampleCountFlags counts;

	/* The counts every attachment has and colour images are sampled of. */
	vkGetPhysicalDeviceProperties(device->physical, &properties);
	counts = properties.limits.framebufferColorSampleCounts;
	counts &= properties.limits.framebufferDepthSampleCounts;
	counts &= properties.limits.framebufferStencilSampleCounts;
	counts &= properties.limits.sampledImageColorSampleCounts;

	/* One above one. */
	if ((counts & (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT)) == 0U)
		return 0;

	/* Succeeded: multisample colour textures are read. */
	return 1;
}

/*
 * Reports whether a device has what OpenGL 4.0 needs of it besides 3.3's:
 * tessellation shaders, cube map arrays, per-sample shading, doubles in
 * shaders, blending each colour attachment on its own, GL_ARB_gpu_shader5's
 * gathers and computed indices, transform feedback's stores and atomic
 * counters in the vertex and geometry stages, and the statistics that
 * count the primitives geometry and tessellation shaders generate.
 */
static int
context_four(
	const struct zegl_display *device)
{
	/* The shader stages and the texture targets. */
	if (!device->features.tessellationShader)
		return 0;
	if (!device->features.imageCubeArray)
		return 0;

	/* Per-sample shading, doubles, and blending each attachment on its own. */
	if (!device->features.sampleRateShading)
		return 0;
	if (!device->features.shaderFloat64)
		return 0;
	if (!device->features.independentBlend)
		return 0;

	/* GL_ARB_gpu_shader5's gathers and computed indices. */
	if (!device->features.shaderImageGatherExtended)
		return 0;
	if (!device->features.shaderSampledImageArrayDynamicIndexing)
		return 0;
	if (!device->features.shaderUniformBufferArrayDynamicIndexing)
		return 0;

	/* Transform feedback, and the primitives generated. */
	if (!device->features.vertexPipelineStoresAndAtomics)
		return 0;
	if (!device->features.pipelineStatisticsQuery)
		return 0;

	/* Succeeded: OpenGL 4.0's needs are there. */
	return 1;
}

/*
 * Returns the desktop GL version of the calling thread's current context
 * (major * 10 + minor) and its GL_CONTEXT_FLAGS.
 */
unsigned
gl_context_version(
	GLint *flags)
{
	const struct zgl_glx_query *query;
	unsigned version;

	/* Without a binding: OpenGL 1.4, no flags. */
	query = __atomic_load_n(&context_query, __ATOMIC_ACQUIRE);
	if (query == NULL) {
		*flags = 0;
		return ZGL_GL_LEGACY;
	}

	/* The binding's answer for the thread's context. */
	version = query->version(flags);

	/* Succeeded: the version. */
	return version;
}

/*
 * Returns the GL_CONTEXT_PROFILE_MASK of the calling thread's current
 * context (0 before 3.2).
 */
GLint
gl_context_profile(void)
{
	const struct zgl_glx_query *query;
	GLint profile;

	/* Without a binding: no profile. */
	query = __atomic_load_n(&context_query, __ATOMIC_ACQUIRE);
	if (query == NULL)
		return 0;

	/* The binding's answer for the thread's context. */
	profile = query->profile();

	/* Succeeded: the profile. */
	return profile;
}
