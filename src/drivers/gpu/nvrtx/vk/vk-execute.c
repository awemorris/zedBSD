/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The running of a command buffer's records on the GPU.
 *
 * Each session has up to EXECUTE_SLOTS runs (slots) of the executor's GPU
 * memory for its submissions, each with a push buffer, a constant buffer 0
 * for each draw, the texture header and sampler pools, a zero page and the
 * corners of a quad.  The records are turned into methods in order
 * (exec-push.c and draw-push.c) and the push buffer is put on the work
 * channel when it is full, when a copy must be done by the CPU, and at the
 * end, without waiting for the GPU: the next push buffer is written after
 * it in the same slot while the slot has room (the GPU still reads the
 * runs before it; a slot's ticket is its newest run's), else in a slot
 * whose run has ended (a new slot is made while one is to be had, else
 * the oldest run is waited for), so the CPU writes one while the GPU runs
 * the others.  The runs on the one work channel run in order, each
 * after the one before has finished (its fence waits for idle).  What
 * needs the GPU done with the session's work first waits for its newest
 * run: a copy by the CPU, a shadow given back, the session's close (and,
 * in the node, a memory or a run of the executor's memory given back).
 *
 * A draw writes what it uses: its constants (the push constants and its
 * texture handles) in constant buffer 0, a header and a sampler for each
 * sampled image, the viewport, scissor, depth and stencil tests, depth
 * bias, blend and its constants, vertex input, uniform buffers (a dynamic
 * one at its bind's offset) and programs, the state the pipeline leaves
 * dynamic from the command buffer's records; right after another draw of
 * the push buffer only what differs from it (the constants loaded into
 * the same constant buffer 0 by the push buffer).  A clear of the pass's
 * attachments clears rectangles of its target and depth target where it
 * is recorded.  A copy, a blit, a copy between a buffer and an
 * image, and a clear are drawn too: a quad of the executor's own programs
 * samples the source (in pixel coordinates) into the destination.  A copy
 * whose buffer is not laid out as a texture or a target can be (its start
 * or row pitch off their boundaries) is done by the CPU through the BAR1
 * window.  A depth image's copies go by the copy engine, its clears as the
 * depth target of a pass of no colour target.  Between a pass or a
 * transfer and what follows, the GPU waits for its idle and its texture
 * and constant caches are invalidated.
 *
 * A dispatch launches its QMD on the compute subchannel: its constant
 * buffer 0 (the push constants, the storage table of its storage buffers
 * and the group counts' three words) and its QMD each take a slot of the
 * run's constants, written by the CPU.  A launch invalidates the shader
 * caches, which waits for the GPU's idle first, so a dispatch follows the
 * work before it; a barrier after a dispatch waits for its idle too, for
 * what reads its writes after it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/compiler/spirv.h"
#include "drivers/gpu/nvrtx/nvrtx-compute.h"
#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-exec.h"
#include "drivers/gpu/nvrtx/nvrtx-draw.h"
#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/compiler/sm75/sm75.h"

/*
 * The runs (slots) a session writes its submissions in at most: one being
 * written while the GPU runs the others.
 */
#define EXECUTE_SLOTS			3U

/* How many lost sessions a boot shows the memory of, and the most of their objects shown. */
#define EXECUTE_SHOWN_LOSSES		3U
#define EXECUTE_SHOWN_OBJECTS		96U

/* A slot's run: its size and where each of its parts starts. */
#define EXECUTE_RUN_BYTES		(2U * 1024U * 1024U)
#define EXECUTE_PUSH_AT			0U
#define EXECUTE_PUSH_BYTES		(1024U * 1024U)
#define EXECUTE_CONSTANTS_AT		(1024U * 1024U)
#define EXECUTE_CONSTANTS_BYTES		(512U * 1024U)
#define EXECUTE_HEADERS_AT		(1536U * 1024U)
#define EXECUTE_SAMPLERS_AT		(1600U * 1024U)
#define EXECUTE_ZERO_AT			(1664U * 1024U)
#define EXECUTE_CORNERS_AT		(1668U * 1024U)

/*
 * The application draws' constant buffers 0, one for each stage (the
 * vertex program's, then the fragment program's, then the geometry
 * program's, EXECUTE_CONSTANTS_SLOT apart), in the first slot (which lives
 * as long as the session): zero when made, then written only by loads in
 * the push buffers, never by the CPU, as the GPU writes them.
 */
#define EXECUTE_ROOT_AT			(1672U * 1024U)
#define EXECUTE_STAGES			3U
#define EXECUTE_STAGE_VERTEX		0U
#define EXECUTE_STAGE_FRAGMENT		1U
#define EXECUTE_STAGE_GEOMETRY		2U

/*
 * The headers and the samplers of one push buffer, each eight words.
 *
 * Value referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597tex.h (the 256-bit
 * NVC597_TEXHEAD_PITCH_* and NVC597_TEXSAMP* layouts).
 */
#define EXECUTE_POOL_ENTRIES		2048U
#define EXECUTE_POOL_ENTRY_BYTES	32U

/* One draw's constant buffer 0 (a whole number of 256-byte units): the push constants, then the texture handles; its slot in the run. */
#define EXECUTE_CONSTANTS_SIZE		0x500U
#define EXECUTE_CONSTANTS_SLOT		0x500U

/*
 * A dispatch's constant buffer 0 in its slot: the push constants at 0, the
 * storage table at NVRTX_SM75_CB_STORAGE (its entries end at the texture
 * handles' place, which a compute program has none of), and the group
 * counts' three words at EXECUTE_GROUPS_AT, which their storage entry
 * names.  A whole slot is bound.
 */
#define EXECUTE_GROUPS_AT		NVRTX_SM75_CB_TEXTURES
#define EXECUTE_GROUPS_BYTES		12U

/* What one record writes at most into the push buffer (a transfer of NVRTX_VK_REGIONS_MAX quads), before the run is started to make room. */
#define EXECUTE_RECORD_ROOM		32768U

/*
 * What one surface of a transfer writes at most into the push buffer (a
 * quad, or a target and its clear): a transfer of more surfaces than its
 * record's room takes starts the run before a surface that might not fit.
 */
#define EXECUTE_SURFACE_ROOM		2048U

/*
 * A query's report's bytes (an occlusion query's end report follows its
 * begin report), and the queries of a reset written between two checks
 * of the push buffer's room (each well under EXECUTE_SURFACE_ROOM's
 * share).
 */
#define EXECUTE_QUERY_REPORT_BYTES	16U
#define EXECUTE_QUERY_GROUP		64U

/*
 * The ways a depth image's texels are copied by the copy engine: from a
 * buffer into the image, from the image out to a buffer, between two depth
 * images of one format.
 */
#define EXECUTE_DEPTH_TO_IMAGE		0U
#define EXECUTE_DEPTH_FROM_IMAGE	1U
#define EXECUTE_DEPTH_IMAGES		2U

/*
 * The application draws after which a push buffer is put on the GPU: the
 * first of a command buffer's run after a few, so the GPU (idle when a
 * frame is submitted) starts while the CPU writes the rest, the others
 * after more.  A draw is written in about half the time the GPU takes to
 * run it, so push buffers this short keep the GPU fed (a long one left
 * it waiting for the CPU to finish it).
 */
#define EXECUTE_FIRST_DRAWS		256U
#define EXECUTE_RUN_DRAWS		2048U

/*
 * A texture handle: the header's index, and the sampler's from bit 20.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_descriptor_types.h
 * (nvk_sampled_image_descriptor).
 */
#define EXECUTE_HANDLE_SAMPLER_SHIFT	20U

/*
 * The program groups of the vertex, geometry and fragment programs, and a
 * uniform buffer: its start on 256 bytes (minUniformBufferOffsetAlignment),
 * its size in 16-byte units up to 64 KiB (maxUniformBufferRange).
 *
 * Value referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_shader.h
 * (nvk_cbuf_binding_for_stage: the group is the stage's number).
 */
#define EXECUTE_GROUP_VERTEX		0U
#define EXECUTE_GROUP_GEOMETRY		3U
#define EXECUTE_GROUP_FRAGMENT		4U
#define EXECUTE_UNIFORM_ALIGN		256U
#define EXECUTE_UNIFORM_UNIT		16U
#define EXECUTE_UNIFORM_BYTES_MAX	65536U

/* The quad's corners: two triangles of six corners, two floats each. */
#define EXECUTE_CORNERS			6U
#define EXECUTE_CORNER_STRIDE		8U

/* The float bits of -1 and 1, and the bytes of an internal draw's push constants. */
#define EXECUTE_FLOAT_MINUS_ONE		0xbf800000U
#define EXECUTE_FLOAT_ONE		0x3f800000U
#define EXECUTE_INTERNAL_PUSH_BYTES	0x20U

/*
 * A buffer written as a target: its start and its row pitch on these
 * boundaries.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/nil/image.rs (Image::new_linear,
 * the 128-byte row alignment for rendering).
 */
#define EXECUTE_BUFFER_START_ALIGN	256U
#define EXECUTE_BUFFER_PITCH_ALIGN	128U

/*
 * The bytes a GOB keeps together of a row (the 16-byte pieces of
 * execute_gob_offset()).
 */
#define EXECUTE_GOB_PIECE_BYTES		16U

/*
 * The formats a copy one to one draws texels of a size through (1, 2 or
 * 4 bytes; 8 and 16 bytes as two and four such pixels): unsigned
 * normalized 8-bit channels go through a float and back unchanged, so
 * every bit of the texel is copied whatever its own format.
 */
#define EXECUTE_RAW_BYTES		4U

/*
 * A buffer read as a texture: the texture header holds the address from
 * bit 5 and the pitch from bit 5, so the texture starts on the 32-byte
 * boundary at or before the region and the region begins that many pixels
 * into its rows.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597tex.h
 * (NVC597_TEXHEAD_PITCH_ADDRESS_BITS31TO5,
 * NVC597_TEXHEAD_PITCH_PITCH_BITS20TO5).
 */
#define EXECUTE_TEXTURE_START_ALIGN	32U
#define EXECUTE_TEXTURE_PITCH_ALIGN	32U

/*
 * One slot: its run (token names it to the node, cpu and va its two views)
 * and the GPU's run of the push buffer last written in it, which must end
 * before the slot is written again.
 */
struct execute_slot {
	void *token;
	uint8_t *cpu;
	uint64_t va;
	struct nvrtx_vk_ticket ticket;
};

/*
 * What the shadow holds after a pass drawn in it: a region (x0, y0 to x1,
 * y1) equal to the same region of the target it was copied onto, a
 * memory's image (backing), at the time the
 * node's token says (nvrtx_vk_gpu_quiet()).  It holds while valid: any
 * other pass, transfer or decode of the executor lets it go, and an equal
 * token later says nothing else wrote the backing meanwhile.
 */
struct execute_kept {
	bool valid;
	uint64_t token;
	void *backing;
	struct nvrtx_exec_target target;
	uint64_t shadow_va;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
};

/*
 * The pool handles a push buffer's draws keep for their textures: one
 * place for each view and sampler pair, chosen by their addresses mixed
 * (a multiplication by the golden ratio's bits, its top bits taken: the
 * objects' addresses lie a fixed distance apart, which their low bits
 * alone do not tell apart); a pair whose place another took makes its
 * entries again.  Two neighbouring places (place and place ^ 1) make a
 * set: a pair is looked for in both and a new one takes the set's place
 * not used in this push buffer yet.
 */
#define EXECUTE_TEXTURE_CACHE		32U
#define EXECUTE_TEXTURE_CACHE_MIX	0x9e3779b97f4a7c15ULL
#define EXECUTE_TEXTURE_CACHE_SHIFT	58U

/*
 * The parts of a pipeline's state a draw writes when they differ from the
 * last draw's pipeline's.
 */
#define EXECUTE_PIPELINE_RASTER		(1U << 0)
#define EXECUTE_PIPELINE_BLEND		(1U << 1)
#define EXECUTE_PIPELINE_VERTEX_PROGRAM	(1U << 2)
#define EXECUTE_PIPELINE_PIXEL_PROGRAM	(1U << 3)
#define EXECUTE_PIPELINE_VERTEX_INPUT	(1U << 4)
#define EXECUTE_PIPELINE_UNIFORMS	(1U << 5)
#define EXECUTE_PIPELINE_GEOMETRY_PROGRAM	(1U << 6)
#define EXECUTE_PIPELINE_ALL		0x7fU

/* The words nvrtx_exec_viewport() writes. */
#define EXECUTE_VIEWPORT_BYTES		48U

/*
 * What draws one after another reuse: the viewport's words the last draw's
 * viewport (viewport_in, as recorded) made (valid once viewport_bytes is
 * not 0), and the pool handles of the textures (view and sampler) the
 * fragment programs sampled, each at its pair's place
 * (EXECUTE_TEXTURE_CACHE) and made in the push buffer counted by pools
 * (execute_begin() counts each new one; the pools start empty there).
 */
struct execute_reuse {
	uint32_t viewport_in[6];
	uint8_t viewport[EXECUTE_VIEWPORT_BYTES];
	size_t viewport_bytes;
	uint32_t pools;
	uint32_t texture_pools[EXECUTE_TEXTURE_CACHE];
	const void *texture_view[EXECUTE_TEXTURE_CACHE];
	const void *texture_sampler[EXECUTE_TEXTURE_CACHE];
	uint32_t texture_handle[EXECUTE_TEXTURE_CACHE];
};

/*
 * What the GPU holds after the last application draw of a push buffer,
 * for the next draw to write only what differs.  It holds while nothing
 * else was written since: the same push buffer (pools) going on where that
 * draw ended (at).  The pipeline's state (the culling, the blend, the
 * programs), the depth and stencil tests, the depth bias and the blend
 * constants as written, the viewport and the scissor as written, the
 * vertex streams' buffers and offsets, the index buffer, the sets the
 * uniform buffers came from and their dynamic offsets, the draw's bases,
 * and each stage's constant buffer 0: its slot (constants_va) holding the
 * push constants push, the texture handles handles, the storage table's
 * words storage (the first storage_valid of them loaded), and the vertex
 * program's the draw's first instance first_instance, and which one the
 * selector still names (selected, the stage plus one; 0 for none, a
 * uniform buffer's binding selects its own buffer).
 */
struct execute_held {
	uint32_t pools;
	size_t at;
	const struct nvrtx_vk_pipeline *pipeline;
	uint32_t viewport[6];
	int32_t scissor[4];
	const struct nvrtx_vk_buffer *buffers[NVRTX_VK_VERTEX_BINDINGS_MAX];
	uint64_t offsets[NVRTX_VK_VERTEX_BINDINGS_MAX];
	bool index_set;
	const struct nvrtx_vk_buffer *index_buffer;
	uint64_t index_offset;
	uint32_t index_type;
	const struct nvrtx_vk_descriptor_set *sets[NVRTX_VK_SETS_MAX];
	uint32_t dynamic[NVRTX_VK_SETS_MAX][NVRTX_VK_BINDINGS_MAX];
	struct nvrtx_exec_depth depth;
	struct nvrtx_exec_depth_bias bias;
	uint32_t blend_constants[4];
	struct nvrtx_exec_bases bases;
	uint64_t constants_va[EXECUTE_STAGES];
	uint32_t selected;
	uint32_t push[EXECUTE_STAGES][NVRTX_VK_PUSH_BYTES / 4U];
	uint32_t handles[EXECUTE_STAGES][NVRTX_VK_TEXTURES_MAX];
	uint32_t storage[EXECUTE_STAGES][NVRTX_VK_STORAGE_MAX * 4U];
	uint32_t storage_valid[EXECUTE_STAGES];
	uint32_t first_instance;
};

/*
 * One draw's words for each stage's constant buffer 0 beside the push
 * constants: the texture handles and the storage table (count words).
 */
struct execute_stage_words {
	uint32_t handles[NVRTX_VK_TEXTURES_MAX];
	uint32_t storage[NVRTX_VK_STORAGE_MAX * 4U];
	uint32_t storage_count;
};

/*
 * What a command buffer's records have bound so far, and the pass they
 * are in; push_bytes are the command buffer's push constant bytes, push
 * the ones bound, none from push_end on (still zero); dynamic the dynamic
 * offset of each set's binding, and the depth bias's factors, the blend
 * constants and each face's stencil compare mask, write mask and reference
 * (front, back) the dynamic state records set (zero until set).  The
 * compute bind point's pipeline, sets and dynamic offsets are kernel,
 * kernel_sets and kernel_dynamic; dispatched says a dispatch ran since the
 * last barrier (the push constants are both bind points').  A
 * pass drawn in the shadow: backing is its target's memory's (NULL
 * without one), and known says the shadow holds the target's
 * pixels in the region known_x0, known_y0 to known_x1, known_y1 at its start.
 * pass and framebuffer are the pass's (its end resolves what it names).
 */
struct execute_state {
	const uint8_t *push_bytes;
	const struct nvrtx_vk_pipeline *pipeline;
	uint32_t viewport[6];
	bool viewport_set;
	VkRect2D scissor;
	bool scissor_set;
	const struct nvrtx_vk_descriptor_set *sets[NVRTX_VK_SETS_MAX];
	uint32_t dynamic[NVRTX_VK_SETS_MAX][NVRTX_VK_BINDINGS_MAX];
	const struct nvrtx_vk_buffer *buffers[NVRTX_VK_VERTEX_BINDINGS_MAX];
	uint64_t offsets[NVRTX_VK_VERTEX_BINDINGS_MAX];
	const struct nvrtx_vk_buffer *index_buffer;
	uint64_t index_offset;
	uint32_t index_type;
	uint32_t push[NVRTX_VK_PUSH_BYTES / 4U];
	uint32_t push_end;
	uint32_t depth_bias[3];
	uint32_t blend_constants[4];
	uint32_t stencil_compare[2];
	uint32_t stencil_write[2];
	uint32_t stencil_reference[2];
	const struct nvrtx_vk_pipeline *kernel;
	const struct nvrtx_vk_descriptor_set *kernel_sets[NVRTX_VK_SETS_MAX];
	uint32_t kernel_dynamic[NVRTX_VK_SETS_MAX][NVRTX_VK_BINDINGS_MAX];
	bool dispatched;
	bool in_pass;
	const struct nvrtx_vk_render_pass *pass;
	const struct nvrtx_vk_framebuffer *framebuffer;
	struct nvrtx_exec_target target;
	bool target_written;
	VkRect2D area;
	struct nvrtx_exec_target resolve;
	bool shadowed;
	void *backing;
	bool known;
	int32_t known_x0;
	int32_t known_y0;
	int32_t known_x1;
	int32_t known_y1;

	/*
	 * In a multiview pass, the view the draw being written renders: its
	 * layer of the layered target, which SET_RT_LAYER names.
	 */
	uint32_t view;
};

/*
 * The executor's own programs (a textured quad placed by push constants,
 * the draw-test draw's) and the session's slots.
 *
 * slot is the slot the push buffer is written in, cpu and va its views;
 * started says a push buffer is begun there and not yet submitted;
 * class_ready that a run on the session's channel set the 3D class's
 * initial state and the channel keeps it; kept is what the shadow holds,
 * reuse what one draw leaves the next, held what the GPU holds after
 * the last application draw, state the command buffer's being run (its
 * own here, so a submission allocates nothing for it).  segment is where the push buffer begun
 * last starts in the slot's push part and at where it goes on;
 * constants, headers and samplers how much of their parts the slot's push
 * buffers use (from the slot's first, at segment 0), draws and quads how many
 * application draws and own quads it holds (for the node's counts); early
 * says the push buffer is a command buffer's first, put on the GPU after
 * EXECUTE_FIRST_DRAWS draws (the others after EXECUTE_RUN_DRAWS).  The
 * programs are compiled once, into a run of their own.
 */
struct nvrtx_vk_execute {
	struct execute_slot slots[EXECUTE_SLOTS];
	uint32_t slot;
	uint8_t *cpu;
	uint64_t va;
	size_t segment;
	size_t at;
	uint32_t constants;
	uint32_t headers;
	uint32_t samplers;
	uint32_t draws;
	uint32_t quads;
	bool started;
	bool early;
	bool class_ready;
	void *program_token;
	uint8_t *program_cpu;
	uint64_t program_va;
	uint64_t quad_vertex;
	uint64_t quad_fragment;
	uint32_t quad_vertex_gprs;
	uint32_t quad_fragment_gprs;
	bool programs_ready;
	void *shadow_token;
	uint64_t shadow_va;
	uint64_t shadow_bytes;
	struct execute_kept kept;
	struct execute_reuse reuse;
	struct execute_held held;
	struct execute_state state;
	/* The owner's generation: the shader model of the programs, the 3D class of the push buffers and its compute launches. */
	uint32_t shader_model;
	uint32_t threed_class;
	struct nvrtx_compute_kind compute_kind;
};

/* One quad an internal draw makes: the destination, the source texture (pitch linear or tiled) and its region in pixels. */
struct execute_quad {
	struct nvrtx_exec_target target;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	uint64_t source;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t source_pitch;
	uint32_t source_format;
	bool source_tiled;
	int32_t u0;
	int32_t v0;
	int32_t u1;
	int32_t v1;
	uint32_t filter;
};

/* The 2D surfaces a copy's region names in an image: its layers, or a 3D image's slices, count of them from first. */
struct execute_surfaces {
	uint32_t first;
	uint32_t count;
};

static int execute_open(struct nvrtx_vk_session *session, struct nvrtx_vk_execute **execute);
static int execute_programs(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute);
static int execute_compile(struct nvrtx_draw_ir *program, uint32_t shader_model, uint8_t *cpu, uint32_t *gprs, uint32_t *bytes);
static int execute_slot_make(struct nvrtx_vk_session *session, struct execute_slot *slot);
static int execute_start(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute);
static void execute_begin(struct nvrtx_vk_execute *execute, size_t segment);
static bool execute_continues(const struct nvrtx_vk_execute *execute);
static int execute_submit(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute);
static int execute_flush(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute);
static int execute_drain(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute);
static int execute_room(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state);
static int execute_record(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static void execute_bind(struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_query(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, uint32_t opcode, const struct nvrtx_vk_query_place *place);
static void execute_bind_sets(struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_dispatch(struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_storage(const struct nvrtx_vk_descriptor_set *const *sets, const uint32_t (*dynamic)[NVRTX_VK_BINDINGS_MAX], const struct nvrtx_vk_texture_source *source, uint64_t *va, uint64_t *range);
static int execute_kernel_uniform(const struct execute_state *state, const struct nvrtx_vk_texture_source *source, uint64_t *va, uint32_t *size);
static int execute_stage_storage(const struct execute_state *state, const struct nvrtx_vk_program *program, struct execute_stage_words *words);
static void execute_after_dispatch(struct nvrtx_vk_execute *execute, struct execute_state *state);
static void execute_bind_stencil(struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_clear_attachments(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static void execute_draw_depth(const struct nvrtx_vk_pipeline *pipeline, const struct execute_state *state, struct nvrtx_exec_depth *depth);
static void execute_draw_state(struct nvrtx_vk_execute *execute, const struct execute_state *state, bool known);
static int execute_begin_pass(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state,
			      const struct nvrtx_vk_record *record);
static bool execute_kept_holds(struct nvrtx_vk_session *session, const struct nvrtx_vk_execute *execute, const struct execute_state *state);
static int execute_shadow(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state,
			  bool load);
static int execute_end_pass(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state);
static void execute_shadow_copy(const struct nvrtx_exec_target *to, const struct nvrtx_exec_target *from, int32_t x0, int32_t y0,
				int32_t x1, int32_t y1, struct execute_quad *quad);
static int execute_draw(struct nvrtx_vk_execute *execute, struct execute_state *state, bool indexed, const uint32_t *values);
static int execute_views(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static uint32_t execute_view_layers(uint32_t view_mask);
static bool execute_layer_drawn(const struct execute_state *state, uint32_t layer);
static int execute_query_views(struct nvrtx_vk_execute *execute, const struct execute_state *state, const struct nvrtx_vk_query_place *place);
static int execute_draw_indirect(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_dispatch_indirect(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static uint32_t execute_pipeline_changes(const struct nvrtx_vk_pipeline *held, const struct nvrtx_vk_pipeline *pipeline);
static bool execute_same_words(const uint32_t *left, const uint32_t *right, size_t count);
static int execute_draw_constants(struct nvrtx_vk_execute *execute, const struct execute_state *state, const struct execute_stage_words *words, uint32_t first_instance, bool known);
static void execute_draw_load(struct nvrtx_vk_execute *execute, uint32_t stage, uint32_t offset, const uint32_t *words, uint32_t *held_words, uint32_t count, uint32_t *valid);
static int execute_textures(struct nvrtx_vk_execute *execute, const struct execute_state *state, const struct nvrtx_vk_program *program, uint32_t *handles);
static bool execute_texture_kept(const struct execute_reuse *reuse, uint32_t place, const void *view, const void *sampler);
static int execute_texture(struct nvrtx_vk_execute *execute, struct nvrtx_vk_view *view, struct nvrtx_vk_sampler *sampler, uint32_t *handle);
static int execute_texel(struct nvrtx_vk_execute *execute, struct nvrtx_vk_buffer_view *view, uint32_t *handle);
static uint64_t execute_texture_shape(const struct nvrtx_vk_image *image, const struct nvrtx_vk_view *view, struct nvrtx_exec_texture_shape *shape);
static int execute_surface_room(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state);
static int execute_pool_entry(struct nvrtx_vk_execute *execute, const uint32_t *header, const uint32_t *sampler, uint32_t *handle);
static int execute_constants(struct nvrtx_vk_execute *execute, uint8_t **cpu, uint64_t *va);
static void execute_vertex_input(const struct nvrtx_vk_pipeline *pipeline, const struct execute_state *state, struct nvrtx_exec_vertex *vertex);
static int execute_transfer(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_decode(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_depth_remap(uint32_t format, uint32_t aspects, uint32_t way, struct nvrtx_exec_rect_copy *copy, uint32_t *buffer_texel);
static int execute_depth_buffer(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_image *image, const struct nvrtx_vk_buffer *buffer, const struct nvrtx_vk_record *record, bool to_image);
static int execute_depth_images(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static void execute_depth_side(struct nvrtx_exec_copy_side *side, uint64_t va, uint64_t offset, uint32_t pitch, uint32_t rows, int32_t x, int32_t y);
static int execute_video_copy(struct nvrtx_vk_execute *execute, const struct nvrtx_vk_image *image, uint64_t image_va, const struct nvrtx_vk_buffer *buffer, uint64_t buffer_va, const struct nvrtx_vk_record *record);
static int execute_region_surfaces(const struct nvrtx_vk_image *image, const VkImageSubresourceLayers *layers, int32_t z0, int32_t z1, struct execute_surfaces *surfaces);
static int execute_copy_images(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_buffer_image(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_buffer_surface(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_buffer *buffer, const struct nvrtx_vk_image *image, const VkBufferImageCopy *region, uint32_t layer, uint64_t offset, bool to_image);
static int execute_cpu_copy(struct nvrtx_vk_session *session, const struct nvrtx_vk_buffer *buffer, const struct nvrtx_vk_image *image, const VkBufferImageCopy *region, uint32_t layer, uint64_t offset, bool to_image);
static int execute_clear_image(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_quad(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct execute_quad *quad);
static int execute_image_target(const struct nvrtx_vk_image *image, uint32_t level, uint32_t layer, uint32_t format, struct nvrtx_exec_target *target);
static int execute_image_source(const struct nvrtx_vk_image *image, uint32_t level, uint32_t layer, struct execute_quad *quad);
static uint32_t execute_raw_format(uint32_t bytes, uint32_t *factor);
static uint32_t execute_copy_format(uint32_t format);
static void execute_raw_quad(struct execute_quad *quad, uint32_t bytes);
static int execute_depth_target(struct nvrtx_vk_session *session, struct nvrtx_vk_image *image, uint32_t level, uint32_t layer, struct nvrtx_exec_target *target);
static int execute_resolve(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_image *source, uint32_t source_layer, int32_t sx, int32_t sy, const struct nvrtx_vk_image *destination, uint32_t level, uint32_t layer, int32_t dx, int32_t dy, uint32_t width, uint32_t height);
static int execute_resolve_images(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_resolve_pass(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state);
static int execute_clear_depth_image(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, struct execute_state *state, const struct nvrtx_vk_record *record);
static int execute_uniforms(const struct execute_state *state, const struct nvrtx_vk_program *program, uint32_t group, uint8_t *push, size_t *at);
static int execute_copy_buffer(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, const struct nvrtx_vk_record *record);
static int execute_write_buffer(struct nvrtx_vk_session *session, struct nvrtx_vk_execute *execute, const struct nvrtx_vk_record *record);
static uint64_t execute_gob_offset(uint32_t pitch, uint32_t x, uint32_t y);
static bool execute_region_inside(int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t image_width, uint32_t image_height);
static void execute_full_viewport(uint32_t *viewport, int32_t x0, int32_t y0, int32_t x1, int32_t y1);

/*
 * Puts an executable command buffer's records on the GPU without waiting
 * for them.  Returns 0, the error of the End that left the command
 * buffer invalid, EINVAL for one not executable otherwise, or the error of
 * the submission or the wait that failed.
 */
int
nvrtx_vk_execute(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_command_buffer *command)
{
	struct nvrtx_vk_execute *execute;
	struct execute_state *state;
	uint32_t index;
	int error;

	/*
	 * Only an executable command buffer runs.  One whose End failed is
	 * turned down with that End's error, so that the submission's result
	 * is the End's.
	 */
	if (command->state == NVRTX_VK_COMMAND_INVALID && command->error != 0)
		return command->error;
	if (command->state != NVRTX_VK_COMMAND_EXECUTABLE)
		return EINVAL;
	if (command->record_count == 0U)
		return 0;

	/* The session's run and programs. */
	error = execute_open(session, &execute);
	if (error != 0)
		return error;

	/* The state starts empty. */
	state = &execute->state;
	kern_memset(state, 0, sizeof(*state));
	state->push_bytes = command->push_bytes;

	/*
	 * A push buffer begun before and never submitted (its command buffer
	 * failed) is begun again in its place; else one goes on after the last
	 * one in the slot while it has room, or a slot is taken.
	 */
	error = 0;
	if (execute->started) {
		execute_begin(execute, execute->segment);
	} else if (execute_continues(execute)) {
		execute_begin(execute, execute->at);
	} else {
		error = execute_start(session, execute);
	}

	/* Each record in order, the push buffer put on the GPU when it fills (the first one early). */
	execute->early = true;
	for (index = 0U; index < command->record_count && error == 0; index++) {
		error = execute_room(session, execute, state);
		if (error != 0)
			break;
		error = execute_record(session, execute, state, &command->records[index]);
		if (error != 0)
			break;
	}

	/* The rest of the push buffer. */
	if (error == 0)
		error = execute_submit(session, execute);

	/* Reports a run that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the GPU runs the command buffer. */
	return 0;
}

/* Gives back a closing session's slots and programs once its runs have ended. */
void
nvrtx_vk_execute_close(
	struct nvrtx_vk_session *session)
{
	struct nvrtx_vk_execute *execute;
	uint32_t index;

	execute = session->execute;
	if (execute == NULL)
		return;

	/* The runs (the node waits for the session's runs first), then the record. */
	for (index = 0U; index < EXECUTE_SLOTS; index++) {
		if (execute->slots[index].token != NULL)
			nvrtx_vk_gpu_free(session->owner, execute->slots[index].token);
	}
	if (execute->program_token != NULL)
		nvrtx_vk_gpu_free(session->owner, execute->program_token);
	if (execute->shadow_token != NULL)
		nvrtx_vk_gpu_free(session->owner, execute->shadow_token);
	kern_free(execute);
	session->execute = NULL;
}

/*
 * Shows where a session's GPU memory is, after its runs were lost, so the
 * address of a fault the GSP reported can be placed: the executor's runs,
 * programs and shadow, then each allocation and each buffer and image
 * bound in one (its address, its bytes; an image's format, type, size,
 * levels, layers and whether it is tiled).  The first few losses of a boot
 * only, and EXECUTE_SHOWN_OBJECTS objects of each.
 */
void
nvrtx_vk_execute_show_memory(
	struct nvrtx_vk_session *session)
{
	static uint32_t shown;
	const struct nvrtx_vk_execute *execute;
	const struct nvrtx_vk_object *object;
	const struct nvrtx_vk_memory *memory;
	const struct nvrtx_vk_buffer *buffer;
	const struct nvrtx_vk_image *image;
	uint64_t base;
	uint32_t lines;
	uint32_t index;

	/* The first few losses only. */
	if (shown >= EXECUTE_SHOWN_LOSSES)
		return;
	shown++;

	/* The executor's own runs. */
	execute = session->execute;
	if (execute != NULL) {
		for (index = 0U; index < EXECUTE_SLOTS; index++) {
			if (execute->slots[index].token != NULL)
				kern_logf("nvrtx: vk: lost: run %llx+%x\n", (unsigned long long)execute->slots[index].va, (unsigned)EXECUTE_RUN_BYTES);
		}
		if (execute->program_token != NULL)
			kern_logf("nvrtx: vk: lost: programs %llx\n", (unsigned long long)execute->program_va);
		if (execute->shadow_token != NULL)
			kern_logf("nvrtx: vk: lost: shadow %llx+%llx\n", (unsigned long long)execute->shadow_va,
				  (unsigned long long)execute->shadow_bytes);
	}

	/* The allocations and what is bound in them. */
	lines = 0U;
	for (object = session->objects; object != NULL && lines < EXECUTE_SHOWN_OBJECTS; object = object->next) {
		if (object->payload == NULL)
			continue;
		if (object->kind == I915_VK_OBJ_MEMORY) {
			memory = object->payload;
			kern_logf("nvrtx: vk: lost: memory %llx+%llx\n", (unsigned long long)memory->va, (unsigned long long)memory->bytes);
			lines++;
		} else if (object->kind == I915_VK_OBJ_BUFFER) {
			buffer = object->payload;
			if (buffer->memory == NULL || buffer->memory->payload == NULL)
				continue;
			memory = buffer->memory->payload;
			kern_logf("nvrtx: vk: lost: buffer %llx+%llx usage %x\n", (unsigned long long)(memory->va + buffer->offset),
				  (unsigned long long)buffer->size, (unsigned)buffer->usage);
			lines++;
		} else if (object->kind == I915_VK_OBJ_IMAGE) {
			image = object->payload;
			if (image->memory == NULL || image->memory->payload == NULL)
				continue;
			memory = image->memory->payload;
			base = memory->va + image->offset;
			kern_logf("nvrtx: vk: lost: image %llx+%llx f%u t%u %ux%ux%u l%u a%u %s\n", (unsigned long long)base,
				  (unsigned long long)image->bytes, (unsigned)image->format, (unsigned)image->type, (unsigned)image->width,
				  (unsigned)image->height, (unsigned)image->slices, (unsigned)image->levels, (unsigned)image->layers,
				  image->tiled ? "tiled" : "pitch");
			lines++;
		}
	}
}

/*
 * Gives the session's slots, made at its first submission with the first
 * slot, and the executor's programs.
 */
static int
execute_open(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute **execute)
{
	struct nvrtx_vk_execute *opened;
	int error;

	/* Made once; programs that did not compile are tried again. */
	if (session->execute != NULL) {
		opened = session->execute;
		if (!opened->programs_ready) {
			error = execute_programs(session, opened);
			if (error != 0)
				return error;
		}
		*execute = opened;
		return 0;
	}

	/* The record and its first slot, the one written first. */
	opened = kern_calloc(1U, sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;
	error = execute_slot_make(session, &opened->slots[0]);
	if (error != 0) {
		kern_free(opened);
		return error;
	}
	opened->slot = 0U;
	opened->cpu = opened->slots[0].cpu;
	opened->va = opened->slots[0].va;

	/* The owner's generation, read once: the programs and the push buffers follow it. */
	opened->shader_model = nvrtx_vk_shader_model(session->owner);
	opened->threed_class = nvrtx_vk_threed_class(session->owner);
	nvrtx_vk_compute_kind(session->owner, &opened->compute_kind);

	/* The executor's programs. */
	session->execute = opened;
	error = execute_programs(session, opened);
	if (error != 0)
		return error;

	/* Succeeded: the slots are the session's. */
	*execute = opened;
	return 0;
}

/* Makes a slot: its run, the zero page and the quad's corners written once. */
static int
execute_slot_make(
	struct nvrtx_vk_session *session,
	struct execute_slot *slot)
{
	static const uint32_t corners[EXECUTE_CORNERS * 2U] = {
		0U, 0U,
		EXECUTE_FLOAT_ONE, 0U,
		0U, EXECUTE_FLOAT_ONE,
		EXECUTE_FLOAT_ONE, 0U,
		EXECUTE_FLOAT_ONE, EXECUTE_FLOAT_ONE,
		0U, EXECUTE_FLOAT_ONE
	};
	uint32_t index;
	int error;

	/* The run, in system memory the CPU writes through its cache (else in VRAM). */
	error = nvrtx_vk_gpu_alloc_system(session->owner, EXECUTE_RUN_BYTES, &slot->token, &slot->cpu, &slot->va);
	if (error != 0)
		error = nvrtx_vk_gpu_alloc(session->owner, EXECUTE_RUN_BYTES, &slot->token, &slot->cpu, &slot->va);
	if (error != 0) {
		slot->token = NULL;
		return error;
	}

	/* The zero page, constant buffer 0, and the corners: (0,0) (1,0) (0,1), (1,0) (1,1) (0,1). */
	kern_memset(slot->cpu + EXECUTE_ZERO_AT, 0, NVRTX_GSP_PAGE_SIZE);
	kern_memset(slot->cpu + EXECUTE_ROOT_AT, 0, EXECUTE_STAGES * EXECUTE_CONSTANTS_SLOT);
	for (index = 0U; index < EXECUTE_CORNERS * 2U; index++)
		nvrtx_put32(slot->cpu, EXECUTE_CORNERS_AT + index * 4U, corners[index]);

	/* Succeeded: no run reads the slot yet. */
	slot->ticket.fence = 0U;
	slot->ticket.submitted_us = 0U;
	return 0;
}

/*
 * Compiles the executor's quad programs (draw-test's textured quad: the
 * corners placed by the push constants' rectangle, the texture read at the
 * push constants' texture rectangle) into a run of their own.
 */
static int
execute_programs(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute)
{
	struct nvrtx_draw_ir *program;
	uint8_t *cpu;
	uint64_t va;
	uint32_t bytes;
	int error;

	/* The run: a page for each program (kept from a try that did not compile). */
	if (execute->program_token == NULL) {
		error = nvrtx_vk_gpu_alloc(session->owner, 2U * NVRTX_GSP_PAGE_SIZE, &execute->program_token, &execute->program_cpu,
					   &execute->program_va);
		if (error != 0)
			return error;
	}
	cpu = execute->program_cpu;
	va = execute->program_va;

	/* The IR's storage. */
	program = kern_malloc(sizeof(*program));
	if (program == NULL)
		return ENOMEM;

	/* The vertex program in the first page, the fragment program in the second. */
	nvrtx_draw_vertex_ir(program, 1);
	error = execute_compile(program, execute->shader_model, cpu, &execute->quad_vertex_gprs, &bytes);
	if (error == 0) {
		nvrtx_draw_fragment_ir(program, 1);
		error = execute_compile(program, execute->shader_model, cpu + NVRTX_GSP_PAGE_SIZE, &execute->quad_fragment_gprs, &bytes);
	}
	kern_free(program);
	if (error != 0)
		return error;

	/* Succeeded: the programs are in place. */
	execute->quad_vertex = va;
	execute->quad_fragment = va + NVRTX_GSP_PAGE_SIZE;
	execute->programs_ready = true;
	return 0;
}

/* Compiles one quad program into a page: its header, then its code. */
static int
execute_compile(
	struct nvrtx_draw_ir *program,
	uint32_t shader_model,
	uint8_t *cpu,
	uint32_t *gprs,
	uint32_t *bytes)
{
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic diagnostic;
	uint32_t code;
	int error;

	/* Compiled for the owner's shader model. */
	binary = NULL;
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = nvrtx_sm75_compile_ir_sm(&program->ir, shader_model, &binary, &diagnostic);
	if (error != 0) {
		kern_logf("nvrtx: vk: a quad program did not compile (%d)\n", error);
		return error;
	}

	/* The header and the code must fit the page. */
	code = binary->code_words * 4U;
	if (binary->graphics == 0U || NVRTX_SM75_SPH_BYTES + code > NVRTX_GSP_PAGE_SIZE) {
		nvrtx_sm75_binary_free(binary);
		return EINVAL;
	}

	/* Copies them. */
	kern_memcpy(cpu, binary->sph, NVRTX_SM75_SPH_BYTES);
	kern_memcpy(cpu + NVRTX_SM75_SPH_BYTES, binary->code, code);
	*gprs = binary->gpr_count;
	*bytes = NVRTX_SM75_SPH_BYTES + code;
	nvrtx_sm75_binary_free(binary);

	/* Succeeded: the program is in place. */
	return 0;
}

/*
 * Starts a push buffer in a slot whose run has ended: the current slot,
 * another one made earlier, a new one while one is to be had, or else the
 * slot of the oldest run once it ended.  Returns 0, or the error of the
 * wait for that run.
 */
static int
execute_start(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute)
{
	struct execute_slot *slot;
	uint32_t chosen;
	uint32_t index;
	uint32_t at;
	bool passed;
	int error;

	/* The current slot when its run ended, else another made one whose run ended. */
	chosen = EXECUTE_SLOTS;
	for (index = 0U; index < EXECUTE_SLOTS && chosen == EXECUTE_SLOTS; index++) {
		at = (execute->slot + index) % EXECUTE_SLOTS;
		slot = &execute->slots[at];
		if (slot->token == NULL)
			continue;
		passed = nvrtx_vk_gpu_passed(session->owner, &slot->ticket);
		if (passed)
			chosen = at;
	}

	/* Else a new slot, while one is to be had (one that cannot be made is waited for instead). */
	for (index = 0U; index < EXECUTE_SLOTS && chosen == EXECUTE_SLOTS; index++) {
		if (execute->slots[index].token != NULL)
			continue;
		error = execute_slot_make(session, &execute->slots[index]);
		if (error == 0)
			chosen = index;
		break;
	}

	/* Else the slot of the oldest run, once it ended (the first slot is always made). */
	if (chosen == EXECUTE_SLOTS) {
		chosen = 0U;
		for (index = 1U; index < EXECUTE_SLOTS; index++) {
			if (execute->slots[index].token == NULL)
				continue;
			if (execute->slots[index].ticket.submitted_us < execute->slots[chosen].ticket.submitted_us)
				chosen = index;
		}
		error = nvrtx_vk_gpu_wait(session->owner, &execute->slots[chosen].ticket);
		if (error != 0)
			return error;
	}

	/* The slot's run is over: counted and forgotten; the push buffer is written there. */
	slot = &execute->slots[chosen];
	nvrtx_vk_gpu_retire(session->owner, &slot->ticket);
	execute->slot = chosen;
	execute->cpu = slot->cpu;
	execute->va = slot->va;
	execute_begin(execute, 0U);

	/* Succeeded: the push buffer is begun. */
	return 0;
}

/*
 * Tells whether the next push buffer goes on in the current slot after the
 * last one submitted there: the slot holds a push buffer, and its push,
 * constant and pool parts keep room for two records of the most they take
 * (one the push buffer's first, so it does not run nearly empty).
 */
static bool
execute_continues(
	const struct nvrtx_vk_execute *execute)
{
	/* Nothing submitted in the slot yet: a slot is taken. */
	if (execute->at == 0U)
		return false;

	/* Room in each part. */
	if (execute->at + 2U * EXECUTE_RECORD_ROOM > EXECUTE_PUSH_BYTES)
		return false;
	if (execute->constants + 2U * NVRTX_VK_REGIONS_MAX * EXECUTE_CONSTANTS_SLOT > EXECUTE_CONSTANTS_BYTES)
		return false;
	if (execute->headers + 2U * (NVRTX_VK_REGIONS_MAX + EXECUTE_STAGES * NVRTX_VK_TEXTURES_MAX) > EXECUTE_POOL_ENTRIES)
		return false;

	/* Succeeded: the slot takes another push buffer. */
	return true;
}

/*
 * Begins a push buffer in the current slot at segment of its push part:
 * the 3D class's state a queue starts with (once on a channel that keeps
 * it from one run to the next; its vertex stream substitute is the first
 * slot's zero page, which lives as long as the session's runs), the
 * header and sampler pools, and the shader caches emptied.  At segment 0
 * nothing of the slot's parts is used yet; after it, the parts the runs
 * before use stay theirs.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_queue.c
 * (nvk_queue_init_context_state) and src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_push_draw_state_init).
 */
static void
execute_begin(
	struct nvrtx_vk_execute *execute,
	size_t segment)
{
	uint8_t *push;

	/* The state and the pools. */
	push = execute->cpu + EXECUTE_PUSH_AT;
	execute->segment = segment;
	execute->at = segment;
	if (!execute->class_ready)
		execute->at = nvrtx_push_draw_init_class(push, execute->at, execute->va + EXECUTE_ZERO_AT, execute->threed_class);
	execute->at = nvrtx_push_draw_textures(push, execute->at, execute->va + EXECUTE_HEADERS_AT, EXECUTE_POOL_ENTRIES,
					       execute->va + EXECUTE_SAMPLERS_AT, EXECUTE_POOL_ENTRIES);

	/* The programs and constants the CPU wrote since the last run are read anew, once for every draw of this push buffer. */
	execute->at = nvrtx_push_draw_invalidate(push, execute->at);

	/* No texture handle of an earlier push buffer is good here; at a slot's start nothing of its parts is used. */
	execute->reuse.pools++;
	if (segment == 0U) {
		execute->constants = 0U;
		execute->headers = 0U;
		execute->samplers = 0U;
	}
	execute->draws = 0U;
	execute->quads = 0U;
	execute->started = true;
}

/*
 * Puts the push buffer written so far on the GPU without waiting for it;
 * no push buffer is begun after it.  Returns the submission's error.
 */
static int
execute_submit(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute)
{
	struct execute_slot *slot;
	int error;

	/* Counts its draws, then submits it; the slot keeps its run. */
	slot = &execute->slots[execute->slot];
	nvrtx_vk_gpu_drawn(session->owner, execute->draws, execute->quads);
	error = nvrtx_vk_gpu_submit(session->owner, execute->va + EXECUTE_PUSH_AT + execute->segment, (uint32_t)(execute->at - execute->segment),
				    &slot->ticket);
	if (error != 0)
		return error;

	/* Succeeded: the GPU runs it; the class's state it set stays for the next run on a channel that keeps it. */
	execute->started = false;
	execute->class_ready = nvrtx_vk_gpu_state_kept(session->owner);
	return 0;
}

/*
 * Puts the push buffer written so far on the GPU, then starts the next
 * one.  Returns the submission's error or the wait's for a slot.
 */
static int
execute_flush(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute)
{
	int error;

	/* The push buffer, then the next one. */
	error = execute_submit(session, execute);
	if (error == 0)
		error = execute_start(session, execute);
	if (error != 0)
		return error;

	/* Succeeded: the next push buffer is begun. */
	return 0;
}

/*
 * Puts the push buffer written so far on the GPU and waits until the
 * session's runs have ended (a copy by the CPU follows), then starts the
 * next one.  Returns the error of the submission or the wait.
 */
static int
execute_drain(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute)
{
	int error;

	/* The push buffer, the wait for it, then the next one. */
	error = execute_submit(session, execute);
	if (error == 0)
		error = nvrtx_vk_gpu_idle(session->owner);
	if (error == 0)
		error = execute_start(session, execute);
	if (error != 0)
		return error;

	/* Succeeded: the GPU has done the work before; the next push buffer is begun. */
	return 0;
}

/*
 * Makes room for one record: runs the push buffer when the record could
 * pass its end or the run's other parts are nearly used, or when it holds
 * its share of draws (fewer for a command buffer's first).  The target is
 * written again in the next push buffer.
 */
static int
execute_room(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state)
{
	bool full;
	int error;

	/* Full when the push buffer, the constants or the pools could not take a record. */
	full = false;
	if (execute->at + EXECUTE_RECORD_ROOM > EXECUTE_PUSH_BYTES)
		full = true;
	if (execute->constants + NVRTX_VK_REGIONS_MAX * EXECUTE_CONSTANTS_SLOT > EXECUTE_CONSTANTS_BYTES)
		full = true;
	if (execute->headers + NVRTX_VK_REGIONS_MAX + EXECUTE_STAGES * NVRTX_VK_TEXTURES_MAX > EXECUTE_POOL_ENTRIES)
		full = true;

	/* The command buffer's first push buffer goes to the idle GPU after a few draws, the others after more. */
	if (execute->early && execute->draws >= EXECUTE_FIRST_DRAWS)
		full = true;
	if (execute->draws >= EXECUTE_RUN_DRAWS)
		full = true;
	if (!full)
		return 0;

	/* Runs what there is; the push buffers after it fill. */
	error = execute_flush(session, execute);
	if (error != 0)
		return error;
	execute->early = false;

	/* Succeeded: the next push buffer has room; its target is written anew. */
	state->target_written = false;
	return 0;
}

/* Runs one record. */
static int
execute_record(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	struct nvrtx_vk_query_place place;
	bool graphics;
	int error;

	/* The record's command. */
	error = 0;
	switch (record->opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
	case GPU_OP_CMD_SET_VIEWPORT:
	case GPU_OP_CMD_SET_SCISSOR:
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
	case GPU_OP_CMD_PUSH_CONSTANTS:
	case GPU_OP_CMD_SET_DEPTH_BIAS:
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
		execute_bind(state, record);
		break;
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
		error = execute_clear_attachments(session, execute, state, record);
		break;
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
		error = execute_begin_pass(session, execute, state, record);
		break;
	case GPU_OP_CMD_END_RENDER_PASS:
		error = execute_end_pass(session, execute, state);
		break;
	case GPU_OP_CMD_DRAW:
	case GPU_OP_CMD_DRAW_INDEXED:
	case GPU_OP_CMD_DRAW_INDIRECT:
	case GPU_OP_CMD_DRAW_INDEXED_INDIRECT:
		error = execute_views(session, execute, state, record);
		break;
	case GPU_OP_CMD_DISPATCH:
	case GPU_OP_CMD_DISPATCH_BASE:
		error = execute_dispatch(execute, state, record);
		break;
	case GPU_OP_CMD_DISPATCH_INDIRECT:
		error = execute_dispatch_indirect(session, execute, state, record);
		break;
	case GPU_OP_CMD_PIPELINE_BARRIER:
		execute_after_dispatch(execute, state);
		break;
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_RESOLVE_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_UPDATE_BUFFER:
	case GPU_OP_CMD_FILL_BUFFER:
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
	case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
	case GPU_OP_CMD_COPY_QUERY_POOL_RESULTS:
		error = execute_transfer(session, execute, state, record);
		break;
	case GPU_OP_CMD_DECODE_VIDEO:
		error = execute_decode(session, execute, state, record);
		break;
	case GPU_OP_CMD_BEGIN_QUERY:
	case GPU_OP_CMD_END_QUERY:
	case GPU_OP_CMD_RESET_QUERY_POOL:
	case GPU_OP_CMD_WRITE_TIMESTAMP:
		/* An occlusion or timestamp query is the GPU's; a result status query the CPU's. */
		graphics = nvrtx_vk_video_query_place(record, &place);
		if (graphics) {
			error = execute_query(session, execute, state, record->opcode, &place);
			if (error == 0 && record->opcode == GPU_OP_CMD_END_QUERY)
				error = execute_query_views(execute, state, &place);
			break;
		}
		execute->kept.valid = false;
		error = nvrtx_vk_video_run(session, record);
		break;
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
	case GPU_OP_CMD_END_VIDEO_CODING:
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
		execute->kept.valid = false;
		error = nvrtx_vk_video_run(session, record);
		break;
	default:
		break;
	}

	/* Reports a record that could not run. */
	if (error != 0)
		return error;

	/* Succeeded: the record ran (or is bound for the draws after it). */
	return 0;
}

/*
 * Runs a query command of an occlusion or timestamp pool on the GPU: a
 * begin reports the samples passing so far (counting on), an end reports
 * them again (counting off) and makes the query available, a timestamp
 * reports the time once its stages are done and makes the query
 * available, a reset makes its queries unavailable and waits for that
 * (so an availability written after it is not overtaken), a run of the
 * push buffer at a time when there are many.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_query_pool.c
 * (nvk_cmd_begin_end_query, nvk_CmdWriteTimestamp2, nvk_CmdResetQueryPool).
 */
static int
execute_query(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	uint32_t opcode,
	const struct nvrtx_vk_query_place *place)
{
	uint8_t *push;
	uint32_t query;
	int error;

	/* A begin or an end of an occlusion query, a timestamp. */
	push = execute->cpu + EXECUTE_PUSH_AT;
	if (opcode == GPU_OP_CMD_BEGIN_QUERY) {
		execute->at = nvrtx_exec_query_occlusion(push, execute->at, place->reports, true);
		return 0;
	}
	if (opcode == GPU_OP_CMD_END_QUERY) {
		execute->at = nvrtx_exec_query_occlusion(push, execute->at, place->reports + EXECUTE_QUERY_REPORT_BYTES, false);
		execute->at = nvrtx_exec_query_available(push, execute->at, place->available, 1U, true);
		return 0;
	}
	if (opcode == GPU_OP_CMD_WRITE_TIMESTAMP) {
		execute->at = nvrtx_exec_query_timestamp(push, execute->at, place->reports, place->stages);
		execute->at = nvrtx_exec_query_available(push, execute->at, place->available, 1U, false);
		return 0;
	}

	/* A reset: each query unavailable, with room for a group of them at a time. */
	for (query = 0U; query < place->count; query++) {
		if ((query % EXECUTE_QUERY_GROUP) == 0U) {
			error = execute_surface_room(session, execute, state);
			if (error != 0)
				return error;
			push = execute->cpu + EXECUTE_PUSH_AT;
		}
		execute->at = nvrtx_exec_query_available(push, execute->at, place->available + (uint64_t)query * 4U, 0U, false);
	}

	/* Succeeded: the GPU waits for the words. */
	execute->at = nvrtx_exec_barrier(push, execute->at);
	return 0;
}

/* Keeps what a binding record binds for the draws after it. */
static void
execute_bind(
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	uint32_t index;

	/* The state each command binds. */
	switch (record->opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
		if (record->values[0] == VK_PIPELINE_BIND_POINT_COMPUTE)
			state->kernel = record->objects[0]->payload;
		else
			state->pipeline = record->objects[0]->payload;
		break;
	case GPU_OP_CMD_SET_VIEWPORT:
		kern_memcpy(state->viewport, record->values, sizeof(state->viewport));
		state->viewport_set = true;
		break;
	case GPU_OP_CMD_SET_SCISSOR:
		state->scissor = record->rect;
		state->scissor_set = true;
		break;
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		execute_bind_sets(state, record);
		break;
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
		for (index = 0U; index < record->object_count; index++) {
			state->buffers[record->first + index] = record->objects[index]->payload;
			state->offsets[record->first + index] = record->offsets[index];
		}
		break;
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
		state->index_buffer = record->objects[0]->payload;
		state->index_offset = record->offsets[0];
		state->index_type = record->values[0];
		break;
	case GPU_OP_CMD_PUSH_CONSTANTS:
		kern_memcpy((uint8_t *)state->push + record->first, state->push_bytes + record->push_at, record->count);
		if (record->first + record->count > state->push_end)
			state->push_end = record->first + record->count;
		break;
	case GPU_OP_CMD_SET_DEPTH_BIAS:
		kern_memcpy(state->depth_bias, record->values, sizeof(state->depth_bias));
		break;
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
		kern_memcpy(state->blend_constants, record->values, sizeof(state->blend_constants));
		break;
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
		execute_bind_stencil(state, record);
		break;
	default:
		break;
	}
}

/*
 * Keeps the sets a bind binds at its bind point (a null one leaves its
 * slot as it was) and gives each dynamic binding of them its offset: the
 * bind's offsets in the order of the sets, then of their binding numbers,
 * one for each descriptor (only a binding's first descriptor is read).
 */
static void
execute_bind_sets(
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_descriptor_set **sets;
	const struct nvrtx_vk_descriptor_set *set;
	uint32_t (*dynamic)[NVRTX_VK_BINDINGS_MAX];
	uint32_t index;
	uint32_t listed;
	uint32_t binding;
	uint32_t taken;
	uint32_t slot;

	/* The bind point's sets. */
	sets = state->sets;
	dynamic = state->dynamic;
	if (record->values[0] == VK_PIPELINE_BIND_POINT_COMPUTE) {
		sets = state->kernel_sets;
		dynamic = state->kernel_dynamic;
	}

	/* Each set the bind names, each dynamic binding of it in order. */
	taken = 0U;
	for (index = 0U; index < record->object_count; index++) {
		if (record->objects[index] == NULL)
			continue;
		set = record->objects[index]->payload;
		slot = record->first + index;
		sets[slot] = set;
		for (listed = 0U; listed < set->dynamic_binding_count; listed++) {
			binding = set->dynamic_bindings[listed];
			if (taken < record->dynamic_count)
				dynamic[slot][binding] = record->dynamic[taken];
			taken += set->slots[binding].dynamic;
		}
	}
}

/*
 * Runs a dispatch of the compute pipeline bound: its constant buffer 0
 * (the push constants, the storage table, the group counts) and its QMD
 * (the program, the grid and the workgroup, the shared memory, the
 * registers, the barriers, constant buffer 0 and the uniform buffers from
 * 1) in two slots of the run's constants, then the launch on the compute
 * subchannel.  A dispatch of no group does nothing.  Returns EINVAL
 * without a pipeline, for a buffer the program uses that is not bound or
 * a grid a QMD cannot hold, or ENOSPC without room for the slots.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_dispatch.c
 * (nvk_cmd_upload_qmd, nvk_CmdDispatchBase).
 */
static int
execute_dispatch(
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_kernel *kernel;
	struct nvrtx_qmd_info info;
	uint8_t *root;
	uint8_t *qmd;
	uint64_t root_va;
	uint64_t qmd_va;
	uint64_t va;
	uint64_t range;
	uint32_t groups_bytes;
	uint32_t entry;
	uint32_t index;
	uint32_t size;
	size_t length;
	int error;

	/* A compute pipeline; nothing to run for no group. */
	if (state->kernel == NULL)
		return EINVAL;
	kernel = &state->kernel->kernel;

	/* A dispatch from a base other than zero needs a pipeline made for one (the API's rule). */
	if (state->kernel->dispatch_base == 0U &&
	    (record->values[3] != 0U || record->values[4] != 0U || record->values[5] != 0U))
		return EINVAL;
	if (record->values[0] == 0U || record->values[1] == 0U || record->values[2] == 0U)
		return 0;

	/* Constant buffer 0: the push constants, then each storage buffer's entry (the group counts' in the slot). */
	error = execute_constants(execute, &root, &root_va);
	if (error != 0)
		return error;
	kern_memcpy(root + NVRTX_SM75_CB_PUSH, state->push, sizeof(state->push));
	nvrtx_put32(root, EXECUTE_GROUPS_AT, record->values[0]);
	nvrtx_put32(root, EXECUTE_GROUPS_AT + 4U, record->values[1]);
	nvrtx_put32(root, EXECUTE_GROUPS_AT + 8U, record->values[2]);
	nvrtx_put32(root, EXECUTE_GROUPS_AT + 12U, record->values[3]);
	nvrtx_put32(root, EXECUTE_GROUPS_AT + 16U, record->values[4]);
	nvrtx_put32(root, EXECUTE_GROUPS_AT + 20U, record->values[5]);
	groups_bytes = EXECUTE_GROUPS_BYTES;
	if (state->kernel->dispatch_base != 0U)
		groups_bytes = DRV_GPU_SPIRV_SYSTEM_BASE_BYTES;
	for (index = 0U; index < kernel->storage_count; index++) {
		va = root_va + EXECUTE_GROUPS_AT;
		range = groups_bytes;
		if (kernel->storage[index].set != NVRTX_VK_SYSTEM_SET) {
			error = execute_storage(state->kernel_sets, (const uint32_t (*)[NVRTX_VK_BINDINGS_MAX])state->kernel_dynamic, &kernel->storage[index], &va, &range);
			if (error != 0)
				return error;
		}
		entry = NVRTX_SM75_CB_STORAGE + index * NVRTX_SM75_CB_STORAGE_ENTRY;
		nvrtx_put32(root, entry, (uint32_t)va);
		nvrtx_put32(root, entry + 4U, (uint32_t)(va >> 32));
		nvrtx_put32(root, entry + NVRTX_SM75_CB_STORAGE_SIZE, (uint32_t)range);
	}

	/* The QMD: the program, the grid and the workgroup, its memory, and the constant buffers. */
	kern_memset(&info, 0, sizeof(info));
	info.program = kernel->va;
	info.groups[0] = record->values[0];
	info.groups[1] = record->values[1];
	info.groups[2] = record->values[2];
	info.local_size[0] = kernel->local_size[0];
	info.local_size[1] = kernel->local_size[1];
	info.local_size[2] = kernel->local_size[2];
	info.gpr_count = kernel->gprs;
	info.shared_bytes = kernel->shared_bytes;
	info.barrier_count = kernel->barrier_count;
	info.cbuf_address[NVRTX_SM75_CB_ROOT] = root_va;
	info.cbuf_size[NVRTX_SM75_CB_ROOT] = EXECUTE_CONSTANTS_SIZE;
	for (index = 0U; index < kernel->uniform_count; index++) {
		error = execute_kernel_uniform(state, &kernel->uniforms[index], &va, &size);
		if (error != 0)
			return error;
		info.cbuf_address[NVRTX_SM75_CB_UNIFORM_FIRST + index] = va;
		info.cbuf_size[NVRTX_SM75_CB_UNIFORM_FIRST + index] = size;
	}
	error = execute_constants(execute, &qmd, &qmd_va);
	if (error != 0)
		return error;
	error = nvrtx_qmd_build_for((uint32_t *)(void *)qmd, &info, &execute->compute_kind);
	if (error != 0)
		return error;

	/* The launch, after the work before (its invalidation waits for idle). */
	length = nvrtx_push_compute_launch_for(execute->cpu + EXECUTE_PUSH_AT + execute->at, qmd_va, &execute->compute_kind);
	execute->at += length;

	/* Succeeded: the GPU runs it; what the shadow held may be written. */
	execute->kept.valid = false;
	state->dispatched = true;
	return 0;
}

/*
 * Gives a program's entry of the storage table from the sets and dynamic
 * offsets of its bind point: the buffer's address (the descriptor's
 * offset, and a dynamic binding's offset from the bind) and its range (the
 * rest of the buffer for the whole size).  Returns EINVAL for a descriptor
 * without a buffer or an offset past its end.
 */
static int
execute_storage(
	const struct nvrtx_vk_descriptor_set *const *sets,
	const uint32_t (*dynamic)[NVRTX_VK_BINDINGS_MAX],
	const struct nvrtx_vk_texture_source *source,
	uint64_t *va,
	uint64_t *range)
{
	const struct nvrtx_vk_descriptor_set *set;
	const struct nvrtx_vk_descriptor_slot *slot;
	const struct nvrtx_vk_buffer *buffer;
	uint64_t offset;
	uint64_t start;
	uint8_t *cpu;
	int error;

	/* The descriptor's buffer. */
	set = sets[source->set];
	if (set == NULL)
		return EINVAL;
	slot = nvrtx_vk_descriptor_find(set, source->binding, source->element);
	if (slot == NULL || slot->buffer == NULL)
		return EINVAL;
	buffer = slot->buffer->payload;
	error = nvrtx_vk_buffer_address(buffer, &start, &cpu);
	if (error != 0)
		return error;

	/* Its start and its range. */
	offset = slot->offset;
	if (slot->dynamic != 0U)
		offset += dynamic[source->set][source->binding];
	if (offset >= buffer->size)
		return EINVAL;
	*range = slot->range;
	if (*range == VK_WHOLE_SIZE || *range > buffer->size - offset)
		*range = buffer->size - offset;
	*va = start + offset;

	/* Succeeded: the buffer's place. */
	return 0;
}

/*
 * Gives a compute program's uniform buffer as a QMD binds it: its address
 * (on the boundary a constant buffer starts at) and its size in whole
 * 256-byte units up to 64 KiB.  Returns EINVAL for a binding without a
 * buffer or a start off the boundary or past its end.
 */
static int
execute_kernel_uniform(
	const struct execute_state *state,
	const struct nvrtx_vk_texture_source *source,
	uint64_t *va,
	uint32_t *size)
{
	const struct nvrtx_vk_descriptor_set *set;
	const struct nvrtx_vk_descriptor_slot *slot;
	const struct nvrtx_vk_buffer *buffer;
	uint64_t offset;
	uint64_t range;
	uint64_t start;
	uint8_t *cpu;
	int error;

	/* The descriptor's buffer. */
	set = state->kernel_sets[source->set];
	if (set == NULL)
		return EINVAL;
	slot = nvrtx_vk_descriptor_find(set, source->binding, source->element);
	if (slot == NULL || slot->buffer == NULL)
		return EINVAL;
	buffer = slot->buffer->payload;
	error = nvrtx_vk_buffer_address(buffer, &start, &cpu);
	if (error != 0)
		return error;

	/* Its start, on the boundary. */
	offset = slot->offset;
	if (slot->dynamic != 0U)
		offset += state->kernel_dynamic[source->set][source->binding];
	if (offset >= buffer->size || ((start + offset) % NVRTX_QMD_CBUF_ALIGN) != 0U)
		return EINVAL;

	/* Its range in whole units (the bytes after it are read as they are), at most 64 KiB. */
	range = slot->range;
	if (range == VK_WHOLE_SIZE || range > buffer->size - offset)
		range = buffer->size - offset;
	range = (range + NVRTX_QMD_CBUF_ALIGN - 1U) & ~(uint64_t)(NVRTX_QMD_CBUF_ALIGN - 1U);
	if (range > EXECUTE_UNIFORM_BYTES_MAX)
		range = EXECUTE_UNIFORM_BYTES_MAX;
	*va = start + offset;
	*size = (uint32_t)range;

	/* Succeeded: the buffer's place. */
	return 0;
}

/*
 * Runs a pipeline barrier: after a dispatch, the GPU waits for its idle
 * and the caches the work after it reads are invalidated; otherwise the
 * commands that need it wait already.
 */
static void
execute_after_dispatch(
	struct nvrtx_vk_execute *execute,
	struct execute_state *state)
{
	/* Nothing ran that the commands after it do not wait for. */
	if (!state->dispatched)
		return;

	/* The idle and the caches. */
	execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);
	state->dispatched = false;
}

/* Keeps a stencil compare mask, write mask or reference for the faces a record names (its low 8 bits). */
static void
execute_bind_stencil(
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	uint32_t *values;
	uint32_t value;

	/* The value the command sets. */
	values = state->stencil_reference;
	if (record->opcode == GPU_OP_CMD_SET_STENCIL_COMPARE_MASK)
		values = state->stencil_compare;
	else if (record->opcode == GPU_OP_CMD_SET_STENCIL_WRITE_MASK)
		values = state->stencil_write;
	value = record->values[1] & 0xffU;

	/* The front face's and the back face's. */
	if ((record->values[0] & VK_STENCIL_FACE_FRONT_BIT) != 0U)
		values[0] = value;
	if ((record->values[0] & VK_STENCIL_FACE_BACK_BIT) != 0U)
		values[1] = value;
}

/*
 * Begins a pass: its framebuffer's colour views' images (at the views'
 * levels and first layers) are the targets from now on, with its depth
 * view's surface as the depth target (a pass of a depth attachment alone
 * draws depth only), and a pass whose attachments load by clearing clears
 * their render area to the clear values the record keeps for them.  A
 * pass of several colour targets draws only into block linear ones
 * (ENOTSUP for another: the shadow stands for one).
 */
static int
execute_begin_pass(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_render_pass *pass;
	const struct nvrtx_vk_framebuffer *framebuffer;
	const struct nvrtx_vk_view *view;
	const struct nvrtx_vk_image *image;
	const struct nvrtx_vk_memory *memory;
	const uint32_t (*clears)[4];
	struct nvrtx_exec_target other;
	static const uint32_t no_clear[4];
	const uint32_t *depth_clear;
	uint32_t clear_colours;
	uint32_t attachment;
	uint32_t format;
	uint32_t index;
	uint32_t layer;
	uint32_t layers;
	bool colour;
	bool depth;
	bool clear_depth;
	bool clear_stencil;
	int32_t x1;
	int32_t y1;
	int error;

	pass = record->objects[0]->payload;
	framebuffer = record->objects[1]->payload;
	clears = record->regions;

	/* A pass that writes no colour or depth attachment draws nothing that stays. */
	state->in_pass = true;
	state->pass = pass;
	state->framebuffer = framebuffer;
	state->target_written = false;
	state->area = record->rect;
	state->shadowed = false;
	state->backing = NULL;
	state->known = false;
	kern_memset(&state->target, 0, sizeof(state->target));
	colour = false;
	if (pass->colour != 0U && pass->colour_attachment < framebuffer->view_count)
		colour = true;
	depth = false;
	if (pass->depth != 0U && pass->depth_attachment < framebuffer->view_count)
		depth = true;
	if (!colour && !depth) {
		execute->kept.valid = false;
		return 0;
	}

	/* The target: the colour view's image at its level and first layer, in the view's format; its memory's backing. */
	if (colour) {
		view = framebuffer->views[pass->colour_attachment]->payload;
		error = execute_image_target(view->image->payload, view->base_level, view->base_layer, view->format, &state->target);
		if (error != 0)
			return error;
		image = view->image->payload;
		if (image->memory != NULL) {
			memory = image->memory->payload;
			if (memory->attached)
				state->backing = memory->backing;
		}
		state->target.colour_count = pass->colour;
	}

	/* The other colour targets: each colour view's image, block linear; an unused attachment's writes go nowhere. */
	for (index = 1U; colour && index < pass->colour && index < NVRTX_EXEC_COLOURS; index++) {
		attachment = pass->colour_attachments[index];
		if (attachment == VK_ATTACHMENT_UNUSED || attachment >= framebuffer->view_count)
			continue;
		view = framebuffer->views[attachment]->payload;
		error = execute_image_target(view->image->payload, view->base_level, view->base_layer, view->format, &other);
		if (error != 0)
			return error;
		if (other.block_linear == 0U || state->target.block_linear == 0U)
			return ENOTSUP;
		state->target.others[index - 1U].address = other.address;
		state->target.others[index - 1U].pitch = other.pitch;
		state->target.others[index - 1U].height = other.height;
		state->target.others[index - 1U].format = other.format;
	}

	/* The depth target: the depth view's surface (a pass of no colour target its size). */
	if (depth) {
		view = framebuffer->views[pass->depth_attachment]->payload;
		error = execute_depth_target(session, view->image->payload, view->base_level, view->base_layer, &state->target);
		if (error != 0)
			return error;
	}

	/*
	 * A layered pass: every target block linear, each its layers from its
	 * view's first, its image's layers' bytes apart.
	 */
	layers = framebuffer->layers;
	if (layers == 0U)
		layers = 1U;
	if (pass->view_mask != 0U)
		layers = execute_view_layers(pass->view_mask);
	if (layers > 1U) {
		if (colour && state->target.block_linear == 0U)
			return ENOTSUP;
		state->target.layers = layers;
		for (index = 0U; colour && index < pass->colour && index < NVRTX_EXEC_COLOURS; index++) {
			attachment = pass->colour_attachments[index];
			if (attachment == VK_ATTACHMENT_UNUSED || attachment >= framebuffer->view_count)
				continue;
			view = framebuffer->views[attachment]->payload;
			image = view->image->payload;
			if (index == 0U)
				state->target.array_pitch = image->layer_bytes;
			else
				state->target.others[index - 1U].array_pitch = image->layer_bytes;
		}
		if (depth) {
			view = framebuffer->views[pass->depth_attachment]->payload;
			image = view->image->payload;
			state->target.depth_array_pitch = image->layer_bytes;
		}
	}

	/* A pitch-linear colour target with a depth target is drawn in a block-linear shadow; any pass lets what the shadow held go. */
	if (colour && state->target.depth_address != 0U && state->target.block_linear == 0U) {
		error = execute_shadow(session, execute, state, pass->load_op == VK_ATTACHMENT_LOAD_OP_LOAD);
		if (error != 0)
			return error;
	}
	execute->kept.valid = false;

	/* A pass loading by clearing clears its area, within the framebuffer and the image. */
	clear_colours = 0U;
	for (index = 0U; colour && index < pass->colour && index < NVRTX_EXEC_COLOURS; index++) {
		if (pass->colour_attachments[index] != VK_ATTACHMENT_UNUSED && pass->load_ops[index] == VK_ATTACHMENT_LOAD_OP_CLEAR &&
		    (index == 0U || state->target.others[index - 1U].address != 0U))
			clear_colours |= 1U << index;
	}
	clear_depth = (state->target.depth_address != 0U && pass->depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR);
	clear_stencil = (state->target.depth_stencil != 0U && pass->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR);
	if (clear_colours == 0U && !clear_depth && !clear_stencil)
		return 0;
	depth_clear = no_clear;
	if (pass->depth != 0U && pass->depth_attachment < record->region_count)
		depth_clear = clears[pass->depth_attachment];
	x1 = state->area.offset.x + (int32_t)state->area.extent.width;
	y1 = state->area.offset.y + (int32_t)state->area.extent.height;
	if (x1 > (int32_t)framebuffer->width)
		x1 = (int32_t)framebuffer->width;
	if (y1 > (int32_t)framebuffer->height)
		y1 = (int32_t)framebuffer->height;
	if (x1 > (int32_t)state->target.width)
		x1 = (int32_t)state->target.width;
	if (y1 > (int32_t)state->target.height)
		y1 = (int32_t)state->target.height;
	if (state->area.offset.x < 0 || state->area.offset.y < 0 || x1 <= state->area.offset.x || y1 <= state->area.offset.y)
		return 0;

	/* Each layer's clears, with room for them (the target written again in a new push buffer); a multiview pass's views' layers. */
	for (layer = 0U; layer < layers; layer++) {
		if (!execute_layer_drawn(state, layer))
			continue;
		error = execute_surface_room(session, execute, state);
		if (error != 0)
			return error;
		if (!state->target_written) {
			execute->at = nvrtx_exec_target(execute->cpu + EXECUTE_PUSH_AT, execute->at, &state->target);
			state->target_written = true;
		}
		for (index = 0U; index < NVRTX_EXEC_COLOURS; index++) {
			if ((clear_colours & (1U << index)) == 0U)
				continue;
			attachment = pass->colour_attachments[index];

			/* The target's format, by which its clear colour is given. */
			format = state->target.format;
			if (index != 0U)
				format = state->target.others[index - 1U].format;
			execute->at = nvrtx_exec_clear(execute->cpu + EXECUTE_PUSH_AT, execute->at, index, format, layer,
						       attachment < record->region_count ? clears[attachment] : no_clear,
						       (uint32_t)state->area.offset.x, (uint32_t)state->area.offset.y, (uint32_t)x1, (uint32_t)y1);
		}
		if (clear_depth)
			execute->at = nvrtx_exec_clear_depth(execute->cpu + EXECUTE_PUSH_AT, execute->at, layer, depth_clear[0],
							     (uint32_t)state->area.offset.x, (uint32_t)state->area.offset.y, (uint32_t)x1,
							     (uint32_t)y1);
		if (clear_stencil)
			execute->at = nvrtx_exec_clear_stencil(execute->cpu + EXECUTE_PUSH_AT, execute->at, layer, depth_clear[1],
							       (uint32_t)state->area.offset.x, (uint32_t)state->area.offset.y, (uint32_t)x1,
							       (uint32_t)y1);
	}

	/* Succeeded: the pass draws into its target. */
	return 0;
}

/*
 * Puts a block-linear shadow of the pass's pitch-linear colour target in
 * its place (the 3D engine draws with a depth target only into a
 * block-linear colour target 0): the shadow is the executor's, grown after
 * the work before it ran when too small, and a pass that loads its colour
 * starts the shadow with the target's pixels.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_linear_render_copy, rendering a linear image through a tiled shadow
 * copied at the start and the end of rendering).
 */
static int
execute_shadow(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	bool load)
{
	struct execute_quad quad;
	uint8_t *cpu;
	uint64_t bytes;
	uint32_t pitch;
	uint32_t height;
	int error;

	/* The shadow's rows in whole GOBs, its height in whole GOBs. */
	pitch = (state->target.width * nvrtx_vk_format_bytes(state->target.format) + NVRTX_VK_GOB_WIDTH - 1U) & ~(NVRTX_VK_GOB_WIDTH - 1U);
	height = (state->target.height + NVRTX_VK_GOB_HEIGHT - 1U) & ~(NVRTX_VK_GOB_HEIGHT - 1U);
	bytes = (uint64_t)pitch * height;

	/* A shadow too small is replaced once the work that may draw in it has run. */
	if (execute->shadow_bytes < bytes) {
		if (execute->shadow_token != NULL) {
			error = execute_flush(session, execute);
			if (error != 0)
				return error;
			nvrtx_vk_gpu_free(session->owner, execute->shadow_token);
			execute->shadow_token = NULL;
			execute->shadow_bytes = 0U;
		}
		error = nvrtx_vk_gpu_alloc(session->owner, (bytes + NVRTX_GSP_PAGE_SIZE - 1U) & ~(uint64_t)(NVRTX_GSP_PAGE_SIZE - 1U),
					   &execute->shadow_token, &cpu, &execute->shadow_va);
		if (error != 0)
			return error;
		execute->shadow_bytes = bytes;
		execute->kept.valid = false;
	}

	/* The pass draws into the shadow; the target gets it at the pass's end. */
	state->resolve = state->target;
	state->resolve.depth_address = 0U;
	state->target.address = execute->shadow_va;
	state->target.pitch = pitch;
	state->target.block_linear = 1U;
	state->shadowed = true;
	if (!load)
		return 0;

	/*
	 * A loading pass whose render area the shadow already holds as the
	 * target has it (the last pass drawn in it copied it there, and nothing
	 * else wrote the target since) starts without the copy.
	 */
	if (execute_kept_holds(session, execute, state)) {
		state->known = true;
		state->known_x0 = execute->kept.x0;
		state->known_y0 = execute->kept.y0;
		state->known_x1 = execute->kept.x1;
		state->known_y1 = execute->kept.y1;
		return 0;
	}

	/* A loading pass: the target's pixels into the shadow. */
	execute_shadow_copy(&state->target, &state->resolve, 0, 0, (int32_t)state->target.width, (int32_t)state->target.height,
			    &quad);
	quad.target.depth_address = 0U;
	quad.source_tiled = false;
	error = execute_quad(session, execute, state, &quad);
	if (error != 0)
		return error;
	execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);
	state->known = true;
	state->known_x0 = 0;
	state->known_y0 = 0;
	state->known_x1 = (int32_t)state->target.width;
	state->known_y1 = (int32_t)state->target.height;

	/* Succeeded: the shadow holds the target's pixels. */
	return 0;
}

/*
 * Tells whether the shadow holds, as the target has them, the pixels of a
 * loading pass's render area: the last pass drawn in this shadow was of the
 * same memory's target, its region takes the area in, and the
 * node's token is the one it had then (nothing else wrote the memory).
 */
static bool
execute_kept_holds(
	struct nvrtx_vk_session *session,
	const struct nvrtx_vk_execute *execute,
	const struct execute_state *state)
{
	const struct execute_kept *kept;
	uint64_t token;
	int32_t x1;
	int32_t y1;

	/* What the shadow held, for this target in this shadow. */
	kept = &execute->kept;
	if (!kept->valid || state->backing == NULL || kept->backing != state->backing)
		return false;
	if (kept->shadow_va != execute->shadow_va)
		return false;
	if (kept->target.address != state->resolve.address || kept->target.pitch != state->resolve.pitch ||
	    kept->target.width != state->resolve.width || kept->target.height != state->resolve.height ||
	    kept->target.format != state->resolve.format)
		return false;

	/* Its region takes the render area in. */
	x1 = state->area.offset.x + (int32_t)state->area.extent.width;
	y1 = state->area.offset.y + (int32_t)state->area.extent.height;
	if (state->area.offset.x < kept->x0 || state->area.offset.y < kept->y0 || x1 > kept->x1 || y1 > kept->y1)
		return false;

	/* Nothing else wrote the memory since. */
	token = nvrtx_vk_gpu_quiet(session->owner, state->backing);
	if (token == 0U || token != kept->token)
		return false;

	/* Succeeded: the copy is not needed. */
	return true;
}

/*
 * Ends a pass: its work is ordered before what follows, and a pass drawn in
 * a shadow copies its render area from the shadow to the target.
 */
static int
execute_end_pass(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state)
{
	struct execute_kept *kept;
	struct execute_quad quad;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	int error;

	/* The pass's work before what follows; its multisampled attachments resolved. */
	state->in_pass = false;
	execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);
	error = execute_resolve_pass(session, execute, state);
	if (error != 0)
		return error;
	if (!state->shadowed)
		return 0;
	state->shadowed = false;

	/* The render area, within the target. */
	x0 = state->area.offset.x;
	y0 = state->area.offset.y;
	x1 = x0 + (int32_t)state->area.extent.width;
	y1 = y0 + (int32_t)state->area.extent.height;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int32_t)state->resolve.width)
		x1 = (int32_t)state->resolve.width;
	if (y1 > (int32_t)state->resolve.height)
		y1 = (int32_t)state->resolve.height;
	if (x1 <= x0 || y1 <= y0)
		return 0;

	/* The shadow's area onto the target, then ordered before what follows. */
	execute_shadow_copy(&state->resolve, &state->target, x0, y0, x1, y1, &quad);
	quad.source_tiled = true;
	error = execute_quad(session, execute, state, &quad);
	if (error != 0)
		return error;
	state->target_written = false;
	execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);

	/*
	 * The shadow now holds what the target has in the area, and in what
	 * it held at the pass's start (which takes the area in); kept for the
	 * next loading pass of the image while nothing else writes it.
	 */
	kept = &execute->kept;
	kept->valid = false;
	if (state->backing != NULL) {
		kept->token = nvrtx_vk_gpu_quiet(session->owner, state->backing);
		kept->valid = (kept->token != 0U);
	}
	kept->backing = state->backing;
	kept->target = state->resolve;
	kept->shadow_va = execute->shadow_va;
	kept->x0 = x0;
	kept->y0 = y0;
	kept->x1 = x1;
	kept->y1 = y1;
	if (state->known && state->known_x0 <= x0 && state->known_y0 <= y0 && state->known_x1 >= x1 && state->known_y1 >= y1) {
		kept->x0 = state->known_x0;
		kept->y0 = state->known_y0;
		kept->x1 = state->known_x1;
		kept->y1 = state->known_y1;
	}

	/* Succeeded: the target has the pass's pixels. */
	return 0;
}

/* Describes a one-to-one, nearest copy of a region between two colour targets of one size as a quad. */
static void
execute_shadow_copy(
	const struct nvrtx_exec_target *to,
	const struct nvrtx_exec_target *from,
	int32_t x0,
	int32_t y0,
	int32_t x1,
	int32_t y1,
	struct execute_quad *quad)
{
	/* The destination and its region. */
	kern_memset(quad, 0, sizeof(*quad));
	quad->target = *to;
	quad->target.depth_address = 0U;
	quad->x0 = x0;
	quad->y0 = y0;
	quad->x1 = x1;
	quad->y1 = y1;

	/* The source, the same region, nearest. */
	quad->source = from->address;
	quad->source_width = from->width;
	quad->source_height = from->height;
	quad->source_pitch = from->pitch;
	quad->source_format = from->format;
	quad->u0 = x0;
	quad->v0 = y0;
	quad->u1 = x1;
	quad->v1 = y1;
	quad->filter = VK_FILTER_NEAREST;

	/* Bit for bit (an sRGB target's pixels are not decoded and encoded again). */
	execute_raw_quad(quad, nvrtx_vk_format_bytes(from->format));
}

/*
 * Runs a draw (indexed or not; values are vkCmdDraw's four counts or
 * vkCmdDrawIndexed's five, in their order) in a pass: the target when not
 * written yet, its constants (the push constants and the texture handles of the
 * fragment program's samples), the viewport and scissor, the culling, the
 * depth test, the blend, the vertex input, the index buffer, the uniform
 * buffers, the programs and the draw.  Right after another application
 * draw of the push buffer (nothing written between), only what differs
 * from that draw's state is written, as the GPU keeps the rest; the
 * constants then go into the same constant buffer 0 by loads in the push
 * buffer, which the draws before do not see.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_cmd_flush_gfx_dynamic_state, nvk_cmd_flush_gfx_root_desc: the
 * state written when dirty, constant buffer 0 loaded in the push buffer).
 */
static int
execute_draw(
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	bool indexed,
	const uint32_t *values)
{
	const struct nvrtx_vk_pipeline *pipeline;
	struct execute_reuse *reuse;
	struct execute_held *held;
	struct nvrtx_exec_draw_args draw;
	struct nvrtx_exec_vertex vertex;
	const uint32_t *viewport;
	const VkRect2D *scissor;
	struct execute_stage_words words[EXECUTE_STAGES];
	uint8_t *push;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	uint64_t index_va;
	uint8_t *index_cpu;
	uint32_t index;
	uint32_t changes;
	uint32_t first_instance;
	bool known;
	bool same;
	int differs;
	int error;

	/* A draw in a pass with a target, with a pipeline; an indexed one with an index buffer. */
	pipeline = state->pipeline;
	if (!state->in_pass || pipeline == NULL)
		return EINVAL;
	if (indexed && state->index_buffer == NULL)
		return EINVAL;
	if ((state->target.address == 0U && state->target.depth_address == 0U) || values[0] == 0U || values[1] == 0U)
		return 0;
	index_va = 0U;
	if (indexed) {
		error = nvrtx_vk_buffer_address(state->index_buffer, &index_va, &index_cpu);
		if (error != 0)
			return error;
		if (state->index_offset >= state->index_buffer->size)
			return EINVAL;
	}

	/* The viewport and the scissor: dynamic or the pipeline's, the scissor within the target; nothing to draw outside it. */
	viewport = pipeline->viewport;
	if (pipeline->dynamic_viewport != 0U && state->viewport_set)
		viewport = state->viewport;
	scissor = &pipeline->scissor;
	if (pipeline->dynamic_scissor != 0U && state->scissor_set)
		scissor = &state->scissor;
	x0 = scissor->offset.x;
	y0 = scissor->offset.y;
	x1 = x0 + (int32_t)scissor->extent.width;
	y1 = y0 + (int32_t)scissor->extent.height;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int32_t)state->target.width)
		x1 = (int32_t)state->target.width;
	if (y1 > (int32_t)state->target.height)
		y1 = (int32_t)state->target.height;
	if (x1 <= x0 || y1 <= y0)
		return 0;

	/* The handles of each program's textures and its storage table. */
	kern_memset(words, 0, sizeof(words));
	error = execute_textures(execute, state, &pipeline->vertex, words[EXECUTE_STAGE_VERTEX].handles);
	if (error == 0)
		error = execute_textures(execute, state, &pipeline->fragment, words[EXECUTE_STAGE_FRAGMENT].handles);
	if (error == 0)
		error = execute_textures(execute, state, &pipeline->geometry, words[EXECUTE_STAGE_GEOMETRY].handles);
	if (error == 0)
		error = execute_stage_storage(state, &pipeline->vertex, &words[EXECUTE_STAGE_VERTEX]);
	if (error == 0)
		error = execute_stage_storage(state, &pipeline->fragment, &words[EXECUTE_STAGE_FRAGMENT]);
	if (error == 0)
		error = execute_stage_storage(state, &pipeline->geometry, &words[EXECUTE_STAGE_GEOMETRY]);
	if (error != 0)
		return error;

	/* What the GPU holds is known right after the push buffer's last application draw. */
	held = &execute->held;
	reuse = &execute->reuse;
	known = false;
	if (held->pools == reuse->pools && held->at == execute->at)
		known = true;

	/* The target, once in a push buffer (what the GPU held is then written anew). */
	push = execute->cpu + EXECUTE_PUSH_AT;
	if (!state->target_written) {
		execute->at = nvrtx_exec_target(push, execute->at, &state->target);
		state->target_written = true;
		known = false;
	}

	/* The constants, the draw's first instance among them (for gl_InstanceIndex). */
	first_instance = values[3];
	if (indexed)
		first_instance = values[4];
	error = execute_draw_constants(execute, state, words, first_instance, known);
	if (error != 0)
		return error;

	/* The viewport, when it changed. */
	same = known;
	for (index = 0U; index < 6U && same; index++) {
		if (held->viewport[index] != viewport[index])
			same = false;
	}
	if (!same) {
		if (reuse->viewport_bytes == 0U || kern_memcmp(reuse->viewport_in, viewport, sizeof(reuse->viewport_in)) != 0) {
			kern_memcpy(reuse->viewport_in, viewport, sizeof(reuse->viewport_in));
			reuse->viewport_bytes = nvrtx_exec_viewport(reuse->viewport, 0U, viewport);
		}
		kern_memcpy(push + execute->at, reuse->viewport, reuse->viewport_bytes);
		execute->at += reuse->viewport_bytes;
		kern_memcpy(held->viewport, viewport, sizeof(held->viewport));
	}

	/* The scissor, when it changed. */
	same = known;
	if (same && (held->scissor[0] != x0 || held->scissor[1] != y0 || held->scissor[2] != x1 || held->scissor[3] != y1))
		same = false;
	if (!same) {
		execute->at = nvrtx_exec_scissor(push, execute->at, (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1);
		held->scissor[0] = x0;
		held->scissor[1] = y0;
		held->scissor[2] = x1;
		held->scissor[3] = y1;
	}

	/*
	 * The pipeline's state, each part when it differs from the last
	 * draw's pipeline's: the culling, the blend, the programs (all of the
	 * pipeline's methods when not known).  The depth and stencil tests,
	 * the depth bias and the blend constants, which may come from the
	 * command buffer, when they differ from what the GPU holds.
	 */
	changes = EXECUTE_PIPELINE_ALL;
	if (known && held->pipeline == pipeline)
		changes = 0U;
	else if (known)
		changes = execute_pipeline_changes(held->pipeline, pipeline);
	if ((changes & EXECUTE_PIPELINE_RASTER) != 0U) {
		execute->at = nvrtx_exec_raster(push, execute->at, pipeline->cull_mode, pipeline->front_face, pipeline->point_size,
						 pipeline->raster_discard, pipeline->polygon_mode, pipeline->depth_clamp);
		execute->at = nvrtx_exec_alpha_coverage(push, execute->at, pipeline->alpha_coverage);
		execute->at = nvrtx_exec_sample_mask(push, execute->at, pipeline->sample_mask);
	}
	execute_draw_state(execute, state, known);
	if ((changes & EXECUTE_PIPELINE_BLEND) != 0U) {
		for (index = 0U; index == 0U || (index < pipeline->colour_count && index < NVRTX_EXEC_COLOURS); index++)
			execute->at = nvrtx_exec_blend(push, execute->at, index, &pipeline->blend[index]);
		execute->at = nvrtx_exec_logic_op(push, execute->at, pipeline->logic_op_enable, pipeline->logic_op);
	}
	if (changes == EXECUTE_PIPELINE_ALL) {
		execute->at = nvrtx_push_draw_pipeline(push, execute->at, pipeline->vertex.va, pipeline->vertex.gprs,
						       pipeline->fragment.va, pipeline->fragment.gprs);
		if (pipeline->geometry.va != 0U)
			execute->at = nvrtx_exec_geometry_program(push, execute->at, pipeline->geometry.va, pipeline->geometry.gprs);
	} else {
		if ((changes & EXECUTE_PIPELINE_VERTEX_PROGRAM) != 0U)
			execute->at = nvrtx_push_draw_program(push, execute->at, false, pipeline->vertex.va, pipeline->vertex.gprs);
		if ((changes & EXECUTE_PIPELINE_PIXEL_PROGRAM) != 0U)
			execute->at = nvrtx_push_draw_program(push, execute->at, true, pipeline->fragment.va, pipeline->fragment.gprs);
		if ((changes & EXECUTE_PIPELINE_GEOMETRY_PROGRAM) != 0U)
			execute->at = nvrtx_exec_geometry_program(push, execute->at, pipeline->geometry.va, pipeline->geometry.gprs);
	}

	/*
	 * Whether the depth and stencil tests run before the pixel program,
	 * after the programs (binding them all sets the tests after it).
	 */
	if (changes == EXECUTE_PIPELINE_ALL || (changes & EXECUTE_PIPELINE_RASTER) != 0U)
		execute->at = nvrtx_exec_early_tests(push, execute->at, pipeline->early_tests);

	/*
	 * The layer's source, after the programs (binding them all sets layer 0
	 * from the state); in a multiview pass the view's layer, every draw.
	 */
	if (state->pass != NULL && state->pass->view_mask != 0U)
		execute->at = nvrtx_exec_layer(push, execute->at, state->view);
	else if ((changes & EXECUTE_PIPELINE_RASTER) != 0U)
		execute->at = nvrtx_exec_layer_source(push, execute->at, pipeline->layer);

	/* The vertex input, when the pipeline's or a bound vertex buffer changed. */
	same = true;
	if ((changes & EXECUTE_PIPELINE_VERTEX_INPUT) != 0U)
		same = false;
	for (index = 0U; index < NVRTX_VK_VERTEX_BINDINGS_MAX && same; index++) {
		if (held->buffers[index] != state->buffers[index] || held->offsets[index] != state->offsets[index])
			same = false;
	}
	if (!same) {
		execute_vertex_input(pipeline, state, &vertex);
		execute->at = nvrtx_exec_vertex(push, execute->at, &vertex);
		kern_memcpy(held->buffers, state->buffers, sizeof(held->buffers));
		kern_memcpy(held->offsets, state->offsets, sizeof(held->offsets));
	}

	/* The index buffer of an indexed draw, when it changed. */
	if (!known)
		held->index_set = false;
	if (indexed) {
		same = held->index_set;
		if (same &&
		    (held->index_buffer != state->index_buffer ||
		     held->index_offset != state->index_offset ||
		     held->index_type != state->index_type))
			same = false;
		if (!same) {
			execute->at = nvrtx_exec_index_buffer(push, execute->at, index_va + state->index_offset,
							      state->index_buffer->size - state->index_offset, state->index_type);
			held->index_set = true;
			held->index_buffer = state->index_buffer;
			held->index_offset = state->index_offset;
			held->index_type = state->index_type;
		}
	}

	/* The uniform buffers, when the pipeline's, a bound set or a dynamic offset changed (their binding selects them, not constant buffer 0). */
	same = true;
	if ((changes & EXECUTE_PIPELINE_UNIFORMS) != 0U)
		same = false;
	for (index = 0U; index < NVRTX_VK_SETS_MAX && same; index++) {
		if (held->sets[index] != state->sets[index])
			same = false;
	}
	if (same) {
		differs = kern_memcmp(held->dynamic, state->dynamic, sizeof(held->dynamic));
		if (differs != 0)
			same = false;
	}
	if (!same) {
		error = execute_uniforms(state, &pipeline->vertex, EXECUTE_GROUP_VERTEX, push, &execute->at);
		if (error == 0)
			error = execute_uniforms(state, &pipeline->fragment, EXECUTE_GROUP_FRAGMENT, push, &execute->at);
		if (error == 0)
			error = execute_uniforms(state, &pipeline->geometry, EXECUTE_GROUP_GEOMETRY, push, &execute->at);
		if (error != 0) {
			held->pools = 0U;
			return error;
		}
		if (pipeline->vertex.uniform_count != 0U || pipeline->fragment.uniform_count != 0U || pipeline->geometry.uniform_count != 0U)
			held->selected = 0U;
		kern_memcpy(held->sets, state->sets, sizeof(held->sets));
		kern_memcpy(held->dynamic, state->dynamic, sizeof(held->dynamic));
	}

	/* The draw, one more in the push buffer. */
	draw.indexed = indexed;
	draw.topology = pipeline->topology;
	draw.first = values[2];
	draw.count = values[0];
	draw.instances = values[1];
	draw.first_instance = values[3];
	draw.vertex_offset = 0;
	if (indexed) {
		draw.first_instance = values[4];
		draw.vertex_offset = (int32_t)values[3];
	}
	execute->at = nvrtx_exec_draw_held(push, execute->at, &held->bases, known, &draw);
	execute->draws++;

	/* What the GPU holds now, for the next draw. */
	held->pipeline = pipeline;
	held->pools = reuse->pools;
	held->at = execute->at;

	/* Succeeded: the draw is written. */
	return 0;
}

/*
 * Runs an indirect dispatch: the work before it has run (it may have
 * written the counts), then the counts, read by the CPU, are dispatched
 * as a direct dispatch from the base 0.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_dispatch.c
 * (nvk_CmdDispatchIndirect: the three counts of the
 * VkDispatchIndirectCommand give the grid; here the CPU reads them once
 * the work before is done, as for the indirect draws).
 */
static int
execute_dispatch_indirect(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_buffer *buffer;
	struct nvrtx_vk_record direct;
	uint32_t counts[3];
	uint8_t *cpu;
	int error;

	/* The work before has run; the buffer's view. */
	buffer = record->objects[0]->payload;
	error = execute_drain(session, execute);
	if (error == 0)
		error = nvrtx_vk_buffer_cpu(session, buffer, &cpu);
	if (error != 0)
		return error;

	/* The counts as a direct dispatch. */
	kern_memcpy(counts, cpu + record->offsets[0], sizeof(counts));
	direct = *record;
	direct.opcode = GPU_OP_CMD_DISPATCH;
	direct.values[0] = counts[0];
	direct.values[1] = counts[1];
	direct.values[2] = counts[2];
	direct.values[3] = 0U;
	direct.values[4] = 0U;
	direct.values[5] = 0U;

	/* Succeeded, or refused: dispatched as a direct dispatch. */
	return execute_dispatch(execute, state, &direct);
}

/*
 * Runs an indirect draw (indexed or not): the work before it has run (it
 * may have written the records), then each record, read by the CPU, is
 * drawn as a direct draw with the same counts.  The records are the
 * buffer's bytes as the draw runs, like the GPU's read of them; the
 * recording checked that they lie within it.  The next push buffer writes
 * the target and the draw's state anew.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_CmdDrawIndirect, nvk_CmdDrawIndexedIndirect: each record's counts
 * give one draw's parameters, the GPU reading them through a macro; here
 * the CPU reads them once the work before is done, as a copy by the CPU
 * waits).
 */
static int
execute_draw_indirect(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_buffer *buffer;
	uint32_t values[5];
	uint8_t *cpu;
	uint64_t at;
	uint32_t bytes;
	uint32_t index;
	bool indexed;
	int error;

	/* A draw in a pass with a pipeline, of one record at least. */
	if (!state->in_pass || state->pipeline == NULL)
		return EINVAL;
	if (record->values[0] == 0U)
		return 0;
	indexed = (record->opcode == GPU_OP_CMD_DRAW_INDEXED_INDIRECT);
	bytes = (uint32_t)sizeof(VkDrawIndirectCommand);
	if (indexed)
		bytes = (uint32_t)sizeof(VkDrawIndexedIndirectCommand);
	buffer = record->objects[0]->payload;

	/* The work before has run; the buffer's view; the target written again after the wait. */
	error = execute_drain(session, execute);
	if (error == 0)
		error = nvrtx_vk_buffer_cpu(session, buffer, &cpu);
	if (error != 0)
		return error;
	state->target_written = false;

	/* Each record as a draw, with room for each after the first. */
	for (index = 0U; index < record->values[0]; index++) {
		if (index != 0U) {
			error = execute_room(session, execute, state);
			if (error != 0)
				return error;
		}
		at = record->offsets[0] + (uint64_t)index * record->values[1];
		kern_memcpy(values, cpu + at, bytes);
		error = execute_draw(execute, state, indexed, values);
		if (error != 0)
			return error;
	}

	/* Succeeded: the records are drawn. */
	return 0;
}

/*
 * Writes the draw's depth and stencil tests, its depth bias and its blend
 * constants, each when it differs from what the GPU holds (always when
 * that is not known): the pipeline's, or the command buffer's where the
 * pipeline leaves them dynamic.
 */
static void
execute_draw_state(
	struct nvrtx_vk_execute *execute,
	const struct execute_state *state,
	bool known)
{
	const struct nvrtx_vk_pipeline *pipeline;
	const uint32_t *factors;
	const uint32_t *constants;
	struct execute_held *held;
	struct nvrtx_exec_depth depth;
	struct nvrtx_exec_depth_bias bias;
	uint8_t *push;
	int differs;

	/* The draw's pipeline, what the GPU holds and the push buffer. */
	pipeline = state->pipeline;
	held = &execute->held;
	push = execute->cpu + EXECUTE_PUSH_AT;

	/* The depth and stencil tests. */
	execute_draw_depth(pipeline, state, &depth);
	differs = kern_memcmp(&depth, &held->depth, sizeof(depth));
	if (!known || differs != 0) {
		execute->at = nvrtx_exec_depth(push, execute->at, &depth);
		held->depth = depth;
	}

	/* The depth bias: its factors only while it is on. */
	kern_memset(&bias, 0, sizeof(bias));
	if (pipeline->depth_bias_enable != 0U) {
		factors = pipeline->depth_bias;
		if ((pipeline->dynamic & NVRTX_VK_DYNAMIC_DEPTH_BIAS) != 0U)
			factors = state->depth_bias;
		bias.enable = 1U;
		bias.constant = factors[0];
		bias.clamp = factors[1];
		bias.slope = factors[2];
	}
	differs = kern_memcmp(&bias, &held->bias, sizeof(bias));
	if (!known || differs != 0) {
		execute->at = nvrtx_exec_depth_bias(push, execute->at, &bias);
		held->bias = bias;
	}

	/* The blend constants. */
	constants = pipeline->blend_constants;
	if ((pipeline->dynamic & NVRTX_VK_DYNAMIC_BLEND_CONSTANTS) != 0U)
		constants = state->blend_constants;
	differs = kern_memcmp(constants, held->blend_constants, sizeof(held->blend_constants));
	if (!known || differs != 0) {
		execute->at = nvrtx_exec_blend_constants(push, execute->at, constants);
		kern_memcpy(held->blend_constants, constants, sizeof(held->blend_constants));
	}
}

/*
 * Gives a draw's depth and stencil tests: the depth test when the pass
 * has a depth target, the stencil test when it has a stencil and the
 * pipeline tests it, each face's masks and reference from the command
 * buffer where the pipeline leaves them dynamic.
 */
static void
execute_draw_depth(
	const struct nvrtx_vk_pipeline *pipeline,
	const struct execute_state *state,
	struct nvrtx_exec_depth *depth)
{
	/* The depth test. */
	kern_memset(depth, 0, sizeof(*depth));
	if (state->target.depth_address != 0U) {
		depth->test = pipeline->depth_test;
		depth->write = pipeline->depth_write;
		depth->compare = pipeline->depth_compare;
	}

	/* Nothing more without a stencil to test. */
	if (state->target.depth_stencil == 0U || pipeline->stencil_test == 0U)
		return;

	/* Both faces, their dynamic masks and references from the command buffer. */
	depth->stencil = 1U;
	depth->front = pipeline->stencil_front;
	depth->back = pipeline->stencil_back;
	if ((pipeline->dynamic & NVRTX_VK_DYNAMIC_STENCIL_COMPARE) != 0U) {
		depth->front.compare_mask = state->stencil_compare[0];
		depth->back.compare_mask = state->stencil_compare[1];
	}
	if ((pipeline->dynamic & NVRTX_VK_DYNAMIC_STENCIL_WRITE) != 0U) {
		depth->front.write_mask = state->stencil_write[0];
		depth->back.write_mask = state->stencil_write[1];
	}
	if ((pipeline->dynamic & NVRTX_VK_DYNAMIC_STENCIL_REFERENCE) != 0U) {
		depth->front.reference = state->stencil_reference[0];
		depth->back.reference = state->stencil_reference[1];
	}
}

/*
 * Reports the parts of a pipeline's state (EXECUTE_PIPELINE_*) that
 * differ from the held pipeline's: what the draw writes anew.
 */
static uint32_t
execute_pipeline_changes(
	const struct nvrtx_vk_pipeline *held,
	const struct nvrtx_vk_pipeline *pipeline)
{
	uint32_t changes;
	bool same;

	/* The culling, the front face, the point size's and the layer's sources, the samples kept, the polygon mode and the depth's clamp. */
	changes = 0U;
	if (held->cull_mode != pipeline->cull_mode || held->front_face != pipeline->front_face ||
	    held->point_size != pipeline->point_size || held->layer != pipeline->layer || held->alpha_coverage != pipeline->alpha_coverage ||
	    held->sample_mask != pipeline->sample_mask ||
	    held->raster_discard != pipeline->raster_discard || held->polygon_mode != pipeline->polygon_mode ||
	    held->depth_clamp != pipeline->depth_clamp || held->early_tests != pipeline->early_tests)
		changes |= EXECUTE_PIPELINE_RASTER;

	/* The blend of each colour target and the logic operation. */
	same = (held->colour_count == pipeline->colour_count);
	if (same && (held->logic_op_enable != pipeline->logic_op_enable || held->logic_op != pipeline->logic_op))
		same = false;
	if (same)
		same = execute_same_words((const uint32_t *)held->blend, (const uint32_t *)pipeline->blend, sizeof(held->blend) / 4U);
	if (!same)
		changes |= EXECUTE_PIPELINE_BLEND;

	/* The programs. */
	if (held->vertex.va != pipeline->vertex.va || held->vertex.gprs != pipeline->vertex.gprs)
		changes |= EXECUTE_PIPELINE_VERTEX_PROGRAM;
	if (held->fragment.va != pipeline->fragment.va || held->fragment.gprs != pipeline->fragment.gprs)
		changes |= EXECUTE_PIPELINE_PIXEL_PROGRAM;
	if (held->geometry.va != pipeline->geometry.va || held->geometry.gprs != pipeline->geometry.gprs)
		changes |= EXECUTE_PIPELINE_GEOMETRY_PROGRAM;

	/* The vertex input: the bindings and the attributes. */
	same = true;
	if (held->binding_count != pipeline->binding_count || held->attribute_count != pipeline->attribute_count)
		same = false;
	if (same)
		same = execute_same_words((const uint32_t *)held->bindings, (const uint32_t *)pipeline->bindings,
					  held->binding_count * (sizeof(held->bindings[0]) / 4U));
	if (same)
		same = execute_same_words((const uint32_t *)held->attributes, (const uint32_t *)pipeline->attributes,
					  held->attribute_count * (sizeof(held->attributes[0]) / 4U));
	if (!same)
		changes |= EXECUTE_PIPELINE_VERTEX_INPUT;

	/* The uniform buffers the programs read. */
	same = true;
	if (held->vertex.uniform_count != pipeline->vertex.uniform_count ||
	    held->fragment.uniform_count != pipeline->fragment.uniform_count ||
	    held->geometry.uniform_count != pipeline->geometry.uniform_count)
		same = false;
	if (same)
		same = execute_same_words((const uint32_t *)held->vertex.uniforms, (const uint32_t *)pipeline->vertex.uniforms,
					  held->vertex.uniform_count * (sizeof(held->vertex.uniforms[0]) / 4U));
	if (same)
		same = execute_same_words((const uint32_t *)held->fragment.uniforms, (const uint32_t *)pipeline->fragment.uniforms,
					  held->fragment.uniform_count * (sizeof(held->fragment.uniforms[0]) / 4U));
	if (same)
		same = execute_same_words((const uint32_t *)held->geometry.uniforms, (const uint32_t *)pipeline->geometry.uniforms,
					  held->geometry.uniform_count * (sizeof(held->geometry.uniforms[0]) / 4U));
	if (!same)
		changes |= EXECUTE_PIPELINE_UNIFORMS;

	/* Succeeded: the parts that differ. */
	return changes;
}

/* Reports whether two runs of count words are equal. */
static bool
execute_same_words(
	const uint32_t *left,
	const uint32_t *right,
	size_t count)
{
	size_t index;

	/* Each word in turn. */
	for (index = 0U; index < count; index++) {
		if (left[index] != right[index])
			return false;
	}

	/* Succeeded: every word is equal. */
	return true;
}

/*
 * Gives the draw's constant buffers 0, one for each stage (EXECUTE_ROOT_AT
 * and the two slots after it), their push constants, texture handles and
 * storage tables, and the vertex program's the draw's first instance, by
 * loads in the push buffer.  Unless the GPU's are known, the vertex one is
 * selected and bound to every program group, its words loaded, then the
 * fragment one selected and bound to the fragment group and its words
 * loaded, then the geometry one to the geometry group; else the words that
 * differ are loaded (each buffer selected again when another was).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_cmd_flush_gfx_cbufs: each stage's group binds its own constant
 * buffers).
 */
static int
execute_draw_constants(
	struct nvrtx_vk_execute *execute,
	const struct execute_state *state,
	const struct execute_stage_words *words,
	uint32_t first_instance,
	bool known)
{
	struct execute_held *held;
	uint32_t stage;
	uint32_t push_words;
	uint32_t instance_valid;
	uint8_t *push;

	/*
	 * Unknown: the vertex buffer selected and bound to every group, the
	 * fragment and geometry ones to their groups, then all their words
	 * loaded (the GPU's loads, which the draws before do not see; the CPU
	 * never writes them).
	 */
	held = &execute->held;
	push = execute->cpu + EXECUTE_PUSH_AT;
	push_words = (state->push_end + 3U) / 4U;
	if (!known) {
		held->constants_va[EXECUTE_STAGE_VERTEX] = execute->slots[0].va + EXECUTE_ROOT_AT;
		held->constants_va[EXECUTE_STAGE_FRAGMENT] = held->constants_va[EXECUTE_STAGE_VERTEX] + EXECUTE_CONSTANTS_SLOT;
		held->constants_va[EXECUTE_STAGE_GEOMETRY] = held->constants_va[EXECUTE_STAGE_FRAGMENT] + EXECUTE_CONSTANTS_SLOT;
		for (stage = 0U; stage < EXECUTE_STAGES; stage++) {
			if (stage == EXECUTE_STAGE_VERTEX) {
				execute->at = nvrtx_push_draw_constants(push, execute->at, held->constants_va[stage], EXECUTE_CONSTANTS_SIZE);
			} else if (stage == EXECUTE_STAGE_FRAGMENT) {
				execute->at = nvrtx_exec_uniform(push, execute->at, EXECUTE_GROUP_FRAGMENT, NVRTX_SM75_CB_ROOT, held->constants_va[stage], EXECUTE_CONSTANTS_SIZE);
			} else {
				execute->at = nvrtx_exec_uniform(push, execute->at, EXECUTE_GROUP_GEOMETRY, NVRTX_SM75_CB_ROOT, held->constants_va[stage], EXECUTE_CONSTANTS_SIZE);
			}
			execute->at = nvrtx_exec_constants_load(push, execute->at, NVRTX_SM75_CB_PUSH, state->push, NVRTX_VK_PUSH_BYTES / 4U);
			execute->at = nvrtx_exec_constants_load(push, execute->at, NVRTX_SM75_CB_TEXTURES, words[stage].handles, NVRTX_VK_TEXTURES_MAX);
			if (words[stage].storage_count != 0U)
				execute->at = nvrtx_exec_constants_load(push, execute->at, NVRTX_SM75_CB_STORAGE, words[stage].storage, words[stage].storage_count);
			kern_memcpy(held->push[stage], state->push, sizeof(held->push[stage]));
			kern_memcpy(held->handles[stage], words[stage].handles, sizeof(held->handles[stage]));
			kern_memcpy(held->storage[stage], words[stage].storage, (size_t)words[stage].storage_count * 4U);
			held->storage_valid[stage] = words[stage].storage_count;
			if (stage == EXECUTE_STAGE_VERTEX)
				execute->at = nvrtx_exec_constants_load(push, execute->at, NVRTX_SM75_CB_BASE_INSTANCE, &first_instance, 1U);
		}
		held->selected = EXECUTE_STAGES;
		held->first_instance = first_instance;
		return 0;
	}

	/*
	 * Known: each stage's push constants (none past push_end, zero in
	 * all), handles and storage words that differ, and the vertex
	 * program's first instance, loaded; the last stage's first, its
	 * buffer the one a draw before left selected.
	 */
	for (stage = EXECUTE_STAGES; stage > 0U; stage--) {
		execute_draw_load(execute, stage - 1U, NVRTX_SM75_CB_PUSH, state->push, held->push[stage - 1U], push_words, NULL);
		execute_draw_load(execute, stage - 1U, NVRTX_SM75_CB_TEXTURES, words[stage - 1U].handles, held->handles[stage - 1U], NVRTX_VK_TEXTURES_MAX, NULL);
		execute_draw_load(execute, stage - 1U, NVRTX_SM75_CB_STORAGE, words[stage - 1U].storage, held->storage[stage - 1U], words[stage - 1U].storage_count,
				  &held->storage_valid[stage - 1U]);
	}
	instance_valid = 1U;
	execute_draw_load(execute, EXECUTE_STAGE_VERTEX, NVRTX_SM75_CB_BASE_INSTANCE, &first_instance, &held->first_instance, 1U, &instance_valid);

	/* Succeeded: each constant buffer 0 holds the draw's constants. */
	return 0;
}

/*
 * Loads the run of words that differ from what a stage's constant buffer 0
 * holds (held, count words from the byte offset; past the first *valid,
 * when valid is given, nothing is known) into it, and keeps them in
 * held_words; nothing when none differs.
 */
static void
execute_draw_load(
	struct nvrtx_vk_execute *execute,
	uint32_t stage,
	uint32_t offset,
	const uint32_t *words,
	uint32_t *held_words,
	uint32_t count,
	uint32_t *valid)
{
	struct execute_held *held;
	uint8_t *push;
	uint32_t known;
	uint32_t first;
	uint32_t last;

	/* The first and the last word that differ (or are not known). */
	known = count;
	if (valid != NULL && *valid < known)
		known = *valid;
	first = 0U;
	while (first < known && words[first] == held_words[first])
		first++;
	if (first == count)
		return;
	last = count - 1U;
	while (last > first && last < known && words[last] == held_words[last])
		last--;

	/* The stage's buffer selected again when another was, then the words from the first to the last. */
	held = &execute->held;
	push = execute->cpu + EXECUTE_PUSH_AT;
	if (held->selected != stage + 1U) {
		execute->at = nvrtx_exec_constants_select(push, execute->at, held->constants_va[stage], EXECUTE_CONSTANTS_SIZE);
		held->selected = stage + 1U;
	}
	execute->at = nvrtx_exec_constants_load(push, execute->at, offset + first * 4U, &words[first], last - first + 1U);
	kern_memcpy(&held_words[first], &words[first], (size_t)(last - first + 1U) * 4U);
	if (valid != NULL && *valid < count)
		*valid = count;
}

/*
 * Gives the words of a graphics program's storage table (its entries'
 * addresses and sizes, four words each) from the graphics bind point's
 * sets.  A descriptor with nothing bound refuses the draw.
 */
static int
execute_stage_storage(
	const struct execute_state *state,
	const struct nvrtx_vk_program *program,
	struct execute_stage_words *words)
{
	uint64_t va;
	uint64_t range;
	uint32_t index;
	int error;

	/* Each entry. */
	for (index = 0U; index < program->storage_count; index++) {
		error = execute_storage(state->sets, (const uint32_t (*)[NVRTX_VK_BINDINGS_MAX])state->dynamic, &program->storage[index], &va, &range);
		if (error != 0)
			return error;
		words->storage[index * 4U] = (uint32_t)va;
		words->storage[index * 4U + 1U] = (uint32_t)(va >> 32);
		words->storage[index * 4U + 2U] = (uint32_t)range;
		words->storage[index * 4U + 3U] = 0U;
	}
	words->storage_count = program->storage_count * 4U;

	/* Succeeded: the table is made. */
	return 0;
}

/*
 * Binds a program's uniform buffers to its group: the k-th one, from its
 * set's binding (a dynamic one at its bind's offset further), as constant
 * buffer 1 + k, its range rounded up to whole 16-byte units and cut at
 * the largest constant buffer.  A binding with nothing bound, or a range
 * off the 256-byte boundary or past the buffer, refuses the draw.
 */
static int
execute_uniforms(
	const struct execute_state *state,
	const struct nvrtx_vk_program *program,
	uint32_t group,
	uint8_t *push,
	size_t *at)
{
	const struct nvrtx_vk_descriptor_set *set;
	const struct nvrtx_vk_descriptor_slot *slot;
	const struct nvrtx_vk_buffer *buffer;
	uint64_t offset;
	uint64_t range;
	uint64_t va;
	uint8_t *cpu;
	uint32_t index;
	int error;

	/* Each uniform buffer of the program. */
	for (index = 0U; index < program->uniform_count; index++) {
		set = state->sets[program->uniforms[index].set];
		if (set == NULL)
			return EINVAL;
		slot = nvrtx_vk_descriptor_find(set, program->uniforms[index].binding, program->uniforms[index].element);
		if (slot == NULL || slot->buffer == NULL)
			return EINVAL;
		buffer = slot->buffer->payload;
		error = nvrtx_vk_buffer_address(buffer, &va, &cpu);
		if (error != 0)
			return error;

		/* Its start: the binding's offset, and a dynamic binding's offset from the bind. */
		offset = slot->offset;
		if (slot->dynamic != 0U)
			offset += state->dynamic[program->uniforms[index].set][program->uniforms[index].binding];
		if (offset >= buffer->size || ((va + offset) % EXECUTE_UNIFORM_ALIGN) != 0U)
			return EINVAL;

		/* Its range: the rest of the buffer for the whole size. */
		range = slot->range;
		if (range == VK_WHOLE_SIZE || range > buffer->size - offset)
			range = buffer->size - offset;
		range = (range + EXECUTE_UNIFORM_UNIT - 1U) & ~(uint64_t)(EXECUTE_UNIFORM_UNIT - 1U);
		if (range > EXECUTE_UNIFORM_BYTES_MAX)
			range = EXECUTE_UNIFORM_BYTES_MAX;
		*at = nvrtx_exec_uniform(push, *at, group, NVRTX_SM75_CB_UNIFORM_FIRST + index, va + offset, (uint32_t)range);
	}

	/* Succeeded: every uniform buffer is bound. */
	return 0;
}

/*
 * Gives the handles of a program's sampled images for its stage's
 * constants (handles, NVRTX_VK_TEXTURES_MAX): each one's header and
 * sampler from the bound set's descriptor (a uniform texel buffer's
 * header and no sampler of its own), put in the pools once a push buffer
 * for a view and a sampler.  A descriptor with nothing bound refuses the
 * draw.
 */
static int
execute_textures(
	struct nvrtx_vk_execute *execute,
	const struct execute_state *state,
	const struct nvrtx_vk_program *program,
	uint32_t *handles)
{
	const struct nvrtx_vk_descriptor_set *set;
	const struct nvrtx_vk_descriptor_slot *slot;
	struct nvrtx_vk_buffer_view *texel;
	struct nvrtx_vk_view *view;
	struct nvrtx_vk_sampler *sampler;
	struct execute_reuse *reuse;
	const void *named;
	uint64_t mixed;
	uint32_t handle;
	uint32_t index;
	uint32_t place;
	bool found;
	int error;

	/* Each sampled image of the program. */
	for (index = 0U; index < program->texture_count; index++) {
		set = state->sets[program->textures[index].set];
		if (set == NULL)
			return EINVAL;
		slot = nvrtx_vk_descriptor_find(set, program->textures[index].binding, program->textures[index].element);
		if (slot == NULL)
			return EINVAL;

		/* A uniform texel buffer's view (no sampler), or an image view and its sampler. */
		texel = NULL;
		view = NULL;
		sampler = NULL;
		if (slot->texel != NULL) {
			texel = slot->texel->payload;
			named = texel;
		} else {
			if (slot->view == NULL || slot->sampler == NULL)
				return EINVAL;
			view = slot->view->payload;
			sampler = slot->sampler->payload;
			named = view;
		}

		/* Its handle: the one an earlier draw of this push buffer made for the same view and sampler (at their place), or a new one. */
		reuse = &execute->reuse;
		mixed = ((uint64_t)(uintptr_t)named ^ ((uint64_t)(uintptr_t)sampler >> 3)) * EXECUTE_TEXTURE_CACHE_MIX;
		place = (uint32_t)(mixed >> EXECUTE_TEXTURE_CACHE_SHIFT) % EXECUTE_TEXTURE_CACHE;
		found = execute_texture_kept(reuse, place, named, sampler);
		if (!found) {
			/* The pair's other place (the two make one set). */
			place ^= 1U;
			found = execute_texture_kept(reuse, place, named, sampler);
		}
		if (found) {
			handle = reuse->texture_handle[place];
		} else {
			/* A new entry, at the set's place this push buffer does not use yet, else at the first. */
			if (reuse->texture_pools[place] == reuse->pools)
				place ^= 1U;
			if (texel != NULL)
				error = execute_texel(execute, texel, &handle);
			else
				error = execute_texture(execute, view, sampler, &handle);
			if (error != 0)
				return error;
			reuse->texture_pools[place] = reuse->pools;
			reuse->texture_view[place] = named;
			reuse->texture_sampler[place] = sampler;
			reuse->texture_handle[place] = handle;
		}
		if (index < NVRTX_VK_TEXTURES_MAX)
			handles[index] = handle;
	}

	/* Succeeded: every handle is in place. */
	return 0;
}

/* Reports whether a place of the push buffer's textures holds the handle of a view and a sampler. */
static bool
execute_texture_kept(
	const struct execute_reuse *reuse,
	uint32_t place,
	const void *view,
	const void *sampler)
{
	/* Made in this push buffer, for the same pair. */
	if (reuse->texture_pools[place] != reuse->pools)
		return false;
	if (reuse->texture_view[place] != view || reuse->texture_sampler[place] != sampler)
		return false;

	/* Succeeded: the place's handle is the pair's. */
	return true;
}

/* Writes a view's header and a sampler into the pools and gives their handle. */
static int
execute_texture(
	struct nvrtx_vk_execute *execute,
	struct nvrtx_vk_view *view,
	struct nvrtx_vk_sampler *sampler,
	uint32_t *handle)
{
	const struct nvrtx_vk_image *image;
	struct nvrtx_exec_sampler words;
	struct nvrtx_exec_texture_shape shape;
	uint64_t va;
	uint64_t offset;
	uint64_t layer_offset;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint8_t *cpu;
	int error;

	/*
	 * The view's image, with storage, in a colour or depth format (a depth
	 * view reads the depth) or a plane of a video image; a multisampled one
	 * a 2D view of its colour (texelFetch of a sample).
	 */
	image = view->image->payload;
	if (image->video && view->plane == 0U)
		return EINVAL;
	if (image->samples > 1U && (image->depth || view->type != VK_IMAGE_VIEW_TYPE_2D))
		return EINVAL;
	error = nvrtx_vk_image_address(image, &va, &cpu);
	if (error != 0)
		return error;

	/*
	 * The header, made once for the view while its image's storage stays
	 * where it is: a video plane's (the luma plane full size, the chroma
	 * plane's pairs half each way), a tiled image's with the view's levels
	 * and from its first layer in the view's shape, a pitch-linear image's
	 * of its one level.
	 */
	if (view->header_va != va || va == 0U) {
		kern_memset(view->header, 0, sizeof(view->header));
		if (image->video) {
			if (view->plane == 1U) {
				nvrtx_exec_texture_header_plane(view->header, va + image->plane_offset[0], image->width, image->height,
								1U, view->swizzle);
			} else {
				nvrtx_exec_texture_header_plane(view->header, va + image->plane_offset[1], image->width / 2U,
								image->height / 2U, 2U, view->swizzle);
			}
		} else if (image->tiled) {
			layer_offset = execute_texture_shape(image, view, &shape);
			nvrtx_exec_texture_header_shaped(view->header, va + layer_offset, &shape, view->format, view->swizzle);
		} else {
			nvrtx_vk_image_level(image, 0U, &offset, &width, &height, &pitch);
			nvrtx_exec_texture_header(view->header, va + offset, width, height, pitch, view->format, view->swizzle);
		}
		view->header_va = va;
	}

	/* The sampler's words, made once. */
	if (!sampler->words_made) {
		kern_memset(&words, 0, sizeof(words));
		words.mag_filter = sampler->mag_filter;
		words.min_filter = sampler->min_filter;
		words.mipmap_mode = sampler->mipmap_mode;
		words.min_lod = sampler->min_lod;
		words.max_lod = sampler->max_lod;
		words.address_u = sampler->address_u;
		words.address_v = sampler->address_v;
		words.address_w = sampler->address_w;
		words.unnormalized = sampler->unnormalized;
		words.compare_enable = sampler->compare_enable;
		words.compare_op = sampler->compare_op;
		nvrtx_exec_sampler_words(sampler->words, &words);
		sampler->words_made = true;
	}

	/* Into the pools. */
	error = execute_pool_entry(execute, view->header, sampler->words, handle);
	if (error != 0)
		return error;

	/* Succeeded: the handle names the header and the sampler. */
	return 0;
}

/*
 * Writes a buffer view's header (a buffer of texels) and a sampler of no
 * filter into the pools and gives their handle: the texels are fetched,
 * never sampled, so the sampler's words are all zero.
 */
static int
execute_texel(
	struct nvrtx_vk_execute *execute,
	struct nvrtx_vk_buffer_view *view,
	uint32_t *handle)
{
	static const uint32_t no_sampler[8] = { 0U };
	const struct nvrtx_vk_buffer *buffer;
	uint64_t va;
	uint32_t texels;
	uint8_t *cpu;
	int error;

	/* The view's buffer, with storage. */
	buffer = view->buffer->payload;
	error = nvrtx_vk_buffer_address(buffer, &va, &cpu);
	if (error != 0)
		return error;

	/* The header, made once for the view while its buffer's storage stays where it is. */
	if (view->header_va != va || va == 0U) {
		texels = (uint32_t)(view->range / nvrtx_vk_format_bytes(view->format));
		nvrtx_exec_texture_header_buffer(view->header, va + view->offset, texels, view->format);
		view->header_va = va;
	}

	/* Into the pools. */
	error = execute_pool_entry(execute, view->header, no_sampler, handle);
	if (error != 0)
		return error;

	/* Succeeded: the handle names the header. */
	return 0;
}

/*
 * Describes a view of a tiled image as a texture header's shape and reports
 * where the view's first layer starts in the image: a 3D view every slice,
 * a cube view its cube, a cube array view its cubes, an array view (2D or
 * 1D) its layers, a 1D or 2D view its layer (a 1D one a 2D surface one row
 * high).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/nil/tic.rs
 * (nvb097_fill_tic: the base layer's address, there being no base layer
 * field; normalize_extent).
 */
static uint64_t
execute_texture_shape(
	const struct nvrtx_vk_image *image,
	const struct nvrtx_vk_view *view,
	struct nvrtx_exec_texture_shape *shape)
{
	/* The image's level 0 and levels, the view's levels. */
	shape->width = image->width;
	shape->height = image->height;
	shape->depth = 1U;
	shape->type = NVRTX_EXEC_TEXTURE_2D;
	shape->levels = image->levels;
	shape->base_level = view->base_level;
	shape->level_count = view->level_count;
	shape->samples = image->samples;

	/* The type and depth of each kind of view. */
	switch (view->type) {
	case VK_IMAGE_VIEW_TYPE_3D:
		shape->type = NVRTX_EXEC_TEXTURE_3D;
		shape->depth = image->slices;
		return 0U;
	case VK_IMAGE_VIEW_TYPE_CUBE:
		shape->type = NVRTX_EXEC_TEXTURE_CUBE;
		break;
	case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
		shape->type = NVRTX_EXEC_TEXTURE_CUBE_ARRAY;
		shape->depth = view->layer_count / 6U;
		break;
	case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
	case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
		shape->type = NVRTX_EXEC_TEXTURE_2D_ARRAY;
		shape->depth = view->layer_count;
		break;
	default:
		break;
	}

	/* Succeeded: the view's first layer, whole layers into the image. */
	return image->layer_bytes * view->base_layer;
}

/*
 * Makes room for one more surface of a transfer: when the push buffer,
 * the constants or the pools could not take its quad, the run so far is
 * put on the GPU and the transfer goes on in the next push buffer.
 */
static int
execute_surface_room(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state)
{
	bool full;
	int error;

	/* Full when one quad would not fit. */
	full = false;
	if (execute->at + EXECUTE_SURFACE_ROOM > EXECUTE_PUSH_BYTES)
		full = true;
	if (execute->constants + EXECUTE_CONSTANTS_SLOT > EXECUTE_CONSTANTS_BYTES)
		full = true;
	if (execute->headers + 1U > EXECUTE_POOL_ENTRIES || execute->samplers + 1U > EXECUTE_POOL_ENTRIES)
		full = true;
	if (!full)
		return 0;

	/* Runs what there is. */
	error = execute_flush(session, execute);
	if (error != 0)
		return error;

	/* Succeeded: the next push buffer has room; its target is written anew. */
	state->target_written = false;
	return 0;
}

/* Puts a header and a sampler into the next entries of the pools and gives their handle. */
static int
execute_pool_entry(
	struct nvrtx_vk_execute *execute,
	const uint32_t *header,
	const uint32_t *sampler,
	uint32_t *handle)
{
	uint32_t index;

	/* Room in the pools (execute_room keeps some for each record). */
	if (execute->headers >= EXECUTE_POOL_ENTRIES || execute->samplers >= EXECUTE_POOL_ENTRIES)
		return ENOSPC;

	/* The two entries' words. */
	for (index = 0U; index < 8U; index++) {
		nvrtx_put32(execute->cpu, EXECUTE_HEADERS_AT + execute->headers * EXECUTE_POOL_ENTRY_BYTES + index * 4U, header[index]);
		nvrtx_put32(execute->cpu, EXECUTE_SAMPLERS_AT + execute->samplers * EXECUTE_POOL_ENTRY_BYTES + index * 4U, sampler[index]);
	}

	/* The handle: the header's index, the sampler's above it. */
	*handle = execute->headers | (execute->samplers << EXECUTE_HANDLE_SAMPLER_SHIFT);
	execute->headers++;
	execute->samplers++;

	/* Succeeded: the entries are the draw's. */
	return 0;
}

/* Gives a draw's constant buffer: the next slot of the run's constants, cleared. */
static int
execute_constants(
	struct nvrtx_vk_execute *execute,
	uint8_t **cpu,
	uint64_t *va)
{
	/* Room for one more (execute_room keeps some for each record). */
	if (execute->constants + EXECUTE_CONSTANTS_SLOT > EXECUTE_CONSTANTS_BYTES)
		return ENOSPC;

	/* The slot. */
	*cpu = execute->cpu + EXECUTE_CONSTANTS_AT + execute->constants;
	*va = execute->va + EXECUTE_CONSTANTS_AT + execute->constants;
	kern_memset(*cpu, 0, EXECUTE_CONSTANTS_SIZE);
	execute->constants += EXECUTE_CONSTANTS_SLOT;

	/* Succeeded: the slot is the draw's. */
	return 0;
}

/*
 * Fills a draw's vertex input: the pipeline's attributes, each from the
 * stream of its binding, and each binding's stream at its bound buffer
 * (a binding without storage stays off: its attributes read the zero
 * page).
 */
static void
execute_vertex_input(
	const struct nvrtx_vk_pipeline *pipeline,
	const struct execute_state *state,
	struct nvrtx_exec_vertex *vertex)
{
	const struct nvrtx_vk_vertex_binding *binding;
	const struct nvrtx_vk_buffer *buffer;
	struct nvrtx_exec_stream *stream;
	uint64_t va;
	uint8_t *cpu;
	uint32_t index;
	int error;

	/* The attributes: the stream is the binding's number. */
	kern_memset(vertex, 0, sizeof(*vertex));
	vertex->attribute_count = pipeline->attribute_count;
	for (index = 0U; index < pipeline->attribute_count; index++) {
		vertex->attributes[index].location = pipeline->attributes[index].location;
		vertex->attributes[index].stream = pipeline->attributes[index].binding;
		vertex->attributes[index].offset = pipeline->attributes[index].offset;
		vertex->attributes[index].format = pipeline->attributes[index].format;
	}

	/* The streams of the pipeline's bindings. */
	for (index = 0U; index < pipeline->binding_count; index++) {
		binding = &pipeline->bindings[index];
		buffer = state->buffers[binding->binding];
		if (buffer == NULL)
			continue;
		error = nvrtx_vk_buffer_address(buffer, &va, &cpu);
		if (error != 0 || state->offsets[binding->binding] >= buffer->size)
			continue;

		stream = &vertex->streams[binding->binding];
		stream->enabled = 1U;
		stream->address = va + state->offsets[binding->binding];
		stream->bytes = buffer->size - state->offsets[binding->binding];
		stream->stride = binding->stride;
		stream->instance = binding->instance;
	}
}

/*
 * Runs a transfer outside a pass: the barrier before it (the work before
 * may have written its source), the transfer, the barrier after it (the
 * target written anew in the next pass).
 */
static int
execute_transfer(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	int error;

	/* What wrote the source has finished; the shadow's pixels may no longer be the target's. */
	execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);
	execute->kept.valid = false;

	/* The transfer. */
	if (record->opcode == GPU_OP_CMD_COPY_BUFFER) {
		error = execute_copy_buffer(session, execute, record);
	} else if (record->opcode == GPU_OP_CMD_UPDATE_BUFFER || record->opcode == GPU_OP_CMD_FILL_BUFFER) {
		error = execute_write_buffer(session, execute, record);
	} else if (record->opcode == GPU_OP_CMD_COPY_QUERY_POOL_RESULTS) {
		error = execute_drain(session, execute);
		if (error == 0)
			error = nvrtx_vk_video_copy_queries(session, record);
	} else if (record->opcode == GPU_OP_CMD_COPY_IMAGE || record->opcode == GPU_OP_CMD_BLIT_IMAGE) {
		error = execute_copy_images(session, execute, state, record);
	} else if (record->opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
		error = execute_clear_image(session, execute, state, record);
	} else if (record->opcode == GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE) {
		error = execute_clear_depth_image(session, execute, state, record);
	} else if (record->opcode == GPU_OP_CMD_RESOLVE_IMAGE) {
		error = execute_resolve_images(session, execute, state, record);
	} else {
		error = execute_buffer_image(session, execute, state, record);
	}
	if (error != 0)
		return error;

	/* What follows reads what it wrote; the pass's target is written again. */
	execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);
	state->target_written = false;

	/* Succeeded: the transfer is written. */
	return 0;
}

/*
 * Runs a clear of the pass's attachments: each rectangle, within the
 * render area and the target, cleared in the colour targets, the depth
 * target and its stencil the record names and the pass has, in each of
 * its layers the pass has.  The clear changes nothing a draw holds, so a
 * draw right after it still writes only what differs (unless the push
 * buffer had to be run for room).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_clear.c
 * (nvk_CmdClearAttachments: the clear rectangle, not the scissor, each
 * layer of it).
 */
static int
execute_clear_attachments(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const VkClearRect *rects;
	struct execute_held *held;
	uint8_t *push;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	uint32_t index;
	uint32_t colours;
	uint32_t count;
	uint32_t target;
	uint32_t format;
	uint32_t layer;
	uint32_t layers;
	uint32_t layer_first;
	uint32_t layer_count;
	uint32_t colour[4];
	bool depth;
	bool stencil;
	bool known;
	int error;

	/* Inside a pass; what of it the record clears and the pass has. */
	if (!state->in_pass)
		return EINVAL;
	colours = 0U;
	if ((record->first & VK_IMAGE_ASPECT_COLOR_BIT) != 0U && state->target.address != 0U) {
		count = state->target.colour_count;
		if (count == 0U)
			count = 1U;
		for (index = 0U; index < count; index++) {
			if ((record->count & (1U << index)) != 0U && (index == 0U || state->target.others[index - 1U].address != 0U))
				colours |= 1U << index;
		}
	}
	depth = false;
	if ((record->first & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U && state->target.depth_address != 0U)
		depth = true;
	stencil = false;
	if ((record->first & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U && state->target.depth_stencil != 0U)
		stencil = true;
	if (colours == 0U && !depth && !stencil)
		return 0;

	/* The target, once in a push buffer; what the GPU holds for the draws stays known only when it was right before. */
	held = &execute->held;
	push = execute->cpu + EXECUTE_PUSH_AT;
	known = false;
	if (held->pools == execute->reuse.pools && held->at == execute->at)
		known = true;
	if (!state->target_written) {
		execute->at = nvrtx_exec_target(push, execute->at, &state->target);
		state->target_written = true;
		known = false;
	}

	/* Each rectangle within the render area and the target. */
	rects = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		x0 = rects[index].rect.offset.x;
		y0 = rects[index].rect.offset.y;
		x1 = x0 + (int32_t)rects[index].rect.extent.width;
		y1 = y0 + (int32_t)rects[index].rect.extent.height;
		if (x0 < state->area.offset.x)
			x0 = state->area.offset.x;
		if (y0 < state->area.offset.y)
			y0 = state->area.offset.y;
		if (x1 > state->area.offset.x + (int32_t)state->area.extent.width)
			x1 = state->area.offset.x + (int32_t)state->area.extent.width;
		if (y1 > state->area.offset.y + (int32_t)state->area.extent.height)
			y1 = state->area.offset.y + (int32_t)state->area.extent.height;
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
		if (x1 > (int32_t)state->target.width)
			x1 = (int32_t)state->target.width;
		if (y1 > (int32_t)state->target.height)
			y1 = (int32_t)state->target.height;
		if (x1 <= x0 || y1 <= y0)
			continue;

		/* The colours, the depth and the stencil it clears, in each of its layers the pass has (with room for them). */
		layers = state->target.layers;
		if (layers == 0U)
			layers = 1U;
		layer_first = rects[index].baseArrayLayer;
		layer_count = rects[index].layerCount;
		if (state->pass != NULL && state->pass->view_mask != 0U) {
			/* In a multiview pass a rectangle clears the layer of each view the subpass renders. */
			layer_first = 0U;
			layer_count = layers;
		}
		for (layer = layer_first; layer < layers && layer - layer_first < layer_count; layer++) {
			if (!execute_layer_drawn(state, layer))
				continue;
			error = execute_surface_room(session, execute, state);
			if (error != 0)
				return error;
			push = execute->cpu + EXECUTE_PUSH_AT;
			if (!state->target_written) {
				execute->at = nvrtx_exec_target(push, execute->at, &state->target);
				state->target_written = true;
				known = false;
			}
			for (target = 0U; target < NVRTX_EXEC_COLOURS; target++) {
				if ((colours & (1U << target)) == 0U)
					continue;
				kern_memcpy(colour, (const uint8_t *)(rects + record->region_count) + target * sizeof(colour), sizeof(colour));

				/* The target's format, by which its clear colour is given. */
				format = state->target.format;
				if (target != 0U)
					format = state->target.others[target - 1U].format;
				execute->at = nvrtx_exec_clear(push, execute->at, target, format, layer, colour, (uint32_t)x0, (uint32_t)y0, (uint32_t)x1,
							       (uint32_t)y1);
			}
			if (depth)
				execute->at = nvrtx_exec_clear_depth(push, execute->at, layer, record->values[4], (uint32_t)x0, (uint32_t)y0,
								     (uint32_t)x1, (uint32_t)y1);
			if (stencil)
				execute->at = nvrtx_exec_clear_stencil(push, execute->at, layer, record->values[5], (uint32_t)x0, (uint32_t)y0,
								       (uint32_t)x1, (uint32_t)y1);
		}
	}

	/* What the GPU held for the draws it still holds. */
	if (known)
		held->at = execute->at;

	/* Succeeded: the rectangles are cleared. */
	return 0;
}

/*
 * Runs a video decode in its place among the records: the push buffer so
 * far is run and waited for (the graphics work before has ended), the
 * decoder runs the picture and is waited for, and the next push buffer
 * is begun, so what follows reads the decoded picture.
 */
static int
execute_decode(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	int error;

	/* The graphics work before the decode, which may write a target the shadow holds. */
	execute->kept.valid = false;
	error = execute_drain(session, execute);
	if (error != 0)
		return error;

	/* The decode, waited for; the next push buffer's target is written anew. */
	error = nvrtx_vk_video_run(session, record);
	state->target_written = false;
	if (error != 0)
		return error;

	/* Succeeded: the picture is decoded. */
	return 0;
}

/*
 * Copies regions of a video image's planes out to a buffer with the copy
 * engine, which reads the block-linear planes: each region's aspect names
 * its plane (the luma plane of one byte a pixel, the chroma plane of two
 * at half the size), the buffer's rows are its row length's pixels of the
 * plane's bytes.
 */
static int
execute_video_copy(
	struct nvrtx_vk_execute *execute,
	const struct nvrtx_vk_image *image,
	uint64_t image_va,
	const struct nvrtx_vk_buffer *buffer,
	uint64_t buffer_va,
	const struct nvrtx_vk_record *record)
{
	const VkBufferImageCopy *regions;
	const VkBufferImageCopy *region;
	struct nvrtx_exec_plane_copy copy;
	uint32_t index;
	uint32_t plane;
	uint32_t pixel;
	uint32_t plane_width;
	uint32_t plane_height;
	uint32_t row;
	uint64_t span;
	bool inside;

	/* Each region. */
	regions = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		region = &regions[index];

		/* The plane its aspect names, its pixel's bytes and its size. */
		if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_PLANE_0_BIT) {
			plane = 0U;
			pixel = 1U;
			plane_width = image->width;
			plane_height = image->height;
		} else if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_PLANE_1_BIT) {
			plane = 1U;
			pixel = 2U;
			plane_width = image->width / 2U;
			plane_height = image->height / 2U;
		} else {
			/* A region of any other aspect is refused. */
			return EINVAL;
		}

		/* Inside the plane, not empty. */
		inside = execute_region_inside(region->imageOffset.x, region->imageOffset.y, region->imageExtent.width,
					       region->imageExtent.height, plane_width, plane_height);
		if (!inside ||
		    region->imageExtent.width == 0U ||
		    region->imageExtent.height == 0U)
			return EINVAL;

		/* The buffer's rows: tightly packed unless the region names a row length. */
		row = region->bufferRowLength;
		if (row == 0U)
			row = region->imageExtent.width;

		/* Inside the buffer: the bytes from its first to its last row's end. */
		span = (uint64_t)row * pixel * (region->imageExtent.height - 1U) + (uint64_t)region->imageExtent.width * pixel;
		if (region->bufferOffset > buffer->size || span > buffer->size - region->bufferOffset)
			return EINVAL;

		/* The copy of the region. */
		copy.source = image_va + image->plane_offset[plane];
		copy.source_pitch = image->pitch;
		copy.source_rows = image->plane_rows[plane];
		copy.x_bytes = (uint32_t)region->imageOffset.x * pixel;
		copy.y = (uint32_t)region->imageOffset.y;
		copy.destination = buffer_va + region->bufferOffset;
		copy.destination_pitch = row * pixel;
		copy.line_bytes = region->imageExtent.width * pixel;
		copy.lines = region->imageExtent.height;
		execute->at = nvrtx_exec_copy_plane(execute->cpu + EXECUTE_PUSH_AT, execute->at, &copy);
	}

	/* Succeeded: the regions are written. */
	return 0;
}

/*
 * Copies or blits regions between two images by drawing quads: the
 * destination region's quad samples the source region (a copy one to one
 * and nearest, a blit scaled and filtered), a quad for each layer (a 3D
 * image's slice) of the region.  A blit's region given backwards mirrors
 * it; a blit between 3D images of other depths takes each destination
 * slice from the nearest source slice.  Depth images are copied by the
 * copy engine (execute_depth_images()).
 */
static int
execute_copy_images(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_image *source;
	const struct nvrtx_vk_image *destination;
	const VkImageCopy *copies;
	const VkImageBlit *blits;
	const VkImageSubresourceLayers *source_layers;
	const VkImageSubresourceLayers *destination_layers;
	struct execute_quad quad;
	struct execute_quad raw;
	struct execute_surfaces source_surfaces;
	struct execute_surfaces destination_surfaces;
	uint32_t index;
	uint32_t surface;
	uint32_t source_surface;
	int32_t dx0;
	int32_t dy0;
	int32_t dx1;
	int32_t dy1;
	int32_t sx0;
	int32_t sy0;
	int32_t sx1;
	int32_t sy1;
	int32_t sz0;
	int32_t sz1;
	int32_t dz0;
	int32_t dz1;
	int error;

	source = record->objects[0]->payload;
	destination = record->objects[1]->payload;

	/* Depth images are copied by the copy engine. */
	if (source->depth || destination->depth)
		return execute_depth_images(session, execute, state, record);

	/* A copy between texels of one size (of any formats, copied bit for bit), a blit between colour formats. */
	if (record->opcode == GPU_OP_CMD_COPY_IMAGE && nvrtx_vk_format_bytes(source->format) != nvrtx_vk_format_bytes(destination->format))
		return EINVAL;

	/* A copy one to one and nearest, a blit filtered. */
	kern_memset(&quad, 0, sizeof(quad));
	quad.filter = VK_FILTER_NEAREST;
	if (record->opcode == GPU_OP_CMD_BLIT_IMAGE)
		quad.filter = record->values[0];

	/* Each region. */
	copies = record->regions;
	blits = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		/* The destination's and the source's corners, layers and slices. */
		if (record->opcode == GPU_OP_CMD_COPY_IMAGE) {
			source_layers = &copies[index].srcSubresource;
			destination_layers = &copies[index].dstSubresource;
			dx0 = copies[index].dstOffset.x;
			dy0 = copies[index].dstOffset.y;
			dz0 = copies[index].dstOffset.z;
			dx1 = dx0 + (int32_t)copies[index].extent.width;
			dy1 = dy0 + (int32_t)copies[index].extent.height;
			dz1 = dz0 + (int32_t)copies[index].extent.depth;
			sx0 = copies[index].srcOffset.x;
			sy0 = copies[index].srcOffset.y;
			sz0 = copies[index].srcOffset.z;
			sx1 = sx0 + (int32_t)copies[index].extent.width;
			sy1 = sy0 + (int32_t)copies[index].extent.height;
			sz1 = sz0 + (int32_t)copies[index].extent.depth;
		} else {
			source_layers = &blits[index].srcSubresource;
			destination_layers = &blits[index].dstSubresource;
			dx0 = blits[index].dstOffsets[0].x;
			dy0 = blits[index].dstOffsets[0].y;
			dz0 = blits[index].dstOffsets[0].z;
			dx1 = blits[index].dstOffsets[1].x;
			dy1 = blits[index].dstOffsets[1].y;
			dz1 = blits[index].dstOffsets[1].z;
			sx0 = blits[index].srcOffsets[0].x;
			sy0 = blits[index].srcOffsets[0].y;
			sz0 = blits[index].srcOffsets[0].z;
			sx1 = blits[index].srcOffsets[1].x;
			sy1 = blits[index].srcOffsets[1].y;
			sz1 = blits[index].srcOffsets[1].z;
		}

		/* The surfaces each side's layers or slices name; a copy takes as many as it gives. */
		error = execute_region_surfaces(source, source_layers, sz0, sz1, &source_surfaces);
		if (error == 0)
			error = execute_region_surfaces(destination, destination_layers, dz0, dz1, &destination_surfaces);
		if (error != 0)
			return error;
		if (record->opcode == GPU_OP_CMD_COPY_IMAGE && source_surfaces.count != destination_surfaces.count)
			return EINVAL;
		if (destination->type != VK_IMAGE_TYPE_3D && source_surfaces.count != destination_surfaces.count)
			return EINVAL;

		/* The quad covers the destination's region in order; a reversed edge swaps the source's. */
		quad.x0 = dx0;
		quad.x1 = dx1;
		quad.u0 = sx0;
		quad.u1 = sx1;
		if (dx1 < dx0) {
			quad.x0 = dx1;
			quad.x1 = dx0;
			quad.u0 = sx1;
			quad.u1 = sx0;
		}
		quad.y0 = dy0;
		quad.y1 = dy1;
		quad.v0 = sy0;
		quad.v1 = sy1;
		if (dy1 < dy0) {
			quad.y0 = dy1;
			quad.y1 = dy0;
			quad.v0 = sy1;
			quad.v1 = sy0;
		}

		/* Each destination surface from its source surface (the nearest slice across depths). */
		for (surface = 0U; surface < destination_surfaces.count; surface++) {
			source_surface = surface;
			if (source_surfaces.count != destination_surfaces.count)
				source_surface = (uint32_t)(((uint64_t)surface * 2U + 1U) * source_surfaces.count / (2U * destination_surfaces.count));
			error = execute_surface_room(session, execute, state);
			if (error == 0)
				error = execute_image_target(destination, destination_layers->mipLevel, destination_surfaces.first + surface,
							     record->opcode == GPU_OP_CMD_COPY_IMAGE ? execute_copy_format(destination->format) : destination->format,
							     &quad.target);
			if (error == 0)
				error = execute_image_source(source, source_layers->mipLevel, source_surfaces.first + source_surface, &quad);
			raw = quad;
			if (record->opcode == GPU_OP_CMD_COPY_IMAGE)
				execute_raw_quad(&raw, nvrtx_vk_format_bytes(source->format));
			if (error == 0)
				error = execute_quad(session, execute, state, &raw);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Reports the 2D surfaces a region of an image names: a 3D image's slices
 * z0 up to z1, another image's layers (the rest of them for
 * VK_REMAINING_ARRAY_LAYERS).  EINVAL for none, or slices given backwards.
 */
static int
execute_region_surfaces(
	const struct nvrtx_vk_image *image,
	const VkImageSubresourceLayers *layers,
	int32_t z0,
	int32_t z1,
	struct execute_surfaces *surfaces)
{
	/* A 3D image: its slices, in order. */
	if (image->type == VK_IMAGE_TYPE_3D) {
		if (z0 < 0 || z1 <= z0)
			return EINVAL;
		surfaces->first = (uint32_t)z0;
		surfaces->count = (uint32_t)(z1 - z0);
		return 0;
	}

	/* Another image: its layers, of which there are some. */
	surfaces->first = layers->baseArrayLayer;
	surfaces->count = layers->layerCount;
	if (surfaces->count == VK_REMAINING_ARRAY_LAYERS && layers->baseArrayLayer < image->layers)
		surfaces->count = image->layers - layers->baseArrayLayer;
	if (surfaces->count == 0U)
		return EINVAL;

	/* Succeeded: the layers (the image's surfaces checks each). */
	return 0;
}

/*
 * Fills a rectangle copy's remap for the aspects of a depth format copied
 * one way, and reports the bytes a buffer's texel of them takes: a
 * buffer holds the aspect's texels alone (a depth of D16_UNORM two bytes,
 * of the others four, D24_UNORM_S8_UINT's in its low three bytes; a
 * stencil one byte), an image its whole texels, of which a copy into it
 * writes only the aspect's components and leaves the rest as they are.
 * Between images the aspects named are copied texel by texel.  ENOTSUP
 * for a buffer's copy of D32_SFLOAT_S8_UINT's stencil (its byte lies past
 * the four components the engine remaps), EINVAL for aspects the format
 * does not have or a buffer's copy of both.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_copy.c
 * (nvk_CmdCopyBufferToImage2 and nvk_CmdCopyImageToBuffer2: the remaps
 * of D24_UNORM_S8_UINT's and D32_SFLOAT_S8_UINT's aspects;
 * nouveau_copy_remap_format: another format's texels as components of
 * up to four bytes).
 */
static int
execute_depth_remap(
	uint32_t format,
	uint32_t aspects,
	uint32_t way,
	struct nvrtx_exec_rect_copy *copy,
	uint32_t *buffer_texel)
{
	const struct nvrtx_exec_depth_format *depth;
	uint32_t index;
	uint32_t whole;

	/* The format's entry; the aspects it has, one at a time with a buffer. */
	depth = nvrtx_exec_depth_format_find(format);
	if (depth == NULL || aspects == 0U)
		return EINVAL;
	whole = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (depth->stencil != 0U)
		whole |= VK_IMAGE_ASPECT_STENCIL_BIT;
	if ((aspects & ~whole) != 0U)
		return EINVAL;
	if (way != EXECUTE_DEPTH_IMAGES && aspects != VK_IMAGE_ASPECT_DEPTH_BIT && aspects != VK_IMAGE_ASPECT_STENCIL_BIT)
		return EINVAL;

	/* Nothing written unless named. */
	for (index = 0U; index < 4U; index++)
		copy->remap[index] = NVRTX_EXEC_COPY_KEEP;

	/* A format of one aspect, or both aspects between images: the whole texel in components of up to four bytes. */
	*buffer_texel = depth->bytes;
	if (aspects == whole) {
		copy->component_bytes = depth->bytes;
		if (copy->component_bytes > 4U)
			copy->component_bytes = 4U;
		copy->source_components = depth->bytes / copy->component_bytes;
		copy->destination_components = copy->source_components;
		for (index = 0U; index < copy->source_components; index++)
			copy->remap[index] = index;
		return 0;
	}

	/* D24_UNORM_S8_UINT: bytes; its depth the low three, its stencil the high one. */
	if (format == VK_FORMAT_D24_UNORM_S8_UINT) {
		copy->component_bytes = 1U;
		copy->source_components = 4U;
		copy->destination_components = 4U;
		if (aspects == VK_IMAGE_ASPECT_DEPTH_BIT) {
			*buffer_texel = 4U;
			copy->remap[0] = 0U;
			copy->remap[1] = 1U;
			copy->remap[2] = 2U;
			return 0;
		}
		*buffer_texel = 1U;
		if (way == EXECUTE_DEPTH_TO_IMAGE) {
			copy->source_components = 1U;
			copy->remap[3] = 0U;
		} else if (way == EXECUTE_DEPTH_FROM_IMAGE) {
			copy->destination_components = 1U;
			copy->remap[0] = 3U;
		} else {
			copy->remap[3] = 3U;
		}
		return 0;
	}

	/* D32_SFLOAT_S8_UINT: words; its depth the first, its stencil (a buffer's copy of it not done) in the second. */
	copy->component_bytes = 4U;
	copy->source_components = 2U;
	copy->destination_components = 2U;
	if (aspects == VK_IMAGE_ASPECT_STENCIL_BIT) {
		if (way != EXECUTE_DEPTH_IMAGES)
			return ENOTSUP;
		copy->remap[1] = 1U;
		return 0;
	}
	*buffer_texel = 4U;
	copy->remap[0] = 0U;
	if (way == EXECUTE_DEPTH_TO_IMAGE)
		copy->source_components = 1U;
	if (way == EXECUTE_DEPTH_FROM_IMAGE)
		copy->destination_components = 1U;

	/* Succeeded: the remap is filled. */
	return 0;
}

/* Fills one side of a rectangle copy: a surface (block linear for an image's, rows for a buffer's) and the rectangle's origin on it. */
static void
execute_depth_side(
	struct nvrtx_exec_copy_side *side,
	uint64_t va,
	uint64_t offset,
	uint32_t pitch,
	uint32_t rows,
	int32_t x,
	int32_t y)
{
	/* The surface's first byte and pitch; an image's is block linear and has rows. */
	side->address = va + offset;
	side->pitch = pitch;
	side->block_linear = 0U;
	side->rows = 0U;
	if (rows != 0U) {
		side->block_linear = 1U;
		side->rows = rows;
	}

	/* Succeeded: the origin. */
	side->x = (uint32_t)x;
	side->y = (uint32_t)y;
}

/*
 * Copies regions between a buffer and a depth image's depth or stencil
 * by the copy engine (execute_depth_remap()), each layer of a region from
 * or to its own run of the buffer's rows.  EINVAL for a region outside
 * the image or the buffer, or the remap's error.
 */
static int
execute_depth_buffer(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_image *image,
	const struct nvrtx_vk_buffer *buffer,
	const struct nvrtx_vk_record *record,
	bool to_image)
{
	const VkBufferImageCopy *regions;
	const VkBufferImageCopy *region;
	struct nvrtx_exec_rect_copy copy;
	struct nvrtx_exec_copy_side image_side;
	struct nvrtx_exec_copy_side buffer_side;
	struct execute_surfaces surfaces;
	uint64_t buffer_va;
	uint64_t image_va;
	uint64_t surface_bytes;
	uint64_t offset;
	uint8_t *cpu;
	uint32_t index;
	uint32_t surface;
	uint32_t texel;
	uint32_t row;
	uint32_t rows;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	bool inside;
	int error;

	/* The two storages. */
	error = nvrtx_vk_buffer_address(buffer, &buffer_va, &cpu);
	if (error == 0)
		error = nvrtx_vk_image_address(image, &image_va, &cpu);
	if (error != 0)
		return error;

	/* Each region. */
	regions = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		region = &regions[index];

		/* The aspect's remap and the buffer's texel. */
		kern_memset(&copy, 0, sizeof(copy));
		error = execute_depth_remap(image->format, region->imageSubresource.aspectMask,
					    to_image ? EXECUTE_DEPTH_TO_IMAGE : EXECUTE_DEPTH_FROM_IMAGE, &copy, &texel);
		if (error != 0)
			return error;

		/* Its layers; the buffer's rows (a row length of zero the region's width), every layer inside the buffer. */
		error = execute_region_surfaces(image, &region->imageSubresource, 0, 1, &surfaces);
		if (error != 0)
			return error;
		if (region->imageExtent.width == 0U || region->imageExtent.height == 0U || region->imageExtent.depth != 1U ||
		    region->imageOffset.z != 0)
			return EINVAL;
		row = region->bufferRowLength;
		if (row == 0U)
			row = region->imageExtent.width;
		rows = region->bufferImageHeight;
		if (rows == 0U)
			rows = region->imageExtent.height;
		if (row < region->imageExtent.width || rows < region->imageExtent.height)
			return EINVAL;
		surface_bytes = (uint64_t)row * texel * rows;
		if (region->bufferOffset > buffer->size ||
		    surface_bytes * (surfaces.count - 1U) + (uint64_t)row * texel * (region->imageExtent.height - 1U) +
		    (uint64_t)region->imageExtent.width * texel > buffer->size - region->bufferOffset)
			return EINVAL;

		/* Each layer from or to its run of the buffer. */
		for (surface = 0U; surface < surfaces.count; surface++) {
			error = nvrtx_vk_image_surface(image, region->imageSubresource.mipLevel, surfaces.first + surface, &offset, &width,
						       &height, &pitch);
			if (error != 0)
				return error;
			inside = execute_region_inside(region->imageOffset.x, region->imageOffset.y, region->imageExtent.width,
						       region->imageExtent.height, width, height);
			if (!inside)
				return EINVAL;
			error = execute_surface_room(session, execute, state);
			if (error != 0)
				return error;
			execute_depth_side(&image_side, image_va, offset, pitch, height, region->imageOffset.x, region->imageOffset.y);
			execute_depth_side(&buffer_side, buffer_va, region->bufferOffset + surface_bytes * surface, row * texel, 0U, 0, 0);
			copy.source = buffer_side;
			copy.destination = image_side;
			if (!to_image) {
				copy.source = image_side;
				copy.destination = buffer_side;
			}
			copy.width = region->imageExtent.width;
			copy.height = region->imageExtent.height;
			execute->at = nvrtx_exec_copy_rect(execute->cpu + EXECUTE_PUSH_AT, execute->at, &copy);
		}
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Copies regions between two depth images of one format by the copy
 * engine (execute_depth_remap()), layer by layer: a copy's regions, and a
 * blit's that is a copy (nearest, of one size, not mirrored).  ENOTSUP for
 * another blit, EINVAL for images of other formats, a colour image or a
 * region outside either.
 */
static int
execute_depth_images(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_image *source;
	const struct nvrtx_vk_image *destination;
	const VkImageCopy *copies;
	const VkImageBlit *blits;
	const VkImageSubresourceLayers *source_layers;
	const VkImageSubresourceLayers *destination_layers;
	struct nvrtx_exec_rect_copy copy;
	struct execute_surfaces source_surfaces;
	struct execute_surfaces destination_surfaces;
	uint64_t source_va;
	uint64_t destination_va;
	uint64_t source_offset;
	uint64_t destination_offset;
	uint8_t *cpu;
	uint32_t index;
	uint32_t surface;
	uint32_t texel;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t source_pitch;
	uint32_t destination_width;
	uint32_t destination_height;
	uint32_t destination_pitch;
	uint32_t width;
	uint32_t height;
	int32_t sx;
	int32_t sy;
	int32_t dx;
	int32_t dy;
	bool inside;
	int error;

	/* Two depth images of one format, with storage. */
	source = record->objects[0]->payload;
	destination = record->objects[1]->payload;
	if (!source->depth || !destination->depth || source->format != destination->format || source->samples > 1U || destination->samples > 1U)
		return EINVAL;
	error = nvrtx_vk_image_address(source, &source_va, &cpu);
	if (error == 0)
		error = nvrtx_vk_image_address(destination, &destination_va, &cpu);
	if (error != 0)
		return error;

	/* Each region: a copy's, or a blit's that is one (nearest, of one size, in order). */
	copies = record->regions;
	blits = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		if (record->opcode == GPU_OP_CMD_COPY_IMAGE) {
			source_layers = &copies[index].srcSubresource;
			destination_layers = &copies[index].dstSubresource;
			sx = copies[index].srcOffset.x;
			sy = copies[index].srcOffset.y;
			dx = copies[index].dstOffset.x;
			dy = copies[index].dstOffset.y;
			width = copies[index].extent.width;
			height = copies[index].extent.height;
			if (copies[index].extent.depth != 1U)
				return EINVAL;
		} else {
			source_layers = &blits[index].srcSubresource;
			destination_layers = &blits[index].dstSubresource;
			sx = blits[index].srcOffsets[0].x;
			sy = blits[index].srcOffsets[0].y;
			dx = blits[index].dstOffsets[0].x;
			dy = blits[index].dstOffsets[0].y;
			if (blits[index].srcOffsets[1].x <= sx || blits[index].srcOffsets[1].y <= sy)
				return ENOTSUP;
			width = (uint32_t)(blits[index].srcOffsets[1].x - sx);
			height = (uint32_t)(blits[index].srcOffsets[1].y - sy);
			if (blits[index].dstOffsets[1].x - dx != (int32_t)width || blits[index].dstOffsets[1].y - dy != (int32_t)height)
				return ENOTSUP;
			if (record->values[0] != VK_FILTER_NEAREST)
				return ENOTSUP;
		}

		/* The aspects' remap (both sides name the same), the layers of each side, as many of one as of the other. */
		if (source_layers->aspectMask != destination_layers->aspectMask)
			return EINVAL;
		kern_memset(&copy, 0, sizeof(copy));
		error = execute_depth_remap(source->format, source_layers->aspectMask, EXECUTE_DEPTH_IMAGES, &copy, &texel);
		if (error == 0)
			error = execute_region_surfaces(source, source_layers, 0, 1, &source_surfaces);
		if (error == 0)
			error = execute_region_surfaces(destination, destination_layers, 0, 1, &destination_surfaces);
		if (error != 0)
			return error;
		if (source_surfaces.count != destination_surfaces.count || width == 0U || height == 0U)
			return EINVAL;

		/* Each layer to its layer. */
		for (surface = 0U; surface < source_surfaces.count; surface++) {
			error = nvrtx_vk_image_surface(source, source_layers->mipLevel, source_surfaces.first + surface, &source_offset,
						       &source_width, &source_height, &source_pitch);
			if (error == 0)
				error = nvrtx_vk_image_surface(destination, destination_layers->mipLevel, destination_surfaces.first + surface,
							       &destination_offset, &destination_width, &destination_height, &destination_pitch);
			if (error != 0)
				return error;
			inside = execute_region_inside(sx, sy, width, height, source_width, source_height);
			if (!inside)
				return EINVAL;
			inside = execute_region_inside(dx, dy, width, height, destination_width, destination_height);
			if (!inside)
				return EINVAL;
			error = execute_surface_room(session, execute, state);
			if (error != 0)
				return error;
			execute_depth_side(&copy.source, source_va, source_offset, source_pitch, source_height, sx, sy);
			execute_depth_side(&copy.destination, destination_va, destination_offset, destination_pitch, destination_height, dx, dy);
			copy.width = width;
			copy.height = height;
			execute->at = nvrtx_exec_copy_rect(execute->cpu + EXECUTE_PUSH_AT, execute->at, &copy);
		}
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Copies regions between a buffer and an image, each layer (a 3D image's
 * slice) of a region from or to its own run of the buffer's rows.  A
 * buffer surface laid out as a texture can be (to an image) or as a target
 * can be (from an image) is drawn; another is copied by the CPU after the
 * work before it ran.  A video image's planes and a depth image's depth or
 * stencil are copied by the copy engine.
 */
static int
execute_buffer_image(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_buffer *buffer;
	const struct nvrtx_vk_image *image;
	const VkBufferImageCopy *regions;
	const VkBufferImageCopy *region;
	struct execute_surfaces surfaces;
	bool to_image;
	uint64_t buffer_va;
	uint64_t image_va;
	uint64_t surface_bytes;
	uint8_t *buffer_cpu;
	uint8_t *image_cpu;
	uint32_t index;
	uint32_t surface;
	uint32_t row;
	uint32_t rows;
	uint32_t pitch;
	int error;

	/* The buffer and the image, with storage. */
	to_image = (record->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE);
	if (to_image) {
		buffer = record->objects[0]->payload;
		image = record->objects[1]->payload;
	} else {
		image = record->objects[0]->payload;
		buffer = record->objects[1]->payload;
	}
	error = nvrtx_vk_buffer_address(buffer, &buffer_va, &buffer_cpu);
	if (error == 0)
		error = nvrtx_vk_image_address(image, &image_va, &image_cpu);

	/* Refuses a copy into a video image, which only the decoder writes, and of a multisampled image. */
	if (error == 0 &&
	    image->video &&
	    to_image)
		error = EINVAL;
	if (error == 0 && image->samples > 1U)
		error = EINVAL;

	/* Propagates a refusal. */
	if (error != 0)
		return error;

	/* A video image's planes are read out by the copy engine. */
	if (image->video) {
		error = execute_video_copy(execute, image, image_va, buffer, buffer_va, record);
		if (error != 0)
			return error;
		return 0;
	}

	/* A depth image's depth or stencil goes by the copy engine too. */
	if (image->depth)
		return execute_depth_buffer(session, execute, state, image, buffer, record, to_image);

	/* Each region. */
	regions = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		region = &regions[index];

		/*
		 * Its surfaces; the buffer's rows (a row length of zero is the
		 * region's width) and each surface's rows (an image height of zero
		 * is the region's height), every surface inside the buffer.
		 */
		error = execute_region_surfaces(image,
						&region->imageSubresource,
						region->imageOffset.z,
						region->imageOffset.z + (int32_t)region->imageExtent.depth,
						&surfaces);
		if (error != 0)
			return error;
		if (region->imageExtent.width == 0U || region->imageExtent.height == 0U)
			return EINVAL;
		row = region->bufferRowLength;
		if (row == 0U)
			row = region->imageExtent.width;
		rows = region->bufferImageHeight;
		if (rows == 0U)
			rows = region->imageExtent.height;
		if (row < region->imageExtent.width || rows < region->imageExtent.height)
			return EINVAL;
		pitch = row * nvrtx_vk_format_bytes(image->format);
		surface_bytes = (uint64_t)pitch * rows;
		if (region->bufferOffset > buffer->size ||
		    surface_bytes * (surfaces.count - 1U) + (uint64_t)pitch * (region->imageExtent.height - 1U) +
		    (uint64_t)region->imageExtent.width * nvrtx_vk_format_bytes(image->format) > buffer->size - region->bufferOffset)
			return EINVAL;

		/* Each surface from its run of the buffer. */
		for (surface = 0U; surface < surfaces.count; surface++) {
			error = execute_buffer_surface(session,
						       execute,
						       state,
						       buffer,
						       image,
						       region,
						       surfaces.first + surface,
						       region->bufferOffset + surface_bytes * surface,
						       to_image);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Copies one surface of a region between a buffer and an image: the layer
 * (or slice) of the region's level, its buffer rows from offset.  The GPU
 * draws it when the buffer can be its texture (whole pixels, rows on 32
 * bytes) or its target (start on 256 bytes, rows on 128); the CPU copies
 * any other after the work before it.
 */
static int
execute_buffer_surface(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_buffer *buffer,
	const struct nvrtx_vk_image *image,
	const VkBufferImageCopy *region,
	uint32_t layer,
	uint64_t offset,
	bool to_image)
{
	struct execute_quad quad;
	uint64_t buffer_va;
	uint64_t start;
	uint64_t level_offset;
	uint8_t *buffer_cpu;
	uint32_t row;
	uint32_t pitch;
	uint32_t skew;
	uint32_t level;
	uint32_t level_width;
	uint32_t level_height;
	uint32_t level_pitch;
	uint32_t texel;
	bool inside;
	bool drawn;
	int error;

	/* The surface of the level, the region inside it. */
	level = region->imageSubresource.mipLevel;
	error = nvrtx_vk_image_surface(image, level, layer, &level_offset, &level_width, &level_height, &level_pitch);
	if (error != 0)
		return error;
	inside = execute_region_inside(region->imageOffset.x, region->imageOffset.y, region->imageExtent.width,
				       region->imageExtent.height, level_width, level_height);
	if (!inside)
		return EINVAL;

	/* The buffer's rows from the surface's start. */
	error = nvrtx_vk_buffer_address(buffer, &buffer_va, &buffer_cpu);
	if (error != 0)
		return error;
	row = region->bufferRowLength;
	if (row == 0U)
		row = region->imageExtent.width;
	texel = nvrtx_vk_format_bytes(image->format);
	pitch = row * texel;
	start = buffer_va + offset;

	/* Drawn when the buffer can be the quad's texture or target. */
	drawn = false;
	if (to_image) {
		if ((start % texel) == 0U && (pitch % EXECUTE_TEXTURE_PITCH_ALIGN) == 0U)
			drawn = true;
	} else if ((start % EXECUTE_BUFFER_START_ALIGN) == 0U && (pitch % EXECUTE_BUFFER_PITCH_ALIGN) == 0U) {
		drawn = true;
	}

	/* Any other surface is copied by the CPU, after the work before it. */
	if (!drawn) {
		error = execute_drain(session, execute);
		if (error == 0)
			error = execute_cpu_copy(session, buffer, image, region, layer, offset, to_image);
		if (error != 0)
			return error;

		/* Counted for nvrtx.vk.perf=1; the next draw writes its target anew. */
		nvrtx_vk_gpu_copied(session->owner, (uint64_t)region->imageExtent.width * texel * region->imageExtent.height);
		state->target_written = false;
		return 0;
	}

	/* Drawn, with room for the quad: the destination's region samples the source's, one to one. */
	error = execute_surface_room(session, execute, state);
	if (error != 0)
		return error;
	kern_memset(&quad, 0, sizeof(quad));
	quad.filter = VK_FILTER_NEAREST;
	if (to_image) {
		/* The texture from the 32-byte boundary at or before the region; the region skew texels into its rows. */
		skew = (uint32_t)(start % EXECUTE_TEXTURE_START_ALIGN) / texel;
		error = execute_image_target(image, level, layer, execute_copy_format(image->format), &quad.target);
		quad.x0 = region->imageOffset.x;
		quad.y0 = region->imageOffset.y;
		quad.source = start - (uint64_t)skew * texel;
		quad.source_width = skew + region->imageExtent.width;
		quad.source_height = region->imageExtent.height;
		quad.source_pitch = pitch;
		quad.source_format = image->format;
		quad.u0 = (int32_t)skew;
		quad.v0 = 0;
	} else {
		quad.target.address = start;
		quad.target.width = region->imageExtent.width;
		quad.target.height = region->imageExtent.height;
		quad.target.pitch = pitch;
		quad.target.format = image->format;
		quad.x0 = 0;
		quad.y0 = 0;
		error = execute_image_source(image, level, layer, &quad);
		quad.u0 = region->imageOffset.x;
		quad.v0 = region->imageOffset.y;
	}
	if (error != 0)
		return error;
	quad.x1 = quad.x0 + (int32_t)region->imageExtent.width;
	quad.y1 = quad.y0 + (int32_t)region->imageExtent.height;
	quad.u1 = quad.u0 + (int32_t)region->imageExtent.width;
	quad.v1 = quad.v0 + (int32_t)region->imageExtent.height;
	execute_raw_quad(&quad, texel);
	error = execute_quad(session, execute, state, &quad);
	if (error != 0)
		return error;

	/* Succeeded: the surface is copied. */
	return 0;
}

/*
 * Copies one surface of a region between a buffer and an image by the
 * CPU, row by row through the BAR1 window (the GPU is idle: the work
 * before has run): the layer (or slice) of the region's level, its buffer
 * rows from offset.  The two storages take their place in the window now
 * if they have none.
 */
static int
execute_cpu_copy(
	struct nvrtx_vk_session *session,
	const struct nvrtx_vk_buffer *buffer,
	const struct nvrtx_vk_image *image,
	const VkBufferImageCopy *region,
	uint32_t layer,
	uint64_t offset,
	bool to_image)
{
	uint8_t *buffer_cpu;
	uint8_t *image_cpu;
	uint8_t *buffer_row;
	uint8_t *image_row;
	uint64_t level_offset;
	uint32_t level_width;
	uint32_t level_height;
	uint32_t level_pitch;
	uint32_t row_pixels;
	uint32_t bytes;
	uint32_t texel;
	uint32_t piece;
	uint32_t column;
	uint32_t run;
	uint32_t x;
	uint32_t y;
	int error;

	/* The two views. */
	run = 1U;
	error = nvrtx_vk_buffer_cpu(session, buffer, &buffer_cpu);
	if (error == 0)
		error = nvrtx_vk_image_cpu(session, image, &image_cpu);
	if (error != 0)
		return error;

	/* The surface's place in the image. */
	error = nvrtx_vk_image_surface(image, region->imageSubresource.mipLevel, layer, &level_offset, &level_width, &level_height, &level_pitch);
	if (error != 0)
		return error;

	/* The rows: the buffer's at its row length, the image's at its level's pitch. */
	row_pixels = region->bufferRowLength;
	if (row_pixels == 0U)
		row_pixels = region->imageExtent.width;
	texel = nvrtx_vk_format_bytes(image->format);
	piece = EXECUTE_GOB_PIECE_BYTES / texel;
	bytes = region->imageExtent.width * texel;
	for (y = 0U; y < region->imageExtent.height; y++) {
		buffer_row = buffer_cpu + offset + (uint64_t)y * row_pixels * texel;

		/*
		 * A tiled image's texels where its GOB keeps them: the texels of a
		 * 16-byte piece lie together, so a whole piece goes at once and
		 * only the row's ragged ends a texel at a time.
		 */
		if (image->tiled) {
			for (x = 0U; x < region->imageExtent.width; x += run) {
				column = ((uint32_t)region->imageOffset.x + x) * texel;
				run = 1U;
				if ((column % EXECUTE_GOB_PIECE_BYTES) == 0U && region->imageExtent.width - x >= piece)
					run = piece;
				image_row = image_cpu + level_offset + execute_gob_offset(level_pitch, column, (uint32_t)region->imageOffset.y + y);
				if (to_image) {
					kern_memcpy(image_row, buffer_row + x * texel, run * texel);
				} else {
					kern_memcpy(buffer_row + x * texel, image_row, run * texel);
				}
			}
			continue;
		}

		/* A pitch-linear image's row at once. */
		image_row = image_cpu + level_offset + (uint64_t)(region->imageOffset.y + (int32_t)y) * level_pitch +
		    (uint64_t)region->imageOffset.x * texel;
		if (to_image) {
			kern_memcpy(image_row, buffer_row, bytes);
		} else {
			kern_memcpy(buffer_row, image_row, bytes);
		}
	}

	/* Succeeded: the region is copied. */
	return 0;
}

/*
 * Reports where a byte of a block-linear surface in one-GOB blocks lies: its
 * GOB (rows of GOBs, pitch bytes a row wide), then its 16-byte piece within
 * the GOB and its place in the piece.
 *
 * Value referenced: Mesa 25.0.7 src/nouveau/nil/copy.rs
 * (CopyGOBTuring2D::for_each_16b, the 16-byte pieces of a 64-byte by 8-row
 * GOB).
 */
static uint64_t
execute_gob_offset(
	uint32_t pitch,
	uint32_t x,
	uint32_t y)
{
	uint64_t gob;
	uint32_t inside;

	/* The GOB, then the byte within it. */
	gob = (uint64_t)(y / NVRTX_VK_GOB_HEIGHT) * (pitch / NVRTX_VK_GOB_WIDTH) + x / NVRTX_VK_GOB_WIDTH;
	x %= NVRTX_VK_GOB_WIDTH;
	y %= NVRTX_VK_GOB_HEIGHT;
	inside = (x & 15U) | ((y & 3U) << 4) | ((x & 16U) << 2) | ((y & 4U) << 5) | ((x & 32U) << 3);

	/* Succeeded: the byte's offset. */
	return gob * NVRTX_VK_GOB_BYTES + inside;
}

/*
 * Copies regions between two buffers by the CPU through the BAR1 window,
 * after the work before it ran (the copy is a one-off upload: a model's
 * vertices and indices from a staging buffer).
 */
static int
execute_copy_buffer(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_buffer *source;
	const struct nvrtx_vk_buffer *destination;
	const VkBufferCopy *regions;
	uint8_t *source_cpu;
	uint8_t *destination_cpu;
	uint32_t index;
	int error;

	source = record->objects[0]->payload;
	destination = record->objects[1]->payload;

	/* Each region inside both buffers. */
	regions = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		if (regions[index].srcOffset > source->size || regions[index].size > source->size - regions[index].srcOffset)
			return EINVAL;
		if (regions[index].dstOffset > destination->size || regions[index].size > destination->size - regions[index].dstOffset)
			return EINVAL;
	}

	/* The work before has run; the two views. */
	error = execute_drain(session, execute);
	if (error == 0)
		error = nvrtx_vk_buffer_cpu(session, source, &source_cpu);
	if (error == 0)
		error = nvrtx_vk_buffer_cpu(session, destination, &destination_cpu);
	if (error != 0)
		return error;

	/* The regions (kern_memmove: they may overlap within one buffer). */
	for (index = 0U; index < record->region_count; index++) {
		kern_memmove(destination_cpu + regions[index].dstOffset, source_cpu + regions[index].srcOffset, (size_t)regions[index].size);
		nvrtx_vk_gpu_copied(session->owner, regions[index].size);
	}

	/* Succeeded: the regions are copied. */
	return 0;
}

/*
 * Writes an update's bytes or a fill's word into a buffer, as a copy
 * between buffers is done: by the CPU once the work before has run.  A
 * fill of VK_WHOLE_SIZE fills to the buffer's end, rounded down to whole
 * words.  EINVAL for a range outside the buffer or not of whole words.
 *
 * Procedure referenced: Vulkan 1.3 vkCmdUpdateBuffer and vkCmdFillBuffer
 * (VUID-vkCmdFillBuffer-size-00028: VK_WHOLE_SIZE is the rest of the
 * buffer, rounded down to a multiple of 4).
 */
static int
execute_write_buffer(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_buffer *buffer;
	uint8_t *cpu;
	uint64_t offset;
	uint64_t size;
	uint64_t index;
	uint32_t word;
	int error;

	buffer = record->objects[0]->payload;
	offset = record->offsets[0];

	/* The range: an update's bytes, or a fill's size (VK_WHOLE_SIZE the rest in whole words). */
	if (offset > buffer->size)
		return EINVAL;
	size = record->region_count;
	if (record->opcode == GPU_OP_CMD_FILL_BUFFER) {
		size = record->offsets[1];
		if (size == VK_WHOLE_SIZE)
			size = (buffer->size - offset) & ~(uint64_t)3U;
	}
	if ((size & 3U) != 0U || size > buffer->size - offset)
		return EINVAL;

	/* The work before has run; the buffer's view. */
	error = execute_drain(session, execute);
	if (error == 0)
		error = nvrtx_vk_buffer_cpu(session, buffer, &cpu);
	if (error != 0)
		return error;

	/* The bytes, or the word over and over. */
	if (record->opcode == GPU_OP_CMD_UPDATE_BUFFER) {
		kern_memcpy(cpu + offset, record->regions, (size_t)size);
	} else {
		word = record->values[0];
		for (index = 0U; index < size; index += 4U)
			kern_memcpy(cpu + offset + index, &word, 4U);
	}
	nvrtx_vk_gpu_copied(session->owner, size);

	/* Succeeded: the buffer holds the bytes. */
	return 0;
}

/*
 * Clears the ranges of an image to a colour: each level of a range, each
 * of its layers (every slice of a 3D image's level) as a target, its
 * whole rectangle cleared.  VK_REMAINING_MIP_LEVELS and
 * VK_REMAINING_ARRAY_LAYERS are the rest of the image's.
 */
static int
execute_clear_image(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_image *image;
	const VkImageSubresourceRange *ranges;
	const uint32_t *colour;
	struct nvrtx_exec_target target;
	uint32_t packed[4];
	uint32_t format;
	uint32_t index;
	uint32_t level;
	uint32_t level_end;
	uint32_t layer;
	uint32_t layer_end;
	uint8_t *push;
	int error;

	image = record->objects[0]->payload;
	ranges = record->regions;

	/*
	 * The format cleared and its colour: a shared exponent image, which
	 * is not rendered to, as the integer format of its size, the colour
	 * packed.
	 */
	format = image->format;
	colour = record->values;
	if (format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) {
		kern_memset(packed, 0, sizeof(packed));
		packed[0] = nvrtx_exec_rgb9e5(record->values);
		format = VK_FORMAT_R32_UINT;
		colour = packed;
	}

	/* Each range's levels, within the image. */
	for (index = 0U; index < record->region_count; index++) {
		if (ranges[index].baseMipLevel >= image->levels)
			return EINVAL;
		level_end = image->levels;
		if (ranges[index].levelCount != VK_REMAINING_MIP_LEVELS && ranges[index].levelCount < image->levels - ranges[index].baseMipLevel)
			level_end = ranges[index].baseMipLevel + ranges[index].levelCount;
		for (level = ranges[index].baseMipLevel; level < level_end; level++) {
			/* The level's layers in the range, of which there is one at least (a 3D image's every slice). */
			layer = ranges[index].baseArrayLayer;
			layer_end = nvrtx_vk_image_surfaces(image, level);
			if (image->type == VK_IMAGE_TYPE_3D) {
				layer = 0U;
			} else if (layer >= layer_end) {
				return EINVAL;
			} else if (ranges[index].layerCount != VK_REMAINING_ARRAY_LAYERS && ranges[index].layerCount < layer_end - layer) {
				layer_end = layer + ranges[index].layerCount;
			}

			/* Each, with room for it, as a target, then its whole rectangle cleared. */
			while (layer < layer_end) {
				error = execute_surface_room(session, execute, state);
				if (error == 0)
					error = execute_image_target(image, level, layer, format, &target);
				if (error != 0)
					return error;
				push = execute->cpu + EXECUTE_PUSH_AT;
				execute->at = nvrtx_exec_target(push, execute->at, &target);
				execute->at = nvrtx_exec_clear(push, execute->at, 0U, target.format, 0U, colour, 0U, 0U, target.width, target.height);
				layer++;
			}
		}
	}
	state->target_written = false;

	/* Succeeded: the clears are written. */
	return 0;
}

/*
 * Resolves a rectangle of a layer of an image of four samples a pixel into
 * a level and layer of an image of one, in its format, by a quad: the
 * source read as a surface of twice its width and height of samples (a
 * pixel's samples a 2x2 block), a destination pixel's centre on the
 * corner its 2x2 block's four samples share, so a linear filter averages
 * the four alike.  EINVAL for images of other samples or formats, or a
 * rectangle outside either.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/nil/image.rs
 * (Image::new for the 2X2 sample layout: an image of samples twice the
 * pixels each way).
 */
static int
execute_resolve(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_image *source,
	uint32_t source_layer,
	int32_t sx,
	int32_t sy,
	const struct nvrtx_vk_image *destination,
	uint32_t level,
	uint32_t layer,
	int32_t dx,
	int32_t dy,
	uint32_t width,
	uint32_t height)
{
	struct execute_quad quad;
	uint64_t va;
	uint64_t offset;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t pitch;
	uint8_t *cpu;
	bool inside;
	int error;

	/* Four samples into one, of one format; the rectangle inside the source. */
	if (source->samples != 4U || destination->samples > 1U || source->format != destination->format || source->depth)
		return EINVAL;
	error = nvrtx_vk_image_address(source, &va, &cpu);
	if (error == 0)
		error = nvrtx_vk_image_surface(source, 0U, source_layer, &offset, &source_width, &source_height, &pitch);
	if (error != 0)
		return error;
	inside = execute_region_inside(sx, sy, width, height, source_width, source_height);
	if (!inside || width == 0U || height == 0U)
		return EINVAL;

	/* With room for the quad: the destination's rectangle as the target. */
	error = execute_surface_room(session, execute, state);
	if (error != 0)
		return error;
	kern_memset(&quad, 0, sizeof(quad));
	error = execute_image_target(destination, level, layer, destination->format, &quad.target);
	if (error != 0)
		return error;
	quad.x0 = dx;
	quad.y0 = dy;
	quad.x1 = dx + (int32_t)width;
	quad.y1 = dy + (int32_t)height;

	/* The source's samples twice the pixels each way, filtered linearly. */
	quad.source = va + offset;
	quad.source_width = source_width * 2U;
	quad.source_height = source_height * 2U;
	quad.source_pitch = pitch;
	quad.source_format = source->format;
	quad.source_tiled = true;
	quad.u0 = sx * 2;
	quad.v0 = sy * 2;
	quad.u1 = (sx + (int32_t)width) * 2;
	quad.v1 = (sy + (int32_t)height) * 2;
	quad.filter = VK_FILTER_LINEAR;
	error = execute_quad(session, execute, state, &quad);
	if (error != 0)
		return error;

	/* Succeeded: the rectangle is resolved. */
	return 0;
}

/*
 * Runs vkCmdResolveImage: each region's layers resolved one by one
 * (execute_resolve()).  EINVAL for a region of other layer counts or of
 * more than one slice.
 */
static int
execute_resolve_images(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_image *source;
	const struct nvrtx_vk_image *destination;
	const VkImageResolve *regions;
	const VkImageResolve *region;
	struct execute_surfaces source_surfaces;
	struct execute_surfaces destination_surfaces;
	uint32_t index;
	uint32_t surface;
	int error;

	source = record->objects[0]->payload;
	destination = record->objects[1]->payload;
	regions = record->regions;

	/* Each region's layers, as many of the source as of the destination. */
	for (index = 0U; index < record->region_count; index++) {
		region = &regions[index];
		error = execute_region_surfaces(source, &region->srcSubresource, 0, 1, &source_surfaces);
		if (error == 0)
			error = execute_region_surfaces(destination, &region->dstSubresource, 0, 1, &destination_surfaces);
		if (error != 0)
			return error;
		if (source_surfaces.count != destination_surfaces.count || region->extent.depth != 1U || region->srcSubresource.mipLevel != 0U)
			return EINVAL;
		for (surface = 0U; surface < source_surfaces.count; surface++) {
			error = execute_resolve(session, execute, state, source, source_surfaces.first + surface, region->srcOffset.x,
						region->srcOffset.y, destination, region->dstSubresource.mipLevel,
						destination_surfaces.first + surface, region->dstOffset.x, region->dstOffset.y, region->extent.width,
						region->extent.height);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the regions are resolved. */
	return 0;
}

/*
 * Resolves an ended pass's multisampled colour attachments into the
 * attachments its subpass names for them, over its render area within the
 * framebuffer and in each of its layers, then orders that before what
 * follows.
 */
static int
execute_resolve_pass(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state)
{
	const struct nvrtx_vk_render_pass *pass;
	const struct nvrtx_vk_framebuffer *framebuffer;
	const struct nvrtx_vk_view *source;
	const struct nvrtx_vk_view *destination;
	uint32_t index;
	uint32_t layer;
	uint32_t resolve;
	uint32_t attachment;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	bool resolved;
	int error;

	/* The render area within the framebuffer. */
	pass = state->pass;
	framebuffer = state->framebuffer;
	if (pass == NULL || framebuffer == NULL)
		return 0;
	x0 = state->area.offset.x;
	y0 = state->area.offset.y;
	x1 = x0 + (int32_t)state->area.extent.width;
	y1 = y0 + (int32_t)state->area.extent.height;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int32_t)framebuffer->width)
		x1 = (int32_t)framebuffer->width;
	if (y1 > (int32_t)framebuffer->height)
		y1 = (int32_t)framebuffer->height;
	if (x1 <= x0 || y1 <= y0)
		return 0;

	/* Each colour attachment its subpass resolves. */
	resolved = false;
	for (index = 0U; index < pass->colour && index < NVRTX_EXEC_COLOURS; index++) {
		attachment = pass->colour_attachments[index];
		resolve = pass->resolves[index];
		if (attachment == VK_ATTACHMENT_UNUSED || resolve == VK_ATTACHMENT_UNUSED)
			continue;
		if (attachment >= framebuffer->view_count || resolve >= framebuffer->view_count)
			return EINVAL;
		source = framebuffer->views[attachment]->payload;
		destination = framebuffer->views[resolve]->payload;
		for (layer = 0U; layer < framebuffer->layers || layer == 0U || layer < state->target.layers; layer++) {
			if (!execute_layer_drawn(state, layer))
				continue;
			error = execute_resolve(session, execute, state, source->image->payload, source->base_layer + layer, x0, y0,
						destination->image->payload, destination->base_level, destination->base_layer + layer, x0, y0,
						(uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
			if (error != 0)
				return error;
		}
		resolved = true;
	}

	/* Succeeded: what follows reads the resolved pixels. */
	if (resolved) {
		state->target_written = false;
		execute->at = nvrtx_exec_barrier(execute->cpu + EXECUTE_PUSH_AT, execute->at);
	}
	return 0;
}

/*
 * Clears a depth image's ranges: each level and layer of a range as the
 * depth target of a pass of no colour target, its whole rectangle's depth
 * and (for a range that names it, of a format that has it) stencil
 * cleared to the record's values (values 0 and 1).  EINVAL for a level or
 * layer the image does not have or a colour image.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_clear.c
 * (nvk_CmdClearDepthStencilImage: each level and layer rendered as the
 * depth attachment and cleared whole).
 */
static int
execute_clear_depth_image(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	struct nvrtx_vk_image *image;
	const VkImageSubresourceRange *ranges;
	struct nvrtx_exec_target target;
	uint32_t index;
	uint32_t level;
	uint32_t level_end;
	uint32_t layer;
	uint32_t layer_end;
	uint8_t *push;
	bool depth;
	bool stencil;
	int error;

	image = record->objects[0]->payload;
	ranges = record->regions;
	if (!image->depth)
		return EINVAL;

	/* Each range's aspects and levels, within the image. */
	for (index = 0U; index < record->region_count; index++) {
		depth = false;
		if ((ranges[index].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U)
			depth = true;
		stencil = false;
		if (image->stencil && (ranges[index].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U)
			stencil = true;
		if (ranges[index].baseMipLevel >= image->levels)
			return EINVAL;
		level_end = image->levels;
		if (ranges[index].levelCount != VK_REMAINING_MIP_LEVELS && ranges[index].levelCount < image->levels - ranges[index].baseMipLevel)
			level_end = ranges[index].baseMipLevel + ranges[index].levelCount;
		for (level = ranges[index].baseMipLevel; level < level_end; level++) {
			/* The level's layers in the range. */
			layer = ranges[index].baseArrayLayer;
			layer_end = nvrtx_vk_image_surfaces(image, level);
			if (layer >= layer_end)
				return EINVAL;
			if (ranges[index].layerCount != VK_REMAINING_ARRAY_LAYERS && ranges[index].layerCount < layer_end - layer)
				layer_end = layer + ranges[index].layerCount;

			/* Each, with room for it, as the depth target alone, then its whole rectangle cleared. */
			while (layer < layer_end) {
				kern_memset(&target, 0, sizeof(target));
				error = execute_surface_room(session, execute, state);
				if (error == 0)
					error = execute_depth_target(session, image, level, layer, &target);
				if (error != 0)
					return error;
				push = execute->cpu + EXECUTE_PUSH_AT;
				execute->at = nvrtx_exec_target(push, execute->at, &target);
				if (depth)
					execute->at = nvrtx_exec_clear_depth(push, execute->at, 0U, record->values[0], 0U, 0U, target.width,
									     target.height);
				if (stencil)
					execute->at = nvrtx_exec_clear_stencil(push, execute->at, 0U, record->values[1], 0U, 0U, target.width,
									       target.height);
				layer++;
			}
		}
	}
	state->target_written = false;

	/* Succeeded: the clears are written. */
	return 0;
}

/*
 * Draws one internal quad: the destination as the target, the viewport
 * and scissor its region, no blending, the corners, the constants (the
 * quad over the whole viewport and the source's region in pixels) with
 * the source's handle, and the executor's programs.
 */
static int
execute_quad(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct execute_quad *quad)
{
	struct nvrtx_exec_sampler sampler;
	struct nvrtx_exec_vertex vertex;
	struct nvrtx_exec_blend blend;
	struct nvrtx_exec_depth depth;
	static const uint32_t identity[4] = {
		VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A
	};
	uint32_t viewport[6];
	uint32_t header[8];
	uint32_t sampled[8];
	uint32_t handle;
	uint8_t *push;
	uint8_t *constants;
	uint64_t constants_va;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	bool inside;
	int error;

	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(state);

	/* A region inside the target; nothing to draw for an empty one. */
	inside = execute_region_inside(quad->x0, quad->y0, (uint32_t)(quad->x1 - quad->x0), (uint32_t)(quad->y1 - quad->y0),
				       quad->target.width, quad->target.height);
	if (quad->x1 <= quad->x0 || quad->y1 <= quad->y0)
		return 0;
	if (!inside)
		return EINVAL;

	/* The source's header (a tiled source's level as a surface of its own) and a sampler in pixels, clamped. */
	if (quad->source_tiled) {
		nvrtx_exec_texture_header_tiled(header, quad->source, quad->source_width, quad->source_height, 1U, 0U, 1U,
						quad->source_format, identity);
	} else {
		nvrtx_exec_texture_header(header, quad->source, quad->source_width, quad->source_height, quad->source_pitch,
					  quad->source_format, identity);
	}
	kern_memset(&sampler, 0, sizeof(sampler));
	sampler.mag_filter = quad->filter;
	sampler.min_filter = quad->filter;
	sampler.address_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.address_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.address_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.unnormalized = 1U;
	nvrtx_exec_sampler_words(sampled, &sampler);
	error = execute_pool_entry(execute, header, sampled, &handle);
	if (error != 0)
		return error;

	/* The constants: the quad from -1 to 1, the source's region, the handle. */
	error = execute_constants(execute, &constants, &constants_va);
	if (error != 0)
		return error;
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_RECT, EXECUTE_FLOAT_MINUS_ONE);
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_RECT + 4U, EXECUTE_FLOAT_MINUS_ONE);
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_RECT + 8U, EXECUTE_FLOAT_ONE);
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_RECT + 12U, EXECUTE_FLOAT_ONE);
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_TEXTURE, nvrtx_exec_float(quad->u0));
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_TEXTURE + 4U, nvrtx_exec_float(quad->v0));
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_TEXTURE + 8U, nvrtx_exec_float(quad->u1));
	nvrtx_put32(constants, NVRTX_DRAW_PUSH_TEXTURE + 12U, nvrtx_exec_float(quad->v1));
	nvrtx_put32(constants, NVRTX_SM75_CB_TEXTURES, handle);

	/* The target, the viewport and the scissor. */
	push = execute->cpu + EXECUTE_PUSH_AT;
	x0 = quad->x0;
	y0 = quad->y0;
	x1 = quad->x1;
	y1 = quad->y1;
	execute->at = nvrtx_exec_target(push, execute->at, &quad->target);
	execute_full_viewport(viewport, x0, y0, x1, y1);
	execute->at = nvrtx_exec_viewport(push, execute->at, viewport);
	execute->at = nvrtx_exec_scissor(push, execute->at, (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1);

	/* No blending, every channel. */
	kern_memset(&blend, 0, sizeof(blend));
	blend.write_mask = 0xfU;
	execute->at = nvrtx_exec_blend(push, execute->at, 0U, &blend);

	/* No culling, filled, no depth clamp, logic operation or depth test, layer 0 (a pass's draws may have left them on). */
	execute->at = nvrtx_exec_raster(push, execute->at, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0U, 0U, VK_POLYGON_MODE_FILL, 0U);
	execute->at = nvrtx_exec_layer_source(push, execute->at, 0U);
	execute->at = nvrtx_exec_sample_mask(push, execute->at, 0xffffU);
	execute->at = nvrtx_exec_logic_op(push, execute->at, 0U, 0U);
	kern_memset(&depth, 0, sizeof(depth));
	execute->at = nvrtx_exec_depth(push, execute->at, &depth);

	/* The corners at location 0, the constants, the programs, two triangles. */
	kern_memset(&vertex, 0, sizeof(vertex));
	vertex.attribute_count = 1U;
	vertex.attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
	vertex.streams[0].enabled = 1U;
	vertex.streams[0].address = execute->va + EXECUTE_CORNERS_AT;
	vertex.streams[0].bytes = EXECUTE_CORNERS * EXECUTE_CORNER_STRIDE;
	vertex.streams[0].stride = EXECUTE_CORNER_STRIDE;
	execute->at = nvrtx_exec_vertex(push, execute->at, &vertex);
	execute->at = nvrtx_push_draw_constants(push, execute->at, constants_va, EXECUTE_CONSTANTS_SIZE);
	execute->at = nvrtx_push_draw_pipeline(push, execute->at, execute->quad_vertex, execute->quad_vertex_gprs,
					       execute->quad_fragment, execute->quad_fragment_gprs);
	execute->at = nvrtx_exec_draw(push, execute->at, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0U, EXECUTE_CORNERS, 1U, 0U);
	execute->quads++;

	/* Succeeded: the quad is written. */
	return 0;
}

/*
 * Describes a level of an image as a colour target in a format, with no
 * depth target: the level's surface of a layer (a 3D image's slice); EIO
 * without storage, EINVAL for a format the executor does not render to or
 * a level or layer the image does not have.
 */
static int
execute_image_target(
	const struct nvrtx_vk_image *image,
	uint32_t level,
	uint32_t layer,
	uint32_t format,
	struct nvrtx_exec_target *target)
{
	uint64_t va;
	uint64_t offset;
	uint8_t *cpu;
	uint32_t code;
	bool renderable;
	int error;

	/* The image's storage. */
	kern_memset(target, 0, sizeof(*target));
	error = nvrtx_vk_image_address(image, &va, &cpu);
	if (error != 0)
		return error;

	/* A format rendered to, of a colour image. */
	renderable = nvrtx_exec_target_format(format, &code);
	if (!renderable || image->depth)
		return EINVAL;

	/* The level's surface of the layer. */
	error = nvrtx_vk_image_surface(image, level, layer, &offset, &target->width, &target->height, &target->pitch);
	if (error != 0)
		return error;

	/* Succeeded: the target is the image's surface, of the image's samples. */
	target->address = va + offset;
	target->format = format;
	target->samples = image->samples;
	target->block_linear = 0U;
	if (image->tiled)
		target->block_linear = 1U;
	return 0;
}

/*
 * Puts the surface of a level and layer of a depth image beside a colour
 * target as its depth target; an image of a depth
 * kind has its pages given the kind the first time (the 3D engine refuses
 * a depth target of such a format in pages of another kind).  EIO without
 * storage, EINVAL for a colour image or a level or layer the image does
 * not have, or the kind's error.
 */
static int
execute_depth_target(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_image *image,
	uint32_t level,
	uint32_t layer,
	struct nvrtx_exec_target *target)
{
	const struct nvrtx_vk_memory *memory;
	uint64_t va;
	uint64_t offset;
	uint32_t width;
	uint8_t *cpu;
	int error;

	/* A depth image with storage. */
	if (!image->depth)
		return EINVAL;
	error = nvrtx_vk_image_address(image, &va, &cpu);
	if (error != 0)
		return error;

	/* The level's surface of the layer. */
	error = nvrtx_vk_image_surface(image, level, layer, &offset, &width, &target->depth_height, &target->depth_pitch);
	if (error != 0)
		return error;

	/* The pages of the format's kind, once. */
	if (image->kind != 0U && !image->kinded) {
		memory = image->memory->payload;
		error = nvrtx_vk_memory_kind(session->owner, memory->backing, image->offset, va, image->bytes, image->kind);
		if (error != 0)
			return error;
		image->kinded = true;
	}

	/* Succeeded: the target has its depth target, and its stencil when the format has one. */
	target->depth_address = va + offset;
	target->depth_format = image->format;
	target->depth_stencil = 0U;
	if (image->stencil)
		target->depth_stencil = 1U;

	/* A pass of no colour target is the depth target's size and samples. */
	if (target->address == 0U) {
		target->width = width;
		target->height = target->depth_height;
		target->samples = image->samples;
	}
	return 0;
}

/*
 * Describes a level of an image as a quad's source, the level's surface of
 * a layer (a 3D image's slice); EIO without storage, EINVAL for a depth
 * image or a level or layer the image does not have.
 */
static int
execute_image_source(
	const struct nvrtx_vk_image *image,
	uint32_t level,
	uint32_t layer,
	struct execute_quad *quad)
{
	uint64_t va;
	uint64_t offset;
	uint8_t *cpu;
	int error;

	/* A colour image of one sample with storage (a depth, video or multisampled image is no quad's source). */
	if (image->depth || image->video || image->samples > 1U)
		return EINVAL;
	error = nvrtx_vk_image_address(image, &va, &cpu);
	if (error != 0)
		return error;

	/* The level's surface of the layer. */
	error = nvrtx_vk_image_surface(image, level, layer, &offset, &quad->source_width, &quad->source_height, &quad->source_pitch);
	if (error != 0)
		return error;

	/* Succeeded: the source is the image's surface. */
	quad->source = va + offset;
	quad->source_format = image->format;
	quad->source_tiled = image->tiled;
	return 0;
}

/*
 * Reports the format a copy one to one draws texels of a size through and
 * how many of its pixels a texel is (EXECUTE_RAW_BYTES).
 */
static uint32_t
execute_raw_format(
	uint32_t bytes,
	uint32_t *factor)
{
	/* One and two bytes: a pixel of one or two 8-bit channels. */
	*factor = 1U;
	if (bytes == 1U)
		return VK_FORMAT_R8_UNORM;
	if (bytes == 2U)
		return VK_FORMAT_R8G8_UNORM;

	/* Four bytes or more: as many four-channel 8-bit pixels. */
	if (bytes > EXECUTE_RAW_BYTES)
		*factor = bytes / EXECUTE_RAW_BYTES;

	/* Succeeded: four 8-bit channels. */
	return VK_FORMAT_R8G8B8A8_UNORM;
}

/*
 * Reports the format a copy one to one takes an image's surface as a
 * target in: its own, or for a colour format that is not rendered to (a
 * shared exponent one) the raw format of its size (the copy's quad then
 * draws in the raw format anyway).
 */
static uint32_t
execute_copy_format(
	uint32_t format)
{
	uint32_t factor;
	uint32_t code;
	bool rendered;

	/* A format rendered to, or one of no colour format of the executor's, as it is. */
	rendered = nvrtx_exec_target_format(format, &code);
	if (rendered || !nvrtx_vk_format_colour(format))
		return format;

	/* Succeeded: the raw format of the size. */
	return execute_raw_format(nvrtx_vk_format_bytes(format), &factor);
}

/*
 * Makes a quad copying texels of a size one to one copy them bit for bit:
 * its target and source in the raw format of the size, their widths and
 * the region's columns in its pixels.
 */
static void
execute_raw_quad(
	struct execute_quad *quad,
	uint32_t bytes)
{
	uint32_t factor;
	uint32_t format;

	/* The raw format, and the pixels of a texel. */
	format = execute_raw_format(bytes, &factor);

	/* Both sides in it, the columns in its pixels. */
	quad->target.format = format;
	quad->target.width *= factor;
	quad->source_format = format;
	quad->source_width *= factor;
	quad->x0 *= (int32_t)factor;
	quad->x1 *= (int32_t)factor;
	quad->u0 *= (int32_t)factor;
	quad->u1 *= (int32_t)factor;
}

/* Reports whether a region lies inside an image of a size. */
static bool
execute_region_inside(
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height,
	uint32_t image_width,
	uint32_t image_height)
{
	/* On the image, not past its edges. */
	if (x < 0 || y < 0)
		return false;
	if ((uint32_t)x > image_width || width > image_width - (uint32_t)x)
		return false;
	if ((uint32_t)y > image_height || height > image_height - (uint32_t)y)
		return false;

	/* Succeeded: the region is inside. */
	return true;
}

/* Fills a VkViewport's floats (as bits) for a whole-pixel rectangle, depths 0 to 1. */
static void
execute_full_viewport(
	uint32_t *viewport,
	int32_t x0,
	int32_t y0,
	int32_t x1,
	int32_t y1)
{
	/* x, y, width, height, min depth, max depth. */
	viewport[0] = nvrtx_exec_float(x0);
	viewport[1] = nvrtx_exec_float(y0);
	viewport[2] = nvrtx_exec_float(x1 - x0);
	viewport[3] = nvrtx_exec_float(y1 - y0);
	viewport[4] = 0U;
	viewport[5] = EXECUTE_FLOAT_ONE;
}

/*
 * Runs a draw or an indirect draw once for each view of a multiview
 * subpass: each with the pipeline's programs for the view (their
 * gl_ViewIndex) into the view's layer of the layered target.  Outside a
 * multiview pass the draw runs once.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (a draw repeated for each bit of the render's view mask, SET_RT_LAYER
 * naming the view's layer).
 */
static int
execute_views(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_execute *execute,
	struct execute_state *state,
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_pipeline *pipeline;
	uint32_t view;
	bool indexed;
	bool indirect;
	int error;

	/* The kind of draw. */
	indexed = false;
	if (record->opcode == GPU_OP_CMD_DRAW_INDEXED)
		indexed = true;
	indirect = false;
	if (record->opcode == GPU_OP_CMD_DRAW_INDIRECT || record->opcode == GPU_OP_CMD_DRAW_INDEXED_INDIRECT)
		indirect = true;

	/* Outside a multiview pass, the draw once. */
	if (state->pass == NULL || state->pass->view_mask == 0U || state->pipeline == NULL) {
		if (indirect) {
			error = execute_draw_indirect(session, execute, state, record);
		} else {
			error = execute_draw(execute, state, indexed, record->values);
		}
		return error;
	}

	/* Each view of the subpass, its programs and its layer, then the pipeline as bound. */
	pipeline = state->pipeline;
	error = 0;
	for (view = 0U; view < NVRTX_VK_VIEWS_MAX && error == 0; view++) {
		/* A view the subpass does not render. */
		if ((state->pass->view_mask & (1U << view)) == 0U)
			continue;

		/* The view's programs and layer. */
		if (pipeline->views[view] != NULL)
			state->pipeline = pipeline->views[view];
		state->view = view;
		if (indirect) {
			error = execute_draw_indirect(session, execute, state, record);
		} else {
			error = execute_draw(execute, state, indexed, record->values);
		}
		state->pipeline = pipeline;
	}

	/* Reports a view that failed. */
	if (error != 0)
		return error;

	/* Succeeded: every view of the subpass is drawn. */
	return 0;
}

/* Returns the layers of a multiview target: up to the subpass's last view. */
static uint32_t
execute_view_layers(
	uint32_t view_mask)
{
	uint32_t layers;
	uint32_t view;

	/* One past the highest view. */
	layers = 1U;
	for (view = 0U; view < NVRTX_VK_VIEWS_MAX; view++) {
		if ((view_mask & (1U << view)) != 0U)
			layers = view + 1U;
	}

	/* Succeeded: the target's layers. */
	return layers;
}

/* Returns whether the pass in progress draws a layer: any layer, or in a multiview pass a layer of one of its views. */
static bool
execute_layer_drawn(
	const struct execute_state *state,
	uint32_t layer)
{
	/* A pass of one view draws each of its layers. */
	if (state->pass == NULL || state->pass->view_mask == 0U)
		return true;

	/* A multiview pass draws its views' layers. */
	if (layer < NVRTX_VK_VIEWS_MAX && (state->pass->view_mask & (1U << layer)) != 0U)
		return true;

	/* Succeeded: a layer no view draws. */
	return false;
}

/*
 * Ends the queries an occlusion query of a multiview subpass takes beyond
 * its own: a query in a subpass of N views uses N consecutive queries, and
 * the views' draws all count in the first, so each of the others is begun
 * and ended at once (zero) and made available.
 */
static int
execute_query_views(
	struct nvrtx_vk_execute *execute,
	const struct execute_state *state,
	const struct nvrtx_vk_query_place *place)
{
	uint8_t *push;
	uint64_t reports;
	uint32_t view;
	uint32_t index;

	/* A query outside a multiview pass takes one query. */
	if (state->pass == NULL || state->pass->view_mask == 0U)
		return 0;

	/* Each further query of the views, from the second view on. */
	push = execute->cpu + EXECUTE_PUSH_AT;
	index = 0U;
	for (view = 0U; view < NVRTX_VK_VIEWS_MAX; view++) {
		/* A view the subpass does not render takes no query. */
		if ((state->pass->view_mask & (1U << view)) == 0U)
			continue;

		/* The first view's query is the one the command named. */
		index++;
		if (index == 1U)
			continue;

		/* Begun and ended with nothing drawn between, then available. */
		reports = place->reports + (uint64_t)(index - 1U) * place->stride;
		execute->at = nvrtx_exec_query_occlusion(push, execute->at, reports, true);
		execute->at = nvrtx_exec_query_occlusion(push, execute->at, reports + EXECUTE_QUERY_REPORT_BYTES, false);
		execute->at = nvrtx_exec_query_available(push, execute->at, place->available + (uint64_t)(index - 1U) * 4U, 1U, true);
	}

	/* Succeeded: every query of the views is available. */
	return 0;
}
