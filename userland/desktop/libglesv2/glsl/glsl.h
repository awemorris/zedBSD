/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's GLSL compiler (WS068 p015-p019): what libGLESv2, libGL and the
 * host tests see of it.
 *
 * A shader is compiled on its own (preprocessed, parsed and checked), and
 * the SPIR-V is made when a vertex and a fragment shader are linked
 * together, because the default uniform block must have one layout in
 * both stages.  The SPIR-V is in the form libGLESv2's translation layer
 * takes (plan/ws068/phase008/phase.md): the uniforms other than samplers
 * in one std140 block at set 0 binding 0, the samplers at set 0 from
 * binding 1, attributes and varyings by location and name.
 *
 * Nothing here depends on the GL headers, so the compiler builds on the
 * host as it is.
 */

#ifndef GLSL_H
#define GLSL_H

#include <stddef.h>
#include <stdint.h>

/* The stages a shader may be of. */
#define GLSL_STAGE_VERTEX	0U
#define GLSL_STAGE_FRAGMENT	1U
#define GLSL_STAGE_GEOMETRY	2U

/* A compute shader (GLSL ES 3.10, ws101-p008): linked alone by glsl_link_compute. */
#define GLSL_STAGE_COMPUTE	3U

/* The tessellation shaders (desktop GLSL 4.00, or GL_ARB_tessellation_shader): linked between the vertex and the geometry or fragment stage. */
#define GLSL_STAGE_TESS_CONTROL	4U
#define GLSL_STAGE_TESS_EVALUATION 5U

/* The stages of a program's SPIR-V (glsl_program.code). */
#define GLSL_STAGES		6U

/*
 * The descriptor binding of a shader storage block's GL binding n (layout(
 * binding = n) buffer B { ... }): set 0, GLSL_STORAGE_FIRST_BINDING + n
 * (0 is the default uniform block, 1 to 16 the samplers, 32 to 55 the
 * uniform blocks, 48 the transform feedback capture; ws101-p008), for n
 * below GLSL_STORAGE_BINDINGS.
 */
#define GLSL_STORAGE_FIRST_BINDING 56U
#define GLSL_STORAGE_BINDINGS	8U

/*
 * The descriptor binding of an image uniform's unit n (layout(binding = n)
 * uniform image2D ...; GLSL ES 3.10, compute shaders only): set 0,
 * GLSL_IMAGE_FIRST_BINDING + n, for n below GLSL_IMAGE_UNITS.
 */
#define GLSL_IMAGE_FIRST_BINDING 64U
#define GLSL_IMAGE_UNITS	8U

/*
 * The formats an image uniform's layout gives (GLSL ES 3.10's 13), as
 * glsl_image_info.format reports them; 0 is none.
 */
#define GLSL_IMAGE_FORMAT_RGBA32F	1U
#define GLSL_IMAGE_FORMAT_RGBA16F	2U
#define GLSL_IMAGE_FORMAT_R32F		3U
#define GLSL_IMAGE_FORMAT_RGBA8		4U
#define GLSL_IMAGE_FORMAT_RGBA8_SNORM	5U
#define GLSL_IMAGE_FORMAT_RGBA32I	6U
#define GLSL_IMAGE_FORMAT_RGBA16I	7U
#define GLSL_IMAGE_FORMAT_RGBA8I	8U
#define GLSL_IMAGE_FORMAT_R32I		9U
#define GLSL_IMAGE_FORMAT_RGBA32UI	10U
#define GLSL_IMAGE_FORMAT_RGBA16UI	11U
#define GLSL_IMAGE_FORMAT_RGBA8UI	12U
#define GLSL_IMAGE_FORMAT_R32UI		13U

/* The scalar kinds of an interface variable (the same numbers as libGLESv2's gles_uniform.base). */
#define GLSL_INFO_FLOAT		0U
#define GLSL_INFO_INT		1U
#define GLSL_INFO_UINT		2U
#define GLSL_INFO_BOOL		3U
#define GLSL_INFO_DOUBLE	4U

/*
 * The optional device features a shader may use (glsl_compile_features):
 * doubles (desktop GLSL 4.00, GL_ARB_gpu_shader_fp64; a Vulkan device's
 * shaderFloat64), and GL_ARB_gpu_shader5 in desktop GLSL 1.50 and 3.30
 * (a Vulkan device's shaderImageGatherExtended, sampleRateShading for
 * the interpolation functions, shaderSampledImageArrayDynamicIndexing and
 * shaderUniformBufferArrayDynamicIndexing).
 * glsl_compile takes them all.
 */
#define GLSL_FEATURE_FP64	0x1U
#define GLSL_FEATURE_SHADER5	0x2U
#define GLSL_FEATURES_ALL	0x3U

/* The kinds of samplers, by the dimension of the image they read. */
#define GLSL_SAMPLER_NONE	0U
#define GLSL_SAMPLER_1D		1U
#define GLSL_SAMPLER_2D		2U
#define GLSL_SAMPLER_3D		3U
#define GLSL_SAMPLER_CUBE	4U

/* Desktop GLSL 1.40's rectangle samplers (a 2D image read in texels) and buffer samplers (a texel buffer). */
#define GLSL_SAMPLER_RECT	5U
#define GLSL_SAMPLER_BUFFER	6U

/* Desktop GLSL 1.50's multisample samplers (a 2D image of several samples a pixel, read by texelFetch). */
#define GLSL_SAMPLER_MS		7U

/*
 * One active uniform of a linked program as the API reports it: a leaf of
 * the default uniform block (named the way libGLESv2's SPIR-V reflection
 * names it: "s.field", "a[1].field"), a sampler, or a leaf of a named
 * uniform block (named as the GL API names it: "Block.member" for a block
 * with an instance name, "member" for one without).
 */
struct glsl_uniform_info {
	/* The name, allocated with the program. */
	char *name;

	/* GLSL_INFO_*, the components of a column, the columns (1 unless a matrix), the elements (1 unless an array). */
	unsigned base;
	unsigned components;
	unsigned columns;
	unsigned size;

	/* GLSL_SAMPLER_* for a sampler, whether it compares depth (a shadow sampler), and whether it reads an array of layers. */
	unsigned sampler;
	unsigned shadow;
	unsigned arrayed;

	/* The named block a block member is in (its index among the program's blocks), -1 for the default block and samplers. */
	int block;

	/* A block member's std140 offset in its block, its array and matrix strides (0 when not an array or a matrix), and whether its matrices are row-major. */
	unsigned offset;
	unsigned array_stride;
	unsigned matrix_stride;
	unsigned row_major;
};

/*
 * One active named uniform block of a linked program: what the API
 * reports of it, and where the SPIR-V reads it from.
 */
struct glsl_block_info {
	/* The block's name (not its instance name), allocated with the program. */
	char *name;

	/* Its binding at descriptor set 0 in both stages' SPIR-V (32 on), and its std140 size in bytes. */
	unsigned binding;
	unsigned size;

	/*
	 * An element of an array of blocks ("B[i]"): its element of the
	 * binding's descriptors, and how many the binding has (the array's
	 * length); 0 and 1 for a block that is not an array.
	 */
	unsigned element;
	unsigned count;

	/*
	 * The block whose members are its members (its index among the
	 * program's blocks): itself, or for each element of an array of
	 * blocks the array's first listed element, which the members name.
	 */
	unsigned members;

	/* The stages that read it: bit 0 the vertex stage, bit 1 the fragment stage, bit 2 the geometry stage. */
	unsigned stages;

	/* How many of the program's uniforms are its members. */
	unsigned member_count;
};

/*
 * The storage buffer a vertex shader writes the outputs it captures into
 * (transform feedback), at descriptor set 0: words of 32 bits, the first
 * GLSL_CAPTURE_HEADER a header whose word 0 is how many vertices each
 * instance has, then each vertex's record (instance * vertices + vertex)
 * of the captured outputs one after another, each component a word (a
 * float's or an int's bits).
 *
 * A geometry shader that captures (the last stage before the fragment
 * shader; one the link makes when a tessellation evaluation shader is
 * the last), or that emits on a stream other than 0, counts the
 * primitives each stream generates there, and writes a record at each
 * vertex it emits on a stream that has captured outputs, in the order
 * its atomic counter gives: the first
 * GLSL_CAPTURE_GEOMETRY_HEADER words are the records there is room for
 * (written by the API; one more record after them takes those that do
 * not fit), the records written (the counter), the invocations that ran
 * (each takes a number of its own), a word left 0, and the primitives
 * each of the four streams generated.  A record is GLSL_CAPTURE_KEY words
 * that put it in GL's order -- the input primitive (gl_PrimitiveIDIn),
 * the invocation (gl_InvocationID), the invocation's number, and the
 * vertex's place among the invocation's (bits 0 to 23), its stream (bits
 * 24 and 25) and whether it starts a strip (bit 31) -- then the captured
 * outputs as a vertex shader's record has them (those of other streams
 * left as they are).
 */
#define GLSL_CAPTURE_BINDING	48U
#define GLSL_CAPTURE_HEADER	4U
#define GLSL_CAPTURE_GEOMETRY_HEADER 8U
#define GLSL_CAPTURE_KEY	4U

/* The words of a geometry shader's capture header: room, records written, invocations, the first stream's primitives. */
#define GLSL_CAPTURE_ROOM	0U
#define GLSL_CAPTURE_RECORDS	1U
#define GLSL_CAPTURE_INVOCATIONS 2U
#define GLSL_CAPTURE_GENERATED	4U

/* The fields of a geometry shader record's last key word: the vertex's place, its stream, a strip's first vertex. */
#define GLSL_CAPTURE_SEQUENCE_MASK 0x00ffffffU
#define GLSL_CAPTURE_STREAM_SHIFT 24U
#define GLSL_CAPTURE_STREAM_MASK 3U
#define GLSL_CAPTURE_STRIP_START 0x80000000U

/*
 * One output a linked program's vertex shader captures, as the API
 * reports it (glGetTransformFeedbackVarying) and where its words are in a
 * vertex's record.
 */
struct glsl_capture_info {
	/* The name, allocated with the program. */
	char *name;

	/* GLSL_INFO_*, the components of a column, the columns (1 unless a matrix), the elements (1 unless an array). */
	unsigned base;
	unsigned components;
	unsigned columns;
	unsigned size;

	/* Its first word in a vertex's record, and how many words it has. */
	unsigned offset;
	unsigned words;

	/* The vertex stream it goes to (a geometry shader's output's; 0 for another stage's). */
	unsigned stream;
};

/*
 * One shader storage block of a linked compute program (ws101-p008): its
 * name, its GL binding (its descriptor binding is GLSL_STORAGE_FIRST_BINDING
 * more), the bytes before its run-time array (the whole block without one),
 * a run-time array's stride (0 without one), and whether the shader only
 * reads it (readonly) -- a buffer it may write is one the device writes.
 */
struct glsl_storage_info {
	char *name;
	unsigned binding;
	unsigned size;
	unsigned array_stride;
	unsigned readonly;
};

/*
 * One buffer variable of a linked compute program's shader storage
 * blocks, as the API lists it (OpenGL ES 3.1 section 7.3.1: a leaf of a
 * block's members, an array of a basic type one variable, a top-level
 * array of an aggregate type only its first element): its name, its kind
 * as a uniform's (GLSL_INFO_*, components, columns), its elements (0 for
 * a run-time array), its std430 offset in the block, its array and
 * matrix strides (0 when not an array or not a matrix), the block's
 * index among the program's storage blocks, and the elements and stride
 * of the top-level member's array of an aggregate type (1 and 0 when the
 * member is not such an array).
 */
struct glsl_buffer_info {
	char *name;
	unsigned base;
	unsigned components;
	unsigned columns;
	unsigned size;
	unsigned offset;
	unsigned array_stride;
	unsigned matrix_stride;
	unsigned block;
	unsigned top_size;
	unsigned top_stride;
};

/*
 * One image uniform of a linked compute program: its name, its unit (the
 * layout's binding; its descriptor binding is GLSL_IMAGE_FIRST_BINDING
 * more) and how many images it is (an array's elements take the next
 * units, and are the descriptors of its one binding; 1 for one image),
 * its dimension (GLSL_SAMPLER_2D, _3D, _CUBE) and whether it is an
 * array of layers, the kind of its texels (GLSL_INFO_FLOAT, _INT, _UINT),
 * its format (GLSL_IMAGE_FORMAT_*), whether the shader only reads or only
 * writes it, and whether an atomic function uses it.
 */
struct glsl_image_info {
	char *name;
	unsigned unit;
	unsigned count;
	unsigned dimension;
	unsigned arrayed;
	unsigned base;
	unsigned format;
	unsigned readonly;
	unsigned writeonly;
	unsigned atomic;
};

/*
 * A linked program: the SPIR-V of its stages and what the API says of
 * the uniforms.  glsl_program_free releases it.
 */
struct glsl_program {
	/*
	 * The SPIR-V words of the vertex, the fragment and (NULL without one)
	 * the geometry stage, or of a compute program's one stage
	 * (GLSL_STAGE_*).
	 */
	uint32_t *code[GLSL_STAGES];
	size_t words[GLSL_STAGES];

	/* A compute program's workgroup size (ws101-p008); 0s for another. */
	unsigned local_size[3];

	/* A compute program's shader storage blocks, by binding (ws101-p008). */
	struct glsl_storage_info *storages;
	unsigned storage_count;

	/* A compute program's image uniforms, in declaration order. */
	struct glsl_image_info *images;
	unsigned image_count;

	/* A compute program's buffer variables, block by block. */
	struct glsl_buffer_info *buffers;
	unsigned buffer_count;

	/* The active uniforms: the default block's leaves and the samplers, then the named blocks' members. */
	struct glsl_uniform_info *uniforms;
	unsigned uniform_count;

	/* The active named uniform blocks, in the order of their bindings (an array's elements in order). */
	struct glsl_block_info *blocks;
	unsigned block_count;

	/* The outputs the vertex shader captures (transform feedback), and the words of a vertex's record (0: none). */
	struct glsl_capture_info *captures;
	unsigned capture_count;
	unsigned capture_stride;

	/*
	 * The stage that captures them (GLSL_STAGE_VERTEX, or
	 * GLSL_STAGE_GEOMETRY: a geometry shader's records; a geometry shader
	 * that emits on a stream other than 0 counts its primitives in a
	 * capture buffer even with nothing captured); a geometry shader's
	 * output primitive as transform feedback names its kind (GL_POINTS,
	 * GL_LINES, GL_TRIANGLES), the most vertices an invocation emits, and
	 * its invocations of each primitive.
	 */
	unsigned capture_stage;
	unsigned capture_primitive;
	unsigned capture_vertices;
	unsigned capture_invocations;
};

/*
 * A location glBindAttribLocation gave to an attribute name.
 */
struct glsl_binding {
	const char *name;
	unsigned location;
};

/* A compiled shader; opaque outside the compiler. */
struct glsl_shader;

/*
 * Compiles a shader of a stage.  default_version is the version of a
 * source without #version (100 for OpenGL ES, 110 for desktop GL).
 * Returns the shader, or NULL when the source has errors.  *log receives
 * a malloc'ed info log (errors, or warnings of a shader that compiled),
 * or NULL when there is nothing to say.
 */
struct glsl_shader *glsl_compile(unsigned stage, const char *source, unsigned default_version, char **log);

/*
 * Compiles a shader as glsl_compile does, for a device that has only the
 * features given (GLSL_FEATURE_*): without GLSL_FEATURE_FP64 the double
 * types, constants and functions are not there, the GL_ARB_gpu_shader_fp64
 * macro is not defined, and the extension cannot be required.
 */
struct glsl_shader *glsl_compile_features(unsigned stage, const char *source, unsigned default_version, unsigned features, char **log);

/* Frees a compiled shader. */
void glsl_shader_free(struct glsl_shader *shader);

/* Reports a compiled shader's stage. */
unsigned glsl_shader_stage(const struct glsl_shader *shader);

/* Reports a compiled shader's GLSL version (100, 110 .. 330, 300, 310) and whether it is OpenGL ES's language. */
unsigned glsl_shader_version(const struct glsl_shader *shader, int *es);

/*
 * Makes the source of a stand-in for the stage a separable program of
 * one stage does not have (a fragment shader for a vertex shader, a
 * vertex shader writing a fragment shader's inputs); malloc'ed, or NULL.
 */
char *glsl_stand_in_source(const struct glsl_shader *shader);

/*
 * Links a vertex and a fragment shader into SPIR-V.  Returns 0 and fills
 * *program, or -1 with *log a malloc'ed info log saying why.
 */
int glsl_link(const struct glsl_shader *vertex, const struct glsl_shader *fragment, const struct glsl_binding *bindings, unsigned binding_count, struct glsl_program *program, char **log);

/*
 * Links as glsl_link does, with the vertex shader capturing the outputs
 * of the names given (transform feedback, GLSL_CAPTURE_BINDING) in that
 * order.  Returns 0 and fills *program, or -1 with *log a malloc'ed info
 * log saying why.
 */
int glsl_link_captured(const struct glsl_shader *vertex, const struct glsl_shader *fragment, const struct glsl_binding *bindings, unsigned binding_count, const char *const *captures, unsigned capture_count, struct glsl_program *program, char **log);

/*
 * Links a vertex shader, a geometry shader (NULL for none) and a fragment
 * shader as glsl_link_captured does (with a geometry shader, it is the
 * one that captures).  Returns 0 and fills *program, or -1 with *log a
 * malloc'ed info log saying why.
 */
int glsl_link_stages(const struct glsl_shader *vertex, const struct glsl_shader *geometry, const struct glsl_shader *fragment, const struct glsl_binding *bindings, unsigned binding_count, const char *const *captures, unsigned capture_count, struct glsl_program *program, char **log);

/*
 * Links the stages of a draw as glsl_link_stages does, with the two
 * tessellation shaders (GLSL_STAGE_TESS_CONTROL and _EVALUATION, both NULL
 * for none) after the vertex shader.  The last stage before the fragment
 * shader captures: a vertex or geometry shader itself; for a tessellation
 * evaluation shader, a geometry shader the link adds after it that passes
 * each primitive on as it is (its SPIR-V at code[GLSL_STAGE_GEOMETRY]).
 * Returns 0 and fills *program (the tessellation stages' SPIR-V at
 * code[GLSL_STAGE_TESS_*]), or -1 with *log a malloc'ed info log saying
 * why.
 */
int glsl_link_pipeline(const struct glsl_shader *vertex, const struct glsl_shader *tess_control, const struct glsl_shader *tess_evaluation, const struct glsl_shader *geometry, const struct glsl_shader *fragment, const struct glsl_binding *bindings, unsigned binding_count, const char *const *captures, unsigned capture_count, struct glsl_program *program, char **log);

/*
 * Reports a compiled tessellation shader's layouts as GL names them: a
 * control shader's output vertices; an evaluation shader's domain
 * (GL_TRIANGLES, GL_QUADS, GL_ISOLINES), spacing (GL_EQUAL,
 * GL_FRACTIONAL_EVEN, GL_FRACTIONAL_ODD), vertex order (GL_CW, GL_CCW)
 * and point mode; 0s for what a stage does not have.
 */
void glsl_tess_layout(const struct glsl_shader *shader, unsigned *vertices, unsigned *domain, unsigned *spacing, unsigned *order, unsigned *point_mode);

/*
 * Links a compute shader (GLSL_STAGE_COMPUTE) alone into SPIR-V
 * (ws101-p008).  Returns 0 and fills *program, or -1 with *log a
 * malloc'ed info log saying why.
 */
int glsl_link_compute(const struct glsl_shader *compute, struct glsl_program *program, char **log);

/*
 * Reports a compiled compute shader's workgroup size (its local_size
 * layout); all 0 for another stage.
 */
void glsl_compute_layout(const struct glsl_shader *shader, unsigned size[3]);

/*
 * Reports a compiled geometry shader's primitives as GL names them: the
 * input (GL_POINTS .. GL_TRIANGLES_ADJACENCY), the output (GL_POINTS,
 * GL_LINE_STRIP, GL_TRIANGLE_STRIP) and the most vertices it emits; all 0
 * for another stage.
 */
void glsl_geometry_layout(const struct glsl_shader *shader, unsigned *input, unsigned *output, unsigned *max_vertices);

/*
 * A compiled shader's subroutines (GLSL 4.00, GL_ARB_shader_subroutine):
 * how many subroutine functions it has and the name of the one of an
 * index; how many subroutine uniforms, and of one of them (in declaration
 * order) its name, the name of the hidden uint uniform (or array of them)
 * of the default block that selects the subroutine a call runs, and its
 * size (1 unless an array); and the indexes of the subroutines compatible
 * with a subroutine uniform (those of its type), up to a capacity,
 * returning how many there are.
 */
unsigned glsl_subroutine_count(const struct glsl_shader *shader);
const char *glsl_subroutine_name(const struct glsl_shader *shader, unsigned index);
unsigned glsl_subroutine_uniform_count(const struct glsl_shader *shader);
const char *glsl_subroutine_uniform(const struct glsl_shader *shader, unsigned index, const char **hidden, unsigned *size);
unsigned glsl_subroutine_compatible(const struct glsl_shader *shader, unsigned uniform, unsigned *indexes, unsigned capacity);

/* Frees what a successful link gave. */
void glsl_program_free(struct glsl_program *program);

#endif
