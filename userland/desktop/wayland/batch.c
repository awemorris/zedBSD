/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glass look's small solid quads drawn together: instead of one draw
 * a quad, each with its
 * own push constants, the quads that follow one another are written as
 * vertices (six a quad, each carrying the quad's place, rounded rectangle,
 * colour and radius) and drawn with one draw.  The order of drawing is
 * kept: only quads drawn one after another are joined, and anything else
 * recorded first draws the quads waiting (kwl_batch_flush).
 *
 * This first step joins only App Home's floor lines (glass.c joins a quad
 * only while server->batch_floors is set), so that its cost on the
 * Raspberry Pi 4 can be measured before more is joined: the kernel packs
 * each draw's vertices on the CPU, which more vertices make dearer, while
 * fewer draws make its per-draw preparation cheaper (review-1 H2).  A draw
 * is kept to BATCH_QUADS quads, whose packed vertices fit the kernel's
 * 16 KiB upload (review-1 N12, M8).  KWL_BATCH=0 in the environment, or a
 * device that cannot make the pipeline, draws every quad alone.
 *
 * Glyphs are joined the same way (ws270-p007): a line of text was a draw a
 * glyph, and on the VisionFive 2's PowerVR a render job holds 64 draws, every
 * further job loading the whole pass again.  A glyph's vertices carry its
 * place, its cell of the atlas and its colour; glyphs of one atlas that
 * follow one another are one draw.
 */

#include "compose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shaders.h"

/* A vertex's floats: its place (2), its quad's rounded rectangle (4), colour (4) and radius (1). */
#define BATCH_FLOATS		11U
#define BATCH_STRIDE		(BATCH_FLOATS * (uint32_t)sizeof(float))

/* The quads one draw takes: 6 vertices of 11 floats each fit the kernel's 16 KiB upload 62 times. */
#define BATCH_QUADS		62U

/* The vertex buffer's size: the vertices of a frame's joined quads. */
#define BATCH_BYTES		(1024U * 1024U)

/* A glyph's vertex's floats: its place (2), its place in the atlas (2) and its colour (4). */
#define TEXT_FLOATS		8U
#define TEXT_STRIDE		(TEXT_FLOATS * (uint32_t)sizeof(float))

/* The glyphs one draw takes: 6 vertices of 8 floats each fit the kernel's 16 KiB upload 85 times. */
#define TEXT_QUADS		85U

/* The output's push constants' byte offset of screen (panel.vert's sixth vec4). */
#define BATCH_SCREEN_OFFSET	80U

static VkResult batch_buffer(struct kwl_compose *compose);
static VkResult batch_pipeline(struct kwl_compose *compose);
static VkResult batch_text_pipeline(struct kwl_compose *compose);
static void batch_join_box(struct kwl_compose *compose, const float *constants);
static void batch_quad_box(const float *constants, float *box);

/*
 * Makes the pipeline and the vertex buffer of the joined quads, after the
 * glass look's pipelines; without them every quad is drawn alone.
 */
void
kwl_batch_open(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	const char *setting;
	VkResult result;

	/* Off when asked for (the reference for the tests). */
	compose = server->compose;
	memset(&compose->batch, 0, sizeof(compose->batch));
	setting = getenv("KWL_BATCH");
	if (setting != NULL && strcmp(setting, "0") == 0) {
		printf("KWL BATCH off\n");
		return;
	}

	/* The pipeline, then the buffer. */
	result = batch_pipeline(compose);
	if (result == VK_SUCCESS)
		result = batch_buffer(compose);
	if (result != VK_SUCCESS) {
		kwl_batch_close(compose);
		printf("KWL BATCH unavailable result=%d\n", (int)result);
		return;
	}

	/* The glyphs' pipeline; without it every glyph is drawn alone. */
	compose->batch.ready = 1U;
	result = batch_text_pipeline(compose);
	if (result == VK_SUCCESS) {
		compose->batch.text_ready = 1U;
	} else {
		printf("KWL BATCH text unavailable result=%d\n", (int)result);
	}

	/* Succeeded: the solid quads, and the glyphs when their pipeline was made. */
	printf("KWL BATCH ready quads=%u bytes=%u text=%u\n", BATCH_QUADS, BATCH_BYTES, compose->batch.text_ready);
}

/* Destroys the joined quads' pipeline and buffer (the device is idle). */
void
kwl_batch_close(
	struct kwl_compose *compose)
{
	/* Whatever was made. */
	if (compose->batch.pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(compose->device, compose->batch.pipeline, NULL);
	if (compose->batch.text_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(compose->device, compose->batch.text_pipeline, NULL);
	if (compose->batch.buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(compose->device, compose->batch.buffer, NULL);
	if (compose->batch.memory != VK_NULL_HANDLE)
		vkFreeMemory(compose->device, compose->batch.memory, NULL);
	memset(&compose->batch, 0, sizeof(compose->batch));
}

/* Starts a drawing into the list: its joined quads' vertices from the buffer's start. */
void
kwl_batch_begin(
	struct kwl_compose *compose)
{
	/* The last recording completed before this one began. */
	compose->batch.used = 0U;
	compose->batch.first = 0U;
	compose->batch.count = 0U;
	compose->batch.overflowed = 0U;
}

/*
 * Joins a solid quad to the ones waiting, from its push constants
 * (glass.c's 24 floats, with the dark appearance's colour and the layer's
 * place already in them).  A quad of another screen or set, or one past
 * the draw's room, draws those waiting first.  Returns 1 when it was
 * joined, 0 when the caller draws it alone (no room in the buffer).
 */
int
kwl_batch_add(
	struct kwl_server *server,
	struct kwl_list *command,
	const float *constants,
	VkDescriptorSet set)
{
	static const float corners[6][2] = {
		{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f },
		{ 0.0f, 1.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }
	};
	struct kwl_compose *compose;
	float vertex[BATCH_FLOATS];
	uint32_t bytes;
	unsigned index;

	/* Only with the batch, and room for the quad's six vertices. */
	compose = server->compose;
	if (!compose->batch.ready)
		return 0;
	bytes = 6U * BATCH_STRIDE;
	if (compose->batch.used + bytes > BATCH_BYTES) {
		kwl_batch_flush(server, command);
		if (!compose->batch.overflowed)
			printf("KWL BATCH overflow bytes=%u\n", compose->batch.used);
		compose->batch.overflowed = 1U;
		return 0;
	}

	/* Glyphs waiting, another screen or set, or a full draw: the quads waiting are drawn first. */
	if (compose->batch.count != 0U &&
	    (compose->batch.text ||
	     compose->batch.count == BATCH_QUADS ||
	     compose->batch.set != set ||
	     compose->batch.screen[0] != constants[20] ||
	     compose->batch.screen[1] != constants[21]))
		kwl_batch_flush(server, command);

	/* The first quad of a draw names its set and screen and where its vertices start; every quad widens the draw's box. */
	if (compose->batch.count == 0U) {
		compose->batch.first = compose->batch.used;
		compose->batch.set = set;
		compose->batch.text = 0U;
	}
	batch_join_box(compose, constants);

	/* The quad's values: its box, its colour with its opacity, its radius. */
	memcpy(&vertex[2], &constants[8], 4U * sizeof(float));
	memcpy(&vertex[6], &constants[12], 4U * sizeof(float));
	vertex[9] = constants[15] * constants[23];
	vertex[10] = constants[16];

	/*
	 * Six vertices at the quad's corners, written in order to the list (its
	 * replay writes them to the mapped buffer at the same offsets,
	 * ws213-p003).
	 */
	for (index = 0U; index < 6U; index++) {
		vertex[0] = constants[0] + (constants[2] - constants[0]) * corners[index][0];
		vertex[1] = constants[1] + (constants[3] - constants[1]) * corners[index][1];
		kwl_list_add_vertices(command, vertex, BATCH_STRIDE);
		compose->batch.used += BATCH_STRIDE;
	}
	compose->batch.count++;

	/* One quad more drawn together (KWL PERF walk counts the things drawn). */
	command->joined++;

	/* Succeeded: the quad waits for the draw. */
	return 1;
}

/*
 * Joins a glyph to the ones waiting, from its push constants (glass.c's
 * 24 floats, its colour already the appearance's, mode 5): its quad, its
 * cell of the atlas, its colour with the shape's opacity.  Solid quads
 * waiting, a glyph of another atlas or screen, or one past the draw's room
 * draws those waiting first.  Returns 1 when it was joined, 0 when the
 * caller draws it alone (no glyphs' pipeline, or no room in the buffer).
 */
int
kwl_batch_add_text(
	struct kwl_server *server,
	struct kwl_list *command,
	const float *constants,
	VkDescriptorSet set)
{
	static const float corners[6][2] = {
		{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f },
		{ 0.0f, 1.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }
	};
	struct kwl_compose *compose;
	float vertex[TEXT_FLOATS];
	uint32_t bytes;
	unsigned index;

	/* Only with the glyphs' pipeline, and room for the glyph's six vertices. */
	compose = server->compose;
	if (!compose->batch.ready || !compose->batch.text_ready)
		return 0;
	bytes = 6U * TEXT_STRIDE;
	if (compose->batch.used + bytes > BATCH_BYTES) {
		kwl_batch_flush(server, command);
		if (!compose->batch.overflowed)
			printf("KWL BATCH overflow bytes=%u\n", compose->batch.used);
		compose->batch.overflowed = 1U;
		return 0;
	}

	/* Solid quads waiting, another screen or atlas, or a full draw: the quads waiting are drawn first. */
	if (compose->batch.count != 0U &&
	    (!compose->batch.text ||
	     compose->batch.count == TEXT_QUADS ||
	     compose->batch.set != set ||
	     compose->batch.screen[0] != constants[20] ||
	     compose->batch.screen[1] != constants[21]))
		kwl_batch_flush(server, command);

	/* The first glyph of a draw names its atlas and screen and where its vertices start; every glyph widens the draw's box. */
	if (compose->batch.count == 0U) {
		compose->batch.first = compose->batch.used;
		compose->batch.set = set;
		compose->batch.text = 1U;
	}
	batch_join_box(compose, constants);

	/* The glyph's colour, its alpha with the shape's opacity (panel.frag's result times screen.w). */
	vertex[4] = constants[12];
	vertex[5] = constants[13];
	vertex[6] = constants[14];
	vertex[7] = constants[15] * constants[23];

	/*
	 * Six vertices at the glyph's corners, each with its place in the atlas
	 * (panel.vert's texcoord, mixed the same way), written in order to the
	 * list (its replay writes them to the mapped buffer at the same offsets).
	 */
	for (index = 0U; index < 6U; index++) {
		vertex[0] = constants[0] + (constants[2] - constants[0]) * corners[index][0];
		vertex[1] = constants[1] + (constants[3] - constants[1]) * corners[index][1];
		vertex[2] = constants[4] + (constants[6] - constants[4]) * corners[index][0];
		vertex[3] = constants[5] + (constants[7] - constants[5]) * corners[index][1];
		kwl_list_add_vertices(command, vertex, TEXT_STRIDE);
		compose->batch.used += TEXT_STRIDE;
	}
	compose->batch.count++;

	/* One glyph more drawn together (KWL PERF walk counts the things drawn). */
	command->joined++;

	/* Succeeded: the glyph waits for the draw. */
	return 1;
}

/*
 * Draws the quads waiting with one draw: the batch's pipeline, a set the
 * layout needs (unused by its shaders), the screen, its vertices; then the
 * corners are bound again for every other draw (review-1 M5).
 */
void
kwl_batch_flush(
	struct kwl_server *server,
	struct kwl_list *command)
{
	struct kwl_compose *compose;
	float constants[KWL_PANEL_CONSTANTS];
	VkDeviceSize offset;
	VkPipeline pipeline;
	uint32_t count;

	/* Nothing waits. */
	compose = server->compose;
	count = compose->batch.count;
	if (count == 0U)
		return;
	compose->batch.count = 0U;

	/* The pipeline of the quads' kind and the set with the glass look's layout, the screen in its push constants (all of them pushed, as every draw does). */
	pipeline = compose->batch.pipeline;
	if (compose->batch.text)
		pipeline = compose->batch.text_pipeline;
	kwl_compose_bind(server, command, pipeline, compose->panel_layout, compose->batch.set);
	memset(constants, 0, sizeof(constants));
	memcpy(&constants[BATCH_SCREEN_OFFSET / sizeof(float)], compose->batch.screen, sizeof(compose->batch.screen));
	kwl_list_push_constants(command, compose->panel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, sizeof(constants), constants);

	/* The quads' vertices, in the part of the buffer of the slot the drawing goes into, then the corners again. */
	offset = (VkDeviceSize)compose->walk_slot * compose->batch.capacity + compose->batch.first;
	kwl_list_bind_vertex_buffers(command, 0U, 1U, &compose->batch.buffer, &offset);
	kwl_compose_clip_draw(
		server,
		command,
		compose->batch.bounds[0],
		compose->batch.bounds[1],
		compose->batch.bounds[2] - compose->batch.bounds[0],
		compose->batch.bounds[3] - compose->batch.bounds[1],
		6U * count);
	offset = 0U;
	kwl_list_bind_vertex_buffers(command, 0U, 1U, &compose->corners, &offset);
	kwl_compose_count_commands(server, 1U, 1U);

	/* The draw drew quads together: it is not one thing more (KWL PERF walk). */
	command->joined_draws++;
}

/* Makes the joined quads' pipeline: the glass look's layout and blending, the vertices batch.c writes. */
static VkResult
batch_pipeline(
	struct kwl_compose *compose)
{
	VkShaderModuleCreateInfo module;
	VkVertexInputBindingDescription binding;
	VkVertexInputAttributeDescription attributes[4];
	VkPipelineVertexInputStateCreateInfo input;
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkResult result;

	/* The two shaders. */
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = sizeof(kwl_batch_vert);
	module.pCode = kwl_batch_vert;
	result = vkCreateShaderModule(compose->device, &module, NULL, &vertex);
	if (result != VK_SUCCESS)
		return result;
	module.codeSize = sizeof(kwl_batch_frag);
	module.pCode = kwl_batch_frag;
	result = vkCreateShaderModule(compose->device, &module, NULL, &fragment);
	if (result != VK_SUCCESS) {
		vkDestroyShaderModule(compose->device, vertex, NULL);
		return result;
	}

	/* One buffer of vertices: the place, the box, the colour and the radius. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.stride = BATCH_STRIDE;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	memset(attributes, 0, sizeof(attributes));
	attributes[0].location = 0U;
	attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[0].offset = 0U;
	attributes[1].location = 1U;
	attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributes[1].offset = 2U * sizeof(float);
	attributes[2].location = 2U;
	attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributes[2].offset = 6U * sizeof(float);
	attributes[3].location = 3U;
	attributes[3].format = VK_FORMAT_R32_SFLOAT;
	attributes[3].offset = 10U * sizeof(float);
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	input.vertexBindingDescriptionCount = 1U;
	input.pVertexBindingDescriptions = &binding;
	input.vertexAttributeDescriptionCount = 4U;
	input.pVertexAttributeDescriptions = attributes;

	/* The pipeline; the modules only while it is made. */
	result = kwl_compose_pipeline_input(compose, vertex, fragment, compose->panel_layout, &input, &compose->batch.pipeline);
	vkDestroyShaderModule(compose->device, vertex, NULL);
	vkDestroyShaderModule(compose->device, fragment, NULL);
	return result;
}

/* Makes the joined glyphs' pipeline: the glass look's layout and blending, the glyphs' vertices batch.c writes. */
static VkResult
batch_text_pipeline(
	struct kwl_compose *compose)
{
	VkShaderModuleCreateInfo module;
	VkVertexInputBindingDescription binding;
	VkVertexInputAttributeDescription attributes[3];
	VkPipelineVertexInputStateCreateInfo input;
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkResult result;

	/* The two shaders. */
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = sizeof(kwl_text_vert);
	module.pCode = kwl_text_vert;
	result = vkCreateShaderModule(compose->device, &module, NULL, &vertex);
	if (result != VK_SUCCESS)
		return result;
	module.codeSize = sizeof(kwl_text_frag);
	module.pCode = kwl_text_frag;
	result = vkCreateShaderModule(compose->device, &module, NULL, &fragment);
	if (result != VK_SUCCESS) {
		vkDestroyShaderModule(compose->device, vertex, NULL);
		return result;
	}

	/* One buffer of vertices: the place, the place in the atlas and the colour. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.stride = TEXT_STRIDE;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	memset(attributes, 0, sizeof(attributes));
	attributes[0].location = 0U;
	attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[0].offset = 0U;
	attributes[1].location = 1U;
	attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[1].offset = 2U * sizeof(float);
	attributes[2].location = 2U;
	attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributes[2].offset = 4U * sizeof(float);
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	input.vertexBindingDescriptionCount = 1U;
	input.pVertexBindingDescriptions = &binding;
	input.vertexAttributeDescriptionCount = 3U;
	input.pVertexAttributeDescriptions = attributes;

	/* The pipeline; the modules only while it is made. */
	result = kwl_compose_pipeline_input(compose, vertex, fragment, compose->panel_layout, &input, &compose->batch.text_pipeline);
	vkDestroyShaderModule(compose->device, vertex, NULL);
	vkDestroyShaderModule(compose->device, fragment, NULL);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the glyphs can be drawn together. */
	return VK_SUCCESS;
}

/*
 * Makes the vertex buffer: host-visible and coherent, mapped while the
 * compositor runs (as the corners', review-1 L3), a part of BATCH_BYTES
 * for each submission's slot (capacity is one part).
 */
static VkResult
batch_buffer(
	struct kwl_compose *compose)
{
	VkPhysicalDeviceMemoryProperties memory;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkBufferCreateInfo buffer;
	VkMemoryPropertyFlags wanted;
	uint32_t index;
	VkResult result;
	void *map;

	/* The buffer. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = (VkDeviceSize)BATCH_BYTES * KWL_FRAME_SLOTS;
	buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(compose->device, &buffer, NULL, &compose->batch.buffer);
	if (result != VK_SUCCESS)
		return result;

	/* Host-visible, coherent memory for it. */
	vkGetBufferMemoryRequirements(compose->device, compose->batch.buffer, &requirements);
	vkGetPhysicalDeviceMemoryProperties(compose->physical, &memory);
	wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (index = 0; index < memory.memoryTypeCount; index++) {
		if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
		    (memory.memoryTypes[index].propertyFlags & wanted) == wanted)
			break;
	}
	if (index == memory.memoryTypeCount)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The memory, bound and mapped. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = index;
	result = vkAllocateMemory(compose->device, &allocate, NULL, &compose->batch.memory);
	if (result != VK_SUCCESS)
		return result;
	result = vkBindBufferMemory(compose->device, compose->batch.buffer, compose->batch.memory, 0U);
	if (result != VK_SUCCESS)
		return result;
	result = vkMapMemory(compose->device, compose->batch.memory, 0U, (VkDeviceSize)BATCH_BYTES * KWL_FRAME_SLOTS, 0U, &map);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded. */
	compose->batch.map = map;
	compose->batch.capacity = BATCH_BYTES;
	return VK_SUCCESS;
}

/*
 * Works out a quad's box (left, top, right, bottom) in the pass's pixels
 * from glass.c's constants: its corners in normalized device coordinates
 * and the screen they were made for.
 */
static void
batch_quad_box(
	const float *constants,
	float *box)
{
	float first;
	float second;

	/* Across: the two corners, the smaller on the left. */
	first = (constants[0] + 1.0f) * 0.5f * constants[20];
	second = (constants[2] + 1.0f) * 0.5f * constants[20];
	box[0] = first;
	box[2] = second;
	if (second < first) {
		box[0] = second;
		box[2] = first;
	}

	/* Down: the two corners, the smaller on top. */
	first = (constants[1] + 1.0f) * 0.5f * constants[21];
	second = (constants[3] + 1.0f) * 0.5f * constants[21];
	box[1] = first;
	box[3] = second;
	if (second < first) {
		box[1] = second;
		box[3] = first;
	}
}


/*
 * Joins a quad's box in the pass's pixels to the draw waiting: the first
 * quad of a draw names its screen and its box is the draw's, a later one
 * widens the box (ws211-p009, for the damage's rectangles it is drawn in).
 */
static void
batch_join_box(
	struct kwl_compose *compose,
	const float *constants)
{
	float box[4];

	/* The quad's box, from its corners in normalized device coordinates. */
	batch_quad_box(constants, box);

	/* The first quad of a draw. */
	if (compose->batch.count == 0U) {
		compose->batch.screen[0] = constants[20];
		compose->batch.screen[1] = constants[21];
		memcpy(compose->batch.bounds, box, sizeof(box));
		return;
	}

	/* A later one widens the draw's box. */
	if (box[0] < compose->batch.bounds[0])
		compose->batch.bounds[0] = box[0];
	if (box[1] < compose->batch.bounds[1])
		compose->batch.bounds[1] = box[1];
	if (box[2] > compose->batch.bounds[2])
		compose->batch.bounds[2] = box[2];
	if (box[3] > compose->batch.bounds[3])
		compose->batch.bounds[3] = box[3];
}
