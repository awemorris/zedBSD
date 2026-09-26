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
 * Shaders are SPIR-V (glShaderBinary with GL_SHADER_BINARY_FORMAT_SPIR_V)
 * in the form plan/ws068/phase008/phase.md gives: the uniforms other than
 * samplers in one uniform block at set 0 binding 0, the samplers at set 0
 * from binding 1, attributes and varyings by location.  Linking reads the
 * names and locations out of the SPIR-V and rewrites the vertex shader so
 * that its gl_Position becomes Vulkan's (y turned over, z from [-w, w] to
 * [0, w]).
 */

#ifndef GLES_H
#define GLES_H

#include "../libegl/zegl.h"

#include <GLES2/gl2.h>
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

/* How many vertex attributes, texture units and texture levels a context has. */
#define GLES_ATTRIBS		16U
#define GLES_UNITS		16U
#define GLES_LEVELS		15U

/* How many of Vulkan's core formats the vertex format cache covers. */
#define GLES_FORMATS		192U

/* The longest name of an attribute or a uniform, with its terminator. */
#define GLES_NAME		64U

/* The size of one piece of the per-frame stream memory. */
#define GLES_STREAM_CHUNK	(4U * 1024U * 1024U)

/* The kinds of objects in the shader and program namespace. */
#define GLES_KIND_SHADER	1
#define GLES_KIND_PROGRAM	2

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

	/* The device copy (host visible), its size, where it is mapped, and the frame that last drew from it. */
	VkBuffer buffer;
	VkDeviceMemory memory;
	size_t device_size;
	void *mapped;
	uint64_t used;
};

/*
 * One level of a texture, as RGBA8 rows from the bottom up (GL's order).
 */
struct gles_level {
	int width;
	int height;
	unsigned char *pixels;
};

/*
 * A 2D texture: its levels on the CPU, its sampling state, and the device
 * image made from the levels.
 */
struct gles_texture {
	/* The GL name and the target it was first bound to. */
	GLuint name;
	GLenum target;

	/* The levels (width 0 when a level is not specified). */
	struct gles_level levels[GLES_LEVELS];

	/* The filters and the wrap modes. */
	GLenum min_filter;
	GLenum mag_filter;
	GLenum wrap_s;
	GLenum wrap_t;

	/* Nonzero when the levels changed since the image was made. */
	int dirty;

	/* The device image, its memory and view, how many levels it has, and the frame that last sampled it. */
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	uint32_t level_count;
	uint64_t used;
};

/*
 * A shader: its SPIR-V, and what glGetShaderiv reports about it.
 */
struct gles_shader {
	/* GLES_KIND_SHADER, the GL name, and GL_VERTEX_SHADER or GL_FRAGMENT_SHADER. */
	int kind;
	GLuint name;
	GLenum type;

	/* The SPIR-V words, once a binary or a compile gave them. */
	uint32_t *code;
	size_t words;

	/* The source glShaderSource gave (NULL when none). */
	char *source;

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
 * block (a scalar, a vector or a matrix, or an array of one of those), or
 * a sampler.
 */
struct gles_uniform {
	/* The GL name (arrays without "[0]"), its GL type and how many elements it has. */
	char name[GLES_NAME];
	GLenum type;
	GLint size;

	/* 0 float, 1 int, 2 unsigned, 3 bool; the components of a column, and the columns (1 unless a matrix). */
	unsigned base;
	unsigned components;
	unsigned columns;

	/* Where it is in the uniform block, and the strides of its elements and columns. */
	uint32_t offset;
	uint32_t array_stride;
	uint32_t matrix_stride;

	/* For a sampler: nonzero, its binding, and the texture unit glUniform1i gave. */
	int sampler;
	uint32_t binding;
	GLint unit;

	/* The location of its first element. */
	GLint location;
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

	/* The attached shaders. */
	struct gles_shader *vertex;
	struct gles_shader *fragment;

	/* The locations glBindAttribLocation gave, by name, for the next link. */
	char bound_names[GLES_ATTRIBS][GLES_NAME];
	GLuint bound_locations[GLES_ATTRIBS];
	unsigned bound_count;

	/* Whether the last link succeeded, and its log. */
	int linked;
	char *log;

	/* A number no other link had, so pipelines made for an earlier link are not taken for this one. */
	uint64_t serial;

	/* The Vulkan shaders, the descriptor set layout and the pipeline layout. */
	VkShaderModule vertex_module;
	VkShaderModule fragment_module;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;

	/* The attributes. */
	struct gles_attribute attributes[GLES_ATTRIBS];
	unsigned attribute_count;

	/* The uniforms and the locations of their elements. */
	struct gles_uniform *uniforms;
	unsigned uniform_count;
	struct gles_location *locations;
	unsigned location_count;

	/* The uniform block's values (NULL when the program has no block), its size and binding. */
	unsigned char *uniform_data;
	uint32_t uniform_size;
	uint32_t uniform_binding;

	/* Whether glDeleteProgram waits for the program to stop being current. */
	int delete_pending;
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

	/* The value an attribute without an enabled array has. */
	float value[4];
};

/*
 * One piece of the stream memory: vertices, indices and uniforms written
 * for the frame being recorded.
 */
struct gles_chunk {
	VkBuffer buffer;
	VkDeviceMemory memory;
	unsigned char *mapped;
	size_t size;
	size_t used;
	struct gles_chunk *next;
};

/*
 * A Vulkan object that waits for the frame that uses it to be done.
 */
struct gles_garbage {
	/* A buffer or an image with its view, and their memory. */
	VkBuffer buffer;
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;

	/* A pipeline, or what a program's link made. */
	VkPipeline pipeline;
	VkPipelineLayout layout;
	VkDescriptorSetLayout set_layout;
	VkShaderModule modules[2];

	/* The next object waiting. */
	struct gles_garbage *next;
};

/*
 * A pool of descriptor sets for the frame being recorded.
 */
struct gles_pool {
	VkDescriptorPool pool;
	struct gles_pool *next;
};

/*
 * The fixed-function state a pipeline is made from, besides the program
 * and the vertex layout.
 */
struct gles_raster {
	/* The primitive topology (after strips, loops and fans became lists). */
	uint32_t topology;

	/* Blending: on, the factors, the equations. */
	uint32_t blend;
	uint32_t blend_src_rgb;
	uint32_t blend_dst_rgb;
	uint32_t blend_src_alpha;
	uint32_t blend_dst_alpha;
	uint32_t blend_equation_rgb;
	uint32_t blend_equation_alpha;

	/* The colour channels written. */
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
};

/*
 * A sampler made for one set of sampling state.
 */
struct gles_sampler {
	GLenum min_filter;
	GLenum mag_filter;
	GLenum wrap_s;
	GLenum wrap_t;
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

	/* The buffers bound to GL_ARRAY_BUFFER and GL_ELEMENT_ARRAY_BUFFER. */
	struct gles_buffer *array_buffer;
	struct gles_buffer *element_buffer;

	/* The vertex attributes. */
	struct gles_attrib attribs[GLES_ATTRIBS];

	/* The current program. */
	struct gles_program *program;

	/* The active texture unit and each unit's 2D texture. */
	unsigned active_unit;
	struct gles_texture *units[GLES_UNITS];

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

	/* Whether the device fetches vertices of each core format: 0 not asked yet, 1 yes, 2 no. */
	unsigned char vertex_formats[GLES_FORMATS];

	/* The frame being recorded, counted from 1; objects a draw of it used carry its number. */
	uint64_t frame;

	/* The upload command buffer, its pool and fence. */
	VkCommandPool upload_pool;
	VkCommandBuffer upload;
	VkFence upload_fence;

	/* The stream memory, the descriptor pools and the garbage of the frame. */
	struct gles_chunk *chunks;
	struct gles_pool *pools;
	struct gles_garbage *garbage;

	/* The last draw's descriptor set. */
	struct gles_set_cache set_cache;

	/* The pipelines and samplers made so far. */
	struct gles_pipeline *pipelines;
	struct gles_sampler *samplers;

	/* The framebuffer and renderbuffer bound (framebuffer objects are not there yet: only 0 draws), and the next names. */
	GLuint framebuffer;
	GLuint renderbuffer;
	GLuint next_framebuffer;
	GLuint next_renderbuffer;

	/* The fixed-function layer's state (libGL), NULL until it is made. */
	void *fixed;

	/* The texture sampled where a unit has no complete texture (black), made at its first use. */
	struct gles_texture *black;
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
};

/* The fixed-function layer, NULL without one (gles.c; libGL sets it). */
extern const struct gles_fixed_hooks *gles_fixed;

/* gles.c: the context's state, errors. */
struct zegl_context *gles_context(void);
struct gles_state *gles_state(struct zegl_context *context);
void gles_error(struct zegl_context *context, GLenum error);
void gles_report(const char *what, int code);
int gles_names_add(struct gles_names *names, GLuint name, void *object);
GLuint gles_names_free(struct gles_names *names);
void *gles_names_get(struct gles_names *names, GLuint name);
void gles_names_remove(struct gles_names *names, GLuint name);

/* buffer.c: device memory, the stream, the garbage, buffer objects. */
uint32_t gles_memory_type(struct gles_state *state, uint32_t bits, VkMemoryPropertyFlags flags);
int gles_device_buffer(struct gles_state *state, size_t size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory, void **mapped);
void *gles_stream(struct gles_state *state, size_t size, size_t alignment, VkBuffer *buffer, VkDeviceSize *offset);
void gles_throw_away(struct gles_state *state, VkBuffer buffer, VkImage image, VkImageView view, VkDeviceMemory memory);
void gles_garbage_keep(struct gles_state *state, const struct gles_garbage *objects);
void gles_garbage_destroy(struct gles_state *state, const struct gles_garbage *objects);
void gles_collect(struct gles_state *state);
int gles_buffer_sync(struct gles_state *state, struct gles_buffer *buffer);
void gles_buffer_free(struct gles_state *state, struct gles_buffer *buffer);
int gles_upload_begin(struct gles_state *state);
int gles_upload_end(struct gles_state *state);

/* texture.c: textures and samplers. */
int gles_texture_sync(struct gles_state *state, struct gles_texture *texture);
int gles_texture_complete(struct gles_texture *texture);
VkSampler gles_sampler_get(struct gles_state *state, struct gles_texture *texture);
struct gles_texture *gles_texture_black(struct gles_state *state);
void gles_texture_free(struct gles_state *state, struct gles_texture *texture);
void gles_texture_define(struct gles_texture *texture, GLint level, int width, int height, unsigned char *pixels);

/* program.c: shaders and programs. */
void gles_program_release(struct gles_state *state, struct gles_program *program);
void gles_shader_release(struct gles_shader *shader);

/* draw.c: pipelines, the frame, and readback. */
void gles_pipelines_forget(struct gles_state *state, uint64_t program);
int gles_read_rgba(struct zegl_context *context, GLint x, GLint y, GLsizei width, GLsizei height, unsigned char *rows);
uint32_t *gles_expand(GLenum mode, const uint32_t *indices, uint32_t first, GLsizei count, int rotate, uint32_t *expanded);

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
};

struct gles_spirv {
	/* The stage: 0 vertex, 4 fragment (SPIR-V's execution models). */
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
};

/* spirv.c: reading SPIR-V and rewriting it. */
int gles_spirv_reflect(const uint32_t *code, size_t words, struct gles_spirv *out, char *log, size_t log_size);
void gles_spirv_free(struct gles_spirv *spirv);
uint32_t *gles_spirv_position(const uint32_t *code, size_t words, size_t *out_words);

#endif
