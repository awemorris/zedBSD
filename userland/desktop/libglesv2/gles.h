/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of zedBSD's OpenGL ES 2.0 (WS068 p008): the objects and the
 * state of a context, and the translation of both into Vulkan.
 *
 * Every GL object keeps its contents on the CPU (a buffer's bytes, a
 * texture's levels as RGBA8) and a device copy that a draw brings up to
 * date when the CPU copy changed.  A device copy that a draw of the
 * frame being recorded already uses is never written again: a new one is
 * made and the old one waits in the garbage until the frame is done.
 *
 * Shaders are GLSL (compiled by glsl/, WS068 p015-p019, into SPIR-V when
 * the program links) or SPIR-V (glShaderBinary with
 * GL_SHADER_BINARY_FORMAT_SPIR_V), both in the form
 * plan/ws068/phase008/phase.md gives: the uniforms other than
 * samplers in one uniform block at set 0 binding 0, the samplers at set 0
 * from binding 1, the named uniform blocks of OpenGL ES 3 (WS068 p024,
 * read from buffer objects) at set 0 from binding 32, attributes and
 * varyings by location.  Linking reads the
 * names and locations out of the SPIR-V and rewrites the vertex shader so
 * that its gl_Position becomes Vulkan's (y turned over, z from [-w, w] to
 * [0, w]).
 */

#ifndef GLES_H
#define GLES_H

#include "../libegl/zegl.h"

#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

#include <stddef.h>
#include <stdint.h>

/* The SPIR-V binary format (GL 4.6's value; OpenGL ES has none of its own). */
#ifndef GL_SHADER_BINARY_FORMAT_SPIR_V
#define GL_SHADER_BINARY_FORMAT_SPIR_V	0x9551
#endif

/* Desktop GL's primitive modes, which only the fixed-function layer's draws take. */
#ifndef GL_QUADS
#define GL_QUADS		0x0007
#define GL_QUAD_STRIP		0x0008
#define GL_POLYGON		0x0009
#endif

/* Desktop GL's occlusion query that counts samples, and GL 3.0's conditional rendering modes (libGL). */
#ifndef GL_SAMPLES_PASSED
#define GL_SAMPLES_PASSED	0x8914
#endif
#ifndef GL_QUERY_WAIT
#define GL_QUERY_WAIT		0x8E13
#define GL_QUERY_NO_WAIT	0x8E14
#define GL_QUERY_BY_REGION_WAIT	0x8E15
#define GL_QUERY_BY_REGION_NO_WAIT 0x8E16
#endif

/* Desktop GL 3.3's timer queries (libGL): the elapsed time of commands, the GPU's time, and the bits of the counters. */
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED		0x88BF
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP		0x8E28
#endif
#ifndef GL_QUERY_COUNTER_BITS
#define GL_QUERY_COUNTER_BITS	0x8864
#endif

/* Desktop GL 3.3's blending with the fragment shader's second colour (libGL). */
#ifndef GL_SRC1_COLOR
#define GL_SRC1_COLOR			0x88F9
#define GL_ONE_MINUS_SRC1_COLOR		0x88FA
#define GL_ONE_MINUS_SRC1_ALPHA		0x88FB
#define GL_MAX_DUAL_SOURCE_DRAW_BUFFERS	0x88FC
#endif
#ifndef GL_SRC1_ALPHA
#define GL_SRC1_ALPHA			0x8589
#endif

/* Desktop GL 4.0's per-sample shading (libGL). */
#ifndef GL_SAMPLE_SHADING
#define GL_SAMPLE_SHADING		0x8C36
#endif
#ifndef GL_MIN_SAMPLE_SHADING_VALUE
#define GL_MIN_SAMPLE_SHADING_VALUE	0x8C37
#endif

/* Desktop GL 4.0's limits of geometry shader invocations and interpolation offsets (libGL; GL_ARB_gpu_shader5). */
#ifndef GL_MAX_GEOMETRY_SHADER_INVOCATIONS
#define GL_MAX_GEOMETRY_SHADER_INVOCATIONS 0x8E5A
#endif
#ifndef GL_MIN_FRAGMENT_INTERPOLATION_OFFSET
#define GL_MIN_FRAGMENT_INTERPOLATION_OFFSET 0x8E5B
#endif
#ifndef GL_MAX_FRAGMENT_INTERPOLATION_OFFSET
#define GL_MAX_FRAGMENT_INTERPOLATION_OFFSET 0x8E5C
#endif
#ifndef GL_FRAGMENT_INTERPOLATION_OFFSET_BITS
#define GL_FRAGMENT_INTERPOLATION_OFFSET_BITS 0x8E5D
#endif

/* Desktop GL 4.0's double uniform types (libGL; GL_ARB_gpu_shader_fp64). */
#ifndef GL_DOUBLE
#define GL_DOUBLE		0x140A
#endif
#ifndef GL_DOUBLE_VEC2
#define GL_DOUBLE_VEC2		0x8FFC
#endif
#ifndef GL_DOUBLE_VEC3
#define GL_DOUBLE_VEC3		0x8FFD
#endif
#ifndef GL_DOUBLE_VEC4
#define GL_DOUBLE_VEC4		0x8FFE
#endif
#ifndef GL_DOUBLE_MAT2
#define GL_DOUBLE_MAT2		0x8F46
#endif
#ifndef GL_DOUBLE_MAT3
#define GL_DOUBLE_MAT3		0x8F47
#endif
#ifndef GL_DOUBLE_MAT4
#define GL_DOUBLE_MAT4		0x8F48
#endif
#ifndef GL_DOUBLE_MAT2x3
#define GL_DOUBLE_MAT2x3	0x8F49
#endif
#ifndef GL_DOUBLE_MAT2x4
#define GL_DOUBLE_MAT2x4	0x8F4A
#endif
#ifndef GL_DOUBLE_MAT3x2
#define GL_DOUBLE_MAT3x2	0x8F4B
#endif
#ifndef GL_DOUBLE_MAT3x4
#define GL_DOUBLE_MAT3x4	0x8F4C
#endif
#ifndef GL_DOUBLE_MAT4x2
#define GL_DOUBLE_MAT4x2	0x8F4D
#endif
#ifndef GL_DOUBLE_MAT4x3
#define GL_DOUBLE_MAT4x3	0x8F4E
#endif

/* Desktop GL 4.0's transform feedback limits (libGL; GL_ARB_transform_feedback3). */
#ifndef GL_MAX_TRANSFORM_FEEDBACK_BUFFERS
#define GL_MAX_TRANSFORM_FEEDBACK_BUFFERS 0x8E70
#endif
#ifndef GL_MAX_VERTEX_STREAMS
#define GL_MAX_VERTEX_STREAMS		0x8E71
#endif

/* Desktop GL 3.0's count of the primitives generated (libGL). */
#ifndef GL_PRIMITIVES_GENERATED
#define GL_PRIMITIVES_GENERATED		0x8C87
#endif

/* Desktop GL 4.0's subroutines (libGL; GL_ARB_shader_subroutine). */
#ifndef GL_ACTIVE_SUBROUTINES
#define GL_ACTIVE_SUBROUTINES		0x8DE5
#define GL_ACTIVE_SUBROUTINE_UNIFORMS	0x8DE6
#define GL_MAX_SUBROUTINES		0x8DE7
#define GL_MAX_SUBROUTINE_UNIFORM_LOCATIONS 0x8DE8
#define GL_ACTIVE_SUBROUTINE_UNIFORM_LOCATIONS 0x8E47
#define GL_ACTIVE_SUBROUTINE_MAX_LENGTH	0x8E48
#define GL_ACTIVE_SUBROUTINE_UNIFORM_MAX_LENGTH 0x8E49
#define GL_NUM_COMPATIBLE_SUBROUTINES	0x8E4A
#define GL_COMPATIBLE_SUBROUTINES	0x8E4B
#endif

/* Desktop GL 4.0's tessellation (libGL; OpenGL ES 3.1's headers do not have it). */
#ifndef GL_PATCHES
#define GL_PATCHES			0x000E
#endif
#ifndef GL_TESS_CONTROL_SHADER
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
#define GL_MAX_TESS_CONTROL_UNIFORM_COMPONENTS 0x8E7F
#define GL_MAX_TESS_EVALUATION_UNIFORM_COMPONENTS 0x8E80
#define GL_MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS 0x8E81
#define GL_MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS 0x8E82
#define GL_MAX_TESS_CONTROL_OUTPUT_COMPONENTS 0x8E83
#define GL_MAX_TESS_PATCH_COMPONENTS	0x8E84
#define GL_MAX_TESS_CONTROL_TOTAL_OUTPUT_COMPONENTS 0x8E85
#define GL_MAX_TESS_EVALUATION_OUTPUT_COMPONENTS 0x8E86
#define GL_TESS_EVALUATION_SHADER	0x8E87
#define GL_TESS_CONTROL_SHADER		0x8E88
#define GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS 0x8E89
#define GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS 0x8E8A
#define GL_MAX_TESS_CONTROL_INPUT_COMPONENTS 0x886C
#define GL_MAX_TESS_EVALUATION_INPUT_COMPONENTS 0x886D
#endif

/* Desktop GL 3.0's context state (libGL answers them). */
#ifndef GL_CONTEXT_FLAGS
#define GL_CONTEXT_FLAGS	0x821E
#endif
#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK	0x9126
#endif

/* Desktop GL 3.1 and 3.2's rectangle and buffer textures, primitive restart, depth clamp, seamless cube maps and provoking vertex (libGL). */
#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE		0x84F5
#define GL_TEXTURE_BINDING_RECTANGLE	0x84F6
#define GL_MAX_RECTANGLE_TEXTURE_SIZE	0x84F8
#define GL_SAMPLER_2D_RECT		0x8B63
#define GL_SAMPLER_2D_RECT_SHADOW	0x8B64
#define GL_INT_SAMPLER_2D_RECT		0x8DCD
#define GL_UNSIGNED_INT_SAMPLER_2D_RECT	0x8DD5
#endif
#ifndef GL_TEXTURE_BUFFER
#define GL_TEXTURE_BUFFER		0x8C2A
#define GL_MAX_TEXTURE_BUFFER_SIZE	0x8C2B
#define GL_TEXTURE_BINDING_BUFFER	0x8C2C
#define GL_TEXTURE_BUFFER_DATA_STORE_BINDING 0x8C2D
#define GL_SAMPLER_BUFFER		0x8DC2
#define GL_INT_SAMPLER_BUFFER		0x8DD0
#define GL_UNSIGNED_INT_SAMPLER_BUFFER	0x8DD8
#endif
#ifndef GL_PRIMITIVE_RESTART
#define GL_PRIMITIVE_RESTART		0x8F9D
#define GL_PRIMITIVE_RESTART_INDEX	0x8F9E
#endif
#ifndef GL_DEPTH_CLAMP
#define GL_DEPTH_CLAMP			0x864F
#endif
#ifndef GL_TEXTURE_CUBE_MAP_SEAMLESS
#define GL_TEXTURE_CUBE_MAP_SEAMLESS	0x884F
#endif
#ifndef GL_GEOMETRY_SHADER
#define GL_GEOMETRY_SHADER		0x8DD9
#define GL_GEOMETRY_VERTICES_OUT	0x8916
#define GL_GEOMETRY_INPUT_TYPE		0x8917
#define GL_GEOMETRY_OUTPUT_TYPE		0x8918
#define GL_LINES_ADJACENCY		0x000A
#define GL_LINE_STRIP_ADJACENCY		0x000B
#define GL_TRIANGLES_ADJACENCY		0x000C
#define GL_TRIANGLE_STRIP_ADJACENCY	0x000D
#define GL_MAX_GEOMETRY_OUTPUT_VERTICES	0x8DE0
#define GL_MAX_GEOMETRY_UNIFORM_COMPONENTS 0x8DDF
#define GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS 0x8C29
#define GL_MAX_GEOMETRY_INPUT_COMPONENTS 0x9123
#define GL_MAX_GEOMETRY_OUTPUT_COMPONENTS 0x9124
#endif
#ifndef GL_SAMPLER_2D_MULTISAMPLE
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
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_LAYERED
#define GL_FRAMEBUFFER_ATTACHMENT_LAYERED	0x8DA7
#define GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS	0x8DA8
#endif
#ifndef GL_MAX_PROGRAM_TEXTURE_GATHER_COMPONENTS_ARB
#define GL_MAX_PROGRAM_TEXTURE_GATHER_COMPONENTS_ARB 0x8F9F
#endif
#ifndef GL_TEXTURE_CUBE_MAP_ARRAY
#define GL_TEXTURE_CUBE_MAP_ARRAY	0x9009
#define GL_TEXTURE_BINDING_CUBE_MAP_ARRAY 0x900A
#define GL_SAMPLER_CUBE_MAP_ARRAY	0x900C
#define GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW 0x900D
#define GL_INT_SAMPLER_CUBE_MAP_ARRAY	0x900E
#define GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY 0x900F
#endif
#ifndef GL_CLIP_ORIGIN
#define GL_LOWER_LEFT			0x8CA1
#define GL_UPPER_LEFT			0x8CA2
#define GL_CLIP_ORIGIN			0x935C
#define GL_CLIP_DEPTH_MODE		0x935D
#define GL_NEGATIVE_ONE_TO_ONE		0x935E
#define GL_ZERO_TO_ONE			0x935F
#endif
#ifndef GL_PROVOKING_VERTEX
#define GL_FIRST_VERTEX_CONVENTION	0x8E4D
#define GL_LAST_VERTEX_CONVENTION	0x8E4E
#define GL_PROVOKING_VERTEX		0x8E4F
#endif

/* How many vertex attributes, texture units and texture levels a context has. */
#define GLES_ATTRIBS		16U
#define GLES_UNITS		16U
#define GLES_LEVELS		15U

/*
 * The buffer objects' device copies kept for reuse (ws101-p017): at most
 * GLES_SPARES of them and GLES_SPARE_BYTES in all; one not reused within
 * GLES_SPARE_FRAMES frames is freed.  A buffer object's bytes of
 * GLES_ON_DEVICE_MIN or more live in its device copy
 * (gles_buffer.on_device); smaller ones stay on the CPU and are copied.
 */
#define GLES_SPARES		8U
#define GLES_SPARE_BYTES	(64UL * 1024UL * 1024UL)
#define GLES_SPARE_FRAMES	8U
#define GLES_ON_DEVICE_MIN	(64UL * 1024UL)

/* The faces of a cube map (a 2D texture uses the first). */
#define GLES_FACES		6U

/* The shapes of textures a unit binds and a sampler reads: 2D, cube map, 3D, 2D array. */
#define GLES_SHAPE_2D		0U
#define GLES_SHAPE_CUBE		1U
#define GLES_SHAPE_3D		2U
#define GLES_SHAPE_ARRAY	3U
#define GLES_SHAPES		4U

/*
 * Desktop GL's shapes (libGL): a rectangle texture (a 2D image read with
 * coordinates in texels, black as a 2D texture) and a buffer texture (a
 * buffer object read as texels).  They come after the shapes of the black
 * textures.
 */
#define GLES_SHAPE_RECT		4U
#define GLES_SHAPE_BUFFER	5U

/* Desktop GL 3.2's multisample textures (read by texelFetch of one sample; black as their own kind). */
#define GLES_SHAPE_MS		6U

/* Desktop GL 4.0's cube map arrays (a 2D array of layer-faces viewed as cubes; black as their own kind). */
#define GLES_SHAPE_CUBE_ARRAY	7U

/* Desktop GL's clip control in a pipeline's key (gles_raster.clip_control): the upper-left origin, the depth range from 0. */
#define GLES_CLIP_UPPER_LEFT	0x1U
#define GLES_CLIP_ZERO_TO_ONE	0x2U

/* The most vertices of a patch (gl_MaxPatchVertices, the size of the GLSL compiler's unsized tessellation inputs; at most the device's). */
#define GLES_MAX_PATCH_VERTICES	32

/* The largest 3D texture side and the most layers of a 2D array texture (at most the device's). */
#define GLES_MAX_3D_SIZE	2048
#define GLES_MAX_LAYERS		2048

/* How many of Vulkan's core formats the vertex format cache covers. */
#define GLES_FORMATS		192U

/*
 * The stages a program has subroutines in (the GLSL compiler's
 * GLSL_STAGE_*: vertex, fragment, geometry, compute, tessellation control
 * and evaluation), and the most subroutines and subroutine uniform
 * locations of a stage (GL's least maximums, desktop GL 4.0).
 */
#define GLES_STAGES		6U
#define GLES_SUBROUTINES	256U
#define GLES_SUBROUTINE_LOCATIONS 1024U

/* The most uniform blocks besides the default one a program has (at bindings 32 on), and a stage reads. */
#define GLES_NAMED_BLOCKS	24U
#define GLES_STAGE_BLOCKS	12U

/* The binding of a program's first named uniform block (the default block is 0, the samplers 1 to 16). */
#define GLES_FIRST_BLOCK_BINDING 32U

/* How many indexed uniform buffer binding points a context has, and transform feedback buffer ones. */
#define GLES_UNIFORM_BINDINGS	24U
#define GLES_FEEDBACK_BINDINGS	4U

/*
 * Desktop GL 4.0's vertex streams (GL_ARB_transform_feedback3, libGL):
 * the count GL_ARB_gpu_shader5 requires.  A vertex shader captures into
 * stream 0; a geometry shader into each stream it emits on.
 */
#define GLES_VERTEX_STREAMS	4U

/*
 * Transform feedback: the most outputs a program captures, the binding of
 * the storage buffer its vertex shader writes them into and the words of
 * that buffer's header (glsl.h's GLSL_CAPTURE_BINDING and _HEADER), and
 * the most components captured into one buffer.
 */
#define GLES_CAPTURES		16U
#define GLES_CAPTURE_BINDING	48U
#define GLES_CAPTURE_HEADER	4U
#define GLES_CAPTURE_COMPONENTS	64U

/*
 * Shader storage buffers (OpenGL ES 3.1's compute, ws101-p009): how many
 * indexed binding points a context has (OpenGL ES 3.1's minimum, which
 * fits the four storage buffers a stage of the i915 device reads), and
 * the descriptor binding of binding point 0 (glsl.h's
 * GLSL_STORAGE_FIRST_BINDING; binding point n is at this more n).
 */
#define GLES_STORAGE_BINDINGS	4U
#define GLES_STORAGE_FIRST_BINDING 56U

/*
 * Image load and store (OpenGL ES 3.1, compute shaders only): the most
 * image units a context has (glsl.h's GLSL_IMAGE_UNITS; a context offers
 * as many as the device's storage images allow), and the descriptor
 * binding of unit 0 (glsl.h's GLSL_IMAGE_FIRST_BINDING; unit n is at this
 * more n).
 */
#define GLES_IMAGE_UNITS	8U
#define GLES_IMAGE_FIRST_BINDING 64U

/* The longest name of an attribute or a uniform, with its terminator. */
#define GLES_NAME		64U

/* The size of one piece of the per-frame stream memory. */
#define GLES_STREAM_CHUNK	(4U * 1024U * 1024U)

/* The kinds of objects in the shader and program namespace. */
#define GLES_KIND_SHADER	1
#define GLES_KIND_PROGRAM	2

/* The formats an image unit takes (OpenGL ES 3.1's 13). */
#define GLES_IMAGE_FORMATS	13U

/*
 * What glBindImageTexture bound to an image unit (OpenGL ES 3.1): the
 * texture (NULL: none), its level, every layer or one, the access and the
 * format.
 */
struct gles_image_unit {
	struct gles_texture *texture;
	GLint level;
	GLboolean layered;
	GLint layer;
	GLenum access;
	GLenum format;
};

/*
 * A buffer object: its bytes on the CPU, and the device buffer draws read.
 */
struct gles_buffer {
	/* The GL name, the bytes and their size, and the usage the application gave. */
	GLuint name;
	unsigned char *data;
	size_t size;
	GLenum usage;

	/* Nonzero when the bytes changed since the device copy was written. */
	int dirty;

	/* Nonzero when the device wrote the device copy (transform feedback) since the bytes were read back from it. */
	int gpu_written;

	/*
	 * Nonzero when the bytes are the device copy's mapping itself
	 * (ws101-p017: glBufferData of a large buffer writes them there, and
	 * nothing is copied to or from the device); a write while this frame
	 * reads the copy moves the bytes back to the CPU first
	 * (gles_buffer_writable).
	 */
	int on_device;

	/* The device copy (host visible), its size, where it is mapped, and the frame that last drew from it. */
	VkBuffer buffer;
	VkDeviceMemory memory;
	size_t device_size;
	void *mapped;
	uint64_t used;

	/*
	 * The application's mapping (glMapBufferRange): nonzero while the
	 * bytes are mapped, the access bits, and the range.  The mapping is
	 * the CPU bytes themselves; unmapping (or flushing a range) marks the
	 * device copy stale.
	 */
	int map_active;
	GLbitfield map_access;
	size_t map_offset;
	size_t map_length;
};

/*
 * A range of a buffer bound to an indexed binding point
 * (glBindBufferBase, glBindBufferRange).
 */
struct gles_buffer_range {
	/* The buffer (NULL: none), and the range: size 0 is the whole buffer from the offset (glBindBufferBase). */
	struct gles_buffer *buffer;
	size_t offset;
	size_t size;
};

/* The kinds of texels a texture format holds, which decide how a shader samples it. */
#define GLES_TEXEL_NORM		0U
#define GLES_TEXEL_FLOAT	1U
#define GLES_TEXEL_INT		2U
#define GLES_TEXEL_UINT		3U
#define GLES_TEXEL_DEPTH	4U

/* How many kinds of black textures there are: float, int, uint and depth ones, for each shape of texture. */
#define GLES_BLACK_KINDS	4U

/*
 * A format a texture level is kept in (format.c): the internal format the
 * application named, and the Vulkan format whose buffer-copy layout the
 * level's texels have on the CPU.  The entries are static and never
 * change.
 */
struct gles_format {
	/* The internal format, and its base format (GL_RED ... GL_RGBA, GL_*_INTEGER, GL_DEPTH_COMPONENT, GL_DEPTH_STENCIL, or the unsized one). */
	GLenum internal;
	GLenum base;

	/* The Vulkan format, the bytes of one kept texel, and the components kept. */
	VkFormat vk;
	unsigned bytes;
	unsigned components;

	/* GLES_TEXEL_*, whether a linear filter may read it, whether it is OpenGL ES 2's RGBA8 (texture.c converts), and whether it has stencil. */
	unsigned kind;
	int filterable;
	int legacy;
	int stencil;

	/* Whether a framebuffer object may draw into it (OpenGL ES 3.0's colour- and depth-renderable formats, and EXT_color_buffer_float's). */
	int renderable;
};

/*
 * One level of a texture, as rows from the bottom up (GL's order) in the
 * kept form of its format: one image, or for a 3D texture its slices and
 * for a 2D array texture its layers one after another.
 */
struct gles_level {
	int width;
	int height;
	unsigned char *pixels;

	/* The slices or layers (1 for a 2D texture and a cube map's face). */
	int depth;

	/* The format the texels are kept in (NULL when the level is not specified). */
	const struct gles_format *format;
};

/*
 * The state that decides how a texture is sampled: a texture's own, or a
 * sampler object's that a unit has bound in its place.
 */
struct gles_sampling {
	/* The filters and the wrap modes. */
	GLenum min_filter;
	GLenum mag_filter;
	GLenum wrap_s;
	GLenum wrap_t;
	GLenum wrap_r;

	/* The levels of detail sampled between. */
	float min_lod;
	float max_lod;

	/* The depth comparison: GL_NONE or GL_COMPARE_REF_TO_TEXTURE, and its function. */
	GLenum compare_mode;
	GLenum compare_func;
};

/*
 * A query object (glGenQueries): its target once begun, the query pool's
 * slots its segments were recorded in (one per run of draws in one render
 * pass) with the frames that recorded them, and a transform feedback
 * query's count of primitives.
 */
struct gles_query {
	GLuint name;
	GLenum target;
	int ended;
	uint32_t *slots;
	uint64_t *frames;
	unsigned slot_count;
	unsigned slot_capacity;
	GLuint primitives;

	/*
	 * A timer query's timestamps (desktop GL 3.3, libGL): the slots of the
	 * timestamp pool written (GL_TIME_ELAPSED's begin and end, or
	 * glQueryCounter's one) with the frames that wrote them, and how many
	 * were written (fewer when there was no surface to record into: the
	 * result is 0 then).
	 */
	uint32_t stamp_slots[2];
	uint64_t stamp_frames[2];
	unsigned stamp_count;
};

/*
 * A fence sync (glFenceSync): the frame being recorded when it was made,
 * in the context's list of them.
 */
struct gles_sync {
	uint64_t frame;
	struct gles_sync *next;
};

struct gles_queries;
struct gles_depth_resolve;

/*
 * A sampler object (glGenSamplers): sampling state a unit uses instead of
 * its texture's own while it is bound there.
 */
struct gles_sampler_object {
	GLuint name;
	struct gles_sampling sampling;
};

/*
 * A 2D texture, a cube map, a 3D texture or a 2D array texture: its levels
 * on the CPU, its sampling state, and the device image made from the
 * levels (a cube map's has six layers, a 2D array's a layer per layer, a
 * 3D texture's is a 3D image).
 */
struct gles_texture {
	/* The GL name and the target it was first bound to (0 before its first bind). */
	GLuint name;
	GLenum target;

	/* The levels of each face, face * GLES_LEVELS + level (a texture other than a cube map has face 0 only; width 0 when a level is not specified). */
	struct gles_level levels[GLES_FACES * GLES_LEVELS];

	/* The sampling state. */
	struct gles_sampling sampling;

	/* The levels the image is made of (the base level and the most), and the channels each read channel comes from (GL_RED ... GL_ONE). */
	GLint base_level;
	GLint max_level;
	GLenum swizzle[4];

	/* Whether glTexStorage2D or glTexStorage3D fixed the levels, and how many it made. */
	int immutable;
	GLint immutable_levels;

	/* Nonzero when the levels changed since the image was made. */
	int dirty;

	/* The device image, its memory and view, how many levels it has, the frame that last sampled it, and its format. */
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	uint32_t level_count;
	uint64_t used;
	const struct gles_format *image_format;

	/* A multisample texture's samples a pixel (1 for any other) and whether GL was asked for fixed sample locations. */
	uint32_t samples;
	GLboolean fixed_locations;

	/*
	 * A buffer texture's buffer object and texel format (glTexBuffer: the
	 * internal format, its Vulkan format, bytes and kind, 0 float, 1 int,
	 * 2 unsigned), and the view of the buffer's device copy draws read,
	 * with the device buffer and range it was made for (made again when
	 * they change).
	 */
	struct gles_buffer *texel_buffer;
	GLenum texel_internal;
	VkFormat texel_vk;
	unsigned texel_bytes;
	unsigned texel_kind;
	VkBufferView texel_view;
	VkBuffer texel_view_buffer;
	VkDeviceSize texel_view_range;

	/*
	 * The views of single levels and layers framebuffer objects draw
	 * into, made at their first use and let go with the image.
	 */
	struct gles_attach_view *attach_views;

	/*
	 * The levels framebuffer objects drew into since they were last read,
	 * by face (bit l: level l, with every layer): their texels on the CPU
	 * are stale, and are read back before the CPU changes the texture
	 * (gles_texture_fetch).
	 */
	uint32_t gpu_levels[GLES_FACES];

	/*
	 * Image load and store (OpenGL ES 3.1): whether the image can be a
	 * storage image (its format is one an image unit takes and the device
	 * stores), and whether it is in the general layout while a dispatch
	 * that reads or writes it as an image is recorded (its samplers then
	 * read it in that layout too).
	 */
	int storage;
	int general;

	/* Whether the image can be sampled (a format the device only stores cannot: draws read it as black). */
	int sampled;

	/*
	 * Whether the image was made mutable (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT:
	 * a storage image whose format an image unit takes, which a unit of
	 * another format of its size views, OpenGL ES 3.1 section 8.22), and
	 * the usage it was made with (a view of another format must have the
	 * features of each).
	 */
	int mutable_format;
	VkImageUsageFlags image_usage;

	/*
	 * A depth and stencil format's texture (OpenGL ES 3.1): whether it is
	 * read as its stencil index (GL_DEPTH_STENCIL_TEXTURE_MODE
	 * GL_STENCIL_INDEX; 0 and GL_DEPTH_COMPONENT read the depth), and the
	 * view of its image's stencil aspect samplers then read, made at its
	 * first use and let go with the image.
	 */
	GLenum stencil_mode;
	VkImageView stencil_view;
};

/*
 * A view of one level and one layer (a cube map's face, a 2D array's
 * layer) of a texture's image, which a framebuffer object draws into.
 */
struct gles_attach_view {
	uint32_t level;
	uint32_t layer;
	VkImageView view;

	/* A storage image's view (image load and store): its view type and format; 0 and VK_FORMAT_UNDEFINED for an attachment's. */
	VkImageViewType storage_type;
	VkFormat storage_format;
	struct gles_attach_view *next;
};

/* The layer of a layered attachment (glFramebufferTexture, desktop GL): every layer of the level, a geometry shader's gl_Layer choosing. */
#define GLES_LAYER_ALL		0xffffffffU

/* What an attachment point of a framebuffer object names. */
#define GLES_ATTACH_NONE		0
#define GLES_ATTACH_TEXTURE		1
#define GLES_ATTACH_RENDERBUFFER	2

/* How many colour attachments a framebuffer object has, and draw buffers a fragment shader writes. */
#define GLES_COLOR_ATTACHMENTS	4U
#define GLES_DRAW_BUFFERS	4U

/*
 * A renderbuffer: an image a framebuffer object draws into and nothing
 * samples, of the format its storage was given (format.c's table: a
 * colour format, or a depth and stencil one).
 */
struct gles_renderbuffer {
	/* The GL name, whether it has been bound (glIsRenderbuffer is false before), and the internal format its storage was given (0 before glRenderbufferStorage). */
	GLuint name;
	int bound;
	GLenum format;

	/* The size, and nonzero for a depth or stencil format. */
	int width;
	int height;
	int depth;

	/* The format its image has, the aspects of that image, and its samples per pixel (1: not multisampled). */
	const struct gles_format *kept;
	VkFormat vk;
	VkImageAspectFlags aspects;
	uint32_t samples;

	/* The device image, the usages it was made with (copies only when its format has them), its memory and view, and the frame that last drew into it. */
	VkImage image;
	VkImageUsageFlags usage;
	VkDeviceMemory memory;
	VkImageView view;
	uint64_t used;
};

/*
 * One attachment point of a framebuffer object: the kind and name of what
 * is attached, looked up at each use (a deleted object leaves the
 * framebuffer incomplete), and the part of a texture it names.
 */
struct gles_attachment {
	int kind;
	GLuint name;

	/* The face of a cube map drawn into (0 otherwise), the level, and the layer of a 2D array or the slice of a 3D texture. */
	unsigned face;
	GLint level;
	GLint layer;
};

/*
 * The formats of a render pass's attachments, by which framebuffer
 * objects' passes and the pipelines made with them are compatible.
 */
struct gles_pass_format {
	uint32_t color_count;
	uint32_t colors[GLES_COLOR_ATTACHMENTS];
	uint32_t depth;
	uint32_t samples;
};

/*
 * A 2D image a framebuffer object draws into in place of a slice of a 3D
 * texture (Vulkan 1.0 cannot view a 3D image's slice as a 2D image): the
 * slice is copied in before the object's pass and back after it.
 */
struct gles_slice_image {
	/* The 3D texture (by name), the level and the slice (none: texture 0). */
	GLuint texture;
	GLint level;
	uint32_t slice;

	/* The image, its memory and view, its format and size. */
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFormat format;
	int width;
	int height;
};

/*
 * A render pass framebuffer objects' pipelines of one set of formats are
 * made with, made at its first use and kept for the context.
 */
struct gles_compatible_pass {
	struct gles_pass_format format;
	VkRenderPass pass;
	struct gles_compatible_pass *next;
};

/*
 * A framebuffer object: its attachments and draw and read buffers, and
 * the render pass and framebuffer made for the views they named when it
 * was last drawn into (made again when a view changes).
 */
struct gles_framebuffer {
	/* The GL name, whether it has been bound (glIsFramebuffer is false before), and the colour, depth and stencil attachments. */
	GLuint name;
	int bound;
	struct gles_attachment colors[GLES_COLOR_ATTACHMENTS];
	struct gles_attachment depth;
	struct gles_attachment stencil;

	/* The attachment each draw buffer writes (GL_COLOR_ATTACHMENTi or GL_NONE), and the one reads read. */
	GLenum draw_buffers[GLES_DRAW_BUFFERS];
	GLenum read_buffer;

	/* The views the pass and framebuffer were made for (VK_NULL_HANDLE: none), the pass, the framebuffer and its size. */
	VkImageView built_colors[GLES_COLOR_ATTACHMENTS];
	VkImageView built_depth;
	VkRenderPass pass;
	VkFramebuffer framebuffer;
	VkExtent2D extent;

	/* The framebuffer's layers (a layered object's: the fewest its attachments have; 1 otherwise). */
	uint32_t layers;

	/* The formats of the pass's attachments, and the compatible pass pipelines are made with. */
	struct gles_pass_format format;
	VkRenderPass compatible;

	/* Each colour attachment's format, and the depth image's aspects (0: none). */
	const struct gles_format *color_formats[GLES_COLOR_ATTACHMENTS];
	VkImageAspectFlags depth_aspects;

	/* The images drawn into in place of 3D textures' slices, by colour attachment. */
	struct gles_slice_image slices[GLES_COLOR_ATTACHMENTS];

	/*
	 * The size, samples and sample locations it has with nothing attached
	 * (OpenGL ES 3.1's glFramebufferParameteri: complete without
	 * attachments when the width and height are not 0).
	 */
	GLint default_width;
	GLint default_height;
	GLint default_samples;
	GLint default_fixed_locations;
};

/*
 * Where a draw, a clear or a read goes: the draw surface's frame, and in
 * it the surface's own images or a framebuffer object's.
 */
struct gles_target {
	/* The surface whose frame records it, and the framebuffer object (NULL: the surface's images). */
	struct zegl_surface *surface;
	struct gles_framebuffer *fbo;

	/* The frame's command buffer, and the render pass pipelines are made with. */
	VkCommandBuffer command;
	VkRenderPass pass;

	/* The size, and nonzero when rows go down from the top (a window; a framebuffer object keeps GL's rows). */
	VkExtent2D extent;
	int flip;

	/*
	 * The colour attachments the pass has (the surface's: one), which of
	 * them the draw buffers write (bit i: attachment i), which hold
	 * integers, and which cannot blend.
	 */
	unsigned color_count;
	unsigned draw_mask;
	unsigned integer_mask;
	unsigned opaque_mask;

	/* The aspects of the depth and stencil image (0: none), and the samples per pixel of every image. */
	VkImageAspectFlags depth_aspects;
	uint32_t samples;

	/* The layers a clear covers (a layered framebuffer object's; 1 otherwise). */
	uint32_t layers;
};

/*
 * Where a read of the read framebuffer's read buffer comes from: the
 * frame it is recorded in, the image with the layer and level, the layout
 * it rests in, its format and size, and whether its rows go down from the
 * top and its bytes are BGRA (a window's).
 */
struct gles_read {
	struct zegl_surface *surface;
	VkImage image;
	uint32_t layer;
	uint32_t slice;
	uint32_t level;
	VkImageLayout layout;
	const struct gles_format *format;
	VkExtent2D extent;
	int flip;
	int swizzle;
};

struct glsl_shader;
struct gles_spirv_variable;

/*
 * A shader: its SPIR-V or its compiled GLSL, and what glGetShaderiv
 * reports about it.
 */
struct gles_shader {
	/* GLES_KIND_SHADER, the GL name, and GL_VERTEX_SHADER or GL_FRAGMENT_SHADER. */
	int kind;
	GLuint name;
	GLenum type;

	/* The SPIR-V words, once a binary or a compile gave them. */
	uint32_t *code;
	size_t words;

	/* The source glShaderSource gave (NULL when none), and the GLSL compiler's shader once it compiled. */
	char *source;
	struct glsl_shader *glsl;

	/* Whether the last compile or binary succeeded, and its log. */
	int compiled;
	char *log;

	/* How many programs have it attached, and whether glDeleteShader waits for them to let it go. */
	unsigned attached;
	int delete_pending;
};

/*
 * One active attribute of a linked program.
 */
struct gles_attribute {
	char name[GLES_NAME];
	GLenum type;
	GLint size;
	uint32_t location;
	unsigned components;
};

/*
 * One active uniform of a linked program: a leaf of the default uniform
 * block (a scalar, a vector or a matrix, or an array of one of those), a
 * sampler, or a leaf of a named uniform block (which has no location:
 * the application writes it into a buffer).
 */
struct gles_uniform {
	/* The GL name (arrays without "[0]"), its GL type and how many elements it has. */
	char name[GLES_NAME];
	GLenum type;
	GLint size;

	/* 0 float, 1 int, 2 unsigned, 3 bool, 4 double (8 bytes a component); the components of a column, and the columns (1 unless a matrix). */
	unsigned base;
	unsigned components;
	unsigned columns;

	/* Where it is in the uniform block, and the strides of its elements and columns. */
	uint32_t offset;
	uint32_t array_stride;
	uint32_t matrix_stride;

	/* For a sampler (or an array of them, ws068-p004): nonzero, its binding, and each element's texture unit glUniform1i gave. */
	int sampler;
	uint32_t binding;
	GLint units[GLES_UNITS];

	/* For an image (OpenGL ES 3.1): nonzero; each element's unit (its layout's and the next ones, which no call changes) is in units. */
	int image;

	/* The location of its first element (-1 for a named block's member). */
	GLint location;

	/* The named block a member is in (its index among the program's blocks, -1 for the default block), and whether its matrices are row-major. */
	GLint block;
	int row_major;

	/* The stages that read a uniform of the default block or a sampler (GL_VERTEX_SHADER_BIT, GL_FRAGMENT_SHADER_BIT, GL_COMPUTE_SHADER_BIT). */
	GLbitfield stages;
};

/*
 * One active named uniform block of a linked program.
 */
struct gles_block {
	/* The block's name, its binding at descriptor set 0 (32 on), and its std140 size (0 when unknown: a SPIR-V binary's). */
	char name[GLES_NAME];
	uint32_t binding;
	uint32_t size;

	/*
	 * An element of an array of blocks ("B[i]"): its element of the
	 * binding's descriptors, and how many the binding has (0 and 1 for a
	 * block that is not an array).  The program's blocks are in order of
	 * binding and element.
	 */
	uint32_t element;
	uint32_t count;

	/* The block whose members are its members (itself, or an array's first listed element, which the members name). */
	GLuint members;

	/* The stages that read it (bit 0 vertex, bit 1 fragment), and how many of the program's uniforms are its members. */
	unsigned stages;
	unsigned member_count;

	/* The indexed uniform buffer binding point it reads (glUniformBlockBinding; 0 after the link). */
	GLuint buffer_binding;
};

/*
 * One name of a linked program's capture (transform feedback): what
 * glGetTransformFeedbackVarying reports, its words in a vertex's record
 * of the capture buffer, and where they go: the binding point and the
 * first word in that buffer's vertex.  Desktop GL's gl_NextBuffer and
 * gl_SkipComponents1 to 4 (GL_ARB_transform_feedback3) are names of
 * their own without words in the record: the first moves on to the next
 * binding point, the others leave their components of the buffer as they
 * are (type GL_NONE, size the components skipped, 0 for gl_NextBuffer).
 */
struct gles_capture {
	char name[GLES_NAME];
	GLenum type;
	GLint size;
	unsigned offset;
	unsigned words;
	unsigned buffer;
	unsigned place;

	/* The vertex stream of the output (a geometry shader's; 0 for a vertex shader's and for a special name). */
	unsigned stream;
};

/*
 * A transform feedback object: whether it is active or paused, the
 * primitive mode and program of the capture, the binding points' ranges
 * (kept here while another object is bound; the bound one's are the
 * context's), and the bytes written into each range so far.  The
 * vertices captured into each vertex stream are counted from
 * glBeginTransformFeedback; glEndTransformFeedback keeps the counts for
 * glDrawTransformFeedback and glDrawTransformFeedbackStream (desktop GL
 * 4.0, libGL) until the next capture ends, and marks the object as one a
 * capture has ended in.
 */
struct gles_feedback {
	GLuint name;
	int bound;
	int active;
	int paused;
	GLenum primitive_mode;
	struct gles_program *program;
	struct gles_buffer_range ranges[GLES_FEEDBACK_BINDINGS];
	size_t written[GLES_FEEDBACK_BINDINGS];
	size_t vertices[GLES_VERTEX_STREAMS];
	size_t ended_vertices[GLES_VERTEX_STREAMS];
	int ended;
};

/*
 * One shader storage block of a linked compute program (ws101-p009): its
 * name, the binding point it reads (its descriptor binding is
 * GLES_STORAGE_FIRST_BINDING more), the bytes before its run-time array
 * (the whole block without one), and whether the shader only reads it (a
 * buffer bound to a block it may write is one the device writes).
 */
struct gles_storage {
	char name[GLES_NAME];
	GLuint binding;
	uint32_t size;
	int readonly;

	/* A run-time array's stride (0 without one: GL_BUFFER_DATA_SIZE counts one element of it). */
	uint32_t array_stride;
};

/*
 * One buffer variable of a linked compute program's shader storage
 * blocks (OpenGL ES 3.1's GL_BUFFER_VARIABLE interface): its GL name,
 * type and elements (0 for a run-time array), its offset in the block
 * and its array and matrix strides, the block's index among the
 * program's storage blocks, and the elements and stride of its top-level
 * member's array of an aggregate type (1 and 0 when there is none).
 */
struct gles_buffer_variable {
	char name[GLES_NAME];
	GLenum type;
	GLint size;
	GLint offset;
	GLint array_stride;
	GLint matrix_stride;
	GLint block;
	GLint top_size;
	GLint top_stride;
};

/*
 * One image of a linked compute program's image uniform (OpenGL ES 3.1;
 * an array of images has one an element): the image unit it reads, its
 * descriptor binding (GLES_IMAGE_FIRST_BINDING more than the uniform's
 * first unit) and its element there, the elements the binding has (set
 * in the first element's only, 0 in the others), the Vulkan format of
 * its layout, its shape (GLES_SHAPE_2D, _3D, _CUBE, _ARRAY), whether the
 * shader writes it (not readonly), and its uniform's index among the
 * program's uniforms.
 */
struct gles_image {
	GLuint unit;
	uint32_t binding;
	uint32_t element;
	uint32_t count;
	VkFormat format;
	GLenum gl_format;
	unsigned shape;
	int writes;
	unsigned uniform;
};

/*
 * One uniform location: the uniform and the element of it.
 */
struct gles_location {
	unsigned uniform;
	unsigned element;
};

/*
 * A program: its shaders, and once linked the Vulkan shaders, the layout,
 * the attributes and the uniforms with their values.
 */
struct gles_program {
	/* GLES_KIND_PROGRAM and the GL name. */
	int kind;
	GLuint name;

	/* The attached shaders (a geometry shader only in desktop GL, libGL; a compute shader alone, OpenGL ES 3.1). */
	struct gles_shader *vertex;
	struct gles_shader *fragment;
	struct gles_shader *geometry;
	struct gles_shader *compute;

	/* The tessellation control and evaluation shaders (desktop GL, libGL; both or neither). */
	struct gles_shader *tess_control;
	struct gles_shader *tess_evaluation;

	/* The locations glBindAttribLocation gave, by name, for the next link. */
	char bound_names[GLES_ATTRIBS][GLES_NAME];
	GLuint bound_locations[GLES_ATTRIBS];
	unsigned bound_count;

	/* Whether the last link succeeded, and its log. */
	int linked;
	char *log;

	/* A number no other link had, so pipelines made for an earlier link are not taken for this one. */
	uint64_t serial;

	/* The Vulkan shaders (the vertex one twice: for a window's rows, which go down, and for a framebuffer object's), the descriptor set layout and the pipeline layout. */
	VkShaderModule vertex_module;
	VkShaderModule vertex_module_fbo;
	VkShaderModule fragment_module;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;

	/*
	 * A geometry shader's modules (for a window and for a framebuffer
	 * object, which rewrite its gl_Position as the vertex shader's is
	 * rewritten without one; VK_NULL_HANDLE without one), its input and
	 * output primitives (GL's modes) and most vertices, and the stages
	 * the descriptors are read by.
	 */
	VkShaderModule geometry_module;
	VkShaderModule geometry_module_fbo;
	GLenum geometry_input;
	GLenum geometry_output;
	GLint geometry_vertices;
	VkShaderStageFlags stages;

	/* Nonzero when the geometry stage is the application's (not one the link added after a tessellation evaluation shader). */
	int geometry_own;

	/*
	 * The tessellation stages' modules (VK_NULL_HANDLE without them; the
	 * evaluation one for a window and for a framebuffer object, which
	 * rewrite its gl_Position when no geometry stage follows), and the
	 * layouts as GL names them: the output vertices, the domain, the
	 * spacing, the vertex order and the point mode.
	 */
	VkShaderModule tess_control_module;
	VkShaderModule tess_evaluation_module;
	VkShaderModule tess_evaluation_module_fbo;
	/*
	 * The last stage before the rasterizer (the geometry stage, else the
	 * tessellation evaluation stage, else the vertex stage): its code as
	 * the link left it before gl_Position was rewritten, and its modules
	 * with z kept as the shader wrote it (desktop GL's
	 * glClipControl(GL_ZERO_TO_ONE)), with y turned over (index 1, as a
	 * window's module) and not (index 0), each made at its first draw
	 * (VK_NULL_HANDLE before).
	 */
	uint32_t *last_code;
	size_t last_words;
	VkShaderModule zero_modules[2];

	GLint tess_vertices;
	GLenum tess_mode;
	GLenum tess_spacing;
	GLenum tess_order;
	GLboolean tess_point_mode;

	/* The attributes. */
	struct gles_attribute attributes[GLES_ATTRIBS];
	unsigned attribute_count;

	/* The outputs glTransformFeedbackVaryings named for the next link, and the buffer mode. */
	char feedback_names[GLES_CAPTURES][GLES_NAME];
	unsigned feedback_count;
	GLenum feedback_mode;

	/*
	 * The names the linked vertex shader captures (0: none), the words of
	 * a vertex's record, and the link's buffer mode; the binding points
	 * the capture writes (each name of separate attributes has its own,
	 * interleaved ones share one until gl_NextBuffer), and the words of a
	 * vertex in each of their buffers (skipped components included; 0 for
	 * one nothing goes into).
	 */
	struct gles_capture captures[GLES_CAPTURES];
	unsigned capture_count;
	unsigned capture_stride;
	GLenum capture_mode;
	unsigned capture_buffers;
	unsigned capture_buffer_words[GLES_FEEDBACK_BINDINGS];

	/*
	 * The stage that captures (GLSL_STAGE_VERTEX, or GLSL_STAGE_GEOMETRY:
	 * the application's geometry shader, or the one the link added after
	 * a tessellation evaluation shader), and a geometry shader's output
	 * primitive as transform feedback names its kind (GL_POINTS, GL_LINES,
	 * GL_TRIANGLES), its most vertices, its invocations of a primitive,
	 * and the vertex stream each binding point's outputs come from.
	 */
	unsigned capture_stage;
	GLenum capture_primitive;
	unsigned capture_vertices;
	unsigned capture_invocations;
	unsigned capture_buffer_streams[GLES_FEEDBACK_BINDINGS];

	/*
	 * The locations glBindFragDataLocation gave fragment shader outputs,
	 * by name, for the next link, and the indexes
	 * glBindFragDataLocationIndexed gave with them (0: the first colour;
	 * desktop GL, libGL).
	 */
	char frag_bound_names[GLES_DRAW_BUFFERS][GLES_NAME];
	GLuint frag_bound_locations[GLES_DRAW_BUFFERS];
	GLuint frag_bound_indexes[GLES_DRAW_BUFFERS];
	unsigned frag_bound_count;

	/* Whether the fragment shader has an input interpolated flat, which takes GL's provoking vertex. */
	int flat_inputs;

	/* The fragment shader's outputs, by name, location and index (glGetFragDataLocation, glGetFragDataIndex). */
	char output_names[GLES_DRAW_BUFFERS][GLES_NAME];
	GLint output_locations[GLES_DRAW_BUFFERS];
	GLint output_indexes[GLES_DRAW_BUFFERS];
	unsigned output_count;

	/* The uniforms and the locations of their elements. */
	struct gles_uniform *uniforms;
	unsigned uniform_count;
	struct gles_location *locations;
	unsigned location_count;

	/* The uniform block's values (NULL when the program has no block), its size and binding. */
	unsigned char *uniform_data;
	uint32_t uniform_size;
	uint32_t uniform_binding;

	/*
	 * The built-in uniforms libGLESv2 fills at each draw (ws068-p004):
	 * gl_ZedFragment's offset in the block (the hidden vec4 that turns
	 * gl_FragCoord.y and gl_PointCoord.t into GL's directions),
	 * gl_DepthRange's near, far and diff, and a compute program's
	 * gl_ZedImages (the hidden uint of the image units an image access
	 * may touch, OpenGL ES 3.1), -1 for one no stage reads.
	 */
	int32_t zed_fragment;
	int32_t depth_range[3];
	int32_t zed_images;

	/* The named uniform blocks, in order of binding and element. */
	struct gles_block blocks[GLES_NAMED_BLOCKS];
	unsigned block_count;

	/*
	 * A linked compute program's pipeline (VK_NULL_HANDLE for a program of
	 * the other stages), its workgroup size and its shader storage blocks
	 * (ws101-p009).
	 */
	VkPipeline compute_pipeline;
	GLint local_size[3];
	struct gles_storage storages[GLES_STORAGE_BINDINGS];
	unsigned storage_count;

	/* A linked compute program's images, an image unit each (OpenGL ES 3.1). */
	struct gles_image images[GLES_IMAGE_UNITS];
	unsigned image_count;

	/* A linked compute program's buffer variables, block by block (OpenGL ES 3.1). */
	struct gles_buffer_variable *buffer_variables;
	unsigned buffer_variable_count;

	/*
	 * What OpenGL ES 3.1's program interface queries list besides the
	 * uniforms, blocks and captures: the stages the last link had
	 * (GL_VERTEX_SHADER_BIT, GL_FRAGMENT_SHADER_BIT, GL_COMPUTE_SHADER_BIT),
	 * the inputs of the first of them and the outputs of the last, as
	 * their SPIR-V says (built-ins left out).
	 */
	GLbitfield linked_stages;
	struct gles_spirv_variable *program_inputs;
	unsigned program_input_count;
	struct gles_spirv_variable *program_outputs;
	unsigned program_output_count;

	/* Each stage's subroutines (desktop GL 4.0; NULL for a stage without any). */
	struct gles_subroutines *subroutines[GLES_STAGES];

	/*
	 * Separable programs (OpenGL ES 3.1): whether the next link makes the
	 * program separable (GL_PROGRAM_SEPARABLE), whether the last link did,
	 * the vertex and fragment shaders' sources that link had (a program
	 * pipeline links them again with another program's stage), and how
	 * many program pipeline objects hold the program (as a stage or as
	 * the active program).
	 */
	int separable;
	int linked_separable;
	char *stage_sources[2];
	unsigned pipeline_uses;

	/* Whether glDeleteProgram waits for the program to stop being current or held by a program pipeline object. */
	int delete_pending;
};

/*
 * Where a uniform or a block of the program a program pipeline object
 * draws with takes its value or binding from: a stage's program and the
 * uniform's or block's index there (NULL: nowhere, a built-in).
 */
struct gles_pipeline_source {
	struct gles_program *program;
	unsigned index;
};

/*
 * A program pipeline object (OpenGL ES 3.1): its name, whether it has
 * state (glIsProgramPipeline is false before its first bind or use), the
 * program of each stage and the active program glUniform* writes, its
 * validation status and log, and the program its draws use: the vertex
 * and fragment stages' shaders linked again together (their uniforms'
 * names that both programs have renamed in the fragment shader's), made
 * for the stages' links (their serials) and given their uniforms' values
 * and blocks' bindings at each draw.
 */
struct gles_pipeline_object {
	GLuint name;
	int bound;
	struct gles_program *vertex;
	struct gles_program *fragment;
	struct gles_program *compute;
	struct gles_program *active;
	int validated;
	char *log;
	struct gles_program *combined;
	uint64_t combined_serials[2];
	struct gles_pipeline_source *uniform_sources;
	struct gles_pipeline_source block_sources[GLES_NAMED_BLOCKS];
};

/*
 * A subroutine uniform of a program's stage (desktop GL 4.0): its name,
 * its first location and its size (its elements' locations follow), the
 * indexes of the subroutines compatible with it, and its hidden uniform:
 * the name, the place in the default block (-1 when no call reads it)
 * and the array stride, where each draw writes the indexes chosen.
 */
struct gles_subroutine_uniform {
	char *name;
	unsigned location;
	unsigned size;
	unsigned *compatible;
	unsigned compatible_count;
	char *hidden;
	int32_t offset;
	uint32_t stride;
};

/*
 * A program stage's subroutines (desktop GL 4.0): the names of its
 * subroutine functions by index, its subroutine uniforms, and how many
 * locations they have.
 */
struct gles_subroutines {
	char **names;
	unsigned count;
	struct gles_subroutine_uniform *uniforms;
	unsigned uniform_count;
	unsigned locations;
};

/*
 * One vertex attribute's array and current value.
 */
struct gles_attrib {
	/* Whether the array is enabled, and its layout. */
	int enabled;
	GLint size;
	GLenum type;
	GLboolean normalized;
	GLsizei stride;

	/* The array: an offset into a buffer object, or a pointer into the application's memory when there is none. */
	const void *pointer;
	struct gles_buffer *buffer;

	/* The value an attribute without an enabled array has (a glVertexAttribI value keeps its integers' bits here). */
	float value[4];

	/* Nonzero when the array's values are integers the shader reads as they are (glVertexAttribIPointer). */
	int integer;

	/* How many instances share one element of the array (0: every vertex has its own). */
	GLuint divisor;

	/* The type of the current value: GL_FLOAT, or GL_INT or GL_UNSIGNED_INT from glVertexAttribI. */
	GLenum value_type;

	/*
	 * The vertex attribute binding (OpenGL ES 3.1, desktop GL 4.3; an
	 * OpenGL ES 3.0 context keeps every attribute on its own binding
	 * point): the binding point the attribute reads (its own index until
	 * glVertexAttribBinding) and its offset in that point's elements, and
	 * whether its stride of 0 means 0 rather than tightly packed.  The binding point of this index: its
	 * buffer (NULL: the application's memory), offset, stride, divisor,
	 * and whether glBindVertexBuffer set its stride (0 is then 0).  The
	 * array above (pointer, buffer, stride, divisor) is the one the
	 * attribute's binding point gives it (gles_vertex_binding_apply).
	 */
	GLuint binding;
	GLuint relative_offset;
	int zero_stride;
	struct gles_buffer *binding_buffer;
	GLintptr binding_offset;
	GLsizei binding_stride;
	GLuint binding_divisor;
	int binding_exact;
};

/*
 * A vertex array object: the attributes' arrays and the element buffer
 * that a bind makes the context's.  The one bound lives in the context's
 * state (gles_state.attribs, element_buffer); the others keep theirs
 * here until they are bound again.
 */
struct gles_vertex_array {
	/* The GL name, and whether it has been bound (glIsVertexArray is false before its first bind). */
	GLuint name;
	int bound;

	/* The arrays and the element buffer, while another vertex array is bound. */
	struct gles_attrib attribs[GLES_ATTRIBS];
	struct gles_buffer *element_buffer;
};

/*
 * One piece of the stream memory: vertices, indices and uniforms written
 * for a frame, the last of which it carries (its bytes are written again
 * only once that frame is done).
 */
struct gles_chunk {
	VkBuffer buffer;
	VkDeviceMemory memory;
	unsigned char *mapped;
	size_t size;
	size_t used;
	uint64_t frame;
	int cached;
	struct gles_chunk *next;
};

/*
 * A Vulkan object that waits for the frame that uses it to be done.
 */
struct gles_garbage {
	/* A buffer or an image with its view, and their memory; a view of a buffer's texels. */
	VkBuffer buffer;
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
	VkBufferView buffer_view;

	/* A pipeline, or what a program's link made. */
	VkPipeline pipeline;
	VkPipelineLayout layout;
	VkDescriptorSetLayout set_layout;
	VkShaderModule modules[10];

	/* A framebuffer object's render pass and framebuffer. */
	VkRenderPass pass;
	VkFramebuffer framebuffer;

	/*
	 * A buffer object's device copy (ws101-p017): where its memory is
	 * mapped and its size, so that it can be kept as a spare
	 * (gles_garbage_destroy) instead of freed.
	 */
	void *mapped;
	size_t size;

	/* The frame being recorded when it was put aside, and the next object waiting. */
	uint64_t frame;
	struct gles_garbage *next;
};

/*
 * A buffer object's device copy that was freed and is kept for the next
 * glBufferData of the same size (ws101-p017): its buffer, memory (mapped)
 * and size, and the frame it was kept in.
 */
struct gles_spare {
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *mapped;
	size_t size;
	uint64_t frame;
};

/*
 * A pool of descriptor sets, and the last frame that took one from it
 * (it is reset once that frame is done).
 */
struct gles_pool {
	VkDescriptorPool pool;
	uint64_t frame;
	struct gles_pool *next;
};

/* A draw buffer's blend factors and equations (desktop GL 4.0's glBlendFunci and glBlendEquationi). */
struct gles_blend_function {
	GLenum src_rgb;
	GLenum dst_rgb;
	GLenum src_alpha;
	GLenum dst_alpha;
	GLenum equation_rgb;
	GLenum equation_alpha;
};

/*
 * The fixed-function state a pipeline is made from, besides the program
 * and the vertex layout.
 */
struct gles_raster {
	/* The primitive topology (after strips, loops and fans became lists). */
	uint32_t topology;

	/* Blending: on (bit i: colour attachment i blends), and each attachment's factors and equations (0 where it does not blend). */
	uint32_t blend;
	uint32_t blend_src_rgb[GLES_COLOR_ATTACHMENTS];
	uint32_t blend_dst_rgb[GLES_COLOR_ATTACHMENTS];
	uint32_t blend_src_alpha[GLES_COLOR_ATTACHMENTS];
	uint32_t blend_dst_alpha[GLES_COLOR_ATTACHMENTS];
	uint32_t blend_equation_rgb[GLES_COLOR_ATTACHMENTS];
	uint32_t blend_equation_alpha[GLES_COLOR_ATTACHMENTS];

	/* The colour attachments, and the channels written: four bits per attachment (attachment i's at bits 4i to 4i + 3). */
	uint32_t color_count;
	uint32_t color_mask;

	/* The depth test: on, its function, whether it writes. */
	uint32_t depth_test;
	uint32_t depth_func;
	uint32_t depth_write;

	/* The stencil test: on, and the functions and operations of each face. */
	uint32_t stencil_test;
	uint32_t stencil_func[2];
	uint32_t stencil_fail[2];
	uint32_t stencil_zfail[2];
	uint32_t stencil_zpass[2];

	/* Culling: on, which faces, which winding is the front. */
	uint32_t cull;
	uint32_t cull_mode;
	uint32_t front_face;

	/* The polygon offset: on. */
	uint32_t polygon_offset;

	/* Rasterization discarded (GL_RASTERIZER_DISCARD). */
	uint32_t discard;

	/* Depths clamped rather than clipped (desktop GL's GL_DEPTH_CLAMP, when the device can). */
	uint32_t depth_clamp;

	/* The samples a fragment may cover (desktop GL's GL_SAMPLE_MASK: its value; every sample otherwise). */
	uint32_t sample_mask;

	/* A patch list's vertices a patch (desktop GL 4.0's tessellation; 0 for another topology). */
	uint32_t patch_vertices;

	/* Per-sample shading (desktop GL's GL_SAMPLE_SHADING on a multisample target): the least fraction's float bits, 0 when off. */
	uint32_t min_sample_shading;

	/* Desktop GL's clip control (glClipControl): GLES_CLIP_UPPER_LEFT and GLES_CLIP_ZERO_TO_ONE, 0 for GL's own. */
	uint32_t clip_control;
};

/*
 * The vertex layout a pipeline is made for: one binding per active
 * attribute.
 */
struct gles_vertex_layout {
	uint32_t count;
	uint32_t locations[GLES_ATTRIBS];
	uint32_t formats[GLES_ATTRIBS];
	uint32_t strides[GLES_ATTRIBS];

	/* Each binding's input rate: VK_VERTEX_INPUT_RATE_VERTEX, or _INSTANCE for an array with a divisor. */
	uint32_t rates[GLES_ATTRIBS];
};

/*
 * Everything a pipeline is made from.
 */
struct gles_pipeline_key {
	uint64_t program;
	VkRenderPass pass;
	struct gles_raster raster;
	struct gles_vertex_layout vertex;
};

/*
 * A pipeline made for one key.
 */
struct gles_pipeline {
	struct gles_pipeline_key key;
	VkPipeline pipeline;
	struct gles_pipeline *next;
};

/*
 * The descriptor set the last draw used and what it describes, reused by
 * the next draw that describes the same (until the frame is done).
 */
struct gles_set_cache {
	VkDescriptorSet set;
	uint64_t program;
	VkBuffer block;
	uint32_t count;
	VkDescriptorImageInfo images[GLES_UNITS];

	/* The views of the buffer textures' texels read in place of images (desktop GL; VK_NULL_HANDLE for an image). */
	VkBufferView texel_views[GLES_UNITS];

	/* The named blocks' buffers. */
	uint32_t block_count;
	VkDescriptorBufferInfo blocks[GLES_NAMED_BLOCKS];
};

/*
 * A Vulkan sampler made for one set of sampling state and level count.
 */
struct gles_sampler {
	struct gles_sampling sampling;
	uint32_t levels;
	VkSampler sampler;
	struct gles_sampler *next;
};

/*
 * A name table: the objects of one GL namespace by name (name 0 is never
 * an object).
 */
struct gles_names {
	void **objects;
	GLuint capacity;
};

/*
 * The state of one context that only libGLESv2 sees.
 */
struct gles_state {
	/* The context the state is of, the display's device, its memory types and its limits. */
	struct zegl_context *context;
	struct zegl_display *display;
	VkDevice device;
	VkPhysicalDeviceMemoryProperties memory;
	VkPhysicalDeviceLimits limits;

	/* The namespaces: buffers, textures, and shaders with programs. */
	struct gles_names buffers;
	struct gles_names textures;
	struct gles_names objects;

	/* The buffers bound to GL_ARRAY_BUFFER and GL_ELEMENT_ARRAY_BUFFER (the latter the bound vertex array's). */
	struct gles_buffer *array_buffer;
	struct gles_buffer *element_buffer;

	/* The buffers bound to OpenGL ES 3's other targets: copies' source and destination, uniforms, pixels, transform feedback. */
	struct gles_buffer *copy_read_buffer;
	struct gles_buffer *copy_write_buffer;
	struct gles_buffer *uniform_buffer;
	struct gles_buffer *pixel_pack_buffer;
	struct gles_buffer *pixel_unpack_buffer;

	/* The buffer bound to desktop GL's GL_TEXTURE_BUFFER target (libGL; glTexBuffer names its buffer itself). */
	struct gles_buffer *texture_buffer;

	/* The buffer indirect draws read their commands from (GL_DRAW_INDIRECT_BUFFER: OpenGL ES 3.1, desktop GL 4.0). */
	struct gles_buffer *draw_indirect_buffer;
	struct gles_buffer *feedback_buffer;

	/* The indexed binding points of uniform buffers and of transform feedback buffers. */
	struct gles_buffer_range uniform_ranges[GLES_UNIFORM_BINDINGS];
	struct gles_buffer_range feedback_ranges[GLES_FEEDBACK_BINDINGS];

	/*
	 * The subroutine each subroutine uniform location of each stage of
	 * the current program selects (desktop GL 4.0's glUniformSubroutinesuiv;
	 * glUseProgram starts each at a subroutine compatible with it).
	 */
	GLuint subroutine_indices[GLES_STAGES][GLES_SUBROUTINE_LOCATIONS];

	/*
	 * What the device lets the context offer (gles_state decides once):
	 * its colour attachments and draw buffers (the device's, at most
	 * GLES_COLOR_ATTACHMENTS), whether it draws into a float format
	 * (GL_EXT_color_buffer_float), and whether it reads buffer textures
	 * (desktop GL 3.1) and their RGB32 formats.
	 */
	unsigned color_attachments;
	int color_buffer_float;
	int texel_buffers;
	int texel_rgb32;

	/*
	 * OpenGL ES 3.1's compute (ws101-p009): whether the context offers it
	 * (gles_state decides from the device), the buffers bound to
	 * GL_SHADER_STORAGE_BUFFER and GL_DISPATCH_INDIRECT_BUFFER, and the
	 * shader storage buffers' indexed binding points.
	 */
	int compute;
	struct gles_buffer *storage_buffer;
	struct gles_buffer *dispatch_buffer;
	struct gles_buffer_range storage_ranges[GLES_STORAGE_BINDINGS];

	/*
	 * Image load and store (OpenGL ES 3.1): how many image units the
	 * context offers (0 without compute or without the device's storage
	 * images), what glBindImageTexture bound to each, and the 1x1 textures
	 * a unit that has none an image can read is read as (by shape and
	 * format, made at their first use).
	 */
	unsigned image_units;
	struct gles_image_unit image_bindings[GLES_IMAGE_UNITS];
	struct gles_texture *image_blanks[GLES_SHAPES][GLES_IMAGE_FORMATS];

	/* The vertex attributes (the bound vertex array's). */
	struct gles_attrib attribs[GLES_ATTRIBS];

	/*
	 * The vertex array bound (0: the default one), the vertex array
	 * objects' namespace, and the default one's arrays while another is
	 * bound.
	 */
	GLuint vertex_array;
	struct gles_names vertex_arrays;
	struct gles_vertex_array default_array;

	/* Whether the largest index of its type restarts strips, loops and fans (GL_PRIMITIVE_RESTART_FIXED_INDEX). */
	int primitive_restart;

	/* Desktop GL's primitive restart (libGL): whether it is on, and the index that restarts (GL_PRIMITIVE_RESTART_INDEX). */
	int primitive_restart_any;
	GLuint restart_index;

	/*
	 * Desktop GL 4.0's tessellation (libGL): a patch's vertices
	 * (GL_PATCH_VERTICES), and the default levels a program without a
	 * control shader would take (kept for glGet; such a program does not
	 * link).
	 */
	GLint patch_vertices;
	GLfloat patch_outer[4];
	GLfloat patch_inner[2];

	/* Desktop GL's per-sample shading (GL_ARB_sample_shading, libGL): on, and the least fraction of samples shaded. */
	int sample_shading;
	GLfloat min_sample_shading;

	/*
	 * Desktop GL's clip control (GL_ARB_clip_control, libGL): where the
	 * clip space's y = 1 lands (GL_LOWER_LEFT: the top; GL_UPPER_LEFT:
	 * the bottom) and the clip space's depth range (GL_NEGATIVE_ONE_TO_ONE
	 * or GL_ZERO_TO_ONE).
	 */
	GLenum clip_origin;
	GLenum clip_depth;

	/* Desktop GL's depth clamping, seamless cube maps (always seamless) and provoking vertex (libGL). */
	int depth_clamp;
	int cube_seamless;
	GLenum provoking_vertex;

	/* The base vertex added to each index of the draw being made (glDrawElementsBaseVertex; 0 otherwise). */
	GLint base_vertex;

	/* The current program. */
	struct gles_program *program;

	/*
	 * Program pipeline objects (OpenGL ES 3.1): the names, the one bound
	 * (NULL: none; it draws while no program is current), and the program
	 * glProgramUniform* is writing (NULL outside such a call).
	 */
	struct gles_names pipeline_objects;
	struct gles_pipeline_object *pipeline;
	struct gles_program *uniform_program;

	/* The active texture unit, and each unit's 2D texture, cube map, 3D texture and 2D array texture. */
	unsigned active_unit;
	struct gles_texture *units[GLES_UNITS];
	struct gles_texture *cube_units[GLES_UNITS];
	struct gles_texture *volume_units[GLES_UNITS];
	struct gles_texture *array_units[GLES_UNITS];

	/* Each unit's rectangle texture and buffer texture (desktop GL, libGL). */
	struct gles_texture *rect_units[GLES_UNITS];
	struct gles_texture *buffer_units[GLES_UNITS];

	/* Each unit's multisample texture (desktop GL 3.2, libGL). */
	struct gles_texture *ms_units[GLES_UNITS];

	/* Each unit's cube map array texture (desktop GL 4.0, libGL, a device with imageCubeArray). */
	struct gles_texture *cube_array_units[GLES_UNITS];

	/* Desktop GL's sample mask: on (GL_SAMPLE_MASK), and the samples it keeps (glSampleMaski's word 0). */
	int sample_mask;
	GLbitfield sample_mask_value;

	/* Each unit's sampler object (NULL: its textures' own sampling), and their namespace. */
	struct gles_sampler_object *unit_samplers[GLES_UNITS];
	struct gles_names sampler_objects;

	/* The query objects' namespace, and the queries' and fence syncs' shared state (query.c; NULL until the first). */
	struct gles_names query_objects;
	struct gles_queries *queries;

	/*
	 * Conditional rendering (desktop GL 3.0, libGL): whether it is on, and
	 * whether its query saw no sample pass, so draws and clears do nothing.
	 */
	int conditional_active;
	int conditional_skip;

	/* The transform feedback objects' namespace, the default one, the one bound (never NULL), and GL_RASTERIZER_DISCARD. */
	struct gles_names feedbacks;
	struct gles_feedback default_feedback;
	struct gles_feedback *feedback;
	int rasterizer_discard;

	/* Blending. */
	int blend;
	GLenum blend_src_rgb;
	GLenum blend_dst_rgb;
	GLenum blend_src_alpha;
	GLenum blend_dst_alpha;
	GLenum blend_equation_rgb;
	GLenum blend_equation_alpha;
	float blend_color[4];

	/* The colour mask. */
	GLboolean color_mask[4];

	/*
	 * Each draw buffer's own colour mask (glColorMaski) and blending
	 * (glEnablei), used once one was set so (indexed nonzero); glColorMask,
	 * glEnable and glDisable make every buffer alike again.
	 */
	GLboolean indexed_masks[GLES_DRAW_BUFFERS][4];
	int indexed_masked;
	unsigned blend_buffers;
	int blend_indexed;

	/*
	 * Each draw buffer's own blend factors and equations (desktop GL 4.0's
	 * glBlendFunci and glBlendEquationi), used once one was set so
	 * (functions_indexed nonzero); glBlendFunc* and glBlendEquation* make
	 * every buffer alike again.  Buffer 0's are also the shared ones above.
	 */
	struct gles_blend_function indexed_functions[GLES_DRAW_BUFFERS];
	int functions_indexed;

	/* The depth test and buffer. */
	int depth_test;
	GLenum depth_func;
	GLboolean depth_mask;
	float clear_depth;
	float depth_near;
	float depth_far;

	/* The stencil test and buffer, front face first. */
	int stencil_test;
	GLenum stencil_func[2];
	GLint stencil_ref[2];
	GLuint stencil_value_mask[2];
	GLuint stencil_write_mask[2];
	GLenum stencil_fail[2];
	GLenum stencil_zfail[2];
	GLenum stencil_zpass[2];
	GLint clear_stencil;

	/* Culling. */
	int cull;
	GLenum cull_mode;
	GLenum front_face;

	/* The scissor test and box (GL's, from the bottom left). */
	int scissor_test;
	GLint scissor[4];

	/* Lines, polygon offset, dithering, and the coverage states nothing uses. */
	float line_width;
	int polygon_offset;
	float polygon_factor;
	float polygon_units;
	int dither;
	int sample_alpha_to_coverage;
	int sample_coverage;
	float sample_coverage_value;
	GLboolean sample_coverage_invert;

	/* The row alignments of glTexImage2D and glReadPixels, and the mipmap hint. */
	GLint unpack_alignment;
	GLint pack_alignment;
	GLenum mipmap_hint;

	/*
	 * OpenGL ES 3's pixel store: the rows' length in pixels (0: the
	 * width), the rows and pixels skipped, and for 3D texels the images'
	 * height in rows (0: the height) and the images skipped.
	 */
	GLint unpack_row_length;
	GLint unpack_skip_rows;
	GLint unpack_skip_pixels;
	GLint unpack_image_height;
	GLint unpack_skip_images;
	GLint pack_row_length;
	GLint pack_skip_rows;
	GLint pack_skip_pixels;

	/* Whether the device fetches vertices of each core format: 0 not asked yet, 1 yes, 2 no. */
	unsigned char vertex_formats[GLES_FORMATS];

	/* The device's optimal-tiling features of each core format, and whether it has been asked (format.c). */
	uint32_t image_features[GLES_FORMATS];
	unsigned char image_asked[GLES_FORMATS];

	/*
	 * The frame being recorded, counted from 1; objects a draw of it used
	 * carry its number.  The last frame known to be done on the GPU: a
	 * window's presented frame may still run while the next is recorded
	 * (ws068-p009), so frames after done are in flight or recording.
	 */
	uint64_t frame;
	uint64_t done;

	/* The window surface whose presented frame may still run (only compared, never followed: a closed surface waited for its frames). */
	const struct zegl_surface *flight;

	/* The upload command buffer, its pool and fence. */
	VkCommandPool upload_pool;
	VkCommandBuffer upload;
	VkFence upload_fence;

	/* The stream memory, the descriptor pools and the garbage of the frames not yet done. */
	struct gles_chunk *chunks;
	struct gles_pool *pools;
	struct gles_garbage *garbage;

	/* The buffer objects' device copies kept for reuse (ws101-p017, gles_spare). */
	struct gles_spare spares[GLES_SPARES];
	unsigned spare_count;

	/* The last draw's descriptor set. */
	struct gles_set_cache set_cache;

	/* The pipelines and samplers made so far. */
	struct gles_pipeline *pipelines;
	struct gles_sampler *samplers;

	/* The draw framebuffer and the read framebuffer (0: the surfaces'), the renderbuffer bound, and their namespaces. */
	GLuint framebuffer;
	GLuint read_framebuffer;
	GLuint renderbuffer;
	struct gles_names framebuffers;
	struct gles_names renderbuffers;

	/*
	 * The framebuffer object whose render pass is open in the draw
	 * surface's frame (NULL: none); it is closed before anything else
	 * records into the frame or the frame is submitted.
	 */
	struct gles_framebuffer *open_fbo;
	struct zegl_surface *open_surface;

	/* The render passes framebuffer objects' pipelines are made with, one per set of formats, made at their first use. */
	struct gles_compatible_pass *compatible_passes;

	/* The draw surface's draw buffer and the read surface's read buffer (GL_BACK or GL_NONE). */
	GLenum default_draw_buffer;
	GLenum default_read_buffer;

	/* The device's depth and stencil format and its aspects (renderbuffers), asked for at the first depth renderbuffer. */
	VkFormat depth_format;
	VkImageAspectFlags depth_aspects;

	/* The fixed-function layer's state (libGL), NULL until it is made. */
	void *fixed;

	/* The textures sampled where a unit has no complete texture (black), by [shape][float, int, uint, depth], made at their first use. */
	struct gles_texture *blacks[GLES_SHAPES][GLES_BLACK_KINDS];

	/*
	 * The texel buffer read where a unit has no buffer texture (four zero
	 * words), its memory, and its views as float, signed and unsigned
	 * texels, made at the first use.
	 */
	VkBuffer black_buffer;
	VkDeviceMemory black_buffer_memory;
	VkBufferView black_buffer_views[3];

	/* The multisample textures sampled where a unit has none (float, int, uint), made at their first use. */
	struct gles_texture *black_ms[3];

	/* The cube map array textures sampled where a unit has none complete (float, int, uint, depth), made at their first use. */
	struct gles_texture *black_cube_arrays[GLES_BLACK_KINDS];

	/* What a blit resolves a multisampled depth and stencil buffer with (framebuffer.c), made at the first such blit. */
	struct gles_depth_resolve *depth_resolve;
};

/*
 * The fixed-function OpenGL 1.x layer of libGL (WS069 p005), which the
 * translation reaches through these hooks; libGLESv2 has none (NULL).
 */
struct gles_fixed_hooks {
	/* The flag of a capability OpenGL ES does not have; nonzero when it is not one of the layer's either. */
	int (*capability)(struct zegl_context *context, GLenum cap, int **flag);

	/* The program for a draw without one, its uniforms written, and whether it shades flat; NULL on failure. */
	struct gles_program *(*program)(struct zegl_context *context, int *flat);

	/* A fixed-function state's values as floats; how many, 0 when the name is not one. */
	unsigned (*get)(struct zegl_context *context, GLenum pname, GLfloat *values);

	/* A string that is the layer's (GL_VERSION), or NULL. */
	const GLubyte *(*string)(GLenum name);


	/* Frees a context's fixed-function state. */
	void (*release)(struct gles_state *state);

	/* The latest desktop GLSL version the context's shaders may have (130 for OpenGL 3.0), 0 for no limit. */
	unsigned (*glsl_version)(void);

	/* How many extensions the context names (-1: OpenGL ES's list), and each one's name (glGetStringi). */
	int (*extension_count)(void);
	const char *(*extension)(GLuint index);

	/* Whether the context is a core profile's (draws need a vertex array object of the application's). */
	int (*core_profile)(void);

	/* Hears each GL error recorded (the debug output's messages). */
	void (*error)(struct zegl_context *context, GLenum error);
};

/* The fixed-function layer, NULL without one (gles.c; libGL sets it). */
extern const struct gles_fixed_hooks *gles_fixed;

/* gles.c: the context's state, errors. */
struct zegl_context *gles_context(void);
struct gles_state *gles_state(struct zegl_context *context);
void gles_error(struct zegl_context *context, GLenum error);
void gles_report(const char *what, int code);
uint64_t gles_time_begin(void);
void gles_time_end(const char *step, uint64_t started, size_t bytes);
int gles_names_add(struct gles_names *names, GLuint name, void *object);
GLuint gles_names_free(struct gles_names *names);
void *gles_names_get(struct gles_names *names, GLuint name);
void gles_names_remove(struct gles_names *names, GLuint name);

/* buffer.c: device memory, the stream, the garbage, buffer objects, vertex array objects. */
uint32_t gles_memory_type(struct gles_state *state, uint32_t bits, VkMemoryPropertyFlags flags);
int gles_device_buffer(struct gles_state *state, size_t size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory, void **mapped);
void *gles_stream(struct gles_state *state, size_t size, size_t alignment, VkBuffer *buffer, VkDeviceSize *offset);
void *gles_stream_read(struct gles_state *state, size_t size, size_t alignment, VkBuffer *buffer, VkDeviceSize *offset);
void gles_throw_away(struct gles_state *state, VkBuffer buffer, VkImage image, VkImageView view, VkDeviceMemory memory);
void gles_garbage_keep(struct gles_state *state, const struct gles_garbage *objects);
void gles_garbage_destroy(struct gles_state *state, const struct gles_garbage *objects);
void gles_collect(struct gles_state *state);
void gles_frame_finished(struct gles_state *state, int finished);
int gles_frame_retire(struct zegl_context *context, struct gles_state *state);
int gles_buffer_sync(struct gles_state *state, struct gles_buffer *buffer);
struct gles_buffer *gles_buffer_named(struct zegl_context *context, struct gles_state *state, GLuint name);
void gles_buffer_free(struct gles_state *state, struct gles_buffer *buffer);
int gles_buffer_writable(struct gles_state *state, struct gles_buffer *buffer);
void gles_spares_release(struct gles_state *state);
void gles_vertex_arrays_release(struct gles_state *state);
int gles_upload_begin(struct gles_state *state);
int gles_upload_end(struct gles_state *state);

/* texture.c: textures, samplers and sampler objects. */
int gles_texture_sync(struct gles_state *state, struct gles_texture *texture);
int gles_texture_complete(struct gles_texture *texture, const struct gles_sampling *sampling);
int gles_texture_reads_stencil(const struct gles_texture *texture);
VkImageView gles_texture_stencil_view(struct gles_state *state, struct gles_texture *texture);
VkSampler gles_sampler_get(struct gles_state *state, struct gles_texture *texture, const struct gles_sampling *sampling);
struct gles_texture *gles_texture_black(struct gles_state *state, unsigned shape, unsigned kind);
VkBufferView gles_texture_buffer_view(struct gles_state *state, struct gles_texture *texture, unsigned kind);
int gles_texel_buffers(const struct zegl_display *display, int rgb32);
struct gles_texture *gles_texture_black_cube_array(struct gles_state *state, unsigned kind, GLenum internal, GLenum type);
struct gles_texture *gles_texture_black_ms(struct gles_state *state, unsigned kind);
void gles_texture_free(struct gles_state *state, struct gles_texture *texture);
void gles_texture_define(struct gles_texture *texture, unsigned face, GLint level, int width, int height, unsigned char *pixels, const struct gles_format *format);
void gles_texture_define_volume(struct gles_texture *texture, GLint level, int width, int height, int depth, unsigned char *pixels, const struct gles_format *format);
void gles_samplers_release(struct gles_state *state);

/* format.c: texture formats and texel conversions. */
const struct gles_format *gles_format_find(struct gles_state *state, GLenum internal, GLenum type);
const struct gles_format *gles_format_rgba8(void);
uint32_t gles_image_features(struct gles_state *state, VkFormat format);
int gles_image_supported(struct gles_state *state, const VkImageCreateInfo *create);
void gles_image_unmade(VkFormat format, const char *what);
GLenum gles_texels_convert(const struct gles_format *storage, GLenum format, GLenum type, GLsizei width, GLsizei height, GLint alignment, const void *pixels, unsigned char **out);
GLenum gles_texels_from_rgba8(const struct gles_format *storage, const unsigned char *rgba, size_t count, unsigned char **out);
int gles_texels_halve(const struct gles_format *format, const unsigned char *source, int source_width, int source_height, int source_depth, int halve_depth, unsigned char **out, int *width, int *height, int *depth);
float gles_half_float(uint16_t half);
uint16_t gles_half_from_float(float value);
size_t gles_pixel_size(GLenum format, GLenum type);
const struct gles_format *gles_format_renderable(struct gles_state *state, GLenum internal);
int gles_read_format_ok(const struct gles_format *storage, GLenum format, GLenum type);
void gles_read_format(const struct gles_format *storage, GLenum *format, GLenum *type);
void gles_texels_read(const struct gles_format *storage, const unsigned char *kept, size_t count, GLenum format, GLenum type, unsigned char *out);

/* etc2.c: OpenGL ES 3.0's compressed formats (ETC2 and EAC), decoded on the CPU into their kept forms. */
const struct gles_format *gles_compressed_format(GLenum internal);
size_t gles_compressed_size(GLenum internal, GLsizei width, GLsizei height, GLsizei depth);
unsigned gles_compressed_formats(GLint *values);
void gles_compressed_decode(const struct gles_format *format, const unsigned char *data, GLsizei width, GLsizei height, unsigned char *out);

/* pixels.c: the pixel store and the pixel buffers of the application's pixels. */
GLenum gles_unpack(struct gles_state *state, GLenum format, GLenum type, GLsizei width, GLsizei height, GLsizei depth, const void *pixels, const void **packed, unsigned char **owned);
GLenum gles_pack_target(struct gles_state *state, GLenum format, GLenum type, GLsizei width, GLsizei height, void *pixels, unsigned char **base, size_t *stride);

/* framebuffer.c: framebuffer objects, renderbuffers, and the target of a draw. */
int gles_target_open(struct zegl_context *context, struct gles_state *state, const VkClearValue *clear, struct gles_target *target);
void gles_target_close(struct gles_state *state);
int gles_read_source(struct zegl_context *context, struct gles_state *state, struct gles_read *read);
const struct gles_format *gles_read_buffer_format(struct gles_state *state);
uint32_t gles_framebuffer_samples(struct gles_state *state, GLuint name);
uint32_t gles_samples_max(struct gles_state *state);
uint32_t gles_framebuffer_default_samples_max(struct gles_state *state);
uint32_t gles_integer_samples_max(struct gles_state *state);
uint32_t gles_color_texture_samples_max(struct gles_state *state);
uint32_t gles_depth_texture_samples_max(struct gles_state *state);
uint32_t gles_format_samples(struct gles_state *state, GLenum target, GLenum internalformat);
uint32_t gles_format_samples_max(struct gles_state *state, GLenum target, GLenum internalformat);
uint32_t gles_format_samples_for(struct gles_state *state, GLenum target, GLenum internalformat, uint32_t samples);
VkImageView gles_texture_attach_view(struct gles_state *state, struct gles_texture *texture, uint32_t level, uint32_t layer);
VkImageView gles_texture_storage_view(struct gles_state *state, struct gles_texture *texture, uint32_t level, uint32_t layer, VkImageViewType type, VkFormat format);
struct gles_texture *gles_texture_blank(struct gles_state *state, GLenum target, GLenum internal);
void gles_framebuffers_forget(struct gles_state *state, int kind, GLuint name);
void gles_framebuffers_release(struct gles_state *state);
int gles_texture_fetch(struct zegl_context *context, struct gles_texture *texture);

/*
 * A draw's capture buffer (transform feedback): a range of the stream the
 * vertex shader writes vertices' records into, and how many vertices an
 * instance has there.  A geometry shader's has an area of the stream for
 * each instance (the range is the first one's; instance i's is i areas
 * on), each a header and room for some records (glsl.h), and where the
 * first one is on the CPU.
 */
struct gles_capture_target {
	VkDescriptorBufferInfo buffer;
	uint32_t vertices;
	uint32_t *areas;
	VkDeviceSize area;
	uint32_t room;
};

/* feedback.c: transform feedback objects and the draws that capture. */
int gles_feedback_check(struct zegl_context *context, struct gles_state *state, GLenum mode, GLenum type, int *capturing);
int gles_feedback_prepare(struct zegl_context *context, struct gles_state *state, int capturing, uint32_t numbers, GLsizei instances, uint32_t expanded, struct gles_capture_target *capture);
void gles_feedback_record(struct zegl_context *context, struct gles_state *state, const struct gles_capture_target *capture, GLint first, GLsizei instances, const uint32_t *list, uint32_t expanded);
int gles_feedback_geometry_prepare(struct zegl_context *context, struct gles_state *state, int capturing, uint32_t expanded, GLsizei instances, struct gles_capture_target *capture);
void gles_feedback_geometry_area(const struct gles_capture_target *capture, GLsizei instance, VkDescriptorBufferInfo *area);
void gles_feedback_geometry_record(struct zegl_context *context, struct gles_state *state, const struct gles_capture_target *capture, int capturing, GLsizei instances);
int gles_buffer_fetch(struct zegl_context *context, struct gles_buffer *buffer);
void gles_feedbacks_release(struct gles_state *state);

/* query.c: query objects and fence syncs. */
void gles_queries_draw(struct gles_state *state, const struct gles_target *target);
void gles_queries_suspend(struct gles_state *state);
void gles_queries_release(struct gles_state *state);
int gles_frame_wait(struct zegl_context *context, struct gles_state *state, uint64_t frame);
void gles_query_primitives(struct gles_state *state, GLuint stream, GLuint primitives);
void gles_query_generated(struct gles_state *state, GLuint stream, GLuint primitives);
int gles_queries_generating(const struct gles_state *state);
int gles_queries_generated(struct gles_state *state, const struct gles_target *target, int shaded, uint32_t topology, uint32_t vertices, uint32_t instances, uint32_t patch_vertices);
void gles_queries_generated_end(struct gles_state *state, const struct gles_target *target);
int gles_blend_indexed(struct zegl_context *context, const struct gles_state *state, GLenum target, GLuint index, GLint64 *value);
void gles_subroutines_reset(struct gles_state *state, const struct gles_program *program);
void gles_subroutines_write(const struct gles_state *state, struct gles_program *program);
void gles_blend_function_of(const struct gles_state *state, unsigned index, struct gles_blend_function *function);
int gles_query_timestamp(struct zegl_context *context, struct gles_state *state, GLint64 *value);

/* program.c: shaders and programs. */
void gles_program_release(struct gles_state *state, struct gles_program *program);
struct gles_program *gles_program_named(struct zegl_context *context, GLuint name);
struct gles_program *gles_program_current(struct zegl_context *context, struct gles_state *state);
struct gles_program *gles_pipeline_program(struct zegl_context *context, struct gles_state *state, struct gles_pipeline_object *pipeline);
int gles_pipeline_make(struct zegl_context *context, struct gles_state *state, struct gles_pipeline_object *pipeline);
void gles_program_append_log(struct gles_program *program, const char *text);
void gles_pipeline_forget(struct gles_state *state, struct gles_pipeline_object *pipeline);
void gles_program_use(struct gles_program *program);
void gles_program_unuse(struct gles_state *state, struct gles_program *program);
void gles_pipelines_release(struct gles_state *state);
VkShaderModule gles_program_zero_module(struct gles_state *state, struct gles_program *program, int flip);
void gles_shader_release(struct gles_shader *shader);

/* draw.c: pipelines, the frame, and readback. */
void gles_pipelines_forget(struct gles_state *state, uint64_t program);
void gles_vertex_binding_apply(struct gles_state *state, GLuint binding);
void gles_bind_vertex_buffer(struct zegl_context *context, struct gles_state *state, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);
void gles_vertex_attrib_format(struct zegl_context *context, struct gles_state *state, GLuint attribindex, GLint size, GLenum type, GLboolean normalized, int integer, GLuint relativeoffset);
void gles_vertex_attrib_binding(struct zegl_context *context, struct gles_state *state, GLuint attribindex, GLuint bindingindex);
void gles_vertex_binding_divisor(struct gles_state *state, GLuint bindingindex, GLuint divisor);
GLsizei gles_vertex_binding_stride(const struct gles_state *state, GLuint bindingindex);
int gles_read_rgba(struct zegl_context *context, GLint x, GLint y, GLsizei width, GLsizei height, unsigned char *rows);
int gles_read_pixels(struct zegl_context *context, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, int clamped, unsigned char *rows);
uint32_t *gles_expand(GLenum mode, const uint32_t *indices, uint32_t first, GLsizei count, int rotate, uint32_t *expanded);
int gles_draw_blocks(struct zegl_context *context, struct gles_state *state, VkDescriptorBufferInfo *blocks);
VkDescriptorSet gles_descriptor_set(struct gles_state *state, VkDescriptorSetLayout layout);
VkDescriptorSet gles_draw_descriptors(struct gles_state *state, const struct gles_target *target, const VkDescriptorBufferInfo *blocks, const VkDescriptorBufferInfo *capture, const VkDescriptorBufferInfo *storages, const VkDescriptorImageInfo *images, uint32_t *offset);

/* compute.c: OpenGL ES 3.1's dispatches and image units. */
unsigned gles_image_units_offered(struct gles_state *state);
int gles_image_format(unsigned glsl_format, VkFormat *vk, GLenum *gl);
int gles_image_unit_format(VkFormat format);
unsigned gles_image_unit_bytes(GLenum internal);
int gles_image_unit_alias(struct gles_state *state, GLenum internal);
int gles_image_indexed(struct zegl_context *context, struct gles_state *state, GLenum target, GLuint index, GLint64 *value);
void gles_image_forget(struct gles_state *state, struct gles_texture *texture);

/*
 * What the SPIR-V of a shader says about its interface.
 */
struct gles_spirv_variable {
	char name[GLES_NAME];
	uint32_t location;
	size_t location_word;
	GLenum type;
	GLint size;
	unsigned components;

	/* Its id, and a fragment output's Index (0 without one) and the word it is in (0: no Index decoration). */
	uint32_t id;
	uint32_t index;
	size_t index_word;
};

struct gles_spirv {
	/* The stage: 0 vertex, 4 fragment, 5 compute (SPIR-V's execution models). */
	uint32_t model;

	/* The inputs and outputs by location (built-ins left out). */
	struct gles_spirv_variable inputs[GLES_ATTRIBS * 2U];
	unsigned input_count;
	struct gles_spirv_variable outputs[GLES_ATTRIBS * 2U];
	unsigned output_count;

	/* The default uniform block's leaves and samplers, its binding and size (0 when none). */
	struct gles_uniform *uniforms;
	unsigned uniform_count;
	unsigned uniform_capacity;
	uint32_t block_binding;
	uint32_t block_size;
	int has_block;

	/*
	 * The bindings of the uniform blocks other than the default one
	 * (GLSL's named blocks), their descriptors (an array of blocks' length,
	 * else 1), and their blocks' type names.
	 */
	uint32_t named_bindings[GLES_NAMED_BLOCKS];
	uint32_t named_counts[GLES_NAMED_BLOCKS];
	char named_names[GLES_NAMED_BLOCKS][GLES_NAME];
	unsigned named_count;
};

/* spirv.c: reading SPIR-V and rewriting it, GL's names of types. */
GLenum gles_gl_type(unsigned base, unsigned components, unsigned columns);
int gles_spirv_reflect(const uint32_t *code, size_t words, struct gles_spirv *out, char *log, size_t log_size);
void gles_spirv_free(struct gles_spirv *spirv);
uint32_t *gles_spirv_position(const uint32_t *code, size_t words, int flip, int half_depth, size_t *out_words);

#endif
