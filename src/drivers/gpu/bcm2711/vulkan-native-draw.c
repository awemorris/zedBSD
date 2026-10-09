/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Complete FIFO draw preparation owns cached GPU inputs without launching a native command list. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/native-state.h"
#include "drivers/gpu/bcm2711/vulkan-native-draw.h"

/* One job's aggregate native staging includes page padding and every draw's independently uploaded inputs. */
#define NATIVE_DRAW_BYTES (256ULL * 1024U * 1024U)

/* One canonical location contributes leading float scalars from a checked coherent source, then Vulkan default components. */
struct native_fetch {
	const uint8_t *cpu;
	uint32_t stride;
	uint32_t source_components;
	uint32_t components;
};

static int allocate_storage(struct bcm2711_vulkan_native_draw *draw, uint64_t bytes, uint64_t available, const struct bcm2711_shader_binary *program, struct bcm2711_native_storage **storage);
static int prepare_textures(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_prepared_event *event, uint64_t available);
static int prepare_texture(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_descriptor *descriptor, uint64_t available, struct bcm2711_vulkan_native_binding *binding);
static int prepare_sampler(const struct bcm2711_vulkan_sampler *source, struct bcm2711_native_sampler *sampler);
static int prepare_programs(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_prepared_event *event, uint64_t available, struct bcm2711_native_shader *shader);
static int prepare_records(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_prepared_event *event, uint64_t available, const struct bcm2711_native_shader *shader);
static int prepare_fetch(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_prepared_event *event, uint64_t available, struct bcm2711_native_attribute attributes[BCM2711_VULKAN_VERTEX_ATTRIBUTES]);
static int resolve_fetch(const struct bcm2711_vulkan_prepared_event *event, uint32_t location, uint32_t components, struct native_fetch *fetch);
static int format_components(VkFormat format, uint32_t *components);
static void write_word(uint8_t *destination, uint32_t word);

/*
 * Builds a complete independently mapped draw at FIFO execution after earlier writes become CPU-visible.
 *
 * The enclosing job retains the pending prepared primary through DMA and
 * uncertainty.  This root copies all GPU-read inputs and consumes no public
 * identities.  Failure releases every acquired unpublished upload owner.
 */
int
bcm2711_vulkan_native_draw_create(
	struct bcm2711_v3d_space *space,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t *available,
	struct bcm2711_vulkan_native_draw **draw)
{
	struct bcm2711_vulkan_native_draw *created;
	struct bcm2711_native_shader shader;
	int error;
	int released;

	/* No partial root may escape through the caller's output. */
	if (draw == NULL)
		return EINVAL;
	*draw = NULL;

	/* Only real prepared draws and an identified native controller provide the required immutable inputs. */
	if (space == NULL || space->native == NULL || event == NULL || event->pipeline == NULL)
		return EINVAL;
	if (event->opcode != GPU_OP_CMD_DRAW || event->draw[0] == 0 || event->draw[1] != 1 || event->draw[3] != 0)
		return EINVAL;

	/* One finite job-wide budget prevents independent draws from bypassing the total native staging bound. */
	if (available == NULL || *available > NATIVE_DRAW_BYTES)
		return EINVAL;
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->space = space;
	created->vertices = event->draw[0];
	kern_memset(&shader, 0, sizeof(shader));

	/* Real IDENT1 determines available VPM; native encoders reject insufficient capacity without launching. */
	shader.vpm_bytes = ((space->native->hardware.core_ident[1] >> 28) & 15U) * 8192U;

	/* Every texture is copied now rather than frozen before prior queue writes retire. */
	error = prepare_textures(created, event, *available);
	if (error == 0)
		error = prepare_programs(created, event, *available, &shader);
	if (error == 0)
		error = prepare_records(created, event, *available, &shader);

	/* Pre-launch retirement is proven here; failed translation teardown remains in the native space's quarantine. */
	if (error != 0) {
		released = bcm2711_vulkan_native_draw_release(&created, true);
		if (released != 0)
			return released;
		return error;
	}

	/* The job budget changes only when a complete native root becomes caller-owned. */
	*available -= created->bytes;
	*draw = created;

	/* Succeeded: all code, uniforms, texture/fetch data and records remain independently owned until checked retirement. */
	return 0;
}

/*
 * Retires all inputs of one complete or unpublished native draw only after DMA is proved stopped.
 */
int
bcm2711_vulkan_native_draw_release(
	struct bcm2711_vulkan_native_draw **draw,
	bool retired)
{
	struct bcm2711_vulkan_native_draw *owned;
	uint32_t index;
	int error;
	int first;

	/* Missing pointer ownership cannot participate in explicit root consumption. */
	if (draw == NULL)
		return EINVAL;
	owned = *draw;
	if (owned == NULL)
		return 0;

	/* Callback completion alone never retires code or any other GPU-read input. */
	if (!retired)
		return EBUSY;

	/* Each mapped reference is consumed once; the space preserves failed unmaps without this CPU root. */
	*draw = NULL;
	first = 0;
	for (index = 0; index < owned->count; index++) {
		error = bcm2711_native_storage_release(owned->space, &owned->storage[index], true);
		if (first == 0 && error != 0)
			first = error;
	}

	/* Complete remaining owners still retire after an earlier translation failure, preserving the first refusal. */
	kern_free(owned);
	if (first != 0)
		return first;

	/* Succeeded: no independently mapped draw input remains owned by the consumed root. */
	return 0;
}

/* Acquires one bounded padded upload owner and immediately attaches it to the unpublished draw root. */
static int
allocate_storage(
	struct bcm2711_vulkan_native_draw *draw,
	uint64_t bytes,
	uint64_t available,
	const struct bcm2711_shader_binary *program,
	struct bcm2711_native_storage **storage)
{
	uint64_t padded;
	int error;

	/* Output remains absent until a complete native mapping exists. */
	*storage = NULL;
	if (bytes == 0 || bytes > NATIVE_DRAW_BYTES)
		return ENOTSUP;
	padded = (bytes + 4095U) & ~4095ULL;

	/* Both allocation count and the enclosing job's remaining total are checked before physical acquisition. */
	if (draw->count == BCM2711_VULKAN_DRAW_STORAGE || draw->bytes > available || padded > available - draw->bytes)
		return ENOMEM;
	error = 0;
	if (program != NULL)
		error = bcm2711_native_program_upload(draw->space, program, storage);
	else
		error = bcm2711_native_storage_create(draw->space, bytes, storage);
	if (error != 0)
		return error;

	/* From this point every late refusal can release the whole acquired prefix through its root. */
	draw->storage[draw->count] = *storage;
	draw->count++;
	draw->bytes += padded;

	/* Succeeded: the actual zeroed padded run and its VA are owned before any bytes or pointers are published. */
	return 0;
}

/* Copies only consumed sampled clones; UBO descriptors contribute scalar reads instead of texture storage. */
static int
prepare_textures(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t available)
{
	const struct bcm2711_vulkan_descriptor *descriptor;
	uint32_t set;
	uint32_t slot;
	int error;

	/* Canonical consumed masks never reinterpret undefined bits as descriptor slots. */
	for (set = 0; set < BCM2711_VULKAN_PIPELINE_SETS; set++) {
		if ((event->used[set] >> BCM2711_VULKAN_LAYOUT_BINDINGS) != 0)
			return EINVAL;
		for (slot = 0; slot < BCM2711_VULKAN_LAYOUT_BINDINGS; slot++) {
			/* Unconsumed slots require no image conversion or native descriptor record. */
			if ((event->used[set] & (1U << slot)) == 0)
				continue;
			descriptor = &event->descriptors[set][slot];
			if (descriptor->buffer != NULL)
				continue;
			error = prepare_texture(draw, descriptor, available, &draw->bindings[set][slot]);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: each consumed sampled descriptor points exclusively to job-owned scratch and records. */
	return 0;
}

/* Converts one actual raster image to strict non-XOR UIF and encodes its exact retained sampler in owned aligned storage. */
static int
prepare_texture(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_descriptor *descriptor,
	uint64_t available,
	struct bcm2711_vulkan_native_binding *binding)
{
	struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_native_storage *pixels;
	struct bcm2711_native_storage *records;
	struct bcm2711_native_texture texture;
	struct bcm2711_native_sampler sampler;
	struct bcm2711_v3d_view *source_view;
	void *cpu;
	uint8_t *destination;
	uint32_t address;
	uint32_t bytes;
	int error;

	/* Retained typed clones supply both immutable sampler parameters and the actual image dependency. */
	if (descriptor->view == NULL || descriptor->sampler == NULL)
		return EINVAL;
	if (descriptor->view->kind != I915_VK_OBJ_IMAGE_VIEW || descriptor->sampler->kind != I915_VK_OBJ_SAMPLER)
		return EINVAL;
	image_view = descriptor->view->payload;
	if (image_view == NULL || image_view->owner.parent == NULL || descriptor->sampler->payload == NULL)
		return EINVAL;
	image = image_view->owner.parent->payload;
	if (image == NULL || (image->format != VK_FORMAT_R8G8B8A8_UNORM && image->format != VK_FORMAT_B8G8R8A8_UNORM))
		return ENOTSUP;

	/* Complete coherent logical raster storage is read only at execution, after previous GPU writes become visible. */
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &source_view, &address, &cpu);
	if (error != 0)
		return error;
	error = bcm2711_native_texture_size(image->width, image->height, &bytes);
	if (error != 0)
		return error;
	error = allocate_storage(draw, bytes, available, NULL, &pixels);
	if (error != 0)
		return error;
	error = bcm2711_native_texture_copy(image->width, image->height, cpu, image->pitch, (size_t)image->bytes, pixels->view->buffer->address, (size_t)pixels->bytes);
	if (error != 0)
		return error;
	error = bcm2711_native_storage_clean(pixels);
	if (error != 0)
		return error;

	/* Both 24-byte records have independent 32-byte alignment within the same zero-padded allocation. */
	error = allocate_storage(draw, 64, available, NULL, &records);
	if (error != 0)
		return error;
	kern_memset(&texture, 0, sizeof(texture));
	texture.address = pixels->view->address;
	texture.width = image->width;
	texture.height = image->height;
	texture.bytes = bytes;
	if (image->format == VK_FORMAT_B8G8R8A8_UNORM)
		texture.swap_red_blue = 1;
	destination = records->view->buffer->address;
	error = bcm2711_native_texture_encode(&texture, destination, 32);
	if (error != 0)
		return error;
	error = prepare_sampler(descriptor->sampler->payload, &sampler);
	if (error != 0)
		return error;
	error = bcm2711_native_sampler_encode(&sampler, destination + 32, 32);
	if (error != 0)
		return error;
	error = bcm2711_native_storage_clean(records);
	if (error != 0)
		return error;

	/* Uniform words borrow these exact addresses only after the whole input records have been cleaned. */
	binding->texture = records->view->address;
	binding->sampler = records->view->address + 32U;

	/* Succeeded: sampling cannot reference the raster layout or another job's transient descriptor records. */
	return 0;
}

/* Converts the supported Vulkan normalized filters and wraps to numerical hardware choices. */
static int
prepare_sampler(
	const struct bcm2711_vulkan_sampler *source,
	struct bcm2711_native_sampler *sampler)
{
	/* Unsupported retained metadata never silently changes filtering. */
	if ((source->mag != VK_FILTER_NEAREST && source->mag != VK_FILTER_LINEAR) ||
	    (source->min != VK_FILTER_NEAREST && source->min != VK_FILTER_LINEAR))
		return ENOTSUP;
	kern_memset(sampler, 0, sizeof(*sampler));
	if (source->mag == VK_FILTER_NEAREST)
		sampler->nearest_mag = 1;
	if (source->min == VK_FILTER_NEAREST)
		sampler->nearest_min = 1;

	/* Vulkan wrap enumeration differs from the native clamp/mirror numerical order. */
	switch (source->u) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:
		break;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
		sampler->wrap_u = 1;
		break;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		sampler->wrap_u = 2;
		break;
	default:
		return ENOTSUP;
	}

	/* V uses the same explicitly supported conversion without inheriting U's choice. */
	switch (source->v) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:
		break;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
		sampler->wrap_v = 1;
		break;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		sampler->wrap_v = 2;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: native sampler encoding receives exact supported filter and wrap values. */
	return 0;
}

/* Uploads all three native programs and their FIFO scalar streams, then prepares float defaults. */
static int
prepare_programs(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t available,
	struct bcm2711_native_shader *shader)
{
	const struct bcm2711_shader_binary *program;
	struct bcm2711_vulkan_uniform_words *words;
	struct bcm2711_native_storage *code;
	struct bcm2711_native_storage *uniforms;
	struct bcm2711_native_storage *defaults;
	uint8_t *destination;
	uint64_t bytes;
	uint32_t stage;
	uint32_t index;
	int error;

	/* Every emitted program retains independent code and exact current scalar consumption. */
	for (stage = 0; stage < 3; stage++) {
		program = event->pipeline->programs[stage];
		if (program == NULL || program->code == NULL || program->code_count == 0 || (uint32_t)program->stage != stage)
			return EINVAL;
		bytes = (uint64_t)program->code_count * 8U;
		error = allocate_storage(draw, bytes, available, program, &code);
		if (error != 0)
			return error;
		error = bcm2711_vulkan_uniform_create(event, stage, draw->bindings, &words);
		if (error != 0)
			return error;

		/* Empty streams still receive a valid aligned placeholder, without fabricating a consumed word. */
		bytes = (uint64_t)words->count * 4U;
		if (bytes == 0)
			bytes = 4;
		error = allocate_storage(draw, bytes, available, NULL, &uniforms);
		if (error != 0) {
			bcm2711_vulkan_uniform_release(words);
			return error;
		}

		/* Scalar words are copied before their temporary CPU owner is retired. */
		destination = uniforms->view->buffer->address;
		for (index = 0; index < words->count; index++)
			write_word(destination + (size_t)index * 4U, words->words[index]);
		bcm2711_vulkan_uniform_release(words);
		error = bcm2711_native_storage_clean(uniforms);
		if (error != 0)
			return error;
		shader->programs[stage].binary = program;
		shader->programs[stage].code = code->view->address;
		shader->programs[stage].uniforms = uniforms->view->address;
	}

	/* Every default attribute is exactly float (0,0,0,1), including locations the shader does not consume. */
	error = allocate_storage(draw, 256, available, NULL, &defaults);
	if (error != 0)
		return error;
	destination = defaults->view->buffer->address;
	for (index = 0; index < 16; index++)
		write_word(destination + index * 16U + 12U, 0x3f800000U);
	error = bcm2711_native_storage_clean(defaults);
	if (error != 0)
		return error;
	shader->defaults = defaults->view->address;

	/* Succeeded: shader record construction can reference only independently uploaded complete code/scalar/default intervals. */
	return 0;
}

/* Encodes contiguous shader and canonical fetch records after every backing interval exists. */
static int
prepare_records(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t available,
	const struct bcm2711_native_shader *shader)
{
	struct bcm2711_native_attribute attributes[BCM2711_VULKAN_VERTEX_ATTRIBUTES];
	struct bcm2711_native_storage *records;
	uint8_t *destination;
	uint32_t bytes;
	uint32_t index;
	int error;

	/* Each fetch record receives a bounded private packed source rather than a speculative user-buffer stride. */
	kern_memset(attributes, 0, sizeof(attributes));
	error = prepare_fetch(draw, event, available, attributes);
	if (error != 0)
		return error;
	bytes = BCM2711_NATIVE_SHADER_BYTES + draw->attributes * BCM2711_NATIVE_ATTRIBUTE_BYTES;
	error = allocate_storage(draw, bytes, available, NULL, &records);
	if (error != 0)
		return error;
	destination = records->view->buffer->address;
	error = bcm2711_native_shader_encode(shader, destination, bytes);
	if (error != 0)
		return error;

	/* Hardware locates each attribute immediately after the 36-byte shader state record. */
	for (index = 0; index < draw->attributes; index++) {
		error = bcm2711_native_attribute_encode(&attributes[index], destination + BCM2711_NATIVE_SHADER_BYTES + index * BCM2711_NATIVE_ATTRIBUTE_BYTES, BCM2711_NATIVE_ATTRIBUTE_BYTES);
		if (error != 0)
			return error;
	}

	/* No pointer can reach the native BCL before complete records and zero page padding are CPU-clean. */
	error = bcm2711_native_storage_clean(records);
	if (error != 0)
		return error;
	draw->shader = records->view->address;

	/* Succeeded: one aligned native shader pointer and its exact attribute count describe this complete owned draw. */
	return 0;
}

/* Packs canonical float inputs once, supplying missing format components and rebasing the copied first vertex to zero. */
static int
prepare_fetch(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t available,
	struct bcm2711_native_attribute attributes[BCM2711_VULKAN_VERTEX_ATTRIBUTES])
{
	const struct bcm2711_shader_binary *coordinate;
	const struct bcm2711_shader_binary *vertex;
	struct native_fetch fetch[BCM2711_VULKAN_VERTEX_ATTRIBUTES];
	struct bcm2711_native_storage *storage;
	uint8_t *destination;
	const uint8_t *source;
	uint64_t bytes;
	uint32_t input;
	uint32_t location;
	uint32_t components;
	uint32_t count;
	uint32_t stride;
	uint32_t item;
	uint32_t scalar;
	uint32_t index;
	uint32_t offset;
	int error;

	/* Current native variants use one identical canonical input FIFO; differing metadata cannot share this fetch packing. */
	coordinate = event->pipeline->programs[BCM2711_SHADER_COORDINATE];
	vertex = event->pipeline->programs[BCM2711_SHADER_VERTEX];
	if (coordinate->input_count != vertex->input_count || vertex->input_count > BCM2711_SHADER_INTERFACE_WORDS)
		return EINVAL;
	for (input = 0; input < vertex->input_count; input++) {
		if (coordinate->inputs[input].location != vertex->inputs[input].location || coordinate->inputs[input].component != vertex->inputs[input].component)
			return EINVAL;
	}

	/* Shader locations are ascending and every declared component is a leading consecutive scalar. */
	kern_memset(fetch, 0, sizeof(fetch));
	count = 0;
	input = 0;
	while (input < vertex->input_count) {
		location = vertex->inputs[input].location;
		if (location >= BCM2711_VULKAN_VERTEX_ATTRIBUTES || count == BCM2711_VULKAN_VERTEX_ATTRIBUTES)
			return EINVAL;
		if (count != 0 && location <= vertex->inputs[input - 1U].location)
			return EINVAL;
		components = 0;
		while (input < vertex->input_count && vertex->inputs[input].location == location) {
			if (vertex->inputs[input].component != components || components == 4)
				return EINVAL;
			components++;
			input++;
		}

		/* Resolve complete logical source intervals before any packed GPU input can be published. */
		error = resolve_fetch(event, location, components, &fetch[count]);
		if (error != 0)
			return error;
		count++;
	}

	/* Zero-input shaders need no fetch allocation; the independently uploaded defaults still exist. */
	stride = vertex->input_count * 4U;
	if (stride != 0) {
		bytes = (uint64_t)draw->vertices * stride;
		error = allocate_storage(draw, bytes, available, NULL, &storage);
		if (error != 0)
			return error;
		destination = storage->view->buffer->address;

		/* Each copied vertex contains only consumed scalars; native draws start at zero after source firstVertex is applied. */
		for (item = 0; item < draw->vertices; item++) {
			offset = 0;
			for (index = 0; index < count; index++) {
				source = fetch[index].cpu + (size_t)item * fetch[index].stride;
				for (scalar = 0; scalar < fetch[index].components; scalar++) {
					/* Missing float format components follow Vulkan's zero/one defaults instead of reading neighbouring attributes. */
					if (scalar < fetch[index].source_components)
						kern_memcpy(destination + (size_t)item * stride + offset, source + scalar * 4U, 4);
					else if (scalar == 3)
						write_word(destination + (size_t)item * stride + offset, 0x3f800000U);
					else
						write_word(destination + (size_t)item * stride + offset, 0);
					offset += 4;
				}
			}
		}

		/* Exact leading counts preserve both VPM FIFOs while native maximum index cannot enter padded storage. */
		offset = 0;
		for (index = 0; index < count; index++) {
			attributes[index].address = storage->view->address + offset;
			attributes[index].stride = stride;
			attributes[index].maximum_index = draw->vertices - 1U;
			attributes[index].components = fetch[index].components;
			attributes[index].coordinate_values = fetch[index].components;
			attributes[index].vertex_values = fetch[index].components;
			offset += fetch[index].components * 4U;
		}

		/* Complete packed values and page padding become device-visible before record construction. */
		error = bcm2711_native_storage_clean(storage);
		if (error != 0)
			return error;
	}

	/* No attribute metadata retains a coherent source pointer after the complete packed GPU copy exists. */
	draw->attributes = count;

	/* Succeeded: all logical source strides are replaced by checked independently owned native fetch intervals. */
	return 0;
}

/* Resolves one location's exact source format, binding and last fetched logical vertex. */
static int
resolve_fetch(
	const struct bcm2711_vulkan_prepared_event *event,
	uint32_t location,
	uint32_t components,
	struct native_fetch *fetch)
{
	const VkVertexInputAttributeDescription *attribute;
	const VkVertexInputBindingDescription *binding;
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint64_t begin;
	uint64_t bytes;
	uint32_t address;
	uint32_t index;
	int error;

	/* Immutable pipeline array counts precede all declaration searches. */
	if (event->pipeline->attribute_count > BCM2711_VULKAN_VERTEX_ATTRIBUTES || event->pipeline->binding_count > BCM2711_VULKAN_VERTEX_BINDINGS)
		return EINVAL;
	attribute = NULL;
	for (index = 0; index < event->pipeline->attribute_count; index++) {
		if (event->pipeline->attributes[index].location == location)
			attribute = &event->pipeline->attributes[index];
	}

	/* A shader-consumed location cannot inherit an undefined user vertex source. */
	if (attribute == NULL || attribute->binding >= BCM2711_VULKAN_VERTEX_BINDINGS)
		return EINVAL;
	resource = event->vertices[attribute->binding];
	if (resource == NULL)
		return EINVAL;
	binding = NULL;
	for (index = 0; index < event->pipeline->binding_count; index++) {
		if (event->pipeline->bindings[index].binding == attribute->binding)
			binding = &event->pipeline->bindings[index];
	}

	/* Only per-vertex float fetch is implemented; absent declarations cannot provide an implicit stride. */
	if (binding == NULL || binding->inputRate != VK_VERTEX_INPUT_RATE_VERTEX)
		return EINVAL;
	error = format_components(attribute->format, &fetch->source_components);
	if (error != 0)
		return error;

	/* Source firstVertex is applied before packing; every last complete source format remains inside logical resource bytes. */
	begin = event->offsets[attribute->binding];
	if (begin > resource->bytes || attribute->offset > resource->bytes - begin)
		return EINVAL;
	begin += attribute->offset;
	bytes = (uint64_t)event->draw[2] * binding->stride;
	if (bytes > resource->bytes - begin)
		return EINVAL;
	begin += bytes;
	bytes = (uint64_t)(event->draw[0] - 1U) * binding->stride + fetch->source_components * 4U;
	error = bcm2711_vulkan_resource_backing(resource, begin, bytes, &view, &address, &cpu);
	if (error != 0)
		return error;
	fetch->cpu = cpu;
	fetch->stride = binding->stride;
	fetch->components = components;

	/* Succeeded: the borrowed pointer remains valid under the controller mutex until this synchronous packed copy finishes. */
	return 0;
}

/* Maps only the float formats already accepted by native graphics pipeline creation to exact physical source widths. */
static int
format_components(
	VkFormat format,
	uint32_t *components)
{
	/* Every admitted format contributes a finite number of four-byte scalar words. */
	switch (format) {
	case VK_FORMAT_R32_SFLOAT:
		*components = 1;
		break;
	case VK_FORMAT_R32G32_SFLOAT:
		*components = 2;
		break;
	case VK_FORMAT_R32G32B32_SFLOAT:
		*components = 3;
		break;
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		*components = 4;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: later logical source checks never use native allocation padding as another float component. */
	return 0;
}

/* Stores one numerical native word explicitly in little-endian byte order. */
static void
write_word(
	uint8_t *destination,
	uint32_t word)
{
	/* Unaligned record fields and host fixtures observe the same exact target byte order. */
	destination[0] = (uint8_t)word;
	destination[1] = (uint8_t)(word >> 8);
	destination[2] = (uint8_t)(word >> 16);
	destination[3] = (uint8_t)(word >> 24);

	/* Succeeded: the complete numerical word occupies its exact four-byte native field. */
	return;
}
