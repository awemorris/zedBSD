/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Owns scene-independent Venus sessions and serialized, dynamically sized replies.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <uapi/gpu.h>
#include "internal.h"

#define VULKAN_REPLY_TRAILER_BYTES 20U
#define VULKAN_TRANSPORT_TIMEOUT_NS UINT64_C(10000000000)
#define VULKAN_PROTOCOL_XML_VERSION VK_MAKE_VERSION(1, 3, 269)

static VkResult vulkan_kernel_error(struct vulkan_context *context, int error);
static uint32_t vulkan_load_word(const uint8_t *bytes);
static VkResult vulkan_context_storage(struct vulkan_context *context, size_t requested, uint64_t *handle, uint32_t *resource, size_t *capacity);
static VkResult vulkan_context_transaction(struct vulkan_context *context, const struct vulkan_writer *writer, size_t reply_capacity, struct vulkan_reader *reader);
static VkResult vulkan_context_poll(struct vulkan_context *context);
static VkResult vulkan_clock_read(uint64_t *nanoseconds);
static void vulkan_encode_reply_stream(struct vulkan_writer *writer, struct vulkan_context *context);
static void vulkan_encode_reply_trailer(struct vulkan_writer *writer, struct vulkan_context *context);
static void vulkan_encode_external_stream(struct vulkan_writer *writer, struct vulkan_context *context, size_t bytes);

/*
 * Opens one compatible renderer session without creating application objects.
 */
VkResult
vulkan_context_open(
	struct vulkan_context *context,
	const char *path)
{
	struct gpu_info information;
	struct gpu_capset capset;
	VkResult error;
	VkResult cleanup;
	uint32_t required;
	uint32_t timelines;
	int status;

	/* Makes every acquisition failure safe for the common close path. */
	memset(context, 0, sizeof(*context));
	context->fd = -1;

	/* Initializes the session lock before publishing any descriptor ownership. */
	status = pthread_mutex_init(&context->mutex, NULL);
	if (status != 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Records that even a later open failure must destroy this mutex. */
	context->mutex_ready = VK_TRUE;

	/* Opens one caller-selected GPU rather than assuming a global GPU identity. */
	context->fd = open(path, O_RDWR);
	if (context->fd < 0) {
		cleanup = vulkan_context_close(context);
		(void)cleanup;
		return VK_ERROR_INCOMPATIBLE_DRIVER;
	}

	/* Reads limits from the driver before allocating any shared resource. */
	memset(&information, 0, sizeof(information));
	information.version = GPU_ABI_VERSION;
	information.size = sizeof(information);
	status = ioctl(context->fd, GPU_GET_INFO, &information);
	if (status != 0) {
		error = vulkan_kernel_error(context, errno);
		cleanup = vulkan_context_close(context);
		(void)cleanup;
		return error;
	}

	/* Requires the ordinary operations needed for Vulkan shared-memory transport. */
	required = GPU_CAP_CAPSET | GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_COMMAND | GPU_CAP_MAPPING;
	if ((information.capabilities & required) != required) {
		cleanup = vulkan_context_close(context);
		(void)cleanup;
		return VK_ERROR_INCOMPATIBLE_DRIVER;
	}

	/* Retains the actual backend limits without inventing a library object cap. */
	context->capabilities = information.capabilities;
	context->max_resource_bytes = information.max_resource_bytes;

	/* Requests the pinned Venus capability record with all output fields clear. */
	memset(&capset, 0, sizeof(capset));
	capset.version = GPU_ABI_VERSION;
	capset.size = sizeof(capset);
	capset.capset_id = 4;
	capset.capacity = GPU_CAPSET_MAX;
	status = ioctl(context->fd, GPU_GET_CAPSET, &capset);
	if (status != 0) {
		error = vulkan_kernel_error(context, errno);
		cleanup = vulkan_context_close(context);
		(void)cleanup;
		return error;
	}

	/* Requires the timeline field before decoding any negotiated offsets. */
	if (capset.bytes < 156 || capset.bytes > GPU_CAPSET_MAX) {
		cleanup = vulkan_context_close(context);
		(void)cleanup;
		return VK_ERROR_INCOMPATIBLE_DRIVER;
	}

	/* Keeps protocol revisions separate from the Vulkan API version exposed to callers. */
	context->wire_version = vulkan_load_word(capset.data);
	context->xml_version = vulkan_load_word(capset.data + 4);
	timelines = vulkan_load_word(capset.data + 152);

	/* Refuses a serialization contract that this independent codec has not implemented. */
	if (context->wire_version != 1 ||
	    context->xml_version != VULKAN_PROTOCOL_XML_VERSION ||
	    timelines == 0) {
		cleanup = vulkan_context_close(context);
		(void)cleanup;
		return VK_ERROR_INCOMPATIBLE_DRIVER;
	}

	/* Succeeded: the caller owns one compatible session with no fixed reply allocation. */
	return VK_SUCCESS;
}

/*
 * Consumes the session descriptor and releases complete or partial context state.
 */
VkResult
vulkan_context_close(
	struct vulkan_context *context)
{
	int descriptor;
	int status;
	VkResult error;

	/* Retains the first release failure while still returning all local ownership. */
	error = VK_SUCCESS;
	if (context->fd >= 0) {
		/* Consumes descriptor ownership before the OS may reuse its number. */
		descriptor = context->fd;
		context->fd = -1;
		context->reply_handle = 0;
		context->stream_handle = 0;
		context->reply_resource = 0;
		context->stream_resource = 0;
		context->reply_capacity = 0;
		context->stream_capacity = 0;

		/* Lets session teardown reclaim resources, including uncertain timeout ownership. */
		status = close(descriptor);
		if (status != 0)
			error = VK_ERROR_DEVICE_LOST;
	}

	/* Destroys only a mutex successfully initialized by this context. */
	if (context->mutex_ready) {
		context->mutex_ready = VK_FALSE;
		status = pthread_mutex_destroy(&context->mutex);
		if (status != 0)
			error = VK_ERROR_DEVICE_LOST;
	}

	/* Reports release failure after consuming the descriptor exactly once. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the context owns neither a descriptor nor a synchronization primitive. */
	return VK_SUCCESS;
}

/*
 * Allocates shared storage and returns its distinct kernel and protocol identities.
 */
VkResult
vulkan_resource_blob(
	struct vulkan_context *context,
	uint64_t bytes,
	uint64_t blob_id,
	uint64_t *handle,
	uint32_t *resource_id)
{
	struct gpu_blob_create request;
	VkResult error;
	int status;

	/* Applies the queried backend limit before a partial allocation can occur. */
	if (bytes == 0 || bytes > context->max_resource_bytes)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* Creates either shared transport storage or an exported Vulkan allocation. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.bytes = bytes;
	request.blob_id = blob_id;
	request.flags = GPU_BLOB_MAPPABLE;
	status = ioctl(context->fd, GPU_BLOB_CREATE, &request);
	if (status != 0) {
		error = vulkan_kernel_error(context, errno);
		return error;
	}

	/* Refuses missing identities without exposing a bogus handle to another subsystem. */
	if (request.handle == 0 || request.resource_id == 0) {
		__atomic_store_n(&context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
		return VK_ERROR_DEVICE_LOST;
	}

	/* Transfers both identity domains only after the complete allocation succeeded. */
	*handle = request.handle;
	*resource_id = request.resource_id;

	/* Succeeded: the caller owns this session resource until destroy or context close. */
	return VK_SUCCESS;
}

/*
 * Releases one kernel-owned resource after its renderer use has completed.
 */
VkResult
vulkan_resource_destroy(
	struct vulkan_context *context,
	uint64_t handle)
{
	struct gpu_resource_destroy request;
	VkResult error;
	int status;

	/* Accepts absent resources during partial-creation rollback. */
	if (handle == 0)
		return VK_SUCCESS;

	/* Returns only the exact resource owned by this context's session. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.handle = handle;
	status = ioctl(context->fd, GPU_RESOURCE_DESTROY, &request);
	if (status != 0) {
		error = vulkan_kernel_error(context, errno);
		return error;
	}

	/* Succeeded: neither this context nor the renderer retains the resource handle. */
	return VK_SUCCESS;
}

/*
 * Copies arbitrary resource spans through bounded kernel transfer requests.
 */
VkResult
vulkan_resource_copy(
	struct vulkan_context *context,
	uint64_t handle,
	uint64_t offset,
	void *bytes,
	size_t count,
	VkBool32 write)
{
	struct gpu_transfer request;
	uint8_t *position;
	size_t remaining;
	uint32_t amount;
	unsigned long operation;
	VkResult error;
	int status;

	/* Refuses wrapped resource intervals before issuing any partial copy. */
	if (count > UINT64_MAX - offset)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* Requires caller storage only when the requested copy is nonempty. */
	if (count != 0 && bytes == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Selects one direction for every bounded chunk of this copy. */
	position = bytes;
	remaining = count;
	operation = GPU_RESOURCE_READ;
	if (write)
		operation = GPU_RESOURCE_WRITE;

	/* Advances through the validated interval without truncating its total size. */
	while (remaining != 0) {
		/* Limits each individual ioctl to the existing transport-copy ABI. */
		amount = GPU_COPY_MAX;
		if (remaining < amount)
			amount = (uint32_t)remaining;

		/* Describes exactly one initialized request with no reserved input data. */
		memset(&request, 0, sizeof(request));
		request.version = GPU_ABI_VERSION;
		request.size = sizeof(request);
		request.handle = handle;
		request.offset = offset;
		request.address = (uintptr_t)position;
		request.bytes = amount;
		status = ioctl(context->fd, operation, &request);
		if (status != 0) {
			error = vulkan_kernel_error(context, errno);
			return error;
		}

		/* Advances ownership only over bytes accepted by the kernel. */
		position += amount;
		offset += amount;
		remaining -= amount;
	}

	/* Succeeded: the complete requested span has been copied in the selected direction. */
	return VK_SUCCESS;
}

/*
 * Executes one command while keeping its reply independent of other threads.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reader)
{
	VkResult error;
	int status;
	int unlocked;

	/* Makes failed preflight safe for the caller's unconditional reader cleanup. */
	vulkan_reader_init(reader, NULL, 0);
	if (writer->error != VK_SUCCESS)
		return writer->error;

	/* Refuses incomplete command headers and unaligned protocol streams. */
	if (writer->bytes < 8 || (writer->bytes & 3) != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Serializes reply-resource reuse, not GPU fence or presentation completion. */
	status = pthread_mutex_lock(&context->mutex);
	if (status != 0)
		return VK_ERROR_DEVICE_LOST;

	/* Does not submit new work after any caller has observed a lost context. */
	error = __atomic_load_n(&context->error, __ATOMIC_ACQUIRE);
	if (error == VK_SUCCESS)
		error = vulkan_context_transaction(context, writer, reply_capacity, reader);

	unlocked = pthread_mutex_unlock(&context->mutex);

	/* Preserves the transaction failure before reporting an internal unlock failure. */
	if (error != VK_SUCCESS)
		return error;

	/* A failed release cannot safely support another transport transaction. */
	if (unlocked != 0)
		return VK_ERROR_DEVICE_LOST;

	/* Succeeded: the caller owns its reply after the context lock has been released. */
	return VK_SUCCESS;
}

/*
 * Serializes a raw resource operation with other transactions in the same session.
 */
void
vulkan_context_lock(
	struct vulkan_context *context)
{
	int status;

	/* A live context always owns an initialized private mutex before publication. */
	status = pthread_mutex_lock(&context->mutex);
	if (status != 0)
		abort();

	/* Succeeded: the caller exclusively owns this session's resource transaction. */
	return;
}

/*
 * Releases a raw transaction without silently accepting an invalid mutex lifetime.
 */
void
vulkan_context_unlock(
	struct vulkan_context *context)
{
	int status;

	/* An unlock failure would invalidate every later assumption about session ownership. */
	status = pthread_mutex_unlock(&context->mutex);
	if (status != 0)
		abort();

	/* Succeeded: the next independent transaction may use this GPU session. */
	return;
}

/* Maps kernel allocation pressure separately from a lost renderer session. */
static VkResult
vulkan_kernel_error(
	struct vulkan_context *context,
	int error)
{
	/* Keeps transient resource pressure retryable through ordinary Vulkan results. */
	if (error == ENOMEM)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* Marks uncertain transport ownership as lost for every device sharing this context. */
	__atomic_store_n(&context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);

	/* Succeeded: reports the terminal session failure consistently to all callers. */
	return VK_ERROR_DEVICE_LOST;
}

/* Decodes one unaligned little-endian word from an already bounded record. */
static uint32_t
vulkan_load_word(
	const uint8_t *bytes)
{
	uint32_t word;

	/* Reconstructs wire byte order independently of CPU alignment requirements. */
	word = bytes[0];
	word |= (uint32_t)bytes[1] << 8;
	word |= (uint32_t)bytes[2] << 16;
	word |= (uint32_t)bytes[3] << 24;

	/* Succeeded: returns the scalar represented by these four bytes. */
	return word;
}

/* Grows shared transport storage only after an earlier transaction completed. */
static VkResult
vulkan_context_storage(
	struct vulkan_context *context,
	size_t requested,
	uint64_t *handle,
	uint32_t *resource,
	size_t *capacity)
{
	size_t bytes;
	uint64_t new_handle;
	uint32_t new_resource;
	VkResult error;
	VkResult cleanup;

	/* Reuses a complete existing resource without changing its renderer identity. */
	if (requested <= *capacity)
		return VK_SUCCESS;

	/* Refuses overflow when rounding the new transport extent to a page. */
	if (requested > SIZE_MAX - 4095)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Rounds storage without imposing a fixed command or response limit. */
	bytes = (requested + 4095) & ~(size_t)4095;
	error = vulkan_resource_blob(context, bytes, 0, &new_handle, &new_resource);
	if (error != VK_SUCCESS)
		return error;

	/* Releases old storage only after the replacement allocation succeeded. */
	error = vulkan_resource_destroy(context, *handle);
	if (error != VK_SUCCESS) {
		cleanup = vulkan_resource_destroy(context, new_handle);
		(void)cleanup;
		return error;
	}

	/* Publishes all ownership fields together inside the context transaction lock. */
	*handle = new_handle;
	*resource = new_resource;
	*capacity = bytes;

	/* Succeeded: future commands may reuse the larger shared transport resource. */
	return VK_SUCCESS;
}

/* Performs a complete reply transaction while the caller owns the context mutex. */
static VkResult
vulkan_context_transaction(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reader)
{
	struct vulkan_writer command;
	struct gpu_command request;
	uint8_t trailer[VULKAN_REPLY_TRAILER_BYTES];
	uint8_t *response;
	size_t storage_bytes;
	VkResult error;
	int status;

	/* Requires room for the reply opcode separately from the completion trailer. */
	if (reply_capacity < 4)
		reply_capacity = 4;

	/* Refuses an unrepresentable response before allocating or submitting work. */
	if (reply_capacity > SIZE_MAX - VULKAN_REPLY_TRAILER_BYTES)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Allocates caller-owned reply bytes before a command can have side effects. */
	if (writer->allocator.has_callbacks) {
		response = writer->allocator.callbacks.pfnAllocation(writer->allocator.callbacks.pUserData, reply_capacity, 16, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	} else {
		response = malloc(reply_capacity);
	}

	if (response == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Transfers local response ownership to the reader for every subsequent outcome. */
	memset(response, 0, reply_capacity);
	vulkan_reader_init(reader, response, reply_capacity);
	reader->allocator = writer->allocator;
	storage_bytes = reply_capacity + VULKAN_REPLY_TRAILER_BYTES;
	error = vulkan_context_storage(context, storage_bytes, &context->reply_handle, &context->reply_resource, &context->reply_capacity);
	if (error != VK_SUCCESS)
		return error;

	/* Clears all readable bytes so an absent output cannot expose a previous reply. */
	error = vulkan_resource_copy(context, context->reply_handle, 0, response, reply_capacity, VK_TRUE);
	if (error != VK_SUCCESS)
		return error;

	/* Clears the final completion store independently of response-body capacity. */
	memset(trailer, 0, sizeof(trailer));
	error = vulkan_resource_copy(context, context->reply_handle, context->reply_capacity - sizeof(trailer), trailer, sizeof(trailer), VK_TRUE);
	if (error != VK_SUCCESS)
		return error;

	/* Encodes small transport framing around the caller's independent command. */
	vulkan_writer_init(&command);
	command.allocator = writer->allocator;
	vulkan_encode_reply_stream(&command, context);

	/* Sends large commands through shared storage rather than truncating an ioctl. */
	if (writer->bytes > GPU_COMMAND_MAX - 128) {
		error = vulkan_context_storage(context, writer->bytes, &context->stream_handle, &context->stream_resource, &context->stream_capacity);
		if (error != VK_SUCCESS) {
			vulkan_writer_finish(&command);
			return error;
		}

		/* Copies the complete large stream before referencing it from the control command. */
		error = vulkan_resource_copy(context, context->stream_handle, 0, writer->data, writer->bytes, VK_TRUE);
		if (error != VK_SUCCESS) {
			vulkan_writer_finish(&command);
			return error;
		}

		/* References one immutable stream with no nested execution or dependencies. */
		vulkan_encode_external_stream(&command, context, writer->bytes);
	} else {
		/* Includes ordinary command bytes directly when the entire framing fits. */
		vulkan_write_bytes(&command, writer->data, writer->bytes);
	}

	/* Places a decoder-completion trailer after every caller-visible reply. */
	vulkan_encode_reply_trailer(&command, context);
	if (command.error != VK_SUCCESS) {
		error = command.error;
		vulkan_writer_finish(&command);
		return error;
	}

	/* Verifies the complete control message still fits the existing kernel ABI. */
	if (command.bytes > GPU_COMMAND_MAX) {
		vulkan_writer_finish(&command);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Submits initialized framing without treating virtqueue receipt as decoder completion. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.address = (uintptr_t)command.data;
	request.bytes = (uint32_t)command.bytes;
	status = ioctl(context->fd, GPU_COMMAND, &request);
	if (status != 0) {
		error = vulkan_kernel_error(context, errno);
		vulkan_writer_finish(&command);
		return error;
	}

	/* Releases local command bytes after the kernel copied the complete stream. */
	vulkan_writer_finish(&command);

	/* Waits for an independently validated trailer before fetching caller output. */
	error = vulkan_context_poll(context);
	if (error != VK_SUCCESS) {
		__atomic_store_n(&context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
		return VK_ERROR_DEVICE_LOST;
	}

	/* Reads the reply after completion, never from an earlier polling snapshot. */
	error = vulkan_resource_copy(context, context->reply_handle, 0, response, reply_capacity, VK_FALSE);
	if (error != VK_SUCCESS) {
		__atomic_store_n(&context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
		return VK_ERROR_DEVICE_LOST;
	}

	/* Succeeded: reader storage contains the complete response from this command. */
	return VK_SUCCESS;
}

/* Waits for decoder progress with a transport deadline independent of GPU waits. */
static VkResult
vulkan_context_poll(
	struct vulkan_context *context)
{
	uint8_t trailer[VULKAN_REPLY_TRAILER_BYTES];
	struct timespec pause;
	uint64_t started;
	uint64_t now;
	uint64_t observed;
	unsigned stagnant_polls;
	uint32_t version;
	uint32_t opcode;
	uint32_t result;
	uint32_t present_low;
	uint32_t present_high;
	VkResult error;
	int status;

	/* Starts a finite decoder deadline without changing a Vulkan fence timeout. */
	error = vulkan_clock_read(&started);
	if (error != VK_SUCCESS)
		return error;

	/* Polls complete trailer records while yielding between incomplete snapshots. */
	observed = started;
	stagnant_polls = 0;
	while (1) {
		error = vulkan_resource_copy(context, context->reply_handle, context->reply_capacity - sizeof(trailer), trailer, sizeof(trailer), VK_FALSE);
		if (error != VK_SUCCESS)
			return error;

		/* A compatible final store indicates the renderer reached its trailer command. */
		version = vulkan_load_word(trailer + 16);
		if (version >= VK_MAKE_VERSION(1, 1, 0)) {
			/* Validates every trailer field before trusting its completion store. */
			opcode = vulkan_load_word(trailer);
			result = vulkan_load_word(trailer + 4);
			present_low = vulkan_load_word(trailer + 8);
			present_high = vulkan_load_word(trailer + 12);
			if (opcode == VULKAN_OPCODE_vkEnumerateInstanceVersion &&
			    result == 0 && present_low == 1 && present_high == 0)
				break;
		}

		/* Refuses a stopped or backwards monotonic deadline source. */
		error = vulkan_clock_read(&now);
		if (error != VK_SUCCESS)
			return error;

		/* Quarantines the session when decoder completion exceeds its real deadline. */
		if (now < started || now - started >= VULKAN_TRANSPORT_TIMEOUT_NS) {
			__atomic_store_n(&context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
			return VK_ERROR_DEVICE_LOST;
		}

		/* Bounds only consecutive polls during which the monotonic clock made no progress. */
		if (now == observed) {
			stagnant_polls++;
		} else {
			observed = now;
			stagnant_polls = 0;
		}

		/* Refuses to keep polling forever when the deadline clock is broken. */
		if (stagnant_polls >= 10000) {
			__atomic_store_n(&context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
			return VK_ERROR_DEVICE_LOST;
		}

		/* Yields host execution without holding a GPU completion or object-state lock. */
		pause.tv_sec = 0;
		pause.tv_nsec = 1000000;
		status = nanosleep(&pause, NULL);
		if (status != 0 && errno != EINTR) {
			__atomic_store_n(&context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
			return VK_ERROR_DEVICE_LOST;
		}
	}

	/* Succeeded: the complete trailer proves this command stream was decoded. */
	return VK_SUCCESS;
}

/* Reads a monotonic timestamp without leaking native time representation into wire data. */
static VkResult
vulkan_clock_read(
	uint64_t *nanoseconds)
{
	struct timespec stamp;
	int status;

	/* Rejects an unavailable monotonic clock instead of inventing progress. */
	status = clock_gettime(CLOCK_MONOTONIC, &stamp);
	if (status != 0)
		return VK_ERROR_DEVICE_LOST;

	/* Refuses timestamps that cannot be represented by the deadline counter. */
	if (stamp.tv_sec < 0 || (uint64_t)stamp.tv_sec > UINT64_MAX / UINT64_C(1000000000))
		return VK_ERROR_DEVICE_LOST;

	/* Converts only a normalized native timestamp. */
	if (stamp.tv_nsec < 0 || stamp.tv_nsec >= 1000000000)
		return VK_ERROR_DEVICE_LOST;

	/* Refuses overflow when combining the whole seconds and fractional remainder. */
	if ((uint64_t)stamp.tv_sec > (UINT64_MAX - (uint64_t)stamp.tv_nsec) / UINT64_C(1000000000))
		return VK_ERROR_DEVICE_LOST;

	/* Publishes the timestamp after both native fields were validated. */
	*nanoseconds = (uint64_t)stamp.tv_sec * UINT64_C(1000000000) + (uint64_t)stamp.tv_nsec;

	/* Succeeded: the caller may compare real decoder-deadline progress. */
	return VK_SUCCESS;
}

/* Selects an owned reply resource without requesting a reply to the transport command. */
static void
vulkan_encode_reply_stream(
	struct vulkan_writer *writer,
	struct vulkan_context *context)
{
	/* Identifies the shared reply region in the renderer's resource namespace. */
	vulkan_write_u32(writer, VULKAN_OPCODE_vkSetReplyCommandStreamMESA);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, context->reply_resource);
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, context->reply_capacity);

	/* Succeeded: following command replies target this context-owned resource. */
	return;
}

/* Appends a fixed-position completion response after the requested Vulkan command. */
static void
vulkan_encode_reply_trailer(
	struct vulkan_writer *writer,
	struct vulkan_context *context)
{
	/* Moves the response cursor beyond every caller-visible output byte. */
	vulkan_write_u32(writer, VULKAN_OPCODE_vkSeekReplyCommandStreamMESA);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, context->reply_capacity - VULKAN_REPLY_TRAILER_BYTES);

	/* Requests an ordinary version response whose final store proves decoder progress. */
	vulkan_write_u32(writer, VULKAN_OPCODE_vkEnumerateInstanceVersion);
	vulkan_write_u32(writer, 1);
	vulkan_write_u64(writer, 1);

	/* Succeeded: the final version word can be polled independently of the reply body. */
	return;
}

/* References a complete large command stream using the pinned shared-stream protocol. */
static void
vulkan_encode_external_stream(
	struct vulkan_writer *writer,
	struct vulkan_context *context,
	size_t bytes)
{
	/* Executes exactly one owned shared command stream without a wrapper reply. */
	vulkan_write_u32(writer, VULKAN_OPCODE_vkExecuteCommandStreamsMESA);
	vulkan_write_u32(writer, 0);
	vulkan_write_u32(writer, 1);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, context->stream_resource);
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, bytes);

	/* Keeps the selected reply cursor and declares no external stream dependencies. */
	vulkan_write_u64(writer, 0);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, 0);
	vulkan_write_u32(writer, 0);

	/* Succeeded: the wrapper carries only bounded shared-stream metadata. */
	return;
}
