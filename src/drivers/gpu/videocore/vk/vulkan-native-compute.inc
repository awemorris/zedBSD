/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The native execution of one prepared vkCmdDispatch as one CSD job.
 *
 * The program's code is uploaded once and kept with its pipeline; the
 * uniform stream is built from the event's descriptor snapshot, its pushed
 * words and the workgroup counts; a dispatch whose program declares shared
 * memory gets an arena of one slot a workgroup.  The CSD configuration
 * follows from the workgroup size, and the job runs to completion while
 * the controller mutex is given up, like a render pass's jobs.  The
 * runner invalidates the GPU caches before it and cleans the TMU writes
 * after it.
 *
 * Every storage address a program computes is clamped into its bound
 * range (the compiler adds the clamp; the stream gives the limit), so a
 * program stays out of the other sessions' memory that the one GPU page
 * table also maps.
 *
 * Included by vulkan-native-job.c, whose job keeps the storage of a
 * dispatch that has not been proved retired.
 */

/*
 * The CSD configuration fields: the workgroup count in the top half of
 * CFG0 to CFG2; in CFG3 the workgroups of a supergroup at bit 8, the
 * batches of a supergroup less one at bit 12 and the workgroup size in the
 * low byte (256 written as 0); in CFG5 the single-segment bit 1 and the
 * four-thread bit 0.  A batch is sixteen lanes, and a supergroup holds at
 * most sixteen workgroups.
 *
 * Values referenced: Mesa 26.2.6 src/broadcom/common/v3d_csd.h
 * (V3D_CSD_CFG012_WG_COUNT_SHIFT, V3D_CSD_CFG3_*, V3D_CSD_CFG5_*).
 */
#define COMPUTE_COUNT_SHIFT 16
#define COMPUTE_SUPERGROUP_SHIFT 8
#define COMPUTE_BATCHES_SHIFT 12
#define COMPUTE_SINGLE_SEGMENT 2U
#define COMPUTE_FOUR_THREADS 1U
#define COMPUTE_LANES 16U
#define COMPUTE_SUPERGROUP_MOST 16U

/*
 * The QPUs a supergroup with a barrier is sized for: eight, the smaller of
 * the two engines, so that it leaves half the QPU threads to others.
 */
#define COMPUTE_BARRIER_QPUS 8U

/* The largest shared memory arena of one dispatch (a larger one is refused, not split). */
#define COMPUTE_ARENA_BYTES (16U * 1024U * 1024U)

/* The workgroup count of one axis of a dispatch. */
#define COMPUTE_GROUP_MOST 65535U

static int compute_groups(const struct bcm2711_vulkan_prepared_event *event, uint32_t *groups);
static int compute_prepare(struct bcm2711_render_device *controller, struct bcm2711_render_session *session, struct bcm2711_vulkan_native_job *job, const struct bcm2711_vulkan_prepared_event *event, const uint32_t *groups, uint64_t arena_bytes, struct bcm2711_native_storage **code);
static int compute_uniforms(const struct bcm2711_vulkan_prepared_event *event, const uint32_t *groups, uint32_t arena, uint64_t arena_bytes, uint32_t *words);
static int compute_word(const struct bcm2711_vulkan_prepared_event *event, const struct bcm2711_shader_uniform *uniform, const uint32_t *groups, uint32_t arena, uint64_t arena_bytes, uint32_t *word);
static int compute_descriptor(const struct bcm2711_vulkan_prepared_event *event, const struct bcm2711_shader_uniform *uniform, const struct bcm2711_vulkan_descriptor **descriptor);
static int compute_image_word(const struct bcm2711_vulkan_descriptor *descriptor, const struct bcm2711_shader_uniform *uniform, uint32_t *word);
static int compute_release(struct bcm2711_render_device *controller, struct bcm2711_vulkan_native_job *job, bool retired);
static void compute_written(const struct bcm2711_vulkan_prepared_event *event);
static void image_written(const struct bcm2711_vulkan_descriptor *descriptor);
static void compute_configuration(const struct bcm2711_shader_binary *program, const uint32_t *groups, bool minus_one, uint32_t code, uint32_t uniforms, uint32_t *configuration);
static uint32_t compute_supergroup(const struct bcm2711_shader_binary *program, uint32_t size, uint32_t total);

/*
 * Runs one prepared dispatch to completion.
 *
 * retired says whether every DMA of the dispatch is proved finished; when
 * it is false the job keeps the dispatch's storage until a checked reset.
 */
int
bcm2711_vulkan_native_compute_run(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_session *session,
	struct bcm2711_vulkan_native_job *job,
	const struct bcm2711_vulkan_prepared_event *event,
	bool *retired)
{
	const struct bcm2711_shader_binary *program;
	struct bcm2711_native_storage *code;
	struct bcm2711_v3d_job native;
	struct bcm2711_v3d_job_result result;
	uint64_t arena_bytes;
	uint32_t groups[3];
	bool minus_one;
	int error;
	int closed;
	int released;

	/* Nothing has launched yet. */
	*retired = true;
	program = event->pipeline->programs[0];
	if (program == NULL || program->stage != BCM2711_SHADER_COMPUTE || program->code_count == 0)
		return EINVAL;

	/* The counts: recorded, or read from the indirect buffer now that every earlier job has retired. */
	error = compute_groups(event, groups);
	if (error != 0)
		return error;

	/* A dispatch with no workgroup along some axis does nothing. */
	if (groups[0] == 0 || groups[1] == 0 || groups[2] == 0)
		return 0;

	/* The shared memory arena: one slot of the program's shared bytes for each workgroup of the dispatch. */
	arena_bytes = (uint64_t)program->shared_bytes * groups[0] * groups[1] * groups[2];
	if (arena_bytes > COMPUTE_ARENA_BYTES)
		return ENOTSUP;

	/* The code, the uniform stream and the arena in native storage, under one checked translation batch. */
	error = bcm2711_v3d_memory_begin(&controller->space);
	if (error != 0)
		return error;
	controller->space.builder = session->owner;
	error = compute_prepare(controller, session, job, event, groups, arena_bytes, &code);
	controller->space.builder = 0;
	closed = bcm2711_v3d_memory_end(&controller->space);
	if (error == 0 && closed != 0)
		error = closed;
	if (error != 0) {
		/* Nothing launched: the storage goes back now. */
		released = compute_release(controller, job, true);
		if (released != 0)
			return released;
		return error;
	}

	/* The CSD job of the whole dispatch. */
	kern_memset(&native, 0, sizeof(native));
	native.kind = BCM2711_V3D_JOB_COMPUTE;
	minus_one = bcm2711_v3d_csd_batches_minus_one(controller->space.native);
	compute_configuration(program, groups, minus_one, code->view->address, job->compute[1]->view->address, native.command.compute.configuration);
	native.command.compute.shader_bytes = program->code_count * 8U;
	native.command.compute.uniform_bytes = (uint32_t)job->compute[1]->bytes;

	/* The job runs without the controller mutex; no translation changes until the window closes. */
	kern_memset(&result, 0, sizeof(result));
	bcm2711_render_window_open(controller);
	error = bcm2711_v3d_job_run(controller->space.native, &native, &result);
	closed = bcm2711_render_window_close(controller);
	if (error == 0 && closed != 0)
		error = closed;
	*retired = result.retired;
	if (error != 0) {
		kern_logf("v3d: dispatch %ux%ux%u of %ux%ux%u failed (%d) retired=%u\n",
			  groups[0],
			  groups[1],
			  groups[2],
			  program->local_size[0],
			  program->local_size[1],
			  program->local_size[2],
			  error,
			  (unsigned)result.retired);
	}

	/* A job not proved retired keeps its storage, owned by the job, until a checked reset. */
	if (!result.retired) {
		if (error == 0)
			error = EIO;
		return error;
	}
	released = compute_release(controller, job, true);
	if (error != 0)
		return error;
	if (released != 0)
		return released;

	/* Snapshots and copies of the bound storage buffers learn that the dispatch may have written them. */
	compute_written(event);

	/* Succeeded: the dispatch ran and its stores reached memory. */
	return 0;
}

/*
 * Reads a dispatch's three workgroup counts: recorded, or from the
 * indirect buffer's uncached storage; a count above 65535 fails the
 * dispatch.
 */
static int
compute_groups(
	const struct bcm2711_vulkan_prepared_event *event,
	uint32_t *groups)
{
	struct bcm2711_v3d_view *view;
	const uint32_t *words;
	uint32_t address;
	uint32_t index;
	void *cpu;
	int error;

	/* A direct dispatch's counts were bounded when it was recorded. */
	if (event->opcode == GPU_OP_CMD_DISPATCH) {
		kern_memcpy(groups, event->draw, 3U * sizeof(groups[0]));
		return 0;
	}

	/* The three words in their current uncached storage. */
	error = bcm2711_vulkan_resource_backing(event->vertices[0], event->offsets[0], 12, &view, &address, &cpu);
	if (error != 0)
		return error;
	kern_io_read_barrier();
	words = cpu;
	for (index = 0; index < 3; index++)
		groups[index] = words[index];

	/* Each count within the limit the device reports. */
	for (index = 0; index < 3; index++) {
		if (groups[index] > COMPUTE_GROUP_MOST)
			return EINVAL;
	}

	/* Succeeded: the counts are read. */
	return 0;
}

/*
 * Places the program's code, the uniform stream and the shared arena in
 * native storage; the job owns the stream, the arena and a code copy the
 * pipeline does not keep.
 */
static int
compute_prepare(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_session *session,
	struct bcm2711_vulkan_native_job *job,
	const struct bcm2711_vulkan_prepared_event *event,
	const uint32_t *groups,
	uint64_t arena_bytes,
	struct bcm2711_native_storage **code)
{
	struct bcm2711_vulkan_pipeline *pipeline;
	const struct bcm2711_shader_binary *program;
	uint64_t bytes;
	uint32_t arena;
	int error;

	/*
	 * The pipeline's code, uploaded by its first dispatch and kept until the
	 * pipeline is released (after every job naming it has retired).
	 */
	(void)session;
	pipeline = event->pipeline;
	program = pipeline->programs[0];
	*code = NULL;
	if (pipeline->code[0] == NULL) {
		error = bcm2711_native_program_upload(&controller->space, program, &pipeline->code[0]);
		if (error != 0)
			return error;
	}

	/* A code copy of another space is not this engine's; the job uploads its own. */
	*code = pipeline->code[0];
	if ((*code)->space != &controller->space) {
		error = bcm2711_native_program_upload(&controller->space, program, &job->compute[0]);
		if (error != 0)
			return error;
		*code = job->compute[0];
	}

	/* The arena, of the session's own earlier bytes or zeroed, cleaned before the device writes it. */
	arena = 0;
	if (arena_bytes != 0) {
		error = bcm2711_native_storage_create_as(&controller->space, arena_bytes, BCM2711_NATIVE_STORAGE_OWNED, &job->compute[2]);
		if (error != 0)
			return error;
		error = bcm2711_native_storage_clean(job->compute[2]);
		if (error != 0)
			return error;
		arena = job->compute[2]->view->address;
	}

	/* The uniform stream, every word written; an empty stream still has one word. */
	bytes = (uint64_t)program->uniform_count * 4U;
	if (bytes == 0)
		bytes = 4;
	error = bcm2711_native_storage_create_as(&controller->space, bytes, BCM2711_NATIVE_STORAGE_ZEROED, &job->compute[1]);
	if (error != 0)
		return error;
	error = compute_uniforms(event, groups, arena, arena_bytes, job->compute[1]->view->buffer->address);
	if (error != 0)
		return error;

	/* The stream becomes visible to the device before its address enters a job. */
	error = bcm2711_native_storage_clean(job->compute[1]);
	if (error != 0)
		return error;

	/* Succeeded: the code, the stream and the arena are in place. */
	return 0;
}

/* Fills the uniform stream in the program's order. */
static int
compute_uniforms(
	const struct bcm2711_vulkan_prepared_event *event,
	const uint32_t *groups,
	uint32_t arena,
	uint64_t arena_bytes,
	uint32_t *words)
{
	const struct bcm2711_shader_binary *program;
	uint32_t index;
	int error;

	/* Each uniform is the word the program expects at its place. */
	program = event->pipeline->programs[0];
	for (index = 0; index < program->uniform_count; index++) {
		error = compute_word(event, &program->uniforms[index], groups, arena, arena_bytes, &words[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the stream is complete. */
	return 0;
}

/* Computes one uniform word of a dispatch from its snapshot, pushed words, counts and arena. */
static int
compute_word(
	const struct bcm2711_vulkan_prepared_event *event,
	const struct bcm2711_shader_uniform *uniform,
	const uint32_t *groups,
	uint32_t arena,
	uint64_t arena_bytes,
	uint32_t *word)
{
	const struct bcm2711_vulkan_descriptor *descriptor;
	struct bcm2711_v3d_view *view;
	const uint32_t *block;
	uint32_t address;
	uint64_t offset;
	void *cpu;
	int error;

	/* Each kind has its own source; the kinds the pipeline refused never reach here. */
	*word = 0;
	switch (uniform->kind) {
	case BCM2711_SHADER_CONSTANT:
		*word = uniform->bits;
		break;
	case BCM2711_SHADER_PUSH:
		if ((uniform->offset & 3U) != 0 || uniform->offset >= 4U * BCM2711_VULKAN_PUSH_WORDS)
			return EINVAL;
		*word = event->push[0][uniform->offset / 4U];
		break;
	case BCM2711_SHADER_GROUP_COUNT:
		if (uniform->bits > 2)
			return EINVAL;
		*word = groups[uniform->bits];
		break;
	case BCM2711_SHADER_GROUP_BASE:
		/* The whole dispatch is one CSD job, starting at the dispatch's first workgroup (zero but for vkCmdDispatchBase). */
		if (uniform->bits > 2)
			return EINVAL;
		if (event->opcode == GPU_OP_CMD_DISPATCH)
			*word = event->base[uniform->bits];
		break;
	case BCM2711_SHADER_SHARED_BASE:
		*word = arena;
		break;
	case BCM2711_SHADER_SHARED_LIMIT:
		/* The arena's last start for an access of `bits` bytes. */
		if (arena_bytes >= uniform->bits)
			*word = (uint32_t)arena_bytes - uniform->bits;
		break;
	case BCM2711_SHADER_STORAGE_BASE:
	case BCM2711_SHADER_STORAGE_LIMIT:
	case BCM2711_SHADER_STORAGE_SIZE:
		/* The bound range's device address, its last start for an access of `bits` bytes, or its size. */
		error = compute_descriptor(event, uniform, &descriptor);
		if (error != 0)
			return error;
		if (descriptor->buffer == NULL)
			return EINVAL;
		error = bcm2711_vulkan_resource_backing(descriptor->buffer->payload, descriptor->offset, descriptor->bytes, &view, &address, &cpu);
		if (error != 0)
			return error;
		if (uniform->kind == BCM2711_SHADER_STORAGE_BASE) {
			*word = address;
		} else if (uniform->kind == BCM2711_SHADER_STORAGE_SIZE) {
			*word = (uint32_t)descriptor->bytes;
		} else if (descriptor->bytes >= uniform->bits) {
			*word = (uint32_t)descriptor->bytes - uniform->bits;
		}
		break;
	case BCM2711_SHADER_BLOCK:
		/* A uniform block's word, read now from its uncached storage after every earlier job retired. */
		error = compute_descriptor(event, uniform, &descriptor);
		if (error != 0)
			return error;
		if (descriptor->buffer == NULL)
			return EINVAL;
		if ((uniform->offset & 3U) != 0 || uniform->offset > descriptor->bytes || 4U > descriptor->bytes - uniform->offset)
			return EINVAL;
		offset = descriptor->offset + uniform->offset;
		error = bcm2711_vulkan_resource_backing(descriptor->buffer->payload, offset, 4, &view, &address, &cpu);
		if (error != 0)
			return error;
		block = cpu;
		*word = *block;
		break;
	case BCM2711_SHADER_IMAGE_BASE:
	case BCM2711_SHADER_IMAGE_LIMIT:
	case BCM2711_SHADER_IMAGE_PITCH:
	case BCM2711_SHADER_IMAGE_LAYER:
	case BCM2711_SHADER_IMAGE_SIZE:
		/* A storage image's view: its raster's place, extent and size. */
		error = compute_descriptor(event, uniform, &descriptor);
		if (error != 0)
			return error;
		error = compute_image_word(descriptor, uniform, word);
		if (error != 0)
			return error;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: the word is the program's. */
	return 0;
}

/* Finds the snapshot's descriptor of a uniform's set and binding through the bound layout's canonical slot. */
static int
compute_descriptor(
	const struct bcm2711_vulkan_prepared_event *event,
	const struct bcm2711_shader_uniform *uniform,
	const struct bcm2711_vulkan_descriptor **descriptor)
{
	const struct bcm2711_vulkan_pipeline_layout *layout;
	const struct bcm2711_vulkan_set_layout *set;
	uint32_t slot;

	/* The pipeline layout's set; its slots are in binding order, as the bound set's are. */
	*descriptor = NULL;
	layout = event->pipeline->owner.parent->payload;
	if (uniform->set >= layout->count || uniform->set >= BCM2711_VULKAN_PIPELINE_SETS)
		return EINVAL;
	set = layout->sets[uniform->set]->payload;

	/* The slot whose binding number is the uniform's, which the snapshot holds when the program uses it. */
	for (slot = 0; slot < set->count; slot++) {
		if (set->bindings[slot].number != uniform->binding || set->bindings[slot].element != uniform->element)
			continue;
		if ((event->used[uniform->set] & (1U << slot)) == 0)
			return EINVAL;
		*descriptor = &event->descriptors[uniform->set][slot];
		break;
	}

	/* A binding the layout lacks; the caller checks the slot holds what it reads. */
	if (*descriptor == NULL)
		return EINVAL;

	/* Succeeded: the descriptor is the snapshot's. */
	return 0;
}

/*
 * Computes one word describing a storage image's view at its base level:
 * the first layer's address, the last start of an access of `bits` bytes
 * (the view's layers, or a 3D view's slices, each a layer stride after the
 * one before, the last only as long as the level's raster), the row pitch,
 * the layer stride, or the size along an axis.
 */
static int
compute_image_word(
	const struct bcm2711_vulkan_descriptor *descriptor,
	const struct bcm2711_shader_uniform *uniform,
	uint32_t *word)
{
	const struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_image_level level;
	struct bcm2711_v3d_view *view;
	uint64_t first;
	uint64_t span;
	uint32_t layers;
	uint32_t address;
	void *cpu;
	int error;

	/* The view and its image. */
	if (descriptor->view == NULL || descriptor->view->payload == NULL)
		return EINVAL;
	image_view = descriptor->view->payload;
	if (image_view->owner.parent == NULL || image_view->owner.parent->payload == NULL)
		return EINVAL;
	image = image_view->owner.parent->payload;

	/* The base level's raster and the view's layers (a 3D view's slices at the level). */
	error = bcm2711_vulkan_image_level(image, image_view->base_level, &level);
	if (error != 0)
		return error;
	layers = image_view->layers;
	if (image_view->type == VK_IMAGE_VIEW_TYPE_3D)
		layers = bcm2711_vulkan_image_slices(image, image_view->base_level);
	if (layers == 0)
		return EINVAL;
	first = (uint64_t)image_view->base_layer * image->layer_stride + level.offset;
	span = (uint64_t)(layers - 1U) * image->layer_stride + level.bytes;

	/* Each word. */
	switch (uniform->kind) {
	case BCM2711_SHADER_IMAGE_BASE:
		error = bcm2711_vulkan_resource_backing(image, first, span, &view, &address, &cpu);
		if (error != 0)
			return error;
		*word = address;
		break;
	case BCM2711_SHADER_IMAGE_LIMIT:
		*word = 0;
		if (span >= uniform->bits)
			*word = (uint32_t)(span - uniform->bits);
		break;
	case BCM2711_SHADER_IMAGE_PITCH:
		*word = level.pitch;
		break;
	case BCM2711_SHADER_IMAGE_LAYER:
		*word = (uint32_t)image->layer_stride;
		break;
	default:
		if (uniform->bits == 0) {
			*word = level.width;
		} else if (uniform->bits == 1U) {
			*word = level.height;
		} else if (uniform->bits == 2U) {
			*word = 1;
			if (image_view->type == VK_IMAGE_VIEW_TYPE_3D)
				*word = layers;
		} else if (uniform->bits == 3U) {
			*word = image_view->layers;
		} else {
			return EINVAL;
		}
		break;
	}

	/* Succeeded: the word describes the view. */
	return 0;
}

/* Gives back the dispatch's storage the job owns: the uniform stream, the arena and a code copy. */
static int
compute_release(
	struct bcm2711_render_device *controller,
	struct bcm2711_vulkan_native_job *job,
	bool retired)
{
	uint32_t index;
	int error;
	int released;

	/* Every storage goes, inside one checked translation batch, even after an earlier one failed. */
	error = bcm2711_v3d_memory_begin(&controller->space);
	if (error != 0)
		return error;
	for (index = 0; index < 3; index++) {
		if (job->compute[index] == NULL)
			continue;
		released = bcm2711_native_storage_release(&controller->space, &job->compute[index], retired);
		if (released != 0 && error == 0)
			error = released;
	}
	released = bcm2711_v3d_memory_end(&controller->space);
	if (released != 0 && error == 0)
		error = released;
	if (error != 0)
		return error;

	/* Succeeded: the job owns no dispatch storage. */
	return 0;
}

/* Tells each storage buffer and storage image the dispatch bound that its bytes may have changed. */
static void
compute_written(
	const struct bcm2711_vulkan_prepared_event *event)
{
	const struct bcm2711_vulkan_descriptor *descriptor;
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_v3d_view *view;
	uint32_t address;
	uint32_t set;
	uint32_t slot;
	void *cpu;
	int error;

	/* Every snapshot buffer of the dispatch's storage type; a uniform block's buffer is only read. */
	for (set = 0; set < BCM2711_VULKAN_PIPELINE_SETS; set++) {
		for (slot = 0; slot < BCM2711_VULKAN_LAYOUT_BINDINGS; slot++) {
			if ((event->used[set] & (1U << slot)) == 0)
				continue;
			descriptor = &event->descriptors[set][slot];

			/* A storage image's view (one without a sampler): every byte of its image may have changed. */
			if (descriptor->view != NULL && descriptor->sampler == NULL) {
				image_written(descriptor);
				continue;
			}
			if (descriptor->buffer == NULL)
				continue;
			resource = descriptor->buffer->payload;
			if ((resource->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0)
				continue;

			/* An unknown extent of the buffer's run changed. */
			error = bcm2711_vulkan_resource_backing(resource, descriptor->offset, descriptor->bytes, &view, &address, &cpu);
			if (error != 0)
				continue;
			bcm2711_v3d_memory_written(view->buffer, address - view->address, NULL);
		}
	}

	/* Succeeded: later snapshots of those buffers are made again. */
	return;
}

/* Tells a storage image's memory that a dispatch may have changed any of its bytes, so later sampling converts it again. */
static void
image_written(
	const struct bcm2711_vulkan_descriptor *descriptor)
{
	const struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_v3d_view *view;
	uint64_t bytes;
	uint32_t address;
	void *cpu;
	int error;

	/* The image's whole binding. */
	image_view = descriptor->view->payload;
	image = image_view->owner.parent->payload;
	bytes = image->storage_bytes;
	if (bytes == 0)
		bytes = image->bytes;
	error = bcm2711_vulkan_resource_backing(image, 0, bytes, &view, &address, &cpu);
	if (error != 0)
		return;
	bcm2711_v3d_memory_written(view->buffer, address - view->address, NULL);

	/* Succeeded: later snapshots of the image are made again. */
	return;
}

/*
 * Computes CFG0 to CFG6 of a CSD job running a whole dispatch; minus_one
 * says the engine takes the batch count less one.
 *
 * The workgroups of a supergroup share batches of sixteen lanes; the
 * batch count is the whole supergroups' plus the last, partial one's.
 *
 * Procedure referenced: Mesa 26.2.6 src/broadcom/vulkan/v3dv_cmd_buffer.c
 * (cmd_buffer_create_csd_job: the batch count and CFG3 to CFG5).
 */
static void
compute_configuration(
	const struct bcm2711_shader_binary *program,
	const uint32_t *groups,
	bool minus_one,
	uint32_t code,
	uint32_t uniforms,
	uint32_t *configuration)
{
	uint32_t size;
	uint32_t total;
	uint32_t per_supergroup;
	uint32_t batches_per_supergroup;
	uint32_t batches;
	uint32_t remainder;

	/* The supergroup, its batches and the dispatch's batches. */
	size = program->local_size[0] * program->local_size[1] * program->local_size[2];
	total = groups[0] * groups[1] * groups[2];
	per_supergroup = compute_supergroup(program, size, total);
	batches_per_supergroup = (per_supergroup * size + COMPUTE_LANES - 1U) / COMPUTE_LANES;
	remainder = total % per_supergroup;
	batches = batches_per_supergroup * (total / per_supergroup) + (remainder * size + COMPUTE_LANES - 1U) / COMPUTE_LANES;

	/* The counts, the supergroup and the batches. */
	configuration[0] = groups[0] << COMPUTE_COUNT_SHIFT;
	configuration[1] = groups[1] << COMPUTE_COUNT_SHIFT;
	configuration[2] = groups[2] << COMPUTE_COUNT_SHIFT;
	configuration[3] = ((per_supergroup & 0xfU) << COMPUTE_SUPERGROUP_SHIFT) |
			   ((batches_per_supergroup - 1U) << COMPUTE_BATCHES_SHIFT) |
			   (size & 0xffU);
	configuration[4] = batches;
	if (minus_one)
		configuration[4] = batches - 1U;

	/* The code with its segment and threading bits, and the uniform stream. */
	configuration[5] = code;
	if (program->starts_final != 0)
		configuration[5] |= COMPUTE_SINGLE_SEGMENT;
	if (program->threads == 4)
		configuration[5] |= COMPUTE_FOUR_THREADS;
	configuration[6] = uniforms;

	/* Succeeded: the seven words describe the dispatch. */
	return;
}

/*
 * Chooses the workgroups of a supergroup: one for a size of whole batches
 * or a program whose barrier sits in a loop, otherwise the count up to
 * sixteen (and the dispatch's) that leaves the fewest idle lanes.
 *
 * A barrier waits for the whole supergroup, so a supergroup with one takes
 * at most half the QPU threads and leaves the other half to run.  A
 * barrier inside a loop may be reached a different number of times by the
 * workgroups of one supergroup, which would wait for each other forever.
 *
 * Procedure referenced: Mesa 26.2.6 src/broadcom/common/v3d_util.c
 * (v3d_csd_choose_workgroups_per_supergroup).
 */
static uint32_t
compute_supergroup(
	const struct bcm2711_shader_binary *program,
	uint32_t size,
	uint32_t total)
{
	uint32_t best;
	uint32_t count;
	uint32_t waste;
	uint32_t least;
	uint32_t most;
	uint32_t batches;

	/* Whole batches need no packing; a barrier in a loop allows none. */
	if (size % COMPUTE_LANES == 0 || program->barrier_in_loop != 0)
		return 1;

	/* The barrier's limit on the supergroup's batches. */
	most = COMPUTE_SUPERGROUP_MOST;
	if (program->uses_barrier != 0) {
		batches = size;
		if (batches > COMPUTE_BARRIER_QPUS * program->threads / 2U)
			batches = COMPUTE_BARRIER_QPUS * program->threads / 2U;
		most = batches * COMPUTE_LANES / size;
		if (most == 0)
			most = 1;
		if (most > COMPUTE_SUPERGROUP_MOST)
			most = COMPUTE_SUPERGROUP_MOST;
	}

	/* The count that wastes the fewest lanes, the smallest on a tie. */
	best = 1;
	least = UINT32_MAX;
	for (count = 1; count <= most && count <= total; count++) {
		waste = ((count * size + COMPUTE_LANES - 1U) / COMPUTE_LANES) * COMPUTE_LANES - count * size;
		if (waste < least) {
			least = waste;
			best = count;
		}
	}

	/* Succeeded: the count fills the supergroup's batches best. */
	return best;
}
