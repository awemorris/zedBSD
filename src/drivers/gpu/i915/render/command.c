/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Command pools, command buffers, recording and vkQueueSubmit (see
 * command.h).
 *
 * Every command is decoded exactly as libvulkan encodes it: the records
 * through the generated codec, the framing around them as read from the
 * library's own senders.  A recording is decoded to its end even when the
 * command buffer it names does not exist, so the stream stays in step.
 */

#include "command.h"
#include "codec.h"
#include "compute.h"
#include "fence.h"
#include "forget.h"
#include "gfx.h"
#include "instance.h"
#include "internal.h"
#include "object.h"
#include "video.h"
#include <kern/kcrt.h>

#include <kern/klog.h>
#include <kern/kmem.h>

#include <libc/vulkan/vulkan_core.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "vulkan-codec.inc"

/*
 * How many operations one command buffer records at most.
 *
 * The list grows as the recording needs it, from the first allocation on;
 * the bound only keeps a runaway recording from taking the kernel's memory
 * (one operation is a few hundred bytes).
 */
#define I915_GFX_MAX_OPS	65536U

/* The most queries one recorded reset clears (six dwords each, within one operation's room). */
#define I915_QUERY_RESET_CHUNK	64U

/* How many operations the list of a command buffer holds when it is first allocated. */
#define I915_GFX_FIRST_OPS	64U

/* How many command buffers one vkAllocateCommandBuffers creates, and one vkQueueSubmit runs. */
#define I915_GFX_MAX_ALLOCATE	16U
#define I915_GFX_MAX_SUBMITTED	32U

/* How many submissions one vkQueueSubmit carries. */
#define I915_GFX_MAX_SUBMITS	8U

/* How many regions one image copy, clear or blit records. */
#define I915_GFX_MAX_REGIONS	16U

/* How many descriptor sets one command buffer binds. */
#define I915_GFX_MAX_SETS	I915_GFX_BOUND_SETS

/* How many dynamic offsets one vkCmdBindDescriptorSets carries. */
#define I915_GFX_MAX_DYNAMIC_OFFSETS	64U

/*
 * The texels of one row of a buffer copy, which runs as a copy between two
 * linear surfaces of four-byte texels.  A power of two keeps the sampled
 * coordinates of every texel exact.
 */
#define I915_GFX_COPY_ROW_TEXELS	4096U

/* How many rows one rectangle of a buffer copy covers at most. */
#define I915_GFX_COPY_MAX_ROWS		4096U

/*
 * The planes of an image a copy or a blit moves, as bits: the colour or
 * depth texels, and the separate stencil plane of a format with stencil.
 */
#define I915_IMAGE_PLANE_MAIN		1U
#define I915_IMAGE_PLANE_STENCIL	2U

struct i915_gfx_cmdpool;
struct i915_vk_fence;

/*
 * One VkCommandBuffer: its recorded operation list.
 *
 * It belongs to its pool from vkAllocateCommandBuffers to vkFreeCommandBuffers
 * or the pool's destruction, and is published in the object table under its
 * identity for that whole time.  Begin and a pool reset empty the list but
 * keep its storage, which is freed with the buffer.
 */
struct i915_gfx_cmdbuf {
	/* The next buffer of the same pool. */
	struct i915_gfx_cmdbuf *next;

	/* The pool the buffer was allocated from. */
	struct i915_gfx_cmdpool *pool;

	/* The wire identity the buffer is published under. */
	uint64_t identity;

	/* How many operations are recorded. */
	uint32_t op_count;

	/* How many operations the list has room for; zero until the first operation. */
	uint32_t op_capacity;

	/*
	 * Nonzero once an operation did not fit: the list reached its bound or
	 * could not grow.  vkEndCommandBuffer then fails, and a begin or a reset
	 * clears it.
	 */
	int overflow;

	/*
	 * Nonzero once an object an operation named was destroyed before the
	 * buffer (BUG-260): the operations are gone, and a submission does not
	 * run the buffer.  A begin or a reset clears it.
	 */
	int stale;

	/* The recorded operations, in recording order; NULL until the first operation. */
	struct i915_gfx_op *ops;
};

/*
 * One VkCommandPool: the buffers allocated from it.
 *
 * It is published in the object table from its creation to its destruction,
 * which frees every buffer still on the list.
 */
struct i915_gfx_cmdpool {
	/* The pool's buffers, newest first. */
	struct i915_gfx_cmdbuf *buffers;
};

/*
 * The operation a recording writes into when it has nowhere else to go.
 *
 * A recording for a command buffer that does not exist, or one that is full,
 * still has to be decoded to its end; its operation is written here and
 * discarded.  It is cleared each time it is handed out and read by nobody.
 * XXX: one operation is shared by every session; it is only ever written.
 */
static struct i915_gfx_op i915_command_discard_op;

static uint32_t i915_command_result(int error);
static int i915_command_pool_create(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void i915_command_buffer_release(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf);
static int i915_command_pool_destroy(struct i915_render_session *session, struct i915_wire_reader *reader);
static int i915_command_pool_reset(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_command_buffer_reset(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_command_buffers_allocate(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_command_buffers_free(struct i915_render_session *session, struct i915_wire_reader *reader);
static int i915_command_buffer_begin(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_command_buffer_end(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static struct i915_gfx_op *i915_command_op(struct i915_gfx_cmdbuf *cmdbuf, enum i915_gfx_op_kind kind);
static void i915_command_ops_clear(struct i915_gfx_cmdbuf *cmdbuf);
static int i915_record_video(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, uint32_t opcode, struct i915_wire_reader *reader);
static uint32_t i915_command_op_mark(const struct i915_gfx_cmdbuf *cmdbuf);
static void i915_command_forget_one(void *object, void *argument);

/*
 * An object being destroyed and how many command buffers lost their
 * operations for it (BUG-260), for one walk of the session's command
 * buffers.  It lives on the stack of the destroy.
 */
struct i915_command_forget {
	const void *object;
	unsigned count;
};
static struct i915_gfx_op *i915_command_op_at(struct i915_gfx_cmdbuf *cmdbuf, uint32_t place);
static int i915_command_grow(struct i915_gfx_cmdbuf *cmdbuf);
static int i915_record_image_copy(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader, enum i915_gfx_op_kind kind);
static int i915_record_clear_image(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader);
static int i915_record_barrier(struct i915_render_session *session, struct i915_wire_reader *reader);
static int i915_record_buffer_image_copy(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader, int to_image);
static int i915_record_query(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader, uint32_t opcode);
static int i915_record_begin_pass(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader);
static int i915_record_bind_vertex(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader);
static int i915_record_bind_descriptor_sets(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader);
static void i915_command_unknown_set(uint32_t set, uint64_t identity);
static int i915_record_dynamic_offsets(struct i915_gfx_cmdbuf *cmdbuf, uint32_t first_op, uint32_t set_count, const uint32_t *offsets, uint32_t offset_count);
static int i915_record_set_blend_constants(struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader);
static int i915_record_push_constants(struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader);
static int i915_record_set_unused(uint32_t opcode, struct i915_wire_reader *reader);
static int i915_record_bind_index(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader);
static int i915_record_set_viewport(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader);
static int i915_record_set_scissor(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader);
static int i915_record_copy_buffer(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader);
static int i915_record_clear_attachments(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader);
static int i915_record_command(struct i915_render_session *session, uint32_t opcode, struct i915_wire_reader *reader);
static int i915_image_surface(const struct i915_gfx_image *image, uint32_t level, uint32_t slice, struct i915_gfx_surface *surface);
static uint32_t i915_image_planes(const struct i915_gfx_image *image, uint32_t aspects);
static int i915_image_plane_surface(const struct i915_gfx_image *image, uint32_t plane, uint32_t level, uint32_t slice, struct i915_gfx_surface *surface);
static int i915_nv12_copy_surface(const struct i915_gfx_op *op, struct i915_gfx_surface *surface, uint32_t *texel_bytes);
static int i915_image_copy_plane(struct i915_render_session *session, const struct i915_gfx_op *op, uint32_t plane);
static int i915_image_blit_plane(struct i915_render_session *session, const struct i915_gfx_op *op, uint32_t plane, const struct i915_gfx_rect *src_rect, const struct i915_gfx_rect *dst_rect, int linear);
static int i915_blit_order(int32_t *first, int32_t *second, int mirror);
static int i915_attachment_surface(const struct i915_gfx_view *view, struct i915_gfx_surface *surface);
static void i915_depth_words_surface(const struct i915_gfx_image *image, struct i915_gfx_surface *surface);
static int i915_clear_stencil(struct i915_render_session *session, const struct i915_gfx_view *view, const struct i915_gfx_rect *area, uint32_t value);
static uint32_t i915_unorm8_float_bits(uint32_t value);
static int i915_record_set_stencil(struct i915_gfx_cmdbuf *cmdbuf, struct i915_wire_reader *reader, uint32_t which);
static void i915_execute_set_stencil(struct i915_gfx_draw_state *state, const struct i915_gfx_op *op);
static int i915_execute_clear(struct i915_render_session *session, const struct i915_gfx_op *op);
static int i915_execute_clear_layer(struct i915_render_session *session, const struct i915_gfx_op *op, uint32_t index, const struct i915_gfx_view *view);
static int i915_execute_clear_rect(struct i915_render_session *session, const struct i915_gfx_op *op, const struct i915_gfx_view *view);
static int i915_execute_buffer_image_copy(struct i915_render_session *session, const struct i915_gfx_op *op);
static int i915_execute_clear_image(struct i915_render_session *session, const struct i915_gfx_op *op);
static int i915_execute_clear_attachment(struct i915_render_session *session, const struct i915_gfx_draw_state *state, const struct i915_gfx_op *op);
static int i915_execute_image_copy(struct i915_render_session *session, const struct i915_gfx_op *op);
static int i915_execute_image_blit(struct i915_render_session *session, const struct i915_gfx_op *op);
static int i915_execute_image_resolve(struct i915_render_session *session, const struct i915_gfx_op *op);
static int i915_fill_samples(struct i915_render_session *session, const struct i915_gfx_image *image, const struct i915_gfx_surface *surface, const struct i915_gfx_rect *rect, const uint32_t words[4]);
static void i915_sample_rect(const struct i915_gfx_image *image, struct i915_gfx_rect *rect);
static int i915_execute_buffer_copy(struct i915_render_session *session, const struct i915_gfx_op *op);
static int i915_execute_draw(struct i915_render_session *session, const struct i915_gfx_draw_state *state, const struct i915_gfx_op *op);
static void i915_execute_bind_pipeline(struct i915_gfx_draw_state *state, const struct i915_gfx_op *op);
static void i915_execute_bind_set(struct i915_gfx_draw_state *state, const struct i915_gfx_op *op);
static int i915_execute_dispatch(struct i915_render_session *session, const struct i915_gfx_draw_state *state, const struct i915_gfx_op *op);
static int i915_execute_dispatch_indirect(struct i915_render_session *session, const struct i915_gfx_draw_state *state, const struct i915_gfx_op *op);
static int i915_dispatch_ready(const struct i915_gfx_draw_state *state);
static int i915_command_buffer_execute(struct i915_render_session *session, struct i915_gfx_cmdbuf *cmdbuf);
static int i915_queue_submit(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

/*
 * Routes a command pool, command buffer, recording or queue submission
 * command.
 *
 * `handled` is cleared for an opcode the module does not own; every opcode
 * from 93 to 136 is a vkCmd* and is owned here (92 is vkResetCommandBuffer).  A recording opcode the
 * module does not implement is refused with ENOTSUP and fails the stream.
 */
int
drv_i915_gfx_rec_dispatch(
	struct i915_render_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int routed;
	int error;

	/* Routes the commands that are not recordings. */
	*handled = 1;
	routed = 1;
	error = 0;
	switch (opcode) {
	case GPU_OP_QUEUE_SUBMIT:
		/* vkQueueSubmit */
		error = i915_queue_submit(session, reader, reply);
		break;
	case GPU_OP_CREATE_COMMAND_POOL:
		/* vkCreateCommandPool */
		error = i915_command_pool_create(session, reader, reply);
		break;
	case GPU_OP_DESTROY_COMMAND_POOL:
		/* vkDestroyCommandPool */
		error = i915_command_pool_destroy(session, reader);
		break;
	case GPU_OP_RESET_COMMAND_POOL:
		/* vkResetCommandPool */
		error = i915_command_pool_reset(session, reader, reply);
		break;
	case GPU_OP_ALLOCATE_COMMAND_BUFFERS:
		/* vkAllocateCommandBuffers */
		error = i915_command_buffers_allocate(session, reader, reply);
		break;
	case GPU_OP_FREE_COMMAND_BUFFERS:
		/* vkFreeCommandBuffers */
		error = i915_command_buffers_free(session, reader);
		break;
	case GPU_OP_BEGIN_COMMAND_BUFFER:
		/* vkBeginCommandBuffer */
		error = i915_command_buffer_begin(session, reader, reply);
		break;
	case GPU_OP_END_COMMAND_BUFFER:
		/* vkEndCommandBuffer */
		error = i915_command_buffer_end(session, reader, reply);
		break;
	case GPU_OP_RESET_COMMAND_BUFFER:
		/* vkResetCommandBuffer */
		error = i915_command_buffer_reset(session, reader, reply);
		break;
	default:
		/* A recording, or an opcode of another module. */
		routed = 0;
		break;
	}

	/* Reports the outcome of a command that is not a recording. */
	if (routed != 0) {
		/* Reports why the command was refused. */
		if (error != 0)
			return error;

		/* Succeeded: the command was decoded and executed. */
		return 0;
	}

	/*
	 * Leaves an opcode outside the recording ranges to its own module: the
	 * vkCmd* of the protocol, and zedBSD's video coding commands.
	 */
	if ((opcode < GPU_OP_RESET_COMMAND_BUFFER || opcode > GPU_OP_CMD_EXECUTE_COMMANDS) &&
	    (opcode < GPU_OP_CMD_BEGIN_VIDEO_CODING || opcode > GPU_OP_CMD_DECODE_VIDEO)) {
		*handled = 0;
		return 0;
	}

	/* Records the vkCmd*; an unimplemented one fails the stream. */
	error = i915_record_command(session, opcode, reader);
	if (error == ENOTSUP) {
		kern_logf("i915: vk: XXX unimplemented opcode %u (recording)\n", opcode);
		reader->error = 1;
	} else if (error != 0) {
		kern_logf("i915: vk: recording refused at opcode %u: error %d\n", opcode, error);
	}

	/* Reports why the recording was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the operation is recorded. */
	return 0;
}

/*
 * Translates an errno into the VkResult a command buffer or submission
 * reply carries.
 *
 * A GPU that timed out or failed is a lost device; out of memory keeps its
 * meaning; anything else is an initialization failure.
 */
static uint32_t
i915_command_result(
	int error)
{
	/* Success is VK_SUCCESS. */
	if (error == 0)
		return 0U;

	/* An allocation that failed is out of device memory. */
	if (error == ENOMEM)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* A GPU run that timed out or failed lost the device. */
	if (error == ETIMEDOUT || error == EIO)
		return (uint32_t)VK_ERROR_DEVICE_LOST;

	/* Anything else is reported as an initialization failure. */
	return (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
}

/*
 * vkCreateCommandPool: [device][present][VkCommandPoolCreateInfo]
 * [pAllocator][present][identity] -> [result][present][identity].
 */
static int
i915_command_pool_create(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandPoolCreateInfo info;
	struct i915_gfx_cmdpool *pool;
	uint64_t identity;
	uint32_t result;
	int error;

	/* Decodes the create info and the identity; the flags are not acted on. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkCommandPoolCreateInfo(reader, &session->arena, &info);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Allocates the pool and publishes it under its identity. */
	pool = kern_calloc(1U, sizeof(*pool));
	if (pool == NULL) {
		error = ENOMEM;
	} else {
		error = drv_i915_object_insert(session, I915_VK_OBJ_COMMAND_POOL, identity, pool);
		if (error != 0)
			kern_free(pool);
	}

	/* Replies with the result, and the identity on success. */
	result = i915_command_result(error);
	drv_i915_wire_reply_u32(reply, result);
	if (error == 0) {
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, identity);
	}

	/* Succeeded: the command was decoded; the reply carries the create's result. */
	return 0;
}

/* Unlinks a command buffer from its pool, withdraws its identity and frees it. */
static void
i915_command_buffer_release(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf)
{
	struct i915_gfx_cmdbuf **link;

	/* Finds the buffer's link in its pool and takes the buffer out of the list. */
	for (link = &cmdbuf->pool->buffers;
	     *link != NULL;
	     link = &(*link)->next) {
		if (*link == cmdbuf) {
			*link = cmdbuf->next;
			break;
		}
	}

	/* Withdraws the identity, then frees the operations' records, the operation list and the buffer. */
	drv_i915_object_remove(session, I915_VK_OBJ_COMMAND_BUFFER, cmdbuf->identity);
	i915_command_ops_clear(cmdbuf);
	kern_free(cmdbuf->ops);
	kern_free(cmdbuf);
}

/* vkDestroyCommandPool: [device][pool][pAllocator]; the pool's buffers go with it. */
static int
i915_command_pool_destroy(
	struct i915_render_session *session,
	struct i915_wire_reader *reader)
{
	struct i915_gfx_cmdpool *pool;
	uint64_t identity;

	/* Decodes the pool's identity. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An unknown pool has nothing to destroy. */
	pool = drv_i915_object_lookup(session, I915_VK_OBJ_COMMAND_POOL, identity);
	if (pool == NULL)
		return 0;

	/* Withdraws the pool's identity, then frees it with its buffers. */
	drv_i915_object_remove(session, I915_VK_OBJ_COMMAND_POOL, identity);
	drv_i915_gfx_command_pool_free(session, pool);

	/* Succeeded: the pool and its buffers are gone. */
	return 0;
}

/*
 * Empties every command buffer of the session that recorded an object
 * being destroyed and marks it stale, so that none of its operations
 * names the freed object and a submission does not run it (BUG-260).
 * Reports how many buffers were emptied.
 */
unsigned
drv_i915_gfx_command_forget(
	struct i915_render_session *session,
	const void *object)
{
	struct i915_command_forget forget;

	/* Each buffer of the session, under the table's lock. */
	forget.object = object;
	forget.count = 0U;
	drv_i915_object_each(session, I915_VK_OBJ_COMMAND_BUFFER, i915_command_forget_one, &forget);

	/* Succeeded: how many buffers lost their operations. */
	return forget.count;
}

/*
 * Frees a command pool whose identity is already withdrawn, with every
 * buffer still allocated from it (their identities are withdrawn here).
 */
void
drv_i915_gfx_command_pool_free(
	struct i915_render_session *session,
	struct i915_gfx_cmdpool *pool)
{
	/* Frees every buffer still allocated from the pool. */
	while (pool->buffers != NULL)
		i915_command_buffer_release(session, pool->buffers);

	/* Frees the pool itself. */
	kern_free(pool);
}

/* vkResetCommandPool: [device][pool][flags] -> [result]; every buffer of the pool is emptied. */
static int
i915_command_pool_reset(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_gfx_cmdpool *pool;
	struct i915_gfx_cmdbuf *cmdbuf;
	uint64_t identity;
	uint32_t result;

	/* Decodes the pool's identity; the flags are not acted on. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Empties the recording of every buffer of the pool. */
	pool = drv_i915_object_lookup(session, I915_VK_OBJ_COMMAND_POOL, identity);
	cmdbuf = NULL;
	if (pool != NULL)
		cmdbuf = pool->buffers;
	for (;
	     cmdbuf != NULL;
	     cmdbuf = cmdbuf->next) {
		i915_command_ops_clear(cmdbuf);
		cmdbuf->overflow = 0;
		cmdbuf->stale = 0;
	}

	/* Replies success for a known pool and failure for an unknown one. */
	result = 0U;
	if (pool == NULL)
		result = i915_command_result(EINVAL);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the command was decoded; the reply carries the reset's result. */
	return 0;
}

/*
 * vkAllocateCommandBuffers: [device][present][VkCommandBufferAllocateInfo]
 * [count][identities] -> [result][count][identities].
 *
 * Only primary command buffers are implemented.
 * XXX: an allocation that fails part-way keeps its earlier buffers.
 */
static int
i915_command_buffers_allocate(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandBufferAllocateInfo info;
	struct i915_gfx_cmdpool *pool;
	struct i915_gfx_cmdbuf *cmdbuf;
	uint64_t identities[I915_GFX_MAX_ALLOCATE];
	uint64_t count;
	uint64_t index;
	uint32_t result;
	int error;

	/* Decodes the allocate info and the number of buffers, at most sixteen. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkCommandBufferAllocateInfo(reader, &session->arena, &info);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > I915_GFX_MAX_ALLOCATE)
		return EINVAL;

	/* Decodes the identity of every buffer. */
	for (index = 0U; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Finds the pool, and refuses secondary command buffers. */
	pool = drv_i915_object_lookup(session, I915_VK_OBJ_COMMAND_POOL, (uint64_t)(uintptr_t)info.commandPool);
	error = 0;
	if (pool == NULL) {
		error = EINVAL;
	} else if (info.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
		kern_logf("i915: vk: XXX secondary command buffers are not implemented\n");
		error = ENOTSUP;
	}

	/* Allocates, publishes and links every buffer, stopping at the first failure. */
	for (index = 0U; index < count && error == 0; index++) {
		/* Allocates the buffer. */
		cmdbuf = kern_calloc(1U, sizeof(*cmdbuf));
		if (cmdbuf == NULL) {
			error = ENOMEM;
			break;
		}

		/* Publishes it under its identity. */
		cmdbuf->pool = pool;
		cmdbuf->identity = identities[index];
		error = drv_i915_object_insert(session, I915_VK_OBJ_COMMAND_BUFFER, identities[index], cmdbuf);
		if (error != 0) {
			kern_free(cmdbuf);
			break;
		}

		/* Links it into the pool. */
		cmdbuf->next = pool->buffers;
		pool->buffers = cmdbuf;
	}

	/* Replies with the result, and the identities on success. */
	result = i915_command_result(error);
	drv_i915_wire_reply_u32(reply, result);
	if (error == 0) {
		drv_i915_wire_reply_u64(reply, count);
		for (index = 0U; index < count; index++)
			drv_i915_wire_reply_u64(reply, identities[index]);
	}

	/* Succeeded: the command was decoded; the reply carries the allocation's result. */
	return 0;
}

/* vkFreeCommandBuffers: [device][pool][present][count][identities]. */
static int
i915_command_buffers_free(
	struct i915_render_session *session,
	struct i915_wire_reader *reader)
{
	struct i915_gfx_cmdbuf *cmdbuf;
	uint64_t identity;
	uint64_t count;
	uint64_t index;

	/* Decodes the number of buffers, at most sixty-four. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > 64U)
		return EINVAL;

	/* Frees every named buffer that exists. */
	for (index = 0U; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		cmdbuf = drv_i915_object_lookup(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
		if (reader->error == 0 && cmdbuf != NULL)
			i915_command_buffer_release(session, cmdbuf);
	}

	/* Refuses a stream that ended inside the identities. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the named buffers are freed. */
	return 0;
}

/* vkBeginCommandBuffer: [command buffer][present][VkCommandBufferBeginInfo] -> [result]. */
static int
i915_command_buffer_begin(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCommandBufferBeginInfo info;
	struct i915_gfx_cmdbuf *cmdbuf;
	uint64_t identity;
	uint64_t present;
	uint32_t result;

	/* Decodes the buffer and the begin info; the begin info is not acted on. */
	kern_memset(&info, 0, sizeof(info));
	identity = drv_i915_wire_read_u64(reader);
	cmdbuf = drv_i915_object_lookup(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkCommandBufferBeginInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return EINVAL;

	/* Starts the recording afresh. */
	if (cmdbuf != NULL) {
		i915_command_ops_clear(cmdbuf);
		cmdbuf->overflow = 0;
		cmdbuf->stale = 0;
	}

	/* Replies success for a known buffer and failure for an unknown one. */
	result = 0U;
	if (cmdbuf == NULL)
		result = i915_command_result(EINVAL);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the command was decoded; the reply carries the begin's result. */
	return 0;
}

/* vkResetCommandBuffer: [command buffer][flags] -> [result]; the recording is emptied. */
static int
i915_command_buffer_reset(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_gfx_cmdbuf *cmdbuf;
	uint64_t identity;
	uint32_t result;

	/* Decodes the buffer; the flags are not acted on (nothing is held beyond the list). */
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	cmdbuf = drv_i915_object_lookup(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (reader->error != 0)
		return EINVAL;

	/* Empties the recording, as a begin does. */
	if (cmdbuf != NULL) {
		i915_command_ops_clear(cmdbuf);
		cmdbuf->overflow = 0;
		cmdbuf->stale = 0;
	}

	/* Replies success for a known buffer and failure for an unknown one. */
	result = 0U;
	if (cmdbuf == NULL)
		result = i915_command_result(EINVAL);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the command was decoded; the reply carries the reset's result. */
	return 0;
}

/* vkEndCommandBuffer: [command buffer] -> [result]; a recording that overflowed fails. */
static int
i915_command_buffer_end(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_gfx_cmdbuf *cmdbuf;
	uint64_t identity;
	uint32_t result;

	/* Decodes the buffer. */
	identity = drv_i915_wire_read_u64(reader);
	cmdbuf = drv_i915_object_lookup(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (reader->error != 0)
		return EINVAL;

	/* Says why a recording that did not fit is refused. */
	if (cmdbuf != NULL && cmdbuf->overflow != 0) {
		kern_logf("i915: vk: command buffer recording refused: it needs more than %u operations or its operation list could not grow\n",
			  cmdbuf->op_capacity);
	}

	/* Replies success only for a known buffer whose recording fit. */
	result = (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
	if (cmdbuf != NULL && cmdbuf->overflow == 0)
		result = 0U;
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the command was decoded; the reply carries the end's result. */
	return 0;
}

/*
 * Returns the next free operation of a command buffer, zeroed and of the
 * given kind.
 *
 * The list grows when it is full.  A buffer that does not exist, that has
 * already overflowed, or whose list cannot grow gets the discarded operation
 * instead; such a buffer is marked overflowed so that its end fails.
 */
static struct i915_gfx_op *
i915_command_op(
	struct i915_gfx_cmdbuf *cmdbuf,
	enum i915_gfx_op_kind kind)
{
	struct i915_gfx_op *op;
	int error;

	/*
	 * Grows a full list of a recording that still fits.  A list that
	 * cannot grow overflows the recording: every later operation of it is
	 * discarded and its end fails.
	 */
	if (cmdbuf != NULL &&
	    cmdbuf->overflow == 0 &&
	    cmdbuf->op_count >= cmdbuf->op_capacity) {
		error = i915_command_grow(cmdbuf);
		if (error != 0)
			cmdbuf->overflow = 1;
	}

	/* Hands a recording with nowhere to go the discarded operation. */
	if (cmdbuf == NULL || cmdbuf->overflow != 0) {
		kern_memset(&i915_command_discard_op, 0, sizeof(i915_command_discard_op));
		return &i915_command_discard_op;
	}

	/* Takes the next operation of the list. */
	op = &cmdbuf->ops[cmdbuf->op_count];
	cmdbuf->op_count++;

	/* Starts it empty, of the given kind. */
	kern_memset(op, 0, sizeof(*op));
	op->kind = kind;

	/* Succeeded: the operation is the buffer's newest. */
	return op;
}

/* Empties a command buffer's operation list, freeing the video records its operations hold. */
static void
i915_command_ops_clear(
	struct i915_gfx_cmdbuf *cmdbuf)
{
	struct i915_gfx_op *op;
	uint32_t index;

	/* Frees the record of every video operation. */
	for (index = 0U; index < cmdbuf->op_count; index++) {
		op = &cmdbuf->ops[index];
		if (op->kind == I915_GFX_OP_VIDEO_BEGIN ||
		    op->kind == I915_GFX_OP_VIDEO_CONTROL ||
		    op->kind == I915_GFX_OP_VIDEO_DECODE ||
		    op->kind == I915_GFX_OP_VIDEO_END) {
			drv_i915_video_command_free(op->u.video);
			op->u.video = NULL;
		}
	}

	/* The list is empty. */
	cmdbuf->op_count = 0U;
}

/* Empties a command buffer one of whose operations names the object being destroyed (a visit of drv_i915_object_each, BUG-260). */
static void
i915_command_forget_one(
	void *object,
	void *argument)
{
	struct i915_gfx_cmdbuf *cmdbuf;
	struct i915_command_forget *forget;
	uint32_t index;
	int names;

	/* Whether one of its operations names it. */
	cmdbuf = object;
	forget = argument;
	names = 0;
	for (index = 0U; index < cmdbuf->op_count && !names; index++)
		names = drv_i915_gfx_op_names(&cmdbuf->ops[index], forget->object);
	if (!names)
		return;

	/* Its operations go, and it does not run until it is recorded again. */
	i915_command_ops_clear(cmdbuf);
	cmdbuf->stale = 1;
	forget->count++;
}

/*
 * Records a video coding command into an operation holding its record.
 *
 * A record that cannot be made overflows the recording, so its end fails;
 * a recording with nowhere to go drops the record.
 */
static int
i915_record_video(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	uint32_t opcode,
	struct i915_wire_reader *reader)
{
	struct i915_video_command *command;
	enum i915_gfx_op_kind kind;
	struct i915_gfx_op *op;
	int error;

	/* The operation kind of the command. */
	kind = I915_GFX_OP_VIDEO_DECODE;
	if (opcode == GPU_OP_CMD_BEGIN_VIDEO_CODING)
		kind = I915_GFX_OP_VIDEO_BEGIN;
	else if (opcode == GPU_OP_CMD_END_VIDEO_CODING)
		kind = I915_GFX_OP_VIDEO_END;
	else if (opcode == GPU_OP_CMD_CONTROL_VIDEO_CODING)
		kind = I915_GFX_OP_VIDEO_CONTROL;

	/* Decodes the command into its record. */
	error = drv_i915_video_record(session, opcode, reader, &command);
	if (error == ENOMEM) {
		if (cmdbuf != NULL)
			cmdbuf->overflow = 1;
		return 0;
	}
	if (error != 0)
		return error;

	/* Hands the record to the operation; a discarded operation drops it. */
	op = i915_command_op(cmdbuf, kind);
	if (op == &i915_command_discard_op) {
		drv_i915_video_command_free(command);
		kern_memset(&i915_command_discard_op, 0, sizeof(i915_command_discard_op));
		return 0;
	}
	op->u.video = command;

	/* Succeeded: the command is recorded. */
	return 0;
}

/*
 * Returns the place in a command buffer's operation list that the next
 * operation takes (0 for a buffer that does not exist).
 *
 * A recorder that comes back to operations it recorded keeps their places,
 * not pointers: the list may grow, and move, while it records more.
 */
static uint32_t
i915_command_op_mark(
	const struct i915_gfx_cmdbuf *cmdbuf)
{
	/* A missing buffer records nothing. */
	if (cmdbuf == NULL)
		return 0U;

	/* Succeeded: the next operation's place. */
	return cmdbuf->op_count;
}

/*
 * Returns the operation recorded at a place of a command buffer's list, or
 * the discarded operation for a place the list does not hold (an operation
 * discarded when the recording overflowed).
 */
static struct i915_gfx_op *
i915_command_op_at(
	struct i915_gfx_cmdbuf *cmdbuf,
	uint32_t place)
{
	/* A place outside the list was discarded. */
	if (cmdbuf == NULL || place >= cmdbuf->op_count)
		return &i915_command_discard_op;

	/* Succeeded: the operation at the place. */
	return &cmdbuf->ops[place];
}

/*
 * Doubles the room of a command buffer's operation list, keeping what is
 * recorded.
 *
 * Returns ENOSPC when the list already holds the most operations a buffer
 * records, and ENOMEM when the larger list cannot be allocated; the list is
 * then left as it was.
 */
static int
i915_command_grow(
	struct i915_gfx_cmdbuf *cmdbuf)
{
	struct i915_gfx_op *ops;
	uint32_t capacity;

	/* Refuses to grow past the bound of a recording. */
	if (cmdbuf->op_capacity >= I915_GFX_MAX_OPS)
		return ENOSPC;

	/* Starts with a small list and doubles it after that, never past the bound. */
	capacity = I915_GFX_FIRST_OPS;
	if (cmdbuf->op_capacity != 0U)
		capacity = cmdbuf->op_capacity * 2U;
	if (capacity > I915_GFX_MAX_OPS)
		capacity = I915_GFX_MAX_OPS;

	/* Allocates the larger list. */
	ops = kern_malloc((size_t)capacity * sizeof(*ops));
	if (ops == NULL) {
		kern_logf("i915: vk: the operation list of a command buffer cannot grow to %u operations\n", capacity);
		return ENOMEM;
	}

	/* Moves the recorded operations over and frees the old list. */
	if (cmdbuf->op_count != 0U)
		kern_memcpy(ops, cmdbuf->ops, (size_t)cmdbuf->op_count * sizeof(*ops));
	kern_free(cmdbuf->ops);

	/* The buffer records into the larger list from now on. */
	cmdbuf->ops = ops;
	cmdbuf->op_capacity = capacity;

	/* Succeeded: the list has room for more operations. */
	return 0;
}

/*
 * vkCmdCopyImage: [src][layout][dst][layout][present][count]{VkImageCopy};
 * vkCmdBlitImage: the same with VkImageBlit, followed by [filter];
 * vkCmdResolveImage: the same with VkImageResolve, whose record is
 * VkImageCopy's field for field and is kept as one.  `kind` is the
 * operation (I915_GFX_OP_COPY_IMAGE, _BLIT_IMAGE or _RESOLVE_IMAGE).
 *
 * Every region is one operation.
 */
static int
i915_record_image_copy(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader,
	enum i915_gfx_op_kind kind)
{
	struct i915_gfx_image *src;
	struct i915_gfx_image *dst;
	struct i915_gfx_op *op;
	uint64_t identity;
	uint64_t count;
	uint64_t index;
	uint32_t filter;
	uint32_t first_op;
	int blit;

	/* Decodes the two images and the number of regions, at most sixteen. */
	identity = drv_i915_wire_read_u64(reader);
	src = drv_i915_object_lookup(session, I915_VK_OBJ_IMAGE, identity);
	(void)drv_i915_wire_read_u32(reader);
	identity = drv_i915_wire_read_u64(reader);
	dst = drv_i915_object_lookup(session, I915_VK_OBJ_IMAGE, identity);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > I915_GFX_MAX_REGIONS)
		return EINVAL;

	/* A blit has its own region record and a filter; a copy and a resolve share theirs. */
	blit = 0;
	if (kind == I915_GFX_OP_BLIT_IMAGE)
		blit = 1;

	/* Records one operation for each region, the first at first_op. */
	first_op = i915_command_op_mark(cmdbuf);
	for (index = 0U; index < count; index++) {
		op = i915_command_op(cmdbuf, kind);
		if (blit) {
			op->u.blit.src = src;
			op->u.blit.dst = dst;
			i915_vkc_dec_VkImageBlit(reader, &session->arena, &op->u.blit.region);
		} else {
			op->u.image_copy.src = src;
			op->u.image_copy.dst = dst;
			i915_vkc_dec_VkImageCopy(reader, &session->arena, &op->u.image_copy.region);
		}
	}

	/* Gives every region of a blit the filter that follows them, found by its place (the list may have moved). */
	if (blit) {
		filter = drv_i915_wire_read_u32(reader);
		for (index = 0U; index < count; index++) {
			op = i915_command_op_at(cmdbuf, first_op + (uint32_t)index);
			op->u.blit.filter = filter;
		}
	}

	/* Refuses a stream that ended inside the regions. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the regions are recorded. */
	return 0;
}

/*
 * vkCmdClearColorImage: [image][layout][present]{[tag][4][4 words]}
 * [present][count]{VkImageSubresourceRange}.
 *
 * Every range is one operation over its levels.  XXX: array layers are not
 * consulted (every image has one).
 */
static int
i915_record_clear_image(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader)
{
	VkImageSubresourceRange range;
	struct i915_gfx_image *image;
	struct i915_gfx_op *op;
	uint64_t identity;
	uint64_t present;
	uint64_t words;
	uint64_t count;
	uint64_t index;
	uint32_t colour[4];

	/* Decodes the image. */
	identity = drv_i915_wire_read_u64(reader);
	image = drv_i915_object_lookup(session, I915_VK_OBJ_IMAGE, identity);
	(void)drv_i915_wire_read_u32(reader);

	/* Decodes the four words of the clear colour: the union's tag, then an array of four. */
	kern_memset(colour, 0, sizeof(colour));
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		(void)drv_i915_wire_read_u32(reader);
		words = drv_i915_wire_read_u64(reader);
		if (words != 4U)
			reader->error = 1;
		for (index = 0U; index < 4U; index++)
			colour[index] = drv_i915_wire_read_u32(reader);
	}

	/* Decodes the number of ranges, at most sixteen. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > I915_GFX_MAX_REGIONS)
		return EINVAL;

	/* Records one clear for each range, with the levels it names. */
	for (index = 0U; index < count; index++) {
		/* A range the stream ends inside of reads as zeros; the recording is then refused below. */
		kern_memset(&range, 0, sizeof(range));
		i915_vkc_dec_VkImageSubresourceRange(reader, &session->arena, &range);
		op = i915_command_op(cmdbuf, I915_GFX_OP_CLEAR_IMAGE);
		op->u.clear_image.image = image;
		kern_memcpy(op->u.clear_image.words, colour, sizeof(colour));
		op->u.clear_image.base_level = range.baseMipLevel;
		op->u.clear_image.level_count = range.levelCount;
		op->u.clear_image.base_layer = range.baseArrayLayer;
		op->u.clear_image.layer_count = range.layerCount;
	}

	/* Refuses a stream that ended inside the ranges. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the clear is recorded. */
	return 0;
}

/*
 * vkCmdPipelineBarrier: decoded to its end and not acted on.
 *
 * Every operation finishes on the GPU before the next starts (command.h),
 * so there is nothing for a barrier to order.
 */
static int
i915_record_barrier(
	struct i915_render_session *session,
	struct i915_wire_reader *reader)
{
	VkMemoryBarrier memory;
	VkBufferMemoryBarrier buffer;
	VkImageMemoryBarrier image;
	uint64_t count;
	uint64_t index;

	/* Decodes the stage masks and the dependency flags. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* Decodes the memory barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	for (index = 0U; reader->error == 0 && index < count; index++)
		i915_vkc_dec_VkMemoryBarrier(reader, &session->arena, &memory);

	/* Decodes the buffer barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	for (index = 0U; reader->error == 0 && index < count; index++)
		i915_vkc_dec_VkBufferMemoryBarrier(reader, &session->arena, &buffer);

	/* Decodes the image barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	for (index = 0U; reader->error == 0 && index < count; index++)
		i915_vkc_dec_VkImageMemoryBarrier(reader, &session->arena, &image);

	/* Refuses a stream that ended inside the barriers. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the barrier is decoded. */
	return 0;
}

/*
 * vkCmdCopyBufferToImage: [buffer][image][layout][present][count]{region};
 * vkCmdCopyImageToBuffer: [image][layout][buffer][present][count]{region}.
 *
 * Every region is one operation.
 */
static int
i915_record_buffer_image_copy(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader,
	int to_image)
{
	struct i915_gfx_buffer *buffer;
	struct i915_gfx_image *image;
	struct i915_gfx_op *op;
	uint64_t buffer_id;
	uint64_t image_id;
	uint64_t count;
	uint64_t index;
	enum i915_gfx_op_kind kind;

	/* Decodes the buffer and the image in the order of the direction. */
	if (to_image != 0) {
		buffer_id = drv_i915_wire_read_u64(reader);
		image_id = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
	} else {
		image_id = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		buffer_id = drv_i915_wire_read_u64(reader);
	}

	/* Decodes the number of regions, at most as many as a buffer records. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > I915_GFX_MAX_OPS)
		return EINVAL;

	/* Resolves the two objects. */
	buffer = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER, buffer_id);
	image = drv_i915_object_lookup(session, I915_VK_OBJ_IMAGE, image_id);

	/* The direction decides the operation. */
	kind = I915_GFX_OP_COPY_IMAGE_TO_BUFFER;
	if (to_image != 0)
		kind = I915_GFX_OP_COPY_BUFFER_TO_IMAGE;

	/* Records one operation for each region. */
	for (index = 0U; index < count; index++) {
		op = i915_command_op(cmdbuf, kind);
		op->u.copy.buffer = buffer;
		op->u.copy.image = image;
		i915_vkc_dec_VkBufferImageCopy(reader, &session->arena, &op->u.copy.region);
	}

	/* Refuses a stream that ended inside the regions. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the regions are recorded. */
	return 0;
}

/*
 * vkCmdSetStencilCompareMask, WriteMask and Reference: [faceMask][value];
 * `which` is 0, 1 or 2 in that order.
 */
static int
i915_record_set_stencil(
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader,
	uint32_t which)
{
	struct i915_gfx_op *op;
	uint32_t faces;
	uint32_t value;

	/* Decodes the faces and the value. */
	faces = drv_i915_wire_read_u32(reader);
	value = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Records them. */
	op = i915_command_op(cmdbuf, I915_GFX_OP_SET_STENCIL);
	op->u.stencil.which = which;
	op->u.stencil.faces = faces;
	op->u.stencil.value = value;

	/* Succeeded: the value is recorded. */
	return 0;
}

/*
 * vkCmdBeginQuery: [pool][query][flags]; vkCmdEndQuery: [pool][query];
 * vkCmdResetQueryPool: [pool][first][count].
 *
 * A begin and an end are one operation each; a reset is one operation for
 * every I915_QUERY_RESET_CHUNK queries.  The control flags (precise) are
 * not acted on: the count is exact.
 */
static int
i915_record_query(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader,
	uint32_t opcode)
{
	struct i915_gfx_query_pool *pool;
	struct i915_gfx_op *op;
	uint64_t identity;
	uint32_t first;
	uint32_t count;
	uint32_t chunk;

	/* Decodes the pool and the query, or the range of a reset. */
	identity = drv_i915_wire_read_u64(reader);
	first = drv_i915_wire_read_u32(reader);
	count = 1U;
	if (opcode == GPU_OP_CMD_BEGIN_QUERY || opcode == GPU_OP_CMD_RESET_QUERY_POOL)
		count = drv_i915_wire_read_u32(reader);
	if (opcode == GPU_OP_CMD_BEGIN_QUERY)
		count = 1U;
	if (reader->error != 0)
		return EINVAL;

	/* Refuses an unknown pool. */
	pool = drv_i915_object_lookup(session, I915_VK_OBJ_QUERY_POOL, identity);
	if (pool == NULL)
		return EINVAL;

	/* A begin or an end. */
	if (opcode != GPU_OP_CMD_RESET_QUERY_POOL) {
		op = i915_command_op(cmdbuf, I915_GFX_OP_QUERY_BEGIN);
		if (opcode == GPU_OP_CMD_END_QUERY)
			op->kind = I915_GFX_OP_QUERY_END;
		op->u.query.pool = pool;
		op->u.query.first = first;
		op->u.query.count = 1U;
		return 0;
	}

	/* A reset, in chunks that fit one operation's room of the batch. */
	while (count != 0U) {
		chunk = count;
		if (chunk > I915_QUERY_RESET_CHUNK)
			chunk = I915_QUERY_RESET_CHUNK;
		op = i915_command_op(cmdbuf, I915_GFX_OP_QUERY_RESET);
		op->u.query.pool = pool;
		op->u.query.first = first;
		op->u.query.count = chunk;
		first += chunk;
		count -= chunk;
	}

	/* Succeeded: the query commands are recorded. */
	return 0;
}

/*
 * vkCmdBeginRenderPass (libvulkan commands.c command_encode_render_begin):
 * [present][sType][pNext][renderPass][framebuffer][VkRect2D][present][count]
 * {clear}[contents].  A clear is [0][tag][4][4 words] for a colour and
 * [1][depth bits][stencil] for a depth / stencil attachment.
 *
 * XXX: the whole attachment is cleared, not the render area.
 */
static int
i915_record_begin_pass(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader)
{
	VkRect2D area;
	struct i915_gfx_op *op;
	uint64_t identity;
	uint64_t present;
	uint64_t length;
	uint64_t count;
	uint64_t index;
	uint32_t words[4];
	uint32_t is_depth;
	uint32_t word;

	/* Refuses a begin without its begin info. */
	op = i915_command_op(cmdbuf, I915_GFX_OP_BEGIN_PASS);
	present = drv_i915_wire_read_u64(reader);
	if (present == 0U) {
		(void)drv_i915_wire_read_u32(reader);
		return EINVAL;
	}

	/* Records the render pass and the framebuffer; the render area is decoded and not kept. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	op->u.begin.pass = drv_i915_object_lookup(session, I915_VK_OBJ_RENDER_PASS, identity);
	identity = drv_i915_wire_read_u64(reader);
	op->u.begin.framebuffer = drv_i915_object_lookup(session, I915_VK_OBJ_FRAMEBUFFER, identity);
	i915_vkc_dec_VkRect2D(reader, &session->arena, &area);

	/* Decodes the number of clear values, at most sixty-four. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > 64U)
		return EINVAL;

	/* Records the clear value of every attachment the operation has room for. */
	for (index = 0U; index < count; index++) {
		/* Decodes one clear value: a depth and stencil pair, or four colour words. */
		kern_memset(words, 0, sizeof(words));
		is_depth = drv_i915_wire_read_u32(reader);
		if (is_depth != 0U) {
			words[0] = drv_i915_wire_read_u32(reader);
			words[1] = drv_i915_wire_read_u32(reader);
		} else {
			(void)drv_i915_wire_read_u32(reader);
			length = drv_i915_wire_read_u64(reader);
			if (length != 4U)
				reader->error = 1;
			for (word = 0U; word < 4U; word++)
				words[word] = drv_i915_wire_read_u32(reader);
		}

		/* Keeps the value of an attachment the pass can have. */
		if (index < I915_GFX_MAX_ATTACHMENTS) {
			op->u.begin.clear_is_depth[index] = is_depth;
			kern_memcpy(op->u.begin.clear_words[index], words, sizeof(words));
			op->u.begin.clear_count = (uint32_t)index + 1U;
		}
	}

	/* Decodes the subpass contents. */
	(void)drv_i915_wire_read_u32(reader);

	/* Refuses a stream that ended inside the begin. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the begin and its clears are recorded. */
	return 0;
}

/*
 * vkCmdClearAttachments: [count][count]{aspect, colour index, is depth,
 * value}[count][count]{VkClearRect}.
 *
 * Every attachment and rectangle pair is one operation.  The pass has one
 * subpass, so a colour clear names one of the subpass's colour attachments
 * and a depth clear its depth attachment.  The rectangle's layers are
 * kept and cleared one by one.
 */
static int
i915_record_clear_attachments(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader)
{
	struct i915_gfx_op *op;
	VkClearRect rects[I915_GFX_MAX_CLEAR_RECTS];
	uint32_t is_depth[I915_GFX_MAX_ATTACHMENTS];
	uint32_t color_index[I915_GFX_MAX_ATTACHMENTS];
	uint32_t aspects[I915_GFX_MAX_ATTACHMENTS];
	uint32_t words[I915_GFX_MAX_ATTACHMENTS][4];
	uint64_t attachments;
	uint64_t count;
	uint64_t length;
	uint64_t index;
	uint64_t rect;
	uint32_t word;

	/* Decodes the number of attachments. */
	(void)drv_i915_wire_read_u32(reader);
	attachments = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || attachments > I915_GFX_MAX_ATTACHMENTS)
		return EINVAL;

	/* Decodes every attachment's aspects, colour index and value: a depth and stencil pair, or four colour words. */
	for (index = 0U; index < attachments; index++) {
		kern_memset(words[index], 0, sizeof(words[index]));
		aspects[index] = drv_i915_wire_read_u32(reader);
		color_index[index] = drv_i915_wire_read_u32(reader);
		is_depth[index] = drv_i915_wire_read_u32(reader);
		if (is_depth[index] != 0U) {
			words[index][0] = drv_i915_wire_read_u32(reader);
			words[index][1] = drv_i915_wire_read_u32(reader);
		} else {
			(void)drv_i915_wire_read_u32(reader);
			length = drv_i915_wire_read_u64(reader);
			if (length != 4U)
				reader->error = 1;
			for (word = 0U; word < 4U; word++)
				words[index][word] = drv_i915_wire_read_u32(reader);
		}
	}

	/* Decodes the rectangles. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > I915_GFX_MAX_CLEAR_RECTS)
		return EINVAL;
	for (rect = 0U; rect < count; rect++)
		i915_vkc_dec_VkClearRect(reader, &session->arena, &rects[rect]);
	if (reader->error != 0)
		return EINVAL;

	/* Records one clear for every attachment and rectangle. */
	for (index = 0U; index < attachments; index++) {
		for (rect = 0U; rect < count; rect++) {
			op = i915_command_op(cmdbuf, I915_GFX_OP_CLEAR_ATTACHMENT);
			op->u.clear_attachment.is_depth = is_depth[index];
			op->u.clear_attachment.color_index = color_index[index];
			op->u.clear_attachment.aspects = aspects[index];
			kern_memcpy(op->u.clear_attachment.words, words[index], sizeof(words[index]));
			op->u.clear_attachment.rect.x = rects[rect].rect.offset.x;
			op->u.clear_attachment.rect.y = rects[rect].rect.offset.y;
			op->u.clear_attachment.rect.w = rects[rect].rect.extent.width;
			op->u.clear_attachment.rect.h = rects[rect].rect.extent.height;
			op->u.clear_attachment.base_layer = rects[rect].baseArrayLayer;
			op->u.clear_attachment.layer_count = rects[rect].layerCount;
		}
	}

	/* Succeeded: every clear is recorded. */
	return 0;
}

/*
 * vkCmdBindVertexBuffers: [first][present][count]{buffer}[count]{offset}.
 *
 * Every binding is one operation.
 */
static int
i915_record_bind_vertex(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader)
{
	struct i915_gfx_op *op;
	uint64_t identity;
	uint64_t offsets;
	uint64_t count;
	uint64_t index;
	uint32_t first;
	uint32_t first_op;

	/* Decodes the first binding and the number of buffers. */
	first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > I915_GFX_MAX_VERTEX_BINDINGS)
		return EINVAL;

	/* Records the buffer of every binding, the first at first_op. */
	first_op = i915_command_op_mark(cmdbuf);
	for (index = 0U; index < count; index++) {
		op = i915_command_op(cmdbuf, I915_GFX_OP_BIND_VERTEX_BUFFER);
		op->u.vertex.binding = first + (uint32_t)index;
		identity = drv_i915_wire_read_u64(reader);
		op->u.vertex.buffer = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER, identity);
	}

	/* Refuses an offset array of another length. */
	offsets = drv_i915_wire_read_u64(reader);
	if (offsets != count)
		reader->error = 1;

	/* Records the offset of every binding, each found by its place (the list may have moved). */
	for (index = 0U; reader->error == 0 && index < count; index++) {
		op = i915_command_op_at(cmdbuf, first_op + (uint32_t)index);
		op->u.vertex.offset = drv_i915_wire_read_u64(reader);
	}

	/* Refuses a stream that ended inside the bindings. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the bindings are recorded. */
	return 0;
}

/*
 * vkCmdBindDescriptorSets: [bind point][layout][first][present][count]{set}
 * [present][count]{dynamic offset}.
 *
 * Every set is one operation, which keeps the dynamic offsets of its
 * dynamic uniform buffers (see i915_record_dynamic_offsets()).
 */
static int
i915_record_bind_descriptor_sets(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader)
{
	uint32_t offsets[I915_GFX_MAX_DYNAMIC_OFFSETS];
	struct i915_gfx_op *op;
	uint64_t identity;
	uint64_t count;
	uint64_t set_count;
	uint64_t index;
	uint32_t first;
	uint32_t first_op;
	uint32_t bind_point;
	int error;

	/* Decodes the bind point, the layout, the first set and the number of sets, at most four. */
	bind_point = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	set_count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || set_count > I915_GFX_MAX_SETS)
		return EINVAL;

	/* Records every set, the first at first_op. */
	first_op = i915_command_op_mark(cmdbuf);
	for (index = 0U; index < set_count; index++) {
		op = i915_command_op(cmdbuf, I915_GFX_OP_BIND_DESCRIPTOR_SET);
		op->u.descriptor.bind_point = bind_point;
		op->u.descriptor.set = first + (uint32_t)index;
		identity = drv_i915_wire_read_u64(reader);
		op->u.descriptor.dset = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_SET, identity);

		/* Says so when the identity names no set of the session (BUG-117): the draws that sample it are refused. */
		if (op->u.descriptor.dset == NULL)
			i915_command_unknown_set(first + (uint32_t)index, identity);
	}

	/* Decodes the number of dynamic offsets, at most sixty-four. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > I915_GFX_MAX_DYNAMIC_OFFSETS)
		return EINVAL;

	/* Decodes the dynamic offsets. */
	for (index = 0U; index < count; index++)
		offsets[index] = drv_i915_wire_read_u32(reader);

	/* Refuses a stream that ended inside the offsets. */
	if (reader->error != 0)
		return EINVAL;

	/* Hands the offsets to the dynamic uniform buffers of the sets. */
	error = i915_record_dynamic_offsets(cmdbuf, first_op, (uint32_t)set_count, offsets, (uint32_t)count);
	if (error != 0)
		return error;

	/* Succeeded: the sets and their dynamic offsets are recorded. */
	return 0;
}


/* Logs a bind of an identity that names no descriptor set of the session, the first 16 of them (BUG-117). */
static void
i915_command_unknown_set(
	uint32_t set,
	uint64_t identity)
{
	static unsigned said;

	/* Only the first ones. */
	if (said >= 16U)
		return;
	said++;
	kern_logf("i915: vk: vkCmdBindDescriptorSets: set %u is 0x%llx, not a descriptor set of the session\n",
		  set,
		  (unsigned long long)identity);
}
/*
 * Gives each dynamic uniform or storage buffer of the bound sets its dynamic
 * offset.
 *
 * Vulkan takes the offsets in the order of the sets, and within a set in the
 * order of the binding numbers, uniform and storage buffers alike.  The sets'
 * operations are the set_count ones from place first_op of the command
 * buffer's list.  Returns EINVAL when the offsets and the dynamic buffers do
 * not pair up.  XXX: a binding holds one descriptor, so an array of dynamic
 * buffers takes one offset.
 */
static int
i915_record_dynamic_offsets(
	struct i915_gfx_cmdbuf *cmdbuf,
	uint32_t first_op,
	uint32_t set_count,
	const uint32_t *offsets,
	uint32_t offset_count)
{
	const struct i915_gfx_dsl *layout;
	struct i915_gfx_op *op;
	uint32_t set;
	uint32_t binding;
	uint32_t entry;
	uint32_t next;
	uint32_t count;
	uint32_t type;

	/* Walks the sets in order and each set's bindings by number. */
	next = 0U;
	for (set = 0U; set < set_count; set++) {
		/* A set that is unknown or has no layout has no dynamic buffers. */
		op = i915_command_op_at(cmdbuf, first_op + set);
		layout = NULL;
		if (op->u.descriptor.dset != NULL)
			layout = op->u.descriptor.dset->layout;
		if (layout == NULL)
			continue;

		/* Gives each dynamic buffer, lowest binding number first, the next offset. */
		for (binding = 0U; binding < I915_GFX_MAX_BINDINGS; binding++) {
			for (entry = 0U; entry < layout->count; entry++) {
				/* Only the layout's dynamic uniform or storage buffer of this number takes an offset. */
				if (layout->bindings[entry].binding != binding)
					continue;
				type = layout->bindings[entry].type;
				if (type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
				    type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
					continue;

				/* Refuses a bind with fewer offsets than dynamic buffers. */
				if (next >= offset_count) {
					kern_logf("i915: vk: vkCmdBindDescriptorSets: %u dynamic offsets for more dynamic buffers\n", offset_count);
					return EINVAL;
				}

				/* Refuses more dynamic buffers in one set than a bind keeps. */
				count = op->u.descriptor.dynamic_count;
				if (count >= I915_GFX_MAX_DYNAMIC_BUFFERS) {
					kern_logf("i915: vk: XXX vkCmdBindDescriptorSets: more than %u dynamic buffers in one set\n",
						  I915_GFX_MAX_DYNAMIC_BUFFERS);
					return ENOTSUP;
				}

				/* Keeps the binding and its offset. */
				op->u.descriptor.dynamic_bindings[count] = binding;
				op->u.descriptor.dynamic_offsets[count] = offsets[next];
				op->u.descriptor.dynamic_count = count + 1U;
				next++;
			}
		}
	}

	/* Refuses a bind with more offsets than dynamic buffers. */
	if (next != offset_count) {
		kern_logf("i915: vk: vkCmdBindDescriptorSets: %u dynamic offsets for %u dynamic buffers\n", offset_count, next);
		return EINVAL;
	}

	/* Succeeded: every dynamic buffer has its offset. */
	return 0;
}

/*
 * vkCmdSetBlendConstants: [count]{float}, four floats.
 */
static int
i915_record_set_blend_constants(
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader)
{
	struct i915_gfx_op *op;
	uint64_t count;
	uint32_t index;

	/* Decodes the count, which is four. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count != 4U)
		return EINVAL;

	/* Records the four constants as float bits. */
	op = i915_command_op(cmdbuf, I915_GFX_OP_SET_BLEND_CONSTANTS);
	for (index = 0U; index < 4U; index++)
		op->u.blend_constants[index] = drv_i915_wire_read_u32(reader);

	/* Refuses a stream that ended inside the constants. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the blend constants are recorded. */
	return 0;
}

/*
 * vkCmdSetLineWidth: [width]; vkCmdSetDepthBias: [constant][clamp][slope].
 * The values are decoded and dropped (WS068 p006: OpenGL ES sets them with
 * every pipeline).
 *
 * XXX: the draws have no depth bias and only one-pixel lines; a width other
 * than 1 is said once.
 */
static int
i915_record_set_unused(
	uint32_t opcode,
	struct i915_wire_reader *reader)
{
	static int wide_said;
	uint32_t words;
	uint32_t first;
	uint32_t index;

	/* The words each command carries (floats as their bits). */
	words = 2U;
	if (opcode == GPU_OP_CMD_SET_LINE_WIDTH)
		words = 1U;
	if (opcode == GPU_OP_CMD_SET_DEPTH_BIAS)
		words = 3U;
	first = drv_i915_wire_read_u32(reader);
	for (index = 1U; index < words; index++)
		(void)drv_i915_wire_read_u32(reader);

	/* Refuses a stream that ended inside the values. */
	if (reader->error != 0)
		return EINVAL;

	/* A line width other than 1.0 (0x3f800000) is not drawn as asked. */
	if (opcode == GPU_OP_CMD_SET_LINE_WIDTH && first != 0x3f800000U && !wide_said) {
		kern_logf("i915: vk: XXX unimplemented path: lines of a width other than 1 (drawn one pixel wide)\n");
		wide_said = 1;
	}

	/* Succeeded: the values are decoded. */
	return 0;
}

/*
 * vkCmdPushConstants: [layout][stages][offset][size][count]{bytes}.
 *
 * XXX: one block is shared by every stage; the stages are not kept.
 */
static int
i915_record_push_constants(
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader)
{
	struct i915_gfx_op *op;
	uint64_t count;

	/* Decodes the range of the update. */
	op = i915_command_op(cmdbuf, I915_GFX_OP_PUSH_CONSTANTS);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	op->u.push.offset = drv_i915_wire_read_u32(reader);
	op->u.push.size = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);

	/* Refuses a stream that ended inside the range. */
	if (reader->error != 0)
		return EINVAL;

	/* Refuses bytes that do not match the declared size. */
	if (count != op->u.push.size)
		return EINVAL;

	/* Refuses a range that starts past the push constant block. */
	if (op->u.push.offset > I915_GFX_PUSH_BYTES)
		return EINVAL;

	/* Refuses a range that runs past the end of the block. */
	if (count > I915_GFX_PUSH_BYTES - op->u.push.offset)
		return EINVAL;

	/* Records the bytes. */
	i915_vkc_read_bytes(reader, op->u.push.bytes, (size_t)count);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the update is recorded. */
	return 0;
}

/* vkCmdBindIndexBuffer: [buffer][offset][index type]. */
static int
i915_record_bind_index(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader)
{
	struct i915_gfx_op *op;
	uint64_t identity;

	/* Records the buffer, the offset and the index type. */
	op = i915_command_op(cmdbuf, I915_GFX_OP_BIND_INDEX_BUFFER);
	identity = drv_i915_wire_read_u64(reader);
	op->u.index.buffer = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER, identity);
	op->u.index.offset = drv_i915_wire_read_u64(reader);
	op->u.index.type = drv_i915_wire_read_u32(reader);

	/* Refuses a stream that ended inside the bind. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the bind is recorded. */
	return 0;
}

/*
 * vkCmdSetViewport: [first][count][count]{VkViewport}.
 *
 * A pipeline has one viewport, so only viewport 0 is recorded; the others
 * are decoded and not kept.
 */
static int
i915_record_set_viewport(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader)
{
	VkViewport viewport;
	struct i915_gfx_op *op;
	uint64_t count;
	uint64_t index;
	uint32_t first;

	/* Decodes the first viewport and the number of viewports, at most sixteen. */
	first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > I915_GFX_MAX_REGIONS)
		return EINVAL;

	/* Decodes every viewport and records the one that lands on viewport 0. */
	for (index = 0U; index < count; index++) {
		kern_memset(&viewport, 0, sizeof(viewport));
		i915_vkc_dec_VkViewport(reader, &session->arena, &viewport);
		if (reader->error != 0)
			return EINVAL;

		/* Keeps viewport 0 as float bits; the others have no place in the pipeline. */
		if ((uint64_t)first + index == 0U) {
			op = i915_command_op(cmdbuf, I915_GFX_OP_SET_VIEWPORT);
			kern_memcpy(&op->u.viewport[0], &viewport.x, sizeof(op->u.viewport[0]));
			kern_memcpy(&op->u.viewport[1], &viewport.y, sizeof(op->u.viewport[1]));
			kern_memcpy(&op->u.viewport[2], &viewport.width, sizeof(op->u.viewport[2]));
			kern_memcpy(&op->u.viewport[3], &viewport.height, sizeof(op->u.viewport[3]));
			kern_memcpy(&op->u.viewport[4], &viewport.minDepth, sizeof(op->u.viewport[4]));
			kern_memcpy(&op->u.viewport[5], &viewport.maxDepth, sizeof(op->u.viewport[5]));
		}
	}

	/* Succeeded: viewport 0, when it was set, is recorded. */
	return 0;
}

/*
 * vkCmdSetScissor: [first][count][count]{VkRect2D}.
 *
 * A pipeline has one scissor, so only scissor 0 is recorded; the others are
 * decoded and not kept.
 */
static int
i915_record_set_scissor(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader)
{
	VkRect2D scissor;
	struct i915_gfx_op *op;
	uint64_t count;
	uint64_t index;
	uint32_t first;

	/* Decodes the first scissor and the number of scissors, at most sixteen. */
	first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > I915_GFX_MAX_REGIONS)
		return EINVAL;

	/* Decodes every scissor and records the one that lands on scissor 0. */
	for (index = 0U; index < count; index++) {
		kern_memset(&scissor, 0, sizeof(scissor));
		i915_vkc_dec_VkRect2D(reader, &session->arena, &scissor);
		if (reader->error != 0)
			return EINVAL;

		/* Keeps scissor 0; the others have no place in the pipeline. */
		if ((uint64_t)first + index == 0U) {
			op = i915_command_op(cmdbuf, I915_GFX_OP_SET_SCISSOR);
			op->u.scissor = scissor;
		}
	}

	/* Succeeded: scissor 0, when it was set, is recorded. */
	return 0;
}

/*
 * vkCmdCopyBuffer: [src][dst][region count][count]{VkBufferCopy}.
 *
 * Every region is one operation.
 */
static int
i915_record_copy_buffer(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf,
	struct i915_wire_reader *reader)
{
	struct i915_gfx_buffer *src;
	struct i915_gfx_buffer *dst;
	struct i915_gfx_op *op;
	uint64_t identity;
	uint64_t count;
	uint64_t index;

	/* Decodes the two buffers and the number of regions, at most as many as a buffer records. */
	identity = drv_i915_wire_read_u64(reader);
	src = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER, identity);
	identity = drv_i915_wire_read_u64(reader);
	dst = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER, identity);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > I915_GFX_MAX_OPS)
		return EINVAL;

	/* Records one operation for each region. */
	for (index = 0U; index < count; index++) {
		op = i915_command_op(cmdbuf, I915_GFX_OP_COPY_BUFFER);
		op->u.buffer_copy.src = src;
		op->u.buffer_copy.dst = dst;
		i915_vkc_dec_VkBufferCopy(reader, &session->arena, &op->u.buffer_copy.region);
	}

	/* Refuses a stream that ended inside the regions. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the regions are recorded. */
	return 0;
}

/*
 * Records one vkCmd* into the command buffer it names.
 *
 * Returns ENOTSUP for a recording the module does not implement.
 */
static int
i915_record_command(
	struct i915_render_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader)
{
	struct i915_gfx_cmdbuf *cmdbuf;
	struct i915_gfx_op *op;
	uint64_t identity;
	int error;

	/* Decodes the command buffer the recording names. */
	identity = drv_i915_wire_read_u64(reader);
	cmdbuf = drv_i915_object_lookup(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (reader->error != 0)
		return EINVAL;

	/* Records the operation the opcode names. */
	switch (opcode) {
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
	case GPU_OP_CMD_END_VIDEO_CODING:
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
	case GPU_OP_CMD_DECODE_VIDEO:
		/* The video coding commands (video.c). */
		error = i915_record_video(session, cmdbuf, opcode, reader);
		return error;
	case GPU_OP_CMD_BIND_PIPELINE:
		/* vkCmdBindPipeline: [bind point][pipeline]. */
		op = i915_command_op(cmdbuf, I915_GFX_OP_BIND_PIPELINE);
		(void)drv_i915_wire_read_u32(reader);
		identity = drv_i915_wire_read_u64(reader);
		op->u.pipeline = drv_i915_object_lookup(session, I915_VK_OBJ_PIPELINE, identity);
		break;
	case GPU_OP_CMD_SET_VIEWPORT:
		/* vkCmdSetViewport */
		error = i915_record_set_viewport(session, cmdbuf, reader);
		return error;
	case GPU_OP_CMD_SET_SCISSOR:
		/* vkCmdSetScissor */
		error = i915_record_set_scissor(session, cmdbuf, reader);
		return error;
	case GPU_OP_CMD_SET_LINE_WIDTH:
	case GPU_OP_CMD_SET_DEPTH_BIAS:
		/* vkCmdSetLineWidth and vkCmdSetDepthBias, which the draws do not use. */
		error = i915_record_set_unused(opcode, reader);
		return error;
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
		/* vkCmdSetBlendConstants */
		error = i915_record_set_blend_constants(cmdbuf, reader);
		return error;
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		/* vkCmdBindDescriptorSets */
		error = i915_record_bind_descriptor_sets(session, cmdbuf, reader);
		return error;
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
		/* vkCmdBindIndexBuffer */
		error = i915_record_bind_index(session, cmdbuf, reader);
		return error;
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
		/* vkCmdBindVertexBuffers */
		error = i915_record_bind_vertex(session, cmdbuf, reader);
		return error;
	case GPU_OP_CMD_DRAW:
		/* vkCmdDraw: [vertices][instances][first vertex][first instance]. */
		op = i915_command_op(cmdbuf, I915_GFX_OP_DRAW);
		op->u.draw.vertex_count = drv_i915_wire_read_u32(reader);
		op->u.draw.instance_count = drv_i915_wire_read_u32(reader);
		op->u.draw.first_vertex = drv_i915_wire_read_u32(reader);
		op->u.draw.first_instance = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_DISPATCH:
		/* vkCmdDispatch: [groups x][groups y][groups z] (ws101-p003). */
		op = i915_command_op(cmdbuf, I915_GFX_OP_DISPATCH);
		op->u.dispatch.groups[0] = drv_i915_wire_read_u32(reader);
		op->u.dispatch.groups[1] = drv_i915_wire_read_u32(reader);
		op->u.dispatch.groups[2] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_DISPATCH_INDIRECT:
		/* vkCmdDispatchIndirect: [buffer][offset] (ws101-p007). */
		op = i915_command_op(cmdbuf, I915_GFX_OP_DISPATCH_INDIRECT);
		identity = drv_i915_wire_read_u64(reader);
		op->u.dispatch_indirect.buffer = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER, identity);
		op->u.dispatch_indirect.offset = drv_i915_wire_read_u64(reader);
		break;
	case GPU_OP_CMD_DRAW_INDEXED:
		/* vkCmdDrawIndexed: [indices][instances][first index][vertex offset][first instance]. */
		op = i915_command_op(cmdbuf, I915_GFX_OP_DRAW_INDEXED);
		op->u.draw_indexed.index_count = drv_i915_wire_read_u32(reader);
		op->u.draw_indexed.instance_count = drv_i915_wire_read_u32(reader);
		op->u.draw_indexed.first_index = drv_i915_wire_read_u32(reader);
		op->u.draw_indexed.vertex_offset = (int32_t)drv_i915_wire_read_u32(reader);
		op->u.draw_indexed.first_instance = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_COPY_BUFFER:
		/* vkCmdCopyBuffer */
		error = i915_record_copy_buffer(session, cmdbuf, reader);
		return error;
	case GPU_OP_CMD_COPY_IMAGE:
		/* vkCmdCopyImage */
		error = i915_record_image_copy(session, cmdbuf, reader, I915_GFX_OP_COPY_IMAGE);
		return error;
	case GPU_OP_CMD_BLIT_IMAGE:
		/* vkCmdBlitImage */
		error = i915_record_image_copy(session, cmdbuf, reader, I915_GFX_OP_BLIT_IMAGE);
		return error;
	case GPU_OP_CMD_RESOLVE_IMAGE:
		/* vkCmdResolveImage */
		error = i915_record_image_copy(session, cmdbuf, reader, I915_GFX_OP_RESOLVE_IMAGE);
		return error;
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
		/* vkCmdClearColorImage */
		error = i915_record_clear_image(session, cmdbuf, reader);
		return error;
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
		/* vkCmdClearAttachments */
		error = i915_record_clear_attachments(session, cmdbuf, reader);
		return error;
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
		/* vkCmdCopyBufferToImage */
		error = i915_record_buffer_image_copy(session, cmdbuf, reader, 1);
		return error;
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
		/* vkCmdCopyImageToBuffer */
		error = i915_record_buffer_image_copy(session, cmdbuf, reader, 0);
		return error;
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
		/* vkCmdSetStencilCompareMask, vkCmdSetStencilWriteMask, vkCmdSetStencilReference */
		error = i915_record_set_stencil(cmdbuf, reader, opcode - 100U);
		return error;
	case GPU_OP_CMD_PIPELINE_BARRIER:
		/* vkCmdPipelineBarrier */
		error = i915_record_barrier(session, reader);
		return error;
	case GPU_OP_CMD_BEGIN_QUERY:
	case GPU_OP_CMD_END_QUERY:
	case GPU_OP_CMD_RESET_QUERY_POOL:
		/* vkCmdBeginQuery, vkCmdEndQuery, vkCmdResetQueryPool */
		error = i915_record_query(session, cmdbuf, reader, opcode);
		return error;
	case GPU_OP_CMD_PUSH_CONSTANTS:
		/* vkCmdPushConstants */
		error = i915_record_push_constants(cmdbuf, reader);
		return error;
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
		/* vkCmdBeginRenderPass */
		error = i915_record_begin_pass(session, cmdbuf, reader);
		return error;
	case GPU_OP_CMD_END_RENDER_PASS:
		/* vkCmdEndRenderPass */
		(void)i915_command_op(cmdbuf, I915_GFX_OP_END_PASS);
		break;
	default:
		return ENOTSUP;
	}

	/* Refuses a stream that ended inside the recording. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the operation is recorded. */
	return 0;
}

/*
 * Describes one mip level of one slice of an image (an array layer, or a
 * depth of a 3D image) as the linear surface it is, in its own format;
 * EINVAL for a level or a slice the image does not have or an image with
 * no storage, with the level and the slice named in the log.
 */
static int
i915_image_surface(
	const struct i915_gfx_image *image,
	uint32_t level,
	uint32_t slice,
	struct i915_gfx_surface *surface)
{
	int error;

	/* Takes the level's address in the slice, its extent, the pitch and the format. */
	error = drv_i915_gfx_image_slice(image, level, slice, surface);
	if (error != 0) {
		kern_logf("i915: vk: level %u slice %u of a %ux%u image of %u levels and %u slices cannot be used: %d\n",
			  level,
			  slice,
			  image->width,
			  image->height,
			  image->levels,
			  drv_i915_gfx_image_slices(image, 0U),
			  error);
		return error;
	}

	/* Succeeded: the surface describes the level. */
	return 0;
}

/*
 * The planes of an image that an aspect mask names: I915_IMAGE_PLANE_MAIN
 * for the colour or depth texels, I915_IMAGE_PLANE_STENCIL for the
 * separate stencil plane.  An image without stencil has only the main
 * plane, whatever the mask; a stencil-only image only the stencil plane.
 */
static uint32_t
i915_image_planes(
	const struct i915_gfx_image *image,
	uint32_t aspects)
{
	uint32_t planes;

	/* An image without stencil is its main plane. */
	if (image->stencil == 0U)
		return I915_IMAGE_PLANE_MAIN;

	/* A stencil-only image is its stencil plane. */
	if (image->format == VK_FORMAT_S8_UINT)
		return I915_IMAGE_PLANE_STENCIL;

	/* The depth aspect (or no aspect at all) names the depth plane. */
	planes = 0U;
	if ((aspects & ~(uint32_t)VK_IMAGE_ASPECT_STENCIL_BIT) != 0U)
		planes |= I915_IMAGE_PLANE_MAIN;
	if (aspects == 0U)
		planes |= I915_IMAGE_PLANE_MAIN;

	/* The stencil aspect names the stencil plane. */
	if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U)
		planes |= I915_IMAGE_PLANE_STENCIL;

	/* Succeeded: the planes the mask names. */
	return planes;
}

/*
 * Describes one slice of one plane of an image as a surface: the main
 * plane as i915_image_surface() does, the stencil plane as its Y-tiled
 * bytes (VK_FORMAT_S8_UINT, which the surface state names Y-tiled
 * R8_UNORM).  The stencil plane has one level; EINVAL for another level, a
 * slice the image does not have or an image with no storage.
 */
static int
i915_image_plane_surface(
	const struct i915_gfx_image *image,
	uint32_t plane,
	uint32_t level,
	uint32_t slice,
	struct i915_gfx_surface *surface)
{
	uint64_t offset;
	int error;

	/* The main plane is the image as it is laid out. */
	if (plane != I915_IMAGE_PLANE_STENCIL) {
		error = i915_image_surface(image, level, slice, surface);
		if (error != 0)
			return error;

		/* Succeeded: the surface describes the main plane's level. */
		return 0;
	}

	/* Refuses a stencil plane the image does not have, and a level or slice outside it. */
	if (image->stencil == 0U || level != 0U)
		return EINVAL;
	if (slice >= drv_i915_gfx_image_slices(image, 0U))
		return EINVAL;

	/* The slice's rows of the stencil plane, which follows the depth plane. */
	offset = image->offset + image->stencil_offset + (uint64_t)slice * image->stencil_pitch * image->stencil_slice_rows;
	surface->va = drv_i915_gfx_memory_va(image->memory, offset);
	surface->width = image->width;
	surface->height = image->height;
	surface->pitch = image->stencil_pitch;
	surface->format = VK_FORMAT_S8_UINT;
	surface->tiled = 1U;

	/* A multisampled plane interleaves each pixel's samples. */
	if (image->samples > 1U) {
		surface->width = image->sample_width;
		surface->height = image->sample_height;
	}

	/* Refuses an image that is not bound to storage. */
	if (surface->va == 0U)
		return EINVAL;

	/* Succeeded: the surface describes the stencil plane's slice. */
	return 0;
}

/*
 * Describes the image of a render pass attachment: the level and the layer
 * its view starts at, in the format the view reads the texels as (a clear
 * of an SRGB view of an UNORM image encodes as the draw does, ws031-p033).
 * A clear of several layers describes each layer through a view of that one
 * layer (ws075-p007b).
 */
static int
i915_attachment_surface(
	const struct i915_gfx_view *view,
	struct i915_gfx_surface *surface)
{
	int error;

	/* Describes the view's first level of its first layer. */
	error = i915_image_surface(view->image, view->base_level, view->base_layer, surface);
	if (error != 0)
		return error;

	/* Succeeded: the surface describes the attachment, in the view's format. */
	surface->format = drv_i915_gfx_view_format(view);
	return 0;
}

/*
 * Fills a rectangle of the stencil plane of a view's layer with a stencil
 * value: the plane's bytes as Y-tiled R8_UNORM (VK_FORMAT_S8_UINT in the
 * surface), the value as value / 255, which the target stores exactly.
 * The rectangle is clipped to the image; an empty one fills nothing.
 */
static int
i915_clear_stencil(
	struct i915_render_session *session,
	const struct i915_gfx_view *view,
	const struct i915_gfx_rect *area,
	uint32_t value)
{
	const struct i915_gfx_image *image;
	struct i915_gfx_surface surface;
	struct i915_gfx_rect rect;
	uint32_t words[4];
	int64_t right;
	int64_t bottom;
	int error;

	/* The stencil plane of the view's layer. */
	image = view->image;
	kern_memset(&surface, 0, sizeof(surface));
	error = i915_image_plane_surface(image, I915_IMAGE_PLANE_STENCIL, 0U, view->base_layer, &surface);
	if (error != 0)
		return error;

	/* The rectangle, clipped to the plane. */
	rect = *area;
	right = (int64_t)rect.x + rect.w;
	bottom = (int64_t)rect.y + rect.h;
	if (rect.x < 0)
		rect.x = 0;
	if (rect.y < 0)
		rect.y = 0;
	if (right > (int64_t)surface.width)
		right = surface.width;
	if (bottom > (int64_t)surface.height)
		bottom = surface.height;
	if (right <= rect.x || bottom <= rect.y)
		return 0;
	rect.w = (uint32_t)(right - rect.x);
	rect.h = (uint32_t)(bottom - rect.y);

	/* The value's normalized form. */
	kern_memset(words, 0, sizeof(words));
	words[0] = i915_unorm8_float_bits(value & 0xffU);

	/* Fills the rectangle. */
	error = drv_i915_gfx_rect(session, &surface, &rect, NULL, NULL, words, 0);
	return error;
}

/*
 * Returns the float bits of value * 257 / 65536, within a 65536th of
 * value / 255: the float an 8-bit unorm target rounds back to `value`.
 * Integer arithmetic only.
 */
static uint32_t
i915_unorm8_float_bits(
	uint32_t value)
{
	uint32_t number;
	uint32_t top;

	/* Zero is all zero bits. */
	number = value * 257U;
	if (number == 0U)
		return 0U;

	/* The highest set bit of the number, 0 to 15. */
	top = 15U;
	while ((number & (1U << top)) == 0U)
		top--;

	/* The exponent of 2^(top - 16) and the mantissa below the leading one. */
	return ((127U + top - 16U) << 23) | ((number << (23U - top)) & 0x7fffffU);
}

/*
 * Turns the surface of a D32 image's slice into the R32_FLOAT view of the
 * slice's bytes, padding rows included: a whole-slice clear writes the same
 * float everywhere, so the Y tiling does not matter.  The slice is the
 * image's slice rows, or all its rows for an image of one slice.  A D16
 * image is not cleared this way: its word of two values (1.0 is
 * 0xffffffff) is a NaN as a float, which the fill does not carry as it is;
 * it is drawn into as the Y-tiled R16_UNORM target instead.
 */
static void
i915_depth_words_surface(
	const struct i915_gfx_image *image,
	struct i915_gfx_surface *surface)
{
	/* Every word of the slice (of the depth plane, before a stencil plane), the surface's address kept. */
	surface->format = VK_FORMAT_R32_SFLOAT;
	surface->width = image->pitch / 4U;
	surface->height = image->slice_rows;
	if (surface->height == 0U && image->stencil != 0U)
		surface->height = (uint32_t)(image->stencil_offset / image->pitch);
	if (surface->height == 0U)
		surface->height = (uint32_t)(image->bytes / image->pitch);
}

/*
 * Runs the clears of a render pass's begin (loadOp CLEAR), one GPU fill for
 * each attachment.
 *
 * A D32 clear writes the depth value through an R32_FLOAT view of the
 * attachment's slice (i915_depth_words_surface()): every word gets the same
 * value, so the depth buffer's tiling does not matter.  A D16 clear draws
 * the depth into the Y-tiled R16_UNORM surface of the attachment.
 * XXX: the stencil value is not written, and the whole attachment is
 * cleared, not the render area.
 */
static int
i915_execute_clear(
	struct i915_render_session *session,
	const struct i915_gfx_op *op)
{
	struct i915_gfx_framebuffer *framebuffer;
	struct i915_gfx_pass *pass;
	struct i915_gfx_view layer_view;
	uint32_t index;
	uint32_t layers;
	uint32_t view_layers;
	uint32_t layer;
	int error;

	/* A begin without a pass or a framebuffer clears nothing. */
	pass = op->u.begin.pass;
	framebuffer = op->u.begin.framebuffer;
	if (pass == NULL || framebuffer == NULL)
		return 0;

	/* Fills every attachment that loads with a clear, on every layer of the framebuffer. */
	for (index = 0U; index < pass->attachment_count && index < framebuffer->view_count; index++) {
		/* Skips an attachment the begin gave no clear value, or the framebuffer no view. */
		if (index >= op->u.begin.clear_count || framebuffer->views[index] == NULL)
			continue;

		/* The layers the begin clears: the framebuffer's, within the view's (zero reads as one). */
		layers = framebuffer->layers;
		if (layers == 0U)
			layers = 1U;
		view_layers = framebuffer->views[index]->layer_count;
		if (view_layers == 0U)
			view_layers = 1U;
		if (layers > view_layers)
			layers = view_layers;

		/* Clears each layer through a view of that one layer. */
		for (layer = 0U; layer < layers; layer++) {
			layer_view = *framebuffer->views[index];
			layer_view.base_layer += layer;
			layer_view.layer_count = 1U;
			error = i915_execute_clear_layer(session, op, index, &layer_view);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: every attachment that loads with a clear is cleared. */
	return 0;
}

/* Runs the clears of one attachment of a render pass's begin on one layer of its view. */
static int
i915_execute_clear_layer(
	struct i915_render_session *session,
	const struct i915_gfx_op *op,
	uint32_t index,
	const struct i915_gfx_view *view)
{
	struct i915_gfx_pass *pass;
	struct i915_gfx_image *image;
	struct i915_gfx_surface surface;
	struct i915_gfx_rect rect;
	uint32_t words[4];
	int error;

	/* A stencil plane that loads with a clear is filled with the stencil value. */
	pass = op->u.begin.pass;
	image = view->image;
	if (image->stencil != 0U && pass->attachments[index].stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
		rect.x = 0;
		rect.y = 0;
		rect.w = image->width;
		rect.h = image->height;
		i915_sample_rect(image, &rect);
		error = i915_clear_stencil(session, view, &rect, op->u.begin.clear_words[index][1]);
		if (error != 0)
			return error;
	}

	/* Skips an attachment whose colour or depth does not load with a clear, and a stencil-only one. */
	if (pass->attachments[index].load_op != VK_ATTACHMENT_LOAD_OP_CLEAR || image->format == VK_FORMAT_S8_UINT)
		return 0;

	/* Describes the attachment's image. */
	error = i915_attachment_surface(view, &surface);
	if (error != 0)
		return error;

	/* Takes the value: D32 through the R32_FLOAT view, D16 as the target's colour, or the four colour words. */
	kern_memset(words, 0, sizeof(words));
	if (op->u.begin.clear_is_depth[index] != 0U &&
	    (image->format == VK_FORMAT_D32_SFLOAT || image->format == VK_FORMAT_D32_SFLOAT_S8_UINT)) {
		i915_depth_words_surface(image, &surface);
		words[0] = drv_i915_gfx_depth_clear_word(image->format, op->u.begin.clear_words[index][0]);
	} else if (op->u.begin.clear_is_depth[index] != 0U) {
		words[0] = op->u.begin.clear_words[index][0];
	} else {
		kern_memcpy(words, op->u.begin.clear_words[index], sizeof(words));
	}

	/* Fills the whole surface, on every sample. */
	rect.x = 0;
	rect.y = 0;
	rect.w = surface.width;
	rect.h = surface.height;
	error = i915_fill_samples(session, image, &surface, &rect, words);
	if (error != 0)
		return error;

	/* Succeeded: the layer of the attachment is cleared. */
	return 0;
}

/*
 * Runs one vkCmdClearAttachments rectangle on the attachment of the pass in
 * progress.
 *
 * The rectangle is clipped to the attachment.  A D32 clear of the whole
 * attachment fills the R32_FLOAT view of the slice's bytes, as the pass
 * begin does; a smaller D32 rectangle and any D16 one are drawn into the
 * Y-tiled depth surface.
 * XXX: the stencil value is not written.
 */
static int
i915_execute_clear_attachment(
	struct i915_render_session *session,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_op *op)
{
	struct i915_gfx_framebuffer *framebuffer;
	struct i915_gfx_view layer_view;
	uint32_t attachment;
	uint32_t layers;
	uint32_t layer;
	int error;

	/* A clear outside a pass is refused. */
	framebuffer = state->framebuffer;
	if (state->pass == NULL || framebuffer == NULL)
		return EINVAL;

	/* Finds the attachment the clear names; one the subpass does not use clears nothing. */
	attachment = drv_i915_gfx_pass_color(state->pass, op->u.clear_attachment.color_index);
	if (op->u.clear_attachment.is_depth != 0U)
		attachment = state->pass->depth_attachment;
	if (attachment >= framebuffer->view_count || framebuffer->views[attachment] == NULL)
		return 0;

	/* Clears each layer of the rectangle (a count of zero reads as one) through a view of that layer. */
	layers = op->u.clear_attachment.layer_count;
	if (layers == 0U)
		layers = 1U;
	for (layer = 0U; layer < layers; layer++) {
		layer_view = *framebuffer->views[attachment];
		layer_view.base_layer += op->u.clear_attachment.base_layer + layer;
		layer_view.layer_count = 1U;
		error = i915_execute_clear_rect(session, op, &layer_view);
		if (error != 0)
			return error;
	}

	/* Succeeded: every layer of the rectangle is cleared. */
	return 0;
}

/* Runs one vkCmdClearAttachments rectangle on one layer of the attachment's view. */
static int
i915_execute_clear_rect(
	struct i915_render_session *session,
	const struct i915_gfx_op *op,
	const struct i915_gfx_view *view)
{
	struct i915_gfx_image *image;
	struct i915_gfx_surface surface;
	struct i915_gfx_rect rect;
	uint32_t words[4];
	int64_t right;
	int64_t bottom;
	int error;

	/* A stencil clear fills the rectangle of the stencil plane; a depth or stencil rectangle is one of samples. */
	image = view->image;
	rect = op->u.clear_attachment.rect;
	if (op->u.clear_attachment.is_depth != 0U)
		i915_sample_rect(image, &rect);
	if (op->u.clear_attachment.is_depth != 0U &&
	    (op->u.clear_attachment.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U &&
	    image->stencil != 0U) {
		error = i915_clear_stencil(session, view, &rect, op->u.clear_attachment.words[1]);
		if (error != 0)
			return error;
	}

	/* A depth / stencil clear without the depth aspect (0 reads as depth), or of a stencil-only image, is done. */
	if (op->u.clear_attachment.is_depth != 0U &&
	    ((op->u.clear_attachment.aspects != 0U &&
	      (op->u.clear_attachment.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) == 0U) ||
	     image->format == VK_FORMAT_S8_UINT))
		return 0;

	/* Describes the attachment's image. */
	error = i915_attachment_surface(view, &surface);
	if (error != 0)
		return error;
	kern_memcpy(words, op->u.clear_attachment.words, sizeof(words));

	/*
	 * A D32 rectangle covering the attachment fills the words of its whole
	 * slice, padding included, as the pass begin does; a smaller one, and
	 * any D16 one, is drawn into the Y-tiled R32_FLOAT or R16_UNORM
	 * surface of the depth buffer with the depth as the colour.
	 */
	if (op->u.clear_attachment.is_depth != 0U &&
	    (image->format == VK_FORMAT_D32_SFLOAT || image->format == VK_FORMAT_D32_SFLOAT_S8_UINT) &&
	    rect.x <= 0 && rect.y <= 0 &&
	    (int64_t)rect.x + rect.w >= (int64_t)surface.width &&
	    (int64_t)rect.y + rect.h >= (int64_t)surface.height) {
		i915_depth_words_surface(image, &surface);
		words[0] = drv_i915_gfx_depth_clear_word(image->format, words[0]);
		rect.x = 0;
		rect.y = 0;
		rect.w = surface.width;
		rect.h = surface.height;
	}

	/* Clips the rectangle to the surface; an empty one clears nothing. */
	right = (int64_t)rect.x + rect.w;
	bottom = (int64_t)rect.y + rect.h;
	if (rect.x < 0)
		rect.x = 0;
	if (rect.y < 0)
		rect.y = 0;
	if (right > (int64_t)surface.width)
		right = surface.width;
	if (bottom > (int64_t)surface.height)
		bottom = surface.height;
	if (right <= rect.x || bottom <= rect.y)
		return 0;
	rect.w = (uint32_t)(right - rect.x);
	rect.h = (uint32_t)(bottom - rect.y);

	/* Fills the rectangle, on every sample. */
	error = i915_fill_samples(session, image, &surface, &rect, words);
	return error;
}

/*
 * Describes one NV12 plane for a standard image-to-buffer copy.  Plane 1
 * coordinates are CbCr pairs, whose extent is half the luma extent rounded
 * up.  The existing GPU rectangle copies R8 or R8G8 texels from Y tiles;
 * callers never need to interpret the decoder's storage themselves.
 */
static int
i915_nv12_copy_surface(
	const struct i915_gfx_op *op,
	struct i915_gfx_surface *surface,
	uint32_t *texel_bytes)
{
	const struct i915_gfx_image *image;
	const VkBufferImageCopy *region;
	uint64_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t bytes;
	uint32_t x;
	uint32_t y;

	/* Only readback was admitted by the NV12 image usage queries. */
	image = op->u.copy.image;
	region = &op->u.copy.region;
	if (op->kind != I915_GFX_OP_COPY_IMAGE_TO_BUFFER)
		return EINVAL;

	/* The resources must have been created for this transfer direction. */
	if ((image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0U)
		return EINVAL;
	if ((op->u.copy.buffer->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0U)
		return EINVAL;

	/* NV12 has one level, one layer and one depth sample per plane. */
	if (region->imageSubresource.mipLevel != 0U || region->imageSubresource.baseArrayLayer != 0U)
		return EINVAL;
	if (region->imageSubresource.layerCount != 1U || region->imageExtent.depth != 1U)
		return EINVAL;
	if (region->imageOffset.x < 0 || region->imageOffset.y < 0 || region->imageOffset.z != 0)
		return EINVAL;

	/* The selected plane's compatible format defines its sample size. */
	offset = 0U;
	width = image->width;
	height = image->height;
	format = VK_FORMAT_R8_UNORM;
	bytes = 1U;
	if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_PLANE_1_BIT) {
		offset = image->chroma_offset;
		width = (width + 1U) / 2U;
		height = (height + 1U) / 2U;
		format = VK_FORMAT_R8G8_UNORM;
		bytes = 2U;
	} else if (region->imageSubresource.aspectMask != VK_IMAGE_ASPECT_PLANE_0_BIT) {
		/* Colour, combined planes and nonexistent planes are not plane copies. */
		return EINVAL;
	}

	/* Reject a rectangle outside the selected plane, before building GPU state. */
	x = (uint32_t)region->imageOffset.x;
	y = (uint32_t)region->imageOffset.y;
	if (x >= width || y >= height)
		return EINVAL;
	if (region->imageExtent.width > width - x || region->imageExtent.height > height - y)
		return EINVAL;

	/* The destination pitch fits the surface field and its texels are aligned. */
	if (region->bufferRowLength > UINT32_MAX / bytes)
		return EINVAL;
	if ((region->bufferOffset % bytes) != 0U)
		return EINVAL;

	/* The bound plane is a Y-tiled surface of the compatible format. */
	surface->va = drv_i915_gfx_memory_va(image->memory, image->offset + offset);
	if (surface->va == 0U)
		return EINVAL;
	surface->width = width;
	surface->height = height;
	surface->pitch = image->pitch;
	surface->format = format;
	surface->tiled = 1U;
	*texel_bytes = bytes;

	/* Succeeded: the rectangle path can read this plane as ordinary texels. */
	return 0;
}

/*
 * Runs a vkCmdCopyBufferToImage or vkCmdCopyImageToBuffer region as one GPU
 * copy.
 *
 * The buffer region is a linear surface of the image's format with the
 * region's row length (a depth image's values as R32_FLOAT or R16_UNORM,
 * a stencil plane's bytes as R8_UNORM); the image side is the mip level of
 * the plane the region's aspect names.
 */
static int
i915_execute_buffer_image_copy(
	struct i915_render_session *session,
	const struct i915_gfx_op *op)
{
	const VkBufferImageCopy *region;
	struct i915_gfx_buffer *buffer;
	struct i915_gfx_image *image;
	struct i915_gfx_surface image_surface;
	struct i915_gfx_surface buffer_surface;
	struct i915_gfx_rect image_rect;
	struct i915_gfx_rect buffer_rect;
	uint64_t row_pixels;
	uint64_t image_rows;
	uint64_t needed;
	uint64_t slice_bytes;
	uint32_t buffer_format;
	uint32_t texel_bytes;
	uint32_t first_slice;
	uint32_t slice_count;
	uint32_t slice;
	uint32_t plane;
	int error;

	/* Refuses a copy whose buffer or image does not exist. */
	buffer = op->u.copy.buffer;
	image = op->u.copy.image;
	region = &op->u.copy.region;
	if (buffer == NULL || image == NULL)
		return EINVAL;

	/* The slices the region names: the layers, or a 3D image's depths. */
	first_slice = region->imageSubresource.baseArrayLayer;
	slice_count = region->imageSubresource.layerCount;
	if (image->type == VK_IMAGE_TYPE_3D) {
		first_slice = (uint32_t)region->imageOffset.z;
		slice_count = region->imageExtent.depth;
	}

	/* Refuses a region of no slice. */
	if (slice_count == 0U)
		return EINVAL;

	/* The plane the region's one aspect names: the stencil plane, or the colour or depth texels. */
	plane = i915_image_planes(image, region->imageSubresource.aspectMask);
	if (plane != I915_IMAGE_PLANE_STENCIL)
		plane = I915_IMAGE_PLANE_MAIN;

	/* Describes the level of the plane the region names, in its first slice. */
	texel_bytes = drv_i915_gfx_format_bytes(image->format);
	if (image->planar != 0U) {
		error = i915_nv12_copy_surface(op, &image_surface, &texel_bytes);
	} else {
		error = i915_image_plane_surface(image, plane, region->imageSubresource.mipLevel, first_slice, &image_surface);
	}

	/* A plane that cannot be represented has no GPU copy. */
	if (error != 0)
		return EINVAL;

	/*
	 * The buffer holds the image's texels in the image's format, a depth
	 * plane's R32_FLOAT or R16_UNORM values, or a stencil plane's bytes as
	 * R8_UNORM, which the copy moves between the linear buffer and the
	 * Y-tiled image.
	 */
	buffer_format = image_surface.format;
	if (plane == I915_IMAGE_PLANE_STENCIL) {
		buffer_format = VK_FORMAT_R8_UNORM;
		texel_bytes = 1U;
	} else if (image->format == VK_FORMAT_D32_SFLOAT || image->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
		buffer_format = VK_FORMAT_R32_SFLOAT;
		texel_bytes = 4U;
	} else if (image->format == VK_FORMAT_D16_UNORM) {
		buffer_format = VK_FORMAT_R16_UNORM;
		texel_bytes = 2U;
	}

	/* A zero row length means rows as long as the region. */
	row_pixels = region->imageExtent.width;
	if (region->bufferRowLength != 0U)
		row_pixels = region->bufferRowLength;

	/* Refuses a region that is not non-empty rectangles, one a slice. */
	if (image->type != VK_IMAGE_TYPE_3D && region->imageExtent.depth != 1U)
		return EINVAL;
	if (region->imageExtent.width == 0U || region->imageExtent.height == 0U)
		return EINVAL;

	/* Refuses rows shorter than the region, and slices shorter than its rows. */
	if (row_pixels < region->imageExtent.width)
		return EINVAL;
	image_rows = region->imageExtent.height;
	if (region->bufferImageHeight != 0U)
		image_rows = region->bufferImageHeight;
	if (image_rows < region->imageExtent.height)
		return EINVAL;

	/* Refuses a region that runs past the end of the buffer. */
	slice_bytes = image_rows * row_pixels * texel_bytes;
	needed = (uint64_t)(slice_count - 1U) * slice_bytes +
	    ((uint64_t)(region->imageExtent.height - 1U) * row_pixels + region->imageExtent.width) * texel_bytes;
	if (region->bufferOffset > buffer->size)
		return EINVAL;
	if (needed > buffer->size - region->bufferOffset)
		return EINVAL;

	/* Describes the buffer region as a linear surface of the image's format. */
	buffer_surface.va = drv_i915_gfx_memory_va(buffer->memory, buffer->offset + region->bufferOffset);
	buffer_surface.width = region->imageExtent.width;
	buffer_surface.height = region->imageExtent.height;
	buffer_surface.pitch = (uint32_t)(row_pixels * texel_bytes);
	buffer_surface.format = buffer_format;
	buffer_surface.tiled = 0U;
	if (buffer_surface.va == 0U)
		return EINVAL;

	/* The whole buffer surface is the region. */
	buffer_rect.x = 0;
	buffer_rect.y = 0;
	buffer_rect.w = region->imageExtent.width;
	buffer_rect.h = region->imageExtent.height;

	/* The image rectangle is the region at its offset. */
	image_rect.x = region->imageOffset.x;
	image_rect.y = region->imageOffset.y;
	image_rect.w = region->imageExtent.width;
	image_rect.h = region->imageExtent.height;

	/* Copies each slice in the direction the operation names, the buffer a slice further each time. */
	for (slice = 0U; slice < slice_count; slice++) {
		/* Describes the slice, and moves the buffer to its rows. */
		if (slice != 0U) {
			error = i915_image_plane_surface(image, plane, region->imageSubresource.mipLevel, first_slice + slice, &image_surface);
			if (error != 0)
				return EINVAL;
			buffer_surface.va += slice_bytes;
		}

		/* Copies the slice. */
		if (op->kind == I915_GFX_OP_COPY_BUFFER_TO_IMAGE) {
			error = drv_i915_gfx_rect(session, &image_surface, &image_rect, &buffer_surface, &buffer_rect, NULL, 0);
		} else {
			error = drv_i915_gfx_rect(session, &buffer_surface, &buffer_rect, &image_surface, &image_rect, NULL, 0);
		}

		/* Reports why the copy failed. */
		if (error != 0)
			return error;
	}

	/* Succeeded: the region is copied. */
	return 0;
}

/*
 * Runs one range of a vkCmdClearColorImage as one GPU fill of each level
 * and layer (every depth of a 3D image's level) it names;
 * VK_REMAINING_MIP_LEVELS and VK_REMAINING_ARRAY_LAYERS run to the last
 * one.
 */
static int
i915_execute_clear_image(
	struct i915_render_session *session,
	const struct i915_gfx_op *op)
{
	struct i915_gfx_image *image;
	struct i915_gfx_surface surface;
	struct i915_gfx_rect rect;
	uint32_t level;
	uint32_t level_count;
	uint32_t first_slice;
	uint32_t slice_count;
	uint32_t slices;
	uint32_t slice;
	int error;

	/* Refuses a clear whose image does not exist. */
	image = op->u.clear_image.image;
	if (image == NULL)
		return EINVAL;

	/* Refuses a range that starts past the image's last level. */
	if (op->u.clear_image.base_level >= image->levels)
		return EINVAL;

	/* The remaining levels run to the last one; a range past the last level is refused. */
	level_count = op->u.clear_image.level_count;
	if (level_count == VK_REMAINING_MIP_LEVELS)
		level_count = image->levels - op->u.clear_image.base_level;
	if (level_count > image->levels - op->u.clear_image.base_level)
		return EINVAL;

	/* Fills every level of the range with the clear words. */
	for (level = op->u.clear_image.base_level; level < op->u.clear_image.base_level + level_count; level++) {
		/* The layers of the range, or every depth of a 3D image's level. */
		slices = drv_i915_gfx_image_slices(image, level);
		first_slice = op->u.clear_image.base_layer;
		slice_count = op->u.clear_image.layer_count;
		if (image->type == VK_IMAGE_TYPE_3D) {
			first_slice = 0U;
			slice_count = slices;
		}

		/* The remaining layers run to the last one. */
		if (slice_count == VK_REMAINING_ARRAY_LAYERS && first_slice < slices)
			slice_count = slices - first_slice;

		/* Fills each slice of the level. */
		for (slice = first_slice; slice < first_slice + slice_count; slice++) {
			/* Describes the level of the slice. */
			error = i915_image_surface(image, level, slice, &surface);
			if (error != 0)
				return EINVAL;

			/* Fills the whole level. */
			rect.x = 0;
			rect.y = 0;
			rect.w = surface.width;
			rect.h = surface.height;
			error = i915_fill_samples(session, image, &surface, &rect, op->u.clear_image.words);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: every level of the range is cleared. */
	return 0;
}

/*
 * Runs a vkCmdCopyImage region as GPU copies between the two levels it
 * names, one for each plane its aspects name (the depth and the stencil of
 * a format with stencil are separate planes).
 */
static int
i915_execute_image_copy(
	struct i915_render_session *session,
	const struct i915_gfx_op *op)
{
	uint32_t src_planes;
	uint32_t dst_planes;
	int error;

	/* Refuses a copy whose images do not exist. */
	if (op->u.image_copy.src == NULL || op->u.image_copy.dst == NULL)
		return EINVAL;

	/* The planes each side's aspects name. */
	src_planes = i915_image_planes(op->u.image_copy.src, op->u.image_copy.region.srcSubresource.aspectMask);
	dst_planes = i915_image_planes(op->u.image_copy.dst, op->u.image_copy.region.dstSubresource.aspectMask);

	/* Copies the colour or depth texels when both sides name them. */
	if ((src_planes & dst_planes & I915_IMAGE_PLANE_MAIN) != 0U) {
		error = i915_image_copy_plane(session, op, I915_IMAGE_PLANE_MAIN);
		if (error != 0)
			return error;
	}

	/* Copies the stencil plane when both sides name it. */
	if ((src_planes & dst_planes & I915_IMAGE_PLANE_STENCIL) != 0U) {
		error = i915_image_copy_plane(session, op, I915_IMAGE_PLANE_STENCIL);
		if (error != 0)
			return error;
	}

	/* Succeeded: the region is copied. */
	return 0;
}

/* Copies one plane of a vkCmdCopyImage region, slice by slice. */
static int
i915_image_copy_plane(
	struct i915_render_session *session,
	const struct i915_gfx_op *op,
	uint32_t plane)
{
	const VkImageCopy *region;
	struct i915_gfx_surface src_surface;
	struct i915_gfx_surface dst_surface;
	struct i915_gfx_rect src_rect;
	struct i915_gfx_rect dst_rect;
	uint32_t src_slice;
	uint32_t dst_slice;
	uint32_t slice_count;
	uint32_t slice;
	int error;

	/* The slices on each side: the layers, or a 3D image's depths from its z offset. */
	region = &op->u.image_copy.region;
	src_slice = region->srcSubresource.baseArrayLayer;
	if (op->u.image_copy.src->type == VK_IMAGE_TYPE_3D)
		src_slice = (uint32_t)region->srcOffset.z;
	dst_slice = region->dstSubresource.baseArrayLayer;
	if (op->u.image_copy.dst->type == VK_IMAGE_TYPE_3D)
		dst_slice = (uint32_t)region->dstOffset.z;
	slice_count = region->srcSubresource.layerCount;
	if (op->u.image_copy.src->type == VK_IMAGE_TYPE_3D || op->u.image_copy.dst->type == VK_IMAGE_TYPE_3D)
		slice_count = region->extent.depth;

	/* The source rectangle is the extent at the source offset. */
	src_rect.x = region->srcOffset.x;
	src_rect.y = region->srcOffset.y;
	src_rect.w = region->extent.width;
	src_rect.h = region->extent.height;

	/* The destination rectangle is the same extent at the destination offset. */
	dst_rect.x = region->dstOffset.x;
	dst_rect.y = region->dstOffset.y;
	dst_rect.w = region->extent.width;
	dst_rect.h = region->extent.height;

	/* Copies the region slice by slice. */
	for (slice = 0U; slice < slice_count; slice++) {
		/* Describes the source level of the slice. */
		error = i915_image_plane_surface(op->u.image_copy.src, plane, region->srcSubresource.mipLevel, src_slice + slice, &src_surface);
		if (error != 0)
			return EINVAL;

		/* Describes the destination level of the slice. */
		error = i915_image_plane_surface(op->u.image_copy.dst, plane, region->dstSubresource.mipLevel, dst_slice + slice, &dst_surface);
		if (error != 0)
			return EINVAL;

		/* Copies the slice. */
		error = drv_i915_gfx_rect(session, &dst_surface, &dst_rect, &src_surface, &src_rect, NULL, 0);
		if (error != 0)
			return error;
	}

	/* Succeeded: the plane of the region is copied. */
	return 0;
}

/*
 * Puts two corner coordinates of a blit in increasing order; returns
 * `mirror` when they were given in decreasing order, 0 otherwise.
 */
static int
i915_blit_order(
	int32_t *first,
	int32_t *second,
	int mirror)
{
	int32_t swap;

	/* An increasing pair stays as it is. */
	if (*second >= *first)
		return 0;

	/* A decreasing pair is swapped and mirrors the copy. */
	swap = *first;
	*first = *second;
	*second = swap;
	return mirror;
}

/*
 * Runs a vkCmdBlitImage region as one scaled GPU copy between the two
 * levels it names for each plane its aspects name, which may be two levels
 * of the same image (the mip chain a linear blit generates level by
 * level).  An offset pair given in decreasing order on one side mirrors the
 * copy along that direction.
 */
static int
i915_execute_image_blit(
	struct i915_render_session *session,
	const struct i915_gfx_op *op)
{
	const VkImageBlit *region;
	struct i915_gfx_rect src_rect;
	struct i915_gfx_rect dst_rect;
	uint32_t src_planes;
	uint32_t dst_planes;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	int linear;
	int error;

	/* Refuses a blit whose images do not exist. */
	region = &op->u.blit.region;
	if (op->u.blit.src == NULL || op->u.blit.dst == NULL)
		return EINVAL;

	/* Takes the source corners in increasing order, a decreasing pair mirroring; an empty rectangle copies nothing. */
	linear = 0;
	x0 = region->srcOffsets[0].x;
	y0 = region->srcOffsets[0].y;
	x1 = region->srcOffsets[1].x;
	y1 = region->srcOffsets[1].y;
	linear ^= i915_blit_order(&x0, &x1, I915_GFX_RECT_MIRROR_X);
	linear ^= i915_blit_order(&y0, &y1, I915_GFX_RECT_MIRROR_Y);
	if (x1 == x0 || y1 == y0)
		return 0;

	/* Takes the source rectangle between its two corners. */
	src_rect.x = x0;
	src_rect.y = y0;
	src_rect.w = (uint32_t)(x1 - x0);
	src_rect.h = (uint32_t)(y1 - y0);

	/* Takes the destination corners the same way: mirroring on both sides cancels. */
	x0 = region->dstOffsets[0].x;
	y0 = region->dstOffsets[0].y;
	x1 = region->dstOffsets[1].x;
	y1 = region->dstOffsets[1].y;
	linear ^= i915_blit_order(&x0, &x1, I915_GFX_RECT_MIRROR_X);
	linear ^= i915_blit_order(&y0, &y1, I915_GFX_RECT_MIRROR_Y);
	if (x1 == x0 || y1 == y0)
		return 0;

	/* Takes the destination rectangle between its two corners. */
	dst_rect.x = x0;
	dst_rect.y = y0;
	dst_rect.w = (uint32_t)(x1 - x0);
	dst_rect.h = (uint32_t)(y1 - y0);

	/* A linear filter samples bilinearly; any other filter samples the nearest texel. */
	if (op->u.blit.filter == VK_FILTER_LINEAR)
		linear |= I915_GFX_RECT_LINEAR;

	/* The planes each side's aspects name. */
	src_planes = i915_image_planes(op->u.blit.src, region->srcSubresource.aspectMask);
	dst_planes = i915_image_planes(op->u.blit.dst, region->dstSubresource.aspectMask);

	/* Blits the colour or depth texels when both sides name them. */
	if ((src_planes & dst_planes & I915_IMAGE_PLANE_MAIN) != 0U) {
		error = i915_image_blit_plane(session, op, I915_IMAGE_PLANE_MAIN, &src_rect, &dst_rect, linear);
		if (error != 0)
			return error;
	}

	/* Blits the stencil plane when both sides name it. */
	if ((src_planes & dst_planes & I915_IMAGE_PLANE_STENCIL) != 0U) {
		error = i915_image_blit_plane(session, op, I915_IMAGE_PLANE_STENCIL, &src_rect, &dst_rect, linear);
		if (error != 0)
			return error;
	}

	/* Succeeded: the region is blitted. */
	return 0;
}

/*
 * Blits one plane of a vkCmdBlitImage region, scaled, layer by layer
 * (a 3D image's depth is not scaled: XXX, its first depth only).
 */
static int
i915_image_blit_plane(
	struct i915_render_session *session,
	const struct i915_gfx_op *op,
	uint32_t plane,
	const struct i915_gfx_rect *src_rect,
	const struct i915_gfx_rect *dst_rect,
	int linear)
{
	const VkImageBlit *region;
	struct i915_gfx_surface src_surface;
	struct i915_gfx_surface dst_surface;
	uint32_t layer;
	int error;

	/* Copies each layer the source names into the matching destination layer. */
	region = &op->u.blit.region;
	for (layer = 0U; layer < region->srcSubresource.layerCount; layer++) {
		/* Describes the source level of the layer. */
		error = i915_image_plane_surface(op->u.blit.src, plane, region->srcSubresource.mipLevel,
						 region->srcSubresource.baseArrayLayer + layer, &src_surface);
		if (error != 0)
			return EINVAL;

		/* Describes the destination level of the layer. */
		error = i915_image_plane_surface(op->u.blit.dst, plane, region->dstSubresource.mipLevel,
						 region->dstSubresource.baseArrayLayer + layer, &dst_surface);
		if (error != 0)
			return EINVAL;

		/* Copies the layer. */
		error = drv_i915_gfx_rect(session, &dst_surface, dst_rect, &src_surface, src_rect, NULL, linear);
		if (error != 0)
			return error;
	}

	/* Succeeded: the plane of the region is blitted. */
	return 0;
}

/*
 * Runs a vkCmdResolveImage region: each pixel of the destination level's
 * rectangle becomes the mean of the source's samples at the matching pixel
 * (a colour image's samples are slices of their own, slice_rows apart).
 * Returns EINVAL for a source of one sample, a destination of several, or
 * a region the images do not have.
 */
static int
i915_execute_image_resolve(
	struct i915_render_session *session,
	const struct i915_gfx_op *op)
{
	const VkImageCopy *region;
	const struct i915_gfx_image *src;
	struct i915_gfx_surface samples[4];
	struct i915_gfx_surface dst_surface;
	struct i915_gfx_rect src_rect;
	struct i915_gfx_rect dst_rect;
	uint32_t sample;
	int error;

	/* Refuses a resolve whose images do not exist. */
	region = &op->u.image_copy.region;
	src = op->u.image_copy.src;
	if (src == NULL || op->u.image_copy.dst == NULL)
		return EINVAL;

	/* Refuses a source of one sample, and a destination of several. */
	if (src->samples <= 1U || op->u.image_copy.dst->samples > 1U)
		return EINVAL;

	/* Describes the source's sample 0 and the destination's level and layer. */
	error = i915_image_surface(src, 0U, 0U, &samples[0]);
	if (error != 0)
		return EINVAL;
	error = i915_image_surface(op->u.image_copy.dst, region->dstSubresource.mipLevel, region->dstSubresource.baseArrayLayer, &dst_surface);
	if (error != 0)
		return EINVAL;

	/* Every further sample is the slice below the one before; a two-sample image names each twice. */
	for (sample = 1U; sample < 4U; sample++) {
		samples[sample] = samples[0];
		samples[sample].va += (uint64_t)(sample % src->samples) * src->slice_rows * src->pitch;
	}

	/* The two rectangles are the extent at each side's offset. */
	src_rect.x = region->srcOffset.x;
	src_rect.y = region->srcOffset.y;
	src_rect.w = region->extent.width;
	src_rect.h = region->extent.height;
	dst_rect.x = region->dstOffset.x;
	dst_rect.y = region->dstOffset.y;
	dst_rect.w = region->extent.width;
	dst_rect.h = region->extent.height;

	/* An empty region resolves nothing. */
	if (region->extent.width == 0U || region->extent.height == 0U)
		return 0;

	/* Resolves the rectangle. */
	error = drv_i915_gfx_resolve(session, &dst_surface, &dst_rect, samples, &src_rect);
	if (error != 0)
		return error;

	/* Succeeded: the region is resolved. */
	return 0;
}

/*
 * Fills a rectangle of an image's surface with four words, and in a
 * multisampled colour image the same rectangle of every further sample,
 * each a slice of its own slice_rows below the one before.
 */
static int
i915_fill_samples(
	struct i915_render_session *session,
	const struct i915_gfx_image *image,
	const struct i915_gfx_surface *surface,
	const struct i915_gfx_rect *rect,
	const uint32_t words[4])
{
	struct i915_gfx_surface sample_surface;
	uint32_t sample;
	int error;

	/* Fills the surface itself: sample 0, or the one sample. */
	error = drv_i915_gfx_rect(session, surface, rect, NULL, NULL, words, 0);
	if (error != 0)
		return error;

	/* An image of one sample has no other. */
	if (image->samples <= 1U)
		return 0;

	/* A depth or stencil plane interleaves its samples: the surface already holds them all. */
	if (image->stencil != 0U || image->format == VK_FORMAT_D32_SFLOAT || image->format == VK_FORMAT_D16_UNORM)
		return 0;

	/* Fills the other samples of the multisampled colour image. */
	if (surface->tiled != 0U) {
		for (sample = 1U; sample < image->samples; sample++) {
			sample_surface = *surface;
			sample_surface.va += (uint64_t)sample * image->slice_rows * image->pitch;
			error = drv_i915_gfx_rect(session, &sample_surface, rect, NULL, NULL, words, 0);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: every sample of the rectangle is filled. */
	return 0;
}

/*
 * Turns a rectangle of pixels into the rectangle of samples that holds
 * them in a multisampled depth or stencil plane: two samples side by side
 * double its width, four in a square double both sides.  A rectangle of
 * any other image is left as it is.
 */
static void
i915_sample_rect(
	const struct i915_gfx_image *image,
	struct i915_gfx_rect *rect)
{
	/* One sample to a pixel: the pixels are the samples. */
	if (image->samples <= 1U)
		return;

	/* Two or four samples: two side by side. */
	rect->x *= 2;
	rect->w *= 2U;

	/* Four samples: two rows of two. */
	if (image->samples == 4U) {
		rect->y *= 2;
		rect->h *= 2U;
	}
}

/*
 * Runs one vkCmdCopyBuffer region as GPU copies.
 *
 * The bytes are copied as texels between two linear surfaces over the
 * buffers: full rows of 4096 texels, at most 4096 rows to a rectangle, then
 * the rest as one short row.  A texel is four bytes (R8G8B8A8_UNORM) when
 * both offsets and the size are multiples of four, else two (R8G8_UNORM)
 * when they are even, else one (R8_UNORM): vkCmdCopyBuffer asks no
 * alignment, and a linear surface needs its address and pitch only in whole
 * texels.  An UNORM texel read and written back keeps its bytes exactly.
 */
static int
i915_execute_buffer_copy(
	struct i915_render_session *session,
	const struct i915_gfx_op *op)
{
	const VkBufferCopy *region;
	struct i915_gfx_buffer *src;
	struct i915_gfx_buffer *dst;
	struct i915_gfx_surface src_surface;
	struct i915_gfx_surface dst_surface;
	struct i915_gfx_rect rect;
	uint64_t texels;
	uint64_t done;
	uint64_t rows;
	uint32_t width;
	uint32_t texel_bytes;
	uint32_t format;
	int error;

	/* Refuses a copy whose buffers do not exist. */
	src = op->u.buffer_copy.src;
	dst = op->u.buffer_copy.dst;
	region = &op->u.buffer_copy.region;
	if (src == NULL || dst == NULL)
		return EINVAL;

	/* An empty region copies nothing. */
	if (region->size == 0U)
		return 0;

	/* Refuses a region that starts past the end of the source or runs past it. */
	if (region->srcOffset > src->size || region->size > src->size - region->srcOffset)
		return EINVAL;

	/* Refuses a region that starts past the end of the destination or runs past it. */
	if (region->dstOffset > dst->size || region->size > dst->size - region->dstOffset)
		return EINVAL;

	/* The largest texel that the offsets and the size are whole multiples of. */
	texel_bytes = 1U;
	format = VK_FORMAT_R8_UNORM;
	if ((region->srcOffset % 4U) == 0U &&
	    (region->dstOffset % 4U) == 0U &&
	    (region->size % 4U) == 0U) {
		texel_bytes = 4U;
		format = VK_FORMAT_R8G8B8A8_UNORM;
	} else if ((region->srcOffset % 2U) == 0U &&
		   (region->dstOffset % 2U) == 0U &&
		   (region->size % 2U) == 0U) {
		texel_bytes = 2U;
		format = VK_FORMAT_R8G8_UNORM;
	}

	/* Copies the region one rectangle at a time. */
	texels = region->size / texel_bytes;
	done = 0U;
	while (done < texels) {
		/* Takes as many full rows as one rectangle covers, or the short row that is left. */
		if (texels - done >= I915_GFX_COPY_ROW_TEXELS) {
			width = I915_GFX_COPY_ROW_TEXELS;
			rows = (texels - done) / I915_GFX_COPY_ROW_TEXELS;
			if (rows > I915_GFX_COPY_MAX_ROWS)
				rows = I915_GFX_COPY_MAX_ROWS;
		} else {
			width = (uint32_t)(texels - done);
			rows = 1U;
		}

		/* Describes the source bytes as a linear surface of those texels. */
		src_surface.va = drv_i915_gfx_memory_va(src->memory, src->offset + region->srcOffset + done * texel_bytes);
		src_surface.width = width;
		src_surface.height = (uint32_t)rows;
		src_surface.pitch = width * texel_bytes;
		src_surface.format = format;
		src_surface.tiled = 0U;
		if (src_surface.va == 0U)
			return EINVAL;

		/* Describes the destination bytes the same way. */
		dst_surface = src_surface;
		dst_surface.va = drv_i915_gfx_memory_va(dst->memory, dst->offset + region->dstOffset + done * texel_bytes);
		if (dst_surface.va == 0U)
			return EINVAL;

		/* The whole of both surfaces is the rectangle. */
		rect.x = 0;
		rect.y = 0;
		rect.w = width;
		rect.h = (uint32_t)rows;

		/* Copies the rectangle, unscaled. */
		error = drv_i915_gfx_rect(session, &dst_surface, &rect, &src_surface, &rect, NULL, 0);
		if (error != 0)
			return error;

		/* Moves past the texels the rectangle copied. */
		done += (uint64_t)width * rows;
	}

	/* Succeeded: the region is copied. */
	return 0;
}

/*
 * Runs a recorded vkCmdDraw or vkCmdDrawIndexed with what is bound.
 */
static int
i915_execute_draw(
	struct i915_render_session *session,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_op *op)
{
	struct i915_gfx_draw_args args;
	int error;

	/* Describes the draw: its counts and firsts, and for an indexed draw the vertex offset. */
	kern_memset(&args, 0, sizeof(args));
	if (op->kind == I915_GFX_OP_DRAW_INDEXED) {
		args.indexed = 1;
		args.count = op->u.draw_indexed.index_count;
		args.instance_count = op->u.draw_indexed.instance_count;
		args.first = op->u.draw_indexed.first_index;
		args.vertex_offset = op->u.draw_indexed.vertex_offset;
		args.first_instance = op->u.draw_indexed.first_instance;
	} else {
		args.count = op->u.draw.vertex_count;
		args.instance_count = op->u.draw.instance_count;
		args.first = op->u.draw.first_vertex;
		args.first_instance = op->u.draw.first_instance;
	}

	/* Records the draw into the batch. */
	error = drv_i915_gfx_draw(session, state, &args);
	if (error != 0)
		return error;

	/* Succeeded: the draw is recorded. */
	return 0;
}

/*
 * Records the operation list of one command buffer, in order, into the
 * submission's batch.
 *
 * Binds are tracked in a draw state that lives for this execution; clears,
 * copies and draws are appended to the batch, which runs when the
 * submission ends (or earlier, when it is full).  The first failure stops
 * the list and is logged with where it stopped.
 */
static int
i915_command_buffer_execute(
	struct i915_render_session *session,
	struct i915_gfx_cmdbuf *cmdbuf)
{
	struct i915_gfx_draw_state state;
	const struct i915_gfx_op *op;
	uint32_t index;
	unsigned kind;
	int error;

	/* Starts with nothing bound. */
	kern_memset(&state, 0, sizeof(state));
	error = 0;

	/* Runs the operations until one fails. */
	for (index = 0U; index < cmdbuf->op_count && error == 0; index++) {
		op = &cmdbuf->ops[index];

		/* Runs or tracks the operation by its kind. */
		switch (op->kind) {
		case I915_GFX_OP_COPY_BUFFER_TO_IMAGE:
		case I915_GFX_OP_COPY_IMAGE_TO_BUFFER:
			error = i915_execute_buffer_image_copy(session, op);
			break;
		case I915_GFX_OP_BEGIN_PASS:
			state.pass = op->u.begin.pass;
			state.framebuffer = op->u.begin.framebuffer;
			error = i915_execute_clear(session, op);
			break;
		case I915_GFX_OP_COPY_IMAGE:
			error = i915_execute_image_copy(session, op);
			break;
		case I915_GFX_OP_BLIT_IMAGE:
			error = i915_execute_image_blit(session, op);
			break;
		case I915_GFX_OP_RESOLVE_IMAGE:
			error = i915_execute_image_resolve(session, op);
			break;
		case I915_GFX_OP_CLEAR_IMAGE:
			error = i915_execute_clear_image(session, op);
			break;
		case I915_GFX_OP_END_PASS:
			state.pass = NULL;
			state.framebuffer = NULL;
			break;
		case I915_GFX_OP_BIND_PIPELINE:
			i915_execute_bind_pipeline(&state, op);
			break;
		case I915_GFX_OP_BIND_VERTEX_BUFFER:
			/* A binding past the tracked ones is ignored. */
			if (op->u.vertex.binding < I915_GFX_MAX_VERTEX_BINDINGS) {
				state.vertex[op->u.vertex.binding].buffer = op->u.vertex.buffer;
				state.vertex[op->u.vertex.binding].offset = op->u.vertex.offset;
			}

			break;
		case I915_GFX_OP_BIND_DESCRIPTOR_SET:
			i915_execute_bind_set(&state, op);
			break;
		case I915_GFX_OP_PUSH_CONSTANTS:
			kern_memcpy(state.push + op->u.push.offset, op->u.push.bytes, op->u.push.size);
			break;
		case I915_GFX_OP_CLEAR_ATTACHMENT:
			error = i915_execute_clear_attachment(session, &state, op);
			break;
		case I915_GFX_OP_DRAW:
		case I915_GFX_OP_DRAW_INDEXED:
			error = i915_execute_draw(session, &state, op);
			break;
		case I915_GFX_OP_BIND_INDEX_BUFFER:
			state.index.buffer = op->u.index.buffer;
			state.index.offset = op->u.index.offset;
			state.index.type = op->u.index.type;
			break;
		case I915_GFX_OP_SET_VIEWPORT:
			kern_memcpy(state.viewport, op->u.viewport, sizeof(state.viewport));
			state.viewport_set = 1;
			break;
		case I915_GFX_OP_SET_SCISSOR:
			state.scissor = op->u.scissor;
			state.scissor_set = 1;
			break;
		case I915_GFX_OP_SET_BLEND_CONSTANTS:
			kern_memcpy(state.blend_constants, op->u.blend_constants, sizeof(state.blend_constants));
			state.blend_constants_set = 1;
			break;
		case I915_GFX_OP_COPY_BUFFER:
			error = i915_execute_buffer_copy(session, op);
			break;
		case I915_GFX_OP_QUERY_BEGIN:
		case I915_GFX_OP_QUERY_END:
		case I915_GFX_OP_QUERY_RESET:
			error = drv_i915_gfx_query_execute(session, op);
			break;
		case I915_GFX_OP_SET_STENCIL:
			i915_execute_set_stencil(&state, op);
			break;
		case I915_GFX_OP_DISPATCH:
			error = i915_execute_dispatch(session, &state, op);
			break;
		case I915_GFX_OP_DISPATCH_INDIRECT:
			error = i915_execute_dispatch_indirect(session, &state, op);
			break;
		case I915_GFX_OP_VIDEO_BEGIN:
		case I915_GFX_OP_VIDEO_CONTROL:
		case I915_GFX_OP_VIDEO_DECODE:
		case I915_GFX_OP_VIDEO_END:
			/* A video command on the graphics family breaks the API's rules: the submission is lost (D18). */
			kern_logf("i915: vk: video command submitted on the graphics queue family\n");
			error = EIO;
			break;
		default:
			error = EINVAL;
			break;
		}
	}

	/*
	 * Says where the list stopped.  The index has already moved past the
	 * failed operation, so the kind logged is the failed one's.
	 */
	if (error != 0) {
		kind = 0U;
		if (index != 0U)
			kind = (unsigned)cmdbuf->ops[index - 1U].kind;
		kern_logf("i915: vk: command buffer stopped at operation %u of %u (kind %u): error %d\n",
			  index,
			  cmdbuf->op_count,
			  kind,
			  error);
		return error;
	}

	/* Succeeded: every operation is recorded. */
	return 0;
}

/*
 * vkQueueSubmit (libvulkan queue.c queue_write_submit): [queue][present]
 * [count]{[sType][pNext][present][count]{wait}[count]{stage}[present]
 * [count]{buffer}[present][count]{signal}}[fence] -> [result].
 *
 * The command buffers are recorded in submission order into one batch,
 * which runs to its end before the reply; the fence is signalled once all
 * of them succeeded.
 * XXX: the waits and signals are decoded and not acted on: every earlier
 * submission has finished, so there is nothing to wait for.
 */
static int
i915_queue_submit(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_gfx_cmdbuf *cmdbufs[I915_GFX_MAX_SUBMITTED];
	const struct i915_gfx_op *lists[I915_GFX_MAX_SUBMITTED];
	uint32_t counts[I915_GFX_MAX_SUBMITTED];
	struct i915_gfx_cmdbuf *cmdbuf;
	struct i915_vk_fence *fence;
	uint64_t identity;
	uint32_t family;
	uint64_t submits;
	uint64_t submit;
	uint64_t count;
	uint64_t item;
	uint32_t total;
	uint32_t index;
	uint32_t result;
	int end_error;
	int error;

	/* Decodes the queue, whose family decides where the submission runs, and the number of submissions, at most eight. */
	identity = drv_i915_wire_read_u64(reader);
	family = drv_i915_render_queue_family(session, identity);
	(void)drv_i915_wire_read_u32(reader);
	submits = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || submits > I915_GFX_MAX_SUBMITS)
		return EINVAL;

	/* Collects the command buffers of every submission, in order. */
	total = 0U;
	for (submit = 0U; submit < submits; submit++) {
		/* Decodes the submission's sType and pNext. */
		(void)drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u64(reader);

		/* Decodes the wait semaphores. */
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		for (item = 0U; reader->error == 0 && item < count; item++)
			(void)drv_i915_wire_read_u64(reader);

		/* Decodes their stages. */
		count = drv_i915_wire_read_u64(reader);
		for (item = 0U; reader->error == 0 && item < count; item++)
			(void)drv_i915_wire_read_u32(reader);

		/* Decodes the number of command buffers, at most thirty-two. */
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || count > I915_GFX_MAX_SUBMITTED)
			return EINVAL;

		/* Keeps every known command buffer while there is room. */
		for (item = 0U; item < count; item++) {
			identity = drv_i915_wire_read_u64(reader);
			cmdbuf = drv_i915_object_lookup(session, I915_VK_OBJ_COMMAND_BUFFER, identity);

			/* A buffer whose objects were destroyed (BUG-260) does not run. */
			if (cmdbuf != NULL && cmdbuf->stale) {
				kern_logf("i915: vk: vkQueueSubmit: a command buffer whose objects were destroyed is not run\n");
				cmdbuf = NULL;
			}

			/* A known one is kept while there is room. */
			if (cmdbuf != NULL && total < I915_GFX_MAX_SUBMITTED) {
				cmdbufs[total] = cmdbuf;
				total++;
			}
		}

		/* Decodes the signal semaphores. */
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		for (item = 0U; reader->error == 0 && item < count; item++)
			(void)drv_i915_wire_read_u64(reader);
	}

	/* Decodes the fence. */
	identity = drv_i915_wire_read_u64(reader);
	fence = drv_i915_object_lookup(session, I915_VK_OBJ_FENCE, identity);
	if (reader->error != 0)
		return EINVAL;

	/*
	 * A submission on the video decode family runs in the video part, with
	 * the render batch open for its query resets (ws083-p003b).
	 */
	if (family == 1U) {
		for (index = 0U; index < total; index++) {
			lists[index] = cmdbufs[index]->ops;
			counts[index] = cmdbufs[index]->op_count;
		}
		error = drv_i915_gfx_submit_begin(session);
		result = (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
		if (error == 0)
			result = drv_i915_video_submit(session, lists, counts, total);
		end_error = drv_i915_gfx_submit_end(session);
		if (result == 0U && end_error != 0)
			result = i915_command_result(end_error);
		if (result == 0U && fence != NULL)
			drv_i915_fence_signal(fence);
		drv_i915_wire_reply_u32(reply, result);
		return 0;
	}

	/* Opens the batch the command buffers are recorded into. */
	error = drv_i915_gfx_submit_begin(session);

	/* Records the command buffers in order until one fails. */
	for (index = 0U; index < total && error == 0; index++)
		error = i915_command_buffer_execute(session, cmdbufs[index]);

	/*
	 * Runs what was recorded, also after a failure, so every operation
	 * before the failed one has run, as it would have alone; the first
	 * failure is the submission's.
	 */
	end_error = drv_i915_gfx_submit_end(session);
	if (error == 0)
		error = end_error;

	/* Signals the fence: everything the submission asked for has been done by now. */
	if (error == 0 && fence != NULL) {
		drv_i915_fence_signal(fence);
	}

	/* Replies with the submission's result. */
	result = i915_command_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the command was decoded; the reply carries the submission's result. */
	return 0;
}

/* Keeps a dynamic stencil mask or reference for the faces it names (bit 0 front, bit 1 back). */
static void
i915_execute_set_stencil(
	struct i915_gfx_draw_state *state,
	const struct i915_gfx_op *op)
{
	uint32_t *values;
	uint32_t face;

	/* The array the value goes into. */
	values = state->stencil_compare_mask;
	if (op->u.stencil.which == 1U)
		values = state->stencil_write_mask;
	if (op->u.stencil.which == 2U)
		values = state->stencil_reference;

	/* Each face named. */
	for (face = 0U; face < 2U; face++) {
		if ((op->u.stencil.faces & (1U << face)) != 0U)
			values[face] = op->u.stencil.value;
	}
}

/*
 * Binds a pipeline at its own bind point: a compute pipeline replaces the
 * bound compute pipeline and leaves the graphics one bound, as Vulkan keeps
 * the two apart (ws101-p003).
 */
static void
i915_execute_bind_pipeline(
	struct i915_gfx_draw_state *state,
	const struct i915_gfx_op *op)
{
	/* A compute pipeline goes to the compute bind point. */
	if (op->u.pipeline != NULL && op->u.pipeline->bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) {
		state->compute_pipeline = op->u.pipeline;
		return;
	}

	/* A graphics pipeline, or an unknown one, which unbinds the graphics pipeline as it always has. */
	state->pipeline = op->u.pipeline;
}

/*
 * Binds a descriptor set at the bind point of its recording, with the
 * dynamic offsets of its buffers; a set past the tracked ones is ignored.
 */
static void
i915_execute_bind_set(
	struct i915_gfx_draw_state *state,
	const struct i915_gfx_op *op)
{
	uint32_t set;

	/* A set past the tracked ones is ignored. */
	set = op->u.descriptor.set;
	if (set >= I915_GFX_MAX_SETS)
		return;

	/* The compute bind point has its own sets (ws101-p003). */
	if (op->u.descriptor.bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) {
		state->compute_dset[set] = op->u.descriptor.dset;
		state->compute_dynamic_count[set] = op->u.descriptor.dynamic_count;
		kern_memcpy(state->compute_dynamic_bindings[set],
			    op->u.descriptor.dynamic_bindings,
			    sizeof(state->compute_dynamic_bindings[set]));
		kern_memcpy(state->compute_dynamic_offsets[set],
			    op->u.descriptor.dynamic_offsets,
			    sizeof(state->compute_dynamic_offsets[set]));
		return;
	}

	/* Any other bind point is the graphics one; a bound set brings its dynamic offsets. */
	state->dset[set] = op->u.descriptor.dset;
	state->dynamic_count[set] = op->u.descriptor.dynamic_count;
	kern_memcpy(state->dynamic_bindings[set],
		    op->u.descriptor.dynamic_bindings,
		    sizeof(state->dynamic_bindings[set]));
	kern_memcpy(state->dynamic_offsets[set],
		    op->u.descriptor.dynamic_offsets,
		    sizeof(state->dynamic_offsets[set]));
}

/*
 * Runs a recorded vkCmdDispatch: the bound compute pipeline over the
 * groups (ws101-p003, ws101-p004).
 *
 * A dispatch needs a prepared compute pipeline; a dispatch of no group does
 * nothing.  Returns EINVAL for a dispatch without a pipeline or past the
 * device's group counts, or the error of recording or running it.
 */
static int
i915_execute_dispatch(
	struct i915_render_session *session,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_op *op)
{
	struct i915_gfx_grid grid;
	const uint32_t *groups;
	int error;

	/* Refuses a dispatch with no prepared compute pipeline bound. */
	error = i915_dispatch_ready(state);
	if (error != 0)
		return error;

	/* Refuses group counts past the device's (maxComputeWorkGroupCount). */
	groups = op->u.dispatch.groups;
	if (groups[0] > I915_GFX_MAX_GROUP_COUNT ||
	    groups[1] > I915_GFX_MAX_GROUP_COUNT ||
	    groups[2] > I915_GFX_MAX_GROUP_COUNT)
		return EINVAL;

	/* A dispatch of no group runs nothing. */
	if (groups[0] == 0U || groups[1] == 0U || groups[2] == 0U)
		return 0;

	/* Records the dispatch of the counts into the submission's batch. */
	kern_memset(&grid, 0, sizeof(grid));
	kern_memcpy(grid.groups, groups, sizeof(grid.groups));
	error = drv_i915_gfx_dispatch(session, state, &grid);
	if (error != 0)
		return error;

	/* Succeeded: the dispatch is recorded. */
	return 0;
}

/*
 * Runs a recorded vkCmdDispatchIndirect (ws101-p007): the bound compute
 * pipeline over the three group counts the buffer holds at the offset,
 * which the GPU reads when the dispatch runs (an earlier operation of the
 * submission may write them).
 *
 * Counts of zero run nothing and counts past the device's are the
 * application's error (Vulkan leaves them undefined); neither is known
 * here.  Returns EINVAL for a dispatch without a pipeline, or of a buffer
 * that is not bound, an offset that is not a word's or three counts that
 * do not fit the buffer, or the error of recording or running it.
 */
static int
i915_execute_dispatch_indirect(
	struct i915_render_session *session,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_op *op)
{
	struct i915_gfx_buffer *buffer;
	struct i915_gfx_grid grid;
	uint64_t offset;
	int error;

	/* Refuses a dispatch with no prepared compute pipeline bound. */
	error = i915_dispatch_ready(state);
	if (error != 0)
		return error;

	/* Refuses a buffer that is not bound to memory. */
	buffer = op->u.dispatch_indirect.buffer;
	offset = op->u.dispatch_indirect.offset;
	if (buffer == NULL || buffer->memory == NULL) {
		kern_logf("i915: vk: indirect dispatch refused: no bound buffer\n");
		return EINVAL;
	}

	/* Refuses an offset that is not a word's, or counts past the buffer's end. */
	if ((offset % 4U) != 0U || offset > buffer->size || buffer->size - offset < 12U) {
		kern_logf("i915: vk: indirect dispatch refused: offset %u in a buffer of %u bytes\n",
			  (unsigned)offset,
			  (unsigned)buffer->size);
		return EINVAL;
	}

	/* Resolves the counts to their GPU address. */
	kern_memset(&grid, 0, sizeof(grid));
	grid.indirect_va = drv_i915_gfx_memory_va(buffer->memory, buffer->offset + offset);
	if (grid.indirect_va == 0U)
		return EINVAL;

	/* Records the dispatch of the buffer's counts into the submission's batch. */
	error = drv_i915_gfx_dispatch(session, state, &grid);
	if (error != 0)
		return error;

	/* Succeeded: the dispatch is recorded. */
	return 0;
}

/* Reports whether a prepared compute pipeline is bound: 0, or EINVAL (logged) when none is. */
static int
i915_dispatch_ready(
	const struct i915_gfx_draw_state *state)
{
	/* A pipeline whose kernel was compiled. */
	if (state->compute_pipeline == NULL || state->compute_pipeline->kernels_ready == 0) {
		kern_logf("i915: vk: dispatch refused: no compute pipeline is bound\n");
		return EINVAL;
	}

	/* Succeeded: the dispatch can run. */
	return 0;
}
