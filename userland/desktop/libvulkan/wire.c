/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Encodes checked Venus wire primitives independently of Vulkan scene data.
 *
 * The wire form (little-endian words, u64 array sizes and pointer marks,
 * sType and pNext presence, strings as a u64 length with the NUL and a
 * 4-byte padded body, handles as u64 object ids) follows the venus-protocol
 * specification docs/VK_EXT_command_serialization.txt, commit
 * ca19b6358d7cc491bc3e4de76f04c6700876a8fa; the code is zedBSD's own.
 */

#include <stdlib.h>
#include <string.h>
#include "internal.h"

static VkBool32 vulkan_reader_need(struct vulkan_reader *reader, size_t bytes);

/*
 * Encodes an internal external-buffer declaration for allocation sharing.
 */
void
vulkan_encode_buffer_external(
	struct vulkan_writer *writer,
	const void *chain)
{
	const VkBaseInStructure *next;
	struct vulkan_external_buffer_info value;
	const struct vulkan_external_buffer_info *info;
	uint32_t types;

	/* Ordinary public core buffers carry no internal external-memory declaration. */
	next = chain;
	while (next != NULL && next->sType != VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO)
		next = next->pNext;
	if (next == NULL) {
		vulkan_write_u64(writer, 0U);

		/* Succeeded: ordinary core buffer creation keeps an empty native chain. */
		return;
	}

	/* Public and private declarations share the registry layout, without C type aliasing. */
	memcpy(&value, next, sizeof(value));
	info = &value;

	/* The renderer sees only its memory handle type, never a guest descriptor or pointer. */
	vulkan_write_u64(writer, 1U);
	vulkan_write_u32(writer, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
	vulkan_write_u64(writer, 0U);
	types = info->handle_types;
	if (types == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
		types = writer->external_memory_type;
	vulkan_write_u32(writer, types);

	/* Succeeded: native buffer creation permits the selected shared allocation. */
	return;
}

/*
 * Encodes the single internal external-image declaration used by WSI.
 */
void
vulkan_encode_image_external(
	struct vulkan_writer *writer,
	const void *chain)
{
	const VkBaseInStructure *next;
	const VkBaseInStructure *layout;
	struct vulkan_external_image_info value;
	const struct vulkan_external_image_info *info;
	struct vulkan_image_layout_info placed;
	uint32_t types;

	/* Public core calls retain an empty native extension chain. */
	next = chain;
	while (next != NULL && next->sType != VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)
		next = next->pNext;
	if (next == NULL) {
		vulkan_write_u64(writer, 0U);

		/* Succeeded: ordinary core image creation carries an empty native extension chain. */
		return;
	}

	/* The registry-compatible declaration is copied before interpreting its native handle types. */
	memcpy(&value, next, sizeof(value));
	info = &value;

	/* The layout record chained after it, if the image has one. */
	layout = info->pNext;
	while (layout != NULL && layout->sType != VULKAN_STRUCTURE_TYPE_IMAGE_LAYOUT)
		layout = layout->pNext;

	/* No guest pointer or fd representation reaches the renderer. */
	vulkan_write_u64(writer, 1U);
	vulkan_write_u32(writer, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
	if (layout == NULL) {
		vulkan_write_u64(writer, 0U);
	} else {
		/* The layout record, chained as the declaration's next: its type, no next, the tiling and the parameter. */
		memcpy(&placed, layout, sizeof(placed));
		vulkan_write_u64(writer, 1U);
		vulkan_write_u32(writer, VULKAN_STRUCTURE_TYPE_IMAGE_LAYOUT);
		vulkan_write_u64(writer, 0U);
		vulkan_write_u32(writer, placed.tiling);
		vulkan_write_u32(writer, placed.parameter);
	}
	types = info->handle_types;
	if (types == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
		types = writer->external_memory_type;
	vulkan_write_u32(writer, types);

	/* Succeeded: the renderer receives only the selected external image memory type. */
	return;
}

/*
 * Initializes independent storage for one encoded command stream.
 */
void
vulkan_writer_init(
	struct vulkan_writer *writer)
{
	/* Starts with no allocation so empty writers always have cheap cleanup. */
	memset(writer, 0, sizeof(*writer));
	writer->external_memory_type = VULKAN_EXTERNAL_MEMORY_DMABUF;

	/* Succeeded: the writer can grow as command parameters are encoded. */
	return;
}

/*
 * Releases command storage without retaining a pointer into a prior request.
 */
void
vulkan_writer_finish(
	struct vulkan_writer *writer)
{
	/* Releases all storage owned by this independent encoding transaction. */
	if (writer->allocator.has_callbacks) {
		writer->allocator.callbacks.pfnFree(writer->allocator.callbacks.pUserData, writer->data);
	} else {
		free(writer->data);
	}

	memset(writer, 0, sizeof(*writer));

	/* Succeeded: the writer no longer retains command bytes. */
	return;
}

/*
 * Reserves checked capacity while retaining the first encoding failure.
 */
void
vulkan_writer_reserve(
	struct vulkan_writer *writer,
	size_t additional)
{
	size_t needed;
	size_t capacity;
	uint8_t *storage;

	/* Leaves the original failure intact for the command's result path. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Rejects byte-count overflow before allocator or source access. */
	if (additional > SIZE_MAX - writer->bytes) {
		writer->error = VK_ERROR_OUT_OF_HOST_MEMORY;
		return;
	}

	/* Reuses capacity already owned by this command writer. */
	needed = writer->bytes + additional;
	if (needed <= writer->capacity)
		return;

	/* Starts small without placing a fixed ceiling on valid Vulkan inputs. */
	capacity = writer->capacity;
	if (capacity == 0)
		capacity = 256;

	/* Grows geometrically until the entire requested append fits. */
	while (capacity < needed) {
		/* Uses the exact remaining capacity when doubling would overflow. */
		if (capacity > SIZE_MAX / 2) {
			capacity = needed;
			break;
		}

		/* Keeps the next growth above every earlier accepted request size. */
		capacity *= 2;
	}

	/* Preserves existing bytes if the host cannot provide more storage. */
	if (writer->allocator.has_callbacks) {
		storage = writer->allocator.callbacks.pfnAllocation(writer->allocator.callbacks.pUserData, capacity, 16, writer->scope);
		if (storage != NULL) {
			/* Retains encoded bytes before returning the previous callback allocation. */
			if (writer->bytes != 0)
				memcpy(storage, writer->data, writer->bytes);
			writer->allocator.callbacks.pfnFree(writer->allocator.callbacks.pUserData, writer->data);
		}
	} else {
		/* Keeps the ordinary allocator fast when the application supplied none. */
		storage = realloc(writer->data, capacity);
	}

	if (storage == NULL) {
		writer->error = VK_ERROR_OUT_OF_HOST_MEMORY;
		return;
	}

	/* Publishes expanded capacity only after allocation succeeded. */
	writer->data = storage;
	writer->capacity = capacity;

	/* Succeeded: the requested append now fits without another allocation. */
	return;
}

/*
 * Appends a little-endian 32-bit protocol word.
 */
void
vulkan_write_u32(
	struct vulkan_writer *writer,
	uint32_t word)
{
	uint8_t *bytes;

	/* Reserves the complete scalar before modifying the stream length. */
	vulkan_writer_reserve(writer, 4);
	if (writer->error != VK_SUCCESS)
		return;

	/* Stores wire byte order independently of host alignment or endianness. */
	bytes = writer->data + writer->bytes;
	bytes[0] = (uint8_t)word;
	bytes[1] = (uint8_t)(word >> 8);
	bytes[2] = (uint8_t)(word >> 16);
	bytes[3] = (uint8_t)(word >> 24);
	writer->bytes += 4;

	/* Succeeded: one complete wire word belongs to this command. */
	return;
}

/*
 * Appends a little-endian 64-bit protocol scalar.
 */
void
vulkan_write_u64(
	struct vulkan_writer *writer,
	uint64_t number)
{
	unsigned index;
	uint8_t *bytes;

	/* Reserves both words atomically before exposing any scalar bytes. */
	vulkan_writer_reserve(writer, 8);
	if (writer->error != VK_SUCCESS)
		return;

	/* Stores every byte without requiring 64-bit host alignment. */
	bytes = writer->data + writer->bytes;
	for (index = 0; index < 8; index++) {
		bytes[index] = (uint8_t)(number >> (index * 8));
	}

	/* Advances the cursor only after the complete scalar is present. */
	writer->bytes += 8;

	/* Succeeded: the wire scalar has its specified width on both user ABIs. */
	return;
}

/*
 * Appends IEEE single-precision bits without a numeric conversion.
 */
void
vulkan_write_float(
	struct vulkan_writer *writer,
	float number)
{
	uint32_t bits;

	/* Preserves all floating-point bits, including signed zero and NaNs. */
	memcpy(&bits, &number, sizeof(bits));
	vulkan_write_u32(writer, bits);

	/* Succeeded: the writer retains either the exact bits or its first error. */
	return;
}

/*
 * Appends an opaque byte array with the protocol's four-byte padding.
 */
void
vulkan_write_bytes(
	struct vulkan_writer *writer,
	const void *bytes,
	size_t count)
{
	size_t padded;

	/* Leaves an earlier error unchanged even when later parameters are empty. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Refuses a rounded wire length that cannot be represented. */
	if (count > SIZE_MAX - 3) {
		writer->error = VK_ERROR_OUT_OF_HOST_MEMORY;
		return;
	}

	/* Requires source storage only for a nonempty protocol array. */
	if (count != 0 && bytes == NULL) {
		writer->error = VK_ERROR_INITIALIZATION_FAILED;
		return;
	}

	/* Reserves all payload and padding before touching caller bytes. */
	padded = (count + 3) & ~(size_t)3;
	vulkan_writer_reserve(writer, padded);
	if (writer->error != VK_SUCCESS)
		return;

	/* Copies only the caller's declared byte range. */
	if (count != 0)
		memcpy(writer->data + writer->bytes, bytes, count);

	/* Avoids transmitting uninitialized host bytes as protocol padding. */
	if (padded != count)
		memset(writer->data + writer->bytes + count, 0, padded - count);

	/* Publishes the aligned extent consumed by this opaque array. */
	writer->bytes += padded;

	/* Succeeded: this array occupies exactly its padded wire length. */
	return;
}

/*
 * Encodes pointer presence without disclosing a user virtual address.
 */
void
vulkan_write_pointer(
	struct vulkan_writer *writer,
	const void *pointer)
{
	uint64_t present;

	/* The wire uses a 64-bit presence count rather than an address. */
	present = 0;
	if (pointer != NULL)
		present = 1;

	/* Appends the presence flag using the protocol's fixed pointer width. */
	vulkan_write_u64(writer, present);

	/* Succeeded: pointer identity remains private to this process. */
	return;
}

/*
 * Encodes a nullable terminated string with its explicit wire extent.
 */
void
vulkan_write_string(
	struct vulkan_writer *writer,
	const char *text)
{
	size_t bytes;

	/* An absent string has a zero array extent and no byte payload. */
	if (text == NULL) {
		vulkan_write_u64(writer, 0);

		/* Succeeded: the decoder will reconstruct a null string. */
		return;
	}

	/* Includes the terminator expected by the renderer's string decoder. */
	bytes = strlen(text);
	if (bytes == SIZE_MAX) {
		writer->error = VK_ERROR_OUT_OF_HOST_MEMORY;
		return;
	}

	/* Encodes the length separately from the padded character bytes. */
	bytes++;
	vulkan_write_u64(writer, bytes);
	vulkan_write_bytes(writer, text, bytes);

	/* Succeeded: the string is encoded or the writer retains its first error. */
	return;
}

/*
 * Takes ownership of a reply whose bytes outlive its transport transaction.
 */
void
vulkan_reader_init(
	struct vulkan_reader *reader,
	uint8_t *owned_data,
	size_t bytes)
{
	/* Initializes an independent cursor with one clear storage owner. */
	memset(reader, 0, sizeof(*reader));
	reader->data = owned_data;
	reader->bytes = bytes;

	/* Refuses a nonempty response without corresponding owned storage. */
	if (bytes != 0 && owned_data == NULL)
		reader->error = VK_ERROR_DEVICE_LOST;

	/* Succeeded: cleanup is valid even for an absent or malformed reply. */
	return;
}

/*
 * Releases the reply bytes retained by one completed command.
 */
void
vulkan_reader_finish(
	struct vulkan_reader *reader)
{
	/* Releases all response storage without touching another transaction. */
	if (reader->allocator.has_callbacks) {
		reader->allocator.callbacks.pfnFree(reader->allocator.callbacks.pUserData, reader->data);
	} else {
		free(reader->data);
	}

	memset(reader, 0, sizeof(*reader));

	/* Succeeded: the reader no longer refers to a renderer response. */
	return;
}

/*
 * Decodes a 32-bit word without advancing beyond the response.
 */
uint32_t
vulkan_read_u32(
	struct vulkan_reader *reader)
{
	VkBool32 available;
	const uint8_t *bytes;
	uint32_t word;

	/* Confirms the complete scalar before reading any response byte. */
	available = vulkan_reader_need(reader, 4);
	if (!available)
		return 0;

	/* Reassembles byte order without unaligned host loads. */
	bytes = reader->data + reader->cursor;
	word = (uint32_t)bytes[0];
	word |= (uint32_t)bytes[1] << 8;
	word |= (uint32_t)bytes[2] << 16;
	word |= (uint32_t)bytes[3] << 24;
	reader->cursor += 4;

	/* Succeeded: returns the complete scalar represented by these bytes. */
	return word;
}

/*
 * Decodes a 64-bit scalar using its fixed protocol width.
 */
uint64_t
vulkan_read_u64(
	struct vulkan_reader *reader)
{
	VkBool32 available;
	uint64_t number;
	unsigned index;

	/* Checks both words before consuming any part of this scalar. */
	available = vulkan_reader_need(reader, 8);
	if (!available)
		return 0;

	/* Reassembles each byte independently of host word size and alignment. */
	number = 0;
	for (index = 0; index < 8; index++) {
		number |= (uint64_t)reader->data[reader->cursor + index] << (index * 8);
	}

	/* Consumes the complete scalar after all bytes have been read. */
	reader->cursor += 8;

	/* Succeeded: returns the complete 64-bit protocol scalar. */
	return number;
}

/*
 * Decodes IEEE single-precision bits without a numeric conversion.
 */
float
vulkan_read_float(
	struct vulkan_reader *reader)
{
	uint32_t bits;
	float number;

	/* Reconstructs the original representation through a checked scalar read. */
	bits = vulkan_read_u32(reader);
	memcpy(&number, &bits, sizeof(number));

	/* Succeeded: returns the decoded bits while preserving any reader failure. */
	return number;
}

/*
 * Copies an opaque output array while consuming its padded wire extent.
 */
void
vulkan_read_bytes(
	struct vulkan_reader *reader,
	void *destination,
	size_t bytes)
{
	size_t padded;
	VkBool32 available;

	/* Refuses an unrepresentable padded length before consuming the reply. */
	if (bytes > SIZE_MAX - 3) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Requires application storage only for a nonempty output array. */
	if (bytes != 0 && destination == NULL) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* Verifies payload and padding together before copying any output byte. */
	padded = (bytes + 3) & ~(size_t)3;
	available = vulkan_reader_need(reader, padded);
	if (!available)
		return;

	/* Copies exactly the requested output range without exposing wire padding. */
	if (bytes != 0)
		memcpy(destination, reader->data + reader->cursor, bytes);

	/* Consumes the padding as part of the same protocol array. */
	reader->cursor += padded;

	/* Succeeded: the output contains this array and no adjacent response bytes. */
	return;
}

/*
 * Preserves Vulkan success, pending, incomplete, and failure result codes.
 */
VkResult
vulkan_read_result(
	struct vulkan_reader *reader)
{
	uint32_t encoded;
	int32_t signed_result;

	/* Reconstructs the signed result from its exact 32-bit representation. */
	encoded = vulkan_read_u32(reader);
	if (reader->error != VK_SUCCESS)
		return reader->error;

	/* Preserves negative error codes without implementation-defined casts. */
	memcpy(&signed_result, &encoded, sizeof(signed_result));

	/* Succeeded: returns the actual Vulkan result carried by this response. */
	return (VkResult)signed_result;
}

/*
 * Begins a command that requests its own ordinary renderer reply.
 */
void
vulkan_command_begin(
	struct vulkan_writer *writer,
	uint32_t opcode)
{
	/* Reuses allocated storage while clearing the previous transaction state. */
	writer->bytes = 0;
	writer->error = VK_SUCCESS;
	writer->refusable = VK_FALSE;
	writer->opcode = opcode;

	/* Encodes the command identity and protocol reply-request flag. */
	vulkan_write_u32(writer, opcode);
	vulkan_write_u32(writer, GPU_OP_FLAG_REPLY);

	/* Succeeded: parameter encoders can append to this command header. */
	return;
}

/*
 * Validates reply framing and leaves its cursor at the first output parameter.
 */
VkResult
vulkan_command_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reader,
	VkBool32 has_result)
{
	VkResult error;
	uint32_t opcode;

	/* Makes caller cleanup valid even when encoding failed before submission. */
	vulkan_reader_init(reader, NULL, 0);
	if (writer->error != VK_SUCCESS)
		return writer->error;

	/* Runs the transaction without exposing transport framing to API encoders. */
	error = vulkan_context_execute(context, writer, reply_capacity, reader);
	if (error != VK_SUCCESS)
		return error;

	/* Requires this reply to belong to the command that produced it. */
	opcode = vulkan_read_u32(reader);
	if (reader->error != VK_SUCCESS) {
		/* A truncated opcode cannot belong to a usable subsequent transaction. */
		if (context != NULL)
			vulkan_context_error(context, VK_ERROR_DEVICE_LOST);
		return reader->error;
	}

	/* A different opcode means the stream cannot safely be decoded further. */
	if (opcode != writer->opcode) {
		reader->error = VK_ERROR_DEVICE_LOST;
		if (context != NULL)
			vulkan_context_error(context, VK_ERROR_DEVICE_LOST);
		return reader->error;
	}

	/* Consumes a result only for commands whose standard return type has one. */
	if (has_result) {
		error = vulkan_read_result(reader);
		if (error != VK_SUCCESS) {
			/* Native loss and malformed result words invalidate every local session shortcut. */
			if (error == VK_ERROR_DEVICE_LOST && context != NULL)
				vulkan_context_error(context, VK_ERROR_DEVICE_LOST);
			return error;
		}
	}

	/* Succeeded: the caller may decode the command's first output parameter. */
	return VK_SUCCESS;
}

/*
 * Applies the most specific allocator to temporary command and response storage.
 */
void
vulkan_writer_init_for_object(
	struct vulkan_writer *writer,
	const struct vulkan_object *object)
{
	/* Starts a fresh command whose bytes never outlive the provoking API call. */
	vulkan_writer_init(writer);

	/* Uses object, pool, device, or instance policy already resolved at creation. */
	if (object != NULL) {
		writer->allocator = object->allocator;

		/* The public OPAQUE declaration uses only this renderer's negotiated native type. */
		if (object->context != NULL) {
			writer->external_memory_type = object->context->external_memory_type;
			writer->core11_chains = object->context->core11_queries;
		}
	}

	/* Succeeded: subsequent transport allocations share this command policy. */
	return;
}

/*
 * Encodes ordinary resource ownership without placing user pointers on the wire.
 */
void
vulkan_encode_handle(
	struct vulkan_writer *writer,
	uint64_t handle)
{
	struct vulkan_object *object;
	uint64_t identity;

	/* Resolves the standard null handle before consulting object storage. */
	object = vulkan_nondispatchable_object(handle);
	identity = vulkan_object_wire_id(object);
	vulkan_write_u64(writer, identity);

	/* Succeeded: the stream contains a renderer identity or a null handle. */
	return;
}

/*
 * Keeps presentable ordinary images in a renderer-supported preservation
 * layout, and gives the layouts of one aspect of a depth and stencil image
 * (Vulkan 1.1 and 1.2) as the Vulkan 1.0 layout that allows the same use of
 * both aspects, so every renderer reads layouts of Vulkan 1.0 only.
 *
 * Values referenced: the Vulkan 1.2 specification (section 12.4, Image
 * Layouts: DEPTH_ATTACHMENT_OPTIMAL and STENCIL_ATTACHMENT_OPTIMAL allow
 * what DEPTH_STENCIL_ATTACHMENT_OPTIMAL allows of their aspect, the
 * READ_ONLY ones what DEPTH_STENCIL_READ_ONLY_OPTIMAL allows; the two
 * mixed layouts of Vulkan 1.1 allow an attachment of one aspect and reads
 * of the other, which only GENERAL allows of both).
 */
uint32_t
vulkan_wire_image_layout(
	VkImageLayout layout)
{
	/* Direct-display WSI owns presentation while the renderer owns ordinary images. */
	if (layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
		return VK_IMAGE_LAYOUT_GENERAL;

	/* The layouts of one aspect, as the layout of both. */
	if (layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL || layout == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL)
		return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	if (layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL || layout == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL)
		return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	if (layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL || layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL)
		return VK_IMAGE_LAYOUT_GENERAL;

	/* Succeeded: all other standard image layouts retain their declared meaning. */
	return (uint32_t)layout;
}

/*
 * Checks a required output pointer's protocol marker before decoding its value.
 */
VkBool32
vulkan_reply_pointer(
	struct vulkan_reader *reader)
{
	uint64_t present;

	/* Requires exactly one output object rather than an arbitrary nonzero count. */
	present = vulkan_read_u64(reader);
	if (reader->error != VK_SUCCESS)
		return VK_FALSE;

	/* A missing output cannot satisfy the command's declared response. */
	if (present != 1) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return VK_FALSE;
	}

	/* Succeeded: one complete output value follows the marker. */
	return VK_TRUE;
}

/*
 * Preserves API results while making malformed replies terminal for their session.
 */
VkResult
vulkan_reply_finish(
	struct vulkan_context *context,
	struct vulkan_reader *reader,
	VkResult status)
{
	/* A structural decoding failure takes precedence over an ordinary API result. */
	if (reader->error != VK_SUCCESS)
		status = reader->error;

	/* Rejects later local shortcuts after the renderer namespace became unreliable. */
	if (status == VK_ERROR_DEVICE_LOST && context != NULL)
		vulkan_context_error(context, VK_ERROR_DEVICE_LOST);

	/* Releases only this independently owned transaction response. */
	vulkan_reader_finish(reader);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the complete response was consumed without protocol failure. */
	return VK_SUCCESS;
}

/* Verifies a complete response interval before any decoder accesses it. */
static VkBool32
vulkan_reader_need(
	struct vulkan_reader *reader,
	size_t bytes)
{
	/* Keeps the first malformed reply visible through subsequent reads. */
	if (reader->error != VK_SUCCESS)
		return VK_FALSE;

	/* Refuses a cursor that cannot belong to the owned response. */
	if (reader->cursor > reader->bytes) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return VK_FALSE;
	}

	/* Checks subtraction before pointer arithmetic to avoid interval wrap. */
	if (bytes > reader->bytes - reader->cursor) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return VK_FALSE;
	}

	/* Succeeded: every byte needed by the decoder belongs to this response. */
	return VK_TRUE;
}

/* Writes the fields of one chained input structure of a forwarded chain. */
typedef void (*wire_chain_self)(struct vulkan_writer *writer, const VkBaseInStructure *record);

static void wire_chain(struct vulkan_writer *writer, const VkBaseInStructure *chain, const VkStructureType *types, uint32_t count, wire_chain_self self);
static void wire_device_chain_self(struct vulkan_writer *writer, const VkBaseInStructure *record);
static void wire_render_pass_chain_self(struct vulkan_writer *writer, const VkBaseInStructure *record);
static void wire_u32_array(struct vulkan_writer *writer, const uint32_t *values, uint32_t count);

/*
 * The 1.1 feature structures a device creation forwards to a renderer
 * that answers the Vulkan 1.1 queries and the 1.2 one to a renderer that
 * answers the 1.2 queries (core11.c makes the chain), and the one render
 * pass structure.
 */
static const VkStructureType wire_device_chain_types[] = {
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES
};
static const VkStructureType wire_render_pass_chain_types[] = {
	VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO
};

/* The number of entries of a table. */
#define WIRE_COUNT(table)		((uint32_t)(sizeof(table) / sizeof((table)[0])))

/*
 * Encodes the chain of a device creation: the 1.1 feature structures the
 * caller chained, to a renderer that answers the Vulkan 1.1 queries (a
 * renderer that does not gets an empty chain, and its device names 1.0).
 */
void
vulkan_encode_device_chain(
	struct vulkan_writer *writer,
	const void *chain)
{
	/* A renderer without the 1.1 queries reads no chain. */
	if (!writer->core11_chains) {
		vulkan_write_u64(writer, 0U);
		return;
	}

	/* The 1.1 feature structures in the caller's order. */
	wire_chain(writer, chain, wire_device_chain_types, WIRE_COUNT(wire_device_chain_types), wire_device_chain_self);

	/* Succeeded: the chain is encoded. */
	return;
}

/*
 * Encodes the chain of a render pass creation: the multiview views of each
 * subpass, to a renderer that answers the Vulkan 1.1 queries.
 */
void
vulkan_encode_render_pass_chain(
	struct vulkan_writer *writer,
	const void *chain)
{
	/* A renderer without the 1.1 queries reads no chain. */
	if (!writer->core11_chains) {
		vulkan_write_u64(writer, 0U);
		return;
	}

	/* The multiview record, if the caller chained one. */
	wire_chain(writer, chain, wire_render_pass_chain_types, WIRE_COUNT(wire_render_pass_chain_types), wire_render_pass_chain_self);

	/* Succeeded: the chain is encoded. */
	return;
}

/*
 * Encodes the structures of a chain whose types are in types, others left
 * out: each present one's header ([u64 1][u32 sType]), the end of the chain
 * ([u64 0]), then the fields of each from the last to the first.
 *
 * Procedure referenced: venus-protocol docs/VK_EXT_command_serialization.txt
 * (the encoding of a pNext chain), commit
 * ca19b6358d7cc491bc3e4de76f04c6700876a8fa.
 */
static void
wire_chain(
	struct vulkan_writer *writer,
	const VkBaseInStructure *chain,
	const VkStructureType *types,
	uint32_t count,
	wire_chain_self self)
{
	const VkBaseInStructure *next;
	VkBool32 known;
	uint32_t index;

	/* The next structure the renderer is to read. */
	for (next = chain; next != NULL; next = next->pNext) {
		/* Whether its type is one of those forwarded. */
		known = VK_FALSE;
		for (index = 0; index < count; index++) {
			if (next->sType == types[index])
				known = VK_TRUE;
		}
		if (known)
			break;
	}

	/* The end of the chain. */
	if (next == NULL) {
		vulkan_write_u64(writer, 0U);
		return;
	}

	/* Its header, the rest of the chain, then its fields. */
	vulkan_write_u64(writer, 1U);
	vulkan_write_u32(writer, (uint32_t)next->sType);
	wire_chain(writer, next->pNext, types, count, self);
	self(writer, next);

	/* Succeeded: this structure and those after it are encoded. */
	return;
}

/* Writes the fields of one 1.1 feature structure of a device creation. */
static void
wire_device_chain_self(
	struct vulkan_writer *writer,
	const VkBaseInStructure *record)
{
	const VkPhysicalDeviceMultiviewFeatures *multiview;
	const VkPhysicalDevice16BitStorageFeatures *storage;
	const VkPhysicalDeviceVariablePointersFeatures *pointers;
	const VkPhysicalDeviceProtectedMemoryFeatures *protection;
	const VkPhysicalDeviceSamplerYcbcrConversionFeatures *conversion;
	const VkPhysicalDeviceShaderDrawParametersFeatures *parameters;
	VkPhysicalDeviceVulkan12Features *core12;
	uint32_t index;

	/* The fields in their declared order, each a 32-bit word, by the structure's type. */
	switch (record->sType) {
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
		core12 = (VkPhysicalDeviceVulkan12Features *)record;
		for (index = 0U; index < VULKAN_CORE12_FEATURE_COUNT; index++)
			vulkan_write_u32(writer, *vulkan_core12_feature(core12, index));
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
		multiview = (const VkPhysicalDeviceMultiviewFeatures *)record;
		vulkan_write_u32(writer, multiview->multiview);
		vulkan_write_u32(writer, multiview->multiviewGeometryShader);
		vulkan_write_u32(writer, multiview->multiviewTessellationShader);
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
		storage = (const VkPhysicalDevice16BitStorageFeatures *)record;
		vulkan_write_u32(writer, storage->storageBuffer16BitAccess);
		vulkan_write_u32(writer, storage->uniformAndStorageBuffer16BitAccess);
		vulkan_write_u32(writer, storage->storagePushConstant16);
		vulkan_write_u32(writer, storage->storageInputOutput16);
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
		pointers = (const VkPhysicalDeviceVariablePointersFeatures *)record;
		vulkan_write_u32(writer, pointers->variablePointersStorageBuffer);
		vulkan_write_u32(writer, pointers->variablePointers);
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
		protection = (const VkPhysicalDeviceProtectedMemoryFeatures *)record;
		vulkan_write_u32(writer, protection->protectedMemory);
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
		conversion = (const VkPhysicalDeviceSamplerYcbcrConversionFeatures *)record;
		vulkan_write_u32(writer, conversion->samplerYcbcrConversion);
		break;
	default:
		parameters = (const VkPhysicalDeviceShaderDrawParametersFeatures *)record;
		vulkan_write_u32(writer, parameters->shaderDrawParameters);
		break;
	}

	/* Succeeded: the fields are written. */
	return;
}

/* Writes the fields of the multiview record of a render pass creation. */
static void
wire_render_pass_chain_self(
	struct vulkan_writer *writer,
	const VkBaseInStructure *record)
{
	const VkRenderPassMultiviewCreateInfo *multiview;

	/* The view mask of each subpass, the view offset of each dependency, and the correlation masks. */
	multiview = (const VkRenderPassMultiviewCreateInfo *)record;
	vulkan_write_u32(writer, multiview->subpassCount);
	wire_u32_array(writer, multiview->pViewMasks, multiview->subpassCount);
	vulkan_write_u32(writer, multiview->dependencyCount);
	wire_u32_array(writer, (const uint32_t *)multiview->pViewOffsets, multiview->dependencyCount);
	vulkan_write_u32(writer, multiview->correlationMaskCount);
	wire_u32_array(writer, multiview->pCorrelationMasks, multiview->correlationMaskCount);

	/* Succeeded: the fields are written. */
	return;
}

/* Writes an array of 32-bit words: its length (0 when absent), then each. */
static void
wire_u32_array(
	struct vulkan_writer *writer,
	const uint32_t *values,
	uint32_t count)
{
	uint32_t index;

	/* An absent array is an empty one. */
	if (values == NULL) {
		vulkan_write_u64(writer, 0U);
		return;
	}

	/* The length, then the words. */
	vulkan_write_u64(writer, count);
	for (index = 0; index < count; index++)
		vulkan_write_u32(writer, values[index]);

	/* Succeeded: the array is written. */
	return;
}
