/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Encodes checked Venus wire primitives independently of Vulkan scene data.
 */

#include <stdlib.h>
#include <string.h>
#include "internal.h"

static VkBool32 vulkan_reader_need(struct vulkan_reader *reader, size_t bytes);

/*
 * Initializes independent storage for one encoded command stream.
 */
void
vulkan_writer_init(
	struct vulkan_writer *writer)
{
	/* Starts with no allocation so empty writers always have cheap cleanup. */
	memset(writer, 0, sizeof(*writer));

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
		storage = writer->allocator.callbacks.pfnAllocation(writer->allocator.callbacks.pUserData, capacity, 16, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
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
	writer->opcode = opcode;

	/* Encodes the command identity and protocol reply-request flag. */
	vulkan_write_u32(writer, opcode);
	vulkan_write_u32(writer, 1);

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
			__atomic_store_n(&context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
		return reader->error;
	}

	/* A different opcode means the stream cannot safely be decoded further. */
	if (opcode != writer->opcode) {
		reader->error = VK_ERROR_DEVICE_LOST;
		if (context != NULL)
			__atomic_store_n(&context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
		return reader->error;
	}

	/* Consumes a result only for commands whose standard return type has one. */
	if (has_result) {
		error = vulkan_read_result(reader);
		if (error != VK_SUCCESS) {
			/* Native loss and malformed result words invalidate every local session shortcut. */
			if (error == VK_ERROR_DEVICE_LOST && context != NULL)
				__atomic_store_n(&context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
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
	if (object != NULL)
		writer->allocator = object->allocator;

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
 * Keeps presentable ordinary images in a renderer-supported preservation layout.
 */
uint32_t
vulkan_wire_image_layout(
	VkImageLayout layout)
{
	/* Direct-display WSI owns presentation while the renderer owns ordinary images. */
	if (layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
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
		__atomic_store_n(&context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);

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
