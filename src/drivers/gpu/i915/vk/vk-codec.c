/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The wire codec of the Vulkan executor (see vk-codec.h).
 *
 * Every reader and writer here checks its cursor's latched error first, so
 * a command that ran past its end or overflowed its reply fails as a whole
 * instead of decoding the rest out of place.  The per-command arena under
 * the generated record codec is carved here as well.
 */

#include "vk-codec.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The most structures one chain of a device creation carries: the six
 * Vulkan 1.1 feature structures libvulkan forwards, each at most once.
 */
#define VKC_DEVICE_CHAIN_DEPTH	6U

/* The number of entries of a table. */
#define VKC_COUNT(table)	((uint32_t)(sizeof(table) / sizeof((table)[0])))

/*
 * The structures libvulkan chains to its Vulkan 1.1 feature query, in its
 * order (libvulkan/instance.c: physical_load_core11).
 */
static const uint32_t vkc_core11_feature_types[] = {
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES
};

/* The structures libvulkan chains to its Vulkan 1.1 property query, in its order. */
static const uint32_t vkc_core11_property_types[] = {
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES
};

static int vkc_read_query_chain(struct i915_wire_reader *reader, uint32_t outer, uint32_t *types, uint32_t capacity, uint32_t *count);
static int vkc_chain_is(const uint32_t *types, uint32_t count, const uint32_t *expected, uint32_t expected_count);
static void vkc_reply_chain(struct i915_wire_writer *writer, uint32_t outer, const uint32_t *types, uint32_t count);
static void vkc_read_device_record(struct i915_wire_reader *reader, struct i915_wire_arena *arena, const void **chain, uint32_t depth);
static const uint32_t *vkc_read_u32_array(struct i915_wire_reader *reader, struct i915_wire_arena *arena, uint32_t count);
static int vkc_feature_refused(VkBool32 requested, VkBool32 supported);

/*
 * Reads one little-endian wire word.
 *
 * A word that would run past the end latches the reader's error and reads
 * as zero.
 */
uint32_t
drv_i915_wire_read_u32(
	struct i915_wire_reader *reader)
{
	const uint8_t *bytes;
	uint32_t word;

	/* A prior error keeps every later read from advancing. */
	if (reader->error != 0)
		return 0U;

	/* A word that would run past the end fails the whole command. */
	if (reader->offset + 4U > reader->size) {
		reader->error = 1;
		return 0U;
	}

	/* Composes the word in wire order, which is little endian whatever the host is. */
	bytes = reader->base + reader->offset;
	word = (uint32_t)bytes[0];
	word |= (uint32_t)bytes[1] << 8;
	word |= (uint32_t)bytes[2] << 16;
	word |= (uint32_t)bytes[3] << 24;
	reader->offset += 4U;

	/* Succeeded: the word was inside the command. */
	return word;
}

/*
 * Reads one little-endian 64-bit wire value, the low word first.
 */
uint64_t
drv_i915_wire_read_u64(
	struct i915_wire_reader *reader)
{
	uint64_t low;
	uint64_t high;

	/* Reads the two words; either short read latches the error and reads as zero. */
	low = drv_i915_wire_read_u32(reader);
	high = drv_i915_wire_read_u32(reader);

	/* Reports the composed value. */
	return low | (high << 32);
}

/*
 * Reads a wire object identity, which is one 64-bit value.
 */
i915_vk_handle
drv_i915_wire_read_handle(
	struct i915_wire_reader *reader)
{
	i915_vk_handle handle;

	/* Reads the identity as a plain 64-bit value. */
	handle = drv_i915_wire_read_u64(reader);

	/* Reports the identity; zero after a short read. */
	return handle;
}

/*
 * Returns a pointer to count * element wire bytes and advances past them.
 *
 * The caller reads the span in place; a span that would run past the end
 * latches the reader's error and yields NULL.
 */
const void *
drv_i915_wire_read_array(
	struct i915_wire_reader *reader,
	size_t count,
	size_t element)
{
	const void *data;
	size_t bytes;

	/* A prior error yields nothing. */
	if (reader->error != 0)
		return NULL;

	/* The span must lie inside the command. */
	bytes = count * element;
	if (reader->offset + bytes > reader->size) {
		reader->error = 1;
		return NULL;
	}

	/* Hands out the span and moves the cursor past it. */
	data = reader->base + reader->offset;
	reader->offset += bytes;

	/* Succeeded: the span is inside the command. */
	return data;
}

/*
 * Appends one little-endian word to the reply.
 *
 * A word that would overflow the reply latches the writer's error.
 */
void
drv_i915_wire_reply_u32(
	struct i915_wire_writer *writer,
	uint32_t value)
{
	uint8_t *bytes;

	/* A prior error stops further writes. */
	if (writer->error != 0)
		return;

	/* A full reply fails the stream when it ends. */
	if (writer->offset + 4U > writer->size) {
		writer->error = 1;
		return;
	}

	/* Lays the word down in the same byte order as the request. */
	bytes = writer->base + writer->offset;
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
	writer->offset += 4U;
}

/*
 * Appends one 64-bit value to the reply, the low word first.
 */
void
drv_i915_wire_reply_u64(
	struct i915_wire_writer *writer,
	uint64_t value)
{
	/* Writes the low word first, matching the reader's composition order. */
	drv_i915_wire_reply_u32(writer, (uint32_t)value);
	drv_i915_wire_reply_u32(writer, (uint32_t)(value >> 32));
}

/*
 * Appends raw bytes to the reply.
 *
 * A run that would overflow the reply is dropped whole and latches the
 * writer's error.
 */
void
drv_i915_wire_reply_bytes(
	struct i915_wire_writer *writer,
	const void *data,
	size_t bytes)
{
	/* A prior error stops further writes. */
	if (writer->error != 0)
		return;

	/* A run that does not fit fails the stream when it ends. */
	if (writer->offset + bytes > writer->size) {
		writer->error = 1;
		return;
	}

	/* Copies the run and moves the cursor past it. */
	kern_memcpy(writer->base + writer->offset, data, bytes);
	writer->offset += bytes;
}

/*
 * Returns count zeroed elements from the command's arena.
 *
 * A count the command cannot carry, or an arena too small for it, latches
 * the reader's error and yields NULL.
 */
void *
i915_vkc_array(
	struct i915_wire_reader *reader,
	struct i915_wire_arena *arena,
	uint64_t count,
	size_t element)
{
	size_t bytes;
	size_t start;

	/* A prior error yields nothing. */
	if (reader->error != 0)
		return NULL;

	/*
	 * Every element occupies at least one byte of the command, so a count
	 * beyond the bytes that remain is malformed and must not size an
	 * allocation.
	 */
	if (count > (uint64_t)(reader->size - reader->offset)) {
		reader->error = 1;
		return NULL;
	}

	/* An element of no size cannot be counted. */
	if (element == 0U) {
		reader->error = 1;
		return NULL;
	}

	/* The total must not wrap. */
	if (count > (uint64_t)(SIZE_MAX / element)) {
		reader->error = 1;
		return NULL;
	}

	/* Rounds the run and its start to eight bytes, so any record type is aligned. */
	bytes = ((size_t)count * element + 7U) & ~(size_t)7U;
	start = (arena->used + 7U) & ~(size_t)7U;

	/* A session without an arena decodes no records. */
	if (arena->base == NULL) {
		reader->error = 1;
		return NULL;
	}

	/* The aligned start must still be inside the arena. */
	if (start > arena->size) {
		reader->error = 1;
		return NULL;
	}

	/* The run must fit behind the start. */
	if (bytes > arena->size - start) {
		reader->error = 1;
		return NULL;
	}

	/* Takes the run for the current command and clears it. */
	arena->used = start + bytes;
	kern_memset(arena->base + start, 0, bytes);

	/* Succeeded: the elements live until the command returns. */
	return arena->base + start;
}

/*
 * Reads a string into the arena.
 *
 * An absent string reads as NULL.
 */
const char *
i915_vkc_read_string(
	struct i915_wire_reader *reader,
	struct i915_wire_arena *arena)
{
	uint64_t bytes;
	char *text;

	/* Reads the length, which includes the terminator; zero is an absent string. */
	bytes = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return NULL;
	if (bytes == 0U)
		return NULL;

	/* Takes room for the string in the arena. */
	text = i915_vkc_array(reader, arena, bytes, 1U);
	if (text == NULL)
		return NULL;

	/* Copies the string's bytes and skips their padding. */
	i915_vkc_read_bytes(reader, text, (size_t)bytes);
	if (reader->error != 0)
		return NULL;

	/* Terminates the string, so a sender that omitted the terminator cannot cause an overrun. */
	text[bytes - 1U] = '\0';

	/* Succeeded: the string lives until the command returns. */
	return text;
}

/*
 * Reads bytes and the padding that rounds them to four.
 */
void
i915_vkc_read_bytes(
	struct i915_wire_reader *reader,
	void *destination,
	size_t bytes)
{
	size_t padded;

	/* A prior error keeps the cursor where it is. */
	if (reader->error != 0)
		return;

	/* A length that cannot be padded is malformed. */
	if (bytes > SIZE_MAX - 3U) {
		reader->error = 1;
		return;
	}

	/* The padded run must lie inside the command. */
	padded = (bytes + 3U) & ~(size_t)3U;
	if (padded > reader->size - reader->offset) {
		reader->error = 1;
		return;
	}

	/* Copies the bytes; an empty run copies nothing. */
	if (bytes != 0U)
		kern_memcpy(destination, reader->base + reader->offset, bytes);

	/* Moves the cursor past the bytes and their padding. */
	reader->offset += padded;
}

/*
 * Reads a float as its bits; no floating-point register is involved.
 */
void
i915_vkc_read_float(
	struct i915_wire_reader *reader,
	float *destination)
{
	uint32_t bits;

	/* Reads the bits and stores them unchanged. */
	bits = drv_i915_wire_read_u32(reader);
	kern_memcpy(destination, &bits, sizeof(bits));
}

/*
 * Skips the one optional external-memory declaration libvulkan may chain to
 * a buffer or an image.
 */
void
i915_vkc_skip_external_chain(
	struct i915_wire_reader *reader)
{
	uint64_t present;

	/* Reads whether a declaration follows (wire.c: [present], then the record). */
	present = drv_i915_wire_read_u64(reader);
	if (present == 0U)
		return;

	/* Skips the record: [sType][pNext = 0][handle types]. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
}

/*
 * Writes bytes and the padding that rounds them to four.
 */
void
i915_vkc_reply_bytes(
	struct i915_wire_writer *writer,
	const void *source,
	size_t bytes)
{
	static const uint8_t zero[4] = { 0U, 0U, 0U, 0U };
	size_t padded;

	/* Writes the bytes themselves. */
	padded = (bytes + 3U) & ~(size_t)3U;
	drv_i915_wire_reply_bytes(writer, source, bytes);

	/* Writes the zero padding that rounds them to four. */
	if (padded != bytes)
		drv_i915_wire_reply_bytes(writer, zero, padded - bytes);
}

/*
 * Writes a float as its bits.
 */
void
i915_vkc_reply_float(
	struct i915_wire_writer *writer,
	const float *source)
{
	uint32_t bits;

	/* Takes the bits without a floating-point register and writes them as a word. */
	kern_memcpy(&bits, source, sizeof(bits));
	drv_i915_wire_reply_u32(writer, bits);
}

/*
 * Fills the Vulkan 1.1 answers every executor starts from.
 *
 * No 1.1 feature is on: multiview, the 16-bit storage, variable pointers,
 * protected memory, sampler YCbCr conversion and shader draw parameters are
 * each the executor's own to turn on when it does them.  A subgroup is one
 * invocation of a vertex, fragment or compute shader with every operation
 * but the quad ones (a quad is four invocations), which the shared SPIR-V
 * frontend and the PowerVR compiler lower for every backend; points are
 * clipped
 * against every clip plane; no view of multiview and no protected memory.
 * The executor sets maxPerSetDescriptors and maxMemoryAllocationSize.
 */
void
i915_vkc_core11_defaults(
	struct i915_vkc_core11 *core11)
{
	/* Every feature off and every limit zero. */
	kern_memset(core11, 0, sizeof(*core11));

	/* The subgroup of one invocation the compilers lower in these stages. */
	core11->subgroup.subgroupSize = 1U;
	core11->subgroup.supportedStages = VK_SHADER_STAGE_VERTEX_BIT |
					   VK_SHADER_STAGE_FRAGMENT_BIT |
					   VK_SHADER_STAGE_COMPUTE_BIT;

	/*
	 * Every operation of one invocation: the votes, the ballots, the
	 * shuffles, the arithmetic and the clusters, but not the quads.
	 */
	core11->subgroup.supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT |
					       VK_SUBGROUP_FEATURE_VOTE_BIT |
					       VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
					       VK_SUBGROUP_FEATURE_BALLOT_BIT |
					       VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
					       VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT |
					       VK_SUBGROUP_FEATURE_CLUSTERED_BIT;
	core11->subgroup.quadOperationsInAllStages = VK_FALSE;

	/* Points are clipped like any other primitive. */
	core11->point_clipping.pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
}

/*
 * Reads the output chain of a Vulkan 1.1 feature query (after the physical
 * device): VkPhysicalDeviceFeatures2 with the six 1.1 feature structures.
 *
 * Returns ENOTSUP for any other chain and EINVAL for a malformed one.
 */
int
i915_vkc_read_features2(
	struct i915_wire_reader *reader)
{
	uint32_t types[VKC_DEVICE_CHAIN_DEPTH + 2U];
	uint32_t count;
	int error;
	int known;

	/* The chain's headers. */
	error = vkc_read_query_chain(reader, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, types, VKC_COUNT(types), &count);
	if (error != 0)
		return error;

	/* Only the chain libvulkan sends is answered. */
	known = vkc_chain_is(types, count, vkc_core11_feature_types, VKC_COUNT(vkc_core11_feature_types));
	if (!known)
		return ENOTSUP;

	/* Succeeded: the query is for the Vulkan 1.1 features. */
	return 0;
}

/*
 * Reads the output chain of a vkGetPhysicalDeviceProperties2 query (after
 * the physical device) and says which form it is: the device's identity or
 * its Vulkan 1.1 properties (I915_VKC_PROPERTIES2_*).
 *
 * Returns ENOTSUP for any other chain and EINVAL for a malformed one.
 */
int
i915_vkc_read_properties2(
	struct i915_wire_reader *reader,
	int *form)
{
	static const uint32_t identity[] = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	uint32_t types[VKC_DEVICE_CHAIN_DEPTH + 2U];
	uint32_t count;
	int error;
	int known;

	/* The chain's headers. */
	*form = 0;
	error = vkc_read_query_chain(reader, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, types, VKC_COUNT(types), &count);
	if (error != 0)
		return error;

	/* The identity, which the export of memory names. */
	known = vkc_chain_is(types, count, identity, VKC_COUNT(identity));
	if (known) {
		*form = I915_VKC_PROPERTIES2_IDENTITY;
		return 0;
	}

	/* The Vulkan 1.1 properties; any other chain is not answered. */
	known = vkc_chain_is(types, count, vkc_core11_property_types, VKC_COUNT(vkc_core11_property_types));
	if (!known)
		return ENOTSUP;

	/* Succeeded: the query is for the Vulkan 1.1 properties. */
	*form = I915_VKC_PROPERTIES2_CORE11;
	return 0;
}

/*
 * Writes the reply of a Vulkan 1.1 feature query up to the core features:
 * the present word, the chain's headers, then the fields of each 1.1
 * structure from the last of the chain to the first (the Venus encoding of
 * an output chain).  The caller writes VkPhysicalDeviceFeatures after it.
 */
void
i915_vkc_reply_core11_features(
	struct i915_wire_writer *writer,
	const struct i915_vkc_core11 *core11)
{
	/* The present word and the headers. */
	drv_i915_wire_reply_u64(writer, 1U);
	vkc_reply_chain(writer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, vkc_core11_feature_types, VKC_COUNT(vkc_core11_feature_types));

	/* Shader draw parameters, sampler YCbCr conversion and protected memory. */
	drv_i915_wire_reply_u32(writer, core11->shader_draw_parameters.shaderDrawParameters);
	drv_i915_wire_reply_u32(writer, core11->sampler_ycbcr_conversion.samplerYcbcrConversion);
	drv_i915_wire_reply_u32(writer, core11->protected_memory.protectedMemory);

	/* Variable pointers. */
	drv_i915_wire_reply_u32(writer, core11->variable_pointers.variablePointersStorageBuffer);
	drv_i915_wire_reply_u32(writer, core11->variable_pointers.variablePointers);

	/* The 16-bit storage. */
	drv_i915_wire_reply_u32(writer, core11->storage_16bit.storageBuffer16BitAccess);
	drv_i915_wire_reply_u32(writer, core11->storage_16bit.uniformAndStorageBuffer16BitAccess);
	drv_i915_wire_reply_u32(writer, core11->storage_16bit.storagePushConstant16);
	drv_i915_wire_reply_u32(writer, core11->storage_16bit.storageInputOutput16);

	/* Multiview. */
	drv_i915_wire_reply_u32(writer, core11->multiview.multiview);
	drv_i915_wire_reply_u32(writer, core11->multiview.multiviewGeometryShader);
	drv_i915_wire_reply_u32(writer, core11->multiview.multiviewTessellationShader);
}

/*
 * Writes the reply of a Vulkan 1.1 property query up to the core
 * properties, as i915_vkc_reply_core11_features() does for the features.
 * The caller writes VkPhysicalDeviceProperties after it.
 */
void
i915_vkc_reply_core11_properties(
	struct i915_wire_writer *writer,
	const struct i915_vkc_core11 *core11)
{
	/* The present word and the headers. */
	drv_i915_wire_reply_u64(writer, 1U);
	vkc_reply_chain(writer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, vkc_core11_property_types, VKC_COUNT(vkc_core11_property_types));

	/* Maintenance 3 and protected memory. */
	drv_i915_wire_reply_u32(writer, core11->maintenance3.maxPerSetDescriptors);
	drv_i915_wire_reply_u64(writer, core11->maintenance3.maxMemoryAllocationSize);
	drv_i915_wire_reply_u32(writer, core11->protected_memory_limits.protectedNoFault);

	/* Multiview and point clipping. */
	drv_i915_wire_reply_u32(writer, core11->multiview_limits.maxMultiviewViewCount);
	drv_i915_wire_reply_u32(writer, core11->multiview_limits.maxMultiviewInstanceIndex);
	drv_i915_wire_reply_u32(writer, (uint32_t)core11->point_clipping.pointClippingBehavior);

	/* The subgroup. */
	drv_i915_wire_reply_u32(writer, core11->subgroup.subgroupSize);
	drv_i915_wire_reply_u32(writer, core11->subgroup.supportedStages);
	drv_i915_wire_reply_u32(writer, core11->subgroup.supportedOperations);
	drv_i915_wire_reply_u32(writer, core11->subgroup.quadOperationsInAllStages);
}

/*
 * Reads the chain of a device creation in place of its pNext: the Vulkan
 * 1.1 feature structures libvulkan forwards (libvulkan/wire.c:
 * vulkan_encode_device_chain), rebuilt in the command's arena and linked
 * like the application's.  Any other structure latches the reader's error.
 */
void
i915_vkc_read_device_chain(
	struct i915_wire_reader *reader,
	struct i915_wire_arena *arena,
	const void **chain)
{
	/* The first structure, and through it the rest. */
	vkc_read_device_record(reader, arena, chain, 0U);
}

/*
 * Reads the chain of a render pass creation in place of its pNext: at most
 * the VkRenderPassMultiviewCreateInfo libvulkan forwards
 * (vulkan_encode_render_pass_chain), rebuilt in the command's arena.  Any
 * other structure latches the reader's error.
 */
void
i915_vkc_read_render_pass_chain(
	struct i915_wire_reader *reader,
	struct i915_wire_arena *arena,
	const void **chain)
{
	VkRenderPassMultiviewCreateInfo *multiview;
	uint64_t present;
	uint64_t next;
	uint32_t type;

	/* An empty chain. */
	*chain = NULL;
	present = drv_i915_wire_read_u64(reader);
	if (present == 0U)
		return;

	/* The one structure, with nothing after it. */
	type = drv_i915_wire_read_u32(reader);
	next = drv_i915_wire_read_u64(reader);
	if (type != VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO || next != 0U) {
		reader->error = 1;
		return;
	}

	/* The record in the arena. */
	multiview = i915_vkc_array(reader, arena, 1U, sizeof(*multiview));
	if (multiview == NULL)
		return;
	multiview->sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;

	/* The view mask of each subpass, the view offset of each dependency and the correlation masks. */
	multiview->subpassCount = drv_i915_wire_read_u32(reader);
	multiview->pViewMasks = vkc_read_u32_array(reader, arena, multiview->subpassCount);
	multiview->dependencyCount = drv_i915_wire_read_u32(reader);
	multiview->pViewOffsets = (const int32_t *)vkc_read_u32_array(reader, arena, multiview->dependencyCount);
	multiview->correlationMaskCount = drv_i915_wire_read_u32(reader);
	multiview->pCorrelationMasks = vkc_read_u32_array(reader, arena, multiview->correlationMaskCount);

	/* Succeeded: the record is the chain. */
	*chain = multiview;
}

/*
 * Checks that a device creation turns on no Vulkan 1.1 feature the executor
 * did not report.
 *
 * Returns ENOTSUP for such a feature (libvulkan refuses it first; this is
 * the executor's own guard).
 */
int
i915_vkc_core11_check_device(
	const VkDeviceCreateInfo *info,
	const struct i915_vkc_core11 *core11)
{
	const VkBaseInStructure *chain;
	const VkPhysicalDeviceMultiviewFeatures *multiview;
	const VkPhysicalDevice16BitStorageFeatures *storage;
	const VkPhysicalDeviceVariablePointersFeatures *pointers;
	const VkPhysicalDeviceProtectedMemoryFeatures *protection;
	const VkPhysicalDeviceSamplerYcbcrConversionFeatures *conversion;
	const VkPhysicalDeviceShaderDrawParametersFeatures *parameters;
	int refused;

	/* Compares each feature structure of the chain with the report. */
	refused = 0;
	for (chain = info->pNext; chain != NULL; chain = chain->pNext) {
		/* The fields by the structure's type. */
		switch (chain->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
			multiview = (const VkPhysicalDeviceMultiviewFeatures *)chain;
			refused |= vkc_feature_refused(multiview->multiview, core11->multiview.multiview);
			refused |= vkc_feature_refused(multiview->multiviewGeometryShader, core11->multiview.multiviewGeometryShader);
			refused |= vkc_feature_refused(multiview->multiviewTessellationShader, core11->multiview.multiviewTessellationShader);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
			storage = (const VkPhysicalDevice16BitStorageFeatures *)chain;
			refused |= vkc_feature_refused(storage->storageBuffer16BitAccess, core11->storage_16bit.storageBuffer16BitAccess);
			refused |= vkc_feature_refused(storage->uniformAndStorageBuffer16BitAccess, core11->storage_16bit.uniformAndStorageBuffer16BitAccess);
			refused |= vkc_feature_refused(storage->storagePushConstant16, core11->storage_16bit.storagePushConstant16);
			refused |= vkc_feature_refused(storage->storageInputOutput16, core11->storage_16bit.storageInputOutput16);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
			pointers = (const VkPhysicalDeviceVariablePointersFeatures *)chain;
			refused |= vkc_feature_refused(pointers->variablePointersStorageBuffer, core11->variable_pointers.variablePointersStorageBuffer);
			refused |= vkc_feature_refused(pointers->variablePointers, core11->variable_pointers.variablePointers);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
			protection = (const VkPhysicalDeviceProtectedMemoryFeatures *)chain;
			refused |= vkc_feature_refused(protection->protectedMemory, core11->protected_memory.protectedMemory);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
			conversion = (const VkPhysicalDeviceSamplerYcbcrConversionFeatures *)chain;
			refused |= vkc_feature_refused(conversion->samplerYcbcrConversion, core11->sampler_ycbcr_conversion.samplerYcbcrConversion);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
			parameters = (const VkPhysicalDeviceShaderDrawParametersFeatures *)chain;
			refused |= vkc_feature_refused(parameters->shaderDrawParameters, core11->shader_draw_parameters.shaderDrawParameters);
			break;
		default:
			break;
		}
	}

	/* Refuses a feature the executor does not do. */
	if (refused != 0)
		return ENOTSUP;

	/* Succeeded: every requested 1.1 feature was reported. */
	return 0;
}

/*
 * Returns the multiview record of a render pass creation, or NULL for a
 * render pass of one view.
 */
const VkRenderPassMultiviewCreateInfo *
i915_vkc_render_pass_multiview(
	const VkRenderPassCreateInfo *info)
{
	const VkBaseInStructure *chain;

	/* The record, wherever it is in the chain. */
	for (chain = info->pNext; chain != NULL; chain = chain->pNext) {
		if (chain->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)
			return (const VkRenderPassMultiviewCreateInfo *)chain;
	}

	/* A render pass without one renders one view. */
	return NULL;
}

/*
 * Reads the output chain of a *2 query: [present][outer] then each
 * structure's [1][sType], then [0].  Returns EINVAL for a malformed chain
 * and ENOTSUP for a longer one than the caller takes.
 */
static int
vkc_read_query_chain(
	struct i915_wire_reader *reader,
	uint32_t outer,
	uint32_t *types,
	uint32_t capacity,
	uint32_t *count)
{
	uint64_t present;
	uint64_t next;
	uint32_t type;

	/* The outer structure, present. */
	*count = 0U;
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || present != 1U || type != outer)
		return EINVAL;

	/* Each chained structure until the end of the chain. */
	while (1) {
		/* The end of the chain. */
		next = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;
		if (next == 0U)
			break;

		/* A chain longer than any the executor answers. */
		if (*count >= capacity)
			return ENOTSUP;

		/* The structure's type. */
		type = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		types[*count] = type;
		(*count)++;
	}

	/* Succeeded: the chain's types are the caller's. */
	return 0;
}

/* Returns nonzero when a chain's types are exactly the expected ones, in order. */
static int
vkc_chain_is(
	const uint32_t *types,
	uint32_t count,
	const uint32_t *expected,
	uint32_t expected_count)
{
	uint32_t index;

	/* The lengths. */
	if (count != expected_count)
		return 0;

	/* Each type in its place. */
	for (index = 0U; index < count; index++) {
		if (types[index] != expected[index])
			return 0;
	}

	/* Succeeded: the chain is the expected one. */
	return 1;
}

/* Writes the headers of an output chain: the outer structure, each chained one, then the end. */
static void
vkc_reply_chain(
	struct i915_wire_writer *writer,
	uint32_t outer,
	const uint32_t *types,
	uint32_t count)
{
	uint32_t index;

	/* The outer structure, whose presence the caller wrote. */
	drv_i915_wire_reply_u32(writer, outer);

	/* Each chained structure, then the end of the chain. */
	for (index = 0U; index < count; index++) {
		drv_i915_wire_reply_u64(writer, 1U);
		drv_i915_wire_reply_u32(writer, types[index]);
	}
	drv_i915_wire_reply_u64(writer, 0U);
}

/*
 * Reads one structure of a device creation's chain and those after it: its
 * header, the rest of the chain, then its own fields (the Venus encoding).
 */
static void
vkc_read_device_record(
	struct i915_wire_reader *reader,
	struct i915_wire_arena *arena,
	const void **chain,
	uint32_t depth)
{
	VkPhysicalDeviceMultiviewFeatures *multiview;
	VkPhysicalDevice16BitStorageFeatures *storage;
	VkPhysicalDeviceVariablePointersFeatures *pointers;
	VkPhysicalDeviceProtectedMemoryFeatures *protection;
	VkPhysicalDeviceSamplerYcbcrConversionFeatures *conversion;
	VkPhysicalDeviceShaderDrawParametersFeatures *parameters;
	const void *next;
	uint64_t present;
	uint32_t type;

	/* The end of the chain. */
	*chain = NULL;
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present == 0U)
		return;

	/* A chain longer than libvulkan sends is malformed. */
	if (depth >= VKC_DEVICE_CHAIN_DEPTH) {
		reader->error = 1;
		return;
	}

	/* The header, then the rest of the chain. */
	type = drv_i915_wire_read_u32(reader);
	vkc_read_device_record(reader, arena, &next, depth + 1U);
	if (reader->error != 0)
		return;

	/* The structure's own fields, by its type. */
	switch (type) {
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
		multiview = i915_vkc_array(reader, arena, 1U, sizeof(*multiview));
		if (multiview == NULL)
			return;
		multiview->sType = type;
		multiview->pNext = (void *)(uintptr_t)next;
		multiview->multiview = drv_i915_wire_read_u32(reader);
		multiview->multiviewGeometryShader = drv_i915_wire_read_u32(reader);
		multiview->multiviewTessellationShader = drv_i915_wire_read_u32(reader);
		*chain = multiview;
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
		storage = i915_vkc_array(reader, arena, 1U, sizeof(*storage));
		if (storage == NULL)
			return;
		storage->sType = type;
		storage->pNext = (void *)(uintptr_t)next;
		storage->storageBuffer16BitAccess = drv_i915_wire_read_u32(reader);
		storage->uniformAndStorageBuffer16BitAccess = drv_i915_wire_read_u32(reader);
		storage->storagePushConstant16 = drv_i915_wire_read_u32(reader);
		storage->storageInputOutput16 = drv_i915_wire_read_u32(reader);
		*chain = storage;
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
		pointers = i915_vkc_array(reader, arena, 1U, sizeof(*pointers));
		if (pointers == NULL)
			return;
		pointers->sType = type;
		pointers->pNext = (void *)(uintptr_t)next;
		pointers->variablePointersStorageBuffer = drv_i915_wire_read_u32(reader);
		pointers->variablePointers = drv_i915_wire_read_u32(reader);
		*chain = pointers;
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
		protection = i915_vkc_array(reader, arena, 1U, sizeof(*protection));
		if (protection == NULL)
			return;
		protection->sType = type;
		protection->pNext = (void *)(uintptr_t)next;
		protection->protectedMemory = drv_i915_wire_read_u32(reader);
		*chain = protection;
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
		conversion = i915_vkc_array(reader, arena, 1U, sizeof(*conversion));
		if (conversion == NULL)
			return;
		conversion->sType = type;
		conversion->pNext = (void *)(uintptr_t)next;
		conversion->samplerYcbcrConversion = drv_i915_wire_read_u32(reader);
		*chain = conversion;
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
		parameters = i915_vkc_array(reader, arena, 1U, sizeof(*parameters));
		if (parameters == NULL)
			return;
		parameters->sType = type;
		parameters->pNext = (void *)(uintptr_t)next;
		parameters->shaderDrawParameters = drv_i915_wire_read_u32(reader);
		*chain = parameters;
		break;
	default:
		/* A structure libvulkan does not forward. */
		reader->error = 1;
		break;
	}
}

/* Reads an array of words, [length] then each, into the arena; its length must be count or 0 (absent). */
static const uint32_t *
vkc_read_u32_array(
	struct i915_wire_reader *reader,
	struct i915_wire_arena *arena,
	uint32_t count)
{
	uint32_t *words;
	uint64_t length;
	uint32_t index;

	/* An absent array. */
	length = drv_i915_wire_read_u64(reader);
	if (length == 0U)
		return NULL;

	/* An array of another length than its count is malformed. */
	if (length != count) {
		reader->error = 1;
		return NULL;
	}

	/* The words. */
	words = i915_vkc_array(reader, arena, length, sizeof(*words));
	if (words == NULL)
		return NULL;
	for (index = 0U; index < count; index++)
		words[index] = drv_i915_wire_read_u32(reader);

	/* Succeeded: the words live until the command returns. */
	return words;
}

/* Returns nonzero when a feature is requested that is not supported. */
static int
vkc_feature_refused(
	VkBool32 requested,
	VkBool32 supported)
{
	/* A feature not requested is never refused. */
	if (requested == VK_FALSE)
		return 0;

	/* A requested feature must have been reported. */
	if (supported == VK_FALSE)
		return 1;

	/* Succeeded: the feature is supported. */
	return 0;
}
