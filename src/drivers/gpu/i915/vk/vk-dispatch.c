/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The opcode router of the Vulkan executor (see vk-dispatch.h).
 *
 * The graphics objects are asked first and the command recording second;
 * an opcode neither owns is routed by its range.  The transport commands
 * and the instance, device and queue commands are tried last, and anything
 * else is refused.
 *
 * A command carries no length word, so the end of a command no part
 * understood cannot be found.  A refused command therefore poisons the
 * reader: nothing further in the stream is decoded or executed, and the
 * submission fails.
 */

#include "vk-dispatch.h"
#include "vk-fence.h"
#include "vk-codec.h"
#include "vk-gfx.h"
#include "vk-instance.h"
#include "vk-transport.h"
#include "vk-video.h"

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stdint.h>

static enum i915_vk_object_kind i915_dispatch_route(uint32_t opcode);
static int i915_dispatch_builtin(struct i915_render_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_dispatch_unported(uint32_t opcode, const char *module, struct i915_wire_reader *reader);
static void i915_dispatch_refused(uint32_t opcode, int error);

/*
 * Decodes one command header and hands the command to the part that owns
 * its opcode.
 *
 * A command that asks for a reply gets the echoed opcode first; the owning
 * part then appends the VkResult and any output parameters, so the reply
 * reads back as libvulkan expects (opcode, result, payload).
 */
int
drv_i915_render_dispatch(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	enum i915_vk_object_kind route;
	uint32_t opcode;
	uint32_t reply_requested;
	int handled;
	int error;

	/* Reads the header: the opcode and the reply-request flag. */
	opcode = drv_i915_wire_read_u32(reader);
	reply_requested = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Opens the reply of a command that asks for one with its opcode. */
	if (reply_requested != 0U)
		drv_i915_wire_reply_u32(reply, opcode);

	/* Offers the command to the graphics objects, then to the command recording. */
	error = drv_i915_gfx_obj_dispatch(session, opcode, reader, reply, &handled);
	if (handled == 0)
		error = drv_i915_gfx_rec_dispatch(session, opcode, reader, reply, &handled);

	/* A command the graphics path owned is finished, whatever it reported. */
	if (handled != 0) {
		/* Reports why the graphics path refused the command (the first few are logged). */
		if (error != 0) {
			i915_dispatch_refused(opcode, error);
			return error;
		}

		/* Succeeded: the graphics path executed the command. */
		return 0;
	}

	/*
	 * Routes the rest by opcode range; NONE means the router's own commands.
	 *
	 * XXX: the object, pipeline, command buffer, sync and WSI modules the
	 * ranges name are not ported.  An opcode that reaches one of them is
	 * refused, not accepted as an empty success.
	 */
	route = i915_dispatch_route(opcode);
	switch (route) {
	case I915_VK_OBJ_MEMORY:
		error = i915_dispatch_unported(opcode, "res", reader);
		break;
	case I915_VK_OBJ_PIPELINE:
		error = i915_dispatch_unported(opcode, "pipe", reader);
		break;
	case I915_VK_OBJ_COMMAND_BUFFER:
		error = i915_dispatch_unported(opcode, "cmdbuf", reader);
		break;
	case I915_VK_OBJ_FENCE:
		error = drv_i915_render_fence_dispatch(session, opcode, reader, reply);
		break;
	case I915_VK_OBJ_SWAPCHAIN:
		error = i915_dispatch_unported(opcode, "wsi", reader);
		break;
	case I915_VK_OBJ_VIDEO_SESSION:
		error = drv_i915_video_dispatch(session, opcode, reader, reply);
		break;
	default:
		error = i915_dispatch_builtin(session, opcode, reader, reply);
		break;
	}

	/* Reports why the command was refused. */
	if (error != 0) {
		i915_dispatch_refused(opcode, error);
		return error;
	}

	/* Succeeded: the command was decoded and executed. */
	return 0;
}

/* Logs a refused command, the first 32 of them (a refusal fails the whole stream). */
static void
i915_dispatch_refused(
	uint32_t opcode,
	int error)
{
	static unsigned said;

	/* Only the first ones. */
	if (said >= 32U)
		return;
	said++;
	kern_logf("i915: vk: command refused at opcode %u: error %d\n", opcode, error);
}

/* Maps an opcode to the object kind that names the module its range belongs to. */
static enum i915_vk_object_kind
i915_dispatch_route(
	uint32_t opcode)
{
	/* Fences, semaphores, events and queries belong to sync. */
	if (opcode >= GPU_OP_CREATE_FENCE && opcode <= GPU_OP_GET_QUERY_POOL_RESULTS)
		return I915_VK_OBJ_FENCE;

	/* Memory, buffer, image, sampler and descriptor objects belong to res. */
	if (opcode >= GPU_OP_ALLOCATE_MEMORY && opcode <= GPU_OP_QUEUE_BIND_SPARSE)
		return I915_VK_OBJ_MEMORY;
	if (opcode >= GPU_OP_CREATE_BUFFER && opcode <= GPU_OP_DESTROY_IMAGE_VIEW)
		return I915_VK_OBJ_MEMORY;
	if (opcode >= GPU_OP_CREATE_SAMPLER && opcode <= GPU_OP_UPDATE_DESCRIPTOR_SETS)
		return I915_VK_OBJ_MEMORY;

	/* Shader modules, pipelines, framebuffers and render passes belong to pipe. */
	if (opcode >= GPU_OP_CREATE_SHADER_MODULE && opcode <= GPU_OP_DESTROY_PIPELINE_LAYOUT)
		return I915_VK_OBJ_PIPELINE;
	if (opcode >= GPU_OP_CREATE_FRAMEBUFFER && opcode <= GPU_OP_GET_RENDER_AREA_GRANULARITY)
		return I915_VK_OBJ_PIPELINE;

	/* vkQueueSubmit (18), command pools, command buffers and every vkCmd* belong to cmdbuf. */
	if (opcode == GPU_OP_QUEUE_SUBMIT)
		return I915_VK_OBJ_COMMAND_BUFFER;
	if (opcode >= GPU_OP_CREATE_COMMAND_POOL && opcode <= GPU_OP_CMD_EXECUTE_COMMANDS)
		return I915_VK_OBJ_COMMAND_BUFFER;

	/* zedBSD's video queries, sessions and parameters belong to video (the video recordings to cmdbuf). */
	if (opcode >= GPU_OP_OWN_FIRST && opcode <= I915_VK_VIDEO_OP_LAST_ROUTED)
		return I915_VK_OBJ_VIDEO_SESSION;

	/* Everything else (instance, device, queue, version, transport) is the router's own. */
	return I915_VK_OBJ_NONE;
}

/* Hands a command to the transport, then to the instance part, and refuses what neither owns. */
static int
i915_dispatch_builtin(
	struct i915_render_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	int handled;
	int error;

	/* Offers the command to the transport: reply select and seek, the version probe, the external stream. */
	error = drv_i915_render_transport_dispatch(session, opcode, reader, reply, &handled);
	if (handled != 0) {
		/* Reports why the transport refused the command. */
		if (error != 0)
			return error;

		/* Succeeded: the transport executed the command. */
		return 0;
	}

	/* Offers the command to the instance, physical-device, device and queue part. */
	error = drv_i915_render_instance_dispatch(session, opcode, reader, reply, &handled);
	if (handled != 0) {
		/* Reports why the instance part refused the command. */
		if (error != 0)
			return error;

		/* Succeeded: the instance part executed the command. */
		return 0;
	}

	/*
	 * Refuses every other opcode.  The command's end cannot be located, so
	 * the reader is poisoned and the stream stops here.  The echoed opcode
	 * already written for a reply-requested command is withdrawn by the
	 * caller: the reply length is only published on success.
	 */
	kern_logf("i915: vk: XXX unimplemented opcode %u (builtin)\n", opcode);
	reader->error = 1;
	return ENOTSUP;
}

/*
 * XXX: refuses an opcode whose range belongs to a module that is not
 * ported, poisoning the reader because the command's end cannot be located.
 */
static int
i915_dispatch_unported(
	uint32_t opcode,
	const char *module,
	struct i915_wire_reader *reader)
{
	/* Names the module the opcode would have reached, and stops the stream. */
	kern_logf("i915: XXX opcode %u routed to the %s module, which is not ported; refused\n", opcode, module);
	reader->error = 1;
	return ENOTSUP;
}
