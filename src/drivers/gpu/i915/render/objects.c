/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The routing of the graphics path's object commands.
 *
 * Each object command is owned by the file of its object: memory and
 * buffers by memory.c, images, views and samplers by image.c, descriptors
 * by descriptor.c, layouts, shader modules and pipelines by pipeline.c,
 * render passes and framebuffers by render-pass.c, and semaphores by
 * sync.c.  The destroy of an object that owns nothing but itself is shared
 * and lives here.
 */

#include "gfx.h"
#include "codec.h"
#include "descriptor.h"
#include "image.h"
#include "internal.h"
#include "memory.h"
#include "object.h"
#include "pipeline.h"
#include "render-pass.h"
#include "sync.h"

#include <kern/kmem.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* The wire opcodes of the object commands, as libvulkan numbers them. */
#define I915_VK_COMMAND_ALLOCATE_MEMORY			21U
#define I915_VK_COMMAND_FREE_MEMORY			22U
#define I915_VK_COMMAND_BIND_BUFFER_MEMORY		28U
#define I915_VK_COMMAND_BIND_IMAGE_MEMORY		29U
#define I915_VK_COMMAND_GET_BUFFER_MEMORY_REQUIREMENTS	30U
#define I915_VK_COMMAND_GET_IMAGE_MEMORY_REQUIREMENTS	31U
#define I915_VK_COMMAND_CREATE_SEMAPHORE		40U
#define I915_VK_COMMAND_DESTROY_SEMAPHORE		41U
#define I915_VK_COMMAND_CREATE_BUFFER			50U
#define I915_VK_COMMAND_DESTROY_BUFFER			51U
#define I915_VK_COMMAND_CREATE_IMAGE			54U
#define I915_VK_COMMAND_DESTROY_IMAGE			55U
#define I915_VK_COMMAND_GET_IMAGE_SUBRESOURCE_LAYOUT	56U
#define I915_VK_COMMAND_CREATE_IMAGE_VIEW		57U
#define I915_VK_COMMAND_DESTROY_IMAGE_VIEW		58U
#define I915_VK_COMMAND_CREATE_SHADER_MODULE		59U
#define I915_VK_COMMAND_DESTROY_SHADER_MODULE		60U
#define I915_VK_COMMAND_CREATE_GRAPHICS_PIPELINES	65U
#define I915_VK_COMMAND_DESTROY_PIPELINE		67U
#define I915_VK_COMMAND_CREATE_PIPELINE_LAYOUT		68U
#define I915_VK_COMMAND_DESTROY_PIPELINE_LAYOUT		69U
#define I915_VK_COMMAND_CREATE_SAMPLER			70U
#define I915_VK_COMMAND_DESTROY_SAMPLER			71U
#define I915_VK_COMMAND_CREATE_DESCRIPTOR_SET_LAYOUT	72U
#define I915_VK_COMMAND_DESTROY_DESCRIPTOR_SET_LAYOUT	73U
#define I915_VK_COMMAND_CREATE_DESCRIPTOR_POOL		74U
#define I915_VK_COMMAND_DESTROY_DESCRIPTOR_POOL		75U
#define I915_VK_COMMAND_ALLOCATE_DESCRIPTOR_SETS	77U
#define I915_VK_COMMAND_UPDATE_DESCRIPTOR_SETS		79U
#define I915_VK_COMMAND_CREATE_FRAMEBUFFER		80U
#define I915_VK_COMMAND_DESTROY_FRAMEBUFFER		81U
#define I915_VK_COMMAND_CREATE_RENDER_PASS		82U
#define I915_VK_COMMAND_DESTROY_RENDER_PASS		83U

static int i915_gfx_destroy_plain(struct i915_render_session *session, struct i915_wire_reader *reader, enum i915_vk_object_kind kind);

/*
 * Routes one object command to the file that owns its object.
 *
 * `handled` is set for an opcode this path owns and cleared for any other,
 * which is left to the next module without reading anything.  The return
 * value is the command's errno; a command whose VkResult is not success
 * still returns 0, because the result travels in the reply.
 */
int
drv_i915_gfx_obj_dispatch(
	struct i915_render_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Claims the opcode; the default arm gives it back. */
	*handled = 1;

	/* Runs the command the opcode names. */
	switch (opcode) {
	case I915_VK_COMMAND_ALLOCATE_MEMORY:
		error = drv_i915_gfx_allocate_memory(session, reader, reply);
		break;
	case I915_VK_COMMAND_FREE_MEMORY:
		error = drv_i915_gfx_free_memory(session, reader);
		break;
	case I915_VK_COMMAND_BIND_BUFFER_MEMORY:
		error = drv_i915_gfx_bind(session, reader, reply, 0);
		break;
	case I915_VK_COMMAND_BIND_IMAGE_MEMORY:
		error = drv_i915_gfx_bind(session, reader, reply, 1);
		break;
	case I915_VK_COMMAND_GET_BUFFER_MEMORY_REQUIREMENTS:
		error = drv_i915_gfx_requirements(session, reader, reply, 0);
		break;
	case I915_VK_COMMAND_GET_IMAGE_MEMORY_REQUIREMENTS:
		error = drv_i915_gfx_requirements(session, reader, reply, 1);
		break;
	case I915_VK_COMMAND_CREATE_SEMAPHORE:
		error = drv_i915_gfx_create_semaphore(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_SEMAPHORE:
		error = i915_gfx_destroy_plain(session, reader, I915_VK_OBJ_SEMAPHORE);
		break;
	case I915_VK_COMMAND_CREATE_BUFFER:
		error = drv_i915_gfx_create_buffer(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_BUFFER:
		error = i915_gfx_destroy_plain(session, reader, I915_VK_OBJ_BUFFER);
		break;
	case I915_VK_COMMAND_CREATE_IMAGE:
		error = drv_i915_gfx_create_image(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_IMAGE:
		error = i915_gfx_destroy_plain(session, reader, I915_VK_OBJ_IMAGE);
		break;
	case I915_VK_COMMAND_GET_IMAGE_SUBRESOURCE_LAYOUT:
		error = drv_i915_gfx_subresource_layout(session, reader, reply);
		break;
	case I915_VK_COMMAND_CREATE_IMAGE_VIEW:
		error = drv_i915_gfx_create_image_view(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_IMAGE_VIEW:
		error = i915_gfx_destroy_plain(session, reader, I915_VK_OBJ_IMAGE_VIEW);
		break;
	case I915_VK_COMMAND_CREATE_SHADER_MODULE:
		error = drv_i915_gfx_create_shader(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_SHADER_MODULE:
		error = i915_gfx_destroy_plain(session, reader, I915_VK_OBJ_SHADER_MODULE);
		break;
	case I915_VK_COMMAND_CREATE_GRAPHICS_PIPELINES:
		error = drv_i915_gfx_create_pipelines(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_PIPELINE:
		error = drv_i915_gfx_destroy_pipeline(session, reader);
		break;
	case I915_VK_COMMAND_CREATE_PIPELINE_LAYOUT:
		error = drv_i915_gfx_create_pipeline_layout(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_PIPELINE_LAYOUT:
		error = i915_gfx_destroy_plain(session, reader, I915_VK_OBJ_PIPELINE_LAYOUT);
		break;
	case I915_VK_COMMAND_CREATE_SAMPLER:
		error = drv_i915_gfx_create_sampler(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_SAMPLER:
		error = i915_gfx_destroy_plain(session, reader, I915_VK_OBJ_SAMPLER);
		break;
	case I915_VK_COMMAND_CREATE_DESCRIPTOR_SET_LAYOUT:
		error = drv_i915_gfx_create_dsl(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_DESCRIPTOR_SET_LAYOUT:
		error = i915_gfx_destroy_plain(session, reader, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT);
		break;
	case I915_VK_COMMAND_CREATE_DESCRIPTOR_POOL:
		error = drv_i915_gfx_create_dpool(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_DESCRIPTOR_POOL:
		error = i915_gfx_destroy_plain(session, reader, I915_VK_OBJ_DESCRIPTOR_POOL);
		break;
	case I915_VK_COMMAND_ALLOCATE_DESCRIPTOR_SETS:
		error = drv_i915_gfx_allocate_dsets(session, reader, reply);
		break;
	case I915_VK_COMMAND_UPDATE_DESCRIPTOR_SETS:
		error = drv_i915_gfx_update_dsets(session, reader);
		break;
	case I915_VK_COMMAND_CREATE_FRAMEBUFFER:
		error = drv_i915_gfx_create_framebuffer(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_FRAMEBUFFER:
		error = i915_gfx_destroy_plain(session, reader, I915_VK_OBJ_FRAMEBUFFER);
		break;
	case I915_VK_COMMAND_CREATE_RENDER_PASS:
		error = drv_i915_gfx_create_render_pass(session, reader, reply);
		break;
	case I915_VK_COMMAND_DESTROY_RENDER_PASS:
		error = i915_gfx_destroy_plain(session, reader, I915_VK_OBJ_RENDER_PASS);
		break;
	default:
		/* Not an object command: another module owns the opcode. */
		*handled = 0;
		return 0;
	}

	/* Reports why the command could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/*
 * Destroys an object that owns nothing but itself: a generic destroy.
 *
 * The command is [device][identity][pAllocator] and has no reply body.  An
 * unknown identity is not an error.
 */
static int
i915_gfx_destroy_plain(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	enum i915_vk_object_kind kind)
{
	void *object;
	uint64_t identity;

	/* Reads the identity between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Unpublishes and frees a known object. */
	object = drv_i915_object_lookup(session->vk, kind, identity);
	if (object != NULL) {
		drv_i915_object_remove(session->vk, kind, identity);
		kern_free(object);
	}

	/* Succeeded: the object is gone. */
	return 0;
}
